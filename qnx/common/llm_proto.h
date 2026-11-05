/*
 * /dev/llm: the language model as a QNX resource manager.
 *
 * One open is one session. A client write()s a request (llm_request_t, then the prompt and the grammar, each ending in
 * a NUL byte) and read()s the answer as the model produces it; read returns 0 at the end. devctl(DCMD_LLM_RESULT) then
 * gives the timings. close() cancels whatever is still running. An open with no request read()s one JSON status line,
 * so `cat /dev/llm` shows the model's state.
 *
 * The server runs on a channel with fixed priority: a client's message does not lift the receiving thread, so no client
 * can raise inference above its own low priority (docs/ADVISOR.md).
 */
#ifndef TITRA_LLM_PROTO_H
#define TITRA_LLM_PROTO_H

#include <devctl.h>
#include <stdint.h>

#define LLM_PATH "/dev/llm"
#define LLM_MAGIC 0x4d4c4c54u /* "TLLM" */
#define LLM_REQUEST_MAX (64 * 1024)
#define LLM_MAX_LOOPS 4

typedef struct {
    uint32_t magic;
    uint32_t deadline_ms;  /* from the moment the write arrives; 0 means no deadline */
    uint32_t max_tokens;
    uint16_t min_loops;    /* 1 to LLM_MAX_LOOPS */
    uint16_t max_loops;
    uint32_t prompt_len;   /* bytes of prompt text, not counting its NUL */
    uint32_t grammar_len;  /* bytes of GBNF, not counting its NUL; 0 means free text */
} llm_request_t;

enum { LLM_WAITING = 0, LLM_RUNNING = 1, LLM_DONE = 2, LLM_CANCELLED = 3, LLM_FAILED = 4 };

typedef struct {
    int32_t state;
    int32_t prompt_tokens;
    int32_t generated;
    int32_t tokens_at_loops[LLM_MAX_LOOPS + 1]; /* generated tokens per loop count */
    int32_t deadline_met;                       /* 1 yes, 0 no; 1 when there was no deadline */
    int32_t deadline_was_feasible;              /* at submission, the estimate fit at the minimum loop count */
    int32_t preempted;                          /* times another session ran between two of this one's steps */
    double queue_ms;
    double prompt_ms;
    double gen_ms;
    double total_ms;
} llm_result_t;

/* What the scheduler is doing, for measurement: the receiving thread's priority while it serves this very message,
 * which proves that the client's priority did not reach it. */
typedef struct {
    int32_t receive_priority;
    int32_t compute_priority;
    uint32_t compute_runmask;
} llm_sched_t;

#define DCMD_LLM_RESULT __DIOF(_DCMD_MISC, 0x60, llm_result_t)
#define DCMD_LLM_SCHED __DIOF(_DCMD_MISC, 0x61, llm_sched_t)

#endif
