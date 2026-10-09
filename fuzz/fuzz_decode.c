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
    return 0;
}
