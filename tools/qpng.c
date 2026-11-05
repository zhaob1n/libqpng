/* qpng - small command-line front end (also used by the tests).
 *   qpng info IN.png
 *   qpng decode IN.png OUT.raw [raw|expand|imagers] [--adler]   (writes the pixel buffer; prints geometry)
 *   qpng encode IN.png OUT.png [fastest|fast|default|best] [filter 0..5|-1] [level]
 *       (IN.png is decoded to raw samples, then re-encoded with the same color type / depth / PLTE / tRNS)
 */
#include "qpng.h"
#include "util.h"

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: see source header\n"); return 2; }
    size_t n = 0;
    qpng_tool_autofree uint8_t *buf = qpng_tool_slurp(argv[2], &n);
    if (!buf) { perror(argv[2]); return 2; }

    if (!strcmp(argv[1], "info")) {
        qpng_info in = {};
        qpng_status s = qpng_read_info(buf, n, &in);
        if (s) { printf("error: %s\n", qpng_status_string(s)); return 1; }
        printf("%ux%u depth %u color %u interlaced %u channels %u plte %u trns %u\n", in.width, in.height, in.bit_depth,
               in.color_type, in.interlaced, in.channels, in.palette_entries, in.trns_bytes);
        return 0;
    }
    if (!strcmp(argv[1], "decode") && argc >= 4) {
        qpng_decode_options o = {};
        qpng_decode_options_init(&o);
        if (argc > 4)
            o.output = !strcmp(argv[4], "expand")   ? QPNG_OUTPUT_EXPAND
                     : !strcmp(argv[4], "imagers")  ? QPNG_OUTPUT_IMAGERS
                                                    : QPNG_OUTPUT_RAW;
        if (argc > 5 && !strcmp(argv[5], "--adler")) o.verify_adler = 1;
        qpng_image im = {};
        qpng_status s = qpng_decode(buf, n, &o, &im);
        if (s) { fprintf(stderr, "decode error: %s\n", qpng_status_string(s)); return 1; }
        printf("%u %u %u %u %u\n", im.width, im.height, im.channels, im.bit_depth, im.color_type);
        int r = qpng_tool_spit(argv[3], im.pixels, im.size);
        qpng_image_free(&im);
        return r;
    }
    if (!strcmp(argv[1], "encode") && argc >= 4) {
        qpng_image im = {};
        qpng_status s = qpng_decode(buf, n, nullptr, &im);
        if (s) { fprintf(stderr, "decode error: %s\n", qpng_status_string(s)); return 1; }
        qpng_encode_options o = {};
        qpng_encode_options_init(&o);
        if (argc > 4)
            o.preset = !strcmp(argv[4], "fastest") ? QPNG_PRESET_FASTEST
                     : !strcmp(argv[4], "fast")    ? QPNG_PRESET_FAST
                     : !strcmp(argv[4], "best")    ? QPNG_PRESET_BEST
                                                   : QPNG_PRESET_DEFAULT;
        if (argc > 5) o.filter = atoi(argv[5]);
        if (argc > 6) o.deflate_level = atoi(argv[6]);
        int pl = 0, tl = 0;
        o.palette = qpng_tool_find_chunk(buf, n, "PLTE", &pl);
        o.palette_entries = o.palette ? pl / 3 : 0;
        o.trns = qpng_tool_find_chunk(buf, n, "tRNS", &tl);
        o.trns_bytes = o.trns ? tl : 0;
        uint8_t *png = nullptr;
        size_t pn = 0;
        s = qpng_encode(im.pixels, im.width, im.height, im.stride, (qpng_color)im.color_type, im.bit_depth, &o, &png, &pn);
        qpng_image_free(&im);
        if (s) { fprintf(stderr, "encode error: %s\n", qpng_status_string(s)); return 1; }
        int r = qpng_tool_spit(argv[3], png, pn);
        qpng_buffer_free(png, nullptr);
        return r;
    }
    fprintf(stderr, "bad command\n");
    return 2;
}
