/* common.c - allocator plumbing, CPU feature detection, status strings, PNG chunk buffer. */
#include "internal.h"
#include <stdatomic.h>

int qpng__have_avx2(void) {
#if QPNG_X86
    static _Atomic(int) v = -1;   /* racing first callers all store the same value */
    int r = atomic_load_explicit(&v, memory_order_relaxed);
    if (r < 0) {
        __builtin_cpu_init();
        r = __builtin_cpu_supports("avx2") ? 1 : 0;
        atomic_store_explicit(&v, r, memory_order_relaxed);
    }
    return r;
#else
    return 0;
#endif
}

void *qpng__alloc(const qpng_allocator *a, size_t n) { return (a && a->alloc) ? a->alloc(a->user, n) : malloc(n); }
void qpng__free(const qpng_allocator *a, void *p) { if (!p) return; if (a && a->free) a->free(a->user, p); else free(p); }
void *qpng__realloc_shrink(const qpng_allocator *a, void *p, size_t old_n, size_t new_n) {
    (void)old_n;
    if (a && a->alloc) return p;                /* custom allocators: keep the (slightly larger) block */
    void *q = realloc(p, new_n ? new_n : 1);
    return q ? q : p;
}

const char *qpng_status_string(qpng_status s) {
    switch (s) {
    case QPNG_OK: return "ok";
    case QPNG_ERR_ARGUMENT: return "invalid argument";
    case QPNG_ERR_SIGNATURE: return "not a PNG file";
    case QPNG_ERR_HEADER: return "invalid IHDR";
    case QPNG_ERR_CHUNK: return "malformed chunk structure";
    case QPNG_ERR_CRC: return "chunk CRC mismatch";
    case QPNG_ERR_ZLIB: return "invalid zlib header";
    case QPNG_ERR_DEFLATE: return "corrupt or truncated image data";
    case QPNG_ERR_FILTER: return "invalid filter type";
    case QPNG_ERR_ADLER: return "Adler-32 mismatch";
    case QPNG_ERR_NOMEM: return "out of memory";
    case QPNG_ERR_LIMITS: return "image exceeds limits";
    case QPNG_ERR_UNSUPPORTED: return "unsupported PNG feature";
    case QPNG_ERR_ENCODE: return "encoder failure";
    case QPNG_ERR_ABORTED: return "aborted by callback";
    case QPNG_ERR_STATE: return "invalid streaming state";
    case QPNG_ERR_APNG: return "invalid APNG structure";
    case QPNG_MORE_DATA: return "more data needed";
    }
    return "unknown status";
}

void *qpng__grow(const qpng_allocator *a, void *p, size_t n, size_t cap[static 1], size_t need, size_t el, size_t first) {
    if (need <= *cap) return p;
    size_t nc = *cap ? *cap : (first > need ? first : need), bytes;
    while (nc < need) if (qpng__mul_size(&nc, nc, 2)) return nullptr;
    if (qpng__mul_size(&bytes, nc, el)) return nullptr;
    void *np = qpng__alloc(a, bytes);
    if (!np) return nullptr;
    if (n) memcpy(np, p, n * el);
    qpng__free(a, p);
    *cap = nc;
    return np;
}

void qpng__wb_put(qpng__wbuf *w, const void *d, size_t n) {
    if (w->err || !n) return;
    size_t need;
    uint8_t *np = qpng__add_size(&need, w->n, n) ? nullptr : qpng__grow(w->A, w->p, w->n, &w->cap, need, 1, 256);
    if (!np) { w->err = QPNG_ERR_NOMEM; return; }
    w->p = np;
    memcpy(w->p + w->n, d, n);
    w->n += n;
}
size_t qpng__wb_begin(qpng__wbuf *w, const char type[static 4]) {
    const size_t start = w->n;
    uint8_t h[8] = {};
    memcpy(h + 4, type, 4);
    qpng__wb_put(w, h, sizeof h);
    return start;
}
void qpng__wb_end(qpng__wbuf *w, size_t start) {
    if (w->err) return;
    const size_t len = w->n - start - 8;
    if (len > 0x7FFFFFFFu) { w->err = QPNG_ERR_ARGUMENT; return; }
    wbe32(w->p + start, (uint32_t)len);
    uint8_t c[4];
    wbe32(c, libdeflate_crc32(0, w->p + start + 4, len + 4));
    qpng__wb_put(w, c, sizeof c);
}
void qpng__wb_chunk(qpng__wbuf *w, const char type[static 4], const void *d, size_t n) {
    if (n > 0x7FFFFFFFu) { if (!w->err) w->err = QPNG_ERR_ARGUMENT; return; }
    const size_t c = qpng__wb_begin(w, type);
    qpng__wb_put(w, d, n);
    qpng__wb_end(w, c);
}
