/* stream.c - incremental decoder (qpng_decoder) and encoder (qpng_encoder). See qpng.h "streaming".
 *
 * Decoder: a byte-level chunk state machine (works with any split of the input). Before the first IDAT the
 * header chunks are kept (IHDR, PLTE, tRNS); the mode is then fixed:
 *  - STREAM: row callback, non-interlaced, zlib-ng available: IDAT payload -> zlib header (2 bytes) -> raw zlib-ng
 *    inflate into a one-row buffer -> unfilter against the previous row -> output transform -> callback.
 *  - BUFFER: everything else: the whole input is kept and decoded by qpng_decode at finish (fast one-shot path).
 * Encoder: rows are copied behind the previous row, filtered in batches of up to ~256 KiB and handed to a
 * qpng_zstream (png-ultra or zlib-ng); compressed bytes are collected into IDAT chunks of <= 256 KiB.
 */
#include "deflate_backend.h"
#include "metadata.h"

/* ================================================= decoder ================================================= */
enum { D_SIG, D_LEN, D_BODY, D_CRC, D_END, D_ERR };

struct qpng_decoder {
    qpng_decode_options o;
    qpng_allocator a; int has_a;
    qpng_row_fn row_cb; void *user;
    int state; qpng_status err;
    uint8_t fix[8]; size_t nfix;              /* signature / length+type / CRC being assembled */
    uint32_t clen, cpos, crc; uint8_t ctype[4];
    uint8_t body[768]; uint32_t nbody;        /* IHDR / PLTE / tRNS payload being assembled */
    /* header */
    int have_info; qpng_info info; int ich, bpp;
    uint8_t plte[768]; uint32_t plen; uint8_t trns[256]; uint32_t tlen; int has_plte, has_trns;
    int seen_idat, idat_done;
    /* BUFFER mode: the whole input */
    int buffered; uint8_t *all; size_t nall, call;
    /* STREAM mode */
    qpng_zistream *zi; int zhdr; uint8_t zh[2]; int zend; uint8_t ztrail[4]; int nztrail;
    uint8_t *prev, *cur, *orow; size_t stride, rs, ostride, rpos; uint32_t y;
    uint32_t adler; qpng_xform x;
};

static const qpng_allocator *dA(const qpng_decoder *d) { return d->has_a ? &d->a : NULL; }

qpng_status qpng_decoder_new(const qpng_decode_options *opts, qpng_row_fn row_cb, void *user, qpng_decoder **out) {
    if (!out) return QPNG_ERR_ARGUMENT;
    *out = NULL;
    qpng_decode_options defo;
    if (!opts) { qpng_decode_options_init(&defo); opts = &defo; }
    if ((unsigned)opts->output > QPNG_OUTPUT_IMAGERS) return QPNG_ERR_ARGUMENT;
    qpng_decoder *d = qpng__alloc(opts->allocator, sizeof *d);
    if (!d) return QPNG_ERR_NOMEM;
    memset(d, 0, sizeof *d);
    d->o = *opts; if (opts->allocator) { d->a = *opts->allocator; d->has_a = 1; d->o.allocator = &d->a; }
    d->row_cb = row_cb; d->user = user; d->adler = 1;
    *out = d;
    return QPNG_OK;
}

void qpng_decoder_free(qpng_decoder *d) {
    if (!d) return;
    const qpng_allocator *A = dA(d); qpng_allocator ac = d->a; if (A) A = &ac;
    qpng__zistream_free(d->zi);
    qpng__free(A, d->all); qpng__free(A, d->prev); qpng__free(A, d->cur); qpng__free(A, d->orow);
    qpng__free(A, d);
}

qpng_status qpng_decoder_info(const qpng_decoder *d, qpng_info *info) {
    if (!d || !info) return QPNG_ERR_ARGUMENT;
    if (d->state == D_ERR) return d->err;
    if (!d->have_info) return QPNG_MORE_DATA;
    *info = d->info;
    return QPNG_OK;
}

static qpng_status d_fail(qpng_decoder *d, qpng_status s) { d->state = D_ERR; d->err = s; return s; }

static int keep_all(qpng_decoder *d, const uint8_t *p, size_t n) {
    if (n > d->call - d->nall) {
        size_t nc = d->call ? d->call : 65536;
        while (nc - d->nall < n) nc *= 2;
        uint8_t *nb = qpng__alloc(dA(d), nc);
        if (!nb) return 0;
        if (d->nall) memcpy(nb, d->all, d->nall);
        qpng__free(dA(d), d->all); d->all = nb; d->call = nc;
    }
    memcpy(d->all + d->nall, p, n); d->nall += n;
    return 1;
}

