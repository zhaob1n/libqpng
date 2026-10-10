/* Vendored libdeflate compressor modes (levels 1-12, png-fast, run-only, png-ultra whole-buffer and chunked) on
 * synthetic inputs: random, runs, PNG-like rows, tiny. Every stream must inflate with stock zlib (when available)
 * and with libdeflate, to the original bytes. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pf_libdeflate.h"
#ifdef PNGFAST_TEST_ZLIB
#include <zlib.h>
#endif

static int check(const char *what, const unsigned char *in, size_t n, const unsigned char *z, size_t zn) {
    unsigned char *b = malloc(n + 1);
    size_t got = 0; int bad = 0;
    struct libdeflate_decompressor *d = libdeflate_alloc_decompressor();
    if (libdeflate_deflate_decompress(d, z, zn, b, n, &got) || got != n || memcmp(b, in, n)) bad |= 1;
    libdeflate_free_decompressor(d);
#ifdef PNGFAST_TEST_ZLIB
    z_stream s; memset(&s, 0, sizeof s);
    inflateInit2(&s, -15);
    s.next_in = (unsigned char *)z; s.avail_in = (uInt)zn; s.next_out = b; s.avail_out = (uInt)n + 1;
    if (inflate(&s, Z_FINISH) != Z_STREAM_END || s.total_out != n || memcmp(b, in, n)) bad |= 2;
    inflateEnd(&s);
#endif
    if (bad) fprintf(stderr, "FAIL %s n=%zu (%s)\n", what, n, bad & 2 ? "zlib" : "libdeflate");
    free(b);
    return bad != 0;
}

static void gen(unsigned char *p, size_t n, int kind, unsigned seed) {
    srand(seed);
    for (size_t i = 0; i < n; i++)
        switch (kind) {
        case 0: p[i] = (unsigned char)rand(); break;                                   /* incompressible */
        case 1: p[i] = (unsigned char)((i / 37) % 3); break;                            /* runs */
        case 2: p[i] = (unsigned char)(i % 1201 == 0 ? 2 : rand() % 7 < 5 ? 0 : rand() % 5); break;  /* filtered rows */
        default: p[i] = (unsigned char)(rand() % 4 ? 0 : rand()); break;
        }
}

int main(void) {
    static const size_t sizes[] = { 1, 7, 33, 100, 4095, 5000, 65536, 70001, 300000, 1200000 };
    static const int levels[] = { 1, 6, 9, 12, LIBDEFLATE_PNG_FAST_LEVEL, -1, LIBDEFLATE_PNG_ULTRA_LEVEL };
    int fails = 0, runs = 0;
    for (size_t si = 0; si < sizeof sizes / sizeof *sizes; si++)
        for (int kind = 0; kind < 4; kind++) {
            size_t n = sizes[si];
            unsigned char *in = malloc(n + 1), *z = malloc(n + n / 8 + 4096);
            gen(in + 1, n, kind, (unsigned)(si * 7 + kind));
            for (size_t li = 0; li < sizeof levels / sizeof *levels; li++) {
                int lv = levels[li];
                struct libdeflate_compressor *c = libdeflate_alloc_compressor(lv < 0 ? LIBDEFLATE_PNG_FAST_LEVEL : lv);
                if (lv < 0) libdeflate_set_png_min_match(c, 0);
                size_t bound = libdeflate_deflate_compress_bound(NULL, n);
                size_t zn = libdeflate_deflate_compress(c, in + 1, n, z, bound);
                char what[64]; snprintf(what, sizeof what, "level %d kind %d", lv, kind);
                runs++;
                if (!zn) { fprintf(stderr, "FAIL %s n=%zu: exceeds bound\n", what, n); fails++; }
                else fails += check(what, in + 1, n, z, zn);
                if (lv == LIBDEFLATE_PNG_ULTRA_LEVEL) {   /* chunked: odd chunk sizes, history byte carried */
                    struct libdeflate_png_ultra_state st = { 0, 0, 0 };
                    size_t zo = 0, off = 0, step = 1000 + n / 3;
                    while (off < n) {
                        size_t k = n - off < step ? n - off : step;
                        size_t o = libdeflate_png_ultra_compress_chunk(c, &st, in + 1 + off, k, off + k == n, z + zo,
                                                                       bound + 64 - zo);
                        if (!o) break;
                        zo += o; off += k;
                    }
                    snprintf(what, sizeof what, "ultra chunked kind %d", kind);
                    runs++;
                    if (off != n) { fprintf(stderr, "FAIL %s n=%zu: chunk overflow\n", what, n); fails++; }
                    else fails += check(what, in + 1, n, z, zo);
                }
                libdeflate_free_compressor(c);
            }
            free(in); free(z);
        }
    printf("deflate: %d streams, %d failures\n", runs, fails);
    return fails != 0;
}
