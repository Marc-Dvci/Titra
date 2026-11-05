/*
 * valved: the blender driver for one cot.
 *
 *   valved -n COT -d LINK [-B BACKUP] [-m MIN] [-M MAX] [-b BAUD] [-p PRIORITY]
 *
 * The controller sends each command with devctl(DCMD_TITRA_VALVE_SET) on /dev/valve/COT. The driver checks it
 * against the cot's limits and slew limit (core/src/guard.c), applies it, and sends a $BL frame to the blender. Its
 * own watchdog (tc_linkwd) holds and then falls back if the controller goes quiet, and it sends a $BL frame every
 * second whether or not a command arrived, so the blender's watchdog sees the driver alive. A reader thread takes
 * the blender's $BS status frames.
 */
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "link.h"
#include "rm.h"
#include "titra/control.h"
#include "titra/frame.h"
#include "titra/guard.h"
#include "titra_qnx.h"

#define LINK_HOLD_S 3.0
#define LINK_FALLBACK_S 15.0
#define SLEW_PM 50
#define BLENDER_QUIET_S 3.0

static titra_link_t g_link;
static pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_tx = PTHREAD_MUTEX_INITIALIZER;
static tc_guard_cfg_t g_guard;
static tc_linkwd_t g_wd;
static titra_valve_state_t g_state;
static int g_have_cmd;
static int g_last_cmd_pm;
static int g_alarm;
static unsigned g_seq;
static double g_blender_rx = -1.0;
static double g_start;
static int g_prio = 30;

/* The newest frame waits here for the sender thread. Each frame carries the whole command, so a newer one replaces an
 * older one that was not sent yet. */
static char g_out[TC_LINE_MAX + 1];
static size_t g_out_len;
static int g_out_ready;
static pthread_cond_t g_out_cond = PTHREAD_COND_INITIALIZER;

/* Queues one $BL frame with the FiO2 the watchdog says to apply. Caller holds g_mtx. The link is opened and written by
 * the sender thread alone: a controller's message holds g_mtx, and it must never wait for the network. */
static void send_frame_locked(double now) {
    int pm;
    int state = tc_linkwd_tick(&g_wd, now, &pm);
    g_state.applied_pm = pm;
    g_state.link_state = state;
    tc_bl_frame_t f;
    f.seq = g_seq;
    f.fio2_pm = pm;
    f.backup_pm = g_guard.backup_pm;
    /* The driver raises the cot alarm itself when the controller has gone quiet. */
    f.alarm = state == TC_LINK_NORMAL ? g_alarm : TC_ALARM_HIGH;
    g_seq = (g_seq + 1u) & 0xFFFFu;
    char buf[TC_LINE_MAX + 1];
    size_t n = tc_bl_format(buf, sizeof buf, &f);
    if (n == 0) return;
    memcpy(g_out, buf, n);
    g_out_len = n;
    g_out_ready = 1;
    pthread_cond_signal(&g_out_cond);
}

static void *sender(void *arg) {
    (void)arg;
    titra_set_priority(g_prio);
    char buf[TC_LINE_MAX + 1];
    for (;;) {
        pthread_mutex_lock(&g_mtx);
        while (!g_out_ready) pthread_cond_wait(&g_out_cond, &g_mtx);
        const size_t n = g_out_len;
        memcpy(buf, g_out, n);
        g_out_ready = 0;
        pthread_mutex_unlock(&g_mtx);
        pthread_mutex_lock(&g_tx);
        if (titra_link_open(&g_link) == 0) titra_link_write(&g_link, buf, n);
        pthread_mutex_unlock(&g_tx);
    }
    return NULL;
}

static int on_devctl(unsigned dcmd, void *data, size_t in_bytes, size_t cap, size_t *out_bytes) {
    if (cap < sizeof(titra_valve_msg_t)) return EMSGSIZE;
    titra_valve_msg_t *m = data;
    double now = titra_now();
    pthread_mutex_lock(&g_mtx);
    if (dcmd == (unsigned)DCMD_TITRA_VALVE_SET) {
        if (in_bytes < sizeof(titra_valve_cmd_t)) {
            pthread_mutex_unlock(&g_mtx);
            return EINVAL;
        }
        titra_valve_cmd_t c = m->cmd;
        tc_guard_result_t r = tc_guard_check(&g_guard, g_have_cmd, g_last_cmd_pm, c.fio2_pm, c.mode);
        g_state.result = r;
        g_state.commands++;
        if (r == TC_GUARD_OK) {
            g_have_cmd = 1;
            g_last_cmd_pm = c.fio2_pm;
            g_alarm = c.alarm;
            tc_linkwd_command(&g_wd, now, c.fio2_pm);
            send_frame_locked(now);
        } else {
            g_state.refusals++;
            fprintf(stderr, "valved: refused fio2 %d mode %d: %s\n", c.fio2_pm, c.mode, tc_guard_result_name(r));
        }
    } else if (dcmd != (unsigned)DCMD_TITRA_VALVE_GET) {
        pthread_mutex_unlock(&g_mtx);
        return ENOSYS;
    }
    g_state.blender_link_up = g_blender_rx >= 0.0 && now - g_blender_rx < BLENDER_QUIET_S;
    m->state = g_state;
    pthread_mutex_unlock(&g_mtx);
    *out_bytes = sizeof(titra_valve_msg_t);
    return EOK;
}

