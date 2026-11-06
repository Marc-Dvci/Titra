/*
 * llm: the language model as a QNX resource manager, /dev/llm (see llm_proto.h).
 *
 *   llm -m /dev/shmem/dory.gguf [-t THREADS] [-f FIRST_CPU] [-p PRIORITY] [-c CTX] [-H] [-T TYPE] [-L LIMIT_MB] [-w STALL_S]
 *   llm -E                 tries everything the security policy must refuse, and reports each result as JSON
 *
 * One compute thread runs llama.cpp with a persistent pool pinned to CPUs FIRST_CPU onward. All of it stays at a fixed low
 * priority: the receiving thread is on a channel with _NTO_CHF_FIXED_PRIORITY, so a client's priority never reaches
 * inference. At each token boundary the compute thread takes the session with the earliest deadline, and picks the
 * largest loop count that can still meet it (core/src/loops.c). One llama_context holds a sequence per session, so
 * switching sessions costs nothing and an urgent note overtakes a long handover at the next token.
 *
 * With -H the process attaches itself to the High Availability Manager like a controller does: it sends a heartbeat while
 * inference makes progress, HAM kills it when the heartbeats stop, and runs its command line again when it dies. The
 * weights stay in their shared memory object (llmstore.c), so the new process is ready in about a second.
 */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/neutrino.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/syspage.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <new>
#include <string>
#include <exception>
#include <vector>

struct llm_ocb;
#define IOFUNC_OCB_T struct llm_ocb
#define RESMGR_OCB_T struct llm_ocb
#include <sys/dispatch.h>
#include <sys/iofunc.h>
#include <sys/resmgr.h>

#include <ha/ham.h>

#include "ggml-cpu.h"
#include "ggml.h"
#include "llama.h"
#include "llm_proto.h"
#include "llm_shared.h"
#include "titra/advice.h"
#include "titra/loops.h"

#define MAX_SESSIONS 3
#define PROMPT_CHUNK 16
#define RECEIVE_PRIORITY 6
#define WATCHDOG_PRIORITY 15 /* above other work, so that heavy load does not look like a hang */
#define PREFIX_OBJECT "/llm-prefix"
#define PREFIX_MAGIC 0x31584650u
#define MSG_MAX 2048

namespace {

double now_ms() {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec * 1e-6;
}

struct session_t {
    int in_use = 0;
    int state = LLM_WAITING;
    int started = 0;
    int cancelled = 0;
    llama_seq_id seq = 0;
    double submitted_ms = 0.0;
    double deadline_ms = 0.0;  /* absolute; 0 means none */
    double first_step_ms = 0.0;
    double prompt_done_ms = 0.0;
    double done_ms = 0.0;
    int min_loops = 1, max_loops = LLM_MAX_LOOPS;
    int max_tokens = 0;
    int feasible = 1;
    int preempted = 0;
    std::vector<llama_token> prompt;
    size_t prompt_pos = 0;
    int prompt_total = 0;
    llama_pos pos = 0;
    llama_sampler *grammar = nullptr;
    llama_token last = 0;
    int generated = 0;
    int tokens_at_loops[LLM_MAX_LOOPS + 1] = {0, 0, 0, 0, 0};
    std::string output;
    size_t read_pos = 0;
    int waiting_rcvid = -1;
    size_t waiting_bytes = 0;
};

/* All shared state: the receiving thread and the compute thread hold g_mtx only to touch it, never across a decode. */
pthread_mutex_t g_mtx = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_cond = PTHREAD_COND_INITIALIZER;
session_t g_sessions[MAX_SESSIONS];

llama_model *g_model = nullptr;
llama_context *g_ctx = nullptr;
const llama_vocab *g_vocab = nullptr;
std::atomic<int> g_ready{0};
std::atomic<uint64_t> g_progress{0};
std::atomic<int> g_decoding{0};
tl_speed_t g_speed;
std::string g_user_suffix;
int g_prefix_tokens = 0;
int g_compute_priority = 5;
unsigned g_runmask = 0;
double g_started_ms = 0.0;
double g_ready_ms = 0.0;
uint64_t g_total_tokens = 0;
uint64_t g_deadlines_missed = 0;
uint64_t g_requests = 0;
const char *g_model_path = "";
int g_loaded_loops = 0;

}  // namespace

struct llm_ocb {
    iofunc_ocb_t hdr;
    session_t *session;
};

