/*
 * Median FiO2 over a sliding time window: the baseline that the fallback FiO2 never goes below
 * (docs/SPEC.md, "Safety layers"). Fixed memory, constant-time insert and expiry, median in one pass over a
 * per-mille histogram.
 */
#ifndef TITRA_BASELINE_H
#define TITRA_BASELINE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TC_BASELINE_CAP 512

typedef struct {
    double window_s;
    double t[TC_BASELINE_CAP];
    int16_t pm[TC_BASELINE_CAP];
    int head;  /* oldest entry */
    int count;
    uint16_t hist[1001];
} tc_baseline_t;

void tc_baseline_init(tc_baseline_t *b, double window_s);

/* Adds a value (per mille, 0 to 1000) at time t_s, expiring entries older than the window. When the buffer is
 * full the oldest entry is dropped. */
void tc_baseline_add(tc_baseline_t *b, double t_s, int pm);

/* Drops entries older than now_s minus the window. */
void tc_baseline_expire(tc_baseline_t *b, double now_s);

/* Removes every entry added at or after since_s (values computed from data later found untrusted). */
void tc_baseline_drop_since(tc_baseline_t *b, double since_s);

/* Lower median of the entries, or -1 when empty. */
int tc_baseline_median(const tc_baseline_t *b);

int tc_baseline_count(const tc_baseline_t *b);

#ifdef __cplusplus
}
#endif

#endif
