/* unfilter.c - PNG scanline unfiltering kernels (decoder side).
 *
 * dst may alias src shifted backwards by >= 1 byte (in-place compaction: raw row y lives at y*(stride+1)+1, final
 * row y at y*stride); all loops load before they store and never store past a byte they have not yet loaded.
 * prev == NULL means the first row of an image / pass.
 *
 * Kernels: scalar (any bpp; bpp 1 with register-carried neighbours), SSE2 per-pixel for bpp 3/4, 16-bit-lane
 * kernels for bpp 6/8, two-row wavefronts (two independent dependency chains interleaved) for bpp 1/2/3/4/6/8,
 * and an AVX2 four-row Paeth wavefront for bpp 3/4 (runtime-dispatched).
 */
#include "internal.h"

static inline __attribute__((always_inline)) void unf_scalar_t(int f, uint8_t *restrict dst, const uint8_t *src, const uint8_t *restrict prev, size_t n, const int bpp) {
    size_t i;
    switch (f) {
    case 0:
        memmove(dst, src, n);
        break;
    case 1:
        for (i = 0; i < (size_t)bpp; i++) dst[i] = src[i];
        for (; i < n; i++) dst[i] = src[i] + dst[i - bpp];
        break;
    case 2:
        if (!prev) { memmove(dst, src, n); break; }
        for (i = 0; i < n; i++) dst[i] = src[i] + prev[i];
        break;
    case 3:
        if (!prev) {
            for (i = 0; i < (size_t)bpp; i++) dst[i] = src[i];
            for (; i < n; i++) dst[i] = src[i] + (dst[i - bpp] >> 1);
            break;
        }
        for (i = 0; i < (size_t)bpp; i++) dst[i] = src[i] + (prev[i] >> 1);
        for (; i < n; i++) dst[i] = src[i] + ((dst[i - bpp] + prev[i]) >> 1);
        break;
    case 4:
        if (!prev) {
            for (i = 0; i < (size_t)bpp; i++) dst[i] = src[i];
            for (; i < n; i++) dst[i] = src[i] + dst[i - bpp];
            break;
        }
        for (i = 0; i < (size_t)bpp; i++) dst[i] = src[i] + prev[i];
        for (; i < n; i++) dst[i] = src[i] + paeth_s(dst[i - bpp], prev[i], prev[i - bpp]);
        break;
    }
}

/* bpp 1: keep the left neighbour in a register (the per-byte dependency chain is the bottleneck) */
static void unf_bpp1(int f, uint8_t *restrict dst, const uint8_t *src, const uint8_t *restrict prev, size_t n) {
    size_t i; int a = 0, c = 0;
    if (!prev && (f == 2 || f == 4)) f = f == 2 ? 0 : 1;
    switch (f) {
    case 1: for (i = 0; i < n; i++) { a = (uint8_t)(src[i] + a); dst[i] = (uint8_t)a; } break;
    case 3: if (!prev) { for (i = 0; i < n; i++) { a = (uint8_t)(src[i] + (a >> 1)); dst[i] = (uint8_t)a; } break; }
            for (i = 0; i < n; i++) { a = (uint8_t)(src[i] + ((a + prev[i]) >> 1)); dst[i] = (uint8_t)a; } break;
    case 4: for (i = 0; i < n; i++) { int b = prev[i]; a = (uint8_t)(src[i] + paeth_s(a, b, c)); c = b; dst[i] = (uint8_t)a; } break;
    default: unf_scalar_t(f, dst, src, prev, n, 1);
    }
}
static void unf_scalar(int f, uint8_t *dst, const uint8_t *src, const uint8_t *prev, size_t n, int bpp) {
    switch (bpp) {
    case 1: unf_bpp1(f, dst, src, prev, n); break;
    case 2: unf_scalar_t(f, dst, src, prev, n, 2); break;
    case 3: unf_scalar_t(f, dst, src, prev, n, 3); break;
    case 4: unf_scalar_t(f, dst, src, prev, n, 4); break;
    case 6: unf_scalar_t(f, dst, src, prev, n, 6); break;
    case 8: unf_scalar_t(f, dst, src, prev, n, 8); break;
    default: unf_scalar_t(f, dst, src, prev, n, bpp);
    }
}

