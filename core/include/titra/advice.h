/*
 * The model's side of the advisor, in plain C with caller-owned buffers:
 *   - the prompts, composed from the facts;
 *   - a GBNF grammar composed per request, so the model can only write numbers from that cot's facts, only actions
 *     allowed by that cot's flags, and every cot exactly once;
 *   - the validator every answer passes before anyone sees it, which repeats those checks without trusting the grammar
 *     and adds the rules a grammar cannot express.
 * Nothing here can reach a controller: the advisor's security type has no path to any driver.
 */
#ifndef TITRA_ADVICE_H
#define TITRA_ADVICE_H

#include <stddef.h>

#include "titra/facts.h"

#ifdef __cplusplus
extern "C" {
#endif

#define TA_MAX_COTS 4
#define TA_WHY_MAX 280
#define TA_NOTE_MAX 120

/* The constant start of every prompt, decoded once and shared between requests. */
const char *ta_system_prefix(void);

/* The fixed list of actions. The model can pick one, never compose one. */
#define TA_ACTION_PROBE "check the probe site"
#define TA_ACTION_SUPPORT "review respiratory support"
#define TA_ACTION_WEAN "consider weaning"
#define TA_ACTION_SYSTEM "check the system"
#define TA_ACTION_NONE "no action needed"

/* The actions a cot's flags allow, most urgent first. Returns how many were written to out. */
int ta_allowed_actions(uint32_t flags, const char **out, int cap);

/* The numbers the model may write about this cot, as strings ("71", "0.25", ...). Returns how many. */
int ta_cot_numbers(const tf_facts_t *f, char out[][12], int cap);

/* Everything below returns the length written, or -1 if it does not fit. notes[i] is an optional nurse's note for
 * cots[i], treated as data; NULL or empty means none. notes may be NULL. */
int ta_watch_prompt(const tf_facts_t *cots, int n, const char *const *notes, char *out, size_t cap);
int ta_watch_grammar(const tf_facts_t *cots, int n, char *out, size_t cap);

int ta_handover_prompt(const tf_facts_t *cots, int n, char *out, size_t cap);
int ta_handover_grammar(const tf_facts_t *cots, int n, char *out, size_t cap);

/* An incident on one cot: what failed, what Titra did, for how long, whether FiO2 moved. */
typedef struct {
    int cot;
    const char *what;     /* a fixed phrase chosen by the advisor, such as "the oximeter driver restarted" */
    const char *did;      /* a fixed phrase, such as "held FiO2" */
    int duration_s;
    int fio2_before_pm;
    int fio2_after_pm;
} ta_incident_t;

int ta_incident_prompt(const ta_incident_t *inc, char *out, size_t cap);
int ta_incident_grammar(const ta_incident_t *inc, char *out, size_t cap);

/* The most tokens a valid answer can take, for the deadline logic. */
int ta_watch_max_tokens(int n);
int ta_handover_max_tokens(int n);
int ta_incident_max_tokens(void);

/* The validator. Reasons name the rule that failed. */
typedef enum {
    TA_OK = 0,
    TA_ERR_SCHEMA,     /* not the exact JSON shape, or trailing text */
    TA_ERR_COTS,       /* a cot unknown, repeated or missing */
    TA_ERR_ACTION,     /* an action not allowed by that cot's flags */
    TA_ERR_NUMBER,     /* a number that is not in that cot's facts */
    TA_ERR_FORBIDDEN,  /* words about settings, doses or diagnoses, or text that looks like an instruction */
    TA_ERR_ORDER,      /* a cot with no flag ranked above a cot with a flag */
    TA_ERR_TOO_LONG    /* a sentence longer than the limit */
} ta_result_t;

const char *ta_result_name(ta_result_t r);

typedef struct {
    int cot;
    char action[32];
    char why[TA_WHY_MAX + 1];
} ta_note_t;

typedef struct {
    int n;
    ta_note_t notes[TA_MAX_COTS]; /* in the model's order: first needs the nurse first */
} ta_watch_t;

ta_result_t ta_validate_watch(const char *text, const tf_facts_t *cots, int n, ta_watch_t *out);
ta_result_t ta_validate_handover(const char *text, const tf_facts_t *cots, int n, ta_watch_t *out);
ta_result_t ta_validate_incident(const char *text, const ta_incident_t *inc, char *note, size_t cap);

/* The advice when no valid model answer exists: the deterministic order and one fixed sentence per cot, built only
 * from the facts. Never fails for n in range. */
void ta_fallback_watch(const tf_facts_t *cots, int n, ta_watch_t *out);

/* Formats an answer as JSON for /dev/titra/advice and the station, escaping what JSON requires. Returns the length or
 * -1. source names where it came from ("model" or "flags"). */
int ta_watch_json(const ta_watch_t *w, const char *source, char *out, size_t cap);

/* ctypes helpers: size of ta_watch_t. */
size_t ta_watch_size(void);

#ifdef __cplusplus
}
#endif

#endif
