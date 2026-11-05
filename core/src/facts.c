/* Facts and flags. See titra/facts.h. */
#include "titra/facts.h"

#include <stdio.h>
#include <string.h>

#include "titra/control.h"
#include "titra/quality.h"
#include "titra/risk.h"

#define DESAT_BELOW 85
#define DESAT_MIN_S 10
#define PROBE_MIN_S 3
#define ROOM_AIR_PM 211

/* FiO2 histogram: 10 per mille a bin is enough for a median of a slow trend, and it needs no sorting. */
#define BINS 102

tf_thresholds_t tf_thresholds_default(void) {
    tf_thresholds_t t;
    t.min_trend_window_s = 7200;
    t.min_rate_window_s = 600;
    t.rising_delta = 0.03;
    t.wean_delta = 0.03;
    t.desats_per_h = 2.0;
    t.probe_per_h = 8.0;
    t.hold_s_per_h = 150.0;
    t.fallback_s_per_h = 120.0;
    t.high_fio2 = 0.60;
    t.high_fio2_pct = 20.0;
    return t;
}

size_t tf_second_size(void) { return sizeof(tf_second_t); }
size_t tf_facts_size(void) { return sizeof(tf_facts_t); }

const char *tf_flag_name(uint32_t flag) {
    switch (flag) {
        case TF_FLAG_RISING_NEED: return "rising_need";
        case TF_FLAG_WEAN: return "wean_opportunity";
        case TF_FLAG_DESATS: return "frequent_desaturations";
        case TF_FLAG_PROBE: return "probe_trouble";
        case TF_FLAG_HOLD_FALLBACK: return "hold_or_fallback";
        case TF_FLAG_HIGH_NEED: return "high_need";
        case TF_FLAG_SYSTEM: return "system_restart";
        default: return NULL;
    }
}

static int median_pm(const int *hist, int count) {
    if (count <= 0) return -1;
    int seen = 0;
    for (int b = 0; b < BINS; b++) {
        seen += hist[b];
        if (seen * 2 >= count) return b * 10;
    }
    return -1;
}

static int bin_of(int pm) {
    int b = pm / 10;
    if (b < 0) return 0;
    return b >= BINS ? BINS - 1 : b;
}

static int pct(int part, int whole) { return whole > 0 ? (int)((200L * part + whole) / (2L * whole)) : 0; }

