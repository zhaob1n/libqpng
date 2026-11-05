/* apng.c - APNG decode (structure validation, frame iterator with composition, raw frames) and encode.
 * Frames are decoded by building a small in-memory PNG (IHDR with the frame size, PLTE, tRNS, one IDAT holding
 * the frame's concatenated IDAT / fdAT payloads) and running the one-shot qpng_decode on it, so every format,
 * output layout and the SIMD paths are shared with still images. Encoding likewise runs qpng_encode per frame and
 * moves its IDAT payload into IDAT / fdAT chunks. */
#include "internal.h"
#include "metadata.h"

typedef struct { const uint8_t *p; uint32_t n; } qpng_seg;
typedef struct { qpng_frame_info fi; uint32_t seg0, nseg; size_t zsize; } qpng_frame;

struct qpng_apng {
    qpng_decode_options o; qpng_allocator a; int has_a;
    const uint8_t *png; size_t size;
    qpng_apng_info info;
    const uint8_t *ihdr, *plte, *trns; uint32_t plen, tlen;   /* chunk payloads in the user buffer */
    qpng_frame *fr; qpng_seg *seg; uint32_t nfr, nseg, capseg;
    uint8_t *canvas, *saved; uint32_t next;                     /* iterator state */
    qpng_frame_info prev; int have_prev;
};
static const qpng_allocator *aA(const qpng_apng *a) { return a->has_a ? &a->a : NULL; }

static int add_seg(qpng_apng *a, const uint8_t *p, uint32_t n) {
    if (a->nseg == a->capseg) {
        uint32_t nc = a->capseg ? a->capseg * 2 : 64;
        qpng_seg *ns = qpng__alloc(aA(a), nc * sizeof *ns);
        if (!ns) return 0;
        if (a->nseg) memcpy(ns, a->seg, a->nseg * sizeof *ns);
        qpng__free(aA(a), a->seg); a->seg = ns; a->capseg = nc;
    }
    a->seg[a->nseg++] = (qpng_seg){ p, n };
    a->fr[a->nfr - 1].nseg++; a->fr[a->nfr - 1].zsize += n;
    return 1;
}

void qpng_apng_free(qpng_apng *a) {
    if (!a) return;
    qpng_allocator ac = a->a; const qpng_allocator *A = a->has_a ? &ac : NULL;
    qpng__free(A, a->fr); qpng__free(A, a->seg); qpng__free(A, a->canvas); qpng__free(A, a->saved); qpng__free(A, a);
}

