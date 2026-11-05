/* Blender driver guard and link watchdog. See guard.h. */
#include "titra/guard.h"

#include <string.h>

#include "titra/control.h"

tc_guard_result_t tc_guard_check(const tc_guard_cfg_t *g, int have_last, int last_pm, int req_pm, int mode) {
    if (req_pm < g->min_pm) return TC_GUARD_BELOW_MIN;
    if (req_pm > g->max_pm) return TC_GUARD_ABOVE_MAX;
    switch (mode) {
        case TC_MODE_CONTROL:
        case TC_MODE_HOLD: {
            int step = req_pm - last_pm;
            if (step < 0) step = -step;
            if (have_last && step > g->slew_pm) return TC_GUARD_SLEW;
            return TC_GUARD_OK;
        }
        case TC_MODE_FALLBACK:
            /* A fallback may step by any amount, but never below the backup FiO2. */
            if (req_pm < g->backup_pm) return TC_GUARD_FALLBACK_LOW;
            return TC_GUARD_OK;
        default:
            return TC_GUARD_MODE;
    }
}

const char *tc_guard_result_name(tc_guard_result_t r) {
    switch (r) {
        case TC_GUARD_OK: return "ok";
        case TC_GUARD_BELOW_MIN: return "below_min";
        case TC_GUARD_ABOVE_MAX: return "above_max";
        case TC_GUARD_SLEW: return "slew";
        case TC_GUARD_FALLBACK_LOW: return "fallback_below_backup";
        case TC_GUARD_MODE: return "mode";
    }
    return "unknown";
}

void tc_linkwd_init(tc_linkwd_t *w, double hold_after_s, double fallback_after_s, double baseline_window_s,
                    int backup_pm, int initial_pm, double now_s) {
    memset(w, 0, sizeof(*w));
    w->hold_after_s = hold_after_s;
    w->fallback_after_s = fallback_after_s;
    w->backup_pm = backup_pm;
    w->last_cmd_s = now_s;
    w->last_pm = initial_pm;
    w->state = TC_LINK_NORMAL;
    w->fallback_pm = initial_pm;
    tc_baseline_init(&w->baseline, baseline_window_s);
}

void tc_linkwd_command(tc_linkwd_t *w, double now_s, int pm) {
    w->last_cmd_s = now_s;
    w->last_pm = pm;
    w->state = TC_LINK_NORMAL;
    tc_baseline_add(&w->baseline, now_s, pm);
}

int tc_linkwd_tick(tc_linkwd_t *w, double now_s, int *out_pm) {
    double quiet = now_s - w->last_cmd_s;
    if (quiet >= w->fallback_after_s) {
        if (w->state != TC_LINK_FALLBACK) {
            /* The baseline covers commands up to the last one received, so it is taken before expiry. */
            int med = tc_baseline_median(&w->baseline);
            w->fallback_pm = med > w->backup_pm ? med : w->backup_pm;
            w->state = TC_LINK_FALLBACK;
        }
        *out_pm = w->fallback_pm;
    } else if (quiet >= w->hold_after_s) {
        w->state = TC_LINK_HOLD;
        *out_pm = w->last_pm;
    } else {
        w->state = TC_LINK_NORMAL;
        *out_pm = w->last_pm;
    }
    return w->state;
}