void tf_compute(const tf_second_t *ring, int cap, int head, int n, int cot, int restarts, const tf_thresholds_t *th,
                tf_facts_t *out) {
    memset(out, 0, sizeof *out);
    out->cot = cot;
    out->restarts = restarts;
    out->fio2_early_pm = -1;
    out->fio2_late_pm = -1;
    if (ring == NULL || cap <= 0 || th == NULL) return;
    if (n > cap) n = cap;
    if (n < 0) n = 0;
    if (head < 0 || head >= cap) head = 0;
    out->seconds = n;

    int early[BINS], late[BINS];
    memset(early, 0, sizeof early);
    memset(late, 0, sizeof late);
    int early_n = 0, late_n = 0;
    const int third = n / 3;

    int in_target = 0, above = 0, on_o2_valid = 0, below85 = 0, high = 0, answered = 0;
    int desat_run = 0, invalid_run = 0, invalid = 0, invalid_counted = 0;

    for (int i = 0; i < n; i++) {
        const tf_second_t *s = &ring[(head + i) % cap];
        const int fio2 = s->fio2_pm;
        const int valid = s->spo2 >= 0 && s->status == TC_SAMPLE_VALID;

        if (fio2 >= 0) {
            answered++;
            if (fio2 > out->fio2_max_pm) out->fio2_max_pm = fio2;
            if ((double)fio2 >= th->high_fio2 * 1000.0) high++;
            if (i < third) {
                early[bin_of(fio2)]++;
                early_n++;
            } else if (i >= n - third) {
                late[bin_of(fio2)]++;
                late_n++;
            }
        }
        if (s->mode == TC_MODE_HOLD) out->hold_s++;
        if (s->mode == TC_MODE_FALLBACK) out->fallback_s++;

        if (valid) {
            out->valid_seconds++;
            const int room_air = fio2 >= 0 && fio2 <= ROOM_AIR_PM;
            if (room_air ? s->spo2 >= 91 : (s->spo2 >= 91 && s->spo2 <= 95)) in_target++;
            if (!room_air) {
                on_o2_valid++;
                if (s->spo2 > 95) above++;
            }
            if (s->spo2 < 85) below85++;
            if (s->spo2 < DESAT_BELOW) {
                desat_run++;
                if (desat_run == DESAT_MIN_S) out->desat_episodes++;
                if (desat_run >= DESAT_MIN_S && desat_run > out->longest_desat_s) out->longest_desat_s = desat_run;
            } else {
                desat_run = 0;
            }
            invalid_run = 0;
            invalid_counted = 0;
        } else {
            invalid++;
            invalid_run++;
            if (invalid_run >= PROBE_MIN_S && !invalid_counted) {
                out->probe_events++;
                invalid_counted = 1;
            }
            /* A short gap in the signal does not end a desaturation episode; a loss of signal does. */
            if (invalid_run >= PROBE_MIN_S) desat_run = 0;
        }
    }

    if (n > 0) out->fallback_now = ring[(head + n - 1) % cap].mode == TC_MODE_FALLBACK;
    out->risk_pct = n >= TR_WINDOW_SHORT ? tr_percent(ring, cap, head, n) : -1;
    out->time_in_target_pct = pct(in_target, out->valid_seconds);
    out->above_95_on_o2_pct = pct(above, on_o2_valid);
    out->below_85_pct = pct(below85, out->valid_seconds);
    out->invalid_pct = pct(invalid, n);
    out->fio2_early_pm = median_pm(early, early_n);
    out->fio2_late_pm = median_pm(late, late_n);

    /* The flags. A window too short to show a trend raises no trend flag, and rates need a window to divide by. */
    uint32_t flags = 0;
    const double hours = (double)n / 3600.0;
    const int trend_ok = out->valid_seconds >= th->min_trend_window_s && early_n > 0 && late_n > 0;
    const double delta = trend_ok ? (double)(out->fio2_late_pm - out->fio2_early_pm) / 1000.0 : 0.0;
    if (trend_ok && delta >= th->rising_delta) flags |= TF_FLAG_RISING_NEED;
    if (trend_ok && delta <= -th->wean_delta) flags |= TF_FLAG_WEAN;
    const int rate_ok = n >= th->min_rate_window_s && hours > 0.0;
    if (rate_ok && out->desat_episodes >= 2 && (double)out->desat_episodes / hours >= th->desats_per_h) {
        flags |= TF_FLAG_DESATS;
    }
    if (rate_ok && out->probe_events >= 2 && (double)out->probe_events / hours >= th->probe_per_h) {
        flags |= TF_FLAG_PROBE;
    }
    if (out->fallback_now || (rate_ok && ((double)out->hold_s / hours >= th->hold_s_per_h ||
                                          (double)out->fallback_s / hours >= th->fallback_s_per_h))) {
        flags |= TF_FLAG_HOLD_FALLBACK;
    }
    if (answered > 0 && 100.0 * high / answered >= th->high_fio2_pct) flags |= TF_FLAG_HIGH_NEED;
    if (restarts > 0) flags |= TF_FLAG_SYSTEM;
    /* Falling and rising need cannot both hold; rising wins, because it is the one that can harm. */
    if (flags & TF_FLAG_RISING_NEED) flags &= ~TF_FLAG_WEAN;
    out->flags = flags;

    /* The deterministic order: what can harm first, then what wastes the nurse's time, then the numbers. */
    int p = 0;
    if (flags & TF_FLAG_HOLD_FALLBACK) p += 400;
    if (flags & TF_FLAG_DESATS) p += 300;
    if (flags & TF_FLAG_HIGH_NEED) p += 250;
    if (flags & TF_FLAG_RISING_NEED) p += 200;
    if (flags & TF_FLAG_SYSTEM) p += 100;
    if (flags & TF_FLAG_PROBE) p += 80;
    if (flags & TF_FLAG_WEAN) p += 20;
    p += 100 - out->time_in_target_pct;
    /* The forecast breaks ties among cots with the same flags; it can never outweigh a flag that can harm. */
    if (out->risk_pct > 0) p += out->risk_pct / 2;
    out->priority = p;
}

int tf_json(const tf_facts_t *f, char *buf, size_t cap) {
    int n = snprintf(buf, cap,
                     "{\"cot\":%d,\"seconds\":%d,\"valid\":%d,\"tir\":%d,\"above95\":%d,\"below85\":%d,\"desats\":%d,"
                     "\"longest\":%d,\"fio2_early\":%.2f,\"fio2_late\":%.2f,\"fio2_max\":%.2f,\"probe\":%d,\"invalid\":%d,"
                     "\"hold\":%d,\"fallback\":%d,\"restarts\":%d,\"risk\":%d,\"flags\":%u,\"priority\":%d}",
                     f->cot, f->seconds, f->valid_seconds, f->time_in_target_pct, f->above_95_on_o2_pct, f->below_85_pct,
                     f->desat_episodes, f->longest_desat_s, f->fio2_early_pm / 1000.0, f->fio2_late_pm / 1000.0,
                     f->fio2_max_pm / 1000.0, f->probe_events, f->invalid_pct, f->hold_s, f->fallback_s, f->restarts,
                     f->risk_pct, (unsigned)f->flags, f->priority);
    return n >= 0 && (size_t)n < cap ? n : -1;
}
