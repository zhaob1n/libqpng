/* fuzz_roundtrip.c - libFuzzer / AFL target: encode fuzzer-chosen pixels in a fuzzer-chosen format with every
 * preset / filter, decode the result (RAW) and require bit-exact pixels.  Any mismatch aborts. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "pngfast.h"

static const struct { pf_color c; int d; int ch; } FMT[] = {
    {PF_COLOR_GRAY, 1, 1}, {PF_COLOR_GRAY, 2, 1}, {PF_COLOR_GRAY, 4, 1}, {PF_COLOR_GRAY, 8, 1}, {PF_COLOR_GRAY, 16, 1},
    {PF_COLOR_RGB, 8, 3}, {PF_COLOR_RGB, 16, 3}, {PF_COLOR_PALETTE, 1, 1}, {PF_COLOR_PALETTE, 2, 1},
    {PF_COLOR_PALETTE, 4, 1}, {PF_COLOR_PALETTE, 8, 1}, {PF_COLOR_GRAY_ALPHA, 8, 2}, {PF_COLOR_GRAY_ALPHA, 16, 2},
    {PF_COLOR_RGBA, 8, 4}, {PF_COLOR_RGBA, 16, 4},
};

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    if (size < 4) return 0;
    const unsigned f = data[0] % (sizeof FMT / sizeof FMT[0]);
    const uint32_t w = 1 + data[1] % 64, h = 1 + data[2] % 64;
    const int knob = data[3];
    data += 4; size -= 4;
    const size_t rowbytes = ((size_t)w * FMT[f].ch * FMT[f].d + 7) / 8, n = rowbytes * h;
    uint8_t *px = malloc(n ? n : 1);
    if (!px) return 0;
    /* pixels: the input repeated (keeps repetitive structure that exercises the match finders) */
    for (size_t i = 0; i < n; i++) px[i] = size ? data[i % size] : (uint8_t)i;
    if (FMT[f].c == PF_COLOR_PALETTE) {      /* keep indices inside the palette */
        if (FMT[f].d == 8) for (size_t i = 0; i < n; i++) px[i] &= 0x7f;
    }
    uint8_t pal[3 * 256];
    for (int i = 0; i < 3 * 256; i++) pal[i] = (uint8_t)(i * 7);

    pf_encode_options eo;
    pf_encode_options_init(&eo);
    eo.preset = (pf_preset)(knob & 3);
    eo.filter = (knob >> 2) % 7 - 1;                                   /* AUTO, NONE..ADAPTIVE */
    static const int levels[] = {0, 0, 0, 1, 6, 9, 12, PF_DEFLATE_PNGFAST, PF_DEFLATE_PNGFAST_RLE, PF_DEFLATE_PNGFAST_HIGH};
    eo.deflate_level = levels[(knob >> 5) % 10];
    if (FMT[f].c == PF_COLOR_PALETTE) { eo.palette = pal; eo.palette_entries = 1 << (FMT[f].d < 8 ? FMT[f].d : 7); }

    uint8_t *png = NULL; size_t pn = 0;
    if (pf_encode(px, w, h, 0, FMT[f].c, FMT[f].d, &eo, &png, &pn) != PF_OK) abort();
    pf_decode_options dopt;
    pf_decode_options_init(&dopt);
    dopt.output = PF_OUTPUT_RAW;
    dopt.verify_adler = 1;
    pf_image img;
    if (pf_decode(png, pn, &dopt, &img) != PF_OK) abort();
    if (img.width != w || img.height != h || img.stride != rowbytes) abort();
    for (uint32_t y = 0; y < h; y++)
        if (memcmp(img.pixels + y * img.stride, px + y * rowbytes, rowbytes)) abort();
    pf_image_free(&img);
    pf_buffer_free(png, NULL);
    free(px);
    return 0;
}
