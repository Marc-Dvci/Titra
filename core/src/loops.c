/* Loop-count and request choices. See titra/loops.h. */
#include "titra/loops.h"

#include <stddef.h>

void tl_speed_init(tl_speed_t *s) {
    for (int i = 0; i <= TL_MAX_LOOPS; i++) {
        s->prompt_ms[i] = 0.0;
        s->gen_ms[i] = 0.0;
    }
}

static void fold(double *slot, double ms) {
    *slot = *slot > 0.0 ? (1.0 - TL_EWMA) * *slot + TL_EWMA * ms : ms;
}

void tl_speed_prompt(tl_speed_t *s, int loops, double ms_per_token) {
    if (loops < 1 || loops > TL_MAX_LOOPS || !(ms_per_token > 0.0)) return;
    fold(&s->prompt_ms[loops], ms_per_token);
}

void tl_speed_gen(tl_speed_t *s, int loops, double ms_per_token) {
    if (loops < 1 || loops > TL_MAX_LOOPS || !(ms_per_token > 0.0)) return;
    fold(&s->gen_ms[loops], ms_per_token);
}

/* Per-token cost grows linearly with the loop count: (input + output blocks) + loops * (recurrent block). Dory has
 * 32 fixed blocks and 40 per loop, so a count not yet measured is scaled from the nearest one by that ratio. */
static double scaled(const double *table, int loops) {
    if (table[loops] > 0.0) return table[loops];
    for (int d = 1; d < TL_MAX_LOOPS; d++) {
        const int candidates[2] = {loops - d, loops + d};
        for (int c = 0; c < 2; c++) {
            const int k = candidates[c];
            if (k >= 1 && k <= TL_MAX_LOOPS && table[k] > 0.0) {
                return table[k] * (32.0 + 40.0 * loops) / (32.0 + 40.0 * k);
            }
        }
    }
    return -1.0;
}

double tl_estimate_ms(const tl_speed_t *s, int loops, int prompt_tokens, int gen_tokens) {
    if (loops < 1 || loops > TL_MAX_LOOPS) return -1.0;
    double total = 0.0;
    if (prompt_tokens > 0) {
        const double p = scaled(s->prompt_ms, loops);
        if (p < 0.0) return -1.0;
        total += p * prompt_tokens;
    }
    if (gen_tokens > 0) {
        const double g = scaled(s->gen_ms, loops);
        if (g < 0.0) return -1.0;
        total += g * gen_tokens;
    }
    return total;
}

int tl_choose_loops(const tl_speed_t *s, double remaining_ms, int prompt_tokens, int gen_tokens, int min_loops,
                    int max_loops, double margin, int *fits) {
    if (min_loops < 1) min_loops = 1;
    if (max_loops > TL_MAX_LOOPS) max_loops = TL_MAX_LOOPS;
    if (max_loops < min_loops) max_loops = min_loops;
    if (fits != NULL) *fits = 1;
    const double budget = remaining_ms * margin;
    for (int k = max_loops; k >= min_loops; k--) {
        const double est = tl_estimate_ms(s, k, prompt_tokens, gen_tokens);
        if (est < 0.0) return min_loops;
        if (est <= budget) return k;
    }
    if (fits != NULL) *fits = 0;
    return min_loops;
}

int tl_pick_edf(const double *deadline_ms, const int *runnable, int n) {
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (runnable[i] && (best < 0 || deadline_ms[i] < deadline_ms[best])) best = i;
    }
    return best;
}
