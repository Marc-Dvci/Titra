// AVX2 kernels for Q5_K weights in the 8x8 repacked layout, added to ggml's x86 repack kernels.
//
// A looped model runs its recurrent block once per loop, so its matrix products set the speed of every token. Q5_K had
// no x86 path here: each column took ggml_vec_dot_q5_k_q8_k on its own, decoding its scales and walking its own memory.
// These kernels take eight columns at once from one cache-friendly block. They produce the same sums as
// ggml_gemv_q5_K_8x8_q8_K_generic and ggml_gemm_q5_K_8x8_q8_K_generic, checked by qnx/llm/kernels/test_q5k.cpp.
//
// Layout of block_q5_Kx8 (8 columns of 256 weights, in 8 sub-block pairs of 32 + 32):
//   qs: 16 groups of 64 bytes, one 8-byte chunk per column. A byte holds the low 4 bits of two weights: low nibble for
//       the even sub-block of the pair, high nibble for the odd one.
//   qh: 4 groups of 64 bytes, the same chunks. Bit 2*sb of a byte is the high bit of the even sub-block's weight, and bit
//       2*sb + 1 the odd one's, for sb = 0..3 (the sub-block pair).
//
// A weight is 0..31 and the row value is int8, so maddubs cannot saturate: 2 * 31 * 128 fits in int16. Each 32-bit lane
// sums at most 128 products times a 6-bit scale, about 3.2e7, so it cannot overflow either.

#if defined(__AVX2__)

static inline void q5k_decode_scales(const uint8_t * scales, uint32_t * utmp) {
    const uint32_t kmask1 = 0x3f3f3f3f;
    const uint32_t kmask2 = 0x0f0f0f0f;
    const uint32_t kmask3 = 0x03030303;
    for (int sb = 0; sb < 8; sb++) {
        memcpy(utmp + sb * 4, scales + sb * 12, 12);
        utmp[sb * 4 + 3]      = ((utmp[sb * 4 + 2] >> 4) & kmask2) | (((utmp[sb * 4 + 1] >> 6) & kmask3) << 4);
        const uint32_t uaux_0 = utmp[sb * 4 + 1] & kmask1;
        utmp[sb * 4 + 1]      = (utmp[sb * 4 + 2] & kmask2) | (((utmp[sb * 4 + 0] >> 6) & kmask3) << 4);
        utmp[sb * 4 + 2]      = uaux_0;
        utmp[sb * 4 + 0] &= kmask1;
    }
}

// Eight 8-bit scales become two vectors of 16-bit lanes matching the maddubs output for columns 0-3 and 4-7.
static inline void q5k_scale_vectors(const uint8_t * s, __m256i * lo, __m256i * hi) {
    const __m256i w = _mm256_broadcastsi128_si256(_mm_cvtepu8_epi16(_mm_loadl_epi64((const __m128i *) s)));
    const __m256i mask_lo = _mm256_setr_epi8(0, 1, 0, 1, 0, 1, 0, 1, 2, 3, 2, 3, 2, 3, 2, 3,
                                             4, 5, 4, 5, 4, 5, 4, 5, 6, 7, 6, 7, 6, 7, 6, 7);
    const __m256i mask_hi = _mm256_setr_epi8(8, 9, 8, 9, 8, 9, 8, 9, 10, 11, 10, 11, 10, 11, 10, 11,
                                             12, 13, 12, 13, 12, 13, 12, 13, 14, 15, 14, 15, 14, 15, 14, 15);
    *lo = _mm256_shuffle_epi8(w, mask_lo);
    *hi = _mm256_shuffle_epi8(w, mask_hi);
}

// Sums the two 32-bit lanes of each column: columns 0-3 in lo, 4-7 in hi, result in column order.
static inline __m256i q5k_column_sums(__m256i lo, __m256i hi) {
    const __m256i order = _mm256_setr_epi32(0, 1, 4, 5, 2, 3, 6, 7);
    return _mm256_permutevar8x32_epi32(_mm256_hadd_epi32(lo, hi), order);
}

