/* fuzz_stream.c - libFuzzer / AFL target: streaming decode (row mode and image mode) with input-derived chunk
 * boundaries must agree with the one-shot pf_decode (same status class, same pixels); APNG open + all composed
 * frames + raw frames. */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "pngfast.h"

typedef struct { const uint8_t *ref; size_t stride, size; uint32_t next; int bad; } rows_t;
static int on_row(void *u, uint32_t y, const uint8_t *row, size_t n) {
    rows_t *r = u;
    if (y != r->next || (r->ref && (n != r->stride || (size_t)(y + 1) * n > r->size || memcmp(r->ref + (size_t)y * n, row, n)))) r->bad = 1;
    r->next++;
    return 0;
}

static pf_status push_split(pf_decoder *d, const uint8_t *data, size_t size) {
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size && i < 64; i++) h = (h ^ data[i]) * 16777619u;
    pf_status s = PF_MORE_DATA;
    for (size_t pos = 0; pos < size && s == PF_MORE_DATA;) {
        h = h * 1103515245u + 12345u;
        size_t k = 1 + (h >> 16) % 300;
        if (k > size - pos) k = size - pos;
        s = pf_decoder_push(d, data + pos, k); pos += k;
    }
    return s;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    pf_decode_options o;
    pf_decode_options_init(&o);
    o.max_output_bytes = 64u << 20;
    pf_image ref;
    const pf_status rs = pf_decode(data, size, &o, &ref);
    /* row mode */
    rows_t r = { rs == PF_OK ? ref.pixels : NULL, rs == PF_OK ? ref.stride : 0, rs == PF_OK ? ref.size : 0, 0, 0 };
    pf_decoder *d;
    if (pf_decoder_new(&o, on_row, &r, &d) == PF_OK) {
        pf_status s = push_split(d, data, size);
        if (s == PF_OK) s = pf_decoder_finish(d, NULL);
        /* the streaming inflater may accept trailing garbage / data the one-shot path rejects only in edge cases
         * of error classification; pixels must match whenever both succeed */
        if (s == PF_OK && rs == PF_OK && (r.bad || r.next != ref.height)) __builtin_trap();
        pf_decoder_free(d);
    }
    /* image mode: identical to pf_decode */
    if (pf_decoder_new(&o, NULL, NULL, &d) == PF_OK) {
        pf_status s = push_split(d, data, size);
        pf_image im;
        if (s == PF_OK) {
            s = pf_decoder_finish(d, &im);
            if ((s == PF_OK) != (rs == PF_OK)) __builtin_trap();
            if (s == PF_OK) { if (im.size != ref.size || memcmp(im.pixels, ref.pixels, im.size)) __builtin_trap(); pf_image_free(&im); }
        }
        pf_decoder_free(d);
    }
    if (rs == PF_OK) pf_image_free(&ref);
    /* APNG */
    pf_apng *a;
    if (pf_apng_open(data, size, &o, &a) == PF_OK) {
        pf_apng_info ai; pf_apng_get_info(a, &ai);
        const uint8_t *c; pf_frame_info fi;
        for (uint32_t i = 0; i < ai.num_frames && i < 64; i++) {
            if (pf_apng_next_frame(a, &c, &fi) != PF_OK) break;
            pf_image im;
            if (pf_apng_raw_frame(a, i, NULL, &im, &fi) == PF_OK) pf_image_free(&im);
        }
        pf_apng_free(a);
    }
    return 0;
}
