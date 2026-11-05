/* The cot controller: control law, hold, fallback, resume and alarms (docs/SPEC.md). */
#include <string.h>

#include "check.h"
#include "titra/control.h"

static const tc_cot_settings_t SETTINGS = {0.21, 1.00, 0.30};

typedef struct {
    tc_controller_t c;
    tc_sample_t s;
    double t;
} rig_t;

static void rig_init(rig_t *r, double fio2) {
    memset(r, 0, sizeof(*r));
    tc_params_t p = tc_params_default();
    tc_controller_init(&r->c, &p, &SETTINGS, fio2, 0.0);
}

/* Advances one second with a new sample of the given status and SpO2, and runs one tick. */
static tc_output_t step(rig_t *r, int status, int spo2) {
    r->t += 1.0;
    r->s.seq++;
    r->s.status = status;
    r->s.spo2 = spo2;
    r->s.reasons = status == TC_SAMPLE_VALID ? 0u : TC_Q_FLAGGED;
    r->s.rx_s = r->t - 0.2;
    return tc_controller_step(&r->c, &r->s, r->t);
}

/* Advances one second with no new frame. */
static tc_output_t quiet(rig_t *r) {
    r->t += 1.0;
    return tc_controller_step(&r->c, &r->s, r->t);
}

TEST(test_control_defaults_match_spec) {
    tc_params_t p = tc_params_default();
    CHECK_NEAR(p.setpoint, 93.0, 0);
    CHECK_NEAR(p.target_low, 91.0, 0);
    CHECK_NEAR(p.target_high, 95.0, 0);
    CHECK_NEAR(p.stale_s, 3.0, 0);
    CHECK_NEAR(p.hold_s, 30.0, 0);
    CHECK_NEAR(p.baseline_window_s, 300.0, 0);
    CHECK_EQ_INT(p.resume_valid, 5);
    CHECK(p.kp_lo > 0 && p.ki_lo > 0 && p.kp_in > 0 && p.ki_in > 0 && p.kp_hi > 0 && p.ki_hi > 0);
    /* One update over the longest interval, plus rounding and hysteresis, stays inside the driver's slew limit. */
    CHECK(p.rate_up * p.max_dt_s + 1.5 * p.quantum <= 0.05 + 1e-12);
    CHECK(p.rate_down * p.max_dt_s + 1.5 * p.quantum <= 0.05 + 1e-12);
}

TEST(test_control_params_set_by_name) {
    tc_params_t p = tc_params_default();
    CHECK_EQ_INT(tc_params_set(&p, "ki_lo", 0.5), 0);
    CHECK_NEAR(p.ki_lo, 0.5, 0);
    CHECK_EQ_INT(tc_params_set(&p, "resume_valid", 7), 0);
    CHECK_EQ_INT(p.resume_valid, 7);
    CHECK_EQ_INT(tc_params_set(&p, "no_such_gain", 1.0), -1);
}

TEST(test_control_starts_in_hold_at_the_initial_fio2) {
    rig_t r;
    rig_init(&r, 0.354);
    CHECK_EQ_INT(r.c.mode, TC_MODE_HOLD);
    CHECK_NEAR(r.c.cmd, 0.35, 1e-9);
    tc_output_t o = step(&r, TC_SAMPLE_VALID, 93);
    CHECK_EQ_INT(o.mode, TC_MODE_CONTROL);
    CHECK_NEAR(o.fio2, 0.35, 1e-9);
}

TEST(test_control_initial_fio2_is_clamped_to_the_limits) {
    rig_t r;
    rig_init(&r, 0.10);
    CHECK_NEAR(r.c.cmd, 0.21, 1e-9);
    rig_init(&r, 1.5);
    CHECK_NEAR(r.c.cmd, 1.00, 1e-9);
}

TEST(test_control_hypoxaemia_raises_fio2_within_the_rate_limit) {
    rig_t r;
    rig_init(&r, 0.30);
    tc_params_t p = tc_params_default();
    double prev = 0.30;
    for (int i = 0; i < 120; i++) {
        tc_output_t o = step(&r, TC_SAMPLE_VALID, 80);
        CHECK(o.fio2 >= prev - 1e-9);
        CHECK(o.fio2 - prev <= p.rate_up * 1.0 + p.quantum + 1e-9);
        prev = o.fio2;
    }
    CHECK(prev > 0.40);
}