void ggml_gemv_q5_K_8x8_q8_K(int n, float * GGML_RESTRICT s, size_t bs, const void * GGML_RESTRICT vx, const void * GGML_RESTRICT vy, int nr, int nc) {
    const int nb = n / QK_K;
    assert(n % QK_K == 0);
    assert(nc % 8 == 0);
    UNUSED(bs);
    UNUSED(nr);

    const __m256i m4 = _mm256_set1_epi8(0x0F);
    const __m256i one = _mm256_set1_epi8(1);
    const block_q8_K * a_ptr = (const block_q8_K *) vy;

    for (int x = 0; x < nc / 8; x++) {
        const block_q5_Kx8 * b_ptr = (const block_q5_Kx8 *) vx + (x * nb);
        __m256 sumf = _mm256_setzero_ps();
        __m256 sum_minf = _mm256_setzero_ps();

        for (int l = 0; l < nb; l++) {
            const block_q5_Kx8 * b = &b_ptr[l];
            const block_q8_K * a = &a_ptr[l];
            uint32_t utmp[32];
            q5k_decode_scales(b->scales, utmp);

            __m256i acc_lo = _mm256_setzero_si256();
            __m256i acc_hi = _mm256_setzero_si256();
            const int8_t * aq = a->qs;
            const uint8_t * qs = b->qs;

            for (int sb = 0; sb < 4; sb++) {
                __m256i s0lo, s0hi, s1lo, s1hi;
                q5k_scale_vectors((const uint8_t *) utmp + sb * 32, &s0lo, &s0hi);
                q5k_scale_vectors((const uint8_t *) utmp + sb * 32 + 16, &s1lo, &s1hi);
                for (int c = 0; c < 4; c++) {
                    const __m256i a0 = _mm256_set1_epi64x(*(const int64_t *) (aq + c * 8));
                    const __m256i a1 = _mm256_set1_epi64x(*(const int64_t *) (aq + 32 + c * 8));
                    const __m256i ql = _mm256_loadu_si256((const __m256i *) qs);
                    const __m256i qh = _mm256_loadu_si256((const __m256i *) (qs + 32));
                    const __m256i hl = _mm256_loadu_si256((const __m256i *) (b->qh + c * 64));
                    const __m256i hh = _mm256_loadu_si256((const __m256i *) (b->qh + c * 64 + 32));

                    // Columns 0-3, then 4-7: the even sub-block from the low nibbles, the odd one from the high nibbles.
                    const __m256i vl0 = _mm256_or_si256(_mm256_and_si256(ql, m4),
                                                        _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hl, sb * 2), one), 4));
                    const __m256i vl1 = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(ql, 4), m4),
                                                        _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hl, sb * 2 + 1), one), 4));
                    const __m256i vh0 = _mm256_or_si256(_mm256_and_si256(qh, m4),
                                                        _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hh, sb * 2), one), 4));
                    const __m256i vh1 = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(qh, 4), m4),
                                                        _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hh, sb * 2 + 1), one), 4));

                    acc_lo = _mm256_add_epi32(acc_lo, _mm256_madd_epi16(_mm256_maddubs_epi16(vl0, a0), s0lo));
                    acc_lo = _mm256_add_epi32(acc_lo, _mm256_madd_epi16(_mm256_maddubs_epi16(vl1, a1), s1lo));
                    acc_hi = _mm256_add_epi32(acc_hi, _mm256_madd_epi16(_mm256_maddubs_epi16(vh0, a0), s0hi));
                    acc_hi = _mm256_add_epi32(acc_hi, _mm256_madd_epi16(_mm256_maddubs_epi16(vh1, a1), s1hi));
                    qs += 64;
                }
                aq += 64;
            }

            const __m256 row_d = _mm256_set1_ps(a->d);
            const __m256 col_d = GGML_F32Cx8_LOAD(b->d);
            const __m256 col_dmin = GGML_F32Cx8_LOAD(b->dmin);
            sumf = _mm256_fmadd_ps(_mm256_cvtepi32_ps(q5k_column_sums(acc_lo, acc_hi)), _mm256_mul_ps(col_d, row_d), sumf);

            // The mins: each sub-block's 8 mins times that sub-block's sum of row values.
            __m256i min_acc = _mm256_setzero_si256();
            for (int sb = 0; sb < 8; sb++) {
                const __m256i mins = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *) ((const uint8_t *) utmp + 8 + sb * 16)));
                const __m256i bsum = _mm256_set1_epi32((int) a->bsums[sb * 2] + (int) a->bsums[sb * 2 + 1]);
                min_acc = _mm256_add_epi32(min_acc, _mm256_mullo_epi32(mins, bsum));
            }
            sum_minf = _mm256_fmadd_ps(_mm256_cvtepi32_ps(min_acc), _mm256_mul_ps(col_dmin, row_d), sum_minf);
        }
        _mm256_storeu_ps(s + x * 8, _mm256_sub_ps(sumf, sum_minf));
    }
}

