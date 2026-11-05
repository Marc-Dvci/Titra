/* The early-warning model's features and its bounded evaluation. */
#include <string.h>

#include "check.h"
#include "titra/control.h"
#include "titra/quality.h"
#include "titra/risk.h"

#define CAP 7200
static tf_second_t ring[CAP];

static void calm(void) {
    for (int i = 0; i < CAP; i++) {
        ring[i].spo2 = 93;
        ring[i].fio2_pm = 300;
        ring[i].mode = TC_MODE_CONTROL;
        ring[i].alarm = 0;
        ring[i].status = TC_SAMPLE_VALID;
        ring[i].reserved = 0;
    }
}

TEST(test_risk_features_of_a_calm_hour) {
    calm();
    double f[TR_N_FEATURES];
    tr_features(ring, CAP, 0, 3600, f);
    CHECK_NEAR(f[0], 93.0, 1e-9);
    CHECK_NEAR(f[1], 0.0, 1e-6);
    CHECK_NEAR(f[2], 93.0, 1e-9);
    CHECK_NEAR(f[3], 0.0, 1e-9);
    CHECK_NEAR(f[5], 0.0, 1e-9);
    CHECK_NEAR(f[6], 0.0, 1e-9);
    CHECK_NEAR(f[7], 0.30, 1e-9);
    CHECK_NEAR(f[8], 0.0, 1e-9);
    CHECK_NEAR(f[11], 0.0, 1e-9);
    CHECK_NEAR(f[12], 3600.0, 1e-9);
}

TEST(test_risk_counts_dips_in_the_right_window) {
    calm();
    for (int i = 100; i < 120; i++) ring[i].spo2 = 80;   /* an hour ago */
    for (int i = 3500; i < 3520; i++) ring[i].spo2 = 80; /* in the last 15 minutes */
    double f[TR_N_FEATURES];
    tr_features(ring, CAP, 0, 3600, f);
    CHECK_NEAR(f[5], 1.0, 1e-9);
    CHECK_NEAR(f[6], 2.0, 1e-9);
    CHECK_NEAR(f[2], 80.0, 1e-9);
    CHECK_NEAR(f[12], 80.0, 1e-9);
}

TEST(test_risk_a_dip_needs_ten_seconds) {
    calm();
    for (int i = 3500; i < 3509; i++) ring[i].spo2 = 80;
    double f[TR_N_FEATURES];
    tr_features(ring, CAP, 0, 3600, f);
    CHECK_NEAR(f[5], 0.0, 1e-9);
    CHECK_NEAR(f[12], 3600.0, 1e-9);
}

TEST(test_risk_reports_missing_readings) {
    calm();
    for (int i = 2700; i < 3600; i++) ring[i].status = TC_SAMPLE_INVALID;
    double f[TR_N_FEATURES];
    tr_features(ring, CAP, 0, 3600, f);
    CHECK_NEAR(f[11], 100.0, 1e-9);
    CHECK_NEAR(f[0], 0.0, 1e-9);
}

TEST(test_risk_handles_no_data_and_short_history) {
    double f[TR_N_FEATURES];
    tr_features(NULL, CAP, 0, 100, f);
    CHECK_NEAR(f[0], 0.0, 1e-9);
    CHECK_NEAR(f[12], 3600.0, 1e-9);
    calm();
    tr_features(ring, CAP, 0, 0, f);
    CHECK_NEAR(f[2], 100.0, 1e-9);
    tr_features(ring, CAP, 0, 60, f);
    CHECK_NEAR(f[0], 93.0, 1e-9);
    tr_features(ring, CAP, 0, CAP + 100, f); /* clamped to the ring */
    CHECK_NEAR(f[0], 93.0, 1e-9);
    tr_features(ring, CAP, -3, 60, f);
    CHECK_NEAR(f[0], 93.0, 1e-9);
}

TEST(test_risk_the_ring_wraps) {
    calm();
    for (int i = 0; i < 20; i++) ring[(CAP - 10 + i) % CAP].spo2 = 80;
    double f[TR_N_FEATURES];
    tr_features(ring, CAP, CAP - 3000, 3060, f);
    CHECK_NEAR(f[5], 1.0, 1e-9);
}

TEST(test_risk_the_score_is_a_probability_and_the_percent_rounds_it) {
    calm();
    const int p = tr_percent(ring, CAP, 0, 3600);
    CHECK(p >= 0 && p <= 100);
    double f[TR_N_FEATURES];
    tr_features(ring, CAP, 0, 3600, f);
    const double s = tr_score(f);
    CHECK(s >= 0.0 && s <= 1.0);
    CHECK_EQ_INT(p, (int)(s * 100.0 + 0.5));
}

TEST(test_risk_scores_stay_bounded_on_extreme_inputs) {
    const double hi[TR_N_FEATURES] = {100, 50, 100, 100, 100, 100, 100, 1, 1, 1, 100, 100, 3600};
    const double lo[TR_N_FEATURES] = {0};
    CHECK(tr_score(hi) >= 0.0 && tr_score(hi) <= 1.0);
    CHECK(tr_score(lo) >= 0.0 && tr_score(lo) <= 1.0);
}

TEST(test_risk_the_model_has_a_fixed_shape) {
    CHECK(tr_tree_count() >= 1);
    CHECK(tr_max_depth() <= 4);
}
