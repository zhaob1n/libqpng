/* apng.c - APNG decode (structure validation, frame iterator with composition, raw frames) and encode.
 * Frames are decoded by building a small in-memory PNG (IHDR with the frame size, PLTE, tRNS, one IDAT holding
 * the frame's concatenated IDAT / fdAT payloads) and running the one-shot pf_decode on it, so every format,
 * output layout and the SIMD paths are shared with still images. Encoding likewise runs pf_encode per frame and
 * moves its IDAT payload into IDAT / fdAT chunks. */
#include "internal.h"
#include "metadata.h"

typedef struct { const uint8_t *p; uint32_t n; } pf_seg;
typedef struct { pf_frame_info fi; uint32_t seg0, nseg; size_t zsize; } pf_frame;

struct pf_apng {
    pf_decode_options o; pf_allocator a; int has_a;
    const uint8_t *png; size_t size;
    pf_apng_info info;
    const uint8_t *ihdr, *plte, *trns; uint32_t plen, tlen;   /* chunk payloads in the user buffer */
    pf_frame *fr; pf_seg *seg; uint32_t nfr, nseg, capseg;
    uint8_t *canvas, *saved; uint32_t next;                     /* iterator state */
    pf_frame_info prev; int have_prev;
};
static const pf_allocator *aA(const pf_apng *a) { return a->has_a ? &a->a : NULL; }

static int add_seg(pf_apng *a, const uint8_t *p, uint32_t n) {
    if (a->nseg == a->capseg) {
        uint32_t nc = a->capseg ? a->capseg * 2 : 64;
        pf_seg *ns = pf__alloc(aA(a), nc * sizeof *ns);
        if (!ns) return 0;
        if (a->nseg) memcpy(ns, a->seg, a->nseg * sizeof *ns);
        pf__free(aA(a), a->seg); a->seg = ns; a->capseg = nc;
    }
    a->seg[a->nseg++] = (pf_seg){ p, n };
    a->fr[a->nfr - 1].nseg++; a->fr[a->nfr - 1].zsize += n;
    return 1;
}

void pf_apng_free(pf_apng *a) {
    if (!a) return;
    pf_allocator ac = a->a; const pf_allocator *A = a->has_a ? &ac : NULL;
    pf__free(A, a->fr); pf__free(A, a->seg); pf__free(A, a->canvas); pf__free(A, a->saved); pf__free(A, a);
}

