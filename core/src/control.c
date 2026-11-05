/* The cot controller. See control.h and docs/SPEC.md. */
#include "titra/control.h"

#include <math.h>
#include <string.h>

tc_params_t tc_params_default(void) {
    tc_params_t p;
    memset(&p, 0, sizeof p);
    p.setpoint = 93.0;
    p.target_low = 91.0;
    p.target_high = 95.0;
    /* Tuned on the development scenarios only (bench/tune.py, eval/tuning_log.jsonl). */
    p.kp_lo = 0.0040;
    p.ki_lo = 0.00010986328125;
    p.kp_in = 0.0016;
    p.ki_in = 0.00003125;
    p.kp_hi = 0.0020;
    p.ki_hi = 0.00032;
    p.rate_up = 0.00625;
    p.rate_down = 0.0128;
    p.quantum = 0.01;
    p.hysteresis = 1.0;
    p.max_dt_s = 2.0;
    p.stale_s = 3.0;
    p.hold_s = 30.0;
    p.resume_valid = 5;
    p.baseline_window_s = 300.0;
    p.spo2_low = 85.0;
    p.spo2_low_s = 10.0;
    p.spo2_vlow = 80.0;
    p.spo2_vlow_s = 10.0;
    p.spo2_high = 98.0;
    p.spo2_high_s = 30.0;
    p.at_max_s = 60.0;
    return p;
}

int tc_params_set(tc_params_t *p, const char *name, double v) {
    struct {
        const char *name;
        double *field;
    } doubles[] = {
        {"setpoint", &p->setpoint},       {"target_low", &p->target_low}, {"target_high", &p->target_high},
        {"kp_lo", &p->kp_lo},             {"ki_lo", &p->ki_lo},           {"kp_in", &p->kp_in},
        {"ki_in", &p->ki_in},             {"kp_hi", &p->kp_hi},           {"ki_hi", &p->ki_hi},
        {"rate_up", &p->rate_up},         {"rate_down", &p->rate_down},   {"quantum", &p->quantum},
        {"hysteresis", &p->hysteresis},
        {"max_dt_s", &p->max_dt_s},       {"stale_s", &p->stale_s},       {"hold_s", &p->hold_s},
        {"baseline_window_s", &p->baseline_window_s},
        {"spo2_low", &p->spo2_low},       {"spo2_low_s", &p->spo2_low_s}, {"spo2_vlow", &p->spo2_vlow},
        {"spo2_vlow_s", &p->spo2_vlow_s}, {"spo2_high", &p->spo2_high},   {"spo2_high_s", &p->spo2_high_s},
        {"at_max_s", &p->at_max_s},
    };
    for (size_t i = 0; i < sizeof doubles / sizeof doubles[0]; i++) {
        if (strcmp(doubles[i].name, name) == 0) {
            *doubles[i].field = v;
            return 0;
        }
    }
    if (strcmp(name, "resume_valid") == 0) {
        p->resume_valid = (int)v;
        return 0;
    }
    return -1;
}

static double clamp(double x, double lo, double hi) { return x < lo ? lo : (x > hi ? hi : x); }

static double quantise(const tc_controller_t *c, double x) {
    double q = c->p.quantum > 0.0 ? c->p.quantum : 0.001;
    double r = floor(x / q + 0.5) * q;
    return clamp(r, c->s.fio2_min, c->s.fio2_max);
}

static int to_pm(double fio2) { return (int)floor(fio2 * 1000.0 + 0.5); }

void tc_controller_init(tc_controller_t *c, const tc_params_t *p, const tc_cot_settings_t *s, double initial_fio2,
                        double now_s) {
    memset(c, 0, sizeof(*c));
    c->p = *p;
    c->s = *s;
    tc_baseline_init(&c->baseline, p->baseline_window_s);
    c->mode = TC_MODE_HOLD;
    c->cmd = quantise(c, clamp(initial_fio2, s->fio2_min, s->fio2_max));
    c->u = c->cmd;
    c->fallback_fio2 = c->cmd;
    c->last_update_s = now_s;
    c->last_valid_s = now_s;
    c->low_since_s = c->vlow_since_s = c->high_since_s = c->at_max_since_s = -1.0;
}

