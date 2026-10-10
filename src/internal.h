/* internal.h - shared helpers for the pngfast implementation (not installed). */
#ifndef PNGFAST_INTERNAL_H
#define PNGFAST_INTERNAL_H

#include "pngfast.h"
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "pf_libdeflate.h"

/* SIMD: x86-64 (SSE2 baseline, AVX2 runtime-dispatched). Everything else, or -DPNGFAST_NO_SIMD, builds the
 * portable scalar code paths (same output). */
#if (defined(__x86_64__) || defined(_M_X64)) && !defined(PNGFAST_NO_SIMD)
#  define PF_X86 1
#  include <emmintrin.h>
#  include <immintrin.h>
#else
#  define PF_X86 0
#endif

#define PF_INLINE static inline __attribute__((always_inline))

static inline uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3]; }
static inline void wbe32(uint8_t *p, uint32_t v) { p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16); p[2] = (uint8_t)(v >> 8); p[3] = (uint8_t)v; }
static inline uint32_t ld32(const void *p) { uint32_t v; memcpy(&v, p, 4); return v; }
static inline void st32(void *p, uint32_t v) { memcpy(p, &v, 4); }
static inline uint64_t ld64(const void *p) { uint64_t v; memcpy(&v, p, 8); return v; }

/* PNG Paeth predictor, branchless (stb_image formulation, == spec). */
static inline int paeth_s(int a, int b, int c) {
    int th = c * 3 - (a + b), lo = a < b ? a : b, hi = a < b ? b : a;
    int t0 = (hi <= th) ? lo : c;
    return (th <= lo) ? hi : t0;
}

/* ---- CPU features (cached) ---- */
int pf__have_avx2(void);

/* ---- allocator ---- */
void *pf__alloc(const pf_allocator *a, size_t n);
void pf__free(const pf_allocator *a, void *p);
void *pf__realloc_shrink(const pf_allocator *a, void *p, size_t old_n, size_t new_n);

/* ---- unfilter.c ---- */
typedef void (*pf_pair_fn)(uint8_t *d0, const uint8_t *s0, const uint8_t *p0, uint8_t *d1, const uint8_t *s1,
                           size_t n, int bpp);
void pf__unfilter_row(int f, uint8_t *dst, const uint8_t *src, const uint8_t *prev, size_t n, int bpp);
pf_pair_fn pf__pair_for_bpp(int f0, int f1, int bpp);
#if PF_X86
void pf__quad_paeth_avx2(uint8_t *d0, const uint8_t *s0, const uint8_t *p0, size_t stride, size_t rs, size_t n, int bpp);
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
} pf_xform;
/* Choose the transform for (color, depth, palette, trns, output); returns output channels/depth/color. */
void pf__xform_setup(pf_xform *x, int color, int depth, int ich, uint32_t W, pf_output output,
                     const uint8_t *plte, uint32_t plte_entries, const uint8_t *trns, uint32_t tlen,
                     int *och, int *odepth, int *ocolor);
void pf__xform_row(const pf_xform *x, const uint8_t *s, uint8_t *o, uint32_t w);
void pf__swap16(uint8_t *p, size_t n);
static inline uint32_t pf__unpack(const uint8_t *s, uint32_t i, int d) {   /* MSB-first packed sample i */
    uint32_t bit = i * (uint32_t)d; return (uint32_t)(s[bit >> 3] >> (8 - d - (bit & 7))) & ((1u << d) - 1);
}

/* ---- filter_enc.c: forward filtering (encoder) ---- */
/* Filter `h` rows of `rowbytes` (source rows `stride` apart) into dst (h * (1 + rowbytes) bytes).
 * mode: 0..4 = fixed filter (row 0 uses None for Up / Sub for Paeth / Avg as specified by PNG semantics: the fixed
 * filter is applied as-is, prev = zeros), 5 = adaptive (min sum |signed residual|). */
void pf__filter_image(uint8_t *dst, const uint8_t *px, size_t stride, uint32_t h, size_t rowbytes, int bpp, int mode);
/* internal filter mode: adaptive with a bias towards Up (graphics) */
#define PF__FILTER_ADAPTIVE_GFX 6
/* PF__FILTER_ADAPTIVE_GFX without Paeth: opaque graphics (Paeth residuals rarely beat None/Sub/Up there and cost
 * the most to evaluate; -1% size, faster) */
#define PF__FILTER_ADAPTIVE_FLAT 7
/* rows [y0, y1) only, written to dst from its start (row y0 - 1 is the Up/Avg/Paeth predecessor) */
void pf__filter_rows(uint8_t *dst, const uint8_t *px, size_t stride, uint32_t y0, uint32_t y1, size_t n, int bpp,
                     int mode);

#endif
