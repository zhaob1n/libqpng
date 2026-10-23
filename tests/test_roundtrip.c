/* Round trip: decode (raw) -> encode with every preset and every fixed filter -> decode -> identical samples.
 * Covers every color type / bit depth in the corpus (palette with PLTE/tRNS carried over). */
#include "qpng.h"
#include "test_util.h"
#ifdef QPNG_TEST_ZLIB
#include <zlib.h>
/* the IDAT stream must also inflate with stock zlib, to h * (1 + rowbytes) bytes */
static int zlib_ok(const uint8_t *png, size_t pn, const qpng_image *im) {
    static const int ch[7] = { 1, 0, 3, 1, 2, 0, 4 };
    const size_t raw = (size_t)im->height * (1 + ((size_t)im->width * ch[im->color_type] * im->bit_depth + 7) / 8);
    z_stream s; memset(&s, 0, sizeof s);
    uint8_t *out = malloc(raw + 1);
    int ok = out && inflateInit(&s) == Z_OK, r = Z_OK;
    s.next_out = out; s.avail_out = (uInt)raw + 1;
    for (size_t i = 8; ok && i + 12 <= pn;) {
        uint32_t len = (uint32_t)png[i] << 24 | (uint32_t)png[i + 1] << 16 | (uint32_t)png[i + 2] << 8 | png[i + 3];
        if (!memcmp(png + i + 4, "IDAT", 4)) {
            s.next_in = (uint8_t *)png + i + 8; s.avail_in = len;
            r = inflate(&s, Z_NO_FLUSH);
            if (r != Z_OK && r != Z_STREAM_END) ok = 0;
        }
        i += 12 + (size_t)len;
    }
    ok = ok && r == Z_STREAM_END && s.total_out == raw;
    inflateEnd(&s); free(out);
    return ok;
}
#else
static int zlib_ok(const uint8_t *png, size_t pn, const qpng_image *im) { (void)png; (void)pn; (void)im; return 1; }
#endif

int main(int argc, char **argv) {
    int fails = 0, runs = 0;
    for (int fi = 1; fi < argc; fi++) {
        size_t n; unsigned char *buf = slurp(argv[fi], &n);
        if (!buf) { fprintf(stderr, "missing %s\n", argv[fi]); fails++; continue; }
        qpng_image src; if (qpng_decode(buf, n, NULL, &src)) { free(buf); continue; }   /* invalid inputs are skipped */
        qpng_encode_options o; qpng_encode_options_init(&o);
        int pl = 0, tl = 0;
        o.palette = find_chunk(buf, n, "PLTE", &pl); o.palette_entries = o.palette ? pl / 3 : 0;
        o.trns = find_chunk(buf, n, "tRNS", &tl); o.trns_bytes = o.trns ? tl : 0;
        if (src.color_type != QPNG_COLOR_PALETTE) { o.palette = NULL; o.palette_entries = 0; }   /* suggested PLTE (e.g. ps1n0g08) */
        if (o.trns && ((src.color_type == QPNG_COLOR_GRAY && tl != 2) || (src.color_type == QPNG_COLOR_RGB && tl != 6) ||
                       (src.color_type == QPNG_COLOR_PALETTE && tl > o.palette_entries) || src.color_type == QPNG_COLOR_RGBA ||
                       src.color_type == QPNG_COLOR_GRAY_ALPHA)) { o.trns = NULL; o.trns_bytes = 0; }
        /* variants: 4 presets (AUTO backend), 6 fixed filters, 4 libdeflate levels, 4 presets forced to libdeflate,
         * then (if built) 4 presets forced to zlib-ng and 5 explicit zlib-ng level / strategy combinations */
        const int nv = 4 + 6 + 4 + 4 + (qpng_backend_available(QPNG_BACKEND_ZLIBNG) ? 4 + 5 : 0);
        for (int v = 0; v < nv; v++) {
            qpng_encode_options e = o;
            if (v < 4) e.preset = (qpng_preset)v;
            else if (v < 10) { e.preset = QPNG_PRESET_FAST; e.filter = v - 4; }
            else if (v < 14) e.deflate_level = (int[]){ QPNG_DEFLATE_QPNG_RLE, QPNG_DEFLATE_QPNG_HIGH, 1, QPNG_DEFLATE_PNGULTRA }[v - 10];
            else if (v < 18) { e.preset = (qpng_preset)(v - 14); e.backend = QPNG_BACKEND_LIBDEFLATE; }
            else if (v < 22) { e.preset = (qpng_preset)(v - 18); e.backend = QPNG_BACKEND_ZLIBNG; }
            else {
                static const int zl[5][2] = { { 1, QPNG_ZSTRATEGY_DEFAULT }, { 4, QPNG_ZSTRATEGY_FILTERED },
                                              { 9, QPNG_ZSTRATEGY_DEFAULT }, { 6, QPNG_ZSTRATEGY_RLE }, { 3, QPNG_ZSTRATEGY_HUFFMAN_ONLY } };
                e.backend = QPNG_BACKEND_ZLIBNG; e.deflate_level = zl[v - 22][0]; e.zlib_strategy = zl[v - 22][1];
                e.filter = v & 1 ? QPNG_FILTER_ADAPTIVE : QPNG_FILTER_PAETH;
            }
            uint8_t *png; size_t pn;
            qpng_status s = qpng_encode(src.pixels, src.width, src.height, src.stride, (qpng_color)src.color_type, src.bit_depth, &e, &png, &pn);
            runs++;
            if (s) { fprintf(stderr, "encode %s v%d: %s\n", argv[fi], v, qpng_status_string(s)); fails++; continue; }
            if (!zlib_ok(png, pn, &src)) { fprintf(stderr, "ZLIB REJECTS %s variant %d\n", argv[fi], v); fails++; }
            qpng_image back; s = qpng_decode(png, pn, NULL, &back);
            if (s || back.size != src.size || back.width != src.width || back.color_type != src.color_type ||
                back.bit_depth != src.bit_depth || memcmp(back.pixels, src.pixels, src.size)) {
                fprintf(stderr, "ROUNDTRIP MISMATCH %s variant %d (%s)\n", argv[fi], v, s ? qpng_status_string(s) : "pixels"); fails++;
            }
            qpng_image_free(&back); qpng_buffer_free(png, NULL);
        }
        qpng_image_free(&src); free(buf);
    }
    printf("roundtrip: %d encodes, %d failures\n", runs, fails);
    return fails != 0;
}
