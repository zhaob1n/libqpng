/* qpng-cli - small command-line front end (also used by the tests).
 *   qpng-cli info IN.png
 *   qpng-cli decode IN.png OUT.raw [raw|expand|imagers] [--adler]   (writes the pixel buffer; prints geometry)
 *   qpng-cli encode IN.png OUT.png [fastest|fast|default|best] [filter 0..5|-1] [level]
 *       (IN.png is decoded to raw samples, then re-encoded with the same color type / depth / PLTE / tRNS)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "qpng.h"

static unsigned char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    if (sz < 0) exit(2);
    unsigned char *b = malloc((size_t)sz + 1); if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) exit(2);
    fclose(f); *n = (size_t)sz; return b;
}
static int spit(const char *p, const void *d, size_t n) {
    FILE *f = fopen(p, "wb"); if (!f) { perror(p); return 2; }
    int ok = fwrite(d, 1, n, f) == n; fclose(f); return ok ? 0 : 2;
}
/* find a chunk payload in a PNG (first occurrence before IDAT) */
static const unsigned char *find_chunk(const unsigned char *b, size_t n, const char *type, int *len) {
    for (size_t i = 8; i + 12 <= n;) {
        size_t L = (size_t)b[i] << 24 | (size_t)b[i + 1] << 16 | (size_t)b[i + 2] << 8 | b[i + 3];
        if (!memcmp(b + i + 4, "IDAT", 4)) break;
        if (!memcmp(b + i + 4, type, 4)) { *len = (int)L; return b + i + 8; }
        i += 12 + L;
    }
    return NULL;
}

int main(int argc, char **argv) {
    if (argc < 3) { fprintf(stderr, "usage: see source header\n"); return 2; }
    size_t n; unsigned char *buf = slurp(argv[2], &n);
    if (!strcmp(argv[1], "info")) {
        qpng_info in; qpng_status s = qpng_read_info(buf, n, &in);
        if (s) { printf("error: %s\n", qpng_status_string(s)); return 1; }
        printf("%ux%u depth %u color %u interlaced %u channels %u plte %u trns %u\n", in.width, in.height, in.bit_depth,
               in.color_type, in.interlaced, in.channels, in.palette_entries, in.trns_bytes);
        return 0;
    }
    if (!strcmp(argv[1], "decode") && argc >= 4) {
        qpng_decode_options o; qpng_decode_options_init(&o);
        if (argc > 4) o.output = !strcmp(argv[4], "expand") ? QPNG_OUTPUT_EXPAND : !strcmp(argv[4], "imagers") ? QPNG_OUTPUT_IMAGERS : QPNG_OUTPUT_RAW;
        if (argc > 5 && !strcmp(argv[5], "--adler")) o.verify_adler = 1;
        qpng_image im; qpng_status s = qpng_decode(buf, n, &o, &im);
        if (s) { fprintf(stderr, "decode error: %s\n", qpng_status_string(s)); return 1; }
        printf("%u %u %u %u %u\n", im.width, im.height, im.channels, im.bit_depth, im.color_type);
        int r = spit(argv[3], im.pixels, im.size); qpng_image_free(&im); return r;
    }
    if (!strcmp(argv[1], "encode") && argc >= 4) {
        qpng_image im; qpng_status s = qpng_decode(buf, n, NULL, &im);
        if (s) { fprintf(stderr, "decode error: %s\n", qpng_status_string(s)); return 1; }
        qpng_encode_options o; qpng_encode_options_init(&o);
        if (argc > 4) o.preset = !strcmp(argv[4], "fastest") ? QPNG_PRESET_FASTEST : !strcmp(argv[4], "fast") ? QPNG_PRESET_FAST :
                                 !strcmp(argv[4], "best") ? QPNG_PRESET_BEST : QPNG_PRESET_DEFAULT;
        if (argc > 5) o.filter = atoi(argv[5]);
        if (argc > 6) o.deflate_level = atoi(argv[6]);
        int pl = 0, tl = 0;
        o.palette = find_chunk(buf, n, "PLTE", &pl); o.palette_entries = pl / 3;
        o.trns = find_chunk(buf, n, "tRNS", &tl); o.trns_bytes = tl;
        if (!o.palette) o.palette_entries = 0;
        if (!o.trns) o.trns_bytes = 0;
        uint8_t *png; size_t pn;
        s = qpng_encode(im.pixels, im.width, im.height, im.stride, (qpng_color)im.color_type, im.bit_depth, &o, &png, &pn);
        qpng_image_free(&im);
        if (s) { fprintf(stderr, "encode error: %s\n", qpng_status_string(s)); return 1; }
        int r = spit(argv[3], png, pn); qpng_buffer_free(png, NULL); return r;
    }
    fprintf(stderr, "bad command\n"); return 2;
}
