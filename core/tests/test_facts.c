/* Facts and flags: each rule at its boundaries, and the healthy case that must raise nothing. */
#include <string.h>

#include "check.h"
#include "titra/control.h"
#include "titra/facts.h"
#include "titra/quality.h"

#define CAP 14400
static tf_second_t ring[CAP];

/* A calm stretch: SpO2 93, FiO2 0.30, in control. */
static void calm(int n) {
    for (int i = 0; i < n; i++) {
        ring[i].spo2 = 93;
        ring[i].fio2_pm = 300;
        ring[i].mode = TC_MODE_CONTROL;
        ring[i].alarm = 0;
        ring[i].status = TC_SAMPLE_VALID;
        ring[i].reserved = 0;
    }
}

static tf_facts_t facts(int n, int restarts) {
    tf_facts_t f;
    const tf_thresholds_t th = tf_thresholds_default();
    tf_compute(ring, CAP, 0, n, 2, restarts, &th, &f);
    return f;
}

static void set_spo2(int from, int to, int v) {
    for (int i = from; i < to; i++) ring[i].spo2 = (int16_t)v;
}

static void set_fio2(int from, int to, int pm) {
    for (int i = from; i < to; i++) ring[i].fio2_pm = (int16_t)pm;
}

static void lose_signal(int from, int to) {
    for (int i = from; i < to; i++) ring[i].status = TC_SAMPLE_INVALID;
}

static void set_mode(int from, int to, int mode) {
    for (int i = from; i < to; i++) ring[i].mode = (uint8_t)mode;
}

TEST(test_a_calm_window_raises_no_flag) {
    calm(10800);
    tf_facts_t f = facts(10800, 0);
    CHECK_EQ_INT(f.flags, 0);
    CHECK_EQ_INT(f.cot, 2);
    CHECK_EQ_INT(f.seconds, 10800);
    CHECK_EQ_INT(f.valid_seconds, 10800);
    CHECK_EQ_INT(f.time_in_target_pct, 100);
    CHECK_EQ_INT(f.desat_episodes, 0);
    CHECK_EQ_INT(f.probe_events, 0);
    CHECK_EQ_INT(f.fio2_early_pm, 300);
    CHECK_EQ_INT(f.fio2_late_pm, 300);
    CHECK_EQ_INT(f.priority, 0);
    CHECK_EQ_INT(f.fallback_now, 0);
}

TEST(test_time_in_target_uses_the_room_air_rule) {
    calm(3600);
    set_spo2(0, 3600, 99);
    set_fio2(0, 3600, 210);
    tf_facts_t f = facts(3600, 0);
    CHECK_EQ_INT(f.time_in_target_pct, 100);
    CHECK_EQ_INT(f.above_95_on_o2_pct, 0);
    set_fio2(0, 3600, 300);
    f = facts(3600, 0);
    CHECK_EQ_INT(f.time_in_target_pct, 0);
    CHECK_EQ_INT(f.above_95_on_o2_pct, 100);
}

TEST(test_a_desaturation_counts_from_ten_seconds_and_once) {
    calm(3600);
    set_spo2(100, 109, 80); /* 9 s: not an episode */
    CHECK_EQ_INT(facts(3600, 0).desat_episodes, 0);
    ring[109].spo2 = 80; /* 10 s */
    tf_facts_t f = facts(3600, 0);
    CHECK_EQ_INT(f.desat_episodes, 1);
    CHECK_EQ_INT(f.longest_desat_s, 10);
    set_spo2(110, 150, 80); /* still one episode */
    f = facts(3600, 0);
    CHECK_EQ_INT(f.desat_episodes, 1);
    CHECK_EQ_INT(f.longest_desat_s, 50);
}

TEST(test_spo2_at_85_is_not_a_desaturation) {
    calm(3600);
    set_spo2(100, 200, 85);
    CHECK_EQ_INT(facts(3600, 0).desat_episodes, 0);
    set_spo2(100, 200, 84);
    CHECK_EQ_INT(facts(3600, 0).desat_episodes, 1);
}