static qpng_status parse(qpng_apng *a) {
    const uint8_t *b = a->png, *end = b + a->size, *p = b + 33;
    a->ihdr = b + 16;
    const qpng_info *in = &a->info.image;
    /* pass 1: acTL position (must precede the first IDAT to count) */
    int actl = 0; uint32_t nf = 0, plays = 0;
    for (const uint8_t *q = p; q + 12 <= end; ) {
        uint32_t len = be32(q);
        if (len > (size_t)(end - q) - 12) break;
        if (!memcmp(q + 4, "IDAT", 4) || !memcmp(q + 4, "IEND", 4)) break;
        if (!memcmp(q + 4, "acTL", 4)) {
            if (actl++) return QPNG_ERR_APNG;   /* repeated acTL */
            if (len != 8) return QPNG_ERR_APNG;
            nf = be32(q + 8); plays = be32(q + 12);
            if (nf == 0 || nf > 0x7FFFFFFFu || plays > 0x7FFFFFFFu) return QPNG_ERR_APNG;
        }
        q += 12 + len;
    }
    a->info.animated = actl;
    a->info.num_plays = plays;
    if (actl) {
        a->fr = qpng__alloc(aA(a), (size_t)(nf < 4096 ? nf : 4096) * sizeof *a->fr);   /* grown below if needed */
        if (!a->fr) return QPNG_ERR_NOMEM;
    }
    uint32_t capfr = nf < 4096 ? nf : 4096, seq = 0;
    int seen_idat = 0, idat_done = 0, cur_has_data = 1, cur_is_default = 0;
    for (; p + 12 <= end; ) {
        const uint32_t len = be32(p);
        if (len > (size_t)(end - p) - 12) return QPNG_ERR_CHUNK;
        const uint8_t *t = p + 4, *d = p + 8;
        if (a->o.verify_crc && libdeflate_crc32(0, t, len + 4) != be32(d + len) && (!memcmp(t, "fcTL", 4) || !memcmp(t, "fdAT", 4) || !memcmp(t, "acTL", 4)))
            return QPNG_ERR_CRC;
        p += 12 + len;
        if (!memcmp(t, "IEND", 4)) break;
        if (!memcmp(t, "PLTE", 4)) { a->plte = d; a->plen = len; continue; }
        if (!memcmp(t, "tRNS", 4)) { a->trns = d; a->tlen = len; continue; }
        if (!memcmp(t, "IDAT", 4)) {
            if (idat_done) return QPNG_ERR_CHUNK;
            if (!seen_idat) {
                seen_idat = 1;
                if (actl && a->nfr == 1 && !cur_has_data) {   /* fcTL before IDAT: default image is frame 0 */
                    const qpng_frame_info *f = &a->fr[0].fi;
                    if (f->width != in->width || f->height != in->height || f->x_offset || f->y_offset) return QPNG_ERR_APNG;
                    cur_is_default = 1; a->info.default_image_is_frame = 1;
                } else if (actl && a->nfr) return QPNG_ERR_APNG;
            }
            if (cur_is_default) { if (!add_seg(a, d, len)) return QPNG_ERR_NOMEM; cur_has_data = 1; }
            continue;
        }
        if (seen_idat) idat_done = 1;
        if (!actl) continue;
        if (!memcmp(t, "fcTL", 4)) {
            if (len != 26 || be32(d) != seq++) return QPNG_ERR_APNG;
            if (!cur_has_data) return QPNG_ERR_APNG;                 /* previous fcTL without data */
            if (a->nfr == nf) return QPNG_ERR_APNG;                  /* more frames than num_frames */
            qpng_frame_info fi;
            fi.width = be32(d + 4); fi.height = be32(d + 8); fi.x_offset = be32(d + 12); fi.y_offset = be32(d + 16);
            fi.delay_num = (uint16_t)(d[20] << 8 | d[21]); fi.delay_den = (uint16_t)(d[22] << 8 | d[23]);
            fi.dispose_op = d[24]; fi.blend_op = d[25];
            if (!fi.width || !fi.height || fi.x_offset > in->width || fi.width > in->width - fi.x_offset ||
                fi.y_offset > in->height || fi.height > in->height - fi.y_offset || fi.dispose_op > 2 || fi.blend_op > 1)
                return QPNG_ERR_APNG;
            if (a->nfr == capfr) {
                uint32_t nc = capfr * 2;
                qpng_frame *nfp = qpng__alloc(aA(a), (size_t)nc * sizeof *nfp);
                if (!nfp) return QPNG_ERR_NOMEM;
                memcpy(nfp, a->fr, (size_t)a->nfr * sizeof *nfp); qpng__free(aA(a), a->fr); a->fr = nfp; capfr = nc;
            }
            a->fr[a->nfr++] = (qpng_frame){ fi, a->nseg, 0, 0 };
            cur_has_data = 0; cur_is_default = 0;
            continue;
        }
        if (!memcmp(t, "fdAT", 4)) {
            if (len < 4 || be32(d) != seq++) return QPNG_ERR_APNG;
            if (!a->nfr || !seen_idat || a->fr[a->nfr - 1].seg0 + a->fr[a->nfr - 1].nseg != a->nseg ||
                (a->info.default_image_is_frame && a->nfr == 1))
                return QPNG_ERR_APNG;                                 /* fdAT without its fcTL / separated */
            if (!add_seg(a, d + 4, len - 4)) return QPNG_ERR_NOMEM;
            cur_has_data = 1;
            continue;
        }
    }
    if (!seen_idat) return QPNG_ERR_CHUNK;
    if (actl) {
        if (a->nfr != nf || !cur_has_data) return QPNG_ERR_APNG;
        for (uint32_t i = 0; i < a->nfr; i++) if (!a->fr[i].nseg) return QPNG_ERR_APNG;
        a->info.num_frames = nf;
    } else {
        a->info.num_frames = 1; a->info.default_image_is_frame = 1;
    }
    return QPNG_OK;
}

