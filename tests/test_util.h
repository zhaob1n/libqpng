#ifndef TEST_UTIL_H
#define TEST_UTIL_H
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
static inline unsigned char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)sz + 1);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) { fclose(f); free(b); return NULL; }
    fclose(f); *n = (size_t)sz; return b;
}
static inline uint64_t fnv1a(const unsigned char *d, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ d[i]) * 0x100000001b3ull;
    return h;
}
static inline const unsigned char *find_chunk(const unsigned char *b, size_t n, const char *type, int *len) {
    for (size_t i = 8; i + 12 <= n;) {
        size_t L = (size_t)b[i] << 24 | (size_t)b[i + 1] << 16 | (size_t)b[i + 2] << 8 | b[i + 3];
        if (L > n - i - 12 || !memcmp(b + i + 4, "IDAT", 4)) break;
        if (!memcmp(b + i + 4, type, 4)) { *len = (int)L; return b + i + 8; }
        i += 12 + L;
    }
    return NULL;
}
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); fails++; } } while (0)
#endif
