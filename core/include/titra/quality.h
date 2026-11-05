/*
 * Sensor-level signal quality, run by the oximeter driver on every parsed frame (docs/SPEC.md, "Signal quality
 * rules"). The controller adds the freshness rule, because only it knows when it last looked.
 */
#ifndef TITRA_QUALITY_H
#define TITRA_QUALITY_H

#include <stdint.h>

#include "titra/frame.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    TC_SAMPLE_VALID = 0,
    TC_SAMPLE_INVALID = 1,   /* missing or flagged: hold, then fall back */
    TC_SAMPLE_UNTRUSTED = 2  /* repeated or frozen: fall back at once */
} tc_sample_status_t;

/* Reasons, as a bit mask. */
#define TC_Q_FLAGGED 0x0001u      /* the oximeter set a status bit */
#define TC_Q_MISSING 0x0002u      /* SpO2 or pulse rate absent */
#define TC_Q_IMPLAUSIBLE 0x0004u  /* pulse rate outside the plausible range */
#define TC_Q_REPEATED 0x0008u     /* the oximeter clock repeats */
#define TC_Q_FROZEN 0x0010u       /* SpO2, pulse rate and perfusion index identical for too long */
#define TC_Q_STALE 0x0020u        /* set by the controller: no frame for too long */
#define TC_Q_NO_DRIVER 0x0040u    /* set by the controller: the driver did not answer */

typedef struct {
    int repeat_clock_frames; /* frames with the same clock that make the run untrusted (3) */
    int frozen_frames;       /* identical frames that make the run untrusted (20) */
    int pr_min;              /* plausible pulse rate, per minute (30 to 300) */
    int pr_max;
} tc_quality_cfg_t;

/* One assessed sample, as the driver publishes it. */
typedef struct {
    uint32_t seq;  /* increments with every assessed frame */
    int status;    /* tc_sample_status_t */
    uint32_t reasons;
    int spo2;
    int pr;
    int pi_centi;
    uint32_t clock;
    double rx_s;              /* monotonic receive time */
    double untrusted_since_s; /* when status is UNTRUSTED: receive time of the first untrusted frame */
} tc_sample_t;

typedef struct {
    tc_quality_cfg_t cfg;
    uint32_t seq;
    int have_prev;
    uint32_t prev_clock;
    int clock_run;          /* consecutive frames sharing prev_clock */
    double clock_run_second_rx_s;
    int prev_spo2, prev_pr, prev_pi;
    int value_run;          /* consecutive frames with identical values */
    double value_run_second_rx_s;
} tc_quality_t;

tc_quality_cfg_t tc_quality_cfg_default(void);
void tc_quality_init(tc_quality_t *q, const tc_quality_cfg_t *cfg);

/* Assesses one parsed frame received at rx_s and returns the published sample. */
tc_sample_t tc_quality_on_frame(tc_quality_t *q, const tc_ox_frame_t *f, double rx_s);

#ifdef __cplusplus
}
#endif

#endif
