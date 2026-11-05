/* Line framing and the $OX, $BL and $BS codecs. */
#include <string.h>

#include "check.h"
#include "titra/frame.h"

static size_t feed_all(tc_line_t *lb, const char *s, char out[][TC_LINE_MAX + 1], size_t max) {
    size_t k = 0;
    for (size_t i = 0; s[i]; i++) {
        const char *line;
        size_t len;
        if (tc_line_feed(lb, s[i], &line, &len) && k < max) {
            memcpy(out[k], line, len + 1);
            k++;
        }
    }
    return k;
}

TEST(test_checksum_matches_protocol_examples) {
    CHECK_EQ_INT(tc_checksum("OX,7261,93,156,1.48,00", 22), 0x12);
    CHECK_EQ_INT(tc_checksum("BL,412,305,300,0", 16), 0x0C);
    CHECK_EQ_INT(tc_checksum("BS,412,301,N", 12), 0x76);
}

TEST(test_ox_parses_protocol_example) {
    const char *l = "$OX,7261,93,156,1.48,00*12";
    tc_ox_frame_t f;
    CHECK_EQ_INT(tc_ox_parse(l, strlen(l), &f), TC_FRAME_OK);
    CHECK_EQ_INT(f.clock, 7261);
    CHECK_EQ_INT(f.spo2, 93);
    CHECK_EQ_INT(f.pr, 156);
    CHECK_EQ_INT(f.pi_centi, 148);
    CHECK_EQ_INT(f.flags, 0);
}

TEST(test_ox_absent_values_parse_as_absent) {
    char buf[TC_LINE_MAX + 1];
    tc_ox_frame_t in = {12, TC_ABSENT, TC_ABSENT, TC_ABSENT, TC_OX_SENSOR_OFF};
    size_t n = tc_ox_format(buf, sizeof buf, &in);
    CHECK(n > 0);
    CHECK(strstr(buf, ",--,--,--,01*") != NULL);
    tc_ox_frame_t out;
    CHECK_EQ_INT(tc_ox_parse(buf, n - 2, &out), TC_FRAME_OK);
    CHECK_EQ_INT(out.spo2, TC_ABSENT);
    CHECK_EQ_INT(out.pr, TC_ABSENT);
    CHECK_EQ_INT(out.pi_centi, TC_ABSENT);
    CHECK_EQ_INT(out.flags, TC_OX_SENSOR_OFF);
}

TEST(test_ox_round_trip_across_ranges) {
    char buf[TC_LINE_MAX + 1];
    const int spo2s[] = {0, 1, 50, 91, 100};
    const int prs[] = {0, 30, 155, 400};
    const int pis[] = {0, 5, 148, 2000};
    for (size_t a = 0; a < 5; a++) {
        for (size_t b = 0; b < 4; b++) {
            for (size_t c = 0; c < 4; c++) {
                tc_ox_frame_t in = {4294967295u, spo2s[a], prs[b], pis[c], 0x1F};
                size_t n = tc_ox_format(buf, sizeof buf, &in);
                CHECK(n > 0 && n <= TC_LINE_MAX);
                CHECK(buf[n - 2] == '\r' && buf[n - 1] == '\n');
                tc_ox_frame_t out;
                CHECK_EQ_INT(tc_ox_parse(buf, n - 2, &out), TC_FRAME_OK);
                CHECK(memcmp(&in, &out, sizeof in) == 0);
            }
        }
    }
}

TEST(test_ox_format_refuses_out_of_range) {
    char buf[TC_LINE_MAX + 1];
    tc_ox_frame_t f = {1, 101, 150, 100, 0};
    CHECK_EQ_INT(tc_ox_format(buf, sizeof buf, &f), 0);
    f.spo2 = 95;
    f.pr = 401;
    CHECK_EQ_INT(tc_ox_format(buf, sizeof buf, &f), 0);
    f.pr = 150;
    f.pi_centi = 2001;
    CHECK_EQ_INT(tc_ox_format(buf, sizeof buf, &f), 0);
    f.pi_centi = 100;
    f.flags = 0x20;
    CHECK_EQ_INT(tc_ox_format(buf, sizeof buf, &f), 0);
    f.flags = 0;
    CHECK_EQ_INT(tc_ox_format(buf, 10, &f), 0);
}

