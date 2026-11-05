/* Sensor-level signal quality. See quality.h. */
#include "titra/quality.h"

#include <string.h>

tc_quality_cfg_t tc_quality_cfg_default(void) {
    tc_quality_cfg_t c;
    c.repeat_clock_frames = 3;
    c.frozen_frames = 20;
    c.pr_min = 30;
    c.pr_max = 300;
    return c;
}

void tc_quality_init(tc_quality_t *q, const tc_quality_cfg_t *cfg) {
    memset(q, 0, sizeof(*q));
    q->cfg = *cfg;
}

tc_sample_t tc_quality_on_frame(tc_quality_t *q, const tc_ox_frame_t *f, double rx_s) {
    tc_sample_t s;
    memset(&s, 0, sizeof s);
    s.seq = ++q->seq;
    s.spo2 = f->spo2;
    s.pr = f->pr;
    s.pi_centi = f->pi_centi;
    s.clock = f->clock;
    s.rx_s = rx_s;

    /* Clock rule: the same oximeter clock in consecutive frames means the device is resending a stale frame.
     * The first frame of the run was genuine; the repeats are not. */
    if (q->have_prev && f->clock == q->prev_clock) {
        q->clock_run++;
        if (q->clock_run == 2) q->clock_run_second_rx_s = rx_s;
    } else {
        q->clock_run = 1;
    }

    int has_values = f->spo2 != TC_ABSENT && f->pr != TC_ABSENT;
    /* Frozen rule: identical SpO2, pulse rate and perfusion index across consecutive frames with values. */
    if (has_values && q->have_prev && q->value_run > 0 && f->spo2 == q->prev_spo2 && f->pr == q->prev_pr &&
        f->pi_centi == q->prev_pi) {
        q->value_run++;
        if (q->value_run == 2) q->value_run_second_rx_s = rx_s;
    } else {
        q->value_run = has_values ? 1 : 0;
    }

    q->have_prev = 1;
    q->prev_clock = f->clock;
    q->prev_spo2 = f->spo2;
    q->prev_pr = f->pr;
    q->prev_pi = f->pi_centi;

    uint32_t reasons = 0;
    if (f->flags != 0) reasons |= TC_Q_FLAGGED;
    if (!has_values) reasons |= TC_Q_MISSING;
    if (f->pr != TC_ABSENT && (f->pr < q->cfg.pr_min || f->pr > q->cfg.pr_max)) reasons |= TC_Q_IMPLAUSIBLE;

    double since = 0.0;
    int untrusted = 0;
    if (q->clock_run >= q->cfg.repeat_clock_frames) {
        reasons |= TC_Q_REPEATED;
        untrusted = 1;
        since = q->clock_run_second_rx_s;
    }
    if (q->value_run >= q->cfg.frozen_frames) {
        reasons |= TC_Q_FROZEN;
        if (!untrusted || q->value_run_second_rx_s < since) since = q->value_run_second_rx_s;
        untrusted = 1;
    }

    s.reasons = reasons;
    if (untrusted) {
        s.status = TC_SAMPLE_UNTRUSTED;
        s.untrusted_since_s = since;
    } else if (reasons != 0) {
        s.status = TC_SAMPLE_INVALID;
    } else {
        s.status = TC_SAMPLE_VALID;
    }
    return s;
}