static int on_text(char *buf, int cap) {
    pthread_mutex_lock(&g_mtx);
    titra_valve_state_t s = g_state;
    double now = titra_now();
    s.blender_link_up = g_blender_rx >= 0.0 && now - g_blender_rx < BLENDER_QUIET_S;
    pthread_mutex_unlock(&g_mtx);
    return snprintf(buf, (size_t)cap,
                    "{\"applied\":%.3f,\"link\":\"%c\",\"blender\":%.3f,\"blender_state\":\"%c\",\"blender_link_up\":%d,"
                    "\"backup\":%.3f,\"last_result\":\"%s\",\"commands\":%u,\"refusals\":%u}\n",
                    s.applied_pm / 1000.0, s.link_state ? s.link_state : '-', s.blender_pm / 1000.0,
                    s.blender_state ? s.blender_state : '-', s.blender_link_up, s.backup_pm / 1000.0,
                    tc_guard_result_name((tc_guard_result_t)s.result), (unsigned)s.commands, (unsigned)s.refusals);
}

/* Heartbeat: a $BL frame every second, and the watchdog checked four times a second. */
static void *ticker(void *arg) {
    (void)arg;
    titra_set_priority(g_prio);
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    int k = 0;
    for (;;) {
        next.tv_nsec += 250000000;
        if (next.tv_nsec >= 1000000000) {
            next.tv_nsec -= 1000000000;
            next.tv_sec++;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL);
        pthread_mutex_lock(&g_mtx);
        double now = titra_now();
        int pm;
        int before = g_state.link_state;
        int state = tc_linkwd_tick(&g_wd, now, &pm);
        /* A restarted driver stays silent until the controller's first command, so the blender keeps holding its
         * last FiO2 instead of stepping to the backup. After the fallback time it speaks anyway. */
        int may_send = g_have_cmd || now - g_start >= LINK_FALLBACK_S;
        if (may_send && (++k % 4 == 0 || state != before)) send_frame_locked(now);
        pthread_mutex_unlock(&g_mtx);
    }
    return NULL;
}

static void *reader(void *arg) {
    (void)arg;
    titra_set_priority(g_prio);
    tc_line_t lb;
    tc_line_init(&lb);
    char buf[128];
    for (;;) {
        pthread_mutex_lock(&g_tx);
        int ok = titra_link_open(&g_link) == 0;
        pthread_mutex_unlock(&g_tx);
        if (!ok) {
            struct timespec ts = {0, 500000000};
            nanosleep(&ts, NULL);
            continue;
        }
        ssize_t n = titra_link_read(&g_link, buf, sizeof buf);
        if (n <= 0) continue;
        for (ssize_t i = 0; i < n; i++) {
            const char *line;
            size_t len;
            tc_bs_frame_t s;
            if (tc_line_feed(&lb, buf[i], &line, &len) && tc_bs_parse(line, len, &s) == TC_FRAME_OK) {
                pthread_mutex_lock(&g_mtx);
                g_state.blender_pm = s.fio2_pm;
                g_state.blender_state = s.state;
                g_blender_rx = titra_now();
                pthread_mutex_unlock(&g_mtx);
            }
        }
    }
    return NULL;
}

static void usage(void) {
    fprintf(stderr, "usage: valved -n COT -d LINK [-B BACKUP] [-m MIN] [-M MAX] [-b BAUD] [-p PRIORITY]\n");
    exit(2);
}

int main(int argc, char **argv) {
    int cot = 0, baud = 115200, opt;
    double backup = 0.30, lo = 0.21, hi = 1.00;
    const char *spec = NULL;
    while ((opt = getopt(argc, argv, "n:d:B:m:M:b:p:C:")) != -1) {
        switch (opt) {
            case 'n': cot = atoi(optarg); break;
            case 'd': spec = optarg; break;
            case 'B': backup = atof(optarg); break;
            case 'm': lo = atof(optarg); break;
            case 'M': hi = atof(optarg); break;
            case 'b': baud = atoi(optarg); break;
            case 'p': g_prio = atoi(optarg); break;
            case 'C': titra_set_process_cpus(atoi(optarg), 1); break;
            default: usage();
        }
    }
    if (cot <= 0 || spec == NULL || lo < 0.21 || hi > 1.0 || lo > hi || backup < lo || backup > hi) usage();

    g_guard.min_pm = (int)lround(lo * 1000);
    g_guard.max_pm = (int)lround(hi * 1000);
    g_guard.backup_pm = (int)lround(backup * 1000);
    g_guard.slew_pm = SLEW_PM;
    double now = titra_now();
    g_start = now;
    /* Until the controller speaks, the driver holds the backup FiO2 and its watchdog runs from start-up. */
    tc_linkwd_init(&g_wd, LINK_HOLD_S, LINK_FALLBACK_S, 300.0, g_guard.backup_pm, g_guard.backup_pm, now);
    g_state.applied_pm = g_guard.backup_pm;
    g_state.backup_pm = g_guard.backup_pm;
    g_state.link_state = TC_LINK_NORMAL;
    titra_link_init(&g_link, spec, baud);

    char path[64];
    snprintf(path, sizeof path, "/dev/valve/%d", cot);
    if (titra_rm_start(path, on_devctl, on_text, g_prio) != 0) {
        fprintf(stderr, "valved: cannot attach %s: %s\n", path, strerror(errno));
        return 1;
    }
    pthread_t t1, t2, t3;
    if (pthread_create(&t1, NULL, ticker, NULL) != 0 || pthread_create(&t2, NULL, reader, NULL) != 0 ||
        pthread_create(&t3, NULL, sender, NULL) != 0) {
        fprintf(stderr, "valved: cannot start threads: %s\n", strerror(errno));
        return 1;
    }
    titra_write_pid("valved", cot);
    fprintf(stderr, "valved: cot %d on %s at %s, backup %.2f, limits %.2f-%.2f\n", cot, spec, path, backup, lo, hi);
    pthread_join(t1, NULL);
    return 0;
}
