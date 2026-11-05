/*
 * One cot's on-board chain in a single object, for the bench's simulation runs: oximeter bytes go in, blender
 * command frames come out, through the same code the QNX processes run:
 *   spo2d   line framing, $OX parsing, signal quality        (frame.c, quality.c)
 *   titractl controller                                      (control.c)
 *   valved  command guard, link watchdog, $BL formatting      (guard.c, frame.c)
 * The API uses only plain types so that it can be called through ctypes.
 */
#ifndef TITRA_SIM_H
#define TITRA_SIM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    double fio2;        /* FiO2 in the frame sent to the blender */
    double ctl_fio2;    /* FiO2 the controller asked for */
    int mode;           /* controller mode */
    int alarm;          /* alarm priority in the frame */
    uint32_t alarms;    /* controller alarm conditions */
    uint32_t reasons;   /* signal quality reasons behind a hold or fallback */
    int adjusted;       /* the controller changed its command on this tick */
    int refused;        /* the guard refused the controller's command on this tick */
    int sample_status;  /* status of the latest sample, or -1 if none */
    int spo2;           /* SpO2 of the latest sample, or -1 */
} tc_sim_tick_t;

size_t tc_sim_size(void);
size_t tc_sim_tick_size(void);

/* Initialises a cot in caller-provided memory of tc_sim_size() bytes. FiO2 values as fractions. */
void tc_sim_init(void *mem, double fio2_min, double fio2_max, double backup_fio2, double initial_fio2, double now_s);

/* Sets a controller parameter by name (see tc_params_set) and restarts the controller with it. Returns 0, or -1
 * for an unknown name. Call before the first tick. */
int tc_sim_param(void *mem, const char *name, double value);

/* Feeds bytes received from the oximeter at rx_s. Returns the number of frames that parsed. */
int tc_sim_feed(void *mem, const char *bytes, size_t n, double rx_s);

/* Runs one controller tick and the blender driver. Writes the $BL frame into bl (NUL-terminated) and returns its
 * length, or 0 if it could not be formatted. */
size_t tc_sim_tick(void *mem, double now_s, char *bl, size_t cap, tc_sim_tick_t *out);

/* Counters since init. */
uint32_t tc_sim_frames_ok(const void *mem);
uint32_t tc_sim_frames_rejected(const void *mem);
uint32_t tc_sim_bytes_dropped(const void *mem);

#ifdef __cplusplus
}
#endif

#endif
