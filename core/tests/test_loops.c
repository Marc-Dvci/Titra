/* The loop-count choice and earliest-deadline-first, at their boundaries. */
#include "check.h"
#include "titra/loops.h"

static tl_speed_t measured(void) {
    tl_speed_t s;
    tl_speed_init(&s);
    tl_speed_gen(&s, 1, 76.0);
    tl_speed_gen(&s, 2, 115.0);
    tl_speed_gen(&s, 4, 181.0);
    tl_speed_prompt(&s, 1, 23.0);
    tl_speed_prompt(&s, 2, 37.0);
    tl_speed_prompt(&s, 4, 60.0);
    return s;
}

TEST(test_nothing_measured_means_the_minimum_loop_count) {
    tl_speed_t s;
    tl_speed_init(&s);
    CHECK_EQ_INT(tl_choose_loops(&s, 100000.0, 10, 10, 1, 4, 0.9, NULL), 1);
    CHECK(tl_estimate_ms(&s, 2, 10, 10) < 0.0);
}

TEST(test_plenty_of_time_gives_the_most_loops) {
    tl_speed_t s = measured();
    int fits = 0;
    CHECK_EQ_INT(tl_choose_loops(&s, 600000.0, 100, 100, 1, 4, 0.9, &fits), 4);
    CHECK_EQ_INT(fits, 1);
}

TEST(test_each_deadline_gets_the_largest_count_that_fits) {
    tl_speed_t s = measured();
    /* 50 tokens: 4 loops 9050 ms, 2 loops 5750 ms, 1 loop 3800 ms, with a margin of 1 for exact edges. */
    CHECK_EQ_INT(tl_choose_loops(&s, 9050.0, 0, 50, 1, 4, 1.0, NULL), 4);
    CHECK_EQ_INT(tl_choose_loops(&s, 9049.0, 0, 50, 1, 4, 1.0, NULL), 3);
    CHECK_EQ_INT(tl_choose_loops(&s, 5750.0, 0, 50, 1, 2, 1.0, NULL), 2);
    CHECK_EQ_INT(tl_choose_loops(&s, 5749.0, 0, 50, 1, 2, 1.0, NULL), 1);
    CHECK_EQ_INT(tl_choose_loops(&s, 3800.0, 0, 50, 1, 4, 1.0, NULL), 1);
}

TEST(test_an_impossible_deadline_reports_it_and_keeps_the_minimum) {
    tl_speed_t s = measured();
    int fits = 1;
    CHECK_EQ_INT(tl_choose_loops(&s, 100.0, 0, 50, 1, 4, 0.9, &fits), 1);
    CHECK_EQ_INT(fits, 0);
    CHECK_EQ_INT(tl_choose_loops(&s, 100.0, 0, 50, 2, 4, 0.9, &fits), 2);
    CHECK_EQ_INT(fits, 0);
}

TEST(test_the_margin_shrinks_the_budget) {
    tl_speed_t s = measured();
    CHECK_EQ_INT(tl_choose_loops(&s, 9050.0, 0, 50, 1, 4, 1.0, NULL), 4);
    CHECK(tl_choose_loops(&s, 9050.0, 0, 50, 1, 4, 0.9, NULL) < 4);
}

TEST(test_the_range_is_respected) {
    tl_speed_t s = measured();
    CHECK_EQ_INT(tl_choose_loops(&s, 600000.0, 0, 10, 1, 2, 0.9, NULL), 2);
    CHECK_EQ_INT(tl_choose_loops(&s, 600000.0, 0, 10, 1, 9, 0.9, NULL), 4);
    CHECK_EQ_INT(tl_choose_loops(&s, 600000.0, 0, 10, 0, 1, 0.9, NULL), 1);
    CHECK_EQ_INT(tl_choose_loops(&s, 600000.0, 0, 10, 3, 2, 0.9, NULL), 3);
}

TEST(test_prompt_and_generation_both_count) {
    tl_speed_t s = measured();
    CHECK_NEAR(tl_estimate_ms(&s, 1, 100, 0), 2300.0, 1e-6);
    CHECK_NEAR(tl_estimate_ms(&s, 1, 0, 100), 7600.0, 1e-6);
    CHECK_NEAR(tl_estimate_ms(&s, 1, 100, 100), 9900.0, 1e-6);
    CHECK_NEAR(tl_estimate_ms(&s, 1, 0, 0), 0.0, 1e-9);
    CHECK(tl_estimate_ms(&s, 0, 1, 1) < 0.0);
    CHECK(tl_estimate_ms(&s, 5, 1, 1) < 0.0);
}

TEST(test_a_count_not_measured_scales_with_the_blocks_per_token) {
    tl_speed_t s;
    tl_speed_init(&s);
    tl_speed_gen(&s, 1, 72.0); /* 32 fixed blocks + 40 per loop = 72 blocks, so 1 ms a block */
    CHECK_NEAR(tl_estimate_ms(&s, 2, 0, 1), 112.0, 1e-6);
    CHECK_NEAR(tl_estimate_ms(&s, 4, 0, 1), 192.0, 1e-6);
    /* The nearest measured count is the one used. */
    tl_speed_gen(&s, 4, 384.0);
    CHECK_NEAR(tl_estimate_ms(&s, 3, 0, 1), 384.0 * 152.0 / 192.0, 1e-6);
}

TEST(test_the_estimate_follows_a_slowdown) {
    tl_speed_t s = measured();
    const double before = tl_estimate_ms(&s, 4, 0, 1);
    for (int i = 0; i < 40; i++) tl_speed_gen(&s, 4, 500.0);
    const double after = tl_estimate_ms(&s, 4, 0, 1);
    CHECK(after > before);
    CHECK_NEAR(after, 500.0, 1.0);
}

TEST(test_bad_measurements_are_ignored) {
    tl_speed_t s = measured();
    const double before = tl_estimate_ms(&s, 1, 0, 1);
    tl_speed_gen(&s, 1, -5.0);
    tl_speed_gen(&s, 1, 0.0);
    tl_speed_gen(&s, 0, 5.0);
    tl_speed_gen(&s, 9, 5.0);
    tl_speed_prompt(&s, 9, 5.0);
    CHECK_NEAR(tl_estimate_ms(&s, 1, 0, 1), before, 1e-9);
}

TEST(test_edf_picks_the_earliest_runnable_deadline) {
    const double d[3] = {500.0, 100.0, 300.0};
    const int all[3] = {1, 1, 1};
    CHECK_EQ_INT(tl_pick_edf(d, all, 3), 1);
    const int two[3] = {1, 0, 1};
    CHECK_EQ_INT(tl_pick_edf(d, two, 3), 2);
    const int none[3] = {0, 0, 0};
    CHECK_EQ_INT(tl_pick_edf(d, none, 3), -1);
    CHECK_EQ_INT(tl_pick_edf(d, all, 0), -1);
}

TEST(test_edf_breaks_ties_by_the_lowest_index) {
    const double d[3] = {100.0, 100.0, 100.0};
    const int all[3] = {1, 1, 1};
    CHECK_EQ_INT(tl_pick_edf(d, all, 3), 0);
    const int last[3] = {0, 1, 1};
    CHECK_EQ_INT(tl_pick_edf(d, last, 3), 1);
}