static tc_frame_result_t ox(const char *l) {
    tc_ox_frame_t f;
    return tc_ox_parse(l, strlen(l), &f);
}

TEST(test_ox_rejects_each_malformation) {
    CHECK_EQ_INT(ox("OX,7261,93,156,1.48,00*12"), TC_FRAME_NO_START);
    CHECK_EQ_INT(ox(""), TC_FRAME_NO_START);
    CHECK_EQ_INT(ox("$OX,7261,93,156,1.48,00"), TC_FRAME_NO_CHECKSUM);
    CHECK_EQ_INT(ox("$OX,7261,93,156,1.48,00*1"), TC_FRAME_NO_CHECKSUM);
    CHECK_EQ_INT(ox("$OX,7261,93,156,1.48,00*1g"), TC_FRAME_NO_CHECKSUM);
    CHECK_EQ_INT(ox("$OX,7261,93,156,1.48,00*13"), TC_FRAME_CHECKSUM);
    CHECK_EQ_INT(ox("$*00"), TC_FRAME_EMPTY);
    CHECK_EQ_INT(ox("$O*X*00"), TC_FRAME_NO_CHECKSUM);
}

/* Builds "$<body>*<cs>" with a correct checksum, so that only the body is under test. */
static tc_frame_result_t ox_body(const char *body) {
    char line[128];
    size_t n = strlen(body);
    snprintf(line, sizeof line, "$%s*%02X", body, tc_checksum(body, n));
    return ox(line);
}

TEST(test_ox_rejects_bad_fields_with_good_checksum) {
    CHECK_EQ_INT(ox_body("OX,7261,93,156,1.48,00"), TC_FRAME_OK);
    CHECK_EQ_INT(ox_body("BL,7261,93,156,1.48,00"), TC_FRAME_TYPE);
    CHECK_EQ_INT(ox_body("OX,7261,93,156,1.48"), TC_FRAME_FIELDS);
    CHECK_EQ_INT(ox_body("OX,7261,93,156,1.48,00,1"), TC_FRAME_FIELDS);
    CHECK_EQ_INT(ox_body("OX,1,2,3,4,5,6,7,8"), TC_FRAME_FIELDS);
    CHECK_EQ_INT(ox_body("OX,-1,93,156,1.48,00"), TC_FRAME_VALUE);
    CHECK_EQ_INT(ox_body("OX,4294967296,93,156,1.48,00"), TC_FRAME_VALUE);
    CHECK_EQ_INT(ox_body("OX,7261,101,156,1.48,00"), TC_FRAME_VALUE);
    CHECK_EQ_INT(ox_body("OX,7261,9a,156,1.48,00"), TC_FRAME_VALUE);
    CHECK_EQ_INT(ox_body("OX,7261,,156,1.48,00"), TC_FRAME_VALUE);
    CHECK_EQ_INT(ox_body("OX,7261,93,401,1.48,00"), TC_FRAME_VALUE);
    CHECK_EQ_INT(ox_body("OX,7261,93,156,1.4,00"), TC_FRAME_VALUE);
    CHECK_EQ_INT(ox_body("OX,7261,93,156,148,00"), TC_FRAME_VALUE);
    CHECK_EQ_INT(ox_body("OX,7261,93,156,20.01,00"), TC_FRAME_VALUE);
    CHECK_EQ_INT(ox_body("OX,7261,93,156,1.48,20"), TC_FRAME_VALUE);
    CHECK_EQ_INT(ox_body("OX,7261,93,156,1.48,0"), TC_FRAME_VALUE);
    CHECK_EQ_INT(ox_body("OX,7261,93,156,1.48,0x"), TC_FRAME_VALUE);
    CHECK_EQ_INT(ox_body("OX,7261,93,156,1.48,1f"), TC_FRAME_VALUE);
}