/* first IDAT: validate the header chunks, choose the mode, set up STREAM state */
static qpng_status d_begin_image(qpng_decoder *d) {
    const qpng_info *in = &d->info;
    if (in->color_type == 3 && !d->has_plte) return QPNG_ERR_CHUNK;
    if (d->has_trns) {
        if (in->color_type == 4 || in->color_type == 6) return QPNG_ERR_CHUNK;
        if (in->color_type == 0 && d->tlen < 2) return QPNG_ERR_CHUNK;
        if (in->color_type == 2 && d->tlen < 6) return QPNG_ERR_CHUNK;
    }
    const int bits = d->ich * in->bit_depth;
    d->bpp = bits >= 8 ? bits / 8 : 1;
    d->stride = ((size_t)in->width * bits + 7) / 8; d->rs = d->stride + 1;
    int och, odepth, ocolor;
    qpng__xform_setup(&d->x, in->color_type, in->bit_depth, d->ich, in->width, d->o.output, d->has_plte ? d->plte : NULL,
                    d->plen, d->has_trns ? d->trns : NULL, d->tlen, &och, &odepth, &ocolor);
    d->ostride = ((size_t)in->width * och * odepth + 7) / 8;
    const uint64_t maxout = d->o.max_output_bytes ? d->o.max_output_bytes : (1ull << 30);
    if (!d->row_cb && (uint64_t)d->ostride * in->height > maxout) return QPNG_ERR_LIMITS;
    if (d->buffered) return QPNG_OK;
    d->zi = qpng__zistream_new(dA(d));
    if (!d->zi) return QPNG_ERR_NOMEM;
    d->prev = qpng__alloc(dA(d), d->stride + 64); d->cur = qpng__alloc(dA(d), d->rs + 64);
    d->orow = qpng__alloc(dA(d), d->ostride + d->stride + 64);
    if (!d->prev || !d->cur || !d->orow) return QPNG_ERR_NOMEM;
    return QPNG_OK;
}

/* STREAM mode: one full filtered row in d->cur */
static qpng_status d_row(qpng_decoder *d) {
    const int f = d->cur[0];
    if (f > 4) return QPNG_ERR_FILTER;
    if (d->o.verify_adler) d->adler = libdeflate_adler32(d->adler, d->cur, d->rs);
    uint8_t *dst = d->orow + d->ostride + 32;   /* unfiltered row, then transformed into orow */
    qpng__unfilter_row(f, dst, d->cur + 1, d->y ? d->prev : NULL, d->stride, d->bpp);
    memcpy(d->prev, dst, d->stride);
    const uint8_t *row = dst;
    if (d->x.xf == XF_SWAP16) { memcpy(d->orow, dst, d->stride); qpng__swap16(d->orow, d->stride); row = d->orow; }
    else if (d->x.xf != XF_NONE) { qpng__xform_row(&d->x, dst, d->orow, d->info.width); row = d->orow; }
    if (d->row_cb(d->user, d->y, row, d->ostride)) return QPNG_ERR_ABORTED;
    d->y++;
    return QPNG_OK;
}

/* STREAM mode: IDAT payload bytes */
static qpng_status d_idat(qpng_decoder *d, const uint8_t *p, size_t n) {
    while (n) {
        if (d->zhdr < 2) {
            d->zh[d->zhdr++] = *p++; n--;
            if (d->zhdr == 2) {
                const uint8_t *z = d->zh;
                if ((z[0] & 15) != 8 || (z[0] >> 4) > 7 || (z[1] & 32) || ((z[0] << 8 | z[1]) % 31)) return QPNG_ERR_ZLIB;
            }
            continue;
        }
        if (d->zend) {   /* Adler-32 trailer (and anything after it, ignored as in qpng_decode) */
            while (n && d->nztrail < 4) { d->ztrail[d->nztrail++] = *p++; n--; }
            return QPNG_OK;
        }
        size_t used, made;
        int r = qpng__zistream_run(d->zi, p, n, &used, d->cur + d->rpos, d->rs - d->rpos, &made);
        if (r < 0) return QPNG_ERR_DEFLATE;
        p += used; n -= used; d->rpos += made;
        if (d->rpos == d->rs) {
            if (d->y >= d->info.height) return QPNG_ERR_DEFLATE;   /* more data than the image */
            qpng_status s = d_row(d); if (s) return s;
            d->rpos = 0;
        }
        if (r == 1) d->zend = 1;
        else if (!used && !made && d->rpos != d->rs) break;   /* needs more input */
    }
    return QPNG_OK;
}

