/* metadata.c - ancillary chunks: reading (qpng_read_metadata) and serialisation for the encoder. */
#include "metadata.h"
#include "deflate_backend.h"

/* ---- owned storage: a list of blocks allocated with the user's allocator ---- */
typedef struct qpng_mblock { struct qpng_mblock *next; size_t size; } qpng_mblock;
typedef struct { qpng_mblock *head; qpng_allocator a; bool has_a; size_t total, max_total; } qpng_marena;

static const qpng_allocator *arena_alloc(const qpng_marena m[static 1]) { return m->has_a ? &m->a : nullptr; }
static void *arena_get(qpng_marena m[static 1], size_t n) {
    if (n > m->max_total - m->total) return nullptr;
    size_t alloc_n;
    if (qpng__add_size(&alloc_n, n, sizeof(qpng_mblock) + 1)) return nullptr;
    qpng_mblock *b = qpng__alloc(arena_alloc(m), alloc_n);
    if (!b) return nullptr;
    *b = (qpng_mblock){ .next = m->head, .size = n };
    m->head = b; m->total += n;
    uint8_t *p = (uint8_t *)(b + 1);
    p[n] = 0;   /* every block is NUL-terminated */
    return p;
}
static void *arena_dup(qpng_marena m[static 1], size_t n, const void *s) {
    void *d = arena_get(m, n);
    if (d && n) memcpy(d, s, n);
    return d;
}

void qpng_metadata_options_init(qpng_metadata_options o[static 1]) {
    *o = (qpng_metadata_options){ .verify_crc = true, .keep_unknown = true };
}

void qpng_metadata_free(qpng_metadata *meta) {
    if (!meta) return;
    qpng_marena *m = meta->internal_;
    if (m) {
        const qpng_allocator ac = m->a;   /* m itself is freed last */
        const qpng_allocator *A = m->has_a ? &ac : nullptr;
        for (qpng_mblock *b = m->head, *nx; b; b = nx) { nx = b->next; qpng__free(A, b); }
        qpng__free(A, m);
    }
    *meta = (qpng_metadata){};
}

/* zlib stream -> new buffer (allocator A) of at most `limit` bytes; nullptr if invalid / too large */
[[nodiscard]] static uint8_t *inflate_limited(size_t n, const uint8_t src[n], size_t limit, const qpng_allocator *A,
                                              size_t outn[static 1]) {
    QPNG_CLEANUP(qpng__free_decompressorp) struct libdeflate_decompressor *d = libdeflate_alloc_decompressor();
    if (!d) return nullptr;
    /* libdeflate needs the output size: grow x4 up to the limit */
    size_t cap0;
    if (qpng__mul_size(&cap0, n, 4) || qpng__add_size(&cap0, cap0, 1024) || cap0 > limit) cap0 = limit;
    for (size_t cap = cap0;; cap = cap > limit / 4 ? limit : cap * 4) {
        if (cap > limit) cap = limit;
        size_t alloc_n;
        if (qpng__add_size(&alloc_n, cap, 1)) return nullptr;
        qpng_owned out = { A, qpng__alloc(A, alloc_n) };
        if (!out.p) return nullptr;
        const enum libdeflate_result r = libdeflate_zlib_decompress(d, src, n, out.p, cap, outn);
        if (r == LIBDEFLATE_SUCCESS) return qpng__take(&out);
        if (r != LIBDEFLATE_INSUFFICIENT_SPACE || cap >= limit) return nullptr;
    }
}

static bool keyword_ok(size_t n, const uint8_t k[n]) {
    if (n < 1 || n > 79 || k[0] == ' ' || k[n - 1] == ' ') return false;
    for (size_t i = 0; i < n; i++) {
        if (!((k[i] >= 32 && k[i] <= 126) || k[i] >= 161)) return false;
        if (k[i] == ' ' && i && k[i - 1] == ' ') return false;
    }
    return true;
}
static bool keyword_str_ok(const char *k) { return k && keyword_ok(strlen(k), (const uint8_t *)k); }
static const uint8_t *find0(const uint8_t *p, const uint8_t *end) {
    return p < end ? memchr(p, 0, (size_t)(end - p)) : nullptr;
}

