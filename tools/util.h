/* tools/util.h - shared helpers for tools/ and bench/ (not installed).
 * Same cleanup vocabulary as src/internal.h (qpng_autofree / take), kept local so the
 * CLI does not need the private implementation headers. */
#ifndef QPNG_TOOLS_UTIL_H
#define QPNG_TOOLS_UTIL_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static inline void qpng_tool_freep(void *pp) { free(*(void **)pp); }
static inline void qpng_tool_fclosep(FILE **fp) { if (*fp) { fclose(*fp); *fp = nullptr; } }

#define QPNG_TOOL_CLEANUP(fn) [[gnu::cleanup(fn)]]
#define qpng_tool_autofree QPNG_TOOL_CLEANUP(qpng_tool_freep)
#define qpng_tool_autoclose QPNG_TOOL_CLEANUP(qpng_tool_fclosep)
#define QPNG_TOOL_TAKE(var) ({ typeof(var) qpng__t_ = (var); (var) = nullptr; qpng__t_; })

/* Read whole file; nullptr on any failure (caller prints). *n is set only on success. */
[[nodiscard]] static inline uint8_t *qpng_tool_slurp(const char *path, size_t *n) {
    qpng_tool_autoclose FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    if (fseek(f, 0, SEEK_END) != 0) return nullptr;
    long sz = ftell(f);
    if (sz < 0 || fseek(f, 0, SEEK_SET) != 0) return nullptr;
    qpng_tool_autofree uint8_t *b = malloc((size_t)sz + 1);
    if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) return nullptr;
    *n = (size_t)sz;
    return QPNG_TOOL_TAKE(b);
}

/* Write n bytes; 0 on success, 2 on I/O error (matches historical qpng CLI exit codes). */
[[nodiscard]] static inline int qpng_tool_spit(const char *path, const void *d, size_t n) {
    qpng_tool_autoclose FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 2; }
    return fwrite(d, 1, n, f) == n ? 0 : 2;
}

/* First chunk payload of `type` before IDAT; nullptr if absent. *len is the payload length. */
static inline const uint8_t *qpng_tool_find_chunk(const uint8_t *b, size_t n, const char *type, int *len) {
    for (size_t i = 8; i + 12 <= n;) {
        size_t L = (size_t)b[i] << 24 | (size_t)b[i + 1] << 16 | (size_t)b[i + 2] << 8 | b[i + 3];
        if (L > n - i - 12 || !memcmp(b + i + 4, "IDAT", 4)) break;
        if (!memcmp(b + i + 4, type, 4)) { *len = (int)L; return b + i + 8; }
        i += 12 + L;
    }
    return nullptr;
}

#endif