#if PF_X86
/* Up: pure vertical add, vectorised (AVX2 clone selected at load time via ifunc). */
__attribute__((target_clones("avx2", "default")))
static void unf_up(uint8_t *dst, const uint8_t *src, const uint8_t *prev, size_t n) {
    size_t i = 0;
    for (; i + 16 <= n; i += 16)
        _mm_storeu_si128((__m128i *)(dst + i), _mm_add_epi8(_mm_loadu_si128((const __m128i *)(src + i)), _mm_loadu_si128((const __m128i *)(prev + i))));
    for (; i < n; i++) dst[i] = src[i] + prev[i];
}
/* Per-pixel SIMD for 3/4 bytes per pixel: pixels live in the low 3/4 lanes. 4-byte loads/stores (safe: see above;
 * the buffer has >= 16 bytes of tail padding). */
#define PX_LOOP_BEGIN(bpp) size_t i = 0; for (; i + 4 <= n + (bpp == 3); i += bpp) {
static void unf_sub_px(uint8_t *dst, const uint8_t *src, size_t n, int bpp) {
    __m128i a = _mm_setzero_si128();
    PX_LOOP_BEGIN(bpp)
        a = _mm_add_epi8(a, _mm_cvtsi32_si128(ld32(src + i)));
        st32(dst + i, _mm_cvtsi128_si32(a));
    }
    (void)i;
}
static void unf_avg_px(uint8_t *dst, const uint8_t *src, const uint8_t *prev, size_t n, int bpp) {
    const __m128i one = _mm_set1_epi8(1);
    __m128i a = _mm_setzero_si128();
    PX_LOOP_BEGIN(bpp)
        __m128i b = _mm_cvtsi32_si128(ld32(prev + i)), x = _mm_cvtsi32_si128(ld32(src + i));
        __m128i avg = _mm_sub_epi8(_mm_avg_epu8(a, b), _mm_and_si128(_mm_xor_si128(a, b), one));   /* floor((a+b)/2) */
        a = _mm_add_epi8(x, avg);
        st32(dst + i, _mm_cvtsi128_si32(a));
    }
    (void)i;
}
static void unf_paeth_px(uint8_t *dst, const uint8_t *src, const uint8_t *prev, size_t n, int bpp) {
    const __m128i z = _mm_setzero_si128(), ff = _mm_set1_epi16(0xff);
    __m128i a = z, c = z;
    PX_LOOP_BEGIN(bpp)
        __m128i b = _mm_unpacklo_epi8(_mm_cvtsi32_si128(ld32(prev + i)), z);
        __m128i x = _mm_unpacklo_epi8(_mm_cvtsi32_si128(ld32(src + i)), z);
        __m128i c3b = _mm_sub_epi16(_mm_add_epi16(c, _mm_add_epi16(c, c)), b);   /* off the a-chain */
        __m128i th = _mm_sub_epi16(c3b, a);
        __m128i lo = _mm_min_epi16(a, b), hi = _mm_max_epi16(a, b);
        __m128i m1 = _mm_cmpgt_epi16(hi, th);
        __m128i t0 = _mm_or_si128(_mm_and_si128(m1, c), _mm_andnot_si128(m1, lo));
        __m128i m2 = _mm_cmpgt_epi16(th, lo);
        __m128i t1 = _mm_or_si128(_mm_and_si128(m2, t0), _mm_andnot_si128(m2, hi));
        a = _mm_and_si128(_mm_add_epi16(x, t1), ff);
        st32(dst + i, _mm_cvtsi128_si32(_mm_packus_epi16(a, a)));
        c = b;
    }
    (void)i;
}
/* Two-row wavefront for any pair of {Sub, Avg, Paeth} rows: row y+1 pixel k-1 depends on row y pixels k-1 and k-2
 * (b, c), which are handed over in registers, so the two serial per-pixel dependency chains run interleaved (ILP).
 * All arithmetic in 16-bit lanes (pixel in lanes 0..bpp-1). Raw data sits >= 2 output rows ahead (see unfilter_all),
 * so 4-byte stores past a row end only touch not-yet-written output. */
