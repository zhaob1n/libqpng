/* Round trip: decode (raw) -> encode with every preset and every fixed filter -> decode -> identical samples.
 * Covers every color type / bit depth in the corpus (palette with PLTE/tRNS carried over). */
#include "qpng.h"
#include "test_common.h"
#ifdef QPNG_TEST_ZLIB
#include <zlib.h>
/* the IDAT stream must also inflate with stock zlib, to h * (1 + rowbytes) bytes */
static bool zlib_ok(size_t pn, const uint8_t png[static pn], const qpng_image im[static 1]) {
    static constexpr int ch[7] = { 1, 0, 3, 1, 2, 0, 4 };
    const size_t raw = (size_t)im->height * (1 + ((size_t)im->width * (size_t)ch[im->color_type] * im->bit_depth + 7) / 8);
    z_stream s = {};
    qpng_tool_autofree uint8_t *out = malloc(raw + 1);
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
    inflateEnd(&s);
    return ok != 0;
}
#else
static bool zlib_ok([[maybe_unused]] size_t pn, [[maybe_unused]] const uint8_t png[static pn],
                    [[maybe_unused]] const qpng_image im[static 1]) { return true; }
#endif

int main(int argc, char **argv) {
    int fails = 0, runs = 0;
    for (int fi = 1; fi < argc; fi++) {
        size_t n = 0;
        qpng_tool_autofree uint8_t *buf = slurp(argv[fi], &n);
        if (!buf) { fprintf(stderr, "missing %s\n", argv[fi]); fails++; continue; }
        qpng_image src = {};
        if (qpng_decode(n, buf, nullptr, &src)) continue;   /* invalid inputs are skipped */
        qpng_encode_options o = {}; qpng_encode_options_init(&o);
        qpng_tool_carry_plte_trns(n, buf, src.color_type, &o);
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
            if (!zlib_ok(pn, png, &src)) { fprintf(stderr, "ZLIB REJECTS %s variant %d\n", argv[fi], v); fails++; }
            qpng_image back; s = qpng_decode(pn, png, nullptr, &back);
            if (s || back.size != src.size || back.width != src.width || back.color_type != src.color_type ||
                back.bit_depth != src.bit_depth || memcmp(back.pixels, src.pixels, src.size)) {
                fprintf(stderr, "ROUNDTRIP MISMATCH %s variant %d (%s)\n", argv[fi], v, s ? qpng_status_string(s) : "pixels"); fails++;
            }
            qpng_image_free(&back); qpng_buffer_free(png, nullptr);
        }
        qpng_image_free(&src);
    }
    printf("roundtrip: %d encodes, %d failures\n", runs, fails);
    return fails != 0;
}