qpng_status qpng_apng_open(const void *png, size_t n, const qpng_decode_options *opts, qpng_apng **out) {
    if (!out || !png) return QPNG_ERR_ARGUMENT;
    *out = NULL;
    qpng_decode_options defo;
    if (!opts) { qpng_decode_options_init(&defo); opts = &defo; }
    qpng_info info;
    qpng_status s = qpng_read_info(png, n, &info);
    if (s) return s;
    qpng_apng *a = qpng__alloc(opts->allocator, sizeof *a);
    if (!a) return QPNG_ERR_NOMEM;
    memset(a, 0, sizeof *a);
    a->o = *opts; if (opts->allocator) { a->a = *opts->allocator; a->has_a = 1; a->o.allocator = &a->a; }
    a->png = png; a->size = n; a->info.image = info;
    s = parse(a);
    if (s) { qpng_apng_free(a); return s; }
    *out = a;
    return QPNG_OK;
}

void qpng_apng_get_info(const qpng_apng *a, qpng_apng_info *info) { if (a && info) *info = a->info; }

/* frame index -> decoded pixels (synthetic PNG through qpng_decode) */
static qpng_status decode_frame(qpng_apng *a, uint32_t idx, const qpng_decode_options *o, qpng_image *img, qpng_frame_info *fi) {
    qpng_frame_info f;
    if (!a->info.animated) {   /* still image: decode the file itself */
        f = (qpng_frame_info){ a->info.image.width, a->info.image.height, 0, 0, 0, 0, 0, 0 };
        if (fi) *fi = f;
        return qpng_decode(a->png, a->size, o, img);
    }
    const qpng_frame *fr = &a->fr[idx];
    f = fr->fi;
    if (fi) *fi = f;
    const size_t total = 8 + 25 + (a->plte ? 12 + a->plen : 0) + (a->trns ? 12 + a->tlen : 0) + 12 + fr->zsize + 12;
    uint8_t *b = qpng__alloc(aA(a), total), *p = b;
    if (!b) return QPNG_ERR_NOMEM;
    memcpy(p, a->png, 8 + 8); p += 16;   /* signature, IHDR length + type */
    wbe32(p, f.width); wbe32(p + 4, f.height); memcpy(p + 8, a->ihdr + 8, 5); p += 13; wbe32(p, 0); p += 4;
    if (a->plte) { wbe32(p, a->plen); memcpy(p + 4, "PLTE", 4); memcpy(p + 8, a->plte, a->plen); p += 12 + a->plen; wbe32(p - 4, 0); }
    if (a->trns) { wbe32(p, a->tlen); memcpy(p + 4, "tRNS", 4); memcpy(p + 8, a->trns, a->tlen); p += 12 + a->tlen; wbe32(p - 4, 0); }
    wbe32(p, (uint32_t)fr->zsize); memcpy(p + 4, "IDAT", 4); p += 8;
    for (uint32_t i = 0; i < fr->nseg; i++) { memcpy(p, a->seg[fr->seg0 + i].p, a->seg[fr->seg0 + i].n); p += a->seg[fr->seg0 + i].n; }
    wbe32(p, 0); p += 4;
    memcpy(p, "\0\0\0\0IEND\xae\x42\x60\x82", 12); p += 12;
    qpng_decode_options oo = *o; oo.verify_crc = 0;   /* synthetic chunks; the real CRCs were checked in parse */
    qpng_status s = qpng_decode(b, (size_t)(p - b), &oo, img);
    qpng__free(aA(a), b);
    return s;
}

qpng_status qpng_apng_raw_frame(qpng_apng *a, uint32_t idx, const qpng_decode_options *o, qpng_image *img, qpng_frame_info *fi) {
    if (img) memset(img, 0, sizeof *img);
    if (!a || !img || idx >= a->info.num_frames) return QPNG_ERR_ARGUMENT;
    qpng_decode_options oo = o ? *o : a->o;
    if (!oo.allocator && a->has_a) oo.allocator = &a->a;
    return decode_frame(a, idx, &oo, img, fi);
}

void qpng_apng_rewind(qpng_apng *a) { if (a) { a->next = 0; a->have_prev = 0; } }

static uint8_t to8(const uint8_t *p, int d16) { return d16 ? (uint8_t)(((p[0] << 8 | p[1]) + 128) / 257) : p[0]; }

