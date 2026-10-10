/* API behaviour: argument validation, limits, allocator hooks, error paths. */
#include "pngfast.h"
#include "test_util.h"

static size_t live, total_allocs;
static void *my_alloc(void *u, size_t n) { (void)u; live++; total_allocs++; return malloc(n); }
static void my_free(void *u, void *p) { (void)u; live--; free(p); }

int main(void) {
    int fails = 0;
    /* tiny 3x2 RGB8 image round trip with a custom allocator */
    uint8_t px[18]; for (int i = 0; i < 18; i++) px[i] = (uint8_t)(i * 13);
    pf_allocator al = { my_alloc, my_free, NULL };
    pf_encode_options eo; pf_encode_options_init(&eo); eo.allocator = &al;
    uint8_t *png = NULL; size_t pn = 0;
    CHECK(pf_encode(px, 3, 2, 0, PF_COLOR_RGB, 8, &eo, &png, &pn) == PF_OK);
    CHECK(live == 1);
    pf_info info; CHECK(pf_read_info(png, pn, &info) == PF_OK);
    CHECK(info.width == 3 && info.height == 2 && info.bit_depth == 8 && info.color_type == PF_COLOR_RGB && info.channels == 3);
    pf_decode_options dopt; pf_decode_options_init(&dopt); dopt.allocator = &al; dopt.verify_adler = 1;
    pf_image im; CHECK(pf_decode(png, pn, &dopt, &im) == PF_OK);
    CHECK(im.size == 18 && !memcmp(im.pixels, px, 18));
    pf_image_free(&im);
    /* limits */
    dopt.max_width = 2; CHECK(pf_decode(png, pn, &dopt, &im) == PF_ERR_LIMITS); dopt.max_width = 0;
    dopt.max_output_bytes = 10; CHECK(pf_decode(png, pn, &dopt, &im) == PF_ERR_LIMITS); dopt.max_output_bytes = 0;
    CHECK(im.pixels == NULL);
    /* corruption: CRC, signature, truncation */
    uint8_t *bad = malloc(pn); memcpy(bad, png, pn); bad[pn - 20] ^= 1;
    CHECK(pf_decode(bad, pn, NULL, &im) == PF_ERR_CRC);
    dopt.verify_crc = 0; { pf_status s = pf_decode(bad, pn, &dopt, &im); CHECK(s != PF_OK || im.pixels); pf_image_free(&im); } dopt.verify_crc = 1;
    memcpy(bad, png, pn); bad[0] = 0; CHECK(pf_decode(bad, pn, NULL, &im) == PF_ERR_SIGNATURE);
    /* any truncation inside the image data must fail (a missing IEND is tolerated, as in the png crate) */
    for (size_t cut = 0; cut < pn - 12; cut++) { pf_status s = pf_decode(png, cut, NULL, &im); CHECK(s != PF_OK); }
    free(bad);
    CHECK(live == 1);   /* only the encoded PNG is still allocated: no leaks on error paths */
    pf_buffer_free(png, &al);
    CHECK(live == 0 && total_allocs > 2);
    /* argument validation */
    CHECK(pf_encode(px, 3, 2, 0, PF_COLOR_RGB, 4, NULL, &png, &pn) == PF_ERR_ARGUMENT);
    CHECK(pf_encode(px, 3, 2, 0, PF_COLOR_PALETTE, 8, NULL, &png, &pn) == PF_ERR_ARGUMENT);   /* no palette */
    CHECK(pf_encode(px, 0, 2, 0, PF_COLOR_RGB, 8, NULL, &png, &pn) == PF_ERR_ARGUMENT);
    CHECK(pf_encode(px, 3, 2, 5, PF_COLOR_RGB, 8, NULL, &png, &pn) == PF_ERR_ARGUMENT);       /* stride < row */
    CHECK(pf_decode(NULL, 0, NULL, &im) == PF_ERR_ARGUMENT);
    {   /* deflate backend options */
        pf_encode_options e; pf_encode_options_init(&e);
        CHECK(e.backend == PF_BACKEND_AUTO && e.zlib_strategy == PF_ZSTRATEGY_DEFAULT);
        CHECK(pf_backend_available(PF_BACKEND_AUTO) && pf_backend_available(PF_BACKEND_LIBDEFLATE));
        e.backend = 3; CHECK(pf_encode(px, 3, 2, 0, PF_COLOR_RGB, 8, &e, &png, &pn) == PF_ERR_ARGUMENT);
        e.backend = PF_BACKEND_AUTO; e.zlib_strategy = 4;
        CHECK(pf_encode(px, 3, 2, 0, PF_COLOR_RGB, 8, &e, &png, &pn) == PF_ERR_ARGUMENT);
        e.zlib_strategy = 0; e.backend = PF_BACKEND_ZLIBNG;
        if (pf_backend_available(PF_BACKEND_ZLIBNG)) {
            e.deflate_level = 10; CHECK(pf_encode(px, 3, 2, 0, PF_COLOR_RGB, 8, &e, &png, &pn) == PF_ERR_ARGUMENT);
            e.deflate_level = PF_DEFLATE_PNGFAST; CHECK(pf_encode(px, 3, 2, 0, PF_COLOR_RGB, 8, &e, &png, &pn) == PF_ERR_ARGUMENT);
            e.deflate_level = 9; e.zlib_strategy = PF_ZSTRATEGY_RLE; e.allocator = &al;   /* zlib-ng uses the hooks */
            const size_t a0 = (size_t)total_allocs;
            CHECK(pf_encode(px, 3, 2, 0, PF_COLOR_RGB, 8, &e, &png, &pn) == PF_OK);
            CHECK(live == 1 && total_allocs > a0 + 2); pf_buffer_free(png, &al); CHECK(live == 0);
        } else {
            CHECK(pf_encode(px, 3, 2, 0, PF_COLOR_RGB, 8, &e, &png, &pn) == PF_ERR_UNSUPPORTED);
        }
    }
    CHECK(strcmp(pf_status_string(PF_ERR_CRC), "chunk CRC mismatch") == 0);
    /* stride > row bytes + every preset on a padded buffer */
    uint8_t padded[2 * 32]; memset(padded, 0xAB, sizeof padded); memcpy(padded, px, 9); memcpy(padded + 32, px + 9, 9);
    for (int p = 0; p < 4; p++) {
        pf_encode_options o2; pf_encode_options_init(&o2); o2.preset = (pf_preset)p;
        CHECK(pf_encode(padded, 3, 2, 32, PF_COLOR_RGB, 8, &o2, &png, &pn) == PF_OK);
        CHECK(pf_decode(png, pn, NULL, &im) == PF_OK && im.size == 18 && !memcmp(im.pixels, px, 18));
        pf_image_free(&im); pf_buffer_free(png, NULL);
    }
    printf("api: %d failures\n", fails);
    return fails != 0;
}
