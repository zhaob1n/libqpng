/* decode.c - PNG decoding: chunk parsing/validation, inflate glue, row driver, Adam7.
 *
 * Non-interlaced images: one libdeflate inflate call writes the filtered scanlines into the output buffer at an
 * offset of 4 output rows; a progress callback (vendored libdeflate extension) unfilters rows in place while the
 * inflater is still running, as soon as they are more than 32 KiB (the deflate window) behind the write position.
 * Unfiltered rows are then transformed (expand / swap16) while still in cache.
 * Interlaced images: inflate the whole stream, then per pass unfilter + transform + scatter.
 */
#include "internal.h"

#define WIN_MARGIN (32768 + 256)
#define PROGRESS_STEP (64u << 10)

typedef struct {
    uint8_t *out; size_t off, stride, rs; uint32_t H, y; int bpp; int err; int pair_ok, quad_ok;
    const uint8_t *prev;
    int verify_adler; uint32_t adler; size_t apos;   /* Adler-32 over the filtered bytes, before they are overwritten */
    uint32_t xy;                   /* rows already transformed */
    uint8_t *dst; size_t ostride;  /* separate output (palette / gray / tRNS expansion) */
    pf_xform x;
} pf_dec;

/* apply the output transform to unfiltered rows [c->xy, upto) */
static void xform_rows(pf_dec *c, uint32_t upto) {
    for (uint32_t y = c->xy; y < upto; y++) {
        uint8_t *row = c->out + (size_t)y * c->stride;
        if (c->x.xf == XF_SWAP16) pf__swap16(row, c->stride);
        else pf__xform_row(&c->x, row, c->dst + (size_t)y * c->ostride, c->x.W);
    }
    if (upto > c->xy) c->xy = upto;
}

/* Unfilter every row whose raw bytes are complete and no longer needed by the inflater. pos = bytes of `out`
 * written by the inflater so far; final = inflate finished. */
static void process_rows(pf_dec *c, size_t pos, int final) {
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
            if (f > 4) { c->err = PF_ERR_FILTER; c->y = c->H; return; }
            if (prev && c->quad_ok && y + 3 < c->H) {      /* 4 Paeth rows: AVX2 wavefront */
                if (y + 3 < ylim) {
                    int f1 = src[rs - 1], f2 = src[2 * rs - 1], f3 = src[3 * rs - 1];
                    if (f1 > 4 || f2 > 4 || f3 > 4) { c->err = PF_ERR_FILTER; c->y = c->H; return; }
                    if (f == 4 && f1 == 4 && f2 == 4 && f3 == 4) {
                        pf__quad_paeth_avx2(dst, src, prev, stride, rs, stride, bpp); prev = dst + 3 * stride; y += 4; continue;
                    }
                } else if (!final) break;
            }
            pf_pair_fn pf = NULL;
            if (prev && c->pair_ok && y + 1 < c->H && (f == 1 || f == 3 || f == 4)) {
                if (y + 1 >= ylim) { if (!final) break; }      /* keep the pair together: wait for row y+1 */
                else if (src[stride] <= 4) pf = pf__pair_for_bpp(f, src[stride], bpp);
            }
            if (pf) { pf(dst, src, prev, dst + stride, src + stride + 1, stride, bpp); prev = dst + stride; y += 2; continue; }
            pf__unfilter_row(f, dst, src, prev, stride, bpp);
            prev = dst; y++;
        }
        c->y = y; c->prev = prev;
    }
    /* swap16 runs one row behind: the row above is still the unfilter predecessor */
    if (c->x.xf != XF_NONE) xform_rows(c, final ? c->y : (c->x.xf == XF_SWAP16 ? (c->y ? c->y - 1 : 0) : c->y));
}
static void on_progress(void *ctx, const unsigned char *out_next) { pf_dec *c = ctx; process_rows(c, (size_t)(out_next - c->out), 0); }

static const uint8_t A7[7][4] = { {0,0,8,8}, {4,0,8,8}, {0,4,4,8}, {2,0,4,4}, {0,2,2,4}, {1,0,2,2}, {0,1,1,2} };  /* x0,y0,dx,dy */

void pf_decode_options_init(pf_decode_options *o) {
    memset(o, 0, sizeof *o);
    o->output = PF_OUTPUT_RAW; o->verify_crc = 1;
}

static const uint8_t PNG_SIG[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };

