/* standalone.c - driver for non-libFuzzer builds (AFL++, plain replay, CI):
 *   prog FILE...      run the target on each file
 *   prog              read one input from stdin (AFL: afl-fuzz -i seeds -o out -- ./prog)
 *   QPNG_FUZZ_ITERS=N prog FILE...   additionally run N random mutations of each file (cheap smoke fuzzing) */
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static uint8_t *slurp(FILE *f, size_t *n) {
    size_t cap = 1 << 16, len = 0; uint8_t *b = malloc(cap);
    for (size_t r; b && (r = fread(b + len, 1, cap - len, f)) > 0;) {
        len += r;
        if (len == cap) { uint8_t *nb = realloc(b, cap *= 2); if (!nb) { free(b); return NULL; } b = nb; }
    }
    *n = len; return b;
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;
static uint32_t rnd(void) { rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17; return (uint32_t)rng; }

int main(int argc, char **argv) {
    if (argc < 2) {
        size_t n; uint8_t *b = slurp(stdin, &n);
        if (b) { LLVMFuzzerTestOneInput(b, n); free(b); }
        return 0;
    }
    long iters = getenv("QPNG_FUZZ_ITERS") ? atol(getenv("QPNG_FUZZ_ITERS")) : 0;
    for (int i = 1; i < argc; i++) {
        FILE *f = fopen(argv[i], "rb");
        if (!f) { perror(argv[i]); return 1; }
        size_t n; uint8_t *b = slurp(f, &n); fclose(f);
        if (!b) return 1;
        LLVMFuzzerTestOneInput(b, n);
        uint8_t *m = malloc(n + 16);
        for (long it = 0; m && it < iters; it++) {
            size_t mn = n; memcpy(m, b, n);
            int k = 1 + rnd() % 8;
            while (k--) {
                switch (rnd() % 4) {
                case 0: if (mn) m[rnd() % mn] ^= (uint8_t)(1u << (rnd() % 8)); break;
                case 1: if (mn) m[rnd() % mn] = (uint8_t)rnd(); break;
                case 2: if (mn) mn = rnd() % mn; break;                       /* truncate */
                case 3: if (mn > 8) { size_t a = rnd() % mn; m[a] = m[rnd() % mn]; } break;
                }
            }
            LLVMFuzzerTestOneInput(m, mn);
        }
        free(m); free(b);
    }
    return 0;
}
