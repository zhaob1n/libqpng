/* common.c - allocator plumbing, CPU feature detection, status strings. */
#include "internal.h"

int qpng__have_avx2(void) {
    static int v = -1;   /* benign race: every thread computes the same value */
#if QPNG_X86
    if (v < 0) { __builtin_cpu_init(); v = __builtin_cpu_supports("avx2") ? 1 : 0; }
#else
    v = 0;
#endif
    return v;
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
