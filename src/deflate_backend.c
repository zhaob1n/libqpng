/* deflate_backend.c - libdeflate / zlib-ng behind one interface (see deflate_backend.h). */
#include "deflate_backend.h"
#ifdef PF_HAVE_ZLIBNG
#  include <zlib-ng.h>
#endif

int pf__backend_available(int backend) {
    if (backend == PF_BACKEND_LIBDEFLATE) return 1;
#ifdef PF_HAVE_ZLIBNG
    if (backend == PF_BACKEND_ZLIBNG) return 1;
#endif
    return 0;
}

int pf__zcfg_valid(const pf_zcfg *c) {
    if (!pf__backend_available(c->backend)) return 0;
    if (c->backend == PF_BACKEND_ZLIBNG) return c->level >= 1 && c->level <= 9 && c->strategy <= PF_ZSTRATEGY_RLE;
    return (c->level >= 1 && c->level <= 12) || c->level == PF_DEFLATE_PNGFAST || c->level == PF_DEFLATE_PNGFAST_RLE ||
           c->level == PF_DEFLATE_PNGFAST_HIGH || c->level == PF_DEFLATE_PNGULTRA;
}

size_t pf__zbound(size_t n) {
    size_t b = libdeflate_zlib_compress_bound(NULL, n);
#ifdef PF_HAVE_ZLIBNG
    /* zlib-ng's bound for stored blocks of 64 KiB is below libdeflate's, but be explicit: deflateBound() without
     * a stream returns the conservative bound for every level / strategy */
    size_t z = (size_t)zng_compressBound((unsigned long)n);
    if (sizeof(unsigned long) >= sizeof(size_t) && z > b) b = z;
#endif
    return b + 64;
}

/* ---- libdeflate ---- */
static struct libdeflate_compressor *ld_make(const pf_zcfg *c) {
    if (c->level < PF_DEFLATE_PNGFAST) {
        struct libdeflate_compressor *z = libdeflate_alloc_compressor(c->level);
        if (z && (c->depth || c->nice)) libdeflate_set_search_params(z, c->depth, c->nice);
        if (z && c->minlen) libdeflate_set_min_match_len(z, c->minlen);
        return z;
    }
    if (c->level == PF_DEFLATE_PNGULTRA) return libdeflate_alloc_compressor(LIBDEFLATE_PNG_ULTRA_LEVEL);
    struct libdeflate_compressor *z = libdeflate_alloc_compressor(LIBDEFLATE_PNG_FAST_LEVEL);
    if (z && c->level == PF_DEFLATE_PNGFAST_RLE) libdeflate_set_png_min_match(z, 0);
    if (z && c->level == PF_DEFLATE_PNGFAST_HIGH) libdeflate_set_png_params(z, 0, 9);
    return z;
}

static size_t ld_compress(const pf_zcfg *c, const uint8_t *src, size_t n, uint8_t *dst, size_t cap, pf_status *st) {
    struct libdeflate_compressor *z = ld_make(c);
    if (!z) { *st = PF_ERR_NOMEM; return 0; }
    size_t zn = libdeflate_zlib_compress(z, src, n, dst, cap);
    libdeflate_free_compressor(z);
    if (!zn) *st = PF_ERR_ENCODE;
    return zn;
}

/* ---- zlib-ng ---- */
#ifdef PF_HAVE_ZLIBNG
static void *zng_alloc_cb(void *user, unsigned items, unsigned size) {
    return pf__alloc((const pf_allocator *)user, (size_t)items * size);
}
static void zng_free_cb(void *user, void *p) { pf__free((const pf_allocator *)user, p); }

