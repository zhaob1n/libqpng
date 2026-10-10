/* metadata.c - ancillary chunks: reading (pf_read_metadata) and serialisation for the encoder. */
#include "metadata.h"

/* ---- owned storage: a list of blocks allocated with the user's allocator ---- */
typedef struct pf_mblock { struct pf_mblock *next; size_t size; } pf_mblock;
typedef struct { pf_mblock *head; pf_allocator a; int has_a; size_t total, max_total; } pf_marena;

static const pf_allocator *arena_alloc(const pf_marena *m) { return m->has_a ? &m->a : NULL; }
static void *arena_get(pf_marena *m, size_t n) {
    if (n > m->max_total - m->total) return NULL;
    pf_mblock *b = pf__alloc(arena_alloc(m), sizeof(pf_mblock) + n + 1);
    if (!b) return NULL;
    b->next = m->head; b->size = n; m->head = b; m->total += n;
    uint8_t *p = (uint8_t *)(b + 1); p[n] = 0;   /* every block is NUL-terminated */
    return p;
}
static char *arena_str(pf_marena *m, const uint8_t *s, size_t n) {
    char *d = arena_get(m, n); if (d && n) memcpy(d, s, n); return d;
}

void pf_metadata_options_init(pf_metadata_options *o) {
    memset(o, 0, sizeof *o); o->verify_crc = 1; o->keep_unknown = 1;
}

void pf_metadata_free(pf_metadata *meta) {
    if (!meta) return;
    pf_marena *m = meta->internal_;
    if (m) {
        pf_allocator ac = m->a;   /* m itself is freed last */
        const pf_allocator *A = m->has_a ? &ac : NULL;
        for (pf_mblock *b = m->head, *nx; b; b = nx) { nx = b->next; pf__free(A, b); }
        pf__free(A, m);
    }
    memset(meta, 0, sizeof *meta);
}

uint8_t *pf__inflate_limited(const uint8_t *src, size_t n, size_t limit, const pf_allocator *A, size_t *outn) {
    struct libdeflate_decompressor *d = libdeflate_alloc_decompressor();
    if (!d) return NULL;
    size_t cap = n * 4 + 1024;
    uint8_t *out = NULL;
    for (;;) {   /* libdeflate needs the output size: grow x4 up to the limit */
        if (cap > limit) cap = limit;
        out = pf__alloc(A, cap + 1);
        if (!out) break;
        size_t got = 0;
        enum libdeflate_result r = libdeflate_zlib_decompress(d, src, n, out, cap, &got);
        if (r == LIBDEFLATE_SUCCESS) { *outn = got; break; }
        pf__free(A, out); out = NULL;
        if (r != LIBDEFLATE_INSUFFICIENT_SPACE || cap >= limit) break;
        cap = cap > limit / 4 ? limit : cap * 4;
    }
    libdeflate_free_decompressor(d);
    return out;
}

static int keyword_ok(const uint8_t *k, size_t n) {
    if (n < 1 || n > 79 || k[0] == ' ' || k[n - 1] == ' ') return 0;
    for (size_t i = 0; i < n; i++) {
        if (!((k[i] >= 32 && k[i] <= 126) || k[i] >= 161)) return 0;
        if (k[i] == ' ' && i && k[i - 1] == ' ') return 0;
    }
    return 1;
}
static const uint8_t *find0(const uint8_t *p, const uint8_t *end) { return p < end ? memchr(p, 0, (size_t)(end - p)) : NULL; }

/* growable arrays kept in plain allocations while parsing; copied into the arena at the end */
typedef struct { void *v; size_t n, cap, el; } pf_vec;
static void *vec_push(pf_vec *v, const pf_allocator *A) {
    if (v->n == v->cap) {
        size_t nc = v->cap ? v->cap * 2 : 16;
        void *nv = pf__alloc(A, nc * v->el);
        if (!nv) return NULL;
        if (v->n) memcpy(nv, v->v, v->n * v->el);
        pf__free(A, v->v); v->v = nv; v->cap = nc;
    }
    return (uint8_t *)v->v + v->el * v->n++;
}