static inline __m128i paeth_step(__m128i a, __m128i b, __m128i c, __m128i x) {
    const __m128i ff = _mm_set1_epi16(0xff);
    __m128i th = _mm_sub_epi16(_mm_sub_epi16(_mm_add_epi16(c, _mm_add_epi16(c, c)), b), a);
    __m128i lo = _mm_min_epi16(a, b), hi = _mm_max_epi16(a, b);
    __m128i m1 = _mm_cmpgt_epi16(hi, th), m2 = _mm_cmpgt_epi16(th, lo);
    __m128i t0 = _mm_or_si128(_mm_and_si128(m1, c), _mm_andnot_si128(m1, lo));
    __m128i t1 = _mm_or_si128(_mm_and_si128(m2, t0), _mm_andnot_si128(m2, hi));
    return _mm_and_si128(_mm_add_epi16(x, t1), ff);
}
#define STEP(F, a, b, c, x) ((F) == 1 ? _mm_and_si128(_mm_add_epi16(x, a), ff) :                                   \
                             (F) == 3 ? _mm_and_si128(_mm_add_epi16(x, _mm_srli_epi16(_mm_add_epi16(a, b), 1)), ff) : \
                             paeth_step(a, b, c, x))
#define LD16(p) _mm_unpacklo_epi8(_mm_cvtsi32_si128(ld32(p)), z)
#define ST16(p, v) st32(p, _mm_cvtsi128_si32(_mm_packus_epi16(v, v)))
#define DEF_PAIR(F0, F1)                                                                                          \
static void pair_##F0##F1(uint8_t *d0, const uint8_t *s0, const uint8_t *p0, uint8_t *d1, const uint8_t *s1,      \
                          size_t n, int bpp) {                                                                    \
    const __m128i z = _mm_setzero_si128(), ff = _mm_set1_epi16(0xff);                                             \
    __m128i a0 = z, c0 = z, a1 = z, c1 = z, b1 = z;                                                               \
    __m128i r0 = paeth_step(z, LD16(p0), z, LD16(s0)); /* pixel 0 of row 0: every filter reduces to x + f(0,b,0) */ \
    if ((F0) == 1) r0 = _mm_and_si128(LD16(s0), ff);                                                              \
    else if ((F0) == 3) r0 = _mm_and_si128(_mm_add_epi16(LD16(s0), _mm_srli_epi16(LD16(p0), 1)), ff);            \
    ST16(d0, r0); a0 = r0; c0 = LD16(p0); b1 = r0;                                                                \
    size_t i = bpp;                                                                                               \
    for (; i < n; i += bpp) {                                                                                     \
        __m128i b = LD16(p0 + i), x = LD16(s0 + i);                                                               \
        __m128i x1 = LD16(s1 + i - bpp);                                                                          \
        r0 = STEP(F0, a0, b, c0, x);                                                                              \
        a1 = STEP(F1, a1, b1, c1, x1);                                                                            \
        c1 = b1; b1 = r0;                                                                                         \
        if (i + bpp < n) ST16(d0 + i, r0); else { uint32_t v = _mm_cvtsi128_si32(_mm_packus_epi16(r0, r0)); memcpy(d0 + i, &v, bpp); } \
        ST16(d1 + i - bpp, a1);                                                                                   \
        a0 = r0; c0 = b;                                                                                          \
    }                                                                                                             \
    a1 = STEP(F1, a1, b1, c1, LD16(s1 + n - bpp));                                                                \
    ST16(d1 + n - bpp, a1);                                                                                       \
}
DEF_PAIR(1, 1) DEF_PAIR(1, 3) DEF_PAIR(1, 4) DEF_PAIR(3, 1) DEF_PAIR(3, 3) DEF_PAIR(3, 4) DEF_PAIR(4, 1) DEF_PAIR(4, 3) DEF_PAIR(4, 4)
static pf_pair_fn pair_for(int f0, int f1) {
    static const pf_pair_fn t[5][5] = { [1] = { [1] = pair_11, [3] = pair_13, [4] = pair_14 },
                                     [3] = { [1] = pair_31, [3] = pair_33, [4] = pair_34 },
                                     [4] = { [1] = pair_41, [3] = pair_43, [4] = pair_44 } };
    return (f0 < 5 && f1 < 5) ? t[f0][f1] : NULL;
}

