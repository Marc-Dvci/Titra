/* Sliding-window median baseline. */
#include "check.h"
#include "titra/baseline.h"

TEST(test_baseline_empty_has_no_median) {
    tc_baseline_t b;
    tc_baseline_init(&b, 300.0);
    CHECK_EQ_INT(tc_baseline_median(&b), -1);
    CHECK_EQ_INT(tc_baseline_count(&b), 0);
}

TEST(test_baseline_lower_median_odd_and_even) {
    tc_baseline_t b;
    tc_baseline_init(&b, 300.0);
    tc_baseline_add(&b, 1, 300);
    tc_baseline_add(&b, 2, 500);
    tc_baseline_add(&b, 3, 400);
    CHECK_EQ_INT(tc_baseline_median(&b), 400);
    tc_baseline_add(&b, 4, 600);
    CHECK_EQ_INT(tc_baseline_median(&b), 400);
}

TEST(test_baseline_expires_entries_outside_the_window) {
    tc_baseline_t b;
    tc_baseline_init(&b, 300.0);
    for (int t = 0; t < 100; t++) tc_baseline_add(&b, t, 800);
    for (int t = 100; t < 400; t++) tc_baseline_add(&b, t, 300);
    /* At t = 399 the window starts at 99: one entry of 800 is left against 300 of 300. */
    CHECK_EQ_INT(tc_baseline_count(&b), 301);
    CHECK_EQ_INT(tc_baseline_median(&b), 300);
    tc_baseline_expire(&b, 1000.0);
    CHECK_EQ_INT(tc_baseline_count(&b), 0);
}

TEST(test_baseline_drop_since_removes_recent_entries_only) {
    tc_baseline_t b;
    tc_baseline_init(&b, 300.0);
    for (int t = 0; t < 10; t++) tc_baseline_add(&b, t, 300);
    for (int t = 10; t < 30; t++) tc_baseline_add(&b, t, 900);
    CHECK_EQ_INT(tc_baseline_median(&b), 900);
    tc_baseline_drop_since(&b, 10.0);
    CHECK_EQ_INT(tc_baseline_count(&b), 10);
    CHECK_EQ_INT(tc_baseline_median(&b), 300);
    tc_baseline_drop_since(&b, -1.0);
    CHECK_EQ_INT(tc_baseline_count(&b), 0);
    tc_baseline_drop_since(&b, 0.0);
    CHECK_EQ_INT(tc_baseline_count(&b), 0);
}

TEST(test_baseline_full_buffer_drops_the_oldest) {
    tc_baseline_t b;
    tc_baseline_init(&b, 1e9);
    for (int i = 0; i < TC_BASELINE_CAP; i++) tc_baseline_add(&b, i, 210);
    for (int i = 0; i < TC_BASELINE_CAP / 2 + 1; i++) tc_baseline_add(&b, TC_BASELINE_CAP + i, 1000);
    CHECK_EQ_INT(tc_baseline_count(&b), TC_BASELINE_CAP);
    CHECK_EQ_INT(tc_baseline_median(&b), 1000);
}

TEST(test_baseline_clamps_values_to_per_mille_range) {
    tc_baseline_t b;
    tc_baseline_init(&b, 300.0);
    tc_baseline_add(&b, 1, -5);
    CHECK_EQ_INT(tc_baseline_median(&b), 0);
    tc_baseline_add(&b, 2, 5000);
    tc_baseline_add(&b, 3, 5000);
    CHECK_EQ_INT(tc_baseline_median(&b), 1000);
}