/* a complete non-IDAT chunk (header ones kept in d->body) */
static qpng_status d_chunk_done(qpng_decoder *d) {
    const uint8_t *t = d->ctype;
    if (!memcmp(t, "IHDR", 4)) {
        if (d->have_info || d->clen != 13) return QPNG_ERR_HEADER;
        uint32_t W, H; int depth, color, il;
        int ich = qpng__check_ihdr(d->body, &W, &H, &depth, &color, &il);
        if (ich < 0) return (qpng_status)ich;
        const uint32_t maxw = d->o.max_width ? d->o.max_width : (1u << 24), maxh = d->o.max_height ? d->o.max_height : (1u << 24);
        if (W > maxw || H > maxh) return QPNG_ERR_LIMITS;
        d->info.width = W; d->info.height = H; d->info.bit_depth = (uint8_t)depth; d->info.color_type = (uint8_t)color;
        d->info.interlaced = (uint8_t)il; d->info.channels = (uint8_t)ich; d->ich = ich; d->have_info = 1;
        d->buffered = !d->row_cb || il || !qpng_backend_available(QPNG_BACKEND_ZLIBNG);
    } else if (!d->have_info) {
        return QPNG_ERR_HEADER;
    } else if (!memcmp(t, "IEND", 4)) {
        if (!d->seen_idat) return QPNG_ERR_CHUNK;
        d->state = D_END;
    } else if (!memcmp(t, "PLTE", 4)) {
        if (d->has_plte || d->seen_idat || d->clen % 3 || d->clen > 768 || !d->clen) return QPNG_ERR_CHUNK;
        memcpy(d->plte, d->body, d->clen); d->plen = d->clen / 3; d->has_plte = 1; d->info.palette_entries = (uint16_t)d->plen;
    } else if (!memcmp(t, "tRNS", 4)) {
        if (d->has_trns || d->seen_idat) return QPNG_ERR_CHUNK;
        d->tlen = d->clen < 256 ? d->clen : 256; memcpy(d->trns, d->body, d->tlen); d->has_trns = 1;
        d->info.trns_bytes = (uint16_t)d->clen;
    } else if (!(t[0] & 0x20)) {
        return QPNG_ERR_CHUNK;   /* unknown critical chunk */
    }
    return QPNG_OK;
}