static pf_status parse(pf_apng *a) {
    const uint8_t *b = a->png, *end = b + a->size, *p = b + 33;
    a->ihdr = b + 16;
    const pf_info *in = &a->info.image;
    /* pass 1: acTL position (must precede the first IDAT to count) */
    int actl = 0; uint32_t nf = 0, plays = 0;
    for (const uint8_t *q = p; q + 12 <= end; ) {
        uint32_t len = be32(q);
        if (len > (size_t)(end - q) - 12) break;
        if (!memcmp(q + 4, "IDAT", 4) || !memcmp(q + 4, "IEND", 4)) break;
        if (!memcmp(q + 4, "acTL", 4)) {
            if (actl++) return PF_ERR_APNG;   /* repeated acTL */
            if (len != 8) return PF_ERR_APNG;
            nf = be32(q + 8); plays = be32(q + 12);
            if (nf == 0 || nf > 0x7FFFFFFFu || plays > 0x7FFFFFFFu) return PF_ERR_APNG;
        }
        q += 12 + len;
    }
    a->info.animated = actl;
    a->info.num_plays = plays;
    if (actl) {
        a->fr = pf__alloc(aA(a), (size_t)(nf < 4096 ? nf : 4096) * sizeof *a->fr);   /* grown below if needed */
        if (!a->fr) return PF_ERR_NOMEM;
    }
    uint32_t capfr = nf < 4096 ? nf : 4096, seq = 0;
    int seen_idat = 0, idat_done = 0, cur_has_data = 1, cur_is_default = 0;
    for (; p + 12 <= end; ) {
        const uint32_t len = be32(p);
        if (len > (size_t)(end - p) - 12) return PF_ERR_CHUNK;
        const uint8_t *t = p + 4, *d = p + 8;
        if (a->o.verify_crc && libdeflate_crc32(0, t, len + 4) != be32(d + len) && (!memcmp(t, "fcTL", 4) || !memcmp(t, "fdAT", 4) || !memcmp(t, "acTL", 4)))
            return PF_ERR_CRC;
        p += 12 + len;
        if (!memcmp(t, "IEND", 4)) break;
        if (!memcmp(t, "PLTE", 4)) { a->plte = d; a->plen = len; continue; }
        if (!memcmp(t, "tRNS", 4)) { a->trns = d; a->tlen = len; continue; }
        if (!memcmp(t, "IDAT", 4)) {
            if (idat_done) return PF_ERR_CHUNK;
            if (!seen_idat) {
                seen_idat = 1;
                if (actl && a->nfr == 1 && !cur_has_data) {   /* fcTL before IDAT: default image is frame 0 */
                    const pf_frame_info *f = &a->fr[0].fi;
                    if (f->width != in->width || f->height != in->height || f->x_offset || f->y_offset) return PF_ERR_APNG;
                    cur_is_default = 1; a->info.default_image_is_frame = 1;
                } else if (actl && a->nfr) return PF_ERR_APNG;
            }
            if (cur_is_default) { if (!add_seg(a, d, len)) return PF_ERR_NOMEM; cur_has_data = 1; }
            continue;
        }
        if (seen_idat) idat_done = 1;
        if (!actl) continue;
        if (!memcmp(t, "fcTL", 4)) {
            if (len != 26 || be32(d) != seq++) return PF_ERR_APNG;
            if (!cur_has_data) return PF_ERR_APNG;                 /* previous fcTL without data */
            if (a->nfr == nf) return PF_ERR_APNG;                  /* more frames than num_frames */
            pf_frame_info fi;
            fi.width = be32(d + 4); fi.height = be32(d + 8); fi.x_offset = be32(d + 12); fi.y_offset = be32(d + 16);
            fi.delay_num = (uint16_t)(d[20] << 8 | d[21]); fi.delay_den = (uint16_t)(d[22] << 8 | d[23]);
            fi.dispose_op = d[24]; fi.blend_op = d[25];
            if (!fi.width || !fi.height || fi.x_offset > in->width || fi.width > in->width - fi.x_offset ||
                fi.y_offset > in->height || fi.height > in->height - fi.y_offset || fi.dispose_op > 2 || fi.blend_op > 1)
                return PF_ERR_APNG;
            if (a->nfr == capfr) {
                uint32_t nc = capfr * 2;
                pf_frame *nfp = pf__alloc(aA(a), (size_t)nc * sizeof *nfp);
                if (!nfp) return PF_ERR_NOMEM;
                memcpy(nfp, a->fr, (size_t)a->nfr * sizeof *nfp); pf__free(aA(a), a->fr); a->fr = nfp; capfr = nc;
            }
            a->fr[a->nfr++] = (pf_frame){ fi, a->nseg, 0, 0 };
            cur_has_data = 0; cur_is_default = 0;
            continue;
        }
        if (!memcmp(t, "fdAT", 4)) {
            if (len < 4 || be32(d) != seq++) return PF_ERR_APNG;
            if (!a->nfr || !seen_idat || a->fr[a->nfr - 1].seg0 + a->fr[a->nfr - 1].nseg != a->nseg ||
                (a->info.default_image_is_frame && a->nfr == 1))
                return PF_ERR_APNG;                                 /* fdAT without its fcTL / separated */
            if (!add_seg(a, d + 4, len - 4)) return PF_ERR_NOMEM;
            cur_has_data = 1;
            continue;
        }
    }
    if (!seen_idat) return PF_ERR_CHUNK;
    if (actl) {
        if (a->nfr != nf || !cur_has_data) return PF_ERR_APNG;
        for (uint32_t i = 0; i < a->nfr; i++) if (!a->fr[i].nseg) return PF_ERR_APNG;
        a->info.num_frames = nf;
    } else {
        a->info.num_frames = 1; a->info.default_image_is_frame = 1;
    }
    return PF_OK;
}