TEST(test_ox_rejects_line_over_limit) {
    char line[200];
    memset(line, '1', sizeof line);
    line[0] = '$';
    CHECK_EQ_INT(tc_ox_parse(line, 79, NULL), TC_FRAME_TOO_LONG);
}

TEST(test_bl_round_trip_and_limits) {
    char buf[TC_LINE_MAX + 1];
    tc_bl_frame_t in = {65535u, 1000, 210, 3};
    size_t n = tc_bl_format(buf, sizeof buf, &in);
    CHECK(n > 0);
    tc_bl_frame_t out;
    CHECK_EQ_INT(tc_bl_parse(buf, n - 2, &out), TC_FRAME_OK);
    CHECK(memcmp(&in, &out, sizeof in) == 0);
    const char *ex = "$BL,412,305,300,0*0C";
    CHECK_EQ_INT(tc_bl_parse(ex, strlen(ex), &out), TC_FRAME_OK);
    CHECK_EQ_INT(out.fio2_pm, 305);
    tc_bl_frame_t bad[] = {{65536u, 300, 300, 0}, {1, 209, 300, 0}, {1, 1001, 300, 0},
                           {1, 300, 209, 0},      {1, 300, 300, 4}, {1, 300, 300, -1}};
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) CHECK_EQ_INT(tc_bl_format(buf, sizeof buf, &bad[i]), 0);
}

static tc_frame_result_t bl_body(const char *body) {
    char line[128];
    snprintf(line, sizeof line, "$%s*%02X", body, tc_checksum(body, strlen(body)));
    tc_bl_frame_t f;
    return tc_bl_parse(line, strlen(line), &f);
}

TEST(test_bl_rejects_bad_fields) {
    CHECK_EQ_INT(bl_body("BL,1,300,300,0"), TC_FRAME_OK);
    CHECK_EQ_INT(bl_body("BS,1,300,300,0"), TC_FRAME_TYPE);
    CHECK_EQ_INT(bl_body("BL,1,300,300"), TC_FRAME_FIELDS);
    CHECK_EQ_INT(bl_body("BL,65536,300,300,0"), TC_FRAME_VALUE);
    CHECK_EQ_INT(bl_body("BL,1,209,300,0"), TC_FRAME_VALUE);
    CHECK_EQ_INT(bl_body("BL,1,1001,300,0"), TC_FRAME_VALUE);
    CHECK_EQ_INT(bl_body("BL,1,300,209,0"), TC_FRAME_VALUE);
    CHECK_EQ_INT(bl_body("BL,1,300,300,4"), TC_FRAME_VALUE);
    CHECK_EQ_INT(bl_body("BL,1,--,300,0"), TC_FRAME_VALUE);
}

