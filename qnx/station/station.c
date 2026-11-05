/*
 * station: the nurse station, a small HTTP server on the board.
 *
 *   station [-c COTS] [-p PORT]          serve the nurse view on PORT (default 8080)
 *   station -V COT [-k COUNT]            try to open /dev/valve/COT COUNT times and report how often it was refused
 *
 * A poller thread reads every cot's status once a second through devctl(DCMD_TITRA_COT_GET) on /dev/titra/cotN and
 * keeps an hour of history. The station never opens a valve: under the security policy its type has no right to
 * connect to the blender drivers, and the -V mode measures that refusal.
 *
 *   GET /              the nurse view
 *   GET /api/cots      the latest status of every cot
 *   GET /api/history   the last hour of SpO2, FiO2, mode and alarm for every cot
 *   GET /api/advice    what the advisor publishes at /dev/titra/advice
 *   POST /api/handover ask the advisor for a shift handover
 *   POST /api/note?cot=N&text=...  store a nurse's note on a cot, which the advisor treats as data
 *
 * The station reads advice and shows it. It has no way to act on it: its type reaches no driver.
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <stdarg.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

#include "advisor_proto.h"
#include "page.h"
#include "titra_qnx.h"

#define MAX_COTS 8
#define HISTORY 3600
#define ADVICE_MAX 16384

typedef struct {
    float t, spo2, fio2;
    unsigned char mode, alarm;
} point_t;

typedef struct {
    titra_cot_status_t status;
    int have;
    point_t hist[HISTORY];
    int head, count;
} cot_t;

static cot_t g_cots[MAX_COTS];
static int g_ncots = 3;
static double g_start;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;

static void *poller(void *arg) {
    (void)arg;
    int fds[MAX_COTS];
    for (int i = 0; i < MAX_COTS; i++) fds[i] = -1;
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        next.tv_sec += 1;
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        for (int i = 0; i < g_ncots; i++) {
            char path[32];
            snprintf(path, sizeof path, "/dev/titra/cot%d", i + 1);
            if (fds[i] < 0) fds[i] = open(path, O_RDONLY);
            titra_cot_status_t s;
            int ok = fds[i] >= 0 && devctl(fds[i], DCMD_TITRA_COT_GET, &s, sizeof s, NULL) == EOK;
            if (!ok && fds[i] >= 0) {
                close(fds[i]);
                fds[i] = -1;
            }
            pthread_mutex_lock(&g_mtx);
            cot_t *c = &g_cots[i];
            c->have = ok;
            if (ok) c->status = s;
            point_t p = {(float)(titra_now() - g_start), ok && s.spo2 >= 0 ? (float)s.spo2 : -1.0f,
                         ok ? (float)s.fio2 : -1.0f, (unsigned char)(ok ? s.mode : 3), (unsigned char)(ok ? s.alarm : 3)};
            int idx = (c->head + c->count) % HISTORY;
            c->hist[idx] = p;
            if (c->count < HISTORY) {
                c->count++;
            } else {
                c->head = (c->head + 1) % HISTORY;
            }
            pthread_mutex_unlock(&g_mtx);
        }
    }
    return NULL;
}

static char g_advice[ADVICE_MAX] = "{\"model\":\"unavailable\",\"offline\":1}";

/* Reads the advisor's line every two seconds. With no advisor the page says so, and the cots carry on. */
static void *advice_poller(void *arg) {
    (void)arg;
    static char line[ADVICE_MAX];
    struct timespec two = {2, 0};
    for (;;) {
        nanosleep(&two, NULL);
        size_t have = 0;
        int fd = open("/dev/titra/advice", O_RDONLY);
        if (fd >= 0) {
            for (;;) {
                const ssize_t n = read(fd, line + have, sizeof line - 1 - have);
                if (n <= 0) break;
                have += (size_t)n;
                if (have >= sizeof line - 1) break;
            }
            close(fd);
        }
        line[have] = '\0';
        pthread_mutex_lock(&g_mtx);
        if (have > 0) {
            memcpy(g_advice, line, have + 1);
        } else {
            snprintf(g_advice, sizeof g_advice, "{\"model\":\"unavailable\",\"offline\":1}");
        }
        pthread_mutex_unlock(&g_mtx);
    }
    return NULL;
}