/* chunks that are not metadata (image data, palette, transparency, APNG control) */
static const char *const SKIP_TYPES[] = { "IHDR", "PLTE", "IDAT", "IEND", "tRNS", "acTL", "fcTL", "fdAT" };
static int skip_type(const uint8_t *t) {
    for (size_t i = 0; i < sizeof SKIP_TYPES / sizeof *SKIP_TYPES; i++) if (!memcmp(t, SKIP_TYPES[i], 4)) return 1;
    return 0;
}

pf_status pf_read_metadata(const void *png, size_t n, const pf_metadata_options *opts, pf_metadata *meta) {
    if (!meta) return PF_ERR_ARGUMENT;
    memset(meta, 0, sizeof *meta);
    if (!png) return PF_ERR_ARGUMENT;
    pf_metadata_options defo;
    if (!opts) { pf_metadata_options_init(&defo); opts = &defo; }
    const pf_allocator *A = opts->allocator;
    const size_t max_chunk = opts->max_chunk_decompressed ? opts->max_chunk_decompressed : (16u << 20);
    const size_t max_total = opts->max_total ? opts->max_total : (64u << 20);
    const uint32_t max_chunks = opts->max_chunks ? opts->max_chunks : 4096;
    pf_info info;
    pf_status st = pf_read_info(png, n, &info);
    if (st) return st;

    pf_marena *m = pf__alloc(A, sizeof *m);
    if (!m) return PF_ERR_NOMEM;
    memset(m, 0, sizeof *m);
    if (A) { m->a = *A; m->has_a = 1; }
    m->max_total = max_total;
    meta->internal_ = m;
    pf_vec texts = { NULL, 0, 0, sizeof(pf_text) }, chunks = { NULL, 0, 0, sizeof(pf_chunk) };
    const uint8_t *buf = png, *p = buf + 8, *end = buf + n;
    int seen_plte = 0, seen_idat = 0, has_plte = 0;
    for (const uint8_t *q = p; q + 12 <= end && be32(q) <= (size_t)(end - q) - 12; q += 12 + be32(q)) {
        if (!memcmp(q + 4, "PLTE", 4)) has_plte = 1;
        if (!memcmp(q + 4, "IDAT", 4)) break;
    }
    for (; p + 12 <= end; ) {
        const uint32_t len = be32(p);
        if (len > (size_t)(end - p) - 12) { st = PF_ERR_CHUNK; break; }
        const uint8_t *t = p + 4, *d = p + 8, *de = d + len;
        p += 12 + len;
        if (!memcmp(t, "IEND", 4)) break;
        if (!memcmp(t, "IDAT", 4) || !memcmp(t, "fdAT", 4)) { seen_idat = 1; continue; }
        if (!memcmp(t, "PLTE", 4)) { seen_plte = 1; continue; }
        if (skip_type(t)) continue;
        if (!(t[0] & 0x20)) { st = PF_ERR_CHUNK; break; }          /* unknown critical chunk */
#define BAD() goto bad   /* NOT a do{}while(0) with `continue`: that would only leave the macro's own loop */
#define ONCE(flag) if (meta->flag) BAD();
        if (opts->verify_crc && libdeflate_crc32(0, t, len + 4) != be32(de)) BAD();
        if (!memcmp(t, "gAMA", 4)) { ONCE(has_gama) if (len != 4) BAD(); meta->has_gama = 1; meta->gamma = be32(d); }
        else if (!memcmp(t, "cHRM", 4)) { ONCE(has_chrm) if (len != 32) BAD(); meta->has_chrm = 1; for (int i = 0; i < 8; i++) meta->chrm[i] = be32(d + 4 * i); }
        else if (!memcmp(t, "sRGB", 4)) { ONCE(has_srgb) if (len != 1 || d[0] > 3) BAD(); meta->has_srgb = 1; meta->srgb_intent = d[0]; }
        else if (!memcmp(t, "cICP", 4)) { ONCE(has_cicp) if (len != 4) BAD(); meta->has_cicp = 1; memcpy(meta->cicp, d, 4); }
        else if (!memcmp(t, "mDCV", 4)) {
            ONCE(has_mdcv) if (len != 24) BAD();
            meta->has_mdcv = 1;
            for (int i = 0; i < 6; i++) meta->mdcv_primaries[i] = (uint16_t)(d[2 * i] << 8 | d[2 * i + 1]);
            meta->mdcv_white[0] = (uint16_t)(d[12] << 8 | d[13]); meta->mdcv_white[1] = (uint16_t)(d[14] << 8 | d[15]);
            meta->mdcv_max_lum = be32(d + 16); meta->mdcv_min_lum = be32(d + 20);
        }
        else if (!memcmp(t, "cLLI", 4)) { ONCE(has_clli) if (len != 8) BAD(); meta->has_clli = 1; meta->clli_max_cll = be32(d); meta->clli_max_fall = be32(d + 4); }
        else if (!memcmp(t, "pHYs", 4)) { ONCE(has_phys) if (len != 9 || d[8] > 1) BAD(); meta->has_phys = 1; meta->phys_x = be32(d); meta->phys_y = be32(d + 4); meta->phys_unit = d[8]; }
        else if (!memcmp(t, "tIME", 4)) {
            ONCE(has_time) if (len != 7) BAD();
            meta->has_time = 1; meta->time_year = (uint16_t)(d[0] << 8 | d[1]); meta->time_month = d[2]; meta->time_day = d[3];
            meta->time_hour = d[4]; meta->time_minute = d[5]; meta->time_second = d[6];
        }
        else if (!memcmp(t, "bKGD", 4)) { ONCE(has_bkgd) if (len != 1 && len != 2 && len != 6) BAD(); meta->has_bkgd = 1; memcpy(meta->bkgd, d, len); meta->bkgd_size = (uint8_t)len; }
        else if (!memcmp(t, "sBIT", 4)) { ONCE(has_sbit) if (len < 1 || len > 4) BAD(); meta->has_sbit = 1; memcpy(meta->sbit, d, len); meta->sbit_size = (uint8_t)len; }
        else if (!memcmp(t, "eXIf", 4)) {
            if (meta->exif) BAD();
            uint8_t *e = arena_get(m, len); if (!e) BAD();
            if (len) memcpy(e, d, len);
            meta->exif = e; meta->exif_size = len;
        }
        else if (!memcmp(t, "iCCP", 4)) {
            if (meta->iccp) BAD();
            const uint8_t *z = find0(d, de);
            if (!z || !keyword_ok(d, (size_t)(z - d)) || de - z < 2 || z[1] != 0) BAD();
            size_t pn = 0; uint8_t *prof = pf__inflate_limited(z + 2, (size_t)(de - z - 2), max_chunk, A, &pn);
            if (!prof) BAD();
            uint8_t *dst = arena_get(m, pn); char *name = arena_str(m, d, (size_t)(z - d));
            if (dst && pn) memcpy(dst, prof, pn);
            pf__free(A, prof);
            if (!dst || !name) BAD();
            meta->iccp = dst; meta->iccp_size = pn; meta->iccp_name = name;
        }
        else if (!memcmp(t, "tEXt", 4) || !memcmp(t, "zTXt", 4) || !memcmp(t, "iTXt", 4)) {
            if (texts.n + chunks.n >= max_chunks) BAD();
            const uint8_t *z = find0(d, de);
            if (!z || !keyword_ok(d, (size_t)(z - d))) BAD();
            pf_text tx; memset(&tx, 0, sizeof tx);
            const uint8_t *body = z + 1, *lang = NULL, *tk = NULL; size_t langn = 0, tkn = 0;
            int comp = 0;
            if (t[0] == 't') tx.kind = PF_TEXT_TEXT;
            else if (t[0] == 'z') { if (de - body < 1 || body[0] != 0) BAD(); tx.kind = PF_TEXT_ZTXT; comp = 1; body++; }
            else {
                if (de - body < 2 || body[0] > 1 || body[1] != 0) BAD();
                tx.kind = PF_TEXT_ITXT; comp = body[0]; tx.compressed = comp;
                lang = body + 2; const uint8_t *l0 = find0(lang, de); if (!l0) BAD();
                langn = (size_t)(l0 - lang); tk = l0 + 1;
                const uint8_t *t0 = find0(tk, de); if (!t0) BAD();
                tkn = (size_t)(t0 - tk); body = t0 + 1;
            }
            const uint8_t *txt = body; size_t txn = (size_t)(de - body); uint8_t *tmp = NULL;
            if (comp) { tmp = pf__inflate_limited(body, txn, max_chunk, A, &txn); if (!tmp) BAD(); txt = tmp; }
            tx.keyword = arena_str(m, d, (size_t)(z - d));
            tx.text = arena_str(m, txt, txn); tx.text_size = txn;
            if (lang) { tx.language = arena_str(m, lang, langn); tx.translated_keyword = arena_str(m, tk, tkn); }
            pf__free(A, tmp);
            if (!tx.keyword || !tx.text || (lang && (!tx.language || !tx.translated_keyword))) BAD();
            pf_text *slot = vec_push(&texts, A); if (!slot) { st = PF_ERR_NOMEM; break; }
            *slot = tx;
        }
        else if (opts->keep_unknown) {
            if (texts.n + chunks.n >= max_chunks) BAD();
            pf_chunk c; memset(&c, 0, sizeof c);
            memcpy(c.type, t, 4);
            c.location = seen_idat ? PF_CHUNK_AFTER_IDAT : seen_plte || !has_plte ? PF_CHUNK_BEFORE_IDAT : PF_CHUNK_BEFORE_PLTE;
            uint8_t *cd = arena_get(m, len); if (!cd) BAD();
            if (len) memcpy(cd, d, len);
            c.data = cd; c.size = len;
            pf_chunk *slot = vec_push(&chunks, A); if (!slot) { st = PF_ERR_NOMEM; break; }
            *slot = c;
        }
        continue;
    bad:   /* malformed, duplicate, bad CRC or over a limit: ignored */
        meta->skipped++;
#undef ONCE
#undef BAD
    }
    if (st == PF_OK && texts.n) {
        pf_text *a = arena_get(m, texts.n * sizeof(pf_text));
        if (!a) st = PF_ERR_LIMITS; else { memcpy(a, texts.v, texts.n * sizeof(pf_text)); meta->texts = a; meta->text_count = texts.n; }
    }
    if (st == PF_OK && chunks.n) {
        pf_chunk *a = arena_get(m, chunks.n * sizeof(pf_chunk));
        if (!a) st = PF_ERR_LIMITS; else { memcpy(a, chunks.v, chunks.n * sizeof(pf_chunk)); meta->chunks = a; meta->chunk_count = chunks.n; }
    }
    pf__free(A, texts.v); pf__free(A, chunks.v);
    if (st != PF_OK) pf_metadata_free(meta);
    return st;
}

