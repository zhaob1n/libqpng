/* filter_enc.c - forward scanline filtering for the encoder.
 *
 * Fixed filters: one SIMD pass per row. Adaptive (libpng's heuristic: per row, the filter with the minimum sum of
 * |signed residual|): one fused SIMD pass computes the five costs without storing anything (residuals stay in
 * registers), then the winning filter is written straight into the output with a second pass over the (L1-hot) row.
 * Row 0 is filtered against a zero row, which is exactly the PNG definition (Up = None, Paeth = Sub there).
 * AVX2 (32 bytes per step) is selected at run time; SSE2 is the baseline.
 */
#include "internal.h"

/* ---- predictors on 16 / 32 independent bytes ---- */
static inline __m128i paeth_v16(__m128i a, __m128i b, __m128i c) {
    const __m128i z = _mm_setzero_si128();
    __m128i r[2];
    for (int h = 0; h < 2; h++) {
        __m128i A = h ? _mm_unpackhi_epi8(a, z) : _mm_unpacklo_epi8(a, z);
        __m128i B = h ? _mm_unpackhi_epi8(b, z) : _mm_unpacklo_epi8(b, z);
        __m128i C = h ? _mm_unpackhi_epi8(c, z) : _mm_unpacklo_epi8(c, z);
        __m128i th = _mm_sub_epi16(_mm_add_epi16(C, _mm_add_epi16(C, C)), _mm_add_epi16(A, B));
        __m128i lo = _mm_min_epi16(A, B), hi = _mm_max_epi16(A, B);
        __m128i m1 = _mm_cmpgt_epi16(hi, th), m2 = _mm_cmpgt_epi16(th, lo);
        __m128i t0 = _mm_or_si128(_mm_and_si128(m1, C), _mm_andnot_si128(m1, lo));
        r[h] = _mm_or_si128(_mm_and_si128(m2, t0), _mm_andnot_si128(m2, hi));
    }
    return _mm_packus_epi16(r[0], r[1]);
}
static inline __m128i avg_v16(__m128i a, __m128i b) {   /* floor((a + b) / 2) */
    return _mm_sub_epi8(_mm_avg_epu8(a, b), _mm_and_si128(_mm_xor_si128(a, b), _mm_set1_epi8(1)));
}
__attribute__((target("avx2"))) static inline __m256i paeth_v32(__m256i a, __m256i b, __m256i c) {
    const __m256i z = _mm256_setzero_si256();
    __m256i r[2];
    for (int h = 0; h < 2; h++) {
        __m256i A = h ? _mm256_unpackhi_epi8(a, z) : _mm256_unpacklo_epi8(a, z);
        __m256i B = h ? _mm256_unpackhi_epi8(b, z) : _mm256_unpacklo_epi8(b, z);
        __m256i C = h ? _mm256_unpackhi_epi8(c, z) : _mm256_unpacklo_epi8(c, z);
        __m256i th = _mm256_sub_epi16(_mm256_add_epi16(C, _mm256_add_epi16(C, C)), _mm256_add_epi16(A, B));
        __m256i lo = _mm256_min_epi16(A, B), hi = _mm256_max_epi16(A, B);
        __m256i t0 = _mm256_blendv_epi8(lo, C, _mm256_cmpgt_epi16(hi, th));
        r[h] = _mm256_blendv_epi8(hi, t0, _mm256_cmpgt_epi16(th, lo));
    }
    return _mm256_packus_epi16(r[0], r[1]);   /* lane-wise unpack/pack round-trips */
}
__attribute__((target("avx2"))) static inline __m256i avg_v32(__m256i a, __m256i b) {
    return _mm256_sub_epi8(_mm256_avg_epu8(a, b), _mm256_and_si256(_mm256_xor_si256(a, b), _mm256_set1_epi8(1)));
}

static inline int pred_scalar(int f, int a, int b, int c) {
    switch (f) { case 0: return 0; case 1: return a; case 2: return b; case 3: return (a + b) >> 1; default: return paeth_s(a, b, c); }
}
static inline int absres(int r) { int v = (int8_t)r; return v < 0 ? -v : v; }