qpng_status qpng_decoder_push(qpng_decoder *d, const void *data, size_t n) {
    if (!d || (!data && n)) return QPNG_ERR_ARGUMENT;
    if (d->state == D_ERR) return d->err;
    const uint8_t *p = data;
    if (d->state != D_END && (!d->have_info || d->buffered) && !keep_all(d, p, n)) return d_fail(d, QPNG_ERR_NOMEM);
    while (n && d->state != D_END) {
        if (d->state == D_SIG) {
            static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
            while (n && d->nfix < 8) { if (*p != sig[d->nfix]) return d_fail(d, QPNG_ERR_SIGNATURE); d->nfix++; p++; n--; }
            if (d->nfix == 8) { d->state = D_LEN; d->nfix = 0; }
        } else if (d->state == D_LEN) {
            while (n && d->nfix < 8) { d->fix[d->nfix++] = *p++; n--; }
            if (d->nfix < 8) break;
            d->nfix = 0; d->clen = be32(d->fix); d->cpos = 0; memcpy(d->ctype, d->fix + 4, 4);
            if (d->clen > 0x7FFFFFFFu) return d_fail(d, QPNG_ERR_CHUNK);
            if (!d->have_info && memcmp(d->ctype, "IHDR", 4)) return d_fail(d, QPNG_ERR_HEADER);
            d->crc = libdeflate_crc32(0, d->ctype, 4);
            const int idat = !memcmp(d->ctype, "IDAT", 4);
            if (idat) {
                if (d->idat_done) return d_fail(d, QPNG_ERR_CHUNK);
                if (!d->seen_idat) { qpng_status s = d_begin_image(d); if (s) return d_fail(d, s); d->seen_idat = 1; }
            } else if (d->seen_idat) d->idat_done = 1;
            d->nbody = 0;
            d->state = d->clen ? D_BODY : D_CRC;
        } else if (d->state == D_BODY) {
            size_t k = d->clen - d->cpos < n ? d->clen - d->cpos : n;
            if (d->o.verify_crc) d->crc = libdeflate_crc32(d->crc, p, k);
            if (!memcmp(d->ctype, "IDAT", 4)) {
                if (!d->buffered) { qpng_status s = d_idat(d, p, k); if (s) return d_fail(d, s); }
            } else if (!memcmp(d->ctype, "IHDR", 4) || !memcmp(d->ctype, "PLTE", 4) || !memcmp(d->ctype, "tRNS", 4)) {
                size_t room = sizeof d->body - d->nbody, c = k < room ? k : room;
                memcpy(d->body + d->nbody, p, c); d->nbody += (uint32_t)c;
            }
            d->cpos += (uint32_t)k; p += k; n -= k;
            if (d->cpos == d->clen) d->state = D_CRC;
        } else if (d->state == D_CRC) {
            while (n && d->nfix < 4) { d->fix[d->nfix++] = *p++; n--; }
            if (d->nfix < 4) break;
            d->nfix = 0;
            if (d->o.verify_crc && d->crc != be32(d->fix)) return d_fail(d, QPNG_ERR_CRC);
            d->state = D_LEN;
            if (memcmp(d->ctype, "IDAT", 4)) { qpng_status s = d_chunk_done(d); if (s) return d_fail(d, s); }
            if (d->have_info && !d->buffered && d->all) { qpng__free(dA(d), d->all); d->all = NULL; d->nall = d->call = 0; }
        }
    }
    return d->state == D_END ? QPNG_OK : QPNG_MORE_DATA;
}

qpng_status qpng_decoder_finish(qpng_decoder *d, qpng_image *img) {
    if (img) memset(img, 0, sizeof *img);
    if (!d) return QPNG_ERR_ARGUMENT;
    if (d->state == D_ERR) return d->err;
    if (d->state != D_END) return QPNG_ERR_STATE;
    if (!d->buffered) {
        if (d->y != d->info.height || !d->zend) return d_fail(d, QPNG_ERR_DEFLATE);
        if (d->o.verify_adler && (d->nztrail < 4 || be32(d->ztrail) != d->adler)) return d_fail(d, QPNG_ERR_ADLER);
        return QPNG_OK;
    }
    qpng_image im;
    qpng_status s = qpng_decode(d->all, d->nall, &d->o, &im);
    if (s) return d_fail(d, s);
    qpng__free(dA(d), d->all); d->all = NULL; d->nall = d->call = 0;
    if (d->row_cb) {
        for (uint32_t y = 0; y < im.height; y++)
            if (d->row_cb(d->user, y, im.pixels + (size_t)y * im.stride, im.stride)) { s = QPNG_ERR_ABORTED; break; }
        qpng_image_free(&im);
        return s ? d_fail(d, s) : QPNG_OK;
    }
    if (img) *img = im; else qpng_image_free(&im);
    return QPNG_OK;
}

qpng_status qpng_decode_stream(qpng_read_fn rd, void *ru, const qpng_decode_options *opts, qpng_row_fn row_cb, void *row_user,
                           qpng_image *img) {
    if (img) memset(img, 0, sizeof *img);
    if (!rd || (!row_cb && !img)) return QPNG_ERR_ARGUMENT;
    qpng_decoder *d;
    qpng_status s = qpng_decoder_new(opts, row_cb, row_user, &d);
    if (s) return s;
    uint8_t *buf = qpng__alloc(opts ? opts->allocator : NULL, 65536);
    if (!buf) { qpng_decoder_free(d); return QPNG_ERR_NOMEM; }
    s = QPNG_MORE_DATA;
    while (s == QPNG_MORE_DATA) {
        size_t k = rd(ru, buf, 65536);
        if (k == (size_t)-1) { s = QPNG_ERR_ABORTED; break; }
        if (!k) { s = QPNG_ERR_DEFLATE; break; }   /* truncated: no IEND */
        s = qpng_decoder_push(d, buf, k);
    }
    qpng__free(opts ? opts->allocator : NULL, buf);
    if (s == QPNG_OK) s = qpng_decoder_finish(d, img);
    qpng_decoder_free(d);
    return s;
}