/* growable arrays kept in plain allocations while parsing; copied into the arena at the end */
typedef struct { const qpng_allocator *A; void *v; size_t n, cap, el; } qpng_vec;
static void vec_release(qpng_vec *v) { qpng__free(v->A, v->v); }
static void *vec_push(qpng_vec *v) {
    void *nv = qpng__grow(v->A, v->v, v->n, &v->cap, v->n + 1, v->el, 16);
    if (!nv) return nullptr;
    v->v = nv;
    return (uint8_t *)v->v + v->el * v->n++;
}

/* chunks that are not metadata (image data, palette, transparency, APNG control) */
static const char *const SKIP_TYPES[] = { "IHDR", "PLTE", "IDAT", "IEND", "tRNS", "acTL", "fcTL", "fdAT" };
static bool skip_type(const uint8_t t[static 4]) {
    for (size_t i = 0; i < _Countof(SKIP_TYPES); i++) if (!memcmp(t, SKIP_TYPES[i], 4)) return true;
    return false;
}

/* reads that fail keep meta zeroed: the guard frees what was gathered unless taken */
static void meta_guard_release(qpng_metadata **m) { qpng_metadata_free(*m); }

qpng_status qpng_read_metadata(size_t n, const uint8_t png[n], const qpng_metadata_options *opts, qpng_metadata *meta) {
    if (!meta) return QPNG_ERR_ARGUMENT;
    *meta = (qpng_metadata){};
    if (!png) return QPNG_ERR_ARGUMENT;
    qpng_metadata_options defo;
    if (!opts) { qpng_metadata_options_init(&defo); opts = &defo; }
    const qpng_allocator *A = opts->allocator;
    const size_t max_chunk = opts->max_chunk_decompressed ? opts->max_chunk_decompressed : 16u << 20;
    const size_t max_chunks = opts->max_chunks ? opts->max_chunks : 4096;
    qpng_info info;
    qpng_status st = qpng_read_info(n, png, &info);
    if (st) return st;
    qpng_marena *m = qpng__alloc(A, sizeof *m);
    if (!m) return QPNG_ERR_NOMEM;
    *m = (qpng_marena){ .max_total = opts->max_total ? opts->max_total : 64u << 20 };
    if (A) { m->a = *A; m->has_a = true; }
    meta->internal_ = m;
    QPNG_CLEANUP(meta_guard_release) qpng_metadata *guard = meta;
    QPNG_CLEANUP(vec_release) qpng_vec texts = { .A = A, .el = sizeof(qpng_text) };
    QPNG_CLEANUP(vec_release) qpng_vec chunks = { .A = A, .el = sizeof(qpng_chunk) };
    const uint8_t *p = png + 8, *end = png + n;
    bool seen_plte = false, seen_idat = false, has_plte = false;
    for (const uint8_t *q = p; q + 12 <= end && be32(q) <= (size_t)(end - q) - 12; q += 12 + be32(q)) {
        if (!memcmp(q + 4, "PLTE", 4)) has_plte = true;
        if (!memcmp(q + 4, "IDAT", 4)) break;
    }
    while (p + 12 <= end) {
        const uint32_t len = be32(p);
        if (len > (size_t)(end - p) - 12) return QPNG_ERR_CHUNK;
        const uint8_t *t = p + 4, *d = p + 8, *de = d + len;
        p += 12 + len;
        if (!memcmp(t, "IEND", 4)) break;
        if (!memcmp(t, "IDAT", 4) || !memcmp(t, "fdAT", 4)) { seen_idat = true; continue; }
        if (!memcmp(t, "PLTE", 4)) { seen_plte = true; continue; }
        if (skip_type(t)) continue;
        if (!(t[0] & 0x20)) return QPNG_ERR_CHUNK;          /* unknown critical chunk */
#define BAD() goto bad   /* NOT a do{}while(0) with `continue`: that would only leave the macro's own loop */
#define ONCE(flag) if (meta->flag) BAD();
        if (opts->verify_crc && libdeflate_crc32(0, t, len + 4) != be32(de)) BAD();
        if (!memcmp(t, "gAMA", 4)) { ONCE(has_gama) if (len != 4) BAD(); meta->has_gama = true; meta->gamma = be32(d); }
        else if (!memcmp(t, "cHRM", 4)) { ONCE(has_chrm) if (len != 32) BAD(); meta->has_chrm = true; for (int i = 0; i < 8; i++) meta->chrm[i] = be32(d + 4 * i); }
        else if (!memcmp(t, "sRGB", 4)) { ONCE(has_srgb) if (len != 1 || d[0] > 3) BAD(); meta->has_srgb = true; meta->srgb_intent = d[0]; }
        else if (!memcmp(t, "cICP", 4)) { ONCE(has_cicp) if (len != 4) BAD(); meta->has_cicp = true; memcpy(meta->cicp, d, 4); }
        else if (!memcmp(t, "mDCV", 4)) {
            ONCE(has_mdcv) if (len != 24) BAD();
            meta->has_mdcv = true;
            for (int i = 0; i < 6; i++) meta->mdcv_primaries[i] = be16(d + 2 * i);
            meta->mdcv_white[0] = be16(d + 12); meta->mdcv_white[1] = be16(d + 14);
            meta->mdcv_max_lum = be32(d + 16); meta->mdcv_min_lum = be32(d + 20);
        }
        else if (!memcmp(t, "cLLI", 4)) { ONCE(has_clli) if (len != 8) BAD(); meta->has_clli = true; meta->clli_max_cll = be32(d); meta->clli_max_fall = be32(d + 4); }
        else if (!memcmp(t, "pHYs", 4)) { ONCE(has_phys) if (len != 9 || d[8] > 1) BAD(); meta->has_phys = true; meta->phys_x = be32(d); meta->phys_y = be32(d + 4); meta->phys_unit = d[8]; }
        else if (!memcmp(t, "tIME", 4)) {
            ONCE(has_time) if (len != 7) BAD();
            meta->has_time = true; meta->time_year = be16(d); meta->time_month = d[2]; meta->time_day = d[3];
            meta->time_hour = d[4]; meta->time_minute = d[5]; meta->time_second = d[6];
        }
        else if (!memcmp(t, "bKGD", 4)) { ONCE(has_bkgd) if (len != 1 && len != 2 && len != 6) BAD(); meta->has_bkgd = true; memcpy(meta->bkgd, d, len); meta->bkgd_size = (uint8_t)len; }
        else if (!memcmp(t, "sBIT", 4)) { ONCE(has_sbit) if (len < 1 || len > 4) BAD(); meta->has_sbit = true; memcpy(meta->sbit, d, len); meta->sbit_size = (uint8_t)len; }
        else if (!memcmp(t, "eXIf", 4)) {
            if (meta->exif) BAD();
            const uint8_t *e = arena_dup(m, len, d);
            if (!e) BAD();
            meta->exif = e; meta->exif_size = len;
        }
        else if (!memcmp(t, "iCCP", 4)) {
            if (meta->iccp) BAD();
            const uint8_t *z = find0(d, de);
            if (!z || !keyword_ok((size_t)(z - d), d) || de - z < 2 || z[1] != 0) BAD();
            size_t pn = 0;
            qpng_owned prof = { A, inflate_limited((size_t)(de - z - 2), z + 2, max_chunk, A, &pn) };
            if (!prof.p) BAD();
            const uint8_t *dst = arena_dup(m, pn, prof.p);
            const char *name = arena_dup(m, (size_t)(z - d), d);
            if (!dst || !name) BAD();
            meta->iccp = dst; meta->iccp_size = pn; meta->iccp_name = name;
        }
        else if (!memcmp(t, "tEXt", 4) || !memcmp(t, "zTXt", 4) || !memcmp(t, "iTXt", 4)) {
            if (texts.n + chunks.n >= max_chunks) BAD();
            const uint8_t *z = find0(d, de);
            if (!z || !keyword_ok((size_t)(z - d), d)) BAD();
            qpng_text tx = {};
            const uint8_t *body = z + 1, *lang = nullptr, *tk = nullptr;
            size_t langn = 0, tkn = 0;
            bool comp = false;
            if (t[0] == 't') {
                tx.kind = QPNG_TEXT_TEXT;
            } else if (t[0] == 'z') {
                if (de - body < 1 || body[0] != 0) BAD();
                tx.kind = QPNG_TEXT_ZTXT; comp = true; body++;
            } else {
                if (de - body < 2 || body[0] > 1 || body[1] != 0) BAD();
                tx.kind = QPNG_TEXT_ITXT; comp = body[0] != 0; tx.compressed = comp;
                lang = body + 2;
                const uint8_t *l0 = find0(lang, de);
                if (!l0) BAD();
                langn = (size_t)(l0 - lang); tk = l0 + 1;
                const uint8_t *t0 = find0(tk, de);
                if (!t0) BAD();
                tkn = (size_t)(t0 - tk); body = t0 + 1;
            }
            const uint8_t *txt = body;
            size_t txn = (size_t)(de - body);
            qpng_owned tmp = { A, nullptr };
            if (comp) {
                tmp.p = inflate_limited(txn, body, max_chunk, A, &txn);
                if (!tmp.p) BAD();
                txt = tmp.p;
            }
            tx.keyword = arena_dup(m, (size_t)(z - d), d);
            tx.text = arena_dup(m, txn, txt); tx.text_size = txn;
            if (lang) { tx.language = arena_dup(m, langn, lang); tx.translated_keyword = arena_dup(m, tkn, tk); }
            if (!tx.keyword || !tx.text || (lang && (!tx.language || !tx.translated_keyword))) BAD();
            qpng_text *slot = vec_push(&texts);
            if (!slot) return QPNG_ERR_NOMEM;
            *slot = tx;
        }
        else if (opts->keep_unknown) {
            if (texts.n + chunks.n >= max_chunks) BAD();
            qpng_chunk c = {
                .location = seen_idat ? QPNG_CHUNK_AFTER_IDAT
                          : seen_plte || !has_plte ? QPNG_CHUNK_BEFORE_IDAT : QPNG_CHUNK_BEFORE_PLTE,
                .data = arena_dup(m, len, d), .size = len,
            };
            if (!c.data) BAD();
            memcpy(c.type, t, 4);
            qpng_chunk *slot = vec_push(&chunks);
            if (!slot) return QPNG_ERR_NOMEM;
            *slot = c;
        }
        continue;
    bad:   /* malformed, duplicate, bad CRC or over a limit: ignored */
        meta->skipped++;
#undef ONCE
#undef BAD
    }
    if (texts.n) {
        size_t bytes;
        if (qpng__mul_size(&bytes, texts.n, sizeof(qpng_text)) || !(meta->texts = arena_dup(m, bytes, texts.v)))
            return QPNG_ERR_LIMITS;
        meta->text_count = texts.n;
    }
    if (chunks.n) {
        size_t bytes;
        if (qpng__mul_size(&bytes, chunks.n, sizeof(qpng_chunk)) || !(meta->chunks = arena_dup(m, bytes, chunks.v)))
            return QPNG_ERR_LIMITS;
        meta->chunk_count = chunks.n;
    }
    guard = nullptr;
    return QPNG_OK;
}