TEST(test_control_hyperoxaemia_lowers_fio2_within_the_rate_limit) {
    rig_t r;
    rig_init(&r, 0.60);
    tc_params_t p = tc_params_default();
    double prev = 0.60;
    for (int i = 0; i < 120; i++) {
        tc_output_t o = step(&r, TC_SAMPLE_VALID, 98);
        CHECK(o.fio2 <= prev + 1e-9);
        CHECK(prev - o.fio2 <= p.rate_down * 1.0 + p.quantum + 1e-9);
        prev = o.fio2;
    }
    CHECK(prev < 0.50);
}

TEST(test_control_at_setpoint_holds_fio2) {
    rig_t r;
    rig_init(&r, 0.40);
    for (int i = 0; i < 600; i++) CHECK_NEAR(step(&r, TC_SAMPLE_VALID, 93).fio2, 0.40, 1e-9);
}

TEST(test_control_never_leaves_the_limits) {
    rig_t r;
    rig_init(&r, 0.90);
    for (int i = 0; i < 2000; i++) CHECK(step(&r, TC_SAMPLE_VALID, 60).fio2 <= 1.00 + 1e-9);
    rig_init(&r, 0.25);
    for (int i = 0; i < 2000; i++) CHECK(step(&r, TC_SAMPLE_VALID, 100).fio2 >= 0.21 - 1e-9);
}

TEST(test_control_output_is_quantised) {
    rig_t r;
    rig_init(&r, 0.30);
    for (int i = 0; i < 200; i++) {
        double f = step(&r, TC_SAMPLE_VALID, 85 + (i % 15)).fio2 * 100.0;
        CHECK_NEAR(f, (double)(long)(f + 0.5), 1e-6);
    }
}

TEST(test_control_no_new_frame_changes_nothing) {
    rig_t r;
    rig_init(&r, 0.30);
    step(&r, TC_SAMPLE_VALID, 80);
    double f = step(&r, TC_SAMPLE_VALID, 80).fio2;
    tc_output_t o = quiet(&r);
    CHECK_EQ_INT(o.mode, TC_MODE_CONTROL);
    CHECK_NEAR(o.fio2, f, 1e-12);
    CHECK_EQ_INT(o.adjusted, 0);
}

TEST(test_control_flagged_sample_holds_and_alarms_at_once) {
    rig_t r;
    rig_init(&r, 0.30);
    for (int i = 0; i < 30; i++) step(&r, TC_SAMPLE_VALID, 88);
    double held = r.c.cmd;
    tc_output_t o = step(&r, TC_SAMPLE_INVALID, 0);
    CHECK_EQ_INT(o.mode, TC_MODE_HOLD);
    CHECK(o.alarms & TC_A_SIGNAL_HOLD);
    CHECK_EQ_INT(o.alarm, TC_ALARM_MEDIUM);
    CHECK(o.reasons & TC_Q_FLAGGED);
    for (int i = 0; i < 28; i++) CHECK_NEAR(step(&r, TC_SAMPLE_INVALID, 0).fio2, held, 1e-12);
}

TEST(test_control_hold_falls_back_30_s_after_the_last_valid_sample) {
    rig_t r;
    rig_init(&r, 0.30);
    for (int i = 0; i < 60; i++) step(&r, TC_SAMPLE_VALID, 93);
    double last_valid_rx = r.s.rx_s;
    tc_output_t o;
    int ticks = 0;
    do {
        o = step(&r, TC_SAMPLE_INVALID, 0);
        ticks++;
    } while (o.mode != TC_MODE_FALLBACK && ticks < 100);
    CHECK_EQ_INT(o.mode, TC_MODE_FALLBACK);
    CHECK(r.t - last_valid_rx >= 30.0);
    CHECK(r.t - last_valid_rx < 31.0);
    CHECK_EQ_INT(o.alarm, TC_ALARM_HIGH);
    CHECK(o.alarms & TC_A_FALLBACK);
}

TEST(test_control_stale_driver_counts_as_missing) {
    rig_t r;
    rig_init(&r, 0.30);
    step(&r, TC_SAMPLE_VALID, 93);
    tc_output_t o = quiet(&r);
    CHECK_EQ_INT(o.mode, TC_MODE_CONTROL);
    quiet(&r);
    o = quiet(&r); /* now 3.2 s after the last frame */
    CHECK_EQ_INT(o.mode, TC_MODE_HOLD);
    CHECK(o.reasons & TC_Q_STALE);
}

