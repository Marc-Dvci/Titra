/*
 * The blender driver's own safety logic (docs/SPEC.md, "Safety layers"):
 *  - a guard that refuses commands outside the cot's limits, steps above the slew limit, and fallback commands
 *    below the backup FiO2;
 *  - a link watchdog that holds, then falls back, when the controller stops sending commands.
 * All FiO2 values are per mille.
 */
#ifndef TITRA_GUARD_H
#define TITRA_GUARD_H

#include "titra/baseline.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int min_pm;
    int max_pm;
    int backup_pm;
    int slew_pm; /* largest change one control or hold command may make */
} tc_guard_cfg_t;

typedef enum {
    TC_GUARD_OK = 0,
    TC_GUARD_BELOW_MIN,
    TC_GUARD_ABOVE_MAX,
    TC_GUARD_SLEW,
    TC_GUARD_FALLBACK_LOW,
    TC_GUARD_MODE
} tc_guard_result_t;

/* Checks a command. `mode` is the controller's tc_mode_t. `have_last` is 0 before the first accepted command. */
tc_guard_result_t tc_guard_check(const tc_guard_cfg_t *g, int have_last, int last_pm, int req_pm, int mode);

const char *tc_guard_result_name(tc_guard_result_t r);

typedef enum { TC_LINK_NORMAL = 'N', TC_LINK_HOLD = 'H', TC_LINK_FALLBACK = 'F' } tc_link_state_t;

typedef struct {
    double hold_after_s;
    double fallback_after_s;
    int backup_pm;
    double last_cmd_s;
    int last_pm;
    int state; /* tc_link_state_t */
    int fallback_pm;
    tc_baseline_t baseline;
} tc_linkwd_t;

void tc_linkwd_init(tc_linkwd_t *w, double hold_after_s, double fallback_after_s, double baseline_window_s,
                    int backup_pm, int initial_pm, double now_s);

/* Records an accepted command. */
void tc_linkwd_command(tc_linkwd_t *w, double now_s, int pm);

/* Advances the watchdog; returns the state and sets *out_pm to the FiO2 to apply. */
int tc_linkwd_tick(tc_linkwd_t *w, double now_s, int *out_pm);

#ifdef __cplusplus
}
#endif

#endif
