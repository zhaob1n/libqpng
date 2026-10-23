/* stream_bench.c - one-shot vs streaming: decode (qpng_decode, qpng_decoder image mode, row mode) and encode
 * (qpng_encode vs qpng_encoder per preset). CSV: op,param,file,ns_median,bytes. Usage: stream_bench REPS files... */
#define _POSIX_C_SOURCE 199309L
#include "qpng.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
static double now(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1e9 + t.tv_nsec; }
static int cmpd(const void *a, const void *b) { double x = *(const double *)a, y = *(const double *)b; return (x > y) - (x < y); }
static int on_row(void *u, uint32_t y, const uint8_t *r, size_t n) { (void)y; *(volatile uint8_t *)u ^= r[n - 1]; return 0; }
typedef struct { size_t n; } cnt_t;
static int wr(void *u, const uint8_t *d, size_t n) { (void)d; ((cnt_t *)u)->n += n; return 0; }

int main(int argc, char **argv) {
    int reps = atoi(argv[1]);
    double *t = malloc(sizeof(double) * reps);
    for (int i = 2; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb"); if (!f) continue;
        fseek(f, 0, SEEK_END); size_t n = (size_t)ftell(f); fseek(f, 0, SEEK_SET);
        uint8_t *b = malloc(n); if (fread(b, 1, n, f) != n) return 1; fclose(f);
        qpng_image src; if (qpng_decode(b, n, NULL, &src)) { free(b); continue; }
        volatile uint8_t sink = 0;
        for (int mode = 0; mode < 3; mode++) {
            for (int r = 0; r <= reps; r++) {
                double t0 = now(); qpng_image im; qpng_decoder *d;
                if (mode == 0) { qpng_decode(b, n, NULL, &im); qpng_image_free(&im); }
                else { qpng_decoder_new(NULL, mode == 2 ? on_row : NULL, (void *)&sink, &d); qpng_decoder_push(d, b, n);
                       qpng_decoder_finish(d, mode == 1 ? &im : NULL); if (mode == 1) qpng_image_free(&im); qpng_decoder_free(d); }
                if (r) t[r - 1] = now() - t0;
            }
            qsort(t, reps, sizeof *t, cmpd);
            printf("decode,%s,%s,%.0f,%zu\n", (const char *[]){ "oneshot", "stream-image", "stream-rows" }[mode], argv[i], t[reps / 2], src.size);
        }
        for (int p = 0; p < 4; p++) for (int mode = 0; mode < 2; mode++) {
            size_t out = 0;
            qpng_encode_options o; qpng_encode_options_init(&o); o.preset = (qpng_preset)p;
            uint8_t pal[768]; /* palette images: skip (needs PLTE extraction) */
            (void)pal; if (src.color_type == QPNG_COLOR_PALETTE) continue;
            for (int r = 0; r <= reps; r++) {
                double t0 = now();
                if (!mode) { uint8_t *png; size_t pn; qpng_encode(src.pixels, src.width, src.height, src.stride, (qpng_color)src.color_type, src.bit_depth, &o, &png, &pn); out = pn; qpng_buffer_free(png, NULL); }
                else { cnt_t c = { 0 }; qpng_encoder *e; qpng_encoder_new(src.width, src.height, (qpng_color)src.color_type, src.bit_depth, &o, wr, &c, &e);
                       qpng_encoder_push_rows(e, src.pixels, src.stride, src.height); qpng_encoder_finish(e); qpng_encoder_free(e); out = c.n; }
                if (r) t[r - 1] = now() - t0;
            }
            qsort(t, reps, sizeof *t, cmpd);
            printf("encode,%s-%s,%s,%.0f,%zu\n", (const char *[]){ "fastest", "fast", "default", "best" }[p], mode ? "stream" : "oneshot", argv[i], t[reps / 2], out);
        }
        qpng_image_free(&src); free(b);
    }
    return 0;
}