TEST(test_control_absent_driver_counts_as_missing) {
    rig_t r;
    rig_init(&r, 0.30);
    step(&r, TC_SAMPLE_VALID, 93);
    tc_output_t o = tc_controller_step(&r.c, NULL, r.t + 1.0);
    CHECK_EQ_INT(o.mode, TC_MODE_HOLD);
    CHECK(o.reasons & TC_Q_NO_DRIVER);
    o = tc_controller_step(&r.c, NULL, r.t + 40.0);
    CHECK_EQ_INT(o.mode, TC_MODE_FALLBACK);
}

TEST(test_control_fallback_uses_backup_when_baseline_is_lower) {
    rig_t r;
    rig_init(&r, 0.25);
    for (int i = 0; i < 300; i++) step(&r, TC_SAMPLE_VALID, 93);
    for (int i = 0; i < 40; i++) step(&r, TC_SAMPLE_INVALID, 0);
    CHECK_EQ_INT(r.c.mode, TC_MODE_FALLBACK);
    CHECK_NEAR(r.c.cmd, 0.30, 1e-9);
}

TEST(test_control_fallback_never_sits_below_the_baseline) {
    rig_t r;
    rig_init(&r, 0.45);
    for (int i = 0; i < 300; i++) step(&r, TC_SAMPLE_VALID, 93);
    for (int i = 0; i < 40; i++) step(&r, TC_SAMPLE_INVALID, 0);
    CHECK_EQ_INT(r.c.mode, TC_MODE_FALLBACK);
    CHECK_NEAR(r.c.cmd, 0.45, 1e-9);
}

TEST(test_control_fallback_rounds_up_so_it_never_sits_below_backup) {
    tc_controller_t c;
    tc_params_t p = tc_params_default();
    tc_cot_settings_t s = {0.21, 1.00, 0.235};
    tc_controller_init(&c, &p, &s, 0.22, 0.0);
    CHECK_NEAR(tc_controller_fallback(&c), 0.24, 1e-9);
    s.backup_fio2 = 0.30;
    tc_controller_init(&c, &p, &s, 0.22, 0.0);
    CHECK_NEAR(tc_controller_fallback(&c), 0.30, 1e-9);
    s.backup_fio2 = 1.20;
    tc_controller_init(&c, &p, &s, 0.22, 0.0);
    CHECK_NEAR(tc_controller_fallback(&c), 1.00, 1e-9);
}

TEST(test_control_untrusted_falls_back_at_once_excluding_the_untrusted_run) {
    rig_t r;
    rig_init(&r, 0.30);
    /* Few pre-fault entries, so that without the exclusion the untrusted ones would set the median. */
    for (int i = 0; i < 10; i++) step(&r, TC_SAMPLE_VALID, 93);
    double since = r.t + 0.8; /* the next sample's receive time */
    /* A frozen low reading drives FiO2 up for 19 s before the rule fires. */
    for (int i = 0; i < 19; i++) step(&r, TC_SAMPLE_VALID, 80);
    CHECK(r.c.cmd > 0.32);
    r.s.untrusted_since_s = since;
    tc_output_t o = step(&r, TC_SAMPLE_UNTRUSTED, 80);
    CHECK_EQ_INT(o.mode, TC_MODE_FALLBACK);
    CHECK(o.alarms & TC_A_UNTRUSTED);
    CHECK_EQ_INT(o.alarm, TC_ALARM_HIGH);
    /* The escalation came from untrusted data, so the fallback is the pre-fault baseline. */
    CHECK_NEAR(o.fio2, 0.30, 1e-9);
}

TEST(test_control_frozen_reading_moves_fio2_by_at_most_20_ticks_of_rate) {
    rig_t r;
    tc_params_t p = tc_params_default();
    rig_init(&r, 0.30);
    for (int i = 0; i < 100; i++) step(&r, TC_SAMPLE_VALID, 93);
    double before = r.c.cmd;
    for (int i = 0; i < 20; i++) step(&r, TC_SAMPLE_VALID, 40);
    CHECK(r.c.cmd - before <= 20 * p.rate_up + p.quantum + 1e-9);
}

TEST(test_control_resumes_from_fallback_after_five_valid_samples) {
    rig_t r;
    rig_init(&r, 0.30);
    for (int i = 0; i < 60; i++) step(&r, TC_SAMPLE_VALID, 93);
    for (int i = 0; i < 40; i++) step(&r, TC_SAMPLE_INVALID, 0);
    CHECK_EQ_INT(r.c.mode, TC_MODE_FALLBACK);
    double fb = r.c.cmd;
    for (int i = 0; i < 4; i++) {
        tc_output_t o = step(&r, TC_SAMPLE_VALID, 85);
        CHECK_EQ_INT(o.mode, TC_MODE_FALLBACK);
        CHECK_NEAR(o.fio2, fb, 1e-12);
    }
    tc_output_t o = step(&r, TC_SAMPLE_VALID, 85);
    CHECK_EQ_INT(o.mode, TC_MODE_CONTROL);
    /* Bumpless: the first update starts from the fallback FiO2 and moves by at most one rate step. */
    CHECK(o.fio2 - fb <= 0.02 + 1e-9);
}

