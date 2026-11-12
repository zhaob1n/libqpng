/* internal.h - shared helpers for the qpng implementation (not installed). */
#ifndef QPNG_INTERNAL_H
#define QPNG_INTERNAL_H

#include "qpng.h"
#include <stdint.h>
#include <stdlib.h>
#include <stdbit.h>
#include <stdckdint.h>
#include <string.h>
#include "qpng_libdeflate.h"

/* SIMD: x86-64 (SSE2 baseline, AVX2 runtime-dispatched). Everything else, or -DQPNG_NO_SIMD, builds the
 * portable scalar code paths (same output). */
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(QPNG_NO_SIMD)
#  define QPNG_X86 1
#  include <emmintrin.h>
#  include <immintrin.h>
#else
#  define QPNG_X86 0
#endif

#define QPNG_INLINE [[gnu::always_inline]] static inline

static constexpr uint8_t QPNG__SIG[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };

static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void wbe32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline uint16_t be16(const uint8_t *p) { return (uint16_t)(p[0] << 8 | p[1]); }
static inline void wbe16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static inline uint32_t ld32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void st32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static inline uint64_t ld64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }
static inline void st64(void *p, uint64_t v) { memcpy(p, &v, 8); }

/* zlib CMF/FLG: deflate, window <= 32 KiB, no preset dictionary, FCHECK */
static inline bool qpng__zlib_hdr_ok(const uint8_t z[static 2]) {
    return (z[0] & 15) == 8 && z[0] >> 4 <= 7 && !(z[1] & 32) && be16(z) % 31 == 0;
}

/* Overflow-checked size math (<stdckdint.h>). ckd_* return true on overflow. */
[[nodiscard]] static inline bool qpng__mul_size(size_t *out, size_t a, size_t b) { return ckd_mul(out, a, b); }
[[nodiscard]] static inline bool qpng__add_size(size_t *out, size_t a, size_t b) { return ckd_add(out, a, b); }

/* PNG Paeth predictor, branchless (stb_image formulation, == spec). */
static inline int paeth_s(int a, int b, int c) {
    int th = c * 3 - (a + b), lo = a < b ? a : b, hi = a < b ? b : a;
    int t0 = (hi <= th) ? lo : c;
    return (th <= lo) ? hi : t0;
}
/* predictor of PNG filter f (0..4) from left a, up b, upper-left c */
QPNG_INLINE int qpng__pred(int f, int a, int b, int c) {
    return f == 0 ? 0 : f == 1 ? a : f == 2 ? b : f == 3 ? (a + b) >> 1 : paeth_s(a, b, c);
}
#if QPNG_X86
/* Paeth predictor on 8 16-bit lanes holding bytes (same formulation as paeth_s). a comes last in th: in the
 * unfilter wavefronts it is the loop-carried value, so 3c - b runs off its dependency chain. */
static inline __m128i qpng__paeth_epi16(__m128i a, __m128i b, __m128i c) {
    const __m128i th = _mm_sub_epi16(_mm_sub_epi16(_mm_add_epi16(c, _mm_add_epi16(c, c)), b), a);
    const __m128i lo = _mm_min_epi16(a, b), hi = _mm_max_epi16(a, b);
    const __m128i m1 = _mm_cmpgt_epi16(hi, th), m2 = _mm_cmpgt_epi16(th, lo);
    const __m128i t0 = _mm_or_si128(_mm_and_si128(m1, c), _mm_andnot_si128(m1, lo));
    return _mm_or_si128(_mm_and_si128(m2, t0), _mm_andnot_si128(m2, hi));
}
#endif

/* ---- IHDR payload (13 bytes). parse (decode.c) validates and fills the IHDR fields and channels of *info,
 * leaving the rest untouched; also checks the decode size limits when o is given ---- */
qpng_status qpng__parse_ihdr(const uint8_t ih[static 13], const qpng_decode_options *o, qpng_info info[static 1]);
static inline void qpng__put_ihdr(uint8_t ih[static 13], uint32_t w, uint32_t h, int depth, int color) {
    wbe32(ih, w); wbe32(ih + 4, h);
    ih[8] = (uint8_t)depth; ih[9] = (uint8_t)color; ih[10] = ih[11] = ih[12] = 0;
}

