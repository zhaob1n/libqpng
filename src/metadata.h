/* metadata.h - internal interface of metadata.c (not installed). */
#ifndef PF_METADATA_H
#define PF_METADATA_H
#include "internal.h"
/* zlib stream -> new buffer (allocator A) of at most `limit` bytes; NULL if invalid / too large */
uint8_t *pf__inflate_limited(const uint8_t *src, size_t n, size_t limit, const pf_allocator *A, size_t *outn);
/* encoder: validate against the image color type; serialise the chunks of one location (0: before PLTE,
 * 1: after PLTE / tRNS, before IDAT, 2: after IDAT) into a new buffer (NULL if empty) */
pf_status pf__meta_validate(const pf_metadata *m, int color);
pf_status pf__meta_serialize(const pf_metadata *m, int loc, const pf_allocator *A, uint8_t **out, size_t *outn);
#endif
