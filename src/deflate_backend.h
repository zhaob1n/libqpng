/* deflate_backend.h - the encoder's only interface to deflate implementations (not installed).
 *
 * Backends: the vendored, modified libdeflate (always present: levels 1-12, png-fast, png-ultra streaming) and the
 * optional vendored zlib-ng (PF_HAVE_ZLIBNG: levels 1-9 incl. deflate_quick/medium, zlib strategies).
 * encode.c describes *what* to run with a pf_zcfg; everything backend-specific lives in deflate_backend.c. */
#ifndef PF_DEFLATE_BACKEND_H
#define PF_DEFLATE_BACKEND_H
#include "internal.h"

typedef struct {
    uint8_t backend;    /* PF_BACKEND_LIBDEFLATE or PF_BACKEND_ZLIBNG (never AUTO) */
    uint8_t strategy;   /* zlib-ng: PF_ZSTRATEGY_*; ignored by libdeflate */
    uint8_t minlen;     /* libdeflate levels 2-12: fixed minimum match length (0 = libdeflate's choice) */
    int16_t level;      /* libdeflate 1..12 / PF_DEFLATE_PNG*; zlib-ng 1..9 */
    uint16_t depth;     /* search depth (libdeflate max_search_depth, zlib-ng max_chain); 0 = level default */
    uint16_t nice;      /* nice match length; 0 = level default */
} pf_zcfg;

static inline pf_zcfg pf__zcfg(int backend, int level, int depth, int nice, int minlen, int strategy) {
    pf_zcfg c = { (uint8_t)backend, (uint8_t)strategy, (uint8_t)minlen, (int16_t)level, (uint16_t)depth, (uint16_t)nice };
    return c;
}

/* 1 if the backend is compiled in */
int pf__backend_available(int backend);
/* is cfg valid (level range for its backend, backend present) */
int pf__zcfg_valid(const pf_zcfg *c);
/* worst-case zlib stream size for n input bytes over every backend and level */
size_t pf__zbound(size_t n);
/* compress src[0..n) into a complete zlib stream at dst (capacity cap >= pf__zbound(n)); returns its size, or 0
 * with *st set */
size_t pf__zcompress(const pf_zcfg *c, const uint8_t *src, size_t n, uint8_t *dst, size_t cap,
                     const pf_allocator *A, pf_status *st);
/* libdeflate png-ultra, streamed: rows are filtered (mode `filter`; hybrid: Up after strongly compressible chunks)
 * into an L2-sized buffer and compressed chunk by chunk. */
size_t pf__zcompress_ultra_rows(const uint8_t *px, size_t stride, uint32_t h, size_t n, int bpp, int filter,
                                int hybrid, uint8_t *dst, size_t cap, const pf_allocator *A, pf_status *st);

/* ---- streaming (pf_encoder / pf_decoder) ---- */
/* compressed-output sink; returns PF_OK or an error that aborts the stream */
typedef pf_status (*pf_zsink)(void *ctx, const uint8_t *data, size_t n);
typedef struct pf_zstream pf_zstream;
/* streamable configs: libdeflate PF_DEFLATE_PNGULTRA, zlib-ng levels 1-9 (else NULL with *st = UNSUPPORTED) */
pf_zstream *pf__zstream_new(const pf_zcfg *c, const pf_allocator *A, pf_zsink sink, void *ctx, pf_status *st);
/* compress the next n bytes (png-ultra: n <= PF_ZSTREAM_MAX_CHUNK and in[-1] readable); final ends the stream */
pf_status pf__zstream_write(pf_zstream *z, const uint8_t *in, size_t n, int final);
void pf__zstream_free(pf_zstream *z);
#define PF_ZSTREAM_MAX_CHUNK (262144 + 4096)

/* raw-deflate streaming inflate (zlib-ng only; NULL without it) */
typedef struct pf_zistream pf_zistream;
pf_zistream *pf__zistream_new(const pf_allocator *A);
/* inflate from in[0..n) into out[0..cap); *used / *made report progress; returns 1 at end of the deflate stream,
 * 0 if more input / output is needed, -1 on corrupt data */
int pf__zistream_run(pf_zistream *z, const uint8_t *in, size_t n, size_t *used, uint8_t *out, size_t cap, size_t *made);
void pf__zistream_free(pf_zistream *z);
#endif
