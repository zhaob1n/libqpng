/* metadata.h - internal interface of metadata.c (not installed). */
#ifndef QPNG_METADATA_H
#define QPNG_METADATA_H
#include "internal.h"
/* zlib stream -> new buffer (allocator A) of at most `limit` bytes; NULL if invalid / too large */
uint8_t *qpng__inflate_limited(const uint8_t *src, size_t n, size_t limit, const qpng_allocator *A, size_t *outn);
/* encoder: validate against the image color type; serialise the chunks of one location (0: before PLTE,
 * 1: after PLTE / tRNS, before IDAT, 2: after IDAT) into a new buffer (NULL if empty) */
qpng_status qpng__meta_validate(const qpng_metadata *m, int color);
qpng_status qpng__meta_serialize(const qpng_metadata *m, int loc, const qpng_allocator *A, uint8_t **out, size_t *outn);
#endif