/* ---- write one filtered row ---- */
static void filt_row_sse2(int f, uint8_t *out, const uint8_t *cur, const uint8_t *prev, size_t n, int bpp) {
    size_t i = 0;
    if (f == 0) { memcpy(out, cur, n); return; }
    for (; i < (size_t)bpp && i < n; i++) out[i] = (uint8_t)(cur[i] - pred_scalar(f, 0, prev[i], 0));
    for (; i + 16 <= n; i += 16) {
        __m128i x = _mm_loadu_si128((const __m128i *)(cur + i)), a = _mm_loadu_si128((const __m128i *)(cur + i - bpp));
        __m128i b = _mm_loadu_si128((const __m128i *)(prev + i)), p;
        switch (f) {
        case 1: p = a; break;
        case 2: p = b; break;
        case 3: p = avg_v16(a, b); break;
        default: p = paeth_v16(a, b, _mm_loadu_si128((const __m128i *)(prev + i - bpp))); break;
        }
        _mm_storeu_si128((__m128i *)(out + i), _mm_sub_epi8(x, p));
    }
    for (; i < n; i++) out[i] = (uint8_t)(cur[i] - pred_scalar(f, cur[i - bpp], prev[i], prev[i - bpp]));
}
__attribute__((target("avx2"))) static void filt_row_avx2(int f, uint8_t *out, const uint8_t *cur, const uint8_t *prev, size_t n, int bpp) {
    size_t i = 0;
    if (f == 0) { memcpy(out, cur, n); return; }
    for (; i < (size_t)bpp && i < n; i++) out[i] = (uint8_t)(cur[i] - pred_scalar(f, 0, prev[i], 0));
    for (; i + 32 <= n; i += 32) {
        __m256i x = _mm256_loadu_si256((const __m256i *)(cur + i)), a = _mm256_loadu_si256((const __m256i *)(cur + i - bpp));
        __m256i b = _mm256_loadu_si256((const __m256i *)(prev + i)), p;
        switch (f) {
        case 1: p = a; break;
        case 2: p = b; break;
        case 3: p = avg_v32(a, b); break;
        default: p = paeth_v32(a, b, _mm256_loadu_si256((const __m256i *)(prev + i - bpp))); break;
        }
        _mm256_storeu_si256((__m256i *)(out + i), _mm256_sub_epi8(x, p));
    }
    for (; i < n; i++) out[i] = (uint8_t)(cur[i] - pred_scalar(f, cur[i - bpp], prev[i], prev[i - bpp]));
}