/* ---- v3: 6/8-byte pixels (16-bit RGB/RGBA): same 16-bit-lane kernels with 8-byte loads/stores ---- */
#define LDW(p) _mm_unpacklo_epi8(_mm_loadl_epi64((const __m128i *)(const void *)(p)), z)
#define STW(p, v) _mm_storel_epi64((__m128i *)(void *)(p), _mm_packus_epi16(v, v))
#define STW_LAST(p, v, bpp) do { uint8_t t_[16]; _mm_storeu_si128((__m128i *)t_, _mm_packus_epi16(v, v)); memcpy(p, t_, bpp); } while (0)
#define DEF_PAIRW(F0, F1)                                                                                         \
static void pairw_##F0##F1(uint8_t *d0, const uint8_t *s0, const uint8_t *p0, uint8_t *d1, const uint8_t *s1,     \
                           size_t n, int bpp) {                                                                   \
    const __m128i z = _mm_setzero_si128(), ff = _mm_set1_epi16(0xff);                                             \
    __m128i a0 = z, c0 = z, a1 = z, c1 = z, b1 = z;                                                               \
    __m128i r0 = paeth_step(z, LDW(p0), z, LDW(s0));                                                              \
    if ((F0) == 1) r0 = _mm_and_si128(LDW(s0), ff);                                                               \
    else if ((F0) == 3) r0 = _mm_and_si128(_mm_add_epi16(LDW(s0), _mm_srli_epi16(LDW(p0), 1)), ff);              \
    STW(d0, r0); a0 = r0; c0 = LDW(p0); b1 = r0;                                                                  \
    size_t i = bpp;                                                                                               \
    for (; i < n; i += bpp) {                                                                                     \
        __m128i b = LDW(p0 + i), x = LDW(s0 + i);                                                                 \
        __m128i x1 = LDW(s1 + i - bpp);                                                                           \
        r0 = STEP(F0, a0, b, c0, x);                                                                              \
        a1 = STEP(F1, a1, b1, c1, x1);                                                                            \
        c1 = b1; b1 = r0;                                                                                         \
        if (i + bpp < n) STW(d0 + i, r0); else STW_LAST(d0 + i, r0, bpp);                                        \
        STW(d1 + i - bpp, a1);                                                                                    \
        a0 = r0; c0 = b;                                                                                          \
    }                                                                                                             \
    a1 = STEP(F1, a1, b1, c1, LDW(s1 + n - bpp));                                                                 \
    STW_LAST(d1 + n - bpp, a1, bpp);                                                                              \
}
DEF_PAIRW(1, 1) DEF_PAIRW(1, 3) DEF_PAIRW(1, 4) DEF_PAIRW(3, 1) DEF_PAIRW(3, 3) DEF_PAIRW(3, 4) DEF_PAIRW(4, 1) DEF_PAIRW(4, 3) DEF_PAIRW(4, 4)
#endif /* PF_X86 */
/* bpp 1/2: scalar two-row wavefront (row y+1 lags row y by one pixel) -> two independent dependency chains */
static inline __attribute__((always_inline)) int pred_s(int F, int a, int b, int c) {
    return F == 1 ? a : F == 2 ? b : F == 3 ? (a + b) >> 1 : paeth_s(a, b, c);
}
#define DEF_PAIRS(B, F0, F1)                                                                                      \
static void pairs##B##_##F0##F1(uint8_t *restrict d0, const uint8_t *restrict s0, const uint8_t *restrict p0,     \
                                uint8_t *restrict d1, const uint8_t *restrict s1, size_t n, int bpp) {            \
    (void)bpp; size_t i;                                                                                          \
    for (i = 0; i < B; i++) { d0[i] = s0[i] + pred_s(F0, 0, p0[i], 0); d1[i] = s1[i] + pred_s(F1, 0, d0[i], 0); } \
    if (B == 1) {                                                                                                 \
        int a0 = d0[0], c0 = p0[0], a1 = d1[0], c1 = d0[0];                                                       \
        for (; i < n; i++) {                                                                                      \
            int b0 = p0[i]; a0 = (uint8_t)(s0[i] + pred_s(F0, a0, b0, c0)); c0 = b0; d0[i] = (uint8_t)a0;        \
            a1 = (uint8_t)(s1[i] + pred_s(F1, a1, a0, c1)); c1 = a0; d1[i] = (uint8_t)a1;                        \
        }                                                                                                         \
    } else for (; i < n; i++) {                                                                                   \
        d0[i] = s0[i] + pred_s(F0, d0[i - B], p0[i], p0[i - B]);                                                  \
        d1[i] = s1[i] + pred_s(F1, d1[i - B], d0[i], d0[i - B]);                                                  \
    }                                                                                                             \
}
#define DEF_PAIRS_B(B) DEF_PAIRS(B,1,1) DEF_PAIRS(B,1,3) DEF_PAIRS(B,1,4) DEF_PAIRS(B,3,1) DEF_PAIRS(B,3,3) \
                       DEF_PAIRS(B,3,4) DEF_PAIRS(B,4,1) DEF_PAIRS(B,4,3) DEF_PAIRS(B,4,4)
