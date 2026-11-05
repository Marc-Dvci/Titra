/* One cot's on-board chain for simulation runs. See sim.h. */
#include "titra/sim.h"

#include <math.h>
#include <string.h>

#include "titra/control.h"
#include "titra/frame.h"
#include "titra/guard.h"
#include "titra/quality.h"

#define LINK_HOLD_S 3.0
#define LINK_FALLBACK_S 15.0
#define SLEW_PM 50

typedef struct {
    tc_cot_settings_t settings;
    tc_params_t params;
    double start_s;
    double initial_fio2;
    /* spo2d */
    tc_line_t line;
    tc_quality_t quality;
    tc_sample_t sample;
    int have_sample;
    uint32_t frames_ok, frames_rejected;
    /* titractl */
    tc_controller_t ctl;
    /* valved */
    tc_guard_cfg_t guard;
    tc_linkwd_t link;
    int have_cmd;
    int last_cmd_pm;
    unsigned seq;
} tc_sim_t;

size_t tc_sim_size(void) { return sizeof(tc_sim_t); }
size_t tc_sim_tick_size(void) { return sizeof(tc_sim_tick_t); }

static int pm(double fio2) { return (int)floor(fio2 * 1000.0 + 0.5); }

void tc_sim_init(void *mem, double fio2_min, double fio2_max, double backup_fio2, double initial_fio2, double now_s) {
    tc_sim_t *s = (tc_sim_t *)mem;
    memset(s, 0, sizeof(*s));
    s->settings.fio2_min = fio2_min;
    s->settings.fio2_max = fio2_max;
    s->settings.backup_fio2 = backup_fio2;
    s->params = tc_params_default();
    s->start_s = now_s;
    s->initial_fio2 = initial_fio2;
    tc_line_init(&s->line);
    tc_quality_cfg_t qc = tc_quality_cfg_default();
    tc_quality_init(&s->quality, &qc);
    tc_controller_init(&s->ctl, &s->params, &s->settings, initial_fio2, now_s);
    s->guard.min_pm = pm(fio2_min);
    s->guard.max_pm = pm(fio2_max);
    s->guard.backup_pm = pm(backup_fio2);
    s->guard.slew_pm = SLEW_PM;
    tc_linkwd_init(&s->link, LINK_HOLD_S, LINK_FALLBACK_S, s->params.baseline_window_s, pm(backup_fio2),
                   pm(initial_fio2), now_s);
}

int tc_sim_param(void *mem, const char *name, double value) {
    tc_sim_t *s = (tc_sim_t *)mem;
    if (tc_params_set(&s->params, name, value) != 0) return -1;
    tc_controller_init(&s->ctl, &s->params, &s->settings, s->initial_fio2, s->start_s);
    return 0;
}

int tc_sim_feed(void *mem, const char *bytes, size_t n, double rx_s) {
    tc_sim_t *s = (tc_sim_t *)mem;
    int ok = 0;
    for (size_t i = 0; i < n; i++) {
        const char *line;
        size_t len;
        if (!tc_line_feed(&s->line, bytes[i], &line, &len)) continue;
        tc_ox_frame_t f;
        if (tc_ox_parse(line, len, &f) != TC_FRAME_OK) {
            s->frames_rejected++;
            continue;
        }
        s->frames_ok++;
        s->sample = tc_quality_on_frame(&s->quality, &f, rx_s);
        s->have_sample = 1;
        ok++;
    }
    return ok;
}

size_t tc_sim_tick(void *mem, double now_s, char *bl, size_t cap, tc_sim_tick_t *out) {
    tc_sim_t *s = (tc_sim_t *)mem;
    tc_output_t o = tc_controller_step(&s->ctl, s->have_sample ? &s->sample : NULL, now_s);

    int req = pm(o.fio2);
    tc_guard_result_t g = tc_guard_check(&s->guard, s->have_cmd, s->last_cmd_pm, req, o.mode);
    if (g == TC_GUARD_OK) {
        s->have_cmd = 1;
        s->last_cmd_pm = req;
        tc_linkwd_command(&s->link, now_s, req);
    }
    int apply_pm;
    tc_linkwd_tick(&s->link, now_s, &apply_pm);

    tc_bl_frame_t f;
    f.seq = s->seq;
    f.fio2_pm = apply_pm;
    f.backup_pm = s->guard.backup_pm;
    f.alarm = o.alarm;
    s->seq = (s->seq + 1u) & 0xFFFFu;
    size_t n = tc_bl_format(bl, cap, &f);
    if (n > 0 && n < cap) bl[n] = '\0';

    if (out != NULL) {
        memset(out, 0, sizeof(*out));
        out->fio2 = apply_pm / 1000.0;
        out->ctl_fio2 = o.fio2;
        out->mode = o.mode;
        out->alarm = o.alarm;
        out->alarms = o.alarms;
        out->reasons = o.reasons;
        out->adjusted = o.adjusted;
        out->refused = g != TC_GUARD_OK;
        out->sample_status = s->have_sample ? s->sample.status : -1;
        out->spo2 = s->have_sample ? s->sample.spo2 : -1;
    }
    return n;
}

uint32_t tc_sim_frames_ok(const void *mem) { return ((const tc_sim_t *)mem)->frames_ok; }
uint32_t tc_sim_frames_rejected(const void *mem) { return ((const tc_sim_t *)mem)->frames_rejected; }
uint32_t tc_sim_bytes_dropped(const void *mem) { return ((const tc_sim_t *)mem)->line.dropped_bytes; }