/* Asks the advisor something over a devctl. Returns 0, or an errno value. */
static int ask_advisor(unsigned dcmd, void *data, size_t size) {
    int fd = open("/dev/titra/advice", O_RDWR);
    if (fd < 0) return errno;
    const int rc = devctl(fd, (int)dcmd, data, size, NULL);
    close(fd);
    return rc;
}

/* Decodes a URL query value into out. Stops at & or the end of the text. */
static void url_value(const char *query, const char *key, char *out, size_t cap) {
    out[0] = '\0';
    const size_t klen = strlen(key);
    for (const char *p = query; p != NULL && *p; p = strchr(p, '&') ? strchr(p, '&') + 1 : NULL) {
        if (strncmp(p, key, klen) == 0 && p[klen] == '=') {
            size_t n = 0;
            for (const char *s = p + klen + 1; *s && *s != '&' && n + 1 < cap; s++) {
                if (*s == '+') {
                    out[n++] = ' ';
                } else if (*s == '%' && s[1] && s[2]) {
                    const char hex[3] = {s[1], s[2], 0};
                    out[n++] = (char)strtol(hex, NULL, 16);
                    s += 2;
                } else {
                    out[n++] = *s;
                }
            }
            out[n] = '\0';
            return;
        }
    }
}

typedef struct {
    char *buf;
    size_t len, cap;
} sbuf_t;

static void put(sbuf_t *b, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void put(sbuf_t *b, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int n = vsnprintf(b->buf + b->len, b->cap - b->len, fmt, ap);
        va_end(ap);
        if (n < 0) return;
        if ((size_t)n < b->cap - b->len) {
            b->len += (size_t)n;
            return;
        }
        size_t cap = b->cap * 2 + (size_t)n;
        char *nb = realloc(b->buf, cap);
        if (nb == NULL) return;
        b->buf = nb;
        b->cap = cap;
    }
}

static void cots_json(sbuf_t *b) {
    put(b, "{\"uptime\":%.1f,\"cots\":[", titra_now() - g_start);
    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < g_ncots; i++) {
        char line[1024];
        if (g_cots[i].have) {
            int n = titra_cot_json(&g_cots[i].status, line, sizeof line);
            if (n > 0 && line[n - 1] == '\n') line[n - 1] = '\0';
        } else {
            snprintf(line, sizeof line, "{\"cot\":%d,\"offline\":1}", i + 1);
        }
        put(b, "%s%s", i ? "," : "", line);
    }
    pthread_mutex_unlock(&g_mtx);
    put(b, "]}");
}

static void history_json(sbuf_t *b) {
    put(b, "{\"cots\":[");
    pthread_mutex_lock(&g_mtx);
    for (int i = 0; i < g_ncots; i++) {
        cot_t *c = &g_cots[i];
        put(b, "%s{\"cot\":%d,\"points\":[", i ? "," : "", i + 1);
        for (int k = 0; k < c->count; k++) {
            point_t *p = &c->hist[(c->head + k) % HISTORY];
            put(b, "%s[%.0f,%.0f,%.2f,%u,%u]", k ? "," : "", p->t, p->spo2, p->fio2, p->mode, p->alarm);
        }
        put(b, "]}");
    }
    pthread_mutex_unlock(&g_mtx);
    put(b, "]}");
}

static void reply(int fd, const char *status, const char *type, const char *body, size_t len) {
    char head[256];
    int n = snprintf(head, sizeof head,
                     "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\nCache-Control: no-store\r\n"
                     "Connection: close\r\n\r\n",
                     status, type, len);
    if (write(fd, head, (size_t)n) < 0) return;
    size_t off = 0;
    while (off < len) {
        ssize_t w = write(fd, body + off, len - off);
        if (w <= 0) return;
        off += (size_t)w;
    }
}

