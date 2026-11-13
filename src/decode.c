/* decode.c - PNG decoding: chunk parsing/validation, inflate glue, row driver, Adam7.
 *
 * Non-interlaced images: one libdeflate inflate call writes the filtered scanlines into the output buffer at an
 * offset of 4 output rows; a progress callback (vendored libdeflate extension) unfilters rows in place while the
 * inflater is still running, as soon as they are more than 32 KiB (the deflate window) behind the write position.
 * Unfiltered rows are then transformed (expand / swap16) while still in cache.
 * Interlaced images: inflate the whole stream, then per pass unfilter + transform + scatter.
 */
#include "internal.h"

constexpr size_t WIN_MARGIN = 32768 + 256;
constexpr size_t PROGRESS_STEP = 64u << 10;

typedef struct {
    uint8_t *out; size_t off, stride, rs; uint32_t H, y; int bpp; int err; bool pair_ok, quad_ok;
    const uint8_t *prev;
    bool verify_adler; uint32_t adler; size_t apos;   /* Adler-32 over the filtered bytes, before they are overwritten */
    uint32_t xy;                   /* rows already transformed */
    uint8_t *dst; size_t ostride;  /* separate output (palette / gray / tRNS expansion) */
    qpng_xform x;
} qpng_dec;

/* apply the output transform to unfiltered rows [c->xy, upto) */
static void xform_rows(qpng_dec c[static 1], uint32_t upto) {
    for (uint32_t y = c->xy; y < upto; y++) {
        uint8_t *row = c->out + (size_t)y * c->stride;
        if (c->x.xf == XF_SWAP16) qpng__swap16(c->stride, row);
        else qpng__xform_row(&c->x, c->x.W, row, c->dst + (size_t)y * c->ostride);
    }
    if (upto > c->xy) c->xy = upto;
}

/* Unfilter every row whose raw bytes are complete and no longer needed by the inflater. pos = bytes of `out`
 * written by the inflater so far; final = inflate finished. */
static void process_rows(qpng_dec c[static 1], size_t pos, bool final) {
    const size_t stride = c->stride, rs = c->rs; const int bpp = c->bpp;
    size_t dec = pos > c->off ? pos - c->off : 0;
    size_t ylim = dec / rs; if (ylim > c->H) ylim = c->H;
    if (!final) { size_t yw = pos > WIN_MARGIN ? (pos - WIN_MARGIN) / stride : 0; if (yw < ylim) ylim = yw; }
    if (ylim > c->y) {
        if (c->verify_adler) {
            size_t upto = ylim * rs;
            c->adler = libdeflate_adler32(c->adler, c->out + c->off + c->apos, upto - c->apos); c->apos = upto;
        }
        uint32_t y = c->y; const uint8_t *prev = c->prev;
        while (y < ylim) {
            const uint8_t *src = c->out + c->off + (size_t)y * rs + 1; int f = src[-1];
            uint8_t *dst = c->out + (size_t)y * stride;
            if (f > 4) { c->err = QPNG_ERR_FILTER; c->y = c->H; return; }
            if (prev && c->quad_ok && y + 3 < c->H) {      /* 4 Paeth rows: AVX2 wavefront */
                if (y + 3 < ylim) {
                    int f1 = src[rs - 1], f2 = src[2 * rs - 1], f3 = src[3 * rs - 1];
                    if (f1 > 4 || f2 > 4 || f3 > 4) { c->err = QPNG_ERR_FILTER; c->y = c->H; return; }
                    if (f == 4 && f1 == 4 && f2 == 4 && f3 == 4) {
#if QPNG_X86
                        qpng__quad_paeth_avx2(stride, rs, stride, (size_t)bpp, dst, src, prev); prev = dst + 3 * stride; y += 4; continue;
#endif
                    }
                } else if (!final) break;
            }
            qpng_pair_fn pair = nullptr;
            if (prev && c->pair_ok && y + 1 < c->H && (f == 1 || f == 3 || f == 4)) {
                if (y + 1 >= ylim) { if (!final) break; }      /* keep the pair together: wait for row y+1 */
                else if (src[stride] <= 4) pair = qpng__pair_for_bpp(f, src[stride], bpp);
            }
            if (pair) { pair(stride, (size_t)bpp, dst, src, prev, dst + stride, src + stride + 1); prev = dst + stride; y += 2; continue; }
            qpng__unfilter_row(f, stride, bpp, dst, src, prev);
            prev = dst; y++;
        }
        c->y = y; c->prev = prev;
    }
    /* swap16 runs one row behind: the row above is still the unfilter predecessor */
    if (c->x.xf != XF_NONE) xform_rows(c, final ? c->y : (c->x.xf == XF_SWAP16 ? (c->y ? c->y - 1 : 0) : c->y));
}
static void on_progress(void *ctx, const unsigned char *out_next) { qpng_dec *c = ctx; process_rows(c, (size_t)(out_next - c->out), false); }

