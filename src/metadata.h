/* metadata.h - internal interface of metadata.c (not installed). */
#ifndef QPNG_METADATA_H
#define QPNG_METADATA_H
#include "internal.h"
/* Growable output buffer (allocator A) that frames PNG chunks. The first error sticks and turns every later write
 * into a no-op, so writers check `err` once at the end; on error the caller frees `p`. */
typedef struct { uint8_t *p; size_t n, cap; const qpng_allocator *A; qpng_status err; } qpng__wbuf;
static inline void qpng__wb_release(qpng__wbuf *w) { qpng__free(w->A, w->p); w->p = nullptr; }
void qpng__wb_put(qpng__wbuf *w, const void *d, size_t n);
/* chunk framing: begin writes the length placeholder and type, end patches the length and appends the CRC */
[[nodiscard]] size_t qpng__wb_begin(qpng__wbuf *w, const char type[static 4]);
void qpng__wb_end(qpng__wbuf *w, size_t start);
void qpng__wb_chunk(qpng__wbuf *w, const char type[static 4], const void *d, size_t n);

/* encoder: validate against the image color type; serialise the chunks of one location (0: before PLTE,
 * 1: after PLTE / tRNS, before IDAT, 2: after IDAT) into w, or into a new buffer (nullptr if empty) */
[[nodiscard]] qpng_status qpng__meta_validate(const qpng_metadata *m, int color);
void qpng__meta_write(qpng__wbuf *w, const qpng_metadata *m, int loc);
[[nodiscard]] qpng_status qpng__meta_serialize(const qpng_metadata *m, int loc, const qpng_allocator *A, uint8_t **out,
                                               size_t *outn);
#endif