/* ---- encoder side ---- */
static bool chunk_type_ok(const char t[static 5]) {
    for (int i = 0; i < 4; i++) if (!((t[i] >= 'a' && t[i] <= 'z') || (t[i] >= 'A' && t[i] <= 'Z'))) return false;
    if (t[4] || !(t[0] & 0x20) || (t[2] & 0x20)) return false;   /* ancillary, reserved bit clear */
    return !skip_type((const uint8_t *)t);
}

static void wb_str(qpng__wbuf w[static 1], const char s[static 1]) { qpng__wb_put(w, strlen(s) + 1, s); }

[[nodiscard]] static uint8_t *zlib9(size_t n, const uint8_t src[n], size_t zn[static 1], const qpng_allocator *A) {
    const qpng_zcfg c = { .backend = QPNG_BACKEND_LIBDEFLATE, .level = 9 };
    const size_t cap = qpng__zbound(n);
    qpng_owned z = { A, qpng__alloc(A, cap) };
    qpng_status st = QPNG_OK;   /* zlib9 collapses every failure to nullptr */
    *zn = z.p ? qpng__zcompress(&c, n, src, cap, z.p, A, &st) : 0;
    return *zn ? qpng__take(&z) : nullptr;
}
/* append zlib(src) at level 9 and close the chunk opened at start */
static void wb_deflated(qpng__wbuf w[static 1], size_t start, size_t n, const uint8_t src[n]) {
    size_t zn = 0;
    qpng_owned z = { w->A, zlib9(n, src, &zn, w->A) };
    if (!z.p) { if (!w->err) w->err = QPNG_ERR_NOMEM; return; }
    qpng__wb_put(w, zn, z.p);
    qpng__wb_end(w, start);
}
static void wb_text(qpng__wbuf w[static 1], const qpng_text t[static 1]) {
    static const char *const ty[3] = { "tEXt", "zTXt", "iTXt" };
    const size_t tn = t->text_size ? t->text_size : t->text ? strlen(t->text) : 0;
    const bool comp = t->kind == QPNG_TEXT_ZTXT || (t->kind == QPNG_TEXT_ITXT && t->compressed);
    const size_t c = qpng__wb_begin(w, ty[t->kind]);
    /* keyword NUL [zTXt: method] [iTXt: flag method lang NUL tkey NUL] */
    wb_str(w, t->keyword);
    if (t->kind == QPNG_TEXT_ZTXT) qpng__wb_put(w, 1, (uint8_t[]){ 0 });
    if (t->kind == QPNG_TEXT_ITXT) {
        qpng__wb_put(w, 2, (uint8_t[]){ (uint8_t)comp, 0 });
        wb_str(w, t->language ? t->language : "");
        wb_str(w, t->translated_keyword ? t->translated_keyword : "");
    }
    if (comp) {
        wb_deflated(w, c, tn, (const uint8_t *)(t->text ? t->text : ""));
    } else {
        qpng__wb_put(w, tn, t->text);
        qpng__wb_end(w, c);
    }
}

