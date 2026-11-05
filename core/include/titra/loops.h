/*
 * Real-time decisions for a looped language model, as pure functions so they run under test on any host:
 *   - how many recurrent loops the next decode can afford, given the time left before the request's deadline;
 *   - which request runs next: earliest deadline first, at every token boundary.
 * A looped model is an imprecise computation in the sense of Liu and Layland: the input block, one loop and the output
 * block are the mandatory part, and each extra loop is optional work the scheduler can drop to meet a deadline.
 */
#ifndef TITRA_LOOPS_H
#define TITRA_LOOPS_H

#ifdef __cplusplus
extern "C" {
#endif

#define TL_MAX_LOOPS 4
#define TL_EWMA 0.2 /* weight of the newest measurement */

/* Measured cost per token, per loop count: index 1 to TL_MAX_LOOPS. 0 means not measured yet. */
typedef struct {
    double prompt_ms[TL_MAX_LOOPS + 1];
    double gen_ms[TL_MAX_LOOPS + 1];
} tl_speed_t;

void tl_speed_init(tl_speed_t *s);

/* Folds one measurement in. Non-positive times and loop counts out of range are ignored. */
void tl_speed_prompt(tl_speed_t *s, int loops, double ms_per_token);
void tl_speed_gen(tl_speed_t *s, int loops, double ms_per_token);

/* Estimated cost of a request from here on. A count not measured yet is scaled from the nearest measured one, in
 * proportion to the blocks that run per token. Returns a negative value if nothing has been measured. */
double tl_estimate_ms(const tl_speed_t *s, int loops, int prompt_tokens, int gen_tokens);

/* The largest loop count in [min_loops, max_loops] whose estimate fits in the time left, with a safety margin of
 * `margin` (0.9 uses 90% of the time left). With nothing measured it returns min_loops. If even min_loops does not fit
 * it returns min_loops and sets *fits to 0; otherwise *fits is 1. fits may be NULL. */
int tl_choose_loops(const tl_speed_t *s, double remaining_ms, int prompt_tokens, int gen_tokens, int min_loops,
                    int max_loops, double margin, int *fits);

/* Earliest deadline first: the index of the runnable request with the smallest deadline, or -1 if none is runnable.
 * Ties go to the lowest index, so the order is deterministic. */
int tl_pick_edf(const double *deadline_ms, const int *runnable, int n);

#ifdef __cplusplus
}
#endif

#endif