/* ================================================= encoder ================================================= */
#define E_IDAT_MAX 262144
#define E_BATCH    65536

struct qpng_encoder {
    const qpng_allocator *A; qpng_allocator a;
    qpng_write_fn write; void *user;
    uint32_t W, H, y; int bpp, filter, hybrid, ultra;
    size_t n;                     /* bytes per row */
    qpng_zstream *z;
    uint8_t *rows; uint32_t nrows, max_rows;   /* rows[0] = previous row, then pending source rows */
    uint8_t *filt;                /* filtered batch (1 byte lookback in front) */
    uint8_t *idat; size_t nidat;  /* IDAT payload being collected */
    uint8_t *tail; size_t ntail;  /* metadata after IDAT */
    qpng_status err; int finished, big;
    size_t sunk;                  /* compressed bytes produced (hybrid filter decision) */
};

static qpng_status e_out(qpng_encoder *e, const uint8_t *p, size_t n) {
    return n && e->write(e->user, p, n) ? QPNG_ERR_ABORTED : QPNG_OK;
}
static qpng_status e_chunk(qpng_encoder *e, const char *type, const uint8_t *p, size_t n) {
    uint8_t h[8], c[4];
    wbe32(h, (uint32_t)n); memcpy(h + 4, type, 4);
    uint32_t crc = libdeflate_crc32(0, h + 4, 4);
    if (n) crc = libdeflate_crc32(crc, p, n);   /* libdeflate_crc32(x, NULL, 0) returns 0, not x */
    wbe32(c, crc);
    qpng_status s = e_out(e, h, 8);
    if (!s) s = e_out(e, p, n);
    if (!s) s = e_out(e, c, 4);
    return s;
}
static qpng_status e_flush_idat(qpng_encoder *e) {
    qpng_status s = e->nidat ? e_chunk(e, "IDAT", e->idat, e->nidat) : QPNG_OK;
    e->nidat = 0;
    return s;
}
/* qpng_zsink: compressed bytes -> IDAT chunks */
static qpng_status e_sink(void *ctx, const uint8_t *p, size_t n) {
    qpng_encoder *e = ctx;
    e->sunk += n;
    while (n) {
        size_t k = E_IDAT_MAX - e->nidat < n ? E_IDAT_MAX - e->nidat : n;
        memcpy(e->idat + e->nidat, p, k); e->nidat += k; p += k; n -= k;
        if (e->nidat == E_IDAT_MAX) { qpng_status s = e_flush_idat(e); if (s) return s; }
    }
    return QPNG_OK;
}