qpng_status qpng__meta_validate(const qpng_metadata m[static 1], int color) {
    static const uint8_t bk[7] = { 2, 0, 6, 1, 2, 0, 6 }, sb[7] = { 1, 0, 3, 3, 2, 0, 4 };
    if ((unsigned)color > 6) return QPNG_ERR_ARGUMENT;
    if (m->has_srgb && m->srgb_intent > 3) return QPNG_ERR_ARGUMENT;
    if (m->has_phys && m->phys_unit > 1) return QPNG_ERR_ARGUMENT;
    if (m->has_bkgd && m->bkgd_size != bk[color]) return QPNG_ERR_ARGUMENT;
    if (m->has_sbit && m->sbit_size != sb[color]) return QPNG_ERR_ARGUMENT;
    if (m->iccp && !keyword_str_ok(m->iccp_name)) return QPNG_ERR_ARGUMENT;
    if (m->text_count && !m->texts) return QPNG_ERR_ARGUMENT;
    for (size_t i = 0; i < m->text_count; i++) {
        const qpng_text *t = &m->texts[i];
        if ((unsigned)t->kind > QPNG_TEXT_ITXT || !keyword_str_ok(t->keyword))
            return QPNG_ERR_ARGUMENT;
        if (!t->text && t->text_size) return QPNG_ERR_ARGUMENT;
        if (t->kind != QPNG_TEXT_ITXT && t->text && t->text_size && memchr(t->text, 0, t->text_size)) return QPNG_ERR_ARGUMENT;
        if (t->kind == QPNG_TEXT_ITXT && ((t->language && strlen(t->language) > 512) ||
                                        (t->translated_keyword && strlen(t->translated_keyword) > 512)))
            return QPNG_ERR_ARGUMENT;
    }
    if (m->chunk_count && !m->chunks) return QPNG_ERR_ARGUMENT;
    for (size_t i = 0; i < m->chunk_count; i++) {
        const qpng_chunk *c = &m->chunks[i];
        if (!chunk_type_ok(c->type) || (unsigned)c->location > QPNG_CHUNK_AFTER_IDAT || (c->size && !c->data) ||
            c->size > 0x7FFFFFFFu)
            return QPNG_ERR_ARGUMENT;
    }
    if (m->exif_size && !m->exif) return QPNG_ERR_ARGUMENT;
    return QPNG_OK;
}