/* IHDR validation; returns file channels or a negative status */
static int check_ihdr(const uint8_t *ih, uint32_t *W, uint32_t *H, int *depth, int *color, int *interlace) {
    *W = be32(ih); *H = be32(ih + 4); *depth = ih[8]; *color = ih[9]; *interlace = ih[12];
    if (!*W || !*H || *W > 0x7FFFFFFFu || *H > 0x7FFFFFFFu || ih[10] || ih[11] || *interlace > 1) return PF_ERR_HEADER;
    switch (*color) {
    case 0: return (*depth == 1 || *depth == 2 || *depth == 4 || *depth == 8 || *depth == 16) ? 1 : PF_ERR_HEADER;
    case 3: return (*depth == 1 || *depth == 2 || *depth == 4 || *depth == 8) ? 1 : PF_ERR_HEADER;
    case 2: return (*depth == 8 || *depth == 16) ? 3 : PF_ERR_HEADER;
    case 4: return (*depth == 8 || *depth == 16) ? 2 : PF_ERR_HEADER;
    case 6: return (*depth == 8 || *depth == 16) ? 4 : PF_ERR_HEADER;
    default: return PF_ERR_HEADER;
    }
}

pf_status pf_read_info(const void *png, size_t n, pf_info *info) {
    const uint8_t *buf = png;
    if (!png || !info) return PF_ERR_ARGUMENT;
    memset(info, 0, sizeof *info);
    if (n < 8 + 25 || memcmp(buf, PNG_SIG, 8)) return PF_ERR_SIGNATURE;
    if (be32(buf + 8) != 13 || memcmp(buf + 12, "IHDR", 4)) return PF_ERR_HEADER;
    uint32_t W, H; int depth, color, il, ich = check_ihdr(buf + 16, &W, &H, &depth, &color, &il);
    if (ich < 0) return (pf_status)ich;
    info->width = W; info->height = H; info->bit_depth = (uint8_t)depth; info->color_type = (uint8_t)color;
    info->interlaced = (uint8_t)il; info->channels = (uint8_t)ich;
    for (const uint8_t *p = buf + 33, *end = buf + n; p + 12 <= end;) {
        uint32_t len = be32(p);
        if (len > (size_t)(end - p) - 12) break;
        if (!memcmp(p + 4, "PLTE", 4)) info->palette_entries = (uint16_t)(len / 3);
        else if (!memcmp(p + 4, "tRNS", 4)) info->trns_bytes = (uint16_t)(len > 0xFFFF ? 0xFFFF : len);
        else if (!memcmp(p + 4, "IDAT", 4) || !memcmp(p + 4, "IEND", 4)) break;
        p += 12 + len;
    }
    return PF_OK;
}

#define FAIL(code) do { st = (code); goto fail; } while (0)