static void serve(int fd) {
    char req[1024];
    ssize_t n = read(fd, req, sizeof req - 1);
    if (n <= 0) return;
    req[n] = '\0';
    char method[8], path[128];
    char path_long[512];
    if (sscanf(req, "%7s %511s", method, path_long) != 2) {
        reply(fd, "400 Bad Request", "text/plain", "bad request\n", 12);
        return;
    }
    snprintf(path, sizeof path, "%.*s", (int)sizeof path - 1, path_long);
    if (strcmp(method, "POST") == 0) {
        if (strcmp(path, "/api/handover") == 0) {
            const int rc = ask_advisor(DCMD_ADVICE_HANDOVER, NULL, 0);
            reply(fd, rc == EOK ? "202 Accepted" : "503 Service Unavailable", "text/plain", "\n", 1);
        } else if (strncmp(path_long, "/api/note?", 10) == 0) {
            char cot[8], text[160];
            url_value(path_long + 10, "cot", cot, sizeof cot);
            url_value(path_long + 10, "text", text, sizeof text);
            advice_note_t note;
            memset(&note, 0, sizeof note);
            note.cot = atoi(cot);
            snprintf(note.text, sizeof note.text, "%.119s", text);
            const int rc = ask_advisor(DCMD_ADVICE_NOTE, &note, sizeof note);
            reply(fd, rc == EOK ? "202 Accepted" : "400 Bad Request", "text/plain", "\n", 1);
        } else {
            reply(fd, "404 Not Found", "text/plain", "not found\n", 10);
        }
        return;
    }
    if (strcmp(method, "GET") != 0) {
        reply(fd, "405 Method Not Allowed", "text/plain", "GET or POST only\n", 17);
        return;
    }
    if (strcmp(path, "/") == 0 || strcmp(path, "/index.html") == 0) {
        reply(fd, "200 OK", "text/html; charset=utf-8", TITRA_PAGE, sizeof(TITRA_PAGE) - 1);
        return;
    }
    sbuf_t b = {malloc(65536), 0, 65536};
    if (b.buf == NULL) return;
    if (strcmp(path, "/api/cots") == 0) {
        cots_json(&b);
        reply(fd, "200 OK", "application/json", b.buf, b.len);
    } else if (strcmp(path, "/api/history") == 0) {
        history_json(&b);
        reply(fd, "200 OK", "application/json", b.buf, b.len);
    } else if (strcmp(path, "/api/advice") == 0) {
        pthread_mutex_lock(&g_mtx);
        put(&b, "%s", g_advice);
        pthread_mutex_unlock(&g_mtx);
        reply(fd, "200 OK", "application/json", b.buf, b.len);
    } else {
        reply(fd, "404 Not Found", "text/plain", "not found\n", 10);
    }
    free(b.buf);
}

/* Tries to open a blender driver the way a misbehaving or compromised process would. */
static int probe_valve(int cot, int count) {
    char path[32];
    snprintf(path, sizeof path, "/dev/valve/%d", cot);
    int refused = 0, opened = 0, other = 0;
    int last_errno = 0;
    for (int i = 0; i < count; i++) {
        int fd = open(path, O_RDWR);
        if (fd >= 0) {
            opened++;
            close(fd);
        } else if (errno == EACCES || errno == EPERM) {
            refused++;
            last_errno = errno;
        } else {
            other++;
            last_errno = errno;
        }
    }
    printf("{\"path\":\"%s\",\"attempts\":%d,\"refused\":%d,\"opened\":%d,\"other\":%d,\"errno\":\"%s\"}\n", path, count,
           refused, opened, other, last_errno ? strerror(last_errno) : "");
    return opened ? 1 : 0;
}

int main(int argc, char **argv) {
    int port = 8080, opt, probe = 0, count = 100;
    while ((opt = getopt(argc, argv, "c:p:V:k:")) != -1) {
        switch (opt) {
            case 'c': g_ncots = atoi(optarg); break;
            case 'p': port = atoi(optarg); break;
            case 'V': probe = atoi(optarg); break;
            case 'k': count = atoi(optarg); break;
            default:
                fprintf(stderr, "usage: station [-c COTS] [-p PORT] | station -V COT [-k COUNT]\n");
                return 2;
        }
    }
    if (probe > 0) return probe_valve(probe, count);
    if (g_ncots < 1 || g_ncots > MAX_COTS) g_ncots = 3;
    g_start = titra_now();
    pthread_t tid;
    pthread_create(&tid, NULL, poller, NULL);
    pthread_create(&tid, NULL, advice_poller, NULL);

    int s = socket(AF_INET, SOCK_STREAM, 0);
    int one = 1;
    setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)port);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (s < 0 || bind(s, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(s, 8) != 0) {
        fprintf(stderr, "station: cannot listen on port %d: %s\n", port, strerror(errno));
        return 1;
    }
    fprintf(stderr, "station: %d cots on port %d\n", g_ncots, port);
    for (;;) {
        int c = accept(s, NULL, NULL);
        if (c < 0) continue;
        serve(c);
        close(c);
    }
}
