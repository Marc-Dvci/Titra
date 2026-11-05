/*
 * Facts and flags: what the on-board advisor knows about a cot, computed in plain C from the controller's own status,
 * one sample a second. Every number the model may write comes from here, and the flags are decided here, not by the
 * model (docs/ADVISOR.md). No dynamic allocation; the caller owns the history.
 */
#ifndef TITRA_FACTS_H
#define TITRA_FACTS_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* One second of a cot, from its status. */
typedef struct {
    int16_t spo2;     /* displayed SpO2, or -1 when there is no reading */
    int16_t fio2_pm;  /* commanded FiO2 in per mille, or -1 when the controller did not answer */
    uint8_t mode;     /* tc_mode_t */
    uint8_t alarm;    /* tc_alarm_t */
    uint8_t status;   /* tc_sample_status_t, or 255 when there is no sample */
    uint8_t reserved;
} tf_second_t;

#define TF_FLAG_RISING_NEED 0x01u      /* the oxygen need rose across the window */
#define TF_FLAG_WEAN 0x02u             /* the need fell, or SpO2 sat above target on oxygen */
#define TF_FLAG_DESATS 0x04u           /* frequent desaturation episodes */
#define TF_FLAG_PROBE 0x08u            /* repeated signal loss or artefact */
#define TF_FLAG_HOLD_FALLBACK 0x10u    /* the cot held or fell back for long in this window */
#define TF_FLAG_HIGH_NEED 0x20u        /* FiO2 stayed high */
#define TF_FLAG_SYSTEM 0x40u           /* a process restarted in this window */
#define TF_FLAG_COUNT 7

/* The thresholds behind the flags, tuned on the development seeds only (eval/flag_thresholds.json). */
typedef struct {
    int min_trend_window_s;  /* the need's trend needs at least this much valid history */
    int min_rate_window_s;   /* rates per hour need at least this long a window */
    double rising_delta;     /* FiO2 rise between the first and the last third */
    double wean_delta;       /* FiO2 fall between the first and the last third */
    double desats_per_h;     /* desaturation episodes per hour */
    double probe_per_h;      /* signal-loss events per hour */
    double hold_s_per_h;     /* seconds in hold per hour */
    double fallback_s_per_h; /* seconds in fallback per hour */
    double high_fio2;        /* what counts as high FiO2 */
    double high_fio2_pct;    /* time at or above high_fio2 */
} tf_thresholds_t;

typedef struct {
    int cot;
    int seconds;              /* seconds in the window */
    int valid_seconds;        /* seconds with a valid SpO2 */
    int time_in_target_pct;   /* the trial's definition: 91-95% on oxygen, 91-100% in room air */
    int above_95_on_o2_pct;
    int below_85_pct;
    int desat_episodes;       /* SpO2 below 85% for at least 10 s */
    int longest_desat_s;
    int fio2_early_pm;        /* median FiO2 of the first third */
    int fio2_late_pm;         /* median FiO2 of the last third */
    int fio2_max_pm;
    int probe_events;         /* signal lost for at least 3 s, counted once per loss */
    int invalid_pct;          /* time without a valid sample */
    int hold_s;
    int fallback_s;
    int restarts;
    int fallback_now;         /* the last second of the window was in fallback */
    uint32_t flags;
    int priority;             /* the deterministic order: higher needs the nurse sooner */
} tf_facts_t;

tf_thresholds_t tf_thresholds_default(void);

/* Computes the facts of `n` seconds read from a ring of `cap` entries, oldest at `head`. `restarts` is the count of
 * process restarts the caller saw in the same window. Never reads outside the ring. A cot with no valid data gets
 * facts with zero percentages and no trend. */
void tf_compute(const tf_second_t *ring, int cap, int head, int n, int cot, int restarts, const tf_thresholds_t *th,
                tf_facts_t *out);

/* Size of tf_second_t and tf_facts_t, for the bench's ctypes layer to check its own view of them. */
size_t tf_second_size(void);
size_t tf_facts_size(void);

/* A short name for one flag, for logs and JSON. NULL for an unknown bit. */
const char *tf_flag_name(uint32_t flag);

/* Formats facts as one JSON object. Returns the length, or -1 if it does not fit. */
int tf_json(const tf_facts_t *f, char *buf, size_t cap);

#ifdef __cplusplus
}
#endif

#endif