pf_status pf_apng_open(const void *png, size_t n, const pf_decode_options *opts, pf_apng **out) {
    if (!out || !png) return PF_ERR_ARGUMENT;
    *out = NULL;
    pf_decode_options defo;
    if (!opts) { pf_decode_options_init(&defo); opts = &defo; }
    pf_info info;
    pf_status s = pf_read_info(png, n, &info);
    if (s) return s;
    pf_apng *a = pf__alloc(opts->allocator, sizeof *a);
    if (!a) return PF_ERR_NOMEM;
    memset(a, 0, sizeof *a);
    a->o = *opts; if (opts->allocator) { a->a = *opts->allocator; a->has_a = 1; a->o.allocator = &a->a; }
    a->png = png; a->size = n; a->info.image = info;
    s = parse(a);
    if (s) { pf_apng_free(a); return s; }
    *out = a;
    return PF_OK;
}

void pf_apng_get_info(const pf_apng *a, pf_apng_info *info) { if (a && info) *info = a->info; }

/* frame index -> decoded pixels (synthetic PNG through pf_decode) */
static pf_status decode_frame(pf_apng *a, uint32_t idx, const pf_decode_options *o, pf_image *img, pf_frame_info *fi) {
    pf_frame_info f;
    if (!a->info.animated) {   /* still image: decode the file itself */
        f = (pf_frame_info){ a->info.image.width, a->info.image.height, 0, 0, 0, 0, 0, 0 };
        if (fi) *fi = f;
        return pf_decode(a->png, a->size, o, img);
    }
    const pf_frame *fr = &a->fr[idx];
    f = fr->fi;
    if (fi) *fi = f;
    const size_t total = 8 + 25 + (a->plte ? 12 + a->plen : 0) + (a->trns ? 12 + a->tlen : 0) + 12 + fr->zsize + 12;
    uint8_t *b = pf__alloc(aA(a), total), *p = b;
    if (!b) return PF_ERR_NOMEM;
    memcpy(p, a->png, 8 + 8); p += 16;   /* signature, IHDR length + type */
    wbe32(p, f.width); wbe32(p + 4, f.height); memcpy(p + 8, a->ihdr + 8, 5); p += 13; wbe32(p, 0); p += 4;
    if (a->plte) { wbe32(p, a->plen); memcpy(p + 4, "PLTE", 4); memcpy(p + 8, a->plte, a->plen); p += 12 + a->plen; wbe32(p - 4, 0); }
    if (a->trns) { wbe32(p, a->tlen); memcpy(p + 4, "tRNS", 4); memcpy(p + 8, a->trns, a->tlen); p += 12 + a->tlen; wbe32(p - 4, 0); }
    wbe32(p, (uint32_t)fr->zsize); memcpy(p + 4, "IDAT", 4); p += 8;
    for (uint32_t i = 0; i < fr->nseg; i++) { memcpy(p, a->seg[fr->seg0 + i].p, a->seg[fr->seg0 + i].n); p += a->seg[fr->seg0 + i].n; }
    wbe32(p, 0); p += 4;
    memcpy(p, "\0\0\0\0IEND\xae\x42\x60\x82", 12); p += 12;
    pf_decode_options oo = *o; oo.verify_crc = 0;   /* synthetic chunks; the real CRCs were checked in parse */
    pf_status s = pf_decode(b, (size_t)(p - b), &oo, img);
    pf__free(aA(a), b);
    return s;
}

