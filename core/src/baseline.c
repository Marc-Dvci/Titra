/* Sliding-window median baseline. See baseline.h. */
#include "titra/baseline.h"

#include <string.h>

void tc_baseline_init(tc_baseline_t *b, double window_s) {
    memset(b, 0, sizeof(*b));
    b->window_s = window_s;
}

static void pop_oldest(tc_baseline_t *b) {
    b->hist[b->pm[b->head]]--;
    b->head = (b->head + 1) % TC_BASELINE_CAP;
    b->count--;
}

void tc_baseline_expire(tc_baseline_t *b, double now_s) {
    while (b->count > 0 && b->t[b->head] < now_s - b->window_s) {
        pop_oldest(b);
    }
}

void tc_baseline_add(tc_baseline_t *b, double t_s, int pm) {
    if (pm < 0) pm = 0;
    if (pm > 1000) pm = 1000;
    tc_baseline_expire(b, t_s);
    if (b->count == TC_BASELINE_CAP) pop_oldest(b);
    int tail = (b->head + b->count) % TC_BASELINE_CAP;
    b->t[tail] = t_s;
    b->pm[tail] = (int16_t)pm;
    b->hist[pm]++;
    b->count++;
}

void tc_baseline_drop_since(tc_baseline_t *b, double since_s) {
    while (b->count > 0) {
        int last = (b->head + b->count - 1) % TC_BASELINE_CAP;
        if (b->t[last] < since_s) break;
        b->hist[b->pm[last]]--;
        b->count--;
    }
}

int tc_baseline_median(const tc_baseline_t *b) {
    if (b->count == 0) return -1;
    int rank = (b->count - 1) / 2; /* zero-based rank of the lower median */
    int seen = 0;
    for (int v = 0; v <= 1000; v++) {
        seen += b->hist[v];
        if (seen > rank) return v;
    }
    return -1; /* hist and count always agree, so the loop returns first */
}

int tc_baseline_count(const tc_baseline_t *b) { return b->count; }
