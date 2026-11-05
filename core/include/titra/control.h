/*
 * The cot controller: a fixed-gain proportional-integral law on SpO2, run once a second, inside a mode machine
 * that holds, falls back and resumes (docs/SPEC.md, "Control law" and "Safety layers").
 *
 * The controller owns every decision about FiO2. It never allocates, never blocks and reads no clock: the caller
 * passes the monotonic time of each tick.
 */
#ifndef TITRA_CONTROL_H
#define TITRA_CONTROL_H

#include <stdint.h>

#include "titra/baseline.h"
#include "titra/quality.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TC_MODE_CONTROL = 0,  /* closed loop on valid SpO2 */
    TC_MODE_HOLD = 1,     /* SpO2 missing or flagged: FiO2 held */
    TC_MODE_FALLBACK = 2  /* fallback FiO2 applied */
} tc_mode_t;

typedef enum { TC_ALARM_NONE = 0, TC_ALARM_LOW = 1, TC_ALARM_MEDIUM = 2, TC_ALARM_HIGH = 3 } tc_alarm_t;

/* Alarm conditions, as a bit mask, with their priority. */
#define TC_A_SIGNAL_HOLD 0x0001u /* medium: SpO2 missing or flagged, FiO2 held */
#define TC_A_FALLBACK 0x0002u    /* high: fallback FiO2 applied */
#define TC_A_UNTRUSTED 0x0004u   /* high: repeated or frozen oximeter data */
#define TC_A_SPO2_LOW 0x0008u    /* medium: SpO2 below spo2_low for spo2_low_s */
#define TC_A_SPO2_VLOW 0x0010u   /* high: SpO2 below spo2_vlow for spo2_vlow_s */
#define TC_A_SPO2_HIGH 0x0020u   /* medium: SpO2 above spo2_high on oxygen for spo2_high_s */
#define TC_A_AT_MAX 0x0040u      /* high: FiO2 at its upper limit with SpO2 below target for at_max_s */

/* Per-cot settings chosen by the clinician. FiO2 as a fraction. */
typedef struct {
    double fio2_min;
    double fio2_max;
    double backup_fio2;
} tc_cot_settings_t;

/* Control law and timing. Gains are FiO2 per percentage point of SpO2 error (proportional, on the change in
 * error) and per percentage point per second (integral). */
typedef struct {
    double setpoint;
    double target_low;
    double target_high;
    double kp_lo, ki_lo; /* SpO2 below target_low */
    double kp_in, ki_in; /* SpO2 inside the target range */
    double kp_hi, ki_hi; /* SpO2 above target_high */
    double rate_up;      /* largest FiO2 increase per second */
    double rate_down;    /* largest FiO2 decrease per second */
    double quantum;      /* resolution of the commanded FiO2 */
    double hysteresis;   /* the command moves once the internal FiO2 is this many quanta away */
    double max_dt_s;     /* longest interval one update may integrate over */
    double stale_s;      /* no new sample for this long makes SpO2 missing */
    double hold_s;       /* hold this long after the last valid sample, then fall back */
    int resume_valid;    /* consecutive valid samples needed to leave fallback */
    double baseline_window_s;
    double spo2_low, spo2_low_s;
    double spo2_vlow, spo2_vlow_s;
    double spo2_high, spo2_high_s;
    double at_max_s;
} tc_params_t;

typedef struct {
    double fio2;       /* commanded FiO2, quantised and inside the cot's limits */
    int mode;          /* tc_mode_t */
    int alarm;         /* tc_alarm_t, the highest active priority */
    uint32_t alarms;   /* TC_A_* conditions active */
    uint32_t reasons;  /* TC_Q_* reasons behind a hold or fallback */
    double fallback_fio2;
    int adjusted;      /* the commanded FiO2 changed on this tick */
} tc_output_t;

typedef struct {
    tc_params_t p;
    tc_cot_settings_t s;
    tc_baseline_t baseline;
    int mode;
    double u;            /* internal FiO2, before quantisation */
    double cmd;          /* last commanded FiO2 */
    double fallback_fio2;
    int have_e;          /* e_prev is set */
    double e_prev;
    double last_update_s;
    uint32_t last_seq;
    int have_seq;
    double last_valid_s;
    int valid_run;
    uint32_t reasons;
    double low_since_s, vlow_since_s, high_since_s, at_max_since_s; /* < 0 when the condition is not active */
} tc_controller_t;

tc_params_t tc_params_default(void);

/* Sets one parameter by name; returns 0, or -1 for an unknown name. Used by the bench to explore gains. */
int tc_params_set(tc_params_t *p, const char *name, double value);

/* Starts the controller at initial_fio2, the FiO2 the blender currently applies, so a (re)start causes no step.
 * Control begins in HOLD until the first valid sample arrives. */
void tc_controller_init(tc_controller_t *c, const tc_params_t *p, const tc_cot_settings_t *s, double initial_fio2,
                        double now_s);

/* One tick. `sample` is the driver's latest sample, or NULL if the driver did not answer. */
tc_output_t tc_controller_step(tc_controller_t *c, const tc_sample_t *sample, double now_s);

/* The fallback FiO2 the controller would apply now: the higher of the backup FiO2 and the baseline. */
double tc_controller_fallback(const tc_controller_t *c);

const char *tc_mode_name(int mode);

#ifdef __cplusplus
}
#endif

#endif