pf_status pf_apng_raw_frame(pf_apng *a, uint32_t idx, const pf_decode_options *o, pf_image *img, pf_frame_info *fi) {
    if (img) memset(img, 0, sizeof *img);
    if (!a || !img || idx >= a->info.num_frames) return PF_ERR_ARGUMENT;
    pf_decode_options oo = o ? *o : a->o;
    if (!oo.allocator && a->has_a) oo.allocator = &a->a;
    return decode_frame(a, idx, &oo, img, fi);
}

void pf_apng_rewind(pf_apng *a) { if (a) { a->next = 0; a->have_prev = 0; } }

static uint8_t to8(const uint8_t *p, int d16) { return d16 ? (uint8_t)(((p[0] << 8 | p[1]) + 128) / 257) : p[0]; }

pf_status pf_apng_next_frame(pf_apng *a, const uint8_t **canvas, pf_frame_info *fiout) {
    if (!a || !canvas) return PF_ERR_ARGUMENT;
    *canvas = NULL;
    if (a->next >= a->info.num_frames) return PF_ERR_STATE;
    const uint32_t W = a->info.image.width, H = a->info.image.height;
    const size_t cs = (size_t)W * 4;
    if (!a->canvas) {
        const uint64_t maxout = a->o.max_output_bytes ? a->o.max_output_bytes : (1ull << 30);
        if ((uint64_t)cs * H > maxout) return PF_ERR_LIMITS;
        a->canvas = pf__alloc(aA(a), cs * H); a->saved = pf__alloc(aA(a), cs * H);
        if (!a->canvas || !a->saved) return PF_ERR_NOMEM;
    }
    if (a->next == 0) { memset(a->canvas, 0, cs * H); a->have_prev = 0; }
    /* dispose of the previous frame */
    if (a->have_prev) {
        const pf_frame_info *p = &a->prev;
        if (p->dispose_op == PF_DISPOSE_BACKGROUND)
            for (uint32_t y = 0; y < p->height; y++) memset(a->canvas + (size_t)(p->y_offset + y) * cs + (size_t)p->x_offset * 4, 0, (size_t)p->width * 4);
        else if (p->dispose_op == PF_DISPOSE_PREVIOUS)
            for (uint32_t y = 0; y < p->height; y++) {
                size_t o = (size_t)(p->y_offset + y) * cs + (size_t)p->x_offset * 4;
                memcpy(a->canvas + o, a->saved + o, (size_t)p->width * 4);
            }
    }
    pf_decode_options o = a->o; o.output = PF_OUTPUT_EXPAND;
    pf_image im; pf_frame_info f;
    pf_status s = decode_frame(a, a->next, &o, &im, &f);
    if (s) return s;
    if (a->next == 0 && f.dispose_op == PF_DISPOSE_PREVIOUS) f.dispose_op = PF_DISPOSE_BACKGROUND;
    if (f.dispose_op == PF_DISPOSE_PREVIOUS)   /* keep the region as it is before this frame */
        for (uint32_t y = 0; y < f.height; y++) {
            size_t off = (size_t)(f.y_offset + y) * cs + (size_t)f.x_offset * 4;
            memcpy(a->saved + off, a->canvas + off, (size_t)f.width * 4);
        }
    const int ch = im.channels, d16 = im.bit_depth == 16, bp = ch * (d16 ? 2 : 1);
    for (uint32_t y = 0; y < f.height; y++) {
        const uint8_t *src = im.pixels + (size_t)y * im.stride;
        uint8_t *dst = a->canvas + (size_t)(f.y_offset + y) * cs + (size_t)f.x_offset * 4;
        for (uint32_t x = 0; x < f.width; x++, src += bp, dst += 4) {
            uint8_t r, g, b, al;
            if (ch <= 2) { r = g = b = to8(src, d16); al = ch == 2 ? to8(src + (d16 ? 2 : 1), d16) : 255; }
            else { r = to8(src, d16); g = to8(src + (d16 ? 2 : 1), d16); b = to8(src + (d16 ? 4 : 2), d16); al = ch == 4 ? to8(src + (d16 ? 6 : 3), d16) : 255; }
            if (f.blend_op == PF_BLEND_SOURCE || al == 255) { dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = al; continue; }
            if (al == 0) continue;
            /* source over destination, 8-bit with rounding */
            const uint32_t ad = (uint32_t)dst[3] * (255u - al), oa = (uint32_t)al * 255u + ad;
            dst[0] = (uint8_t)(((uint32_t)r * al * 255u + (uint32_t)dst[0] * ad + oa / 2) / oa);
            dst[1] = (uint8_t)(((uint32_t)g * al * 255u + (uint32_t)dst[1] * ad + oa / 2) / oa);
            dst[2] = (uint8_t)(((uint32_t)b * al * 255u + (uint32_t)dst[2] * ad + oa / 2) / oa);
            dst[3] = (uint8_t)((oa + 127) / 255);
        }
    }
    pf_image_free(&im);
    a->prev = f; a->have_prev = 1; a->next++;
    *canvas = a->canvas;
    if (fiout) *fiout = f;
    return PF_OK;
}