TEST(test_frequent_desaturations_needs_the_rate_and_at_least_two) {
    calm(3600);
    set_spo2(100, 120, 70);
    CHECK(!(facts(3600, 0).flags & TF_FLAG_DESATS)); /* 1 per hour */
    set_spo2(300, 320, 70);
    CHECK(facts(3600, 0).flags & TF_FLAG_DESATS); /* 2 per hour */
    /* The same two over two hours is 1 per hour. */
    calm(7200);
    set_spo2(100, 120, 70);
    set_spo2(300, 320, 70);
    CHECK(!(facts(7200, 0).flags & TF_FLAG_DESATS));
    /* One episode in a short window has a high rate but is one event. */
    calm(900);
    set_spo2(100, 120, 70);
    CHECK(!(facts(900, 0).flags & TF_FLAG_DESATS));
}

TEST(test_a_window_too_short_gives_no_rate_flags) {
    calm(300);
    set_spo2(10, 30, 70);
    set_spo2(50, 70, 70);
    CHECK(!(facts(300, 0).flags & TF_FLAG_DESATS));
    calm(600);
    set_spo2(10, 30, 70);
    set_spo2(50, 70, 70);
    CHECK(facts(600, 0).flags & TF_FLAG_DESATS);
}

TEST(test_rising_need_needs_the_delta_and_enough_history) {
    calm(10800);
    set_fio2(7200, 10800, 320);
    CHECK(!(facts(10800, 0).flags & TF_FLAG_RISING_NEED)); /* 0.02 */
    set_fio2(7200, 10800, 330);
    tf_facts_t f = facts(10800, 0);
    CHECK(f.flags & TF_FLAG_RISING_NEED); /* 0.03 */
    CHECK_EQ_INT(f.fio2_late_pm, 330);
    /* The same rise in an hour is not enough history for a trend. */
    calm(3600);
    set_fio2(2400, 3600, 400);
    CHECK(!(facts(3600, 0).flags & TF_FLAG_RISING_NEED));
}

TEST(test_a_falling_need_is_a_wean_opportunity) {
    calm(10800);
    set_fio2(7200, 10800, 270);
    CHECK(facts(10800, 0).flags & TF_FLAG_WEAN);
    set_fio2(7200, 10800, 280);
    CHECK(!(facts(10800, 0).flags & TF_FLAG_WEAN));
}

TEST(test_time_above_target_alone_is_not_a_wean_flag) {
    /* A healthy baby near room air often sits above 95%, and the controller is already weaning it. */
    calm(10800);
    set_spo2(0, 10800, 97);
    set_fio2(0, 10800, 220);
    tf_facts_t f = facts(10800, 0);
    CHECK_EQ_INT(f.above_95_on_o2_pct, 100);
    CHECK_EQ_INT(f.flags, 0);
}

TEST(test_signal_loss_counts_from_three_seconds_and_once) {
    calm(3600);
    lose_signal(100, 102);
    CHECK_EQ_INT(facts(3600, 0).probe_events, 0);
    lose_signal(102, 103);
    CHECK_EQ_INT(facts(3600, 0).probe_events, 1);
    lose_signal(103, 200);
    CHECK_EQ_INT(facts(3600, 0).probe_events, 1);
}

TEST(test_missing_spo2_counts_as_no_reading) {
    calm(3600);
    set_spo2(100, 110, -1);
    tf_facts_t f = facts(3600, 0);
    CHECK_EQ_INT(f.probe_events, 1);
    CHECK_EQ_INT(f.valid_seconds, 3590);
}

TEST(test_probe_trouble_at_its_rate) {
    calm(3600);
    for (int k = 0; k < 7; k++) lose_signal(100 + 200 * k, 110 + 200 * k);
    tf_facts_t f = facts(3600, 0);
    CHECK_EQ_INT(f.probe_events, 7);
    CHECK(!(f.flags & TF_FLAG_PROBE));
    lose_signal(1600, 1610);
    f = facts(3600, 0);
    CHECK_EQ_INT(f.probe_events, 8);
    CHECK(f.flags & TF_FLAG_PROBE);
}

