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
[[nodiscard]] static inline uint8_t *qpng_tool_slurp(const char path[static 1], size_t n[static 1]) {
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
[[nodiscard]] static inline int qpng_tool_spit(const char path[static 1], size_t n, const uint8_t d[n]) {
    qpng_tool_autoclose FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return 2; }
    return fwrite(d, 1, n, f) == n ? 0 : 2;
}

/* First chunk payload of `type` before IDAT; nullptr if absent. *len is the payload length. */
static inline const uint8_t *qpng_tool_find_chunk(size_t n, const uint8_t b[static n], const char type[static 4],
                                                  int len[static 1]) {
    for (size_t i = 8; i + 12 <= n;) {
        size_t L = (size_t)b[i] << 24 | (size_t)b[i + 1] << 16 | (size_t)b[i + 2] << 8 | b[i + 3];
        if (L > n - i - 12 || !memcmp(b + i + 4, "IDAT", 4)) break;
        if (!memcmp(b + i + 4, type, 4)) { *len = (int)L; return b + i + 8; }
        i += 12 + L;
    }
    return nullptr;
}

#ifdef QPNG_H
/* Carry PLTE/tRNS from a source PNG into encode options when valid for `color_type` (skips suggested PLTE on
 * non-palette images and out-of-spec tRNS lengths). */
static inline void qpng_tool_carry_plte_trns(size_t n, const uint8_t png[static n], uint8_t color_type,
                                             qpng_encode_options o[static 1]) {
    int pl = 0, tl = 0;
    if (color_type == QPNG_COLOR_PALETTE) {
        const uint8_t *p = qpng_tool_find_chunk(n, png, "PLTE", &pl);
        if (p && pl >= 3 && pl <= 768 && pl % 3 == 0) { o->palette = p; o->palette_entries = pl / 3; }
    }
    const uint8_t *t = qpng_tool_find_chunk(n, png, "tRNS", &tl);
    if (t && tl >= 1 &&
        ((color_type == QPNG_COLOR_GRAY && tl == 2) || (color_type == QPNG_COLOR_RGB && tl == 6) ||
         (color_type == QPNG_COLOR_PALETTE && tl <= o->palette_entries))) {
        o->trns = t; o->trns_bytes = tl;
    }
}
#endif

#endif