/* ---- encode.c: argument / qpng_encode_options validation shared by qpng_encode and qpng_encoder_new;
 * returns the channel count or a negative qpng_status ---- */
[[nodiscard]] int qpng__check_encode_opts(const qpng_encode_options o[static 1], uint32_t w, uint32_t h, qpng_color color,
                                          int depth);

/* ---- CPU features (cached) ---- */
int qpng__have_avx2(void);

/* ---- allocator ---- */
[[nodiscard]] void *qpng__alloc(const qpng_allocator *a, size_t n);
void qpng__free(const qpng_allocator *a, void *p);
[[nodiscard]] void *qpng__realloc_shrink(const qpng_allocator *a, void *p, size_t old_n, size_t new_n);
/* Array p of n live elements of el bytes, capacity *cap: returns storage for >= need elements (p itself when it
 * fits; else capacity doubles from `first` (>= 1; first == 0 starts at need), the n elements move
 * and p is freed), or nullptr with p untouched. */
[[nodiscard]] void *qpng__grow(const qpng_allocator *a, void *p, size_t n, size_t cap[static 1], size_t need, size_t el,
                               size_t first);

/* ---- scope-bound release: [[gnu::cleanup]] instead of free()/goto-cleanup ----
 * Owning locals are declared with a cleanup attribute; error returns just return.
 * Hand ownership out with QPNG_TAKE / qpng__take (nulls the local so cleanup is a no-op).
 *
 *   qpng_owned out = { A, qpng__alloc(A, n) };
 *   if (!out.p) return QPNG_ERR_NOMEM;
 *   if (bad) return QPNG_ERR_CHUNK;           // freed here
 *   im->pixels = qpng__take(&out);            // kept
 *
 *   qpng_autofree uint8_t *tmp = malloc(n);   // libc memory (tools, tests)
 *   return QPNG_TAKE(tmp);
 *
 * Cleanups run in reverse declaration order, also on early return; not on longjmp.
 * Do not take the address of an owner past its scope; do not copy a qpng_owned. */
#define QPNG_CLEANUP(fn) [[gnu::cleanup(fn)]]

#define QPNG_TAKE(var) ({ typeof(var) qpng__t_ = (var); (var) = nullptr; qpng__t_; })

static inline void qpng__freep(void *pp) { free(*(void **)pp); }
#define qpng_autofree QPNG_CLEANUP(qpng__freep)

typedef struct { const qpng_allocator *A; void *p; } qpng__owned;
static inline void qpng__owned_release(qpng__owned *o) { qpng__free(o->A, o->p); o->p = nullptr; }
#define qpng_owned QPNG_CLEANUP(qpng__owned_release) qpng__owned
[[nodiscard]] static inline void *qpng__take(qpng__owned *o) { void *p = o->p; o->p = nullptr; return p; }

/* ---- PNG chunk writers (common.c) ---- */
/* Frame len bytes already at p + 8 (data == nullptr) or copied from data: length, type, CRC. Returns the chunk end. */
static inline uint8_t *qpng__put_chunk(uint8_t *p, const char type[static 4], const uint8_t *data, uint32_t len) {
    wbe32(p, len); memcpy(p + 4, type, 4);
    if (data && len) memcpy(p + 8, data, len);
    wbe32(p + 8 + len, libdeflate_crc32(0, p + 4, (size_t)len + 4));
    return p + 12 + len;
}
/* Growable output buffer (allocator A) that frames PNG chunks. The first error sticks and turns every later write
 * into a no-op, so writers check `err` once at the end; on error the caller frees `p`. */
