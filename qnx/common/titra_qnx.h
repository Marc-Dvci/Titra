/*
 * Messages between Titra's QNX processes, and small helpers they share.
 *
 *   /dev/spo2/N      spo2d     DCMD_TITRA_SPO2_GET   latest assessed oximeter sample
 *   /dev/valve/N     valved    DCMD_TITRA_VALVE_SET  command from the controller, reply with the driver's state
 *                              DCMD_TITRA_VALVE_GET  the driver's state
 *   /dev/titra/cotN  titractl  DCMD_TITRA_COT_GET    the cot's status; read() gives the same as one JSON line
 *
 * A devctl() is a QNX message: the receiving thread in the driver runs at the sender's priority.
 */
#ifndef TITRA_QNX_H
#define TITRA_QNX_H

#include <devctl.h>
#include <stdint.h>
#include <time.h>

#include "titra/control.h"
#include "titra/quality.h"

typedef struct {
    tc_sample_t sample;
    int have_sample;
    int link_up;
    uint32_t frames_ok;
    uint32_t frames_rejected;
    uint32_t bytes_dropped;
} titra_spo2_state_t;

typedef struct {
    int fio2_pm;
    int mode;  /* tc_mode_t */
    int alarm; /* tc_alarm_t */
} titra_valve_cmd_t;

typedef struct {
    int result;          /* tc_guard_result_t of the last command */
    int applied_pm;      /* FiO2 the driver is sending to the blender */
    int link_state;      /* 'N', 'H' or 'F': the driver's own watchdog on the controller */
    int blender_pm;      /* FiO2 the blender reports in the circuit */
    int blender_state;   /* 'N', 'H' or 'F' from the blender, or 0 before the first status */
    int blender_link_up; /* a status frame arrived in the last 3 s */
    int backup_pm;
    uint32_t commands;
    uint32_t refusals;
} titra_valve_state_t;

typedef union {
    titra_valve_cmd_t cmd;
    titra_valve_state_t state;
} titra_valve_msg_t;

typedef struct {
    int cot;
    double t;          /* monotonic seconds of the last tick */
    double fio2;       /* commanded FiO2 */
    int mode;
    int alarm;
    uint32_t alarms;
    uint32_t reasons;
    int spo2;          /* latest SpO2, or -1 */
    int pr;
    int sample_status; /* tc_sample_status_t, or -1 */
    int blender_pm;
    int valve_link;    /* the blender driver answered on the last tick */
    double fallback_fio2;
    double backup_fio2;
    uint32_t ticks;
    uint32_t late_ticks;   /* ticks more than 1.5 s after the previous one */
    double max_interval_s; /* longest interval between ticks */
    double started_s;      /* monotonic time this controller started */
} titra_cot_status_t;

#define DCMD_TITRA_SPO2_GET __DIOF(_DCMD_MISC, 0x51, titra_spo2_state_t)
#define DCMD_TITRA_VALVE_SET __DIOTF(_DCMD_MISC, 0x52, titra_valve_msg_t)
#define DCMD_TITRA_VALVE_GET __DIOF(_DCMD_MISC, 0x53, titra_valve_state_t)
#define DCMD_TITRA_COT_GET __DIOF(_DCMD_MISC, 0x54, titra_cot_status_t)

static inline double titra_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* Sets the calling thread to SCHED_FIFO at `prio`. Returns 0 or an errno value. */
int titra_set_priority(int prio);

/* Writes this process's pid to /dev/shmem/titra-NAME-COT.pid, so the bench can find one cot's process. */
void titra_write_pid(const char *name, int cot);

/* Formats a cot status as one JSON line. Returns the length. */
int titra_cot_json(const titra_cot_status_t *s, char *buf, int cap);

#endif
