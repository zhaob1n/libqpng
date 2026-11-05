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

/* Preferred: designated-initializer literals (omitted fields are 0 = "backend / level default"):
 *   (qpng_zcfg){ .backend = QPNG_BACKEND_LIBDEFLATE, .level = 5, .minlen = 3 }
 * qpng__zcfg() remains for call sites not yet converted (stream.c). */
static inline qpng_zcfg qpng__zcfg(int backend, int level, int depth, int nice, int minlen, int strategy) {
    return (qpng_zcfg){ .backend = (uint8_t)backend, .strategy = (uint8_t)strategy, .minlen = (uint8_t)minlen,
                        .level = (int16_t)level, .depth = (uint16_t)depth, .nice = (uint16_t)nice };
}

[[nodiscard]] bool qpng__backend_available(int backend);
/* level in range for its backend, backend compiled in */
[[nodiscard]] bool qpng__zcfg_valid(const qpng_zcfg c[static 1]);
/* worst-case zlib stream size for n input bytes over every backend and level */
[[nodiscard]] size_t qpng__zbound(size_t n);
/* compress src[0..n) into a complete zlib stream at dst (capacity cap >= qpng__zbound(n)); returns its size, or 0
 * with *st set */
[[nodiscard]] size_t qpng__zcompress(const qpng_zcfg c[static 1], const uint8_t *src, size_t n, uint8_t *dst, size_t cap,
                                     const qpng_allocator *A, qpng_status st[static 1]);
/* libdeflate png-ultra, streamed: rows are filtered (mode `filter`; hybrid: Up after strongly compressible chunks)
 * into an L2-sized buffer and compressed chunk by chunk. */
[[nodiscard]] size_t qpng__zcompress_ultra_rows(const uint8_t *px, size_t stride, uint32_t h, size_t n, int bpp, int filter,
                                                bool hybrid, uint8_t *dst, size_t cap, const qpng_allocator *A,
                                                qpng_status st[static 1]);

/* ---- streaming (qpng_encoder / qpng_decoder) ---- */
/* compressed-output sink; returns QPNG_OK or an error that aborts the stream */
typedef qpng_status (*qpng_zsink)(void *ctx, const uint8_t *data, size_t n);
typedef struct qpng_zstream qpng_zstream;
/* streamable configs: libdeflate QPNG_DEFLATE_PNGULTRA, zlib-ng levels 1-9 (else nullptr with *st = UNSUPPORTED) */
[[nodiscard]] qpng_zstream *qpng__zstream_new(const qpng_zcfg c[static 1], const qpng_allocator *A, qpng_zsink sink,
                                             void *ctx, qpng_status st[static 1]);
/* compress the next n bytes (png-ultra: n <= QPNG_ZSTREAM_MAX_CHUNK and in[-1] readable); final ends the stream */
[[nodiscard]] qpng_status qpng__zstream_write(qpng_zstream *z, const uint8_t *in, size_t n, bool final);
void qpng__zstream_free(qpng_zstream *z);
constexpr size_t QPNG_ZSTREAM_MAX_CHUNK = 262144 + 4096;

/* raw-deflate streaming inflate (zlib-ng only; nullptr without it) */
typedef struct qpng_zistream qpng_zistream;
[[nodiscard]] qpng_zistream *qpng__zistream_new(const qpng_allocator *A);
typedef enum : int8_t { QPNG_ZRUN_BAD = -1, QPNG_ZRUN_MORE, QPNG_ZRUN_END } qpng_zrun;
/* inflate from in[0..n) into out[0..cap); *used / *made report progress. MORE: needs more input or output space.
 * Numeric values match the previous -1 / 0 / 1 contract used by stream.c. */
[[nodiscard]] qpng_zrun qpng__zistream_run(qpng_zistream *z, const uint8_t *in, size_t n, size_t used[static 1],
                                           uint8_t *out, size_t cap, size_t made[static 1]);
void qpng__zistream_free(qpng_zistream *z);
#endif
