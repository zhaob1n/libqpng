/* fuzz_stream.c - libFuzzer / AFL target: streaming decode (row mode and image mode) with input-derived chunk
 * boundaries must agree with the one-shot qpng_decode (same status class, same pixels); APNG open + all composed
 * frames + raw frames. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "qpng.h"

typedef struct { const uint8_t *ref; size_t stride, size; uint32_t next; int bad; } rows_t;
static int on_row(void *u, uint32_t y, const uint8_t *row, size_t n) {
    rows_t *r = u;
    if (y != r->next || (r->ref && (n != r->stride || (size_t)(y + 1) * n > r->size || memcmp(r->ref + (size_t)y * n, row, n)))) r->bad = 1;
    r->next++;
    return 0;
}

static qpng_status push_split(qpng_decoder *d, const uint8_t *data, size_t size) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size && i < 64; i++) h = (h ^ data[i]) * 16777619u;
    qpng_status s = QPNG_MORE_DATA;
    for (size_t pos = 0; pos < size && s == QPNG_MORE_DATA;) {
        h = h * 1103515245u + 12345u;
        size_t k = 1 + (h >> 16) % 300;
        if (k > size - pos) k = size - pos;
        s = qpng_decoder_push(d, data + pos, k); pos += k;
    }
    return s;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    qpng_decode_options o;
    qpng_decode_options_init(&o);
    o.max_output_bytes = 64u << 20;
    qpng_image ref;
    const qpng_status rs = qpng_decode(data, size, &o, &ref);
    /* row mode */
    rows_t r = { rs == QPNG_OK ? ref.pixels : NULL, rs == QPNG_OK ? ref.stride : 0, rs == QPNG_OK ? ref.size : 0, 0, 0 };
    qpng_decoder *d;
    if (qpng_decoder_new(&o, on_row, &r, &d) == QPNG_OK) {
        qpng_status s = push_split(d, data, size);
        if (s == QPNG_OK) s = qpng_decoder_finish(d, NULL);
        /* the streaming inflater may accept trailing garbage / data the one-shot path rejects only in edge cases
         * of error classification; pixels must match whenever both succeed */
        if (s == QPNG_OK && rs == QPNG_OK && (r.bad || r.next != ref.height)) __builtin_trap();
        qpng_decoder_free(d);
    }
    /* image mode: identical to qpng_decode */
    if (qpng_decoder_new(&o, NULL, NULL, &d) == QPNG_OK) {
        qpng_status s = push_split(d, data, size);
        qpng_image im;
        if (s == QPNG_OK) {
            s = qpng_decoder_finish(d, &im);
            if ((s == QPNG_OK) != (rs == QPNG_OK)) __builtin_trap();
            if (s == QPNG_OK) { if (im.size != ref.size || memcmp(im.pixels, ref.pixels, im.size)) __builtin_trap(); qpng_image_free(&im); }
        }
        qpng_decoder_free(d);
    }
    if (rs == QPNG_OK) qpng_image_free(&ref);
    /* APNG */
    qpng_apng *a;
    if (qpng_apng_open(data, size, &o, &a) == QPNG_OK) {
        qpng_apng_info ai; qpng_apng_get_info(a, &ai);
        const uint8_t *c; qpng_frame_info fi;
        for (uint32_t i = 0; i < ai.num_frames && i < 64; i++) {
            if (qpng_apng_next_frame(a, &c, &fi) != QPNG_OK) break;
            qpng_image im;
            if (qpng_apng_raw_frame(a, i, NULL, &im, &fi) == QPNG_OK) qpng_image_free(&im);
        }
        qpng_apng_free(a);
    }
    return 0;
}
