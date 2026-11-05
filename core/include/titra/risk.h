/*
 * The early-warning model: how likely is this cot to dip below 85% within ten minutes? A small gradient-boosted tree
 * ensemble trained on simulated babies (scripts/train_risk.py, eval/risk_cases.json), evaluated here in plain C: no
 * allocation, no floating-point loops of unknown length, and a fixed worst-case path through a fixed number of trees.
 * Its score is one more fact for the nurse's list. It never reaches a controller.
 */
#ifndef TITRA_RISK_H
#define TITRA_RISK_H

#include <stddef.h>

#include "titra/facts.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TR_N_FEATURES 13
#define TR_WINDOW_SHORT 900
#define TR_WINDOW_LONG 3600

/* The features, from the seconds before the newest one: the last 15 minutes and the last hour of what the advisor sees.
 * Index order matches the trained model:
 *   0 mean SpO2, 1 SD of SpO2, 2 lowest SpO2, 3 percent below 91, 4 percent below 85 (all 15 min)
 *   5 dips below 85 for 10 s or more (15 min), 6 the same (1 h)
 *   7 mean FiO2, 8 FiO2 change across the 15 min, 9 SD of FiO2
 *   10 percent above 95 on oxygen (15 min), 11 percent with no valid reading (15 min)
 *   12 seconds since the last dip below 85, up to 3600 */
void tr_features(const tf_second_t *ring, int cap, int head, int n, double out[TR_N_FEATURES]);

/* The probability of a dip in the next ten minutes, 0 to 1. */
double tr_score(const double features[TR_N_FEATURES]);

/* The same as a whole percent, for the prompt and the page. */
int tr_percent(const tf_second_t *ring, int cap, int head, int n);

/* The model's shape, for tests. */
int tr_tree_count(void);
int tr_max_depth(void);

#ifdef __cplusplus
}
#endif

#endif