pf_status pf_decode(const void *png, size_t n, const pf_decode_options *opts, pf_image *im) {
    const uint8_t *buf = png;
    pf_decode_options defo;
    if (!im) return PF_ERR_ARGUMENT;
    memset(im, 0, sizeof *im);
    if (!png) return PF_ERR_ARGUMENT;
    if (!opts) { pf_decode_options_init(&defo); opts = &defo; }
    if ((unsigned)opts->output > PF_OUTPUT_IMAGERS) return PF_ERR_ARGUMENT;
    const pf_allocator *A = opts->allocator;
    const uint32_t maxw = opts->max_width ? opts->max_width : (1u << 24), maxh = opts->max_height ? opts->max_height : (1u << 24);
    const uint64_t maxout = opts->max_output_bytes ? opts->max_output_bytes : (1ull << 30);

    if (n < 8 + 25 || memcmp(buf, PNG_SIG, 8)) return PF_ERR_SIGNATURE;
    if (be32(buf + 8) != 13 || memcmp(buf + 12, "IHDR", 4)) return PF_ERR_HEADER;
    uint32_t W, H; int depth, color, interlace;
    const int ich = check_ihdr(buf + 16, &W, &H, &depth, &color, &interlace);
    if (ich < 0) return (pf_status)ich;
    if (opts->verify_crc && libdeflate_crc32(0, buf + 12, 17) != be32(buf + 29)) return PF_ERR_CRC;
    if (W > maxw || H > maxh) return PF_ERR_LIMITS;
    const int bits = ich * depth, bpp = bits >= 8 ? bits / 8 : 1;
    const size_t stride = ((size_t)W * bits + 7) / 8;

    /* pass 1: chunk layout (rules as in the png crate: PLTE/tRNS before IDAT, no duplicates, no unknown critical
     * chunks); ancillary chunks (gAMA, sRGB, iCCP, text, APNG acTL/fcTL/fdAT, ...) are skipped. */
    const uint8_t *p = buf + 8, *end = buf + n, *z1 = NULL, *plte = NULL, *trns = NULL;
    size_t ztot = 0; uint32_t plen = 0, tlen = 0; int nidat = 0, idat_done = 0;
    while (p + 12 <= end) {
        uint32_t len = be32(p);
        if (len > (size_t)(end - p) - 12) return PF_ERR_CHUNK;
        const uint8_t *t = p + 4;
        if (!memcmp(t, "IDAT", 4)) {
            if (idat_done) return PF_ERR_CHUNK;          /* IDATs must be consecutive */
            if (!nidat) z1 = p + 8;
            nidat++; ztot += len;
        } else {
            if (nidat) idat_done = 1;
            if (!memcmp(t, "IEND", 4)) break;
            else if (!memcmp(t, "PLTE", 4)) { if (plte || nidat || len % 3 || len > 768 || !len) return PF_ERR_CHUNK; plte = p + 8; plen = len / 3; }
            else if (!memcmp(t, "tRNS", 4)) { if (trns || nidat) return PF_ERR_CHUNK; trns = p + 8; tlen = len; }
            else if (!memcmp(t, "IHDR", 4)) { if (p != buf + 8) return PF_ERR_CHUNK; }
            else if (!(t[0] & 0x20)) return PF_ERR_CHUNK;   /* unknown critical chunk */
        }
        p += 12 + len;
    }
    if (!nidat || ztot < 2) return PF_ERR_CHUNK;
    if (color == 3 && !plte) return PF_ERR_CHUNK;
    if (trns) {
        if (color == 4 || color == 6) return PF_ERR_CHUNK;
        if (color == 0 && tlen < 2) return PF_ERR_CHUNK;
        if (color == 2 && tlen < 6) return PF_ERR_CHUNK;
    }

    pf_status st = PF_OK;
    pf_dec c; memset(&c, 0, sizeof c);
    int och, odepth, ocolor;
    pf__xform_setup(&c.x, color, depth, ich, W, opts->output, plte, plen, trns, tlen, &och, &odepth, &ocolor);
    const size_t ostride = ((size_t)W * och * odepth + 7) / 8;
    c.ostride = ostride;
    /* raw (filtered) size */
    size_t rawn = 0, rs = stride + 1;
    if (!interlace) rawn = (size_t)H * rs;
    else for (int ps = 0; ps < 7; ps++) {
        if (W <= A7[ps][0] || H <= A7[ps][1]) continue;
        size_t pw = (W - A7[ps][0] + A7[ps][2] - 1) / A7[ps][2], ph = (H - A7[ps][1] + A7[ps][3] - 1) / A7[ps][3];
        rawn += ph * (1 + (pw * bits + 7) / 8);
    }
    const size_t off = interlace ? 0 : 4 * stride;   /* raw rows sit >= 4 output rows ahead (in-place 4-row kernel) */
    if ((uint64_t)ostride * H > maxout || (uint64_t)rawn + off > maxout + (maxout >> 2) + (1u << 20)) return PF_ERR_LIMITS;

    /* pass 2: CRCs + gather split IDATs */
    const uint8_t *z = z1; uint8_t *zbuf = NULL, *out = NULL, *obuf = NULL, *rb = NULL;
    struct libdeflate_decompressor *d = NULL;
    if (nidat > 1) { zbuf = pf__alloc(A, ztot); if (!zbuf) return PF_ERR_NOMEM; }
    if (opts->verify_crc || zbuf) {
        uint8_t *zo = zbuf;
        for (p = buf + 8; p + 12 <= end; p += 12 + be32(p)) {
            uint32_t len = be32(p); int idat = !memcmp(p + 4, "IDAT", 4);
            if (opts->verify_crc && libdeflate_crc32(0, p + 4, len + 4) != be32(p + 8 + len)) FAIL(PF_ERR_CRC);
            if (idat && zbuf) { memcpy(zo, p + 8, len); zo += len; }
            if (!memcmp(p + 4, "IEND", 4)) break;
        }
        if (zbuf) z = zbuf;
    }
    if ((z[0] & 15) != 8 || (z[0] >> 4) > 7 || (z[1] & 32) || ((z[0] << 8 | z[1]) % 31)) FAIL(PF_ERR_ZLIB);

    out = pf__alloc(A, off + rawn + 64);
    if (!out) FAIL(PF_ERR_NOMEM);
    const int sep = interlace || (c.x.xf != XF_NONE && c.x.xf != XF_SWAP16);   /* output != unfiltered rows */
    if (sep) {
        obuf = pf__alloc(A, ostride * H + 16);
        if (!obuf) FAIL(PF_ERR_NOMEM);
        if (interlace && odepth < 8) memset(obuf, 0, ostride * H + 16);
    }
    c.verify_adler = opts->verify_adler; c.adler = 1;
    c.out = out; c.off = off; c.stride = stride; c.rs = rs; c.H = H; c.bpp = bpp; c.dst = obuf;
    c.pair_ok = (bpp <= 4 || bpp == 6 || bpp == 8) && stride >= 16;
    c.quad_ok = c.pair_ok && bpp >= 3 && bpp <= 4 && pf__have_avx2();
    d = libdeflate_alloc_decompressor();
    if (!d) FAIL(PF_ERR_NOMEM);
    libdeflate_set_pair_literals(d, 1);
    size_t ain = 0, aout = 0; enum libdeflate_result r;
    if (interlace) r = libdeflate_deflate_decompress_ex(d, z + 2, ztot - 2, out + off, rawn, &ain, &aout);
    else r = libdeflate_deflate_decompress_progress(d, z + 2, ztot - 2, out + off, rawn, &ain, &aout, on_progress, &c, PROGRESS_STEP);
    libdeflate_free_decompressor(d); d = NULL;
    if (r != LIBDEFLATE_SUCCESS || aout != rawn) FAIL(PF_ERR_DEFLATE);
    if (opts->verify_adler && ain + 6 > ztot) FAIL(PF_ERR_ADLER);   /* no room for the Adler-32 trailer */
    if (!interlace) {
        process_rows(&c, off + rawn, 1);
        if (c.err) FAIL((pf_status)c.err);
        if (opts->verify_adler && c.adler != be32(z + 2 + ain)) FAIL(PF_ERR_ADLER);
    } else {
        if (opts->verify_adler && libdeflate_adler32(1, out, rawn) != be32(z + 2 + ain)) FAIL(PF_ERR_ADLER);
        /* Adam7: unfilter each pass row into a 2-row ping-pong buffer, transform to output pixels, scatter */
        const int obits = och * odepth, opx = obits >= 8 ? obits / 8 : 0;
        rb = malloc(2 * (stride + 32) + ostride + 32);
        if (!rb) FAIL(PF_ERR_NOMEM);
        uint8_t *rows[2] = { rb, rb + stride + 32 }, *trow = rb + 2 * (stride + 32);
        const uint8_t *q = out;
        for (int ps = 0; ps < 7; ps++) {
            uint32_t x0 = A7[ps][0], y0 = A7[ps][1], dx = A7[ps][2], dy = A7[ps][3];
            if (W <= x0 || H <= y0) continue;
            uint32_t pw = (W - x0 + dx - 1) / dx, ph = (H - y0 + dy - 1) / dy;
            size_t pst = ((size_t)pw * bits + 7) / 8;
            const uint8_t *prev = NULL;
            for (uint32_t j = 0; j < ph; j++, q += pst + 1) {
                int f = q[0]; if (f > 4) FAIL(PF_ERR_FILTER);
                uint8_t *cur = rows[j & 1];
                pf__unfilter_row(f, cur, q + 1, prev, pst, bpp);
                prev = cur;
                const uint8_t *px = cur;
                if (c.x.xf == XF_SWAP16) { memcpy(trow, cur, pst); pf__swap16(trow, pst); px = trow; }
                else if (c.x.xf != XF_NONE) { pf__xform_row(&c.x, cur, trow, pw); px = trow; }
                uint8_t *orow = obuf + (size_t)(y0 + j * dy) * ostride;
                if (opx) {
#define SC(N) for (uint32_t i = 0; i < pw; i++) memcpy(orow + (size_t)(x0 + i * dx) * N, px + (size_t)i * N, N); break;
                    switch (opx) { case 1: SC(1) case 2: SC(2) case 3: SC(3) case 4: SC(4) case 6: SC(6) case 8: SC(8) default: SC(opx) }
#undef SC
                } else {   /* raw sub-8-bit output: bit scatter (MSB first) */
                    for (uint32_t i = 0; i < pw; i++) {
                        uint32_t v = pf__unpack(px, i, obits), bit = (x0 + i * dx) * (uint32_t)obits;
                        orow[bit >> 3] |= (uint8_t)(v << (8 - obits - (bit & 7)));
                    }
                }
            }
        }
        free(rb); rb = NULL;
    }
    pf__free(A, zbuf);
    if (sep) { pf__free(A, out); im->pixels = obuf; }
    else im->pixels = pf__realloc_shrink(A, out, off + rawn + 64, (size_t)H * stride);
    im->width = W; im->height = H; im->channels = (uint8_t)och; im->bit_depth = (uint8_t)odepth; im->color_type = (uint8_t)ocolor;
    im->stride = ostride; im->size = (size_t)H * ostride;
    if (A) im->allocator = *A;
    return PF_OK;
fail:
    if (d) libdeflate_free_decompressor(d);
    free(rb); pf__free(A, zbuf); pf__free(A, out); pf__free(A, obuf);
    memset(im, 0, sizeof *im);
    return st;
}

void pf_image_free(pf_image *im) {
    if (!im) return;
    pf__free(im->allocator.alloc ? &im->allocator : NULL, im->pixels);
    memset(im, 0, sizeof *im);
}
