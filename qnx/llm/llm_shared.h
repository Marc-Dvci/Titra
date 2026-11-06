/*
 * How a token is chosen and how a request is wrapped in the model's chat format.
 */
#ifndef TITRA_LLM_SHARED_H
#define TITRA_LLM_SHARED_H

#include <math.h>
#include <string.h>

#include <string>
#include <vector>

#include "llama.h"

/* Greedy, with the grammar checked on the best token first and applied to the whole vocabulary only when that token is
 * refused: the full pass is the slow path and most tokens never need it. */
static inline llama_token llm_sample_next(llama_context *ctx, const llama_vocab *vocab, llama_sampler *grammar) {
    const float *logits = llama_get_logits_ith(ctx, -1);
    const int n_vocab = llama_vocab_n_tokens(vocab);
    int best = 0;
    for (int i = 1; i < n_vocab; i++) {
        if (logits[i] > logits[best]) best = i;
    }
    if (grammar != nullptr) {
        llama_token_data one = {best, logits[best], 0.0f};
        llama_token_data_array single = {&one, 1, -1, false};
        llama_sampler_apply(grammar, &single);
        if (std::isinf(single.data[0].logit)) {
            std::vector<llama_token_data> all((size_t)n_vocab);
            for (int i = 0; i < n_vocab; i++) all[(size_t)i] = {i, logits[i], 0.0f};
            llama_token_data_array arr = {all.data(), all.size(), -1, false};
            llama_sampler_apply(grammar, &arr);
            float top = -INFINITY;
            best = -1;
            for (size_t i = 0; i < arr.size; i++) {
                if (!std::isinf(arr.data[i].logit) && arr.data[i].logit > top) {
                    top = arr.data[i].logit;
                    best = arr.data[i].id;
                }
            }
            if (best < 0) return llama_vocab_eos(vocab);
        }
        llama_sampler_accept(grammar, best);
    }
    return best;
}

/* The shared start of every prompt and what follows each request's text, in the model's own chat format. */
static inline void llm_chat_format(const llama_model *model, const char *system_text, std::string *prefix, std::string *suffix) {
    const char *tmpl = llama_model_chat_template(model, nullptr);
    const bool chatml = tmpl != nullptr && strstr(tmpl, "<|im_start|>") != nullptr;
    const bool thinks = tmpl != nullptr && strstr(tmpl, "<think>") != nullptr;
    if (chatml || tmpl == nullptr) {
        *prefix = std::string("<|im_start|>system\n") + system_text + "<|im_end|>\n<|im_start|>user\n";
        *suffix = std::string("<|im_end|>\n<|im_start|>assistant\n") + (thinks ? "<think></think>" : "");
        return;
    }
    static const char MARK[] = "@@TITRA_REQUEST@@";
    llama_chat_message msgs[2] = {{"system", system_text}, {"user", MARK}};
    std::vector<char> out(16384);
    const int n = llama_chat_apply_template(tmpl, msgs, 2, true, out.data(), (int32_t)out.size());
    if (n > 0 && n < (int)out.size()) {
        std::string all(out.data(), (size_t)n);
        const size_t at = all.find(MARK);
        if (at != std::string::npos) {
            *prefix = all.substr(0, at);
            *suffix = all.substr(at + strlen(MARK));
            return;
        }
    }
    *prefix = std::string(system_text) + "\n";
    *suffix = "\n";
}

/* Loads a looped model with all four loops in its layer table and one KV slot per physical recurrent layer. */
static inline void llm_dory_overrides(llama_model_kv_override *kv, int max_loops) {
    memset(kv, 0, sizeof(llama_model_kv_override) * 3);
    snprintf(kv[0].key, sizeof kv[0].key, "dory.num_loops");
    kv[0].tag = LLAMA_KV_OVERRIDE_TYPE_INT;
    kv[0].val_i64 = max_loops;
    snprintf(kv[1].key, sizeof kv[1].key, "dory.recurrent_kv_cache_mode");
    kv[1].tag = LLAMA_KV_OVERRIDE_TYPE_STR;
    snprintf(kv[1].val_str, sizeof kv[1].val_str, "last_loop");
}

#endif