qpng_status qpng_apng_next_frame(qpng_apng *a, const uint8_t **canvas, qpng_frame_info *fiout) {
    if (!a || !canvas) return QPNG_ERR_ARGUMENT;
    *canvas = NULL;
    if (a->next >= a->info.num_frames) return QPNG_ERR_STATE;
    const uint32_t W = a->info.image.width, H = a->info.image.height;
    const size_t cs = (size_t)W * 4;
    if (!a->canvas) {
        const uint64_t maxout = a->o.max_output_bytes ? a->o.max_output_bytes : (1ull << 30);
        if ((uint64_t)cs * H > maxout) return QPNG_ERR_LIMITS;
        a->canvas = qpng__alloc(aA(a), cs * H); a->saved = qpng__alloc(aA(a), cs * H);
        if (!a->canvas || !a->saved) return QPNG_ERR_NOMEM;
    }
    if (a->next == 0) { memset(a->canvas, 0, cs * H); a->have_prev = 0; }
    /* dispose of the previous frame */
    if (a->have_prev) {
        const qpng_frame_info *p = &a->prev;
        if (p->dispose_op == QPNG_DISPOSE_BACKGROUND)
            for (uint32_t y = 0; y < p->height; y++) memset(a->canvas + (size_t)(p->y_offset + y) * cs + (size_t)p->x_offset * 4, 0, (size_t)p->width * 4);
        else if (p->dispose_op == QPNG_DISPOSE_PREVIOUS)
            for (uint32_t y = 0; y < p->height; y++) {
                size_t o = (size_t)(p->y_offset + y) * cs + (size_t)p->x_offset * 4;
                memcpy(a->canvas + o, a->saved + o, (size_t)p->width * 4);
            }
    }
    qpng_decode_options o = a->o; o.output = QPNG_OUTPUT_EXPAND;
    qpng_image im; qpng_frame_info f;
    qpng_status s = decode_frame(a, a->next, &o, &im, &f);
    if (s) return s;
    if (a->next == 0 && f.dispose_op == QPNG_DISPOSE_PREVIOUS) f.dispose_op = QPNG_DISPOSE_BACKGROUND;
    if (f.dispose_op == QPNG_DISPOSE_PREVIOUS)   /* keep the region as it is before this frame */
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
            if (f.blend_op == QPNG_BLEND_SOURCE || al == 255) { dst[0] = r; dst[1] = g; dst[2] = b; dst[3] = al; continue; }
            if (al == 0) continue;
            /* source over destination, 8-bit with rounding */
            const uint32_t ad = (uint32_t)dst[3] * (255u - al), oa = (uint32_t)al * 255u + ad;
            dst[0] = (uint8_t)(((uint32_t)r * al * 255u + (uint32_t)dst[0] * ad + oa / 2) / oa);
            dst[1] = (uint8_t)(((uint32_t)g * al * 255u + (uint32_t)dst[1] * ad + oa / 2) / oa);
            dst[2] = (uint8_t)(((uint32_t)b * al * 255u + (uint32_t)dst[2] * ad + oa / 2) / oa);
            dst[3] = (uint8_t)((oa + 127) / 255);
        }
    }
    qpng_image_free(&im);
    a->prev = f; a->have_prev = 1; a->next++;
    *canvas = a->canvas;
    if (fiout) *fiout = f;
    return QPNG_OK;
}

/* ---- encode ---- */
/* IDAT or (with a sequence number) fdAT chunk */
static void put_data(qpng__wbuf *o, uint32_t *seq, const uint8_t *d, size_t n) {
    if (!seq) {
        qpng__wb_chunk(o, "IDAT", d, n);
        return;
    }
    const size_t c = qpng__wb_begin(o, "fdAT");
    uint8_t sb[4];
    wbe32(sb, (*seq)++);
    qpng__wb_put(o, sb, 4);
    qpng__wb_put(o, d, n);
    qpng__wb_end(o, c);
}

/* encode one image with qpng_encode and append its IDAT payload as IDAT (seq == nullptr) or fdAT chunks */
static qpng_status put_frame(qpng__wbuf *o, const void *px, size_t stride, uint32_t w, uint32_t h, qpng_color color,
                             int depth, const qpng_encode_options *eo, uint32_t *seq) {
    uint8_t *png; size_t pn;
    qpng_status s = qpng_encode(px, w, h, stride, color, depth, eo, &png, &pn);
    if (s) return s;
    qpng_owned own = { eo->allocator, png };
    constexpr size_t max_chunk = 1u << 20;
    for (size_t q = 8; q + 12 <= pn; q += 12 + be32(png + q)) {
        if (memcmp(png + q + 4, "IDAT", 4)) continue;
        const uint8_t *d = png + q + 8;
        const size_t len = be32(png + q);
        size_t k = 0;
        do {
            const size_t c = len - k < max_chunk ? len - k : max_chunk;
            put_data(o, seq, d + k, c);
            k += c;
        } while (k < len);
    }
    return o->err;
}