double tc_controller_fallback(const tc_controller_t *c) {
    double fb = c->s.backup_fio2;
    int med = tc_baseline_median(&c->baseline);
    if (med >= 0 && med / 1000.0 > fb) fb = med / 1000.0;
    /* Rounded up to the command resolution, so the fallback never sits below the backup FiO2. */
    double q = c->p.quantum > 0.0 ? c->p.quantum : 0.001;
    double up = ceil(fb / q - 1e-9) * q;
    return clamp(up, c->s.fio2_min, c->s.fio2_max);
}

static void enter_fallback(tc_controller_t *c) {
    c->mode = TC_MODE_FALLBACK;
    c->fallback_fio2 = tc_controller_fallback(c);
    c->cmd = c->fallback_fio2;
    c->u = c->cmd;
    c->have_e = 0;
    c->valid_run = 0;
}

/* One proportional-integral update on a new valid sample. */
static void control_update(tc_controller_t *c, int spo2, double now_s) {
    const tc_params_t *p = &c->p;
    double e = p->setpoint - (double)spo2;
    double kp, ki;
    if (spo2 < p->target_low) {
        kp = p->kp_lo;
        ki = p->ki_lo;
    } else if (spo2 > p->target_high) {
        kp = p->kp_hi;
        ki = p->ki_hi;
    } else {
        kp = p->kp_in;
        ki = p->ki_in;
    }
    double dt = clamp(now_s - c->last_update_s, 0.0, p->max_dt_s);
    if (!c->have_e) {
        c->e_prev = e;
        c->have_e = 1;
    }
    double du = kp * (e - c->e_prev) + ki * e * dt;
    du = clamp(du, -p->rate_down * dt, p->rate_up * dt);
    /* Velocity form: clamping the output is the anti-windup, since no integral state can run away. */
    c->u = clamp(c->u + du, c->s.fio2_min, c->s.fio2_max);
    /* Hysteresis: small movements of the internal FiO2 do not reach the blender, so noise in SpO2 does not turn
     * into a stream of one-step adjustments. The command lags the internal value by less than `hysteresis` steps. */
    if (fabs(c->u - c->cmd) >= p->quantum * p->hysteresis - 1e-12) c->cmd = quantise(c, c->u);
    c->e_prev = e;
    tc_baseline_add(&c->baseline, now_s, to_pm(c->cmd));
}

static void update_timer(double *since, int active, double now_s) {
    if (!active) {
        *since = -1.0;
    } else if (*since < 0.0) {
        *since = now_s;
    }
}

static int timer_elapsed(double since, double now_s, double need_s) { return since >= 0.0 && now_s - since >= need_s; }

