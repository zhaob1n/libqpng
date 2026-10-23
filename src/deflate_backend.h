/* deflate_backend.h - the encoder's only interface to deflate implementations (not installed).
 *
 * Backends: the vendored, modified libdeflate (always present: levels 1-12, png-fast, png-ultra streaming) and the
 * optional vendored zlib-ng (QPNG_HAVE_ZLIBNG: levels 1-9 incl. deflate_quick/medium, zlib strategies).
 * encode.c describes *what* to run with a qpng_zcfg; everything backend-specific lives in deflate_backend.c. */
#ifndef QPNG_DEFLATE_BACKEND_H
#define QPNG_DEFLATE_BACKEND_H
#include "internal.h"

typedef struct {
    uint8_t backend;    /* QPNG_BACKEND_LIBDEFLATE or QPNG_BACKEND_ZLIBNG (never AUTO) */
    uint8_t strategy;   /* zlib-ng: QPNG_ZSTRATEGY_*; ignored by libdeflate */
    uint8_t minlen;     /* libdeflate levels 2-12: fixed minimum match length (0 = libdeflate's choice) */
    int16_t level;      /* libdeflate 1..12 / QPNG_DEFLATE_PNG*; zlib-ng 1..9 */
    uint16_t depth;     /* search depth (libdeflate max_search_depth, zlib-ng max_chain); 0 = level default */
    uint16_t nice;      /* nice match length; 0 = level default */
} qpng_zcfg;

static inline qpng_zcfg qpng__zcfg(int backend, int level, int depth, int nice, int minlen, int strategy) {
    qpng_zcfg c = { (uint8_t)backend, (uint8_t)strategy, (uint8_t)minlen, (int16_t)level, (uint16_t)depth, (uint16_t)nice };
    return c;
}

/* 1 if the backend is compiled in */
int qpng__backend_available(int backend);
/* is cfg valid (level range for its backend, backend present) */
int qpng__zcfg_valid(const qpng_zcfg *c);
/* worst-case zlib stream size for n input bytes over every backend and level */
size_t qpng__zbound(size_t n);
/* compress src[0..n) into a complete zlib stream at dst (capacity cap >= qpng__zbound(n)); returns its size, or 0
 * with *st set */
size_t qpng__zcompress(const qpng_zcfg *c, const uint8_t *src, size_t n, uint8_t *dst, size_t cap,
                     const qpng_allocator *A, qpng_status *st);
/* libdeflate png-ultra, streamed: rows are filtered (mode `filter`; hybrid: Up after strongly compressible chunks)
 * into an L2-sized buffer and compressed chunk by chunk. */
size_t qpng__zcompress_ultra_rows(const uint8_t *px, size_t stride, uint32_t h, size_t n, int bpp, int filter,
                                int hybrid, uint8_t *dst, size_t cap, const qpng_allocator *A, qpng_status *st);

/* ---- streaming (qpng_encoder / qpng_decoder) ---- */
/* compressed-output sink; returns QPNG_OK or an error that aborts the stream */
typedef qpng_status (*qpng_zsink)(void *ctx, const uint8_t *data, size_t n);
typedef struct qpng_zstream qpng_zstream;
/* streamable configs: libdeflate QPNG_DEFLATE_PNGULTRA, zlib-ng levels 1-9 (else NULL with *st = UNSUPPORTED) */
qpng_zstream *qpng__zstream_new(const qpng_zcfg *c, const qpng_allocator *A, qpng_zsink sink, void *ctx, qpng_status *st);
/* compress the next n bytes (png-ultra: n <= QPNG_ZSTREAM_MAX_CHUNK and in[-1] readable); final ends the stream */
qpng_status qpng__zstream_write(qpng_zstream *z, const uint8_t *in, size_t n, int final);
void qpng__zstream_free(qpng_zstream *z);
#define QPNG_ZSTREAM_MAX_CHUNK (262144 + 4096)

/* raw-deflate streaming inflate (zlib-ng only; NULL without it) */
typedef struct qpng_zistream qpng_zistream;
qpng_zistream *qpng__zistream_new(const qpng_allocator *A);
/* inflate from in[0..n) into out[0..cap); *used / *made report progress; returns 1 at end of the deflate stream,
 * 0 if more input / output is needed, -1 on corrupt data */
int qpng__zistream_run(qpng_zistream *z, const uint8_t *in, size_t n, size_t *used, uint8_t *out, size_t cap, size_t *made);
void qpng__zistream_free(qpng_zistream *z);
#endif
