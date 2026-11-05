#include "test_common.h"

unsigned char *slurp(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return nullptr; }
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) { fclose(f); return nullptr; }
    unsigned char *b = malloc((size_t)sz + 1);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) {
        fclose(f);
        free(b);
        return nullptr;
    }
    fclose(f);
    *n = (size_t)sz;
    return b;
}

uint64_t fnv1a(const unsigned char *d, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; i++) h = (h ^ d[i]) * 0x100000001b3ull;
    return h;
}

const unsigned char *find_chunk(const unsigned char *b, size_t n, const char *type, int *len) {
    for (size_t i = 8; i + 12 <= n;) {
        size_t L = (size_t)b[i] << 24 | (size_t)b[i + 1] << 16 | (size_t)b[i + 2] << 8 | b[i + 3];
        if (L > n - i - 12 || !memcmp(b + i + 4, "IDAT", 4)) break;
        if (!memcmp(b + i + 4, type, 4)) { *len = (int)L; return b + i + 8; }
        i += 12 + L;
    }
    return nullptr;
}