qpng_status qpng_encoder_new(uint32_t w, uint32_t h, qpng_color color, int depth, const qpng_encode_options *opts,
                         qpng_write_fn write, void *wu, qpng_encoder **out) {
    if (!out) return QPNG_ERR_ARGUMENT;
    *out = NULL;
    qpng_encode_options defo;
    if (!opts) { qpng_encode_options_init(&defo); opts = &defo; }
    if (!write) return QPNG_ERR_ARGUMENT;
    /* argument validation: run qpng_encode's checks on a 1-row dummy is not possible without pixels; replicate */
    int ch;
    switch (color) {
    case QPNG_COLOR_GRAY: ch = 1; if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) return QPNG_ERR_ARGUMENT; break;
    case QPNG_COLOR_PALETTE: ch = 1; if (depth != 1 && depth != 2 && depth != 4 && depth != 8) return QPNG_ERR_ARGUMENT; break;
    case QPNG_COLOR_RGB: ch = 3; if (depth != 8 && depth != 16) return QPNG_ERR_ARGUMENT; break;
    case QPNG_COLOR_GRAY_ALPHA: ch = 2; if (depth != 8 && depth != 16) return QPNG_ERR_ARGUMENT; break;
    case QPNG_COLOR_RGBA: ch = 4; if (depth != 8 && depth != 16) return QPNG_ERR_ARGUMENT; break;
    default: return QPNG_ERR_ARGUMENT;
    }
    if (!w || !h || w > 0x7FFFFFFFu || h > 0x7FFFFFFFu) return QPNG_ERR_ARGUMENT;
    if (color == QPNG_COLOR_PALETTE && (!opts->palette || opts->palette_entries < 1 || opts->palette_entries > 256)) return QPNG_ERR_ARGUMENT;
    if (opts->palette && (opts->palette_entries < 1 || opts->palette_entries > 256)) return QPNG_ERR_ARGUMENT;
    if (opts->trns && (opts->trns_bytes < 1 || color == QPNG_COLOR_GRAY_ALPHA || color == QPNG_COLOR_RGBA ||
                       (color == QPNG_COLOR_GRAY && opts->trns_bytes != 2) || (color == QPNG_COLOR_RGB && opts->trns_bytes != 6) ||
                       (color == QPNG_COLOR_PALETTE && opts->trns_bytes > opts->palette_entries)))
        return QPNG_ERR_ARGUMENT;
    if (opts->metadata) { qpng_status ms = qpng__meta_validate(opts->metadata, color); if (ms) return ms; }
    if (opts->backend < QPNG_BACKEND_AUTO || opts->backend > QPNG_BACKEND_ZLIBNG || (unsigned)opts->preset > QPNG_PRESET_BEST ||
        opts->filter < -1 || opts->filter > QPNG_FILTER_ADAPTIVE || opts->zlib_strategy < 0 || opts->zlib_strategy > QPNG_ZSTRATEGY_RLE)
        return QPNG_ERR_ARGUMENT;
    if (!qpng_backend_available((qpng_backend)opts->backend)) return QPNG_ERR_UNSUPPORTED;
    const int bits = ch * depth;
    const size_t n = ((size_t)w * bits + 7) / 8;
    if (n > (64u << 20)) return QPNG_ERR_LIMITS;

    /* stream configuration */
    const int zng_ok = qpng_backend_available(QPNG_BACKEND_ZLIBNG);
    const int fast = opts->preset <= QPNG_PRESET_FAST;
    int use_zng = opts->backend == QPNG_BACKEND_ZLIBNG || (opts->backend == QPNG_BACKEND_AUTO && !fast && zng_ok);
    int level = opts->deflate_level;
    if (level && !use_zng && level != QPNG_DEFLATE_PNGULTRA) return QPNG_ERR_UNSUPPORTED;   /* libdeflate levels: not streamable */
    const qpng_zcfg zc = use_zng ? qpng__zcfg(QPNG_BACKEND_ZLIBNG, level ? level : (int[]){ 1, 2, 6, 9 }[opts->preset],
                                          opts->search_depth, opts->nice_length, 0, opts->zlib_strategy)
                               : qpng__zcfg(QPNG_BACKEND_LIBDEFLATE, QPNG_DEFLATE_PNGULTRA, 0, 0, 0, 0);
    if (!qpng__zcfg_valid(&zc)) return QPNG_ERR_ARGUMENT;

    const qpng_allocator *A = opts->allocator;
    qpng_encoder *e = qpng__alloc(A, sizeof *e);
    if (!e) return QPNG_ERR_NOMEM;
    memset(e, 0, sizeof *e);
    if (A) { e->a = *A; e->A = &e->a; }
    e->write = write; e->user = wu; e->W = w; e->H = h; e->n = n;
    e->bpp = bits >= 8 ? bits / 8 : 1;
    e->filter = opts->filter >= 0 ? opts->filter : opts->preset == QPNG_PRESET_FASTEST ? QPNG_FILTER_UP : QPNG_FILTER_ADAPTIVE;
    e->hybrid = opts->filter < 0 && !use_zng && opts->preset != QPNG_PRESET_FASTEST;   /* as one-shot FAST */
    e->ultra = !use_zng;
    e->max_rows = (uint32_t)(E_BATCH / (n + 1) + 1);
    if (e->ultra && (size_t)e->max_rows * (n + 1) > QPNG_ZSTREAM_MAX_CHUNK) e->max_rows = (uint32_t)(QPNG_ZSTREAM_MAX_CHUNK / (n + 1));
    qpng_status s = QPNG_OK;
    if (e->ultra && (n + 1) > QPNG_ZSTREAM_MAX_CHUNK) s = QPNG_ERR_UNSUPPORTED;   /* > 256 KiB rows: use qpng_encode */
    if (!s) {
        e->rows = qpng__alloc(e->A, (size_t)(e->max_rows + 1) * n + 64);
        e->filt = qpng__alloc(e->A, (size_t)e->max_rows * (n + 1) + 64);
        e->idat = qpng__alloc(e->A, E_IDAT_MAX);
        if (!e->rows || !e->filt || !e->idat) s = QPNG_ERR_NOMEM;
    }
    if (!s) { memset(e->rows, 0, n); memset(e->filt, 0, 1); e->z = qpng__zstream_new(&zc, e->A, e_sink, e, &s); }
    /* signature, IHDR, metadata (before PLTE), PLTE, tRNS, metadata (before IDAT) */
    uint8_t *m0 = NULL, *m1 = NULL; size_t n0 = 0, n1 = 0;
    if (!s && opts->metadata) {
        s = qpng__meta_serialize(opts->metadata, 0, e->A, &m0, &n0);
        if (!s) s = qpng__meta_serialize(opts->metadata, 1, e->A, &m1, &n1);
        if (!s) s = qpng__meta_serialize(opts->metadata, 2, e->A, &e->tail, &e->ntail);
    }
    if (!s) {
        static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
        uint8_t ihdr[13];
        wbe32(ihdr, w); wbe32(ihdr + 4, h); ihdr[8] = (uint8_t)depth; ihdr[9] = (uint8_t)color; ihdr[10] = ihdr[11] = ihdr[12] = 0;
        s = e_out(e, sig, 8);
        if (!s) s = e_chunk(e, "IHDR", ihdr, 13);
        if (!s) s = e_out(e, m0, n0);
        if (!s && opts->palette) s = e_chunk(e, "PLTE", opts->palette, 3u * (size_t)opts->palette_entries);
        if (!s && opts->trns) s = e_chunk(e, "tRNS", opts->trns, (size_t)opts->trns_bytes);
        if (!s) s = e_out(e, m1, n1);
    }
    qpng__free(e->A, m0); qpng__free(e->A, m1);
    if (s) { qpng_encoder_free(e); return s; }
    *out = e;
    return QPNG_OK;
}