TEST(test_bs_round_trip_and_states) {
    char buf[TC_LINE_MAX + 1];
    const char states[] = {'N', 'H', 'F'};
    for (size_t i = 0; i < 3; i++) {
        tc_bs_frame_t in = {7, 301, states[i]};
        size_t n = tc_bs_format(buf, sizeof buf, &in);
        CHECK(n > 0);
        tc_bs_frame_t out;
        CHECK_EQ_INT(tc_bs_parse(buf, n - 2, &out), TC_FRAME_OK);
        CHECK_EQ_INT(out.seq, in.seq);
        CHECK_EQ_INT(out.fio2_pm, in.fio2_pm);
        CHECK_EQ_INT(out.state, in.state);
    }
    tc_bs_frame_t bad = {7, 301, 'X'};
    CHECK_EQ_INT(tc_bs_format(buf, sizeof buf, &bad), 0);
    const char *ex = "$BS,412,301,N*76";
    tc_bs_frame_t out;
    CHECK_EQ_INT(tc_bs_parse(ex, strlen(ex), &out), TC_FRAME_OK);
    const char *body = "BS,412,301,NN";
    char line[64];
    snprintf(line, sizeof line, "$%s*%02X", body, tc_checksum(body, strlen(body)));
    CHECK_EQ_INT(tc_bs_parse(line, strlen(line), &out), TC_FRAME_VALUE);
    body = "BS,412,1001,N";
    snprintf(line, sizeof line, "$%s*%02X", body, tc_checksum(body, strlen(body)));
    CHECK_EQ_INT(tc_bs_parse(line, strlen(line), &out), TC_FRAME_VALUE);
    body = "OX,412,301,N";
    snprintf(line, sizeof line, "$%s*%02X", body, tc_checksum(body, strlen(body)));
    CHECK_EQ_INT(tc_bs_parse(line, strlen(line), &out), TC_FRAME_TYPE);
}

TEST(test_line_splits_frames_and_drops_noise) {
    tc_line_t lb;
    tc_line_init(&lb);
    char out[4][TC_LINE_MAX + 1];
    size_t k = feed_all(&lb, "xx\x01$OX,1*00\r\nnoise$BL,2*00\n", out, 4);
    CHECK_EQ_INT(k, 2);
    CHECK(strcmp(out[0], "$OX,1*00") == 0);
    CHECK(strcmp(out[1], "$BL,2*00") == 0);
    CHECK_EQ_INT(lb.dropped_bytes, 8);
}

TEST(test_line_restarts_on_dollar_inside_a_line) {
    tc_line_t lb;
    tc_line_init(&lb);
    char out[2][TC_LINE_MAX + 1];
    size_t k = feed_all(&lb, "$OX,12$BS,1*00\r\n", out, 2);
    CHECK_EQ_INT(k, 1);
    CHECK(strcmp(out[0], "$BS,1*00") == 0);
    CHECK_EQ_INT(lb.dropped_bytes, 6);
}

TEST(test_line_drops_overlong_line_then_recovers) {
    tc_line_t lb;
    tc_line_init(&lb);
    char s[300];
    memset(s, 'A', sizeof s);
    s[0] = '$';
    memcpy(s + 200, "\r\n$OK*00\r\n", 11);
    char out[2][TC_LINE_MAX + 1];
    size_t k = feed_all(&lb, s, out, 2);
    CHECK_EQ_INT(k, 1);
    CHECK(strcmp(out[0], "$OK*00") == 0);
    CHECK_EQ_INT(lb.dropped_lines, 1);
}

TEST(test_line_length_limit_counts_the_terminator) {
    /* 78 bytes of content plus "\r\n" is exactly TC_LINE_MAX and passes; 79 does not. */
    char s[100];
    tc_line_t lb;
    char out[1][TC_LINE_MAX + 1];
    memset(s, 'A', 78);
    s[0] = '$';
    memcpy(s + 78, "\r\n", 3);
    tc_line_init(&lb);
    CHECK_EQ_INT(feed_all(&lb, s, out, 1), 1);
    memset(s, 'A', 79);
    s[0] = '$';
    memcpy(s + 79, "\r\n", 3);
    tc_line_init(&lb);
    CHECK_EQ_INT(feed_all(&lb, s, out, 1), 0);
    CHECK_EQ_INT(lb.dropped_lines, 1);
}

TEST(test_result_names_are_distinct) {
    for (int a = TC_FRAME_OK; a <= TC_FRAME_TOO_LONG; a++) {
        for (int b = a + 1; b <= TC_FRAME_TOO_LONG; b++) {
            CHECK(strcmp(tc_frame_result_name((tc_frame_result_t)a), tc_frame_result_name((tc_frame_result_t)b)) != 0);
        }
    }
}
