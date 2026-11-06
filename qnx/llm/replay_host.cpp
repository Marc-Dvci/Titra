/*
 * replay_host: replays case windows through the model on a host, for scoring answer quality quickly.
 *
 *   replay_host -m MODEL -f WINDOWS.txt [-l LOOPS] [-g GPU_LAYERS] > answers.jsonl
 *
 * The same C core composes each prompt and grammar and validates each answer, the same code picks each token
 * (llm_shared.h), and decoding runs in the same chunks from the same shared prefix. It records answers only: no timings
 * and no deadlines, which `advisor -r` measures on the board.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <string>
#include <vector>

#include "llama.h"
#include "llm_shared.h"
#include "titra/advice.h"
#include "titra/facts.h"

#define PROMPT_CHUNK 16
#define SEQ_PREFIX 0
#define SEQ_ANSWER 1

static void quiet(enum ggml_log_level level, const char *text, void *) {
    if (level == GGML_LOG_LEVEL_ERROR) fputs(text, stderr);
}

static int decode(llama_context *ctx, llama_batch &batch, const llama_token *toks, int n, llama_seq_id seq, llama_pos *pos) {
    batch.n_tokens = n;
    for (int i = 0; i < n; i++) {
        batch.token[i] = toks[i];
        batch.pos[i] = *pos + i;
        batch.n_seq_id[i] = 1;
        batch.seq_id[i][0] = seq;
        batch.logits[i] = i == n - 1;
    }
    const int rc = llama_decode(ctx, batch);
    if (rc == 0) *pos += n;
    return rc;
}

static std::string json_text(const std::string &text) {
    std::string out;
    for (const char c : text) {
        if (c == '"' || c == '\\') out.push_back('\\');
        out.push_back((unsigned char)c < 0x20 ? ' ' : c);
    }
    return out;
}

int main(int argc, char **argv) {
    const char *model_path = nullptr, *windows = nullptr;
    int loops = 4, gpu_layers = 999, opt;
    while ((opt = getopt(argc, argv, "m:f:l:g:")) != -1) {
        switch (opt) {
            case 'm': model_path = optarg; break;
            case 'f': windows = optarg; break;
            case 'l': loops = atoi(optarg); break;
            case 'g': gpu_layers = atoi(optarg); break;
            default:
                fprintf(stderr, "usage: replay_host -m MODEL -f WINDOWS [-l LOOPS] [-g GPU_LAYERS]\n");
                return 2;
        }
    }
    if (model_path == nullptr || windows == nullptr) {
        fprintf(stderr, "usage: replay_host -m MODEL -f WINDOWS [-l LOOPS] [-g GPU_LAYERS]\n");
        return 2;
    }
    llama_log_set(quiet, nullptr);
    llama_backend_init();
    llama_model_kv_override kv[3];
    llm_dory_overrides(kv, 4);
    llama_model_params mp = llama_model_default_params();
    mp.n_gpu_layers = gpu_layers;
    mp.kv_overrides = kv;
    llama_model *model = llama_model_load_from_file(model_path, mp);
    if (model == nullptr) return 1;
    const llama_vocab *vocab = llama_model_get_vocab(model);

    llama_context_params cp = llama_context_default_params();
    cp.n_ctx = 4096;
    cp.n_batch = PROMPT_CHUNK;
    cp.n_ubatch = PROMPT_CHUNK;
    cp.n_seq_max = 2;
    cp.kv_unified = true;
    llama_context *ctx = llama_init_from_model(model, cp);
    if (ctx == nullptr) return 1;
    llama_memory_t mem = llama_get_memory(ctx);
    llama_batch batch = llama_batch_init(PROMPT_CHUNK, 0, 1);

    std::string prefix, suffix;
    llm_chat_format(model, ta_system_prefix(), &prefix, &suffix);
    std::vector<llama_token> ptoks(prefix.size() + 16);
    const int np = llama_tokenize(vocab, prefix.c_str(), (int32_t)prefix.size(), ptoks.data(), (int32_t)ptoks.size(), true, true);
    llama_pos pos0 = 0;
    llama_set_n_loops(ctx, 4); /* the board decodes the shared prefix at the most loops */
    for (int i = 0; i < np; i += PROMPT_CHUNK) {
        const int n = np - i < PROMPT_CHUNK ? np - i : PROMPT_CHUNK;
        if (decode(ctx, batch, ptoks.data() + i, n, SEQ_PREFIX, &pos0) != 0) return 1;
    }

    FILE *f = fopen(windows, "r");
    if (f == nullptr) {
        perror(windows);
        return 1;
    }
    static char line[512], prompt[8192], grammar[16384];
    while (fgets(line, sizeof line, f) != nullptr) {
        char id[64], kind[16];
        int n = 0;
        if (sscanf(line, "case %63s %15s %d", id, kind, &n) != 3 || n < 1 || n > TA_MAX_COTS) continue;
        tf_facts_t c[TA_MAX_COTS];
        memset(c, 0, sizeof c);
        for (int i = 0; i < n; i++) {
            if (fgets(line, sizeof line, f) == nullptr) break;
            unsigned flags = 0;
            sscanf(line, "%d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %d %u %d", &c[i].cot, &c[i].seconds, &c[i].valid_seconds, &c[i].time_in_target_pct,
                   &c[i].above_95_on_o2_pct, &c[i].below_85_pct, &c[i].desat_episodes, &c[i].longest_desat_s, &c[i].fio2_early_pm, &c[i].fio2_late_pm, &c[i].fio2_max_pm,
                   &c[i].probe_events, &c[i].invalid_pct, &c[i].hold_s, &c[i].fallback_s, &c[i].restarts, &c[i].fallback_now, &c[i].risk_pct, &flags, &c[i].priority);
            c[i].flags = flags;
        }
        const bool handover = strcmp(kind, "handover") == 0;
        /* As in service: with no flag on the ward the model is not asked. */
        int flagged = 0;
        for (int i = 0; i < n; i++) flagged += c[i].flags != 0;
        if (flagged == 0) {
            printf("{\"id\":\"%s\",\"kind\":\"%s\",\"validator\":\"not asked\",\"prompt_tokens\":0,\"generated\":0,\"loops\":0,\"text\":\"\"}\n", id, kind);
            fflush(stdout);
            continue;
        }
        const int pl = handover ? ta_handover_prompt(c, n, prompt, sizeof prompt) : ta_watch_prompt(c, n, nullptr, prompt, sizeof prompt);
        const int gl = handover ? ta_handover_grammar(c, n, grammar, sizeof grammar) : ta_watch_grammar(c, n, grammar, sizeof grammar);
        if (pl <= 0 || gl <= 0) continue;
        const int max_tokens = handover ? ta_handover_max_tokens(flagged) : ta_watch_max_tokens(n);

        std::string text = std::string(prompt) + suffix;
        std::vector<llama_token> toks(text.size() + 16);
        const int nt = llama_tokenize(vocab, text.c_str(), (int32_t)text.size(), toks.data(), (int32_t)toks.size(), false, true);
        llama_sampler *smpl = llama_sampler_init_grammar(vocab, grammar, "root");
        if (nt <= 0 || smpl == nullptr) return 1;

        llama_memory_seq_rm(mem, SEQ_ANSWER, -1, -1);
        llama_memory_seq_cp(mem, SEQ_PREFIX, SEQ_ANSWER, -1, -1);
        llama_pos pos = np;
        llama_set_n_loops(ctx, loops);
        for (int i = 0; i < nt; i += PROMPT_CHUNK) {
            const int k = nt - i < PROMPT_CHUNK ? nt - i : PROMPT_CHUNK;
            if (decode(ctx, batch, toks.data() + i, k, SEQ_ANSWER, &pos) != 0) return 1;
        }
        std::string answer;
        int generated = 0;
        while (generated < max_tokens) {
            llama_token tok = llm_sample_next(ctx, vocab, smpl);
            if (llama_vocab_is_eog(vocab, tok)) break;
            char piece[256];
            const int len = llama_token_to_piece(vocab, tok, piece, sizeof piece, 0, false);
            if (len > 0) answer.append(piece, (size_t)len);
            if (decode(ctx, batch, &tok, 1, SEQ_ANSWER, &pos) != 0) return 1;
            generated++;
        }
        llama_sampler_free(smpl);

        ta_watch_t w;
        const ta_result_t v = handover ? ta_validate_handover(answer.c_str(), c, n, &w) : ta_validate_watch(answer.c_str(), c, n, &w);
        printf("{\"id\":\"%s\",\"kind\":\"%s\",\"validator\":\"%s\",\"prompt_tokens\":%d,\"generated\":%d,\"loops\":%d,\"text\":\"%s\"}\n",
               id, kind, ta_result_name(v), nt, generated, loops, json_text(answer).c_str());
        fflush(stdout);
    }
    fclose(f);
    llama_batch_free(batch);
    llama_free(ctx);
    llama_model_free(model);
    llama_backend_free();
    return 0;
}
