/* metadata.h - internal interface of metadata.c (not installed). */
#ifndef QPNG_METADATA_H
#define QPNG_METADATA_H
#include "internal.h"
/* encoder: validate against the image color type; serialise the chunks of one location (0: before PLTE,
 * 1: after PLTE / tRNS, before IDAT, 2: after IDAT) into w, or into a new buffer (nullptr if empty) */
qpng_status qpng__meta_validate(const qpng_metadata m[static 1], int color);
void qpng__meta_write(qpng__wbuf w[static 1], const qpng_metadata m[static 1], int loc);
qpng_status qpng__meta_serialize(const qpng_metadata m[static 1], int loc, const qpng_allocator *A,
                                 uint8_t *out[static 1], size_t outn[static 1]);
#endif