/* ---- encoder side ---- */
static int chunk_type_ok(const char *t) {
    for (int i = 0; i < 4; i++) if (!((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= 'A' && t[i] <= 'Z'))) return 0;
    if (t[4] || !(t[0] & 0x20) || (t[2] & 0x20)) return 0;   /* ancillary, reserved bit clear */
    return !skip_type((const uint8_t *)t);
}

typedef struct { uint8_t *p; size_t n, cap; int err; const pf_allocator *A; } pf_wbuf;
static uint8_t *wb_reserve(pf_wbuf *w, size_t k) {
    if (w->err) return NULL;
    if (k > w->cap - w->n) {
        size_t nc = w->cap * 2 + k + 256;
        uint8_t *np = pf__alloc(w->A, nc);
        if (!np) { w->err = PF_ERR_NOMEM; return NULL; }
        if (w->n) memcpy(np, w->p, w->n);
        pf__free(w->A, w->p); w->p = np; w->cap = nc;
    }
    return w->p + w->n;
}
/* chunk with payload parts a (+ b) */
static void wb_chunk(pf_wbuf *w, const char *type, const void *a, size_t an, const void *b, size_t bn) {
    if (an + bn > 0x7FFFFFFFu) { w->err = PF_ERR_ARGUMENT; return; }
    uint8_t *q = wb_reserve(w, 12 + an + bn);
    if (!q) return;
    wbe32(q, (uint32_t)(an + bn)); memcpy(q + 4, type, 4);
    if (an) memcpy(q + 8, a, an);
    if (bn) memcpy(q + 8 + an, b, bn);
    wbe32(q + 8 + an + bn, libdeflate_crc32(0, q + 4, 4 + an + bn));
    w->n += 12 + an + bn;
}
static uint8_t *zlib9(const void *src, size_t n, size_t *zn, const pf_allocator *A) {
    struct libdeflate_compressor *c = libdeflate_alloc_compressor(9);
    if (!c) return NULL;
    size_t cap = libdeflate_zlib_compress_bound(c, n);
    uint8_t *z = pf__alloc(A, cap);
    if (z) *zn = libdeflate_zlib_compress(c, src, n, z, cap);
    libdeflate_free_compressor(c);
    if (z && !*zn) { pf__free(A, z); z = NULL; }
    return z;
}
static void wb_text(pf_wbuf *w, const pf_text *t) {
    const size_t kn = strlen(t->keyword);
    const size_t tn = t->text_size ? t->text_size : (t->text ? strlen(t->text) : 0);
    const int comp = t->kind == PF_TEXT_ZTXT || (t->kind == PF_TEXT_ITXT && t->compressed);
    const char *lg = t->language ? t->language : "", *tk = t->translated_keyword ? t->translated_keyword : "";
    const size_t ln = strlen(lg), tkn = strlen(tk);
    /* header: keyword NUL [zTXt: method] [iTXt: flag method lang NUL tkey NUL] */
    const size_t hn = kn + 1 + (t->kind == PF_TEXT_ZTXT ? 1 : 0) + (t->kind == PF_TEXT_ITXT ? 2 + ln + 1 + tkn + 1 : 0);
    uint8_t *h = pf__alloc(w->A, hn);
    if (!h) { w->err = PF_ERR_NOMEM; return; }
    memcpy(h, t->keyword, kn); h[kn] = 0;
    size_t o = kn + 1;
    if (t->kind == PF_TEXT_ZTXT) h[o++] = 0;
    if (t->kind == PF_TEXT_ITXT) {
        h[o++] = (uint8_t)comp; h[o++] = 0;
        memcpy(h + o, lg, ln); o += ln; h[o++] = 0;
        memcpy(h + o, tk, tkn); o += tkn; h[o++] = 0;
    }
    const void *body = t->text; size_t bn = tn; uint8_t *z = NULL;
    if (comp) { z = zlib9(t->text ? t->text : "", tn, &bn, w->A); if (!z) w->err = PF_ERR_NOMEM; body = z; }
    static const char *const ty[3] = { "tEXt", "zTXt", "iTXt" };
    if (!w->err) wb_chunk(w, ty[t->kind], h, hn, body, bn);
    pf__free(w->A, z); pf__free(w->A, h);
}