TEST(test_hold_and_fallback_need_time_unless_fallback_is_now) {
    calm(3600);
    set_mode(100, 249, TC_MODE_HOLD);
    CHECK(!(facts(3600, 0).flags & TF_FLAG_HOLD_FALLBACK)); /* 149 s an hour */
    set_mode(249, 250, TC_MODE_HOLD);
    CHECK(facts(3600, 0).flags & TF_FLAG_HOLD_FALLBACK);
    calm(3600);
    set_mode(100, 219, TC_MODE_FALLBACK);
    CHECK(!(facts(3600, 0).flags & TF_FLAG_HOLD_FALLBACK)); /* 119 s an hour */
    set_mode(219, 220, TC_MODE_FALLBACK);
    CHECK(facts(3600, 0).flags & TF_FLAG_HOLD_FALLBACK);
    calm(3600);
    set_mode(3599, 3600, TC_MODE_FALLBACK);
    tf_facts_t f = facts(3600, 0);
    CHECK_EQ_INT(f.fallback_now, 1);
    CHECK(f.flags & TF_FLAG_HOLD_FALLBACK);
    /* Even in a window too short for a rate. */
    calm(60);
    set_mode(59, 60, TC_MODE_FALLBACK);
    CHECK(facts(60, 0).flags & TF_FLAG_HOLD_FALLBACK);
}

TEST(test_high_need_at_its_threshold) {
    calm(3600);
    set_fio2(0, 700, 600); /* 19.4% */
    CHECK(!(facts(3600, 0).flags & TF_FLAG_HIGH_NEED));
    set_fio2(700, 720, 600); /* 20% */
    CHECK(facts(3600, 0).flags & TF_FLAG_HIGH_NEED);
}

TEST(test_a_restart_raises_the_system_flag) {
    calm(3600);
    tf_facts_t f = facts(3600, 1);
    CHECK(f.flags & TF_FLAG_SYSTEM);
    CHECK_EQ_INT(f.restarts, 1);
}

TEST(test_seconds_without_a_controller_are_not_counted_as_fio2) {
    calm(3600);
    set_fio2(0, 100, -1);
    tf_facts_t f = facts(3600, 0);
    CHECK_EQ_INT(f.fio2_early_pm, 300);
    CHECK_EQ_INT(f.fio2_max_pm, 300);
}

TEST(test_the_order_puts_harm_before_nuisance) {
    calm(3600);
    tf_facts_t healthy = facts(3600, 0);
    for (int k = 0; k < 8; k++) lose_signal(100 + 200 * k, 110 + 200 * k);
    tf_facts_t probe = facts(3600, 0);
    calm(3600);
    set_spo2(100, 120, 70);
    set_spo2(300, 320, 70);
    tf_facts_t desats = facts(3600, 0);
    CHECK(probe.priority > healthy.priority);
    CHECK(desats.priority > probe.priority);
}

TEST(test_the_ring_wraps_and_stays_inside) {
    calm(CAP);
    for (int i = 0; i < 20; i++) ring[(CAP - 10 + i) % CAP].spo2 = 70;
    tf_facts_t f;
    const tf_thresholds_t th = tf_thresholds_default();
    tf_compute(ring, CAP, CAP - 10, 3600, 1, 0, &th, &f);
    CHECK_EQ_INT(f.desat_episodes, 1);
    CHECK_EQ_INT(f.longest_desat_s, 20);
    /* More seconds than the ring holds are clamped. */
    tf_compute(ring, CAP, 0, CAP + 100, 1, 0, &th, &f);
    CHECK_EQ_INT(f.seconds, CAP);
}

TEST(test_no_data_gives_empty_facts_and_no_flags) {
    tf_facts_t f;
    const tf_thresholds_t th = tf_thresholds_default();
    tf_compute(ring, CAP, 0, 0, 3, 0, &th, &f);
    CHECK_EQ_INT(f.seconds, 0);
    CHECK_EQ_INT(f.flags, 0);
    CHECK_EQ_INT(f.fio2_early_pm, -1);
    tf_compute(NULL, CAP, 0, 10, 3, 0, &th, &f);
    CHECK_EQ_INT(f.seconds, 0);
    tf_compute(ring, CAP, -5, 10, 3, 0, &th, &f);
    CHECK_EQ_INT(f.seconds, 10);
}

TEST(test_flag_names_and_json) {
    CHECK(strcmp(tf_flag_name(TF_FLAG_PROBE), "probe_trouble") == 0);
    CHECK(tf_flag_name(0x1000u) == NULL);
    calm(3600);
    tf_facts_t f = facts(3600, 0);
    char buf[512];
    CHECK(tf_json(&f, buf, sizeof buf) > 0);
    CHECK(strstr(buf, "\"tir\":100") != NULL);
    CHECK_EQ_INT(tf_json(&f, buf, 8), -1);
    CHECK_EQ_INT(tf_second_size(), sizeof(tf_second_t));
    CHECK_EQ_INT(tf_facts_size(), sizeof(tf_facts_t));
}
