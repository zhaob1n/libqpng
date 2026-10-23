/* fuzz_roundtrip.c - libFuzzer / AFL target: encode fuzzer-chosen pixels in a fuzzer-chosen format with every
 * preset / filter, decode the result (RAW) and require bit-exact pixels.  Any mismatch aborts. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "qpng.h"

static const struct { qpng_color c; int d; int ch; } FMT[] = {
    {QPNG_COLOR_GRAY, 1, 1}, {QPNG_COLOR_GRAY, 2, 1}, {QPNG_COLOR_GRAY, 4, 1}, {QPNG_COLOR_GRAY, 8, 1}, {QPNG_COLOR_GRAY, 16, 1},
    {QPNG_COLOR_RGB, 8, 3}, {QPNG_COLOR_RGB, 16, 3}, {QPNG_COLOR_PALETTE, 1, 1}, {QPNG_COLOR_PALETTE, 2, 1},
    {QPNG_COLOR_PALETTE, 4, 1}, {QPNG_COLOR_PALETTE, 8, 1}, {QPNG_COLOR_GRAY_ALPHA, 8, 2}, {QPNG_COLOR_GRAY_ALPHA, 16, 2},
    {QPNG_COLOR_RGBA, 8, 4}, {QPNG_COLOR_RGBA, 16, 4},
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4) return 0;
    const unsigned f = data[0] % (sizeof FMT / sizeof FMT[0]);
    const uint32_t w = 1 + data[1] % 64, h = 1 + data[2] % 64;
    const int knob = data[3], knob2 = data[2] ^ data[0];   /* knob2: backend, zlib strategy */
    data += 4; size -= 4;
    const size_t rowbytes = ((size_t)w * FMT[f].ch * FMT[f].d + 7) / 8, n = rowbytes * h;
    uint8_t *px = malloc(n ? n : 1);
    if (!px) return 0;
    /* pixels: the input repeated (keeps repetitive structure that exercises the match finders) */
    for (size_t i = 0; i < n; i++) px[i] = size ? data[i % size] : (uint8_t)i;
    if (FMT[f].c == QPNG_COLOR_PALETTE) {      /* keep indices inside the palette */
        if (FMT[f].d == 8) for (size_t i = 0; i < n; i++) px[i] &= 0x7f;
    }
    uint8_t pal[3 * 256];
    for (int i = 0; i < 3 * 256; i++) pal[i] = (uint8_t)(i * 7);

    qpng_encode_options eo;
    qpng_encode_options_init(&eo);
    eo.preset = (qpng_preset)(knob & 3);
    eo.filter = (knob >> 2) % 7 - 1;                                   /* AUTO, NONE..ADAPTIVE */
    static const int levels[] = {0, 0, 0, 1, 6, 9, 12, QPNG_DEFLATE_QPNG, QPNG_DEFLATE_QPNG_RLE, QPNG_DEFLATE_QPNG_HIGH, QPNG_DEFLATE_PNGULTRA};
    eo.deflate_level = levels[(knob >> 5) % 11];
    eo.backend = (int)(knob2 % 3);                               /* AUTO, LIBDEFLATE, ZLIBNG */
    if (eo.backend == QPNG_BACKEND_ZLIBNG) {
        if (!qpng_backend_available(QPNG_BACKEND_ZLIBNG)) eo.backend = QPNG_BACKEND_AUTO;
        else {
            if (eo.deflate_level > 9) eo.deflate_level = eo.deflate_level % 9 + 1;
            eo.zlib_strategy = (int)((knob2 >> 2) & 3);
        }
    }
    if (FMT[f].c == QPNG_COLOR_PALETTE) { eo.palette = pal; eo.palette_entries = 1 << (FMT[f].d < 8 ? FMT[f].d : 7); }

    uint8_t *png = NULL; size_t pn = 0;
    if (qpng_encode(px, w, h, 0, FMT[f].c, FMT[f].d, &eo, &png, &pn) != QPNG_OK) abort();
    qpng_decode_options dopt;
    qpng_decode_options_init(&dopt);
    dopt.output = QPNG_OUTPUT_RAW;
    dopt.verify_adler = 1;
    qpng_image img;
    if (qpng_decode(png, pn, &dopt, &img) != QPNG_OK) abort();
    if (img.width != w || img.height != h || img.stride != rowbytes) abort();
    for (uint32_t y = 0; y < h; y++)
        if (memcmp(img.pixels + y * img.stride, px + y * rowbytes, rowbytes)) abort();
    qpng_image_free(&img);
    qpng_buffer_free(png, NULL);
    free(px);
    return 0;
}
