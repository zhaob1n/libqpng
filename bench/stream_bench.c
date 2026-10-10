/* stream_bench.c - one-shot vs streaming: decode (pf_decode, pf_decoder image mode, row mode) and encode
 * (pf_encode vs pf_encoder per preset). CSV: op,param,file,ns_median,bytes. Usage: stream_bench REPS files... */
#define _POSIX_C_SOURCE 199309L
#include "pngfast.h"
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
        pf_image src; if (pf_decode(b, n, NULL, &src)) { free(b); continue; }
        volatile uint8_t sink = 0;
        for (int mode = 0; mode < 3; mode++) {
            for (int r = 0; r <= reps; r++) {
                double t0 = now(); pf_image im; pf_decoder *d;
                if (mode == 0) { pf_decode(b, n, NULL, &im); pf_image_free(&im); }
                else { pf_decoder_new(NULL, mode == 2 ? on_row : NULL, (void *)&sink, &d); pf_decoder_push(d, b, n);
                       pf_decoder_finish(d, mode == 1 ? &im : NULL); if (mode == 1) pf_image_free(&im); pf_decoder_free(d); }
                if (r) t[r - 1] = now() - t0;
            }
            qsort(t, reps, sizeof *t, cmpd);
            printf("decode,%s,%s,%.0f,%zu\n", (const char *[]){ "oneshot", "stream-image", "stream-rows" }[mode], argv[i], t[reps / 2], src.size);
        }
        for (int p = 0; p < 4; p++) for (int mode = 0; mode < 2; mode++) {
            size_t out = 0;
            pf_encode_options o; pf_encode_options_init(&o); o.preset = (pf_preset)p;
            uint8_t pal[768]; /* palette images: skip (needs PLTE extraction) */
            (void)pal; if (src.color_type == PF_COLOR_PALETTE) continue;
            for (int r = 0; r <= reps; r++) {
                double t0 = now();
                if (!mode) { uint8_t *png; size_t pn; pf_encode(src.pixels, src.width, src.height, src.stride, (pf_color)src.color_type, src.bit_depth, &o, &png, &pn); out = pn; pf_buffer_free(png, NULL); }
                else { cnt_t c = { 0 }; pf_encoder *e; pf_encoder_new(src.width, src.height, (pf_color)src.color_type, src.bit_depth, &o, wr, &c, &e);
                       pf_encoder_push_rows(e, src.pixels, src.stride, src.height); pf_encoder_finish(e); pf_encoder_free(e); out = c.n; }
                if (r) t[r - 1] = now() - t0;
            }
            qsort(t, reps, sizeof *t, cmpd);
            printf("encode,%s-%s,%s,%.0f,%zu\n", (const char *[]){ "fastest", "fast", "default", "best" }[p], mode ? "stream" : "oneshot", argv[i], t[reps / 2], out);
        }
        pf_image_free(&src); free(b);
    }
    return 0;
}