namespace {

void set_process_runmask(int first_cpu, int count) {
    const int ncpu = _syspage_ptr->num_cpu;
    const int rsize = RMSK_SIZE(ncpu);
    std::vector<unsigned> buf((size_t)(1 + rsize * 2), 0u);
    buf[0] = (unsigned)rsize;
    unsigned *mask = &buf[1];
    unsigned *inherit = &buf[1 + (size_t)rsize];
    for (int c = first_cpu; c < first_cpu + count && c < ncpu; c++) {
        RMSK_SET(c, mask);
        RMSK_SET(c, inherit);
        g_runmask |= 1u << c;
    }
    if (ThreadCtl(_NTO_TCTL_RUNMASK_GET_AND_SET_INHERIT, buf.data()) == -1) {
        fprintf(stderr, "llm: cannot set the runmask: %s\n", strerror(errno));
    }
}

void set_priority(int prio) {
    struct sched_param sp;
    memset(&sp, 0, sizeof sp);
    sp.sched_priority = prio;
    int rc = pthread_setschedparam(pthread_self(), SCHED_RR, &sp);
    if (rc != 0) fprintf(stderr, "llm: priority %d: %s\n", prio, strerror(rc));
}

void log_quiet(enum ggml_log_level level, const char *text, void *) {
    if (level == GGML_LOG_LEVEL_ERROR) fputs(text, stderr);
}

/* Decodes tokens on one sequence. Only the last one asks for logits. */
int decode_tokens(const llama_token *toks, int n, llama_seq_id seq, llama_pos *pos, llama_batch &batch) {
    batch.n_tokens = n;
    for (int i = 0; i < n; i++) {
        batch.token[i] = toks[i];
        batch.pos[i] = *pos + i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = seq;
        batch.logits[i] = i == n - 1;
    }
    g_decoding.store(1);
    const int rc = llama_decode(g_ctx, batch);
    g_decoding.store(0);
    if (rc == 0) *pos += n;
    return rc;
}

void reply_waiting_reader(session_t *s) {
    if (s->waiting_rcvid < 0) return;
    const size_t have = s->output.size() - s->read_pos;
    const int finished = s->state == LLM_DONE || s->state == LLM_CANCELLED || s->state == LLM_FAILED;
    if (have == 0 && !finished) return;
    const size_t n = have < s->waiting_bytes ? have : s->waiting_bytes;
    MsgReply(s->waiting_rcvid, (long)n, s->output.data() + s->read_pos, n);
    s->read_pos += n;
    s->waiting_rcvid = -1;
}

void finish(session_t *s, int state) {
    s->state = state;
    s->done_ms = now_ms();
    if (s->prompt_done_ms == 0.0) s->prompt_done_ms = s->done_ms;
    if (s->deadline_ms > 0.0 && s->done_ms > s->deadline_ms) g_deadlines_missed++;
    llama_memory_seq_rm(llama_get_memory(g_ctx), s->seq, -1, -1);
    if (s->grammar != nullptr) {
        llama_sampler_free(s->grammar);
        s->grammar = nullptr;
    }
    reply_waiting_reader(s);
}

void release_if_closed(session_t *s) {
    if (s->cancelled && s->state != LLM_RUNNING && s->state != LLM_WAITING) {
        s->prompt.clear();
        s->output.clear();
        s->in_use = 0;
        s->cancelled = 0;
    }
}

void measure_startup(llama_batch &batch) {
    /* A few tokens per loop count on a scratch sequence give the deadline logic real figures for this machine. */
    llama_memory_t mem = llama_get_memory(g_ctx);
    const llama_seq_id scratch = MAX_SESSIONS;
    const int probe[3] = {1, 2, 4};
    std::vector<llama_token> toks((size_t)PROMPT_CHUNK, llama_vocab_bos(g_vocab));
    for (int k : probe) {
        llama_memory_seq_rm(mem, scratch, -1, -1);
        llama_memory_seq_cp(mem, 0, scratch, -1, -1);
        llama_pos pos = g_prefix_tokens;
        llama_set_n_loops(g_ctx, k);
        double t0 = now_ms();
        if (decode_tokens(toks.data(), PROMPT_CHUNK, scratch, &pos, batch) != 0) break;
        tl_speed_prompt(&g_speed, k, (now_ms() - t0) / PROMPT_CHUNK);
        for (int i = 0; i < 6; i++) {
            t0 = now_ms();
            if (decode_tokens(toks.data(), 1, scratch, &pos, batch) != 0) break;
            if (i > 0) tl_speed_gen(&g_speed, k, now_ms() - t0);
        }
    }
    llama_memory_seq_rm(mem, scratch, -1, -1);
}

/* The decoded prefix and the speed table, kept in shared memory: a restarted process picks them up instead of decoding
 * 170 tokens again. Written once by the first process; a mismatch of any field means a normal start. */
struct prefix_header_t {
    uint32_t magic;
    int32_t prefix_tokens;
    uint32_t n_ctx;
    uint64_t state_bytes;
    tl_speed_t speed;
};

bool restore_prefix(int n_ctx, int prefix_tokens) {
    int fd = shm_open(PREFIX_OBJECT, O_RDONLY, 0);
    if (fd < 0) return false;
    struct stat st;
    bool ok = false;
    if (fstat(fd, &st) == 0 && (size_t)st.st_size > sizeof(prefix_header_t)) {
        void *p = mmap(nullptr, (size_t)st.st_size, PROT_READ, MAP_SHARED, fd, 0);
        if (p != MAP_FAILED) {
            const prefix_header_t *h = static_cast<const prefix_header_t *>(p);
            if (h->magic == PREFIX_MAGIC && h->prefix_tokens == prefix_tokens && h->n_ctx == (uint32_t)n_ctx &&
                h->state_bytes + sizeof(prefix_header_t) <= (uint64_t)st.st_size) {
                const uint8_t *data = static_cast<const uint8_t *>(p) + sizeof(prefix_header_t);
                ok = llama_state_seq_set_data(g_ctx, data, (size_t)h->state_bytes, 0) == (size_t)h->state_bytes;
                if (ok) g_speed = h->speed;
            }
            munmap(p, (size_t)st.st_size);
        }
    }
    close(fd);
    return ok;
}

void save_prefix(int n_ctx, int prefix_tokens) {
    const size_t bytes = llama_state_seq_get_size(g_ctx, 0);
    const size_t total = sizeof(prefix_header_t) + bytes;
    int fd = shm_open(PREFIX_OBJECT, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        fprintf(stderr, "llm: cannot keep the prefix: %s\n", strerror(errno));
        return;
    }
    if (ftruncate(fd, (off_t)total) == 0) {
        void *p = mmap(nullptr, total, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (p != MAP_FAILED) {
            prefix_header_t *h = static_cast<prefix_header_t *>(p);
            h->prefix_tokens = prefix_tokens;
            h->n_ctx = (uint32_t)n_ctx;
            h->state_bytes = bytes;
            h->speed = g_speed;
            llama_state_seq_get_data(g_ctx, static_cast<uint8_t *>(p) + sizeof(prefix_header_t), bytes, 0);
            h->magic = PREFIX_MAGIC;
            munmap(p, total);
        }
    }
    close(fd);
}

struct compute_args_t {
    const char *model;
    int threads, first_cpu, n_ctx, poll;
};

void signal_stack_for_this_thread();

void *compute_thread(void *arg) {
    const compute_args_t *a = static_cast<compute_args_t *>(arg);
    set_priority(g_compute_priority);
    signal_stack_for_this_thread();

    llama_log_set(log_quiet, nullptr);
    llama_backend_init();
    llama_model_kv_override kv[3];
    llm_dory_overrides(kv, LLM_MAX_LOOPS);

    llama_model_params mp = llama_model_default_params();
    mp.load_mode = LLAMA_LOAD_MODE_MMAP;
    mp.kv_overrides = kv;
    /* No repacking: it would copy the weights out of their shared object, and a request gains nothing overall. */
    mp.use_extra_bufts = false;
    g_model = llama_model_load_from_file(a->model, mp);
    if (g_model == nullptr) {
        fprintf(stderr, "llm: cannot load %s\n", a->model);
        exit(1);
    }
    g_vocab = llama_model_get_vocab(g_model);
    char arch[32] = "";
    llama_model_meta_val_str(g_model, "general.architecture", arch, sizeof arch);
    const bool looped = strcmp(arch, "dory") == 0;
    g_loaded_loops = looped ? LLM_MAX_LOOPS : 1;

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = (uint32_t)a->n_ctx;
    cp.n_batch = PROMPT_CHUNK;
    cp.n_ubatch = PROMPT_CHUNK;
    cp.n_seq_max = MAX_SESSIONS + 1;
    cp.kv_unified = true;
    cp.n_threads = a->threads;
    cp.n_threads_batch = a->threads;
    g_ctx = llama_init_from_model(g_model, cp);
    if (g_ctx == nullptr) {
        fprintf(stderr, "llm: cannot create the context\n");
        exit(1);
    }

    ggml_threadpool_params tpp;
    ggml_threadpool_params_init(&tpp, a->threads);
    tpp.poll = (uint32_t)a->poll;
    tpp.strict_cpu = true;
    for (int i = 0; i < a->threads; i++) tpp.cpumask[a->first_cpu + i] = true;
    ggml_threadpool *tp = ggml_threadpool_new(&tpp);
    llama_attach_threadpool(g_ctx, tp, tp);

    /* The constant start of every prompt goes on sequence 0 once, at the most loops; each request starts from a copy. */
    llama_batch batch = llama_batch_init(PROMPT_CHUNK, 0, 1);
    std::string prefix;
    llm_chat_format(g_model, ta_system_prefix(), &prefix, &g_user_suffix);
    std::vector<llama_token> ptoks(prefix.size() + 16);
    const int np = llama_tokenize(g_vocab, prefix.c_str(), (int32_t)prefix.size(), ptoks.data(), (int32_t)ptoks.size(), true, true);
    llama_pos pos = 0;
    tl_speed_init(&g_speed);
    g_prefix_tokens = np;
    const bool restored = restore_prefix(a->n_ctx, np);
    if (!restored) {
        if (looped) llama_set_n_loops(g_ctx, LLM_MAX_LOOPS);
        const double t_prefix = now_ms();
        for (int i = 0; i < np; i += PROMPT_CHUNK) {
            const int n = np - i < PROMPT_CHUNK ? np - i : PROMPT_CHUNK;
            if (decode_tokens(ptoks.data() + i, n, 0, &pos, batch) != 0) {
                fprintf(stderr, "llm: decoding the shared prefix failed\n");
                exit(1);
            }
            g_progress++;
        }
        tl_speed_prompt(&g_speed, looped ? LLM_MAX_LOOPS : 1, (now_ms() - t_prefix) / (np > 0 ? np : 1));
        if (looped) measure_startup(batch);
        save_prefix(a->n_ctx, np);
    }

    g_ready_ms = now_ms();
    g_ready.store(1);
    fprintf(stderr, "llm: ready in %.0f ms, prefix %d tokens, %s, arch %s\n", g_ready_ms - g_started_ms, np,
            restored ? "weights and prefix from shared memory" : "prefix decoded", arch);

    int last_index = -1;
    for (;;) {
        pthread_mutex_lock(&g_mtx);
        int pick = -1;
        for (;;) {
            double deadlines[MAX_SESSIONS];
            int runnable[MAX_SESSIONS];
            for (int i = 0; i < MAX_SESSIONS; i++) {
                session_t *s = &g_sessions[i];
                const int active = s->in_use && (s->state == LLM_WAITING || s->state == LLM_RUNNING);
                if (s->in_use && s->cancelled && active) {
                    finish(s, LLM_CANCELLED);
                    release_if_closed(s);
                    runnable[i] = 0;
                    deadlines[i] = 0.0;
                    continue;
                }
                runnable[i] = active;
                /* A request with no deadline runs when nothing with a deadline is waiting. */
                deadlines[i] = s->deadline_ms > 0.0 ? s->deadline_ms : 1e18;
            }
            pick = tl_pick_edf(deadlines, runnable, MAX_SESSIONS);
            if (pick >= 0) break;
            pthread_cond_wait(&g_cond, &g_mtx);
        }
        session_t *s = &g_sessions[pick];
        if (last_index >= 0 && last_index != pick && g_sessions[last_index].in_use && g_sessions[last_index].state == LLM_RUNNING) {
            g_sessions[last_index].preempted++;
        }
        last_index = pick;
        pthread_mutex_unlock(&g_mtx);

        const double t_step = now_ms();
        if (s->state == LLM_WAITING) {
            llama_memory_t mem = llama_get_memory(g_ctx);
            llama_memory_seq_rm(mem, s->seq, -1, -1);
            llama_memory_seq_cp(mem, 0, s->seq, -1, -1);
            s->pos = g_prefix_tokens;
            s->first_step_ms = t_step;
            pthread_mutex_lock(&g_mtx);
            s->state = LLM_RUNNING;
            pthread_mutex_unlock(&g_mtx);
        }

        const int prompt_left = (int)(s->prompt.size() - s->prompt_pos);
        const int gen_left = s->max_tokens - s->generated;
        int loops = s->min_loops;
        if (looped) {
            const double remaining = s->deadline_ms > 0.0 ? s->deadline_ms - t_step : 1e12;
            loops = tl_choose_loops(&g_speed, remaining, prompt_left, gen_left, s->min_loops, s->max_loops, 0.9, nullptr);
            llama_set_n_loops(g_ctx, loops);
        }

        int rc = 0;
        if (prompt_left > 0) {
            const int n = prompt_left < PROMPT_CHUNK ? prompt_left : PROMPT_CHUNK;
            rc = decode_tokens(s->prompt.data() + s->prompt_pos, n, s->seq, &s->pos, batch);
            if (rc == 0) {
                s->prompt_pos += (size_t)n;
                tl_speed_prompt(&g_speed, loops, (now_ms() - t_step) / n);
                if (s->prompt_pos == s->prompt.size()) s->prompt_done_ms = now_ms();
            }
        } else {
            const llama_token tok = llm_sample_next(g_ctx, g_vocab, s->grammar);
            int done = llama_vocab_is_eog(g_vocab, tok) || s->generated >= s->max_tokens;
            if (!done) {
                char piece[256];
                const int len = llama_token_to_piece(g_vocab, tok, piece, sizeof piece, 0, false);
                llama_token one = tok;
                rc = decode_tokens(&one, 1, s->seq, &s->pos, batch);
                if (rc == 0) {
                    tl_speed_gen(&g_speed, loops, now_ms() - t_step);
                    pthread_mutex_lock(&g_mtx);
                    if (len > 0) s->output.append(piece, (size_t)len);
                    s->generated++;
                    s->tokens_at_loops[loops]++;
                    g_total_tokens++;
                    reply_waiting_reader(s);
                    pthread_mutex_unlock(&g_mtx);
                }
            } else {
                pthread_mutex_lock(&g_mtx);
                finish(s, LLM_DONE);
                release_if_closed(s);
                pthread_mutex_unlock(&g_mtx);
            }
        }
        g_progress++;

        if (rc != 0) {
            pthread_mutex_lock(&g_mtx);
            finish(s, LLM_FAILED);
            release_if_closed(s);
            pthread_mutex_unlock(&g_mtx);
        }
    }
    return nullptr;
}

/* ---- the resource manager ---- */

IOFUNC_OCB_T *ocb_calloc(resmgr_context_t *, IOFUNC_ATTR_T *) { return static_cast<IOFUNC_OCB_T *>(calloc(1, sizeof(IOFUNC_OCB_T))); }
void ocb_free(IOFUNC_OCB_T *o) { free(o); }
iofunc_funcs_t g_ocb_funcs;
iofunc_mount_t g_mount;

int status_json(char *buf, size_t cap) {
    pthread_mutex_lock(&g_mtx);
    int active = 0;
    for (auto &s : g_sessions) active += s.in_use;
    const double up = (now_ms() - g_started_ms) / 1000.0;
    const int n = snprintf(buf, cap,
                           "{\"state\":\"%s\",\"model\":\"%s\",\"pid\":%d,\"uptime\":%.1f,\"ready_after_ms\":%.0f,\"sessions\":%d,\"requests\":%llu,\"tokens\":%llu,"
                           "\"deadlines_missed\":%llu,\"priority\":%d,\"runmask\":%u,\"ms_per_token\":{\"1\":%.1f,\"2\":%.1f,\"4\":%.1f}}\n",
                           g_ready.load() ? "ready" : "loading", g_model_path, (int)getpid(), up, g_ready.load() ? g_ready_ms - g_started_ms : 0.0, active,
                           (unsigned long long)g_requests, (unsigned long long)g_total_tokens, (unsigned long long)g_deadlines_missed, g_compute_priority,
                           g_runmask, g_speed.gen_ms[1], g_speed.gen_ms[2], g_speed.gen_ms[4]);
    pthread_mutex_unlock(&g_mtx);
    return n;
}

int on_read(resmgr_context_t *ctp, io_read_t *msg, RESMGR_OCB_T *ocb) {
    int status = iofunc_read_verify(ctp, msg, &ocb->hdr, nullptr);
    if (status != EOK) return status;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
    session_t *s = ocb->session;
    if (s == nullptr) {
        char text[512];
        const int len = status_json(text, sizeof text);
        size_t n = 0;
        if (ocb->hdr.offset < len) {
            n = (size_t)(len - ocb->hdr.offset);
            if (n > msg->i.nbytes) n = msg->i.nbytes;
        }
        if (n == 0) {
            _IO_SET_READ_NBYTES(ctp, 0);
            return _RESMGR_NPARTS(0);
        }
        const off_t off = ocb->hdr.offset;
        ocb->hdr.offset += (off_t)n;
        MsgReply(ctp->rcvid, (long)n, text + off, n);
        return _RESMGR_NOREPLY;
    }
    pthread_mutex_lock(&g_mtx);
    const size_t have = s->output.size() - s->read_pos;
    const int finished = s->state == LLM_DONE || s->state == LLM_CANCELLED || s->state == LLM_FAILED;
    if (have > 0 || finished) {
        const size_t n = have < msg->i.nbytes ? have : msg->i.nbytes;
        MsgReply(ctp->rcvid, (long)n, s->output.data() + s->read_pos, n);
        s->read_pos += n;
        pthread_mutex_unlock(&g_mtx);
        return _RESMGR_NOREPLY;
    }
    if (ocb->hdr.ioflag & O_NONBLOCK) {
        pthread_mutex_unlock(&g_mtx);
        return EAGAIN;
    }
    s->waiting_rcvid = ctp->rcvid;
    s->waiting_bytes = msg->i.nbytes;
    pthread_mutex_unlock(&g_mtx);
    return _RESMGR_NOREPLY;
}

int on_write(resmgr_context_t *ctp, io_write_t *msg, RESMGR_OCB_T *ocb) {
    int status = iofunc_write_verify(ctp, msg, &ocb->hdr, nullptr);
    if (status != EOK) return status;
    if ((msg->i.xtype & _IO_XTYPE_MASK) != _IO_XTYPE_NONE) return ENOSYS;
    if (!g_ready.load()) return EAGAIN;
    if (ocb->session != nullptr) return EBUSY;
    const size_t total = (size_t)_IO_WRITE_GET_NBYTES(msg);
    if (total < sizeof(llm_request_t) || total > LLM_REQUEST_MAX) return EMSGSIZE;
    std::vector<char> blob(total + 1, '\0');
    if (resmgr_msgread(ctp, blob.data(), total, sizeof(msg->i)) != (ssize_t)total) return EIO;
    llm_request_t req;
    memcpy(&req, blob.data(), sizeof req);
    if (req.magic != LLM_MAGIC) return EINVAL;
    const size_t need = sizeof req + (size_t)req.prompt_len + 1 + (req.grammar_len ? (size_t)req.grammar_len + 1 : 0);
    if (need != total || req.max_tokens == 0) return EINVAL;
    const char *prompt = blob.data() + sizeof req;
    const char *grammar = req.grammar_len ? prompt + req.prompt_len + 1 : nullptr;

    std::string text = std::string(prompt, req.prompt_len) + g_user_suffix;
    std::vector<llama_token> toks(text.size() + 16);
    const int n = llama_tokenize(g_vocab, text.c_str(), (int32_t)text.size(), toks.data(), (int32_t)toks.size(), false, true);
    if (n <= 0) return EINVAL;
    toks.resize((size_t)n);

    llama_sampler *smpl = nullptr;
    if (grammar != nullptr) {
        smpl = llama_sampler_init_grammar(g_vocab, grammar, "root");
        if (smpl == nullptr) return EINVAL;
    }
    pthread_mutex_lock(&g_mtx);
    session_t *s = nullptr;
    int slot = -1;
    for (int i = 0; i < MAX_SESSIONS; i++) {
        if (!g_sessions[i].in_use) {
            s = &g_sessions[i];
            slot = i;
            break;
        }
    }
    if (s == nullptr) {
        pthread_mutex_unlock(&g_mtx);
        if (smpl != nullptr) llama_sampler_free(smpl);
        return EBUSY;
    }
    s->~session_t();
    new (s) session_t();
    s->in_use = 1;
    s->seq = slot + 1;
    s->state = LLM_WAITING;
    s->submitted_ms = now_ms();
    s->deadline_ms = req.deadline_ms ? s->submitted_ms + (double)req.deadline_ms : 0.0;
    int lo = req.min_loops < 1 ? 1 : req.min_loops;
    int hi = req.max_loops > LLM_MAX_LOOPS || req.max_loops < 1 ? LLM_MAX_LOOPS : req.max_loops;
    if (lo > LLM_MAX_LOOPS) lo = LLM_MAX_LOOPS;
    if (hi < lo) hi = lo;
    s->min_loops = lo;
    s->max_loops = hi;
    s->max_tokens = (int)req.max_tokens;
    s->prompt = std::move(toks);
    s->prompt_total = n;
    s->grammar = smpl;
    if (req.deadline_ms) {
        int fits = 1;
        tl_choose_loops(&g_speed, (double)req.deadline_ms, n, s->max_tokens, lo, lo, 1.0, &fits);
        s->feasible = fits;
    }
    ocb->session = s;
    g_requests++;
    pthread_cond_signal(&g_cond);
    pthread_mutex_unlock(&g_mtx);
    _IO_SET_WRITE_NBYTES(ctp, (ssize_t)total);
    return EOK;
}

int on_devctl(resmgr_context_t *ctp, io_devctl_t *msg, RESMGR_OCB_T *ocb) {
    int status = iofunc_devctl_default(ctp, msg, &ocb->hdr);
    if (status != _RESMGR_DEFAULT) return status;
    void *data = _DEVCTL_DATA(msg->i);
    if (msg->i.dcmd == (unsigned)DCMD_LLM_RESULT) {
        session_t *s = ocb->session;
        if (s == nullptr) return EINVAL;
        llm_result_t r;
        memset(&r, 0, sizeof r);
        pthread_mutex_lock(&g_mtx);
        r.state = s->state;
        r.prompt_tokens = s->prompt_total;
        r.generated = s->generated;
        memcpy(r.tokens_at_loops, s->tokens_at_loops, sizeof r.tokens_at_loops);
        r.deadline_met = s->deadline_ms <= 0.0 || (s->done_ms > 0.0 && s->done_ms <= s->deadline_ms);
        r.deadline_was_feasible = s->feasible;
        r.preempted = s->preempted;
        r.queue_ms = s->first_step_ms > 0.0 ? s->first_step_ms - s->submitted_ms : 0.0;
        r.prompt_ms = s->prompt_done_ms > 0.0 ? s->prompt_done_ms - s->first_step_ms : 0.0;
        r.gen_ms = s->done_ms > 0.0 ? s->done_ms - s->prompt_done_ms : 0.0;
        r.total_ms = s->done_ms > 0.0 ? s->done_ms - s->submitted_ms : 0.0;
        pthread_mutex_unlock(&g_mtx);
        memcpy(data, &r, sizeof r);
        memset(&msg->o, 0, sizeof msg->o);
        msg->o.nbytes = sizeof r;
        return _RESMGR_PTR(ctp, &msg->o, sizeof msg->o + sizeof r);
    }
    if (msg->i.dcmd == (unsigned)DCMD_LLM_SCHED) {
        llm_sched_t r;
        struct sched_param sp;
        int policy = 0;
        pthread_getschedparam(pthread_self(), &policy, &sp);
        r.receive_priority = sp.sched_curpriority;
        r.compute_priority = g_compute_priority;
        r.compute_runmask = g_runmask;
        memcpy(data, &r, sizeof r);
        memset(&msg->o, 0, sizeof msg->o);
        msg->o.nbytes = sizeof r;
        return _RESMGR_PTR(ctp, &msg->o, sizeof msg->o + sizeof r);
    }
    return ENOSYS;
}

int on_close(resmgr_context_t *ctp, void *reserved, RESMGR_OCB_T *ocb) {
    session_t *s = ocb->session;
    if (s != nullptr) {
        pthread_mutex_lock(&g_mtx);
        s->cancelled = 1;
        const int running = s->state == LLM_WAITING || s->state == LLM_RUNNING;
        if (s->waiting_rcvid >= 0) {
            MsgError(s->waiting_rcvid, ECANCELED);
            s->waiting_rcvid = -1;
        }
        if (!running) {
            release_if_closed(s);
        } else {
            pthread_cond_signal(&g_cond);
        }
        pthread_mutex_unlock(&g_mtx);
        ocb->session = nullptr;
    }
    return iofunc_close_ocb_default(ctp, reserved, &ocb->hdr);
}

int on_unblock(resmgr_context_t *ctp, io_pulse_t *msg, RESMGR_OCB_T *ocb) {
    session_t *s = ocb->session;
    if (s != nullptr) {
        pthread_mutex_lock(&g_mtx);
        if (s->waiting_rcvid == ctp->rcvid) {
            MsgError(s->waiting_rcvid, EINTR);
            s->waiting_rcvid = -1;
            pthread_mutex_unlock(&g_mtx);
            return _RESMGR_NOREPLY;
        }
        pthread_mutex_unlock(&g_mtx);
    }
    return iofunc_unblock_default(ctp, msg, &ocb->hdr);
}

/* ---- watchdog: the heartbeat is progress, not liveness ---- */

struct watch_args_t {
    int use_ham;
    int stall_s;
};

void *watchdog(void *arg) {
    const watch_args_t *w = static_cast<watch_args_t *>(arg);
    set_priority(WATCHDOG_PRIORITY);
    uint64_t seen = 0;
    double last_change = now_ms();
    int reported = 0;
    /* Twice per HAM's period, on an absolute clock. A relative sleep drifts past the period, and each drift is a missed beat. */
    struct timespec next;
    clock_gettime(CLOCK_MONOTONIC, &next);
    double woke = now_ms();
    for (;;) {
        next.tv_nsec += 500000000L;
        if (next.tv_nsec >= 1000000000L) {
            next.tv_nsec -= 1000000000L;
            next.tv_sec += 1;
        }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
        const double t = now_ms();
        if (t - woke > 3000.0) fprintf(stderr, "llm: the watchdog woke %.1f s after its last wake\n", (t - woke) / 1000.0);
        woke = t;
        const uint64_t p = g_progress.load();
        /* Before the first answer the only work is the start-up decode, which has its own progress count. The heartbeat never
         * waits for the shared state: if another thread holds it, this beat counts the model as busy. */
        int busy = 1;
        if (pthread_mutex_trylock(&g_mtx) == 0) {
            busy = 0;
            for (auto &s : g_sessions) busy |= s.in_use && (s.state == LLM_RUNNING || s.state == LLM_WAITING);
            pthread_mutex_unlock(&g_mtx);
        }
        if (p != seen || !busy) {
            seen = p;
            last_change = now_ms();
        }
        const double stalled = (now_ms() - last_change) / 1000.0;
        /* Says once where the work stopped: inside a decode, or in the single-threaded sampling between decodes. */
        if (stalled < 20.0) {
            reported = 0;
        } else if (!reported) {
            reported = 1;
            if (pthread_mutex_trylock(&g_mtx) == 0) {
                for (auto &s : g_sessions) {
                    if (!s.in_use) continue;
                    const size_t len = s.output.size();
                    const size_t from = len > 120 ? len - 120 : 0;
                    fprintf(stderr, "llm: no progress for %.0f s, %s, state %d, prompt %zu of %zu, generated %d, ends: %s\n", stalled,
                            g_decoding.load() ? "inside a decode" : "between decodes", s.state, s.prompt_pos, s.prompt.size(), s.generated, s.output.c_str() + from);
                }
                pthread_mutex_unlock(&g_mtx);
            } else {
                fprintf(stderr, "llm: no progress for %.0f s, shared state held\n", stalled);
            }
        }
        if (w->use_ham && stalled < w->stall_s) ham_heartbeat();
    }
    return nullptr;
}

/* A death says why, on the console: HAM restarts the process either way, and a silent restart hides the fault. */
void on_fatal_signal(int sig) {
    char text[64];
    const int n = snprintf(text, sizeof text, "llm: died on signal %d\n", sig);
    if (n > 0 && write(STDERR_FILENO, text, (size_t)n) < 0) _exit(128 + sig);
    _exit(128 + sig);
}

void on_terminate() {
    const char *what = "unknown";
    try {
        if (std::current_exception()) std::rethrow_exception(std::current_exception());
    } catch (const std::exception &e) {
        what = e.what();
    } catch (...) {
    }
    fprintf(stderr, "llm: terminated: %s\n", what);
    _exit(127);
}

/* Each thread that can overflow its stack gives the handler another one, or a stack overflow would die silently. */
void signal_stack_for_this_thread() {
    static thread_local char area[65536];
    stack_t st;
    memset(&st, 0, sizeof st);
    st.ss_sp = area;
    st.ss_size = sizeof area;
    sigaltstack(&st, nullptr);
}

void report_deaths() {
    std::set_terminate(on_terminate);
    signal_stack_for_this_thread();
    struct sigaction sa;
    memset(&sa, 0, sizeof sa);
    sa.sa_handler = on_fatal_signal;
    sa.sa_flags = SA_ONSTACK;
    for (int sig : {SIGSEGV, SIGBUS, SIGILL, SIGFPE, SIGABRT}) sigaction(sig, &sa, nullptr);
}

void attach_ham(int argc, char **argv, const char *type) {
    char line[512];
    int len = type ? snprintf(line, sizeof line, "/proc/boot/on -T %s", type) : 0;
    line[len] = '\0';
    for (int i = 0; i < argc && len < (int)sizeof line; i++) {
        len += snprintf(line + len, sizeof line - (size_t)len, "%s%s", len ? " " : "", argv[i]);
    }
    if (ham_connect(0) == -1) {
        fprintf(stderr, "llm: HAM is not running: %s\n", strerror(errno));
        return;
    }
    ham_detach_name(0, "llm", 0);
    /* Heartbeat every second; ten missed is "high", which allows for the long first decode under load. */
    ham_entity_t *e = ham_attach_self("llm", 1000000000ULL, 5, 10, 0);
    ham_condition_t *hb = e ? ham_condition(e, CONDHBEATMISSEDHIGH, "missed", 0) : nullptr;
    ham_condition_t *death = e ? ham_condition(e, CONDDEATH, "death", 0) : nullptr;
    if (hb == nullptr || death == nullptr || ham_action_notify_signal(hb, "kill", 0, getpid(), SIGKILL, 0, 0, 0) == nullptr ||
        ham_action_execute(death, "restart", line, 0) == nullptr) {
        fprintf(stderr, "llm: HAM supervision not set up: %s\n", strerror(errno));
    }
}

/* ---- the self-attack: everything below must be refused by the kernel under the policy ---- */

void report(const char *name, int attempts, int refused, int last_errno) {
    printf("{\"attempt\":\"%s\",\"attempts\":%d,\"refused\":%d,\"errno\":\"%s\"}\n", name, attempts, refused, last_errno ? strerror(last_errno) : "");
}

int try_open(const char *name, const char *path, int flags, int attempts) {
    int refused = 0, last = 0;
    for (int i = 0; i < attempts; i++) {
        int fd = open(path, flags);
        if (fd >= 0) {
            close(fd);
        } else {
            refused++;
            last = errno;
        }
    }
    report(name, attempts, refused, last);
    return attempts - refused;
}

void write_pid() {
    FILE *f = fopen("/dev/shmem/titra-llm-0.pid", "w");
    if (f != nullptr) {
        fprintf(f, "%d\n", (int)getpid());
        fclose(f);
    }
    chmod("/dev/shmem/titra-llm-0.pid", 0644);
}

/* The model runs as an ordinary user, so a file's permissions bind it: the weights are read-only for everyone but root. */
void drop_user(int uid) {
    if (uid <= 0) return;
    if (setgid((gid_t)uid) != 0 || setuid((uid_t)uid) != 0) {
        fprintf(stderr, "llm: cannot become user %d: %s\n", uid, strerror(errno));
    }
}

int escape_attempts(int attempts, const char *weights) {
    int opened = 0;
    opened += try_open("open_blender_driver_1", "/dev/valve/1", O_RDWR, attempts);
    opened += try_open("open_blender_driver_2", "/dev/valve/2", O_RDWR, attempts);
    opened += try_open("open_blender_driver_3", "/dev/valve/3", O_RDWR, attempts);
    opened += try_open("open_oximeter_driver_1", "/dev/spo2/1", O_RDWR, attempts);
    opened += try_open("open_controller_1", "/dev/titra/cot1", O_RDWR, attempts);
    opened += try_open("open_advisor", "/dev/titra/advice", O_RDONLY, attempts);

    /* Above the controllers' priority: the ceiling is procnto -P, and this type holds no priority ability. */
    {
        int refused = 0, last = 0;
        struct sched_param sp;
        memset(&sp, 0, sizeof sp);
        sp.sched_priority = 30;
        for (int i = 0; i < attempts; i++) {
            const int rc = pthread_setschedparam(pthread_self(), SCHED_RR, &sp);
            if (rc != 0) {
                refused++;
                last = rc;
            }
        }
        report("raise_priority_to_30", attempts, refused, last);
        opened += attempts - refused;
    }
    /* The network: the type has no right to connect to the socket manager. */
    {
        int refused = 0, last = 0;
        for (int i = 0; i < attempts; i++) {
            int s = socket(AF_INET, SOCK_STREAM, 0);
            if (s >= 0) {
                close(s);
            } else {
                refused++;
                last = errno;
            }
        }
        report("open_a_socket", attempts, refused, last);
        opened += attempts - refused;
    }
    /* Starting another program. */
    {
        int refused = 0, last = 0;
        char *const args[] = {const_cast<char *>("/proc/boot/ksh"), const_cast<char *>("-c"), const_cast<char *>("true"), nullptr};
        for (int i = 0; i < attempts; i++) {
            pid_t pid;
            const int rc = posix_spawn(&pid, "/proc/boot/ksh", nullptr, nullptr, args, nullptr);
            if (rc != 0) {
                refused++;
                last = rc;
            }
        }
        report("spawn_a_shell", attempts, refused, last);
        opened += attempts - refused;
    }
    /* Physical memory: a blender on a real board would sit behind registers like these. */
    {
        int refused = 0, last = 0;
        for (int i = 0; i < attempts; i++) {
            void *p = mmap(nullptr, 4096, PROT_READ | PROT_WRITE | PROT_NOCACHE, MAP_PHYS | MAP_SHARED, NOFD, 0xfee00000);
            if (p == MAP_FAILED) {
                refused++;
                last = errno;
            } else {
                munmap(p, 4096);
            }
        }
        report("map_physical_memory", attempts, refused, last);
        opened += attempts - refused;
    }
    /* I/O privilege. */
    {
        int refused = 0, last = 0;
        for (int i = 0; i < attempts; i++) {
            if (ThreadCtl(_NTO_TCTL_IO_LEVEL, reinterpret_cast<void *>(static_cast<uintptr_t>(_NTO_IO_LEVEL_2))) == -1) {
                refused++;
                last = errno;
            }
        }
        report("gain_io_privilege", attempts, refused, last);
        opened += attempts - refused;
    }
    /* Its own weights: open for writing and map them writable. */
    {
        int refused = 0, last = 0;
        for (int i = 0; i < attempts; i++) {
            int fd = open(weights, O_RDWR);
            if (fd >= 0) {
                close(fd);
            } else {
                refused++;
                last = errno;
            }
        }
        report("open_the_weights_for_writing", attempts, refused, last);
        opened += attempts - refused;
    }
    {
        int refused = 0, last = 0;
        int fd = open(weights, O_RDONLY);
        for (int i = 0; i < attempts; i++) {
            void *p = fd >= 0 ? mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) : MAP_FAILED;
            if (p == MAP_FAILED) {
                refused++;
                last = errno;
            } else {
                munmap(p, 4096);
            }
        }
        if (fd >= 0) close(fd);
        report("map_the_weights_writable", attempts, refused, last);
        opened += attempts - refused;
    }
    /* More memory than its limit. */
    {
        int refused = 0, last = 0;
        const size_t big = (size_t)16 * 1024 * 1024 * 1024;
        for (int i = 0; i < attempts; i++) {
            void *p = mmap(nullptr, big, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, NOFD, 0);
            if (p == MAP_FAILED) {
                refused++;
                last = errno;
            } else {
                munmap(p, big);
            }
        }
        report("allocate_16_gb", attempts, refused, last);
        opened += attempts - refused;
    }
    printf("{\"allowed\":%d}\n", opened);
    return opened ? 1 : 0;
}

}  // namespace

