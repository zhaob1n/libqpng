/* Round trip: decode (raw) -> encode with every preset and every fixed filter -> decode -> identical samples.
 * Covers every color type / bit depth in the corpus (palette with PLTE/tRNS carried over). */
#include "pngfast.h"
#include "test_util.h"

int main(int argc, char **argv) {
    int fails = 0, runs = 0;
    for (int fi = 1; fi < argc; fi++) {
        size_t n; unsigned char *buf = slurp(argv[fi], &n);
        if (!buf) { fprintf(stderr, "missing %s\n", argv[fi]); fails++; continue; }
        pf_image src; if (pf_decode(buf, n, NULL, &src)) { free(buf); continue; }   /* invalid inputs are skipped */
        pf_encode_options o; pf_encode_options_init(&o);
        int pl = 0, tl = 0;
        o.palette = find_chunk(buf, n, "PLTE", &pl); o.palette_entries = o.palette ? pl / 3 : 0;
        o.trns = find_chunk(buf, n, "tRNS", &tl); o.trns_bytes = o.trns ? tl : 0;
        if (src.color_type != PF_COLOR_PALETTE) { o.palette = NULL; o.palette_entries = 0; }   /* suggested PLTE (e.g. ps1n0g08) */
        if (o.trns && ((src.color_type == PF_COLOR_GRAY && tl != 2) || (src.color_type == PF_COLOR_RGB && tl != 6) ||
                       (src.color_type == PF_COLOR_PALETTE && tl > o.palette_entries) || src.color_type == PF_COLOR_RGBA ||
                       src.color_type == PF_COLOR_GRAY_ALPHA)) { o.trns = NULL; o.trns_bytes = 0; }
        for (int v = 0; v < 4 + 6 + 3; v++) {
            pf_encode_options e = o;
            if (v < 4) e.preset = (pf_preset)v;
            else if (v < 10) { e.preset = PF_PRESET_FAST; e.filter = v - 4; }
            else e.deflate_level = (int[]){ PF_DEFLATE_PNGFAST_RLE, PF_DEFLATE_PNGFAST_HIGH, 1 }[v - 10];
            uint8_t *png; size_t pn;
            pf_status s = pf_encode(src.pixels, src.width, src.height, src.stride, (pf_color)src.color_type, src.bit_depth, &e, &png, &pn);
            runs++;
            if (s) { fprintf(stderr, "encode %s v%d: %s\n", argv[fi], v, pf_status_string(s)); fails++; continue; }
            pf_image back; s = pf_decode(png, pn, NULL, &back);
            if (s || back.size != src.size || back.width != src.width || back.color_type != src.color_type ||
                back.bit_depth != src.bit_depth || memcmp(back.pixels, src.pixels, src.size)) {
                fprintf(stderr, "ROUNDTRIP MISMATCH %s variant %d (%s)\n", argv[fi], v, s ? pf_status_string(s) : "pixels"); fails++;
            }
            pf_image_free(&back); pf_buffer_free(png, NULL);
        }
        pf_image_free(&src); free(buf);
    }
    printf("roundtrip: %d encodes, %d failures\n", runs, fails);
    return fails != 0;
}