pf_status pf__meta_validate(const pf_metadata *m, int color) {
    static const uint8_t bk[7] = { 2, 0, 6, 1, 2, 0, 6 }, sb[7] = { 1, 0, 3, 3, 2, 0, 4 };
    if (m->has_srgb && m->srgb_intent > 3) return PF_ERR_ARGUMENT;
    if (m->has_phys && m->phys_unit > 1) return PF_ERR_ARGUMENT;
    if (m->has_bkgd && m->bkgd_size != bk[color]) return PF_ERR_ARGUMENT;
    if (m->has_sbit && m->sbit_size != sb[color]) return PF_ERR_ARGUMENT;
    if (m->iccp && (!m->iccp_name || !keyword_ok((const uint8_t *)m->iccp_name, strlen(m->iccp_name)))) return PF_ERR_ARGUMENT;
    if (m->text_count && !m->texts) return PF_ERR_ARGUMENT;
    for (size_t i = 0; i < m->text_count; i++) {
        const pf_text *t = &m->texts[i];
        if ((unsigned)t->kind > PF_TEXT_ITXT || !t->keyword || !keyword_ok((const uint8_t *)t->keyword, strlen(t->keyword)))
            return PF_ERR_ARGUMENT;
        if (!t->text && t->text_size) return PF_ERR_ARGUMENT;
        if (t->kind != PF_TEXT_ITXT && t->text && t->text_size && memchr(t->text, 0, t->text_size)) return PF_ERR_ARGUMENT;
        if (t->kind == PF_TEXT_ITXT && ((t->language && strlen(t->language) > 512) ||
                                        (t->translated_keyword && strlen(t->translated_keyword) > 512)))
            return PF_ERR_ARGUMENT;
    }
    if (m->chunk_count && !m->chunks) return PF_ERR_ARGUMENT;
    for (size_t i = 0; i < m->chunk_count; i++) {
        const pf_chunk *c = &m->chunks[i];
        if (!chunk_type_ok(c->type) || (unsigned)c->location > PF_CHUNK_AFTER_IDAT || (c->size && !c->data) ||
            c->size > 0x7FFFFFFFu)
            return PF_ERR_ARGUMENT;
    }
    if (m->exif_size && !m->exif) return PF_ERR_ARGUMENT;
    return PF_OK;
}