/* ---- encode ---- */
typedef struct { uint8_t *p; size_t n, cap; const pf_allocator *A; int err; } obuf;
static void ob_put(obuf *o, const void *d, size_t n) {
    if (o->err) return;
    if (n > o->cap - o->n) {
        size_t nc = (o->cap + n) * 2;
        uint8_t *np = pf__alloc(o->A, nc);
        if (!np) { o->err = PF_ERR_NOMEM; return; }
        if (o->n) memcpy(np, o->p, o->n);
        pf__free(o->A, o->p); o->p = np; o->cap = nc;
    }
    if (n) memcpy(o->p + o->n, d, n);
    o->n += n;
}
/* chunk with optional 4-byte sequence-number prefix */
static void ob_chunk(obuf *o, const char *type, const int64_t seq, const uint8_t *d, size_t n) {
    uint8_t h[12]; const size_t extra = seq >= 0 ? 4 : 0;
    wbe32(h, (uint32_t)(n + extra)); memcpy(h + 4, type, 4); if (extra) wbe32(h + 8, (uint32_t)seq);
    uint32_t crc = libdeflate_crc32(0, h + 4, 4 + extra);
    if (n) crc = libdeflate_crc32(crc, d, n);
    ob_put(o, h, 8 + extra); ob_put(o, d, n);
    uint8_t c[4]; wbe32(c, crc); ob_put(o, c, 4);
}

/* encode one image with pf_encode and append its IDAT payload as IDAT / fdAT chunks */
static pf_status put_frame(obuf *o, const void *px, size_t stride, uint32_t w, uint32_t h, pf_color color, int depth,
                           const pf_encode_options *eo, int as_fdat, uint32_t *seq) {
    uint8_t *png; size_t pn;
    pf_status s = pf_encode(px, w, h, stride, color, depth, eo, &png, &pn);
    if (s) return s;
    for (size_t q = 8; q + 12 <= pn; ) {
        const uint32_t len = be32(png + q);
        if (!memcmp(png + q + 4, "IDAT", 4)) {
            for (size_t k = 0; k < len || (!len && !k); ) {   /* <= 1 MiB per chunk */
                size_t c = len - k < (1u << 20) ? len - k : (1u << 20);
                ob_chunk(o, as_fdat ? "fdAT" : "IDAT", as_fdat ? (int64_t)(*seq)++ : -1, png + q + 8 + k, c);
                k += c; if (!len) break;
            }
        }
        q += 12 + len;
    }
    pf_buffer_free(png, eo->allocator);
    return (pf_status)o->err;
}

