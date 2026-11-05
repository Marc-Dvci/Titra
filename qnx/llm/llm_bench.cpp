/*
 * llm_bench: measures a GGUF model on this machine through llama.cpp's C API, with greedy sampling so that two runs
 * can be compared token for token.
 *
 *   llm_bench -m MODEL [-t THREADS] [-f FIRST_CPU] [-L LOOPS,LOOPS,...] [-n TOKENS] [-c CTX] [-P PRIORITY] [-w POLL]
 *             [-M mmap|read] [-p PROMPT] [-s] [-X]
 * -X turns weight repacking off, so one binary gives the before and the after of a CPU kernel.
 *
 * For a looped model (Dory) the model loads once with its full loop table and each entry of -L runs through
 * llama_set_n_loops(). One JSON line per entry: prompt and generation tokens per second. -s prints the generated text.
 */
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "ggml-cpu.h"
#include "ggml.h"
#include "llama.h"

#if defined(__QNX__)
#include <sys/neutrino.h>
#endif

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

static const char *DEFAULT_PROMPT =
    "Cot 2, last 2 hours: time in target 71 percent, 6 desaturation episodes below 80 percent (longest 48 s), FiO2 median "
    "rose from 0.25 to 0.34, 2 probe-off events, controller mode control. Cot 1: time in target 92 percent, 0 episodes, "
    "FiO2 steady at 0.24. Cot 3: time in target 88 percent, 1 episode, FiO2 fell from 0.31 to 0.27, 31 percent of the time "
    "above 95 percent on oxygen. Summarise for the night nurse which cot needs attention first and why, in two sentences.";

static void quiet_log(enum ggml_log_level level, const char *text, void *) {
    if (level == GGML_LOG_LEVEL_ERROR || level == GGML_LOG_LEVEL_WARN) fputs(text, stderr);
}

