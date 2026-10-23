/* API behaviour: argument validation, limits, allocator hooks, error paths. */
#include "qpng.h"
#include "test_util.h"

static size_t live, total_allocs;
static void *my_alloc(void *u, size_t n) { (void)u; live++; total_allocs++; return malloc(n); }
static void my_free(void *u, void *p) { (void)u; live--; free(p); }

int main(void) {
    int fails = 0;
    /* tiny 3x2 RGB8 image round trip with a custom allocator */
    uint8_t px[18]; for (int i = 0; i < 18; i++) px[i] = (uint8_t)(i * 13);
    qpng_allocator al = { my_alloc, my_free, NULL };
    qpng_encode_options eo; qpng_encode_options_init(&eo); eo.allocator = &al;
    uint8_t *png = NULL; size_t pn = 0;
    CHECK(qpng_encode(px, 3, 2, 0, QPNG_COLOR_RGB, 8, &eo, &png, &pn) == QPNG_OK);
    CHECK(live == 1);
    qpng_info info; CHECK(qpng_read_info(png, pn, &info) == QPNG_OK);
    CHECK(info.width == 3 && info.height == 2 && info.bit_depth == 8 && info.color_type == QPNG_COLOR_RGB && info.channels == 3);
    qpng_decode_options dopt; qpng_decode_options_init(&dopt); dopt.allocator = &al; dopt.verify_adler = 1;
    qpng_image im; CHECK(qpng_decode(png, pn, &dopt, &im) == QPNG_OK);
    CHECK(im.size == 18 && !memcmp(im.pixels, px, 18));
    qpng_image_free(&im);
    /* limits */
    dopt.max_width = 2; CHECK(qpng_decode(png, pn, &dopt, &im) == QPNG_ERR_LIMITS); dopt.max_width = 0;
    dopt.max_output_bytes = 10; CHECK(qpng_decode(png, pn, &dopt, &im) == QPNG_ERR_LIMITS); dopt.max_output_bytes = 0;
    CHECK(im.pixels == NULL);
    /* corruption: CRC, signature, truncation */
    uint8_t *bad = malloc(pn); memcpy(bad, png, pn); bad[pn - 20] ^= 1;
    CHECK(qpng_decode(bad, pn, NULL, &im) == QPNG_ERR_CRC);
    dopt.verify_crc = 0; { qpng_status s = qpng_decode(bad, pn, &dopt, &im); CHECK(s != QPNG_OK || im.pixels); qpng_image_free(&im); } dopt.verify_crc = 1;
    memcpy(bad, png, pn); bad[0] = 0; CHECK(qpng_decode(bad, pn, NULL, &im) == QPNG_ERR_SIGNATURE);
    /* any truncation inside the image data must fail (a missing IEND is tolerated, as in the png crate) */
    for (size_t cut = 0; cut < pn - 12; cut++) { qpng_status s = qpng_decode(png, cut, NULL, &im); CHECK(s != QPNG_OK); }
    free(bad);
    CHECK(live == 1);   /* only the encoded PNG is still allocated: no leaks on error paths */
    qpng_buffer_free(png, &al);
    CHECK(live == 0 && total_allocs > 2);
    /* argument validation */
    CHECK(qpng_encode(px, 3, 2, 0, QPNG_COLOR_RGB, 4, NULL, &png, &pn) == QPNG_ERR_ARGUMENT);
    CHECK(qpng_encode(px, 3, 2, 0, QPNG_COLOR_PALETTE, 8, NULL, &png, &pn) == QPNG_ERR_ARGUMENT);   /* no palette */
    CHECK(qpng_encode(px, 0, 2, 0, QPNG_COLOR_RGB, 8, NULL, &png, &pn) == QPNG_ERR_ARGUMENT);
    CHECK(qpng_encode(px, 3, 2, 5, QPNG_COLOR_RGB, 8, NULL, &png, &pn) == QPNG_ERR_ARGUMENT);       /* stride < row */
    CHECK(qpng_decode(NULL, 0, NULL, &im) == QPNG_ERR_ARGUMENT);
    {   /* deflate backend options */
        qpng_encode_options e; qpng_encode_options_init(&e);
        CHECK(e.backend == QPNG_BACKEND_AUTO && e.zlib_strategy == QPNG_ZSTRATEGY_DEFAULT);
        CHECK(qpng_backend_available(QPNG_BACKEND_AUTO) && qpng_backend_available(QPNG_BACKEND_LIBDEFLATE));
        e.backend = 3; CHECK(qpng_encode(px, 3, 2, 0, QPNG_COLOR_RGB, 8, &e, &png, &pn) == QPNG_ERR_ARGUMENT);
        e.backend = QPNG_BACKEND_AUTO; e.zlib_strategy = 4;
        CHECK(qpng_encode(px, 3, 2, 0, QPNG_COLOR_RGB, 8, &e, &png, &pn) == QPNG_ERR_ARGUMENT);
        e.zlib_strategy = 0; e.backend = QPNG_BACKEND_ZLIBNG;
        if (qpng_backend_available(QPNG_BACKEND_ZLIBNG)) {
            e.deflate_level = 10; CHECK(qpng_encode(px, 3, 2, 0, QPNG_COLOR_RGB, 8, &e, &png, &pn) == QPNG_ERR_ARGUMENT);
            e.deflate_level = QPNG_DEFLATE_QPNG; CHECK(qpng_encode(px, 3, 2, 0, QPNG_COLOR_RGB, 8, &e, &png, &pn) == QPNG_ERR_ARGUMENT);
            e.deflate_level = 9; e.zlib_strategy = QPNG_ZSTRATEGY_RLE; e.allocator = &al;   /* zlib-ng uses the hooks */
            const size_t a0 = (size_t)total_allocs;
            CHECK(qpng_encode(px, 3, 2, 0, QPNG_COLOR_RGB, 8, &e, &png, &pn) == QPNG_OK);
            CHECK(live == 1 && total_allocs > a0 + 2); qpng_buffer_free(png, &al); CHECK(live == 0);
        } else {
            CHECK(qpng_encode(px, 3, 2, 0, QPNG_COLOR_RGB, 8, &e, &png, &pn) == QPNG_ERR_UNSUPPORTED);
        }
    }
    CHECK(strcmp(qpng_status_string(QPNG_ERR_CRC), "chunk CRC mismatch") == 0);
    /* stride > row bytes + every preset on a padded buffer */
    uint8_t padded[2 * 32]; memset(padded, 0xAB, sizeof padded); memcpy(padded, px, 9); memcpy(padded + 32, px + 9, 9);
    for (int p = 0; p < 4; p++) {
        qpng_encode_options o2; qpng_encode_options_init(&o2); o2.preset = (qpng_preset)p;
        CHECK(qpng_encode(padded, 3, 2, 32, QPNG_COLOR_RGB, 8, &o2, &png, &pn) == QPNG_OK);
        CHECK(qpng_decode(png, pn, NULL, &im) == QPNG_OK && im.size == 18 && !memcmp(im.pixels, px, 18));
        qpng_image_free(&im); qpng_buffer_free(png, NULL);
    }
    printf("api: %d failures\n", fails);
    return fails != 0;
}
