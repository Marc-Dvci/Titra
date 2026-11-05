/* Sensor-level signal quality rules (docs/SPEC.md, "Signal quality rules"). */
#include <string.h>

#include "check.h"
#include "titra/quality.h"

static tc_quality_t fresh(void) {
    tc_quality_t q;
    tc_quality_cfg_t c = tc_quality_cfg_default();
    tc_quality_init(&q, &c);
    return q;
}

static tc_ox_frame_t frame(uint32_t clock, int spo2, int pr, int pi, unsigned flags) {
    tc_ox_frame_t f = {clock, spo2, pr, pi, flags};
    return f;
}

TEST(test_quality_defaults_match_spec) {
    tc_quality_cfg_t c = tc_quality_cfg_default();
    CHECK_EQ_INT(c.repeat_clock_frames, 3);
    CHECK_EQ_INT(c.frozen_frames, 20);
    CHECK_EQ_INT(c.pr_min, 30);
    CHECK_EQ_INT(c.pr_max, 300);
}

TEST(test_quality_clean_frame_is_valid) {
    tc_quality_t q = fresh();
    tc_ox_frame_t f = frame(1, 93, 155, 150, 0);
    tc_sample_t s = tc_quality_on_frame(&q, &f, 10.0);
    CHECK_EQ_INT(s.status, TC_SAMPLE_VALID);
    CHECK_EQ_INT(s.reasons, 0);
    CHECK_EQ_INT(s.spo2, 93);
    CHECK_EQ_INT(s.seq, 1);
    CHECK_NEAR(s.rx_s, 10.0, 1e-12);
}

TEST(test_quality_every_flag_invalidates) {
    for (unsigned bit = 1; bit <= TC_OX_INTERFERENCE; bit <<= 1) {
        tc_quality_t q = fresh();
        tc_ox_frame_t f = frame(1, 93, 155, 150, bit);
        tc_sample_t s = tc_quality_on_frame(&q, &f, 1.0);
        CHECK_EQ_INT(s.status, TC_SAMPLE_INVALID);
        CHECK(s.reasons & TC_Q_FLAGGED);
    }
}

TEST(test_quality_missing_spo2_or_pulse_invalidates) {
    tc_quality_t q = fresh();
    tc_ox_frame_t f = frame(1, TC_ABSENT, 155, 150, 0);
    tc_sample_t s = tc_quality_on_frame(&q, &f, 1.0);
    CHECK_EQ_INT(s.status, TC_SAMPLE_INVALID);
    CHECK(s.reasons & TC_Q_MISSING);
    f = frame(2, 93, TC_ABSENT, 150, 0);
    s = tc_quality_on_frame(&q, &f, 2.0);
    CHECK_EQ_INT(s.status, TC_SAMPLE_INVALID);
    CHECK(s.reasons & TC_Q_MISSING);
}

TEST(test_quality_missing_perfusion_index_is_allowed) {
    tc_quality_t q = fresh();
    tc_ox_frame_t f = frame(1, 93, 155, TC_ABSENT, 0);
    CHECK_EQ_INT(tc_quality_on_frame(&q, &f, 1.0).status, TC_SAMPLE_VALID);
}

TEST(test_quality_pulse_rate_plausibility_bounds) {
    const struct {
        int pr;
        int status;
    } cases[] = {{29, TC_SAMPLE_INVALID}, {30, TC_SAMPLE_VALID}, {300, TC_SAMPLE_VALID}, {301, TC_SAMPLE_INVALID}};
    for (size_t i = 0; i < 4; i++) {
        tc_quality_t q = fresh();
        tc_ox_frame_t f = frame(1, 93, cases[i].pr, 150, 0);
        tc_sample_t s = tc_quality_on_frame(&q, &f, 1.0);
        CHECK_EQ_INT(s.status, cases[i].status);
        if (cases[i].status == TC_SAMPLE_INVALID) CHECK(s.reasons & TC_Q_IMPLAUSIBLE);
    }
}

TEST(test_quality_deep_desaturation_stays_valid) {
    /* A real deep desaturation must reach the controller so that it raises FiO2. */
    tc_quality_t q = fresh();
    for (int spo2 = 0; spo2 <= 100; spo2 += 10) {
        tc_ox_frame_t f = frame((uint32_t)spo2 + 1u, spo2, 90 + spo2, 150 + spo2, 0);
        CHECK_EQ_INT(tc_quality_on_frame(&q, &f, (double)spo2).status, TC_SAMPLE_VALID);
    }
}