/* filter + compress the pending rows (rows[1..nrows], rows[0] = predecessor) */
static qpng_status e_compress(qpng_encoder *e, int final) {
    const size_t len = (size_t)e->nrows * (e->n + 1);
    const int f = e->hybrid && e->big ? QPNG_FILTER_UP : e->filter;
    if (e->nrows) qpng__filter_rows(e->filt + 1, e->rows, e->n, 1, e->nrows + 1, e->n, e->bpp, f);
    const size_t before = e->sunk;
    qpng_status s = qpng__zstream_write(e->z, e->filt + 1, len, final);
    if (s) return s;
    e->big = (e->sunk - before) * 4 < len;   /* >= 4:1: graphics-like, hybrid switches to Up (as one-shot FAST) */
    e->filt[0] = e->filt[len];   /* png-ultra looks one byte back */
    if (e->nrows) memcpy(e->rows, e->rows + (size_t)e->nrows * e->n, e->n);
    e->nrows = 0;
    return QPNG_OK;
}

qpng_status qpng_encoder_push_rows(qpng_encoder *e, const void *rows, size_t stride, uint32_t k) {
    if (!e || (!rows && k)) return QPNG_ERR_ARGUMENT;
    if (e->err) return e->err;
    if (e->finished || k > e->H - e->y) return QPNG_ERR_STATE;
    if (!stride) stride = e->n;
    if (stride < e->n) return QPNG_ERR_ARGUMENT;
    const uint8_t *src = rows;
    for (uint32_t i = 0; i < k; i++) {
        if (e->nrows == e->max_rows) {
            qpng_status s = e_compress(e, 0);
            if (s) return e->err = s;
        }
        memcpy(e->rows + (size_t)(e->nrows + 1) * e->n, src + (size_t)i * stride, e->n);
        e->nrows++; e->y++;
    }
    return QPNG_OK;
}

qpng_status qpng_encoder_finish(qpng_encoder *e) {
    if (!e) return QPNG_ERR_ARGUMENT;
    if (e->err) return e->err;
    if (e->finished || e->y != e->H) return QPNG_ERR_STATE;
    qpng_status s = e_compress(e, 1);
    if (!s) s = e_flush_idat(e);
    if (!s) s = e_out(e, e->tail, e->ntail);
    if (!s) s = e_chunk(e, "IEND", NULL, 0);
    e->finished = 1;
    return s ? (e->err = s) : QPNG_OK;
}

void qpng_encoder_free(qpng_encoder *e) {
    if (!e) return;
    qpng_allocator ac = e->a; const qpng_allocator *A = e->A ? &ac : NULL;
    qpng__zstream_free(e->z);
    qpng__free(A, e->rows); qpng__free(A, e->filt); qpng__free(A, e->idat); qpng__free(A, e->tail);
    qpng__free(A, e);
}