/* ---- fused cost of all five filters: cost[f] = sum |(int8)(x - pred_f)| ---- */
static void costs_sse2(const uint8_t *cur, const uint8_t *prev, size_t n, int bpp, uint64_t cost[5]) {
    const __m128i z = _mm_setzero_si128();
    __m128i acc[5] = { z, z, z, z, z };
    size_t i = 0;
    uint64_t s[5] = { 0, 0, 0, 0, 0 };
    for (; i < (size_t)bpp && i < n; i++) for (int f = 0; f < 5; f++) s[f] += (uint64_t)absres(cur[i] - pred_scalar(f, 0, prev[i], 0));
#define ACC(k, r) acc[k] = _mm_add_epi64(acc[k], _mm_sad_epu8(_mm_min_epu8(r, _mm_sub_epi8(z, r)), z))
    for (; i + 16 <= n; i += 16) {
        __m128i x = _mm_loadu_si128((const __m128i *)(cur + i)), a = _mm_loadu_si128((const __m128i *)(cur + i - bpp));
        __m128i b = _mm_loadu_si128((const __m128i *)(prev + i)), c = _mm_loadu_si128((const __m128i *)(prev + i - bpp));
        ACC(0, x);
        __m128i r1 = _mm_sub_epi8(x, a); ACC(1, r1);
        __m128i r2 = _mm_sub_epi8(x, b); ACC(2, r2);
        __m128i r3 = _mm_sub_epi8(x, avg_v16(a, b)); ACC(3, r3);
        __m128i r4 = _mm_sub_epi8(x, paeth_v16(a, b, c)); ACC(4, r4);
    }
#undef ACC
    for (; i < n; i++) for (int f = 0; f < 5; f++) s[f] += (uint64_t)absres(cur[i] - pred_scalar(f, cur[i - bpp], prev[i], prev[i - bpp]));
    for (int f = 0; f < 5; f++)
        cost[f] = s[f] + (uint64_t)_mm_cvtsi128_si64(acc[f]) + (uint64_t)_mm_cvtsi128_si64(_mm_unpackhi_epi64(acc[f], acc[f]));
}
__attribute__((target("avx2"))) static void costs_avx2(const uint8_t *cur, const uint8_t *prev, size_t n, int bpp, uint64_t cost[5]) {
    const __m256i z = _mm256_setzero_si256();
    __m256i acc[5] = { z, z, z, z, z };
    size_t i = 0;
    uint64_t s[5] = { 0, 0, 0, 0, 0 };
    for (; i < (size_t)bpp && i < n; i++) for (int f = 0; f < 5; f++) s[f] += (uint64_t)absres(cur[i] - pred_scalar(f, 0, prev[i], 0));
#define ACC(k, r) acc[k] = _mm256_add_epi64(acc[k], _mm256_sad_epu8(_mm256_min_epu8(r, _mm256_sub_epi8(z, r)), z))
    for (; i + 32 <= n; i += 32) {
        __m256i x = _mm256_loadu_si256((const __m256i *)(cur + i)), a = _mm256_loadu_si256((const __m256i *)(cur + i - bpp));
        __m256i b = _mm256_loadu_si256((const __m256i *)(prev + i)), c = _mm256_loadu_si256((const __m256i *)(prev + i - bpp));
        ACC(0, x);
        __m256i r1 = _mm256_sub_epi8(x, a); ACC(1, r1);
        __m256i r2 = _mm256_sub_epi8(x, b); ACC(2, r2);
        __m256i r3 = _mm256_sub_epi8(x, avg_v32(a, b)); ACC(3, r3);
        __m256i r4 = _mm256_sub_epi8(x, paeth_v32(a, b, c)); ACC(4, r4);
    }
#undef ACC
    for (; i < n; i++) for (int f = 0; f < 5; f++) s[f] += (uint64_t)absres(cur[i] - pred_scalar(f, cur[i - bpp], prev[i], prev[i - bpp]));
    for (int f = 0; f < 5; f++) {
        __m128i v = _mm_add_epi64(_mm256_castsi256_si128(acc[f]), _mm256_extracti128_si256(acc[f], 1));
        cost[f] = s[f] + (uint64_t)_mm_cvtsi128_si64(v) + (uint64_t)_mm_cvtsi128_si64(_mm_unpackhi_epi64(v, v));
    }
}

void pf__filter_image(uint8_t *dst, const uint8_t *px, size_t stride, uint32_t h, size_t n, int bpp, int mode) {
    pf__filter_rows(dst, px, stride, 0, h, n, bpp, mode);
}

void pf__filter_rows(uint8_t *dst, const uint8_t *px, size_t stride, uint32_t y0, uint32_t y1, size_t n, int bpp,
                     int mode) {
    const int avx2 = pf__have_avx2();
    uint8_t zrow_small[4096 + 64];
    /* the all-zero predecessor of row 0 (only needed if row 0 is in range) */
    uint8_t *zrow = zrow_small;
    if (y0 == 0) {
        if (n + 64 > sizeof zrow_small) zrow = calloc(1, n + 64);
        else memset(zrow, 0, n + 64);
    }
    if (!zrow) { mode = 0; zrow = zrow_small; }   /* out of memory for the zero row: filter None needs no prev */
    for (uint32_t y = y0; y < y1; y++) {
        const uint8_t *cur = px + (size_t)y * stride, *prev = y ? cur - stride : zrow;
        uint8_t *o = dst + (size_t)(y - y0) * (n + 1);
        int f = mode;
        if (mode == PF_FILTER_ADAPTIVE || mode == PF__FILTER_ADAPTIVE_GFX) {
            uint64_t cost[5];
            if (avx2) costs_avx2(cur, prev, n, bpp, cost); else costs_sse2(cur, prev, n, bpp, cost);
            f = 0;
            for (int k = 1; k < 5; k++) if (cost[k] < cost[f]) f = k;
            /* graphics: Up whenever it is within 2x of the best residual sum (repeated rows / zero runs
             * matter more to LZ than small residuals; -3..6% on screenshots, worse on photos) */
            if (mode == PF__FILTER_ADAPTIVE_GFX && cost[PF_FILTER_UP] <= 2 * cost[f]) f = PF_FILTER_UP;
        }
        o[0] = (uint8_t)f;
        if (avx2) filt_row_avx2(f, o + 1, cur, prev, n, bpp); else filt_row_sse2(f, o + 1, cur, prev, n, bpp);
    }
    if (zrow != zrow_small) free(zrow);
}
