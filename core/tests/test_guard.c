/* The blender driver's command guard and link watchdog. */
#include <string.h>

#include "check.h"
#include "titra/control.h"
#include "titra/guard.h"

static const tc_guard_cfg_t G = {210, 600, 300, 30};

TEST(test_guard_accepts_in_range_commands) {
    CHECK_EQ_INT(tc_guard_check(&G, 1, 400, 420, TC_MODE_CONTROL), TC_GUARD_OK);
    CHECK_EQ_INT(tc_guard_check(&G, 1, 400, 430, TC_MODE_CONTROL), TC_GUARD_OK);
    CHECK_EQ_INT(tc_guard_check(&G, 1, 400, 370, TC_MODE_HOLD), TC_GUARD_OK);
    CHECK_EQ_INT(tc_guard_check(&G, 0, 0, 600, TC_MODE_CONTROL), TC_GUARD_OK);
}

TEST(test_guard_refuses_outside_the_limits) {
    CHECK_EQ_INT(tc_guard_check(&G, 1, 220, 209, TC_MODE_CONTROL), TC_GUARD_BELOW_MIN);
    CHECK_EQ_INT(tc_guard_check(&G, 1, 590, 601, TC_MODE_CONTROL), TC_GUARD_ABOVE_MAX);
    CHECK_EQ_INT(tc_guard_check(&G, 1, 590, 601, TC_MODE_FALLBACK), TC_GUARD_ABOVE_MAX);
}

TEST(test_guard_refuses_a_step_above_the_slew_limit) {
    CHECK_EQ_INT(tc_guard_check(&G, 1, 400, 431, TC_MODE_CONTROL), TC_GUARD_SLEW);
    CHECK_EQ_INT(tc_guard_check(&G, 1, 400, 369, TC_MODE_HOLD), TC_GUARD_SLEW);
}

TEST(test_guard_fallback_may_step_but_never_below_backup) {
    CHECK_EQ_INT(tc_guard_check(&G, 1, 600, 300, TC_MODE_FALLBACK), TC_GUARD_OK);
    CHECK_EQ_INT(tc_guard_check(&G, 1, 210, 450, TC_MODE_FALLBACK), TC_GUARD_OK);
    CHECK_EQ_INT(tc_guard_check(&G, 1, 300, 299, TC_MODE_FALLBACK), TC_GUARD_FALLBACK_LOW);
}

TEST(test_guard_refuses_an_unknown_mode) {
    CHECK_EQ_INT(tc_guard_check(&G, 1, 300, 300, 7), TC_GUARD_MODE);
    CHECK_EQ_INT(tc_guard_check(&G, 1, 300, 300, -1), TC_GUARD_MODE);
}

TEST(test_guard_result_names) {
    CHECK(strcmp(tc_guard_result_name(TC_GUARD_OK), "ok") == 0);
    CHECK(strcmp(tc_guard_result_name(TC_GUARD_SLEW), "slew") == 0);
    CHECK(strcmp(tc_guard_result_name(TC_GUARD_FALLBACK_LOW), "fallback_below_backup") == 0);
    CHECK(strcmp(tc_guard_result_name((tc_guard_result_t)99), "unknown") == 0);
}

TEST(test_linkwd_normal_while_commands_arrive) {
    tc_linkwd_t w;
    tc_linkwd_init(&w, 3.0, 15.0, 300.0, 300, 400, 0.0);
    int out = 0;
    for (int t = 1; t <= 100; t++) {
        tc_linkwd_command(&w, t, 400 + (t % 3));
        CHECK_EQ_INT(tc_linkwd_tick(&w, t + 0.5, &out), TC_LINK_NORMAL);
        CHECK_EQ_INT(out, 400 + (t % 3));
    }
}

TEST(test_linkwd_holds_at_3_s_and_falls_back_at_15_s) {
    tc_linkwd_t w;
    tc_linkwd_init(&w, 3.0, 15.0, 300.0, 300, 400, 0.0);
    int out = 0;
    for (int t = 1; t <= 60; t++) tc_linkwd_command(&w, t, 450);
    CHECK_EQ_INT(tc_linkwd_tick(&w, 62.9, &out), TC_LINK_NORMAL);
    CHECK_EQ_INT(tc_linkwd_tick(&w, 63.0, &out), TC_LINK_HOLD);
    CHECK_EQ_INT(out, 450);
    CHECK_EQ_INT(tc_linkwd_tick(&w, 74.9, &out), TC_LINK_HOLD);
    CHECK_EQ_INT(tc_linkwd_tick(&w, 75.0, &out), TC_LINK_FALLBACK);
    /* The baseline (450) is above the backup (300). */
    CHECK_EQ_INT(out, 450);
}

TEST(test_linkwd_fallback_uses_backup_when_baseline_is_lower) {
    tc_linkwd_t w;
    tc_linkwd_init(&w, 3.0, 15.0, 300.0, 300, 250, 0.0);
    int out = 0;
    for (int t = 1; t <= 60; t++) tc_linkwd_command(&w, t, 250);
    CHECK_EQ_INT(tc_linkwd_tick(&w, 80.0, &out), TC_LINK_FALLBACK);
    CHECK_EQ_INT(out, 300);
}

TEST(test_linkwd_fallback_without_any_command_uses_backup) {
    tc_linkwd_t w;
    tc_linkwd_init(&w, 3.0, 15.0, 300.0, 300, 250, 0.0);
    int out = 0;
    CHECK_EQ_INT(tc_linkwd_tick(&w, 20.0, &out), TC_LINK_FALLBACK);
    CHECK_EQ_INT(out, 300);
}

TEST(test_linkwd_new_command_returns_to_normal) {
    tc_linkwd_t w;
    tc_linkwd_init(&w, 3.0, 15.0, 300.0, 300, 400, 0.0);
    int out = 0;
    CHECK_EQ_INT(tc_linkwd_tick(&w, 20.0, &out), TC_LINK_FALLBACK);
    tc_linkwd_command(&w, 21.0, 380);
    CHECK_EQ_INT(tc_linkwd_tick(&w, 21.5, &out), TC_LINK_NORMAL);
    CHECK_EQ_INT(out, 380);
}