int main(int argc, char **argv) {
    const char *model_path = NULL, *prompt = DEFAULT_PROMPT, *mode = "read";
    std::vector<int> loops = {1, 2, 4};
    int threads = 3, first_cpu = 1, n_predict = 64, n_ctx = 4096, prio = 0, poll = 50, show = 0, no_repack = 0, opt;
    while ((opt = getopt(argc, argv, "m:t:f:L:n:c:P:w:M:p:sX")) != -1) {
        switch (opt) {
            case 'm': model_path = optarg; break;
            case 't': threads = atoi(optarg); break;
            case 'f': first_cpu = atoi(optarg); break;
            case 'n': n_predict = atoi(optarg); break;
            case 'c': n_ctx = atoi(optarg); break;
            case 'P': prio = atoi(optarg); break;
            case 'w': poll = atoi(optarg); break;
            case 'M': mode = optarg; break;
            case 'p': prompt = optarg; break;
            case 's': show = 1; break;
            case 'X': no_repack = 1; break;
            case 'L': {
                loops.clear();
                for (char *t = strtok(optarg, ","); t; t = strtok(NULL, ",")) loops.push_back(atoi(t));
                break;
            }
            default:
                fprintf(stderr, "usage: llm_bench -m MODEL [-t N] [-f CPU] [-L 1,2,4] [-n N] [-c N] [-P PRIO] [-w POLL] [-M mmap|read] [-p TEXT] [-s]\n");
                return 2;
        }
    }
    if (model_path == NULL) {
        fprintf(stderr, "llm_bench: -m MODEL is required\n");
        return 2;
    }
    if (prio > 0) {
        struct sched_param sp;
        sp.sched_priority = prio;
        int rc = pthread_setschedparam(pthread_self(), SCHED_RR, &sp);
        if (rc != 0) fprintf(stderr, "llm_bench: priority %d: %s\n", prio, strerror(rc));
    }

    llama_log_set(quiet_log, NULL);
    llama_backend_init();

    /* Dory loads with all four loops in its layer table and one KV slot per physical recurrent layer. */
    llama_model_kv_override kv[3];
    memset(kv, 0, sizeof kv);
    snprintf(kv[0].key, sizeof kv[0].key, "dory.num_loops");
    kv[0].tag = LLAMA_KV_OVERRIDE_TYPE_INT;
    kv[0].val_i64 = 4;
    snprintf(kv[1].key, sizeof kv[1].key, "dory.recurrent_kv_cache_mode");
    kv[1].tag = LLAMA_KV_OVERRIDE_TYPE_STR;
    snprintf(kv[1].val_str, sizeof kv[1].val_str, "last_loop");

    llama_model_params mp = llama_model_default_params();
    mp.load_mode = strcmp(mode, "mmap") == 0 ? LLAMA_LOAD_MODE_MMAP : LLAMA_LOAD_MODE_NONE;
    mp.kv_overrides = kv;
    mp.use_extra_bufts = !no_repack;

    double t0 = now_s();
    llama_model *model = llama_model_load_from_file(model_path, mp);
    if (model == NULL) {
        fprintf(stderr, "llm_bench: cannot load %s\n", model_path);
        return 1;
    }
    const double load_s = now_s() - t0;

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = (uint32_t)n_ctx;
    cp.n_batch = 512;
    cp.n_threads = threads;
    cp.n_threads_batch = threads;
    cp.no_perf = false;
    t0 = now_s();
    llama_context *ctx = llama_init_from_model(model, cp);
    if (ctx == NULL) {
        fprintf(stderr, "llm_bench: cannot create the context\n");
        return 1;
    }
    const double ctx_s = now_s() - t0;

    ggml_threadpool_params tpp;
    ggml_threadpool_params_init(&tpp, threads);
    tpp.poll = (uint32_t)poll;
    tpp.strict_cpu = first_cpu >= 0;
    if (first_cpu >= 0) {
        for (int i = 0; i < threads; i++) tpp.cpumask[first_cpu + i] = true;
    }
    ggml_threadpool *tp = ggml_threadpool_new(&tpp);
    llama_attach_threadpool(ctx, tp, tp);

    printf("{\"load_s\":%.2f,\"context_s\":%.2f,\"threads\":%d,\"first_cpu\":%d,\"poll\":%d,\"mode\":\"%s\",\"repack\":%d}\n", load_s, ctx_s, threads,
           first_cpu, poll, mode, !no_repack);
    fflush(stdout);

    char arch[32] = "";
    llama_model_meta_val_str(model, "general.architecture", arch, sizeof arch);
    const bool is_dory = strcmp(arch, "dory") == 0;

    const llama_vocab *vocab = llama_model_get_vocab(model);
    std::vector<llama_token> toks((size_t)n_ctx);
    int n_prompt = llama_tokenize(vocab, prompt, (int32_t)strlen(prompt), toks.data(), (int32_t)toks.size(), true, true);
    if (n_prompt < 0) return 1;
    toks.resize((size_t)n_prompt);

    llama_sampler *smpl = llama_sampler_chain_init(llama_sampler_chain_default_params());
    llama_sampler_chain_add(smpl, llama_sampler_init_greedy());

    for (int L : loops) {
        llama_memory_clear(llama_get_memory(ctx), true);
        llama_sampler_reset(smpl);
        if (is_dory) llama_set_n_loops(ctx, L);

        std::vector<llama_token> in = toks;
        t0 = now_s();
        if (llama_decode(ctx, llama_batch_get_one(in.data(), (int32_t)in.size())) != 0) {
            fprintf(stderr, "llm_bench: decode failed on the prompt\n");
            return 1;
        }
        const double prompt_s = now_s() - t0;

        std::string text;
        int generated = 0;
        double worst = 0.0;
        t0 = now_s();
        for (int i = 0; i < n_predict; i++) {
            llama_token tok = llama_sampler_sample(smpl, ctx, -1);
            if (llama_vocab_is_eog(vocab, tok)) break;
            char piece[256];
            int len = llama_token_to_piece(vocab, tok, piece, sizeof piece, 0, true);
            if (len > 0) text.append(piece, (size_t)len);
            double t1 = now_s();
            if (llama_decode(ctx, llama_batch_get_one(&tok, 1)) != 0) {
                fprintf(stderr, "llm_bench: decode failed\n");
                return 1;
            }
            double dt = now_s() - t1;
            if (dt > worst) worst = dt;
            generated++;
        }
        const double gen_s = now_s() - t0;
        printf("{\"loops\":%d,\"prompt_tokens\":%d,\"prompt_tok_s\":%.1f,\"generated\":%d,\"gen_tok_s\":%.2f,\"worst_token_ms\":%.0f}\n", L, n_prompt,
               (double)n_prompt / prompt_s, generated, (double)generated / gen_s, worst * 1000.0);
        if (show) printf("%s\n", text.c_str());
        fflush(stdout);
    }
    llama_sampler_free(smpl);
    llama_free(ctx);
    ggml_threadpool_free(tp);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
