/* One cot's chain end to end: oximeter bytes in, blender frames out. */
#include <stdlib.h>
#include <string.h>

#include "check.h"
#include "titra/control.h"
#include "titra/frame.h"
#include "titra/sim.h"

static void *new_cot(double fio2) {
    void *m = calloc(1, tc_sim_size());
    tc_sim_init(m, 0.21, 1.00, 0.30, fio2, 0.0);
    return m;
}

static void send_ox(void *m, uint32_t clock, int spo2, unsigned flags, double rx) {
    char buf[TC_LINE_MAX + 1];
    tc_ox_frame_t f = {clock, spo2, 150 + (int)(clock % 7), 140 + (int)(clock % 5), flags};
    size_t n = tc_ox_format(buf, sizeof buf, &f);
    tc_sim_feed(m, buf, n, rx);
}

TEST(test_sim_sizes_are_positive) {
    CHECK(tc_sim_size() > 0);
    CHECK_EQ_INT(tc_sim_tick_size(), sizeof(tc_sim_tick_t));
}

TEST(test_sim_frames_flow_to_a_parseable_blender_frame) {
    void *m = new_cot(0.30);
    char bl[TC_LINE_MAX + 1];
    tc_sim_tick_t o;
    double t = 0;
    for (uint32_t i = 1; i <= 120; i++) {
        t += 1.0;
        send_ox(m, i, 80, 0, t - 0.3);
        size_t n = tc_sim_tick(m, t, bl, sizeof bl, &o);
        CHECK(n > 0);
        tc_bl_frame_t f;
        CHECK_EQ_INT(tc_bl_parse(bl, n - 2, &f), TC_FRAME_OK);
        CHECK_EQ_INT(f.fio2_pm, (int)(o.fio2 * 1000 + 0.5));
        CHECK_EQ_INT(f.backup_pm, 300);
        CHECK_EQ_INT(f.seq, (unsigned)(i - 1));
        CHECK_EQ_INT(o.refused, 0);
    }
    CHECK(o.fio2 > 0.40);
    CHECK_EQ_INT(o.mode, TC_MODE_CONTROL);
    CHECK_EQ_INT(tc_sim_frames_ok(m), 120);
    free(m);
}

TEST(test_sim_rejects_corrupt_frames_and_keeps_control) {
    void *m = new_cot(0.30);
    char bl[TC_LINE_MAX + 1];
    tc_sim_tick_t o;
    double t = 0;
    for (uint32_t i = 1; i <= 90; i++) {
        t += 1.0;
        tc_sim_feed(m, "\x07garbage#", 9, t - 0.5);
        if (i % 3 == 0) {
            tc_sim_feed(m, "$OX,1,93,150,1.40,00*00\r\n", 25, t - 0.3);
        } else {
            send_ox(m, i, 93, 0, t - 0.3);
        }
        tc_sim_tick(m, t, bl, sizeof bl, &o);
        CHECK(o.mode != TC_MODE_FALLBACK);
    }
    CHECK_EQ_INT(tc_sim_frames_rejected(m), 30);
    CHECK_EQ_INT(tc_sim_frames_ok(m), 60);
    CHECK(tc_sim_bytes_dropped(m) >= 90 * 9);
    free(m);
}

TEST(test_sim_sensor_off_holds_then_falls_back) {
    void *m = new_cot(0.40);
    char bl[TC_LINE_MAX + 1];
    tc_sim_tick_t o;
    double t = 0;
    uint32_t i = 1;
    for (; i <= 400; i++) {
        t += 1.0;
        send_ox(m, i, 93, 0, t - 0.3);
        tc_sim_tick(m, t, bl, sizeof bl, &o);
    }
    double held = o.fio2;
    double fault_t = t;
    double fallback_at = -1;
    for (; i <= 460; i++) {
        t += 1.0;
        send_ox(m, i, TC_ABSENT, TC_OX_SENSOR_OFF, t - 0.3);
        tc_sim_tick(m, t, bl, sizeof bl, &o);
        if (o.mode == TC_MODE_HOLD) CHECK_NEAR(o.fio2, held, 1e-12);
        if (o.mode == TC_MODE_FALLBACK && fallback_at < 0) fallback_at = t;
        CHECK(o.alarm >= 2);
    }
    CHECK(fallback_at > 0);
    CHECK(fallback_at - fault_t <= 32.0);
    CHECK_NEAR(o.fio2, 0.40, 1e-9);
    free(m);
}

TEST(test_sim_param_restarts_the_controller_with_the_new_value) {
    void *m = new_cot(0.30);
    CHECK_EQ_INT(tc_sim_param(m, "rate_up", 0.0), 0);
    CHECK_EQ_INT(tc_sim_param(m, "nonsense", 1.0), -1);
    char bl[TC_LINE_MAX + 1];
    tc_sim_tick_t o;
    for (uint32_t i = 1; i <= 60; i++) {
        send_ox(m, i, 80, 0, i - 0.3);
        tc_sim_tick(m, i, bl, sizeof bl, &o);
    }
    CHECK_NEAR(o.fio2, 0.30, 1e-9);
    free(m);
}

TEST(test_sim_tick_without_any_frame_holds_then_falls_back) {
    void *m = new_cot(0.25);
    char bl[TC_LINE_MAX + 1];
    tc_sim_tick_t o;
    tc_sim_tick(m, 1.0, bl, sizeof bl, &o);
    CHECK_EQ_INT(o.mode, TC_MODE_HOLD);
    CHECK_EQ_INT(o.sample_status, -1);
    CHECK_EQ_INT(o.spo2, -1);
    tc_sim_tick(m, 31.0, bl, sizeof bl, &o);
    CHECK_EQ_INT(o.mode, TC_MODE_FALLBACK);
    CHECK_NEAR(o.fio2, 0.30, 1e-9);
    free(m);
}