static size_t zng_compress_all(const pf_zcfg *c, const uint8_t *src, size_t n, uint8_t *dst, size_t cap,
                               const pf_allocator *A, pf_status *st) {
    zng_stream s;
    memset(&s, 0, sizeof s);
    s.zalloc = zng_alloc_cb; s.zfree = zng_free_cb; s.opaque = (void *)A;
    /* window 32 KiB, memLevel 9 (128 KiB hash / symbol buffer: measured smaller on graphics than 8 at equal speed) */
    if (zng_deflateInit2(&s, c->level, Z_DEFLATED, 15, 9, c->strategy) != Z_OK) { *st = PF_ERR_NOMEM; return 0; }
    if (c->depth || c->nice) {
        /* zng_deflateTune(good, lazy, nice, chain): keep the level's good / lazy lengths */
        static const struct { uint16_t good, lazy, nice, chain; } lv[10] = {
            { 0, 0, 0, 0 }, { 0, 0, 0, 0 }, { 4, 4, 8, 4 }, { 4, 6, 16, 6 }, { 4, 12, 32, 24 }, { 8, 16, 32, 32 },
            { 8, 16, 128, 128 }, { 8, 32, 128, 256 }, { 32, 128, 258, 1024 }, { 32, 258, 258, 4096 } };   /* zlib-ng deflate.c */
        zng_deflateTune(&s, lv[c->level].good, lv[c->level].lazy, c->nice ? c->nice : lv[c->level].nice,
                        c->depth ? c->depth : lv[c->level].chain);
    }
    size_t in = 0, out = 0;
    int r = Z_OK;
    while (r == Z_OK) {   /* avail_in / avail_out are 32-bit */
        const size_t ci = n - in < 0x40000000u ? n - in : 0x40000000u, co = cap - out < 0x40000000u ? cap - out : 0x40000000u;
        s.next_in = src + in; s.avail_in = (uint32_t)ci;
        s.next_out = dst + out; s.avail_out = (uint32_t)co;
        r = zng_deflate(&s, in + ci == n ? Z_FINISH : Z_NO_FLUSH);
        in += ci - s.avail_in; out += co - s.avail_out;
        if (r == Z_BUF_ERROR && out < cap && in < n) r = Z_OK;
    }
    zng_deflateEnd(&s);
    if (r != Z_STREAM_END) { *st = PF_ERR_ENCODE; return 0; }
    return out;
}
#endif

size_t pf__zcompress(const pf_zcfg *c, const uint8_t *src, size_t n, uint8_t *dst, size_t cap,
                     const pf_allocator *A, pf_status *st) {
#ifdef PF_HAVE_ZLIBNG
    if (c->backend == PF_BACKEND_ZLIBNG) return zng_compress_all(c, src, n, dst, cap, A, st);
#endif
    (void)A;
    return ld_compress(c, src, n, dst, cap, st);
}

/*
 * png-ultra streaming: rows are filtered into a small (L2-sized) buffer and compressed chunk by chunk, so the
 * filtered image is never materialised. Chunks are ~64 KiB, 256 KiB once the data compresses >= 4:1.
 */
#define ULTRA_CHUNK      65536
#define ULTRA_CHUNK_BIG  262144
#define ULTRA_MIN_TAIL   4096
size_t pf__zcompress_ultra_rows(const uint8_t *px, size_t stride, uint32_t h, size_t n, int bpp, int filter,
                                int hybrid, uint8_t *dst, size_t cap, const pf_allocator *A, pf_status *st) {
    const size_t row = n + 1;
    const size_t max_rows = (ULTRA_CHUNK_BIG + ULTRA_MIN_TAIL) / row + 2;
    struct libdeflate_compressor *c = libdeflate_alloc_compressor(LIBDEFLATE_PNG_ULTRA_LEVEL);
    uint8_t *buf = pf__alloc(A, max_rows * row + 1 + 64);
    struct libdeflate_png_ultra_state us = { 0, 0, 0 };
    uint32_t adler = 1;
    size_t zn = 2, target = ULTRA_CHUNK;
    if (!c || !buf || cap < 6) { *st = c && buf ? PF_ERR_ENCODE : PF_ERR_NOMEM; zn = 0; goto done; }
    dst[0] = 0x78; dst[1] = 0x01;
    for (uint32_t y = 0; y < h;) {
        uint32_t k = (uint32_t)(target / row ? target / row : 1);
        if (k > h - y) k = h - y;
        if ((size_t)(h - y - k) * row < ULTRA_MIN_TAIL && (size_t)(h - y) <= max_rows) k = h - y;   /* no tiny tail */
        const size_t len = (size_t)k * row;
        pf__filter_rows(buf + 1, px, stride, y, y + k, n, bpp, hybrid && target == ULTRA_CHUNK_BIG ? PF_FILTER_UP : filter);
        adler = libdeflate_adler32(adler, buf + 1, len);
        y += k;
        size_t o = libdeflate_png_ultra_compress_chunk(c, &us, buf + 1, len, y == h, dst + zn, cap - zn - 4);
        if (!o) { *st = PF_ERR_ENCODE; zn = 0; goto done; }
        zn += o;
        buf[0] = buf[len];   /* the parser may look one byte back */
        target = o * 4 < len ? ULTRA_CHUNK_BIG : ULTRA_CHUNK;
    }
    wbe32(dst + zn, adler); zn += 4;
done:
    pf__free(A, buf);
    libdeflate_free_compressor(c);
    return zn;
}
