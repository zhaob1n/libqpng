/* fuzz_decode.c - libFuzzer / AFL target: decode arbitrary bytes in all output modes.
 * Build: see fuzz/README.md (clang -fsanitize=fuzzer,address,undefined, or standalone driver for AFL). */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "pngfast.h"

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size) {
    pf_info info;
    if (pf_read_info(data, size, &info) != PF_OK) return 0;
    for (int mode = 0; mode < 3; mode++) {
        pf_decode_options o;
        pf_decode_options_init(&o);
        o.output = (pf_output)mode;
        o.verify_crc = mode != 1;          /* exercise both CRC paths */
        o.verify_adler = mode == 2;
        o.max_output_bytes = 64u << 20;    /* keep the fuzzer's RSS bounded */
        pf_image img;
        if (pf_decode(data, size, &o, &img) == PF_OK) {
            /* touch every byte so ASan/MSan see the whole buffer */
            volatile uint8_t acc = 0;
            for (size_t i = 0; i < img.size; i++) acc ^= img.pixels[i];
            (void)acc;
            pf_image_free(&img);
        }
    }
    /* metadata: parse (CRCs not verified so mutations reach the parsers, small limits), write back into a 1x1
     * image of the same color type, parse again: same number of texts / chunks */
    pf_metadata_options mo;
    pf_metadata_options_init(&mo);
    mo.verify_crc = 0; mo.max_chunk_decompressed = 1u << 20; mo.max_total = 8u << 20;
    pf_metadata m;
    if (pf_read_metadata(data, size, &mo, &m) == PF_OK) {
        static const uint8_t px[8] = { 0 }, pal[3] = { 0 };
        pf_encode_options eo;
        pf_encode_options_init(&eo);
        eo.metadata = &m; eo.preset = PF_PRESET_FASTEST;
        if (info.color_type == PF_COLOR_PALETTE) { eo.palette = pal; eo.palette_entries = 1; }
        const int depth = info.color_type == PF_COLOR_PALETTE || info.bit_depth < 8 ? 8 : info.bit_depth;
        uint8_t *png; size_t pn;
        if (pf_encode(px, 1, 1, 0, (pf_color)info.color_type, depth, &eo, &png, &pn) == PF_OK) {
            pf_metadata r;
            if (pf_read_metadata(png, pn, &mo, &r) != PF_OK || r.text_count != m.text_count || r.chunk_count != m.chunk_count)
                __builtin_trap();
            pf_metadata_free(&r);
            pf_buffer_free(png, NULL);
        }
        pf_metadata_free(&m);
    }
    return 0;
}