pf_status pf__meta_serialize(const pf_metadata *m, int loc, const pf_allocator *A, uint8_t **out, size_t *outn) {
    pf_wbuf w = { NULL, 0, 0, 0, A };
    uint8_t b[32];
    if (loc == 0) {   /* color information must precede PLTE and IDAT */
        if (m->has_cicp) wb_chunk(&w, "cICP", m->cicp, 4, NULL, 0);
        if (m->has_mdcv) {
            for (int i = 0; i < 6; i++) { b[2 * i] = (uint8_t)(m->mdcv_primaries[i] >> 8); b[2 * i + 1] = (uint8_t)m->mdcv_primaries[i]; }
            b[12] = (uint8_t)(m->mdcv_white[0] >> 8); b[13] = (uint8_t)m->mdcv_white[0];
            b[14] = (uint8_t)(m->mdcv_white[1] >> 8); b[15] = (uint8_t)m->mdcv_white[1];
            wbe32(b + 16, m->mdcv_max_lum); wbe32(b + 20, m->mdcv_min_lum);
            wb_chunk(&w, "mDCV", b, 24, NULL, 0);
        }
        if (m->has_clli) { wbe32(b, m->clli_max_cll); wbe32(b + 4, m->clli_max_fall); wb_chunk(&w, "cLLI", b, 8, NULL, 0); }
        if (m->iccp) {
            size_t zn = 0; uint8_t *z = zlib9(m->iccp, m->iccp_size, &zn, A);
            const size_t nn = strlen(m->iccp_name);
            uint8_t hdr[81];
            memcpy(hdr, m->iccp_name, nn); hdr[nn] = 0; hdr[nn + 1] = 0;   /* name NUL method */
            if (!z) w.err = PF_ERR_NOMEM; else wb_chunk(&w, "iCCP", hdr, nn + 2, z, zn);
            pf__free(A, z);
        }
        if (m->has_srgb) wb_chunk(&w, "sRGB", &m->srgb_intent, 1, NULL, 0);
        if (m->has_gama) { wbe32(b, m->gamma); wb_chunk(&w, "gAMA", b, 4, NULL, 0); }
        if (m->has_chrm) { for (int i = 0; i < 8; i++) wbe32(b + 4 * i, m->chrm[i]); wb_chunk(&w, "cHRM", b, 32, NULL, 0); }
        if (m->has_sbit) wb_chunk(&w, "sBIT", m->sbit, m->sbit_size, NULL, 0);
    } else if (loc == 1) {   /* after PLTE / tRNS (bKGD must follow PLTE) */
        if (m->has_bkgd) wb_chunk(&w, "bKGD", m->bkgd, m->bkgd_size, NULL, 0);
        if (m->has_phys) { wbe32(b, m->phys_x); wbe32(b + 4, m->phys_y); b[8] = m->phys_unit; wb_chunk(&w, "pHYs", b, 9, NULL, 0); }
        if (m->exif) wb_chunk(&w, "eXIf", m->exif, m->exif_size, NULL, 0);
        if (m->has_time) {
            b[0] = (uint8_t)(m->time_year >> 8); b[1] = (uint8_t)m->time_year; b[2] = m->time_month; b[3] = m->time_day;
            b[4] = m->time_hour; b[5] = m->time_minute; b[6] = m->time_second;
            wb_chunk(&w, "tIME", b, 7, NULL, 0);
        }
        for (size_t i = 0; i < m->text_count; i++) wb_text(&w, &m->texts[i]);
    }
    for (size_t i = 0; i < m->chunk_count; i++)
        if ((int)m->chunks[i].location == loc) wb_chunk(&w, m->chunks[i].type, m->chunks[i].data, m->chunks[i].size, NULL, 0);
    if (w.err) { pf__free(A, w.p); *out = NULL; *outn = 0; return (pf_status)w.err; }
    *out = w.p; *outn = w.n;
    return PF_OK;
}