void ggml_gemm_q5_K_8x8_q8_K(int n, float * GGML_RESTRICT s, size_t bs, const void * GGML_RESTRICT vx, const void * GGML_RESTRICT vy, int nr, int nc) {
    const int nb = n / QK_K;
    assert(n % QK_K == 0);
    assert(nr % 4 == 0);
    assert(nc % 8 == 0);

    const __m256i m4 = _mm256_set1_epi8(0x0F);
    const __m256i one = _mm256_set1_epi8(1);
    const int rows = nr / 4;

    for (int y = 0; y < rows; y++) {
        const block_q8_Kx4 * a_ptr = (const block_q8_Kx4 *) vy + (y * nb);
        for (int x = 0; x < nc / 8; x++) {
            const block_q5_Kx8 * b_ptr = (const block_q5_Kx8 *) vx + (x * nb);
            __m256 sumf[4];
            __m256 sum_minf[4];
            for (int r = 0; r < 4; r++) {
                sumf[r] = _mm256_setzero_ps();
                sum_minf[r] = _mm256_setzero_ps();
            }

            for (int l = 0; l < nb; l++) {
                const block_q5_Kx8 * b = &b_ptr[l];
                const block_q8_Kx4 * a = &a_ptr[l];
                uint32_t utmp[32];
                q5k_decode_scales(b->scales, utmp);

                __m256i acc_lo[4], acc_hi[4];
                for (int r = 0; r < 4; r++) {
                    acc_lo[r] = _mm256_setzero_si256();
                    acc_hi[r] = _mm256_setzero_si256();
                }
                const int8_t * aq = a->qs;
                const uint8_t * qs = b->qs;

                for (int sb = 0; sb < 4; sb++) {
                    __m256i s0lo, s0hi, s1lo, s1hi;
                    q5k_scale_vectors((const uint8_t *) utmp + sb * 32, &s0lo, &s0hi);
                    q5k_scale_vectors((const uint8_t *) utmp + sb * 32 + 16, &s1lo, &s1hi);
                    for (int c = 0; c < 4; c++) {
                        const __m256i ql = _mm256_loadu_si256((const __m256i *) qs);
                        const __m256i qh = _mm256_loadu_si256((const __m256i *) (qs + 32));
                        const __m256i hl = _mm256_loadu_si256((const __m256i *) (b->qh + c * 64));
                        const __m256i hh = _mm256_loadu_si256((const __m256i *) (b->qh + c * 64 + 32));
                        const __m256i vl0 = _mm256_or_si256(_mm256_and_si256(ql, m4),
                                                            _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hl, sb * 2), one), 4));
                        const __m256i vl1 = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(ql, 4), m4),
                                                            _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hl, sb * 2 + 1), one), 4));
                        const __m256i vh0 = _mm256_or_si256(_mm256_and_si256(qh, m4),
                                                            _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hh, sb * 2), one), 4));
                        const __m256i vh1 = _mm256_or_si256(_mm256_and_si256(_mm256_srli_epi16(qh, 4), m4),
                                                            _mm256_slli_epi16(_mm256_and_si256(_mm256_srli_epi16(hh, sb * 2 + 1), one), 4));
                        for (int r = 0; r < 4; r++) {
                            // block_q8_Kx4 interleaves its rows 8 bytes at a time.
                            const __m256i a0 = _mm256_set1_epi64x(*(const int64_t *) (aq + c * 32 + r * 8));
                            const __m256i a1 = _mm256_set1_epi64x(*(const int64_t *) (aq + 128 + c * 32 + r * 8));
                            acc_lo[r] = _mm256_add_epi32(acc_lo[r], _mm256_madd_epi16(_mm256_maddubs_epi16(vl0, a0), s0lo));
                            acc_lo[r] = _mm256_add_epi32(acc_lo[r], _mm256_madd_epi16(_mm256_maddubs_epi16(vl1, a1), s1lo));
                            acc_hi[r] = _mm256_add_epi32(acc_hi[r], _mm256_madd_epi16(_mm256_maddubs_epi16(vh0, a0), s0hi));
                            acc_hi[r] = _mm256_add_epi32(acc_hi[r], _mm256_madd_epi16(_mm256_maddubs_epi16(vh1, a1), s1hi));
                        }
                        qs += 64;
                    }
                    aq += 256;
                }

                const __m256 col_d = GGML_F32Cx8_LOAD(b->d);
                const __m256 col_dmin = GGML_F32Cx8_LOAD(b->dmin);
                for (int r = 0; r < 4; r++) {
                    const __m256 row_d = _mm256_set1_ps(a->d[r]);
                    sumf[r] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(q5k_column_sums(acc_lo[r], acc_hi[r])), _mm256_mul_ps(col_d, row_d), sumf[r]);
                    __m256i min_acc = _mm256_setzero_si256();
                    for (int sb = 0; sb < 8; sb++) {
                        const __m256i mins = _mm256_cvtepu8_epi32(_mm_loadl_epi64((const __m128i *) ((const uint8_t *) utmp + 8 + sb * 16)));
                        // ggml_quantize_mat_q8_K_4x8 interleaves the sums four at a time per row.
                        const int16_t * bs_row = a->bsums + (sb * 8) + (r * 4) - ((sb % 2) * 6);
                        const __m256i bsum = _mm256_set1_epi32((int) bs_row[0] + (int) bs_row[1]);
                        min_acc = _mm256_add_epi32(min_acc, _mm256_mullo_epi32(mins, bsum));
                    }
                    sum_minf[r] = _mm256_fmadd_ps(_mm256_cvtepi32_ps(min_acc), _mm256_mul_ps(col_dmin, row_d), sum_minf[r]);
                }
            }
            for (int r = 0; r < 4; r++) {
                _mm256_storeu_ps(s + (y * 4 + r) * bs + x * 8, _mm256_sub_ps(sumf[r], sum_minf[r]));
            }
        }
    }
}

#else

void ggml_gemv_q5_K_8x8_q8_K(int n, float * GGML_RESTRICT s, size_t bs, const void * GGML_RESTRICT vx, const void * GGML_RESTRICT vy, int nr, int nc) {
    ggml_gemv_q5_K_8x8_q8_K_generic(n, s, bs, vx, vy, nr, nc);
}

void ggml_gemm_q5_K_8x8_q8_K(int n, float * GGML_RESTRICT s, size_t bs, const void * GGML_RESTRICT vx, const void * GGML_RESTRICT vy, int nr, int nc) {
    ggml_gemm_q5_K_8x8_q8_K_generic(n, s, bs, vx, vy, nr, nc);
}

#endif