typedef struct { uint8_t *p; size_t n, cap; const qpng_allocator *A; qpng_status err; } qpng__wbuf;
static inline void qpng__wb_release(qpng__wbuf *w) { qpng__free(w->A, w->p); w->p = nullptr; }
void qpng__wb_put(qpng__wbuf *w, const void *d, size_t n);
/* chunk framing: begin writes the length placeholder and type, end patches the length and appends the CRC */
[[nodiscard]] size_t qpng__wb_begin(qpng__wbuf *w, const char type[static 4]);
void qpng__wb_end(qpng__wbuf *w, size_t start);
void qpng__wb_chunk(qpng__wbuf *w, const char type[static 4], const void *d, size_t n);


/* ---- unfilter.c ---- */
typedef void (*qpng_pair_fn)(uint8_t *d0, const uint8_t *s0, const uint8_t *p0, uint8_t *d1, const uint8_t *s1,
                           size_t n, size_t bpp);
void qpng__unfilter_row(int f, uint8_t *dst, const uint8_t *src, const uint8_t *prev, size_t n, int bpp);
qpng_pair_fn qpng__pair_for_bpp(int f0, int f1, int bpp);
#if QPNG_X86
void qpng__quad_paeth_avx2(uint8_t *d0, const uint8_t *s0, const uint8_t *p0, size_t stride, size_t rs, size_t n, size_t bpp);
#endif

/* ---- expand.c: output transforms applied to unfiltered rows ---- */
enum { XF_NONE, XF_SWAP16, XF_PAL, XF_GRAY, XF_TRNS8, XF_TRNS16 };
typedef struct {
    int xf, depth, ich;            /* kind, input bit depth, input channels */
    uint32_t W;                    /* image width */
    int alpha, native16, scale;    /* add alpha (tRNS) / swap 16-bit to native / gray scale factor */
    uint8_t trns[6];
    uint32_t lut[256];             /* palette -> RGBA */
    uint8_t glut[256][16];         /* packed gray byte -> 8-bit samples (+alpha) */
} qpng_xform;
/* Choose the transform for (color, depth, palette, trns, output); returns output channels/depth/color. */
void qpng__xform_setup(qpng_xform *x, int color, int depth, int ich, uint32_t W, qpng_output output,
                     const uint8_t *plte, uint32_t plte_entries, const uint8_t *trns, uint32_t tlen,
                     int *och, int *odepth, int *ocolor);
void qpng__xform_row(const qpng_xform *x, const uint8_t *s, uint8_t *o, uint32_t w);
void qpng__swap16(uint8_t *p, size_t n);
static inline uint32_t qpng__unpack(const uint8_t *s, uint32_t i, int d) {   /* MSB-first packed sample i */
    uint32_t bit = i * (uint32_t)d; return (uint32_t)(s[bit >> 3] >> ((uint32_t)(8 - d) - (bit & 7))) & ((1u << d) - 1);
}

/* ---- filter_enc.c: forward filtering (encoder) ---- */
/* Filter `h` rows of `rowbytes` (source rows `stride` apart) into dst (h * (1 + rowbytes) bytes).
 * mode: 0..4 = fixed filter (row 0 uses None for Up / Sub for Paeth / Avg as specified by PNG semantics: the fixed
 * filter is applied as-is, prev = zeros), 5 = adaptive (min sum |signed residual|). */
void qpng__filter_image(uint8_t *dst, const uint8_t *px, size_t stride, uint32_t h, size_t rowbytes, int bpp, int mode);
/* internal filter modes beyond qpng_filter */
enum : uint8_t {
    QPNG__FILTER_ADAPTIVE_GFX = QPNG_FILTER_ADAPTIVE + 1,   /* adaptive with a bias towards Up (graphics) */
    /* ..._GFX without Paeth: opaque graphics (Paeth residuals rarely beat None/Sub/Up there and cost the most to
     * evaluate; -1% size, faster) */
    QPNG__FILTER_ADAPTIVE_FLAT,
};
/* rows [y0, y1) only, written to dst from its start (row y0 - 1 is the Up/Avg/Paeth predecessor) */
void qpng__filter_rows(uint8_t *dst, const uint8_t *px, size_t stride, uint32_t y0, uint32_t y1, size_t n, int bpp,
                     int mode);

#endif
