/* Serial line framing and frame codecs. See frame.h and docs/PROTOCOL.md. */
#include "titra/frame.h"

#include <stdio.h>
#include <string.h>

#define MAX_FIELDS 8

void tc_line_init(tc_line_t *lb) { memset(lb, 0, sizeof(*lb)); }

int tc_line_feed(tc_line_t *lb, char c, const char **line, size_t *len) {
    if (c == '$') {
        if (lb->in_line) {
            lb->dropped_bytes += (uint32_t)lb->len;
        }
        lb->in_line = 1;
        lb->overflow = 0;
        lb->len = 0;
        lb->buf[lb->len++] = c;
        return 0;
    }
    if (!lb->in_line) {
        lb->dropped_bytes++;
        return 0;
    }
    if (c == '\n') {
        size_t n = lb->len;
        int was_overflow = lb->overflow;
        lb->in_line = 0;
        lb->overflow = 0;
        lb->len = 0;
        /* The limit counts the line terminator: content plus "\r\n" must fit in TC_LINE_MAX. */
        if (n > 0 && lb->buf[n - 1] == '\r') {
            n--;
        }
        if (was_overflow || n + 2 > TC_LINE_MAX) {
            lb->dropped_lines++;
            return 0;
        }
        lb->buf[n] = '\0';
        *line = lb->buf;
        *len = n;
        return 1;
    }
    if (lb->overflow) {
        return 0;
    }
    if (lb->len >= TC_LINE_MAX) {
        lb->overflow = 1;
        return 0;
    }
    lb->buf[lb->len++] = c;
    return 0;
}

unsigned tc_checksum(const char *s, size_t n) {
    unsigned x = 0;
    for (size_t i = 0; i < n; i++) {
        x ^= (unsigned char)s[i];
    }
    return x & 0xFFu;
}