qpng_status qpng_encode_apng(uint32_t W, uint32_t H, qpng_color color, int depth, const qpng_apng_frame *fr, uint32_t nf,
                             uint32_t plays, const void *defimg, size_t defstride, const qpng_encode_options *opts,
                             uint8_t **out, size_t *outn) {
    if (!out || !outn) return QPNG_ERR_ARGUMENT;
    *out = nullptr; *outn = 0;
    qpng_encode_options defo;
    if (!opts) { qpng_encode_options_init(&defo); opts = &defo; }
    if (!fr || !nf || nf > 0x7FFFFFFFu || plays > 0x7FFFFFFFu || !W || !H) return QPNG_ERR_ARGUMENT;
    for (uint32_t i = 0; i < nf; i++) {
        const qpng_frame_info *f = &fr[i].info;
        if (!fr[i].pixels || !f->width || !f->height || f->x_offset > W || f->width > W - f->x_offset || f->y_offset > H ||
            f->height > H - f->y_offset || f->dispose_op > 2 || f->blend_op > 1)
            return QPNG_ERR_ARGUMENT;
    }
    if (!defimg && (fr[0].info.width != W || fr[0].info.height != H || fr[0].info.x_offset || fr[0].info.y_offset))
        return QPNG_ERR_ARGUMENT;
    const qpng_metadata *meta = opts->metadata;
    if (meta) {
        qpng_status s = qpng__meta_validate(meta, color);
        if (s) return s;
    }
    qpng_encode_options eo = *opts;   /* per-frame qpng_encode: payload only, metadata is written here */
    eo.metadata = nullptr;
    QPNG_CLEANUP(qpng__wb_release) qpng__wbuf o = { .A = opts->allocator };
    static constexpr uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    uint8_t ih[13] = { [8] = (uint8_t)depth, [9] = (uint8_t)color }, ac[8];
    wbe32(ih, W); wbe32(ih + 4, H);
    wbe32(ac, nf); wbe32(ac + 4, plays);
    qpng__wb_put(&o, sig, sizeof sig);
    qpng__wb_chunk(&o, "IHDR", ih, sizeof ih);
    qpng__wb_chunk(&o, "acTL", ac, sizeof ac);
    if (meta) qpng__meta_write(&o, meta, 0);
    if (opts->palette && opts->palette_entries > 0 && opts->palette_entries <= 256)
        qpng__wb_chunk(&o, "PLTE", opts->palette, 3u * (size_t)opts->palette_entries);
    if (opts->trns && opts->trns_bytes > 0 && opts->trns_bytes <= 256)
        qpng__wb_chunk(&o, "tRNS", opts->trns, (size_t)opts->trns_bytes);
    if (meta) qpng__meta_write(&o, meta, 1);
    if (o.err) return o.err;
    uint32_t seq = 0;
    if (defimg) {
        qpng_status s = put_frame(&o, defimg, defstride, W, H, color, depth, &eo, nullptr);
        if (s) return s;
    }
    for (uint32_t i = 0; i < nf; i++) {
        const qpng_frame_info *f = &fr[i].info;
        uint8_t fc[26];
        wbe32(fc, seq++);
        wbe32(fc + 4, f->width); wbe32(fc + 8, f->height); wbe32(fc + 12, f->x_offset); wbe32(fc + 16, f->y_offset);
        fc[20] = (uint8_t)(f->delay_num >> 8); fc[21] = (uint8_t)f->delay_num;
        fc[22] = (uint8_t)(f->delay_den >> 8); fc[23] = (uint8_t)f->delay_den;
        fc[24] = f->dispose_op; fc[25] = f->blend_op;
        qpng__wb_chunk(&o, "fcTL", fc, sizeof fc);
        qpng_status s = put_frame(&o, fr[i].pixels, fr[i].stride, f->width, f->height, color, depth, &eo,
                                  defimg || i > 0 ? &seq : nullptr);
        if (s) return s;
    }
    if (meta) qpng__meta_write(&o, meta, 2);
    qpng__wb_chunk(&o, "IEND", nullptr, 0);
    if (o.err) return o.err;
    *outn = o.n;
    *out = QPNG_TAKE(o.p);
    return QPNG_OK;
}