tc_output_t tc_controller_step(tc_controller_t *c, const tc_sample_t *sample, double now_s) {
    const tc_params_t *p = &c->p;
    double prev_cmd = c->cmd;
    tc_baseline_expire(&c->baseline, now_s);

    int status;
    uint32_t reasons;
    int is_new = 0;
    if (sample == NULL) {
        status = TC_SAMPLE_INVALID;
        reasons = TC_Q_NO_DRIVER;
    } else if (now_s - sample->rx_s >= p->stale_s) {
        status = TC_SAMPLE_INVALID;
        reasons = TC_Q_STALE;
    } else {
        status = sample->status;
        reasons = sample->reasons;
        is_new = !c->have_seq || sample->seq != c->last_seq;
        c->last_seq = sample->seq;
        c->have_seq = 1;
    }

    int untrusted_now = 0;
    if (status == TC_SAMPLE_UNTRUSTED) {
        untrusted_now = 1;
        c->valid_run = 0;
        c->reasons = reasons;
        if (c->mode != TC_MODE_FALLBACK) {
            /* Commands issued since the untrusted run began were driven by bad data: keep them out of the
             * baseline before computing the fallback. */
            tc_baseline_drop_since(&c->baseline, sample->untrusted_since_s);
            enter_fallback(c);
        }
    } else if (status == TC_SAMPLE_INVALID) {
        c->valid_run = 0;
        c->reasons = reasons;
        if (c->mode == TC_MODE_CONTROL) c->mode = TC_MODE_HOLD;
        if (c->mode == TC_MODE_HOLD && now_s - c->last_valid_s >= p->hold_s) enter_fallback(c);
    } else if (is_new) {
        c->last_valid_s = sample->rx_s;
        c->valid_run++;
        if (c->mode == TC_MODE_HOLD) {
            c->mode = TC_MODE_CONTROL;
        } else if (c->mode == TC_MODE_FALLBACK && c->valid_run >= p->resume_valid) {
            c->mode = TC_MODE_CONTROL;
            c->u = c->cmd;
        }
        if (c->mode == TC_MODE_CONTROL) {
            c->reasons = 0;
            control_update(c, sample->spo2, now_s);
        }
    }
    if (c->mode != TC_MODE_CONTROL || is_new) c->last_update_s = now_s;

    /* SpO2 alarms use valid samples only; while SpO2 is not valid the hold and fallback alarms speak instead. */
    int valid = status == TC_SAMPLE_VALID;
    int spo2 = valid ? sample->spo2 : 0;
    update_timer(&c->low_since_s, valid && spo2 < p->spo2_low, now_s);
    update_timer(&c->vlow_since_s, valid && spo2 < p->spo2_vlow, now_s);
    update_timer(&c->high_since_s, valid && spo2 > p->spo2_high && c->cmd > 0.21 + 1e-9, now_s);
    update_timer(&c->at_max_since_s, valid && spo2 < p->target_low && c->cmd >= c->s.fio2_max - 1e-9, now_s);

    tc_output_t out;
    memset(&out, 0, sizeof out);
    uint32_t a = 0;
    if (c->mode == TC_MODE_HOLD) a |= TC_A_SIGNAL_HOLD;
    if (c->mode == TC_MODE_FALLBACK) a |= TC_A_FALLBACK;
    if (untrusted_now) a |= TC_A_UNTRUSTED;
    if (timer_elapsed(c->low_since_s, now_s, p->spo2_low_s)) a |= TC_A_SPO2_LOW;
    if (timer_elapsed(c->vlow_since_s, now_s, p->spo2_vlow_s)) a |= TC_A_SPO2_VLOW;
    if (timer_elapsed(c->high_since_s, now_s, p->spo2_high_s)) a |= TC_A_SPO2_HIGH;
    if (timer_elapsed(c->at_max_since_s, now_s, p->at_max_s)) a |= TC_A_AT_MAX;
    int level = TC_ALARM_NONE;
    if (a & (TC_A_SIGNAL_HOLD | TC_A_SPO2_LOW | TC_A_SPO2_HIGH)) level = TC_ALARM_MEDIUM;
    if (a & (TC_A_FALLBACK | TC_A_UNTRUSTED | TC_A_SPO2_VLOW | TC_A_AT_MAX)) level = TC_ALARM_HIGH;

    out.fio2 = c->cmd;
    out.mode = c->mode;
    out.alarm = level;
    out.alarms = a;
    out.reasons = c->mode == TC_MODE_CONTROL ? 0u : c->reasons;
    out.fallback_fio2 = c->mode == TC_MODE_FALLBACK ? c->fallback_fio2 : tc_controller_fallback(c);
    out.adjusted = fabs(c->cmd - prev_cmd) > 1e-9;
    return out;
}

const char *tc_mode_name(int mode) {
    switch (mode) {
        case TC_MODE_CONTROL: return "control";
        case TC_MODE_HOLD: return "hold";
        case TC_MODE_FALLBACK: return "fallback";
    }
    return "unknown";
}
