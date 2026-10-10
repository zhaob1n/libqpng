/* link_test.c - see link_test.sh: pngfast next to stock zlib-ng / libdeflate (or zlib-compat zlib-ng). */
#include <stdio.h>
#include <string.h>
#include "pngfast.h"
#ifdef PF_LINK_ZLIB_COMPAT
#  include <zlib.h>
#else
#  include <zlib-ng.h>
#  include "libdeflate.h"
#endif

int main(void) {
    static uint8_t px[64 * 64 * 3], z[65536], back[sizeof px];
    for (size_t i = 0; i < sizeof px; i++) px[i] = (uint8_t)(i * 7 / 5);
    int ok = 1;
    for (int b = PF_BACKEND_AUTO; b <= PF_BACKEND_ZLIBNG; b++) {
        if (!pf_backend_available((pf_backend)b)) continue;
        pf_encode_options o; pf_encode_options_init(&o); o.backend = b; o.preset = PF_PRESET_BEST;
        uint8_t *png; size_t pn; pf_image im;
        ok &= pf_encode(px, 64, 64, 0, PF_COLOR_RGB, 8, &o, &png, &pn) == PF_OK;
        ok &= pf_decode(png, pn, NULL, &im) == PF_OK && im.size == sizeof px && !memcmp(im.pixels, px, sizeof px);
        pf_image_free(&im); pf_buffer_free(png, NULL);
    }
#ifdef PF_LINK_ZLIB_COMPAT
    uLongf zn = sizeof z, bn = sizeof back;
    ok &= compress2(z, &zn, px, sizeof px, 9) == Z_OK && uncompress(back, &bn, z, zn) == Z_OK && bn == sizeof px;
#else
    size_t zn = sizeof z, bn = sizeof back;
    ok &= zng_compress2(z, &zn, px, sizeof px, 9) == Z_OK && zng_uncompress(back, &bn, z, zn) == Z_OK && bn == sizeof px;
    struct libdeflate_compressor *c = libdeflate_alloc_compressor(6);
    struct libdeflate_decompressor *d = libdeflate_alloc_decompressor();
    size_t n2 = libdeflate_zlib_compress(c, px, sizeof px, z, sizeof z), got = 0;
    ok &= n2 && libdeflate_zlib_decompress(d, z, n2, back, sizeof back, &got) == LIBDEFLATE_SUCCESS && got == sizeof px;
    libdeflate_free_compressor(c); libdeflate_free_decompressor(d);
#endif
    ok &= !memcmp(back, px, sizeof px);
    printf("link_test: %s\n", ok ? "ok" : "FAILED");
    return !ok;
}