constexpr uint8_t A7[7][4] = { {0,0,8,8}, {4,0,8,8}, {0,4,4,8}, {2,0,4,4}, {0,2,2,4}, {1,0,2,2}, {0,1,1,2} };  /* x0,y0,dx,dy */

void qpng_decode_options_init(qpng_decode_options o[static 1]) {
    *o = (qpng_decode_options){ .output = QPNG_OUTPUT_RAW, .verify_crc = true };
}

qpng_status qpng__parse_ihdr(const uint8_t ih[static 13], const qpng_decode_options *o, qpng_info info[static 1]) {
    const uint32_t W = be32(ih), H = be32(ih + 4);
    const int ch = qpng__channels(ih[9], ih[8]);
    if (!W || !H || W > 0x7FFFFFFFu || H > 0x7FFFFFFFu || ih[10] || ih[11] || ih[12] > 1 || !ch) return QPNG_ERR_HEADER;
    if (o && (W > (o->max_width ? o->max_width : 1u << 24) || H > (o->max_height ? o->max_height : 1u << 24)))
        return QPNG_ERR_LIMITS;
    info->width = W; info->height = H; info->bit_depth = ih[8]; info->color_type = ih[9];
    info->interlaced = ih[12]; info->channels = (uint8_t)ch;
    return QPNG_OK;
}

/* signature + IHDR framing + fields */
static qpng_status read_ihdr(size_t n, const uint8_t buf[static n], const qpng_decode_options *o, qpng_info info[static 1]) {
    if (n < 8 + 25 || memcmp(buf, QPNG__SIG, sizeof QPNG__SIG)) return QPNG_ERR_SIGNATURE;
    if (be32(buf + 8) != 13 || memcmp(buf + 12, "IHDR", 4)) return QPNG_ERR_HEADER;
    return qpng__parse_ihdr(buf + 16, o, info);
}

qpng_status qpng_read_info(size_t n, const uint8_t png[n], qpng_info *info) {
    if (!png || !info) return QPNG_ERR_ARGUMENT;
    *info = (qpng_info){};
    const qpng_status s = read_ihdr(n, png, nullptr, info);
    if (s) return s;
    for (const uint8_t *p = png + 33, *end = png + n; p + 12 <= end;) {
        uint32_t len = be32(p);
        if (len > (size_t)(end - p) - 12) break;
        if (!memcmp(p + 4, "PLTE", 4)) info->palette_entries = (uint16_t)(len / 3);
        else if (!memcmp(p + 4, "tRNS", 4)) info->trns_bytes = (uint16_t)(len > 0xFFFF ? 0xFFFF : len);
        else if (!memcmp(p + 4, "IDAT", 4) || !memcmp(p + 4, "IEND", 4)) break;
        p += 12 + len;
    }
    return QPNG_OK;
}

/* Adam7: unfilter each pass row into a 2-row ping-pong buffer (rb), transform to output pixels, scatter into ob.
 * Kept out of qpng_decode: the short pass rows make per-row overhead visible, and the caller's cleanup-owned
 * locals cost registers across the per-row calls (-5..15% on interlaced 8-bit gray / palette). */