void qpng__meta_write(qpng__wbuf w[static 1], const qpng_metadata m[static 1], int loc) {
    uint8_t b[32];
    if (loc == 0) {   /* color information must precede PLTE and IDAT */
        if (m->has_cicp) qpng__wb_chunk(w, "cICP", 4, m->cicp);
        if (m->has_mdcv) {
            for (int i = 0; i < 6; i++) wbe16(b + 2 * i, m->mdcv_primaries[i]);
            wbe16(b + 12, m->mdcv_white[0]); wbe16(b + 14, m->mdcv_white[1]);
            wbe32(b + 16, m->mdcv_max_lum); wbe32(b + 20, m->mdcv_min_lum);
            qpng__wb_chunk(w, "mDCV", 24, b);
        }
        if (m->has_clli) { wbe32(b, m->clli_max_cll); wbe32(b + 4, m->clli_max_fall); qpng__wb_chunk(w, "cLLI", 8, b); }
        if (m->iccp) {   /* name NUL method */
            const size_t c = qpng__wb_begin(w, "iCCP");
            wb_str(w, m->iccp_name);
            qpng__wb_put(w, 1, (uint8_t[]){ 0 });
            wb_deflated(w, c, m->iccp_size, m->iccp);
        }
        if (m->has_srgb) qpng__wb_chunk(w, "sRGB", 1, &m->srgb_intent);
        if (m->has_gama) { wbe32(b, m->gamma); qpng__wb_chunk(w, "gAMA", 4, b); }
        if (m->has_chrm) { for (int i = 0; i < 8; i++) wbe32(b + 4 * i, m->chrm[i]); qpng__wb_chunk(w, "cHRM", 32, b); }
        if (m->has_sbit) qpng__wb_chunk(w, "sBIT", m->sbit_size, m->sbit);
    } else if (loc == 1) {   /* after PLTE / tRNS (bKGD must follow PLTE) */
        if (m->has_bkgd) qpng__wb_chunk(w, "bKGD", m->bkgd_size, m->bkgd);
        if (m->has_phys) { wbe32(b, m->phys_x); wbe32(b + 4, m->phys_y); b[8] = m->phys_unit; qpng__wb_chunk(w, "pHYs", 9, b); }
        if (m->exif) qpng__wb_chunk(w, "eXIf", m->exif_size, m->exif);
        if (m->has_time) {
            wbe16(b, m->time_year);
            b[2] = m->time_month; b[3] = m->time_day; b[4] = m->time_hour; b[5] = m->time_minute; b[6] = m->time_second;
            qpng__wb_chunk(w, "tIME", 7, b);
        }
        for (size_t i = 0; i < m->text_count; i++) wb_text(w, &m->texts[i]);
    }
    for (size_t i = 0; i < m->chunk_count; i++)
        if ((int)m->chunks[i].location == loc) qpng__wb_chunk(w, m->chunks[i].type, m->chunks[i].size, m->chunks[i].data);
}

qpng_status qpng__meta_serialize(const qpng_metadata m[static 1], int loc, const qpng_allocator *A,
                                 uint8_t *out[static 1], size_t outn[static 1]) {
    QPNG_CLEANUP(qpng__wb_release) qpng__wbuf w = { .A = A };
    *out = nullptr; *outn = 0;
    qpng__meta_write(&w, m, loc);
    if (w.err) return w.err;
    *outn = w.n;
    *out = QPNG_TAKE(w.p);
    return QPNG_OK;
}