static int hex_value(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

typedef struct {
    const char *p;
    size_t n;
} field_t;

/* Checks framing and checksum, then splits the body into fields. On success fields[0] is the frame type. */
static tc_frame_result_t split(const char *line, size_t len, field_t *fields, size_t *nfields) {
    if (len + 2 > TC_LINE_MAX) return TC_FRAME_TOO_LONG;
    if (len == 0 || line[0] != '$') return TC_FRAME_NO_START;
    if (len < 4 || line[len - 3] != '*') return TC_FRAME_NO_CHECKSUM;
    int hi = hex_value(line[len - 2]);
    int lo = hex_value(line[len - 1]);
    if (hi < 0 || lo < 0) return TC_FRAME_NO_CHECKSUM;
    const char *body = line + 1;
    size_t body_len = len - 4;
    if (body_len == 0) return TC_FRAME_EMPTY;
    for (size_t i = 0; i < body_len; i++) {
        if (body[i] == '*' || body[i] == '$') return TC_FRAME_NO_CHECKSUM;
    }
    if (tc_checksum(body, body_len) != (unsigned)(hi * 16 + lo)) return TC_FRAME_CHECKSUM;
    size_t k = 0;
    size_t start = 0;
    for (size_t i = 0; i <= body_len; i++) {
        if (i == body_len || body[i] == ',') {
            if (k == MAX_FIELDS) return TC_FRAME_FIELDS;
            fields[k].p = body + start;
            fields[k].n = i - start;
            k++;
            start = i + 1;
        }
    }
    *nfields = k;
    return TC_FRAME_OK;
}

static int is_absent(field_t f) { return f.n == 2 && f.p[0] == '-' && f.p[1] == '-'; }

/* Unsigned decimal, 1 to 10 digits, no sign, value <= max. */
static int parse_uint(field_t f, uint32_t max, uint32_t *out) {
    if (f.n == 0 || f.n > 10) return 0;
    uint64_t v = 0;
    for (size_t i = 0; i < f.n; i++) {
        if (f.p[i] < '0' || f.p[i] > '9') return 0;
        v = v * 10u + (uint64_t)(f.p[i] - '0');
    }
    if (v > max) return 0;
    *out = (uint32_t)v;
    return 1;
}

/* Integer in [0, max] or "--" (TC_ABSENT). */
static int parse_opt_int(field_t f, int max, int *out) {
    if (is_absent(f)) {
        *out = TC_ABSENT;
        return 1;
    }
    uint32_t v;
    if (!parse_uint(f, (uint32_t)max, &v)) return 0;
    *out = (int)v;
    return 1;
}

/* Decimal with exactly two places ("1.48") in hundredths, in [0, max_centi], or "--". */
static int parse_opt_centi(field_t f, int max_centi, int *out) {
    if (is_absent(f)) {
        *out = TC_ABSENT;
        return 1;
    }
    if (f.n < 4 || f.p[f.n - 3] != '.') return 0;
    field_t whole = {f.p, f.n - 3};
    field_t frac = {f.p + f.n - 2, 2};
    uint32_t w, fr;
    if (!parse_uint(whole, 1000u, &w) || !parse_uint(frac, 99u, &fr)) return 0;
    uint32_t v = w * 100u + fr;
    if (v > (uint32_t)max_centi) return 0;
    *out = (int)v;
    return 1;
}

static int type_is(field_t f, const char *t) { return f.n == strlen(t) && memcmp(f.p, t, f.n) == 0; }

tc_frame_result_t tc_ox_parse(const char *line, size_t len, tc_ox_frame_t *out) {
    field_t fl[MAX_FIELDS];
    size_t n = 0;
    tc_frame_result_t r = split(line, len, fl, &n);
    if (r != TC_FRAME_OK) return r;
    if (!type_is(fl[0], "OX")) return TC_FRAME_TYPE;
    if (n != 6) return TC_FRAME_FIELDS;
    tc_ox_frame_t f;
    uint32_t flags;
    if (!parse_uint(fl[1], 0xFFFFFFFFu, &f.clock)) return TC_FRAME_VALUE;
    if (!parse_opt_int(fl[2], 100, &f.spo2)) return TC_FRAME_VALUE;
    if (!parse_opt_int(fl[3], 400, &f.pr)) return TC_FRAME_VALUE;
    if (!parse_opt_centi(fl[4], 2000, &f.pi_centi)) return TC_FRAME_VALUE;
    if (fl[5].n != 2) return TC_FRAME_VALUE;
    int h = hex_value(fl[5].p[0]);
    int l = hex_value(fl[5].p[1]);
    if (h < 0 || l < 0) return TC_FRAME_VALUE;
    flags = (uint32_t)(h * 16 + l);
    if (flags & ~TC_OX_FLAGS_ALL) return TC_FRAME_VALUE;
    f.flags = flags;
    *out = f;
    return TC_FRAME_OK;
}

tc_frame_result_t tc_bl_parse(const char *line, size_t len, tc_bl_frame_t *out) {
    field_t fl[MAX_FIELDS];
    size_t n = 0;
    tc_frame_result_t r = split(line, len, fl, &n);
    if (r != TC_FRAME_OK) return r;
    if (!type_is(fl[0], "BL")) return TC_FRAME_TYPE;
    if (n != 5) return TC_FRAME_FIELDS;
    uint32_t seq, fio2, backup, alarm;
    if (!parse_uint(fl[1], 65535u, &seq)) return TC_FRAME_VALUE;
    if (!parse_uint(fl[2], 1000u, &fio2) || fio2 < 210u) return TC_FRAME_VALUE;
    if (!parse_uint(fl[3], 1000u, &backup) || backup < 210u) return TC_FRAME_VALUE;
    if (!parse_uint(fl[4], 3u, &alarm)) return TC_FRAME_VALUE;
    out->seq = seq;
    out->fio2_pm = (int)fio2;
    out->backup_pm = (int)backup;
    out->alarm = (int)alarm;
    return TC_FRAME_OK;
}

tc_frame_result_t tc_bs_parse(const char *line, size_t len, tc_bs_frame_t *out) {
    field_t fl[MAX_FIELDS];
    size_t n = 0;
    tc_frame_result_t r = split(line, len, fl, &n);
    if (r != TC_FRAME_OK) return r;
    if (!type_is(fl[0], "BS")) return TC_FRAME_TYPE;
    if (n != 4) return TC_FRAME_FIELDS;
    uint32_t seq, fio2;
    if (!parse_uint(fl[1], 65535u, &seq)) return TC_FRAME_VALUE;
    if (!parse_uint(fl[2], 1000u, &fio2)) return TC_FRAME_VALUE;
    if (fl[3].n != 1 || (fl[3].p[0] != 'N' && fl[3].p[0] != 'H' && fl[3].p[0] != 'F')) return TC_FRAME_VALUE;
    out->seq = seq;
    out->fio2_pm = (int)fio2;
    out->state = fl[3].p[0];
    return TC_FRAME_OK;
}

/* Appends the checksum and "\r\n" to a body already written at buf+1 (buf[0] is '$'). */
static size_t finish(char *buf, size_t cap, int body_len) {
    if (body_len <= 0) return 0;
    size_t b = (size_t)body_len;
    /* '$' + body + "*XX\r\n" + NUL must fit in the buffer, and the line itself in TC_LINE_MAX. */
    if (b + 7 > cap || b + 6 > TC_LINE_MAX) return 0;
    unsigned cs = tc_checksum(buf + 1, b);
    int m = snprintf(buf + 1 + b, cap - 1 - b, "*%02X\r\n", cs);
    if (m != 5) return 0;
    return b + 6;
}

static void put_opt_int(char *dst, size_t cap, int v) {
    if (v == TC_ABSENT) {
        snprintf(dst, cap, "--");
    } else {
        snprintf(dst, cap, "%d", v);
    }
}

size_t tc_ox_format(char *buf, size_t cap, const tc_ox_frame_t *f) {
    if (cap < 2) return 0;
    if (f->spo2 != TC_ABSENT && (f->spo2 < 0 || f->spo2 > 100)) return 0;
    if (f->pr != TC_ABSENT && (f->pr < 0 || f->pr > 400)) return 0;
    if (f->pi_centi != TC_ABSENT && (f->pi_centi < 0 || f->pi_centi > 2000)) return 0;
    if (f->flags & ~TC_OX_FLAGS_ALL) return 0;
    char spo2[12], pr[12], pi[16];
    put_opt_int(spo2, sizeof spo2, f->spo2);
    put_opt_int(pr, sizeof pr, f->pr);
    if (f->pi_centi == TC_ABSENT) {
        snprintf(pi, sizeof pi, "--");
    } else {
        unsigned v = (unsigned)f->pi_centi;
        snprintf(pi, sizeof pi, "%u.%02u", v / 100u, v % 100u);
    }
    buf[0] = '$';
    int n = snprintf(buf + 1, cap - 1, "OX,%lu,%s,%s,%s,%02X", (unsigned long)f->clock, spo2, pr, pi, f->flags);
    if (n < 0 || (size_t)n >= cap - 1) return 0;
    return finish(buf, cap, n);
}

size_t tc_bl_format(char *buf, size_t cap, const tc_bl_frame_t *f) {
    if (cap < 2) return 0;
    if (f->seq > 65535u || f->fio2_pm < 210 || f->fio2_pm > 1000 || f->backup_pm < 210 || f->backup_pm > 1000 ||
        f->alarm < 0 || f->alarm > 3) {
        return 0;
    }
    buf[0] = '$';
    int n = snprintf(buf + 1, cap - 1, "BL,%u,%d,%d,%d", f->seq, f->fio2_pm, f->backup_pm, f->alarm);
    if (n < 0 || (size_t)n >= cap - 1) return 0;
    return finish(buf, cap, n);
}

size_t tc_bs_format(char *buf, size_t cap, const tc_bs_frame_t *f) {
    if (cap < 2) return 0;
    if (f->seq > 65535u || f->fio2_pm < 0 || f->fio2_pm > 1000 ||
        (f->state != 'N' && f->state != 'H' && f->state != 'F')) {
        return 0;
    }
    buf[0] = '$';
    int n = snprintf(buf + 1, cap - 1, "BS,%u,%d,%c", f->seq, f->fio2_pm, f->state);
    if (n < 0 || (size_t)n >= cap - 1) return 0;
    return finish(buf, cap, n);
}

const char *tc_frame_result_name(tc_frame_result_t r) {
    switch (r) {
        case TC_FRAME_OK: return "ok";
        case TC_FRAME_EMPTY: return "empty";
        case TC_FRAME_NO_START: return "no_start";
        case TC_FRAME_NO_CHECKSUM: return "no_checksum";
        case TC_FRAME_CHECKSUM: return "checksum";
        case TC_FRAME_TYPE: return "type";
        case TC_FRAME_FIELDS: return "fields";
        case TC_FRAME_VALUE: return "value";
        case TC_FRAME_TOO_LONG: return "too_long";
    }
    return "unknown";
}
