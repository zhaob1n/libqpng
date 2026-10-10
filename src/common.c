/* common.c - allocator plumbing, CPU feature detection, status strings. */
#include "internal.h"

int pf__have_avx2(void) {
    static int v = -1;   /* benign race: every thread computes the same value */
#if PF_X86
    if (v < 0) { __builtin_cpu_init(); v = __builtin_cpu_supports("avx2") ? 1 : 0; }
#else
    v = 0;
#endif
    return v;
}

void *pf__alloc(const pf_allocator *a, size_t n) { return (a && a->alloc) ? a->alloc(a->user, n) : malloc(n); }
void pf__free(const pf_allocator *a, void *p) { if (!p) return; if (a && a->free) a->free(a->user, p); else free(p); }
void *pf__realloc_shrink(const pf_allocator *a, void *p, size_t old_n, size_t new_n) {
    (void)old_n;
    if (a && a->alloc) return p;                /* custom allocators: keep the (slightly larger) block */
    void *q = realloc(p, new_n ? new_n : 1);
    return q ? q : p;
}

const char *pf_status_string(pf_status s) {
    switch (s) {
    case PF_OK: return "ok";
    case PF_ERR_ARGUMENT: return "invalid argument";
    case PF_ERR_SIGNATURE: return "not a PNG file";
    case PF_ERR_HEADER: return "invalid IHDR";
    case PF_ERR_CHUNK: return "malformed chunk structure";
    case PF_ERR_CRC: return "chunk CRC mismatch";
    case PF_ERR_ZLIB: return "invalid zlib header";
    case PF_ERR_DEFLATE: return "corrupt or truncated image data";
    case PF_ERR_FILTER: return "invalid filter type";
    case PF_ERR_ADLER: return "Adler-32 mismatch";
    case PF_ERR_NOMEM: return "out of memory";
    case PF_ERR_LIMITS: return "image exceeds limits";
    case PF_ERR_UNSUPPORTED: return "unsupported PNG feature";
    case PF_ERR_ENCODE: return "encoder failure";
    }
    return "unknown status";
}
