/* stream_bench.c - one-shot vs streaming: decode (qpng_decode, qpng_decoder image mode, row mode) and encode
 * (qpng_encode vs qpng_encoder per preset). CSV: op,param,file,ns_median,bytes. Usage: stream_bench REPS files... */
#define _POSIX_C_SOURCE 199309L
#include "qpng.h"
#include "../tools/util.h"
#include <time.h>

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e9 + (double)t.tv_nsec;
}
static int cmpd(const void *a, const void *b) {
    double x = *(const double *)a, y = *(const double *)b;
    return (x > y) - (x < y);
}
static int on_row(void *u, [[maybe_unused]] uint32_t y, size_t n, const uint8_t r[static n]) {
    *(volatile uint8_t *)u ^= r[n - 1];
    return 0;
}
typedef struct { size_t n; } cnt_t;
static int wr(void *u, size_t n, [[maybe_unused]] const uint8_t d[static n]) {
    ((cnt_t *)u)->n += n;
    return 0;
}

int main(int argc, char **argv) {
    if (argc < 3) return 2;
    int reps = atoi(argv[1]);
    qpng_tool_autofree double *t = malloc(sizeof(double) * (size_t)reps);
    if (!t) return 2;
    for (int i = 2; i < argc; i++) {
        size_t n = 0;
        qpng_tool_autofree uint8_t *b = qpng_tool_slurp(argv[i], &n);
        if (!b) continue;
        qpng_image src = {};
        if (qpng_decode(n, b, nullptr, &src)) continue;
        volatile uint8_t sink = 0;
        for (int mode = 0; mode < 3; mode++) {
            for (int r = 0; r <= reps; r++) {
                double t0 = now();
                qpng_image im = {};
                qpng_decoder *d = nullptr;
                if (mode == 0) {
                    (void)qpng_decode(n, b, nullptr, &im);
                    qpng_image_free(&im);
                } else {
                    (void)qpng_decoder_new(nullptr, mode == 2 ? on_row : nullptr, (void *)&sink, &d);
                    (void)qpng_decoder_push(d, n, b);
                    (void)qpng_decoder_finish(d, mode == 1 ? &im : nullptr);
                    if (mode == 1) qpng_image_free(&im);
                    qpng_decoder_free(d);
                }
                if (r) t[r - 1] = now() - t0;
            }
            qsort(t, (size_t)reps, sizeof *t, cmpd);
            printf("decode,%s,%s,%.0f,%zu\n",
                   (const char *[]){ "oneshot", "stream-image", "stream-rows" }[mode], argv[i], t[reps / 2], src.size);
        }
        for (int p = 0; p < 4; p++) {
            if (src.color_type == QPNG_COLOR_PALETTE) continue;   /* needs PLTE extraction */
            for (int mode = 0; mode < 2; mode++) {
                size_t out = 0;
                qpng_encode_options o = {};
                qpng_encode_options_init(&o);
                o.preset = (qpng_preset)p;
                for (int r = 0; r <= reps; r++) {
                    double t0 = now();
                    if (!mode) {
                        uint8_t *png = nullptr;
                        size_t pn = 0;
                        (void)qpng_encode(src.pixels, src.width, src.height, src.stride, (qpng_color)src.color_type,
                                    src.bit_depth, &o, &png, &pn);
                        out = pn;
                        qpng_buffer_free(png, nullptr);
                    } else {
                        cnt_t c = {};
                        qpng_encoder *e = nullptr;
                        (void)qpng_encoder_new(src.width, src.height, (qpng_color)src.color_type, src.bit_depth, &o, wr, &c, &e);
                        (void)qpng_encoder_push_rows(e, src.pixels, src.stride, src.height);
                        (void)qpng_encoder_finish(e);
                        qpng_encoder_free(e);
                        out = c.n;
                    }
                    if (r) t[r - 1] = now() - t0;
                }
                qsort(t, (size_t)reps, sizeof *t, cmpd);
                printf("encode,%s-%s,%s,%.0f,%zu\n",
                       (const char *[]){ "fastest", "fast", "default", "best" }[p],
                       mode ? "stream" : "oneshot", argv[i], t[reps / 2], out);
            }
        }
        qpng_image_free(&src);
    }
    return 0;
}