static qpng_status adam7(const qpng_xform x[static 1], const uint8_t *raw, uint8_t *ob, uint8_t *rb, uint32_t W,
                         uint32_t H, int bits, int bpp, size_t stride, size_t ostride, int obits) {
    const int opx = obits >= 8 ? obits / 8 : 0;
    uint8_t *rows[2] = { rb, rb + stride + 32 }, *trow = rb + 2 * (stride + 32);
    const uint8_t *q = raw;
    for (int ps = 0; ps < 7; ps++) {
        uint32_t x0 = A7[ps][0], y0 = A7[ps][1], dx = A7[ps][2], dy = A7[ps][3];
        if (W <= x0 || H <= y0) continue;
        uint32_t pw = (W - x0 + dx - 1) / dx, ph = (H - y0 + dy - 1) / dy;
        size_t pst = ((size_t)pw * (size_t)bits + 7) / 8;
        const uint8_t *prev = nullptr;
        for (uint32_t j = 0; j < ph; j++, q += pst + 1) {
            int f = q[0]; if (f > 4) return QPNG_ERR_FILTER;
            uint8_t *cur = rows[j & 1];
            qpng__unfilter_row(f, pst, bpp, cur, q + 1, prev);
            prev = cur;
            const uint8_t *px = cur;
            if (x->xf == XF_SWAP16) { memcpy(trow, cur, pst); qpng__swap16(pst, trow); px = trow; }
            else if (x->xf != XF_NONE) { qpng__xform_row(x, pw, cur, trow); px = trow; }
            uint8_t *orow = ob + (size_t)(y0 + j * dy) * ostride;
            if (opx) {
#define SC(N) for (uint32_t i = 0; i < pw; i++) memcpy(orow + (size_t)(x0 + i * dx) * N, px + (size_t)i * N, N); break;
                switch (opx) { case 1: SC(1) case 2: SC(2) case 3: SC(3) case 4: SC(4) case 6: SC(6) case 8: SC(8) default: SC((size_t)opx) }
#undef SC
            } else {   /* raw sub-8-bit output: bit scatter (MSB first) */
                for (uint32_t i = 0; i < pw; i++) {
                    uint32_t v = qpng__unpack(px, i, obits), bit = (x0 + i * dx) * (uint32_t)obits;
                    orow[bit >> 3] |= (uint8_t)(v << ((uint32_t)(8 - obits) - (bit & 7)));
                }
            }
        }
    }
    return QPNG_OK;
}

