// The AVX2 Q5_K kernels against ggml's generic ones: the same blocks must give the same products. Run on the host:
//   qnx/llm/kernels/run_test.sh
// Random weights, scales and row values; every column count and row count the code paths take; and the extreme values
// that would saturate a careless kernel.
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <random>
#include <vector>

#define GGML_COMMON_DECL_CPP
#include "ggml.h"
#include "ggml-cpu.h"
#include "ggml-common.h"
#include "ggml-cpu-impl.h"
#include "repack.h"
#include "traits.h"

static std::mt19937 rng(20261106);

enum fill_t { RANDOM, ALL_MAX, ALL_ZERO };

static uint8_t pick(int bound, fill_t mode) {
    if (mode == ALL_MAX) return (uint8_t)(bound - 1);
    if (mode == ALL_ZERO) return 0;
    return (uint8_t)(rng() % (unsigned)bound);
}

static void fill_block(block_q5_Kx8 *b, fill_t mode) {
    for (int i = 0; i < 8; i++) {
        b->d[i] = ggml_fp32_to_fp16(mode == RANDOM ? (float)(rng() % 1000 + 1) * 1e-4f : 0.05f);
        b->dmin[i] = ggml_fp32_to_fp16(mode == RANDOM ? (float)(rng() % 1000 + 1) * 1e-4f : 0.02f);
    }
    for (auto &v : b->scales) v = pick(256, mode);
    for (auto &v : b->qh) v = pick(256, mode);
    for (auto &v : b->qs) v = pick(256, mode);
}

static int8_t pick_row(fill_t mode) {
    if (mode == ALL_MAX) return -128;
    if (mode == ALL_ZERO) return 0;
    return (int8_t)(int)(rng() % 255 - 127);
}

static void fill_row(block_q8_K *a, fill_t mode) {
    a->d = mode == RANDOM ? (float)(rng() % 1000 + 1) * 1e-3f : 0.5f;
    for (auto &v : a->qs) v = pick_row(mode);
    for (int i = 0; i < 16; i++) {
        int sum = 0;
        for (int k = 0; k < 16; k++) sum += a->qs[i * 16 + k];
        a->bsums[i] = (int16_t)sum;
    }
}

// Four rows through ggml's own quantizer, so the layout under test is the one the real code path produces.
static void fill_rows4(block_q8_Kx4 *a, int count, fill_t mode) {
    std::vector<float> x((size_t)count * 4 * QK_K);
    for (auto &v : x) {
        v = mode == ALL_ZERO ? 0.0f : mode == ALL_MAX ? -1.0f : (float)((int)(rng() % 2001) - 1000) * 1e-3f;
    }
    ggml_quantize_mat_q8_K_4x8_generic(x.data(), a, (int64_t)count * QK_K);
}

static int compare(const std::vector<float> &x, const std::vector<float> &y, const char *what) {
    int bad = 0;
    for (size_t i = 0; i < x.size(); i++) {
        const float tol = 1e-3f * (fabsf(y[i]) + 1.0f);
        if (fabsf(x[i] - y[i]) > tol) {
            if (bad < 4) fprintf(stderr, "%s[%zu]: avx2 %g generic %g\n", what, i, (double)x[i], (double)y[i]);
            bad++;
        }
    }
    return bad;
}

int main() {
    ggml_cpu_init(); // fills the FP16 table the generic kernels read
    int failed = 0, checks = 0;
    for (fill_t mode : {RANDOM, ALL_MAX, ALL_ZERO}) {
        for (int nb : {1, 2, 3}) {
            for (int groups : {1, 2}) {
                const int nc = groups * 8;
                const int n = nb * QK_K;
                const int rounds = mode == RANDOM ? 50 : 1;
                for (int round = 0; round < rounds; round++) {
                    std::vector<block_q5_Kx8> weights((size_t)(groups * nb));
                    for (auto &b : weights) fill_block(&b, mode);

                    std::vector<block_q8_K> row((size_t)nb);
                    for (auto &a : row) fill_row(&a, mode);
                    std::vector<float> sa((size_t)nc), sb((size_t)nc);
                    ggml_gemv_q5_K_8x8_q8_K(n, sa.data(), 0, weights.data(), row.data(), 1, nc);
                    ggml_gemv_q5_K_8x8_q8_K_generic(n, sb.data(), 0, weights.data(), row.data(), 1, nc);
                    failed += compare(sa, sb, "gemv");
                    checks += nc;

                    for (int rows : {4, 8}) {
                        const int bs = nc;
                        // ggml's kernels read the row blocks of one row group nb blocks apart.
                        std::vector<block_q8_Kx4> rows4((size_t)(nb * rows / 4));
                        for (int g = 0; g < rows / 4; g++) fill_rows4(&rows4[(size_t)(g * nb)], nb, mode);
                        std::vector<float> ma((size_t)(rows * nc)), mb((size_t)(rows * nc));
                        ggml_gemm_q5_K_8x8_q8_K(n, ma.data(), (size_t)bs, weights.data(), rows4.data(), rows, nc);
                        ggml_gemm_q5_K_8x8_q8_K_generic(n, mb.data(), (size_t)bs, weights.data(), rows4.data(), rows, nc);
                        failed += compare(ma, mb, "gemm");
                        checks += rows * nc;
                    }
                }
            }
        }
    }
    printf("%d products compared with the generic kernels, %d differ\n", checks, failed);
    return failed ? 1 : 0;
}