int main(int argc, char **argv) {
    const char *model = "/dev/shmem/dory.gguf", *type = nullptr;
    report_deaths();
    int threads = 3, first_cpu = 1, n_ctx = 4096, use_ham = 0, opt, limit_mb = 6144, stall_s = 60, poll = 50, escape = 0, uid = 300;
    while ((opt = getopt(argc, argv, "m:t:f:p:c:HT:L:w:P:Eu:")) != -1) {
        switch (opt) {
            case 'm': model = optarg; break;
            case 't': threads = atoi(optarg); break;
            case 'f': first_cpu = atoi(optarg); break;
            case 'p': g_compute_priority = atoi(optarg); break;
            case 'c': n_ctx = atoi(optarg); break;
            case 'H': use_ham = 1; break;
            case 'T': type = optarg; break;
            case 'L': limit_mb = atoi(optarg); break;
            case 'w': stall_s = atoi(optarg); break;
            case 'P': poll = atoi(optarg); break;
            case 'E': escape = 1; break;
            case 'u': uid = atoi(optarg); break;
            default:
                fprintf(stderr, "usage: llm [-m MODEL] [-t N] [-f CPU] [-p PRIO] [-c CTX] [-H] [-T TYPE] [-L MB] [-w S] [-P POLL] | llm -E\n");
                return 2;
        }
    }
    g_model_path = model;
    g_started_ms = now_ms();
    struct rlimit rl;
    rl.rlim_cur = rl.rlim_max = (rlim_t)limit_mb * 1024u * 1024u;
    if (setrlimit(RLIMIT_AS, &rl) != 0) fprintf(stderr, "llm: cannot set the memory limit: %s\n", strerror(errno));
    if (escape) {
        drop_user(uid);
        return escape_attempts(100, model);
    }

    set_process_runmask(first_cpu, threads);
    set_priority(RECEIVE_PRIORITY);

    /* The channel has fixed priority: a client at any priority does not lift this thread. */
    int chid = ChannelCreate(_NTO_CHF_FIXED_PRIORITY | _NTO_CHF_UNBLOCK | _NTO_CHF_DISCONNECT);
    dispatch_t *dpp = chid >= 0 ? dispatch_create_channel(chid, 0) : nullptr;
    if (dpp == nullptr) {
        fprintf(stderr, "llm: cannot create the channel: %s\n", strerror(errno));
        return 1;
    }
    resmgr_attr_t rattr;
    memset(&rattr, 0, sizeof rattr);
    rattr.nparts_max = 2;
    rattr.msg_max_size = MSG_MAX;
    static resmgr_connect_funcs_t cf;
    static resmgr_io_funcs_t iof;
    static iofunc_attr_t attr;
    iofunc_func_init(_RESMGR_CONNECT_NFUNCS, &cf, _RESMGR_IO_NFUNCS, &iof);
    iof.read = on_read;
    iof.write = on_write;
    iof.devctl = on_devctl;
    iof.close_ocb = on_close;
    iof.unblock = on_unblock;
    iofunc_attr_init(&attr, S_IFCHR | 0666, nullptr, nullptr);
    memset(&g_ocb_funcs, 0, sizeof g_ocb_funcs);
    g_ocb_funcs.nfuncs = _IOFUNC_NFUNCS;
    g_ocb_funcs.ocb_calloc = ocb_calloc;
    g_ocb_funcs.ocb_free = ocb_free;
    memset(&g_mount, 0, sizeof g_mount);
    g_mount.funcs = &g_ocb_funcs;
    attr.mount = &g_mount;
    if (resmgr_attach(dpp, &rattr, LLM_PATH, _FTYPE_ANY, 0, &cf, &iof, &attr) == -1) {
        fprintf(stderr, "llm: cannot attach %s: %s\n", LLM_PATH, strerror(errno));
        return 1;
    }

    if (use_ham) attach_ham(argc, argv, type);
    write_pid();
    drop_user(uid);
    static watch_args_t wa;
    wa.use_ham = use_ham;
    wa.stall_s = stall_s;
    pthread_t tid;
    pthread_create(&tid, nullptr, watchdog, &wa);

    static compute_args_t ca;
    ca.model = model;
    ca.threads = threads;
    ca.first_cpu = first_cpu;
    ca.n_ctx = n_ctx;
    ca.poll = poll;
    pthread_attr_t pa;
    pthread_attr_init(&pa);
    pthread_attr_setstacksize(&pa, 8u * 1024u * 1024u);
    pthread_create(&tid, &pa, compute_thread, &ca);

    dispatch_context_t *ctp = dispatch_context_alloc(dpp);
    if (ctp == nullptr) return 1;
    for (;;) {
        dispatch_context_t *c = dispatch_block(ctp);
        if (c == nullptr) continue;
        ctp = c;
        dispatch_handler(ctp);
    }
    return 0;
}