qpng_status qpng_decode(size_t n, const uint8_t png[n], const qpng_decode_options *opts, qpng_image *im) {
    qpng_decode_options defo;
    if (!im) return QPNG_ERR_ARGUMENT;
    *im = (qpng_image){};
    if (!png) return QPNG_ERR_ARGUMENT;
    if (!opts) { qpng_decode_options_init(&defo); opts = &defo; }
    if ((unsigned)opts->output > QPNG_OUTPUT_IMAGERS) return QPNG_ERR_ARGUMENT;
    const qpng_allocator *A = opts->allocator;
    const uint64_t maxout = opts->max_output_bytes ? opts->max_output_bytes : (1ull << 30);

    qpng_info in;
    const qpng_status hs = read_ihdr(n, png, opts, &in);
    if (hs) return hs;
    if (opts->verify_crc && libdeflate_crc32(0, png + 12, 17) != be32(png + 29)) return QPNG_ERR_CRC;
    const uint32_t W = in.width, H = in.height;
    const int depth = in.bit_depth, color = in.color_type, interlace = in.interlaced, ich = in.channels;
    const int bits = ich * depth, bpp = bits >= 8 ? bits / 8 : 1;
    size_t stride;
    if (qpng__row_bytes(&stride, W, bits)) return QPNG_ERR_LIMITS;

    /* pass 1: chunk layout (rules as in the png crate: PLTE/tRNS before IDAT, no duplicates, no unknown critical
     * chunks); ancillary chunks (gAMA, sRGB, iCCP, text, APNG acTL/fcTL/fdAT, ...) are skipped. */
    const uint8_t *p = png + 8, *end = png + n, *z1 = nullptr, *plte = nullptr, *trns = nullptr;
    size_t ztot = 0; uint32_t plen = 0, tlen = 0; int nidat = 0; bool idat_done = false;
    while (p + 12 <= end) {
        uint32_t len = be32(p);
        if (len > (size_t)(end - p) - 12) return QPNG_ERR_CHUNK;
        const uint8_t *t = p + 4;
        if (!memcmp(t, "IDAT", 4)) {
            if (idat_done) return QPNG_ERR_CHUNK;          /* IDATs must be consecutive */
            if (!nidat) z1 = p + 8;
            nidat++; ztot += len;
        } else {
            if (nidat) idat_done = true;
            if (!memcmp(t, "IEND", 4)) break;
            else if (!memcmp(t, "PLTE", 4)) { if (plte || nidat || len % 3 || len > 768 || !len) return QPNG_ERR_CHUNK; plte = p + 8; plen = len / 3; }
            else if (!memcmp(t, "tRNS", 4)) { if (trns || nidat) return QPNG_ERR_CHUNK; trns = p + 8; tlen = len; }
            else if (!memcmp(t, "IHDR", 4)) { if (p != png + 8) return QPNG_ERR_CHUNK; }
            else if (!(t[0] & 0x20)) return QPNG_ERR_CHUNK;   /* unknown critical chunk */
        }
        p += 12 + len;
    }
    if (!nidat || ztot < 2) return QPNG_ERR_CHUNK;
    if (!qpng__plte_trns_ok(color, plte != nullptr, trns != nullptr, tlen)) return QPNG_ERR_CHUNK;

    qpng_dec c = {};
    int och, odepth, ocolor;
    qpng__xform_setup(&c.x, color, depth, ich, W, opts->output, plen, plte, tlen, trns, &och, &odepth, &ocolor);
    size_t ostride;
    if (qpng__row_bytes(&ostride, W, och * odepth)) return QPNG_ERR_LIMITS;
    c.ostride = ostride;
    /* raw (filtered) size */
    size_t rawn = 0, rs;
    if (qpng__add_size(&rs, stride, 1)) return QPNG_ERR_LIMITS;
    if (!interlace) {
        if (qpng__mul_size(&rawn, H, rs)) return QPNG_ERR_LIMITS;
    } else for (int ps = 0; ps < 7; ps++) {
        if (W <= A7[ps][0] || H <= A7[ps][1]) continue;
        size_t pw = (W - A7[ps][0] + A7[ps][2] - 1) / A7[ps][2], ph = (H - A7[ps][1] + A7[ps][3] - 1) / A7[ps][3];
        size_t pbytes, prow, add;
        if (qpng__row_bytes(&pbytes, (uint32_t)pw, bits) || qpng__add_size(&prow, pbytes, 1) ||
            qpng__mul_size(&add, ph, prow) || qpng__add_size(&rawn, rawn, add))
            return QPNG_ERR_LIMITS;
    }
    size_t off = 0, out_bytes, need;
    if (!interlace && qpng__mul_size(&off, stride, 4)) return QPNG_ERR_LIMITS;   /* raw rows >= 4 output rows ahead */
    if (qpng__mul_size(&out_bytes, ostride, H) || out_bytes > maxout) return QPNG_ERR_LIMITS;
    uint64_t need_lim;
    if (ckd_add(&need_lim, maxout, maxout >> 2) || ckd_add(&need_lim, need_lim, 1u << 20)) need_lim = UINT64_MAX;
    if (qpng__add_size(&need, rawn, off) || (uint64_t)need > need_lim) return QPNG_ERR_LIMITS;

    /* Owned buffers: early return frees them; success takes out/obuf into im->pixels. */
    qpng_owned zbuf = { .A = A, .p = nullptr };
    qpng_owned out = { .A = A, .p = nullptr };
    qpng_owned obuf = { .A = A, .p = nullptr };
    qpng_owned rb = { .A = A, .p = nullptr };
    QPNG_CLEANUP(qpng__free_decompressorp) struct libdeflate_decompressor *d = nullptr;

    /* pass 2: CRCs + gather split IDATs */
    const uint8_t *z = z1;
    if (nidat > 1) { zbuf.p = qpng__alloc(A, ztot); if (!zbuf.p) return QPNG_ERR_NOMEM; }
    if (opts->verify_crc || zbuf.p) {
        uint8_t *zo = zbuf.p;
        for (p = png + 8; p + 12 <= end; p += 12 + be32(p)) {
            uint32_t len = be32(p); bool idat = !memcmp(p + 4, "IDAT", 4);
            if (opts->verify_crc && libdeflate_crc32(0, p + 4, len + 4) != be32(p + 8 + len)) return QPNG_ERR_CRC;
            if (idat && zbuf.p) { memcpy(zo, p + 8, len); zo += len; }
            if (!memcmp(p + 4, "IEND", 4)) break;
        }
        if (zbuf.p) z = zbuf.p;
    }
    if (!qpng__zlib_hdr_ok(z)) return QPNG_ERR_ZLIB;

    size_t raw_span, out_cap;
    if (qpng__add_size(&raw_span, off, rawn) || qpng__add_size(&out_cap, raw_span, 64)) return QPNG_ERR_LIMITS;
    out.p = qpng__alloc(A, out_cap);
    if (!out.p) return QPNG_ERR_NOMEM;
    const bool sep = interlace || (c.x.xf != XF_NONE && c.x.xf != XF_SWAP16);   /* output != unfiltered rows */
    if (sep) {
        size_t obuf_cap;
        if (qpng__add_size(&obuf_cap, out_bytes, 16)) return QPNG_ERR_LIMITS;
        obuf.p = qpng__alloc(A, obuf_cap);
        if (!obuf.p) return QPNG_ERR_NOMEM;
        if (interlace && odepth < 8) memset(obuf.p, 0, obuf_cap);
    }
    c.verify_adler = opts->verify_adler; c.adler = 1;
    c.out = out.p; c.off = off; c.stride = stride; c.rs = rs; c.H = H; c.bpp = bpp; c.dst = obuf.p;
    c.pair_ok = (bpp <= 4 || bpp == 6 || bpp == 8) && stride >= 16;
    c.quad_ok = c.pair_ok && bpp >= 3 && bpp <= 4 && qpng__have_avx2();
    d = libdeflate_alloc_decompressor();
    if (!d) return QPNG_ERR_NOMEM;
    libdeflate_set_pair_literals(d, 1);
    size_t ain = 0, aout = 0; enum libdeflate_result r;
    if (interlace) r = libdeflate_deflate_decompress_ex(d, z + 2, ztot - 2, out.p + off, rawn, &ain, &aout);
    else r = libdeflate_deflate_decompress_progress(d, z + 2, ztot - 2, out.p + off, rawn, &ain, &aout, on_progress, &c, PROGRESS_STEP);
    libdeflate_free_decompressor(d); d = nullptr;
    if (r != LIBDEFLATE_SUCCESS || aout != rawn) return QPNG_ERR_DEFLATE;
    if (opts->verify_adler && ain + 6 > ztot) return QPNG_ERR_ADLER;   /* no room for the Adler-32 trailer */
    if (!interlace) {
        process_rows(&c, off + rawn, true);
        if (c.err) return (qpng_status)c.err;
        if (opts->verify_adler && c.adler != be32(z + 2 + ain)) return QPNG_ERR_ADLER;
    } else {
        if (opts->verify_adler && libdeflate_adler32(1, out.p, rawn) != be32(z + 2 + ain)) return QPNG_ERR_ADLER;
        rb.p = qpng__alloc(A, 2 * (stride + 32) + ostride + 32);
        if (!rb.p) return QPNG_ERR_NOMEM;
        const qpng_status as = adam7(&c.x, out.p, obuf.p, rb.p, W, H, bits, bpp, stride, ostride, och * odepth);
        if (as) return as;
    }
    if (sep) {
        im->pixels = qpng__take(&obuf);
        /* out released by cleanup */
    } else {
        void *raw = qpng__take(&out);
        im->pixels = qpng__realloc_shrink(A, raw, out_cap, (size_t)H * stride);
    }
    im->width = W; im->height = H; im->channels = (uint8_t)och; im->bit_depth = (uint8_t)odepth; im->color_type = (uint8_t)ocolor;
    im->stride = ostride; im->size = (size_t)H * ostride;
    if (A) im->allocator = *A;
    return QPNG_OK;
}

void qpng_image_free(qpng_image *im) {
    if (!im) return;
    qpng__free(im->allocator.alloc ? &im->allocator : nullptr, im->pixels);
    *im = (qpng_image){};
}