DEF_PAIRS_B(1) DEF_PAIRS_B(2)
#if PF_X86
pf_pair_fn pf__pair_for_bpp(int f0, int f1, int bpp) {
    static const pf_pair_fn t[5][5] = { [1] = { [1] = pairw_11, [3] = pairw_13, [4] = pairw_14 },
                                     [3] = { [1] = pairw_31, [3] = pairw_33, [4] = pairw_34 },
                                     [4] = { [1] = pairw_41, [3] = pairw_43, [4] = pairw_44 } };
    static const pf_pair_fn t1[5][5] = { [1] = { [1] = pairs1_11, [3] = pairs1_13, [4] = pairs1_14 },
                                      [3] = { [1] = pairs1_31, [3] = pairs1_33, [4] = pairs1_34 },
                                      [4] = { [1] = pairs1_41, [3] = pairs1_43, [4] = pairs1_44 } };
    static const pf_pair_fn t2[5][5] = { [1] = { [1] = pairs2_11, [3] = pairs2_13, [4] = pairs2_14 },
                                      [3] = { [1] = pairs2_31, [3] = pairs2_33, [4] = pairs2_34 },
                                      [4] = { [1] = pairs2_41, [3] = pairs2_43, [4] = pairs2_44 } };
    if (bpp == 1) return (f0 < 5 && f1 < 5) ? t1[f0][f1] : NULL;
    if (bpp == 2) return (f0 < 5 && f1 < 5) ? t2[f0][f1] : NULL;
    if (bpp <= 4) return pair_for(f0, f1);
    return (f0 < 5 && f1 < 5) ? t[f0][f1] : NULL;
}
#else
/* portable: scalar two-row wavefront for every pixel size */
DEF_PAIRS_B(3) DEF_PAIRS_B(4) DEF_PAIRS_B(6) DEF_PAIRS_B(8)
#define PAIR_TABLE(B) { [1] = { [1] = pairs##B##_11, [3] = pairs##B##_13, [4] = pairs##B##_14 }, \
                        [3] = { [1] = pairs##B##_31, [3] = pairs##B##_33, [4] = pairs##B##_34 }, \
                        [4] = { [1] = pairs##B##_41, [3] = pairs##B##_43, [4] = pairs##B##_44 } }
pf_pair_fn pf__pair_for_bpp(int f0, int f1, int bpp) {
    static const pf_pair_fn t[9][5][5] = { [1] = PAIR_TABLE(1), [2] = PAIR_TABLE(2), [3] = PAIR_TABLE(3),
                                           [4] = PAIR_TABLE(4), [6] = PAIR_TABLE(6), [8] = PAIR_TABLE(8) };
    return (bpp >= 1 && bpp <= 8 && f0 < 5 && f1 < 5) ? t[bpp][f0][f1] : NULL;
}
#undef PAIR_TABLE
#endif
#if PF_X86
#define DEF_ROWW(F)                                                                                               \
static void roww_##F(uint8_t *d, const uint8_t *s, const uint8_t *p, size_t n, int bpp) {                         \
    const __m128i z = _mm_setzero_si128(), ff = _mm_set1_epi16(0xff);                                             \
    __m128i a = z, c = z;                                                                                         \
    for (size_t i = 0; i < n; i += bpp) {                                                                         \
        __m128i b = (F) == 1 ? z : LDW(p + i), x = LDW(s + i);                                                    \
        a = STEP(F, a, b, c, x); c = b;                                                                           \
        if (i + bpp < n) STW(d + i, a); else STW_LAST(d + i, a, bpp);                                            \
    }                                                                                                             \
}
DEF_ROWW(1) DEF_ROWW(3) DEF_ROWW(4)

#endif /* PF_X86 */

void pf__unfilter_row(int f, uint8_t *dst, const uint8_t *src, const uint8_t *prev, size_t n, int bpp) {
#if !PF_X86
    if (f == 0 || (f == 2 && !prev)) memmove(dst, src, n);
    else unf_scalar(f, dst, src, prev, n, bpp);
#else
    if ((bpp == 6 || bpp == 8) && n >= 16) {
        switch (f) {
        case 0: memmove(dst, src, n); return;
        case 1: roww_1(dst, src, NULL, n, bpp); return;
        case 2: if (prev) unf_up(dst, src, prev, n); else memmove(dst, src, n); return;
        case 3: if (prev) roww_3(dst, src, prev, n, bpp); else unf_scalar(3, dst, src, prev, n, bpp); return;
        case 4: if (prev) roww_4(dst, src, prev, n, bpp); else roww_1(dst, src, NULL, n, bpp); return;
        }
    }
    if ((bpp != 3 && bpp != 4) || n < 8) { unf_scalar(f, dst, src, prev, n, bpp); return; }
    switch (f) {
    case 0: memmove(dst, src, n); break;
    case 1: unf_sub_px(dst, src, n, bpp); break;
    case 2: if (prev) unf_up(dst, src, prev, n); else memmove(dst, src, n); break;
    case 3: if (prev) unf_avg_px(dst, src, prev, n, bpp); else unf_scalar(3, dst, src, prev, n, bpp); break;
    case 4: if (prev) unf_paeth_px(dst, src, prev, n, bpp); else unf_sub_px(dst, src, n, bpp); break;
    }
#endif
}

#if PF_X86
#define LDX(p) _mm_unpacklo_epi8(_mm_cvtsi32_si128(ld32(p)), _mm_setzero_si128())
/* Four consecutive Paeth rows as a wavefront: row g lags row g-1 by one pixel; 4 x 16-bit-lane pixels per AVX2 vector. */
__attribute__((target("avx2"))) void pf__quad_paeth_avx2(uint8_t *d0, const uint8_t *s0, const uint8_t *p0, size_t stride,
                                                 size_t rs, size_t n, int bpp) {
    const __m256i ff = _mm256_set1_epi16(0xff), z = _mm256_setzero_si256();

    const long npx = (long)(n / bpp);
    __m256i r = z, bprev = z;
    for (long k = 0; k < npx + 3; k++) {
        const int edge = k < 3 || k >= npx;
        __m128i xs[4];
        for (int g = 0; g < 4; g++) { long j = k - g;
            xs[g] = (j >= 0 && j < npx) ? LDX(s0 + g * rs + j * bpp) : _mm_setzero_si128(); }
        __m256i x = _mm256_set_m128i(_mm_unpacklo_epi64(xs[2], xs[3]), _mm_unpacklo_epi64(xs[0], xs[1]));
        __m256i pv = _mm256_zextsi128_si256(k < npx ? LDX(p0 + k * bpp) : _mm_setzero_si128());
        __m256i a = r, c = bprev;
        __m256i b = _mm256_blend_epi32(_mm256_permute4x64_epi64(r, _MM_SHUFFLE(2, 1, 0, 3)), pv, 0x03);
        bprev = b;
        __m256i th = _mm256_sub_epi16(_mm256_sub_epi16(_mm256_add_epi16(c, _mm256_add_epi16(c, c)), b), a);
        __m256i lo = _mm256_min_epi16(a, b), hi = _mm256_max_epi16(a, b);
        __m256i m1 = _mm256_cmpgt_epi16(hi, th), m2 = _mm256_cmpgt_epi16(th, lo);
        __m256i t0 = _mm256_blendv_epi8(lo, c, m1);
        __m256i pred = _mm256_blendv_epi8(hi, t0, m2);
        r = _mm256_and_si256(_mm256_add_epi16(x, pred), ff);
        if (__builtin_expect(edge, 0)) {   /* zero groups outside [0, npx) so they start from a = c = 0 */
            int64_t vm[4]; for (int g = 0; g < 4; g++) vm[g] = -(int64_t)(k - g >= 0 && k - g < npx);
            r = _mm256_and_si256(r, _mm256_loadu_si256((const __m256i *)vm));
        }
        __m256i pk = _mm256_packus_epi16(r, r);
        __m128i plo = _mm256_castsi256_si128(pk), phi = _mm256_extracti128_si256(pk, 1);
        uint32_t o[4] = { (uint32_t)_mm_cvtsi128_si32(plo), (uint32_t)_mm_extract_epi32(plo, 1),
                          (uint32_t)_mm_cvtsi128_si32(phi), (uint32_t)_mm_extract_epi32(phi, 1) };
        for (int g = 0; g < 4; g++) { long j = k - g;
            if (j >= 0 && j < npx) { uint8_t *dp = d0 + g * stride + j * bpp;
                if (j + 1 < npx) st32(dp, o[g]); else memcpy(dp, &o[g], bpp); } }
    }
}
#endif /* PF_X86 */