TEST(test_quality_repeated_clock_untrusted_on_third_frame) {
    tc_quality_t q = fresh();
    tc_ox_frame_t f = frame(50, 93, 155, 150, 0);
    CHECK_EQ_INT(tc_quality_on_frame(&q, &f, 1.0).status, TC_SAMPLE_VALID);
    CHECK_EQ_INT(tc_quality_on_frame(&q, &f, 2.0).status, TC_SAMPLE_VALID);
    tc_sample_t s = tc_quality_on_frame(&q, &f, 3.0);
    CHECK_EQ_INT(s.status, TC_SAMPLE_UNTRUSTED);
    CHECK(s.reasons & TC_Q_REPEATED);
    /* The first frame of the run was genuine; distrust starts with the first repeat. */
    CHECK_NEAR(s.untrusted_since_s, 2.0, 1e-12);
    s = tc_quality_on_frame(&q, &f, 4.0);
    CHECK_EQ_INT(s.status, TC_SAMPLE_UNTRUSTED);
    CHECK_NEAR(s.untrusted_since_s, 2.0, 1e-12);
    f.clock = 51;
    CHECK_EQ_INT(tc_quality_on_frame(&q, &f, 5.0).status, TC_SAMPLE_VALID);
}

TEST(test_quality_frozen_values_untrusted_on_twentieth_frame) {
    tc_quality_t q = fresh();
    for (int i = 1; i <= 19; i++) {
        tc_ox_frame_t f = frame((uint32_t)i, 93, 155, 150, 0);
        CHECK_EQ_INT(tc_quality_on_frame(&q, &f, (double)i).status, TC_SAMPLE_VALID);
    }
    tc_ox_frame_t f = frame(20, 93, 155, 150, 0);
    tc_sample_t s = tc_quality_on_frame(&q, &f, 20.0);
    CHECK_EQ_INT(s.status, TC_SAMPLE_UNTRUSTED);
    CHECK(s.reasons & TC_Q_FROZEN);
    CHECK_NEAR(s.untrusted_since_s, 2.0, 1e-12);
}

TEST(test_quality_one_changed_value_breaks_the_frozen_run) {
    tc_quality_t q = fresh();
    for (int i = 1; i <= 60; i++) {
        /* The perfusion index changes every 19 frames: never 20 identical in a row. */
        tc_ox_frame_t f = frame((uint32_t)i, 93, 155, 150 + (i / 19), 0);
        CHECK_EQ_INT(tc_quality_on_frame(&q, &f, (double)i).status, TC_SAMPLE_VALID);
    }
}

TEST(test_quality_flagged_frame_breaks_the_frozen_run) {
    tc_quality_t q = fresh();
    for (int i = 1; i <= 40; i++) {
        unsigned flags = (i == 15) ? TC_OX_INTERFERENCE : 0u;
        int spo2 = (i == 15) ? TC_ABSENT : 93;
        tc_ox_frame_t f = frame((uint32_t)i, spo2, 155, 150, flags);
        tc_sample_t s = tc_quality_on_frame(&q, &f, (double)i);
        /* The run restarts at frame 16, so the twentieth identical frame is frame 35. */
        CHECK_EQ_INT(s.status == TC_SAMPLE_UNTRUSTED, i >= 35);
    }
}

TEST(test_quality_untrusted_takes_the_earlier_run_start) {
    tc_quality_t q = fresh();
    tc_sample_t s;
    for (int i = 1; i <= 20; i++) {
        uint32_t clock = (i <= 18) ? (uint32_t)i : 18u; /* clock sticks at frame 18 */
        tc_ox_frame_t f = frame(clock, 93, 155, 150, 0);
        s = tc_quality_on_frame(&q, &f, (double)i);
    }
    CHECK_EQ_INT(s.status, TC_SAMPLE_UNTRUSTED);
    CHECK(s.reasons & TC_Q_REPEATED);
    CHECK(s.reasons & TC_Q_FROZEN);
    CHECK_NEAR(s.untrusted_since_s, 2.0, 1e-12);
}

TEST(test_quality_clock_may_go_backwards_after_restart) {
    tc_quality_t q = fresh();
    tc_ox_frame_t f = frame(9000, 93, 155, 150, 0);
    tc_quality_on_frame(&q, &f, 1.0);
    f = frame(1, 94, 155, 150, 0);
    CHECK_EQ_INT(tc_quality_on_frame(&q, &f, 2.0).status, TC_SAMPLE_VALID);
}

TEST(test_quality_seq_counts_every_frame) {
    tc_quality_t q = fresh();
    tc_sample_t s;
    for (int i = 0; i < 5; i++) {
        tc_ox_frame_t f = frame((uint32_t)i, TC_ABSENT, TC_ABSENT, TC_ABSENT, TC_OX_SENSOR_OFF);
        s = tc_quality_on_frame(&q, &f, (double)i);
    }
    CHECK_EQ_INT(s.seq, 5);
}