TEST(test_control_invalid_sample_restarts_the_resume_count) {
    rig_t r;
    rig_init(&r, 0.30);
    for (int i = 0; i < 40; i++) step(&r, TC_SAMPLE_INVALID, 0);
    CHECK_EQ_INT(r.c.mode, TC_MODE_FALLBACK);
    for (int i = 0; i < 4; i++) step(&r, TC_SAMPLE_VALID, 93);
    step(&r, TC_SAMPLE_INVALID, 0);
    for (int i = 0; i < 4; i++) CHECK_EQ_INT(step(&r, TC_SAMPLE_VALID, 93).mode, TC_MODE_FALLBACK);
    CHECK_EQ_INT(step(&r, TC_SAMPLE_VALID, 93).mode, TC_MODE_CONTROL);
}

TEST(test_control_hold_resumes_on_the_first_valid_sample) {
    rig_t r;
    rig_init(&r, 0.30);
    step(&r, TC_SAMPLE_VALID, 93);
    for (int i = 0; i < 10; i++) step(&r, TC_SAMPLE_INVALID, 0);
    tc_output_t o = step(&r, TC_SAMPLE_VALID, 70);
    CHECK_EQ_INT(o.mode, TC_MODE_CONTROL);
    /* The first update starts at once; hysteresis lets the command move once the change reaches a full step. */
    for (int i = 0; i < 4; i++) o = step(&r, TC_SAMPLE_VALID, 70);
    CHECK(o.fio2 > 0.30);
}

TEST(test_control_low_spo2_alarms_after_ten_seconds) {
    rig_t r;
    rig_init(&r, 1.00);
    tc_output_t o;
    for (int i = 0; i < 10; i++) {
        o = step(&r, TC_SAMPLE_VALID, 84);
        CHECK((o.alarms & TC_A_SPO2_LOW) == 0);
    }
    o = step(&r, TC_SAMPLE_VALID, 84);
    CHECK(o.alarms & TC_A_SPO2_LOW);
    CHECK_EQ_INT(o.alarm >= TC_ALARM_MEDIUM, 1);
    o = step(&r, TC_SAMPLE_VALID, 90);
    CHECK((o.alarms & TC_A_SPO2_LOW) == 0);
}

TEST(test_control_very_low_spo2_and_at_max_alarm_high) {
    rig_t r;
    rig_init(&r, 1.00);
    tc_output_t o;
    for (int i = 0; i < 61; i++) o = step(&r, TC_SAMPLE_VALID, 75);
    CHECK(o.alarms & TC_A_SPO2_VLOW);
    CHECK(o.alarms & TC_A_AT_MAX);
    CHECK_EQ_INT(o.alarm, TC_ALARM_HIGH);
}

TEST(test_control_high_spo2_alarms_only_on_oxygen) {
    rig_t r;
    rig_init(&r, 0.21);
    tc_output_t o;
    for (int i = 0; i < 60; i++) o = step(&r, TC_SAMPLE_VALID, 100);
    CHECK((o.alarms & TC_A_SPO2_HIGH) == 0);
    CHECK_EQ_INT(o.alarm, TC_ALARM_NONE);
    tc_params_t p = tc_params_default();
    p.rate_down = 0.0; /* keep FiO2 above room air for the test */
    tc_controller_init(&r.c, &p, &SETTINGS, 0.40, r.t);
    for (int i = 0; i < 32; i++) o = step(&r, TC_SAMPLE_VALID, 99);
    CHECK(o.alarms & TC_A_SPO2_HIGH);
}

TEST(test_control_mode_names) {
    CHECK(strcmp(tc_mode_name(TC_MODE_CONTROL), "control") == 0);
    CHECK(strcmp(tc_mode_name(TC_MODE_HOLD), "hold") == 0);
    CHECK(strcmp(tc_mode_name(TC_MODE_FALLBACK), "fallback") == 0);
    CHECK(strcmp(tc_mode_name(9), "unknown") == 0);
}