pf_status pf_encode_apng(uint32_t W, uint32_t H, pf_color color, int depth, const pf_apng_frame *fr, uint32_t nf,
                         uint32_t plays, const void *defimg, size_t defstride, const pf_encode_options *opts,
                         uint8_t **out, size_t *outn) {
    if (!out || !outn) return PF_ERR_ARGUMENT;
    *out = NULL; *outn = 0;
    pf_encode_options defo;
    if (!opts) { pf_encode_options_init(&defo); opts = &defo; }
    if (!fr || !nf || nf > 0x7FFFFFFFu || plays > 0x7FFFFFFFu || !W || !H) return PF_ERR_ARGUMENT;
    for (uint32_t i = 0; i < nf; i++) {
        const pf_frame_info *f = &fr[i].info;
        if (!fr[i].pixels || !f->width || !f->height || f->x_offset > W || f->width > W - f->x_offset || f->y_offset > H ||
            f->height > H - f->y_offset || f->dispose_op > 2 || f->blend_op > 1)
            return PF_ERR_ARGUMENT;
    }
    if (!defimg && (fr[0].info.width != W || fr[0].info.height != H || fr[0].info.x_offset || fr[0].info.y_offset))
        return PF_ERR_ARGUMENT;
    /* header + metadata + PLTE/tRNS: from a 1-pixel-high pf_encode of the default image / frame 0 (validates opts) */
    pf_encode_options eo = *opts; eo.metadata = NULL;
    obuf o = { NULL, 0, 0, opts->allocator, 0 };
    uint8_t *m[3] = { NULL, NULL, NULL }; size_t mn[3] = { 0, 0, 0 };
    pf_status s = PF_OK;
    if (opts->metadata) {
        s = pf__meta_validate(opts->metadata, color);
        for (int l = 0; l < 3 && !s; l++) s = pf__meta_serialize(opts->metadata, l, opts->allocator, &m[l], &mn[l]);
    }
    uint32_t seq = 0;
    if (!s) {
        static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
        uint8_t ih[13], ac[8];
        wbe32(ih, W); wbe32(ih + 4, H); ih[8] = (uint8_t)depth; ih[9] = (uint8_t)color; ih[10] = ih[11] = ih[12] = 0;
        wbe32(ac, nf); wbe32(ac + 4, plays);
        ob_put(&o, sig, 8);
        ob_chunk(&o, "IHDR", -1, ih, 13);
        ob_chunk(&o, "acTL", -1, ac, 8);
        ob_put(&o, m[0], mn[0]);
        if (opts->palette && opts->palette_entries > 0 && opts->palette_entries <= 256)
            ob_chunk(&o, "PLTE", -1, opts->palette, 3u * (size_t)opts->palette_entries);
        if (opts->trns && opts->trns_bytes > 0 && opts->trns_bytes <= 256) ob_chunk(&o, "tRNS", -1, opts->trns, (size_t)opts->trns_bytes);
        ob_put(&o, m[1], mn[1]);
        s = (pf_status)o.err;
    }
    if (!s && defimg) s = put_frame(&o, defimg, defstride, W, H, color, depth, &eo, 0, &seq);
    for (uint32_t i = 0; i < nf && !s; i++) {
        const pf_frame_info *f = &fr[i].info;
        uint8_t fc[26];
        wbe32(fc, f->width); wbe32(fc + 4, f->height); wbe32(fc + 8, f->x_offset); wbe32(fc + 12, f->y_offset);
        fc[16] = (uint8_t)(f->delay_num >> 8); fc[17] = (uint8_t)f->delay_num; fc[18] = (uint8_t)(f->delay_den >> 8); fc[19] = (uint8_t)f->delay_den;
        fc[20] = f->dispose_op; fc[21] = f->blend_op;
        ob_chunk(&o, "fcTL", seq++, fc, 22);
        s = put_frame(&o, fr[i].pixels, fr[i].stride, f->width, f->height, color, depth, &eo, defimg || i > 0, &seq);
    }
    if (!s) { ob_put(&o, m[2], mn[2]); ob_chunk(&o, "IEND", -1, NULL, 0); s = (pf_status)o.err; }
    for (int l = 0; l < 3; l++) pf__free(opts->allocator, m[l]);
    if (s) { pf__free(opts->allocator, o.p); return s; }
    *out = o.p; *outn = o.n;
    return PF_OK;
}
