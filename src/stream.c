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
    qpng_allocator a; bool has_a;
    qpng_row_fn row_cb; void *user;
    int state; qpng_status err;
    uint8_t fix[8]; size_t nfix;              /* signature / length+type / CRC being assembled */
    uint32_t clen, cpos, crc; uint8_t ctype[4];
    uint8_t body[768]; uint32_t nbody;        /* IHDR / PLTE / tRNS payload being assembled */
    /* header */
    bool have_info; qpng_info info; int bpp;
    uint8_t plte[768]; uint32_t plen; uint8_t trns[256]; uint32_t tlen; bool has_plte, has_trns;
    bool seen_idat, idat_done;
    /* BUFFER mode: the whole input */
    bool buffered; uint8_t *all; size_t nall, call;
    /* STREAM mode */
    qpng_zistream *zi; int zhdr; uint8_t zh[2]; bool zend; uint8_t ztrail[4]; int nztrail;
    uint8_t *prev, *cur, *orow; size_t stride, rs, ostride, rpos; uint32_t y;
    uint32_t adler; qpng_xform x;
};

static const qpng_allocator *dA(const qpng_decoder d[static 1]) { return d->has_a ? &d->a : nullptr; }
qpng_status qpng_decoder_new(const qpng_decode_options *opts, qpng_row_fn row_cb, void *user, qpng_decoder **out) {
    if (!out) return QPNG_ERR_ARGUMENT;
    *out = nullptr;
    qpng_decode_options defo;
    if (!opts) { qpng_decode_options_init(&defo); opts = &defo; }
    if ((unsigned)opts->output > QPNG_OUTPUT_IMAGERS) return QPNG_ERR_ARGUMENT;
    qpng_owned od = { opts->allocator, qpng__alloc(opts->allocator, sizeof(qpng_decoder)) };
    if (!od.p) return QPNG_ERR_NOMEM;
    qpng_decoder *d = od.p;
    *d = (qpng_decoder){};
    d->o = *opts;
    if (opts->allocator) { d->a = *opts->allocator; d->has_a = true; d->o.allocator = &d->a; }
    d->row_cb = row_cb; d->user = user; d->adler = 1;
    *out = qpng__take(&od);
    return QPNG_OK;
}

void qpng_decoder_free(qpng_decoder *d) {
    if (!d) return;
    qpng_allocator ac = d->a;
    const qpng_allocator *A = d->has_a ? &ac : nullptr;
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

static qpng_status d_fail(qpng_decoder d[static 1], qpng_status s) { d->state = D_ERR; d->err = s; return s; }

[[nodiscard]] static bool keep_all(qpng_decoder d[static 1], size_t n, const uint8_t p[n]) {
    if (!n) return true;
    size_t need;
    uint8_t *nb = qpng__add_size(&need, d->nall, n) ? nullptr : qpng__grow(dA(d), d->all, d->nall, &d->call, need, 1, 65536);
    if (!nb) return false;
    d->all = nb;
    memcpy(d->all + d->nall, p, n); d->nall += n;
    return true;
}

/* first IDAT: validate the header chunks, choose the mode, set up STREAM state */
static qpng_status d_begin_image(qpng_decoder d[static 1]) {
    const qpng_info *in = &d->info;
    if (!qpng__plte_trns_ok(in->color_type, d->has_plte, d->has_trns, d->tlen)) return QPNG_ERR_CHUNK;
    const int bits = in->channels * in->bit_depth;
    d->bpp = bits >= 8 ? bits / 8 : 1;
    if (qpng__row_bytes(&d->stride, in->width, bits) || qpng__add_size(&d->rs, d->stride, 1)) return QPNG_ERR_LIMITS;
    int och, odepth, ocolor;
    qpng__xform_setup(&d->x, in->color_type, in->bit_depth, in->channels, in->width, d->o.output,
                      d->plen, d->has_plte ? d->plte : nullptr, d->tlen, d->has_trns ? d->trns : nullptr,
                      &och, &odepth, &ocolor);
    if (qpng__row_bytes(&d->ostride, in->width, och * odepth)) return QPNG_ERR_LIMITS;
    const uint64_t maxout = d->o.max_output_bytes ? d->o.max_output_bytes : (1ull << 30);
    uint64_t out_bytes;
    if (!d->row_cb && (ckd_mul(&out_bytes, d->ostride, in->height) || out_bytes > maxout)) return QPNG_ERR_LIMITS;
    if (d->buffered) return QPNG_OK;
    const qpng_allocator *A = dA(d);
    d->zi = qpng__zistream_new(A);
    if (!d->zi) return QPNG_ERR_NOMEM;
    qpng_owned prev = { A, qpng__alloc(A, d->stride + 64) };
    qpng_owned cur = { A, qpng__alloc(A, d->rs + 64) };
    qpng_owned orow = { A, qpng__alloc(A, d->ostride + d->stride + 64) };
    if (!prev.p || !cur.p || !orow.p) return QPNG_ERR_NOMEM;
    d->prev = qpng__take(&prev);
    d->cur = qpng__take(&cur);
    d->orow = qpng__take(&orow);
    return QPNG_OK;
}

/* STREAM mode: one full filtered row in d->cur */
static qpng_status d_row(qpng_decoder d[static 1]) {
    const int f = d->cur[0];
    if (f > 4) return QPNG_ERR_FILTER;
    if (d->o.verify_adler) d->adler = libdeflate_adler32(d->adler, d->cur, d->rs);
    uint8_t *dst = d->orow + d->ostride + 32;   /* unfiltered row, then transformed into orow */
    qpng__unfilter_row(f, d->stride, d->bpp, dst, d->cur + 1, d->y ? d->prev : nullptr);
    memcpy(d->prev, dst, d->stride);
    const uint8_t *row = dst;
    if (d->x.xf == XF_SWAP16) { memcpy(d->orow, dst, d->stride); qpng__swap16(d->stride, d->orow); row = d->orow; }
    else if (d->x.xf != XF_NONE) { qpng__xform_row(&d->x, d->info.width, dst, d->orow); row = d->orow; }
    if (d->row_cb(d->user, d->y, d->ostride, row)) return QPNG_ERR_ABORTED;
    d->y++;
    return QPNG_OK;
}

/* STREAM mode: IDAT payload bytes */
static qpng_status d_idat(qpng_decoder d[static 1], size_t n, const uint8_t p[static n]) {
    while (n) {
        if (d->zhdr < 2) {
            d->zh[d->zhdr++] = *p++; n--;
            if (d->zhdr == 2 && !qpng__zlib_hdr_ok(d->zh)) return QPNG_ERR_ZLIB;
            continue;
        }
        if (d->zend) {   /* Adler-32 trailer (and anything after it, ignored as in qpng_decode) */
            while (n && d->nztrail < 4) { d->ztrail[d->nztrail++] = *p++; n--; }
            return QPNG_OK;
        }
        size_t used, made;
        const qpng_zrun r = qpng__zistream_run(d->zi, n, p, &used, d->rs - d->rpos, d->cur + d->rpos, &made);
        if (r == QPNG_ZRUN_BAD) return QPNG_ERR_DEFLATE;
        p += used; n -= used; d->rpos += made;
        if (d->rpos == d->rs) {
            if (d->y >= d->info.height) return QPNG_ERR_DEFLATE;   /* more data than the image */
            qpng_status s = d_row(d); if (s) return s;
            d->rpos = 0;
        }
        if (r == QPNG_ZRUN_END) d->zend = true;
        else if (!used && !made && d->rpos != d->rs) break;   /* needs more input */
    }
    return QPNG_OK;
}

/* a complete non-IDAT chunk (header ones kept in d->body) */
static qpng_status d_chunk_done(qpng_decoder d[static 1]) {
    const uint8_t *t = d->ctype;
    if (!memcmp(t, "IHDR", 4)) {
        if (d->have_info || d->clen != 13) return QPNG_ERR_HEADER;
        const qpng_status s = qpng__parse_ihdr(d->body, &d->o, &d->info);
        if (s) return s;
        d->have_info = true;
        d->buffered = !d->row_cb || d->info.interlaced || !qpng_backend_available(QPNG_BACKEND_ZLIBNG);
    } else if (!d->have_info) {
        return QPNG_ERR_HEADER;
    } else if (!memcmp(t, "IEND", 4)) {
        if (!d->seen_idat) return QPNG_ERR_CHUNK;
        d->state = D_END;
    } else if (!memcmp(t, "PLTE", 4)) {
        if (d->has_plte || d->seen_idat || d->clen % 3 || d->clen > 768 || !d->clen) return QPNG_ERR_CHUNK;
        memcpy(d->plte, d->body, d->clen); d->plen = d->clen / 3; d->has_plte = true; d->info.palette_entries = (uint16_t)d->plen;
    } else if (!memcmp(t, "tRNS", 4)) {
        if (d->has_trns || d->seen_idat) return QPNG_ERR_CHUNK;
        d->tlen = d->clen < 256 ? d->clen : 256; memcpy(d->trns, d->body, d->tlen); d->has_trns = true;
        d->info.trns_bytes = (uint16_t)d->clen;
    } else if (!(t[0] & 0x20)) {
        return QPNG_ERR_CHUNK;   /* unknown critical chunk */
    }
    return QPNG_OK;
}

qpng_status qpng_decoder_push(qpng_decoder *d, size_t n, const uint8_t p[n]) {
    if (!d || (!p && n)) return QPNG_ERR_ARGUMENT;
    if (d->state == D_ERR) return d->err;
    if (d->state != D_END && (!d->have_info || d->buffered) && !keep_all(d, n, p)) return d_fail(d, QPNG_ERR_NOMEM);
    while (n && d->state != D_END) {
        if (d->state == D_SIG) {
            while (n && d->nfix < 8) { if (*p != QPNG__SIG[d->nfix]) return d_fail(d, QPNG_ERR_SIGNATURE); d->nfix++; p++; n--; }
            if (d->nfix == 8) { d->state = D_LEN; d->nfix = 0; }
        } else if (d->state == D_LEN) {
            while (n && d->nfix < 8) { d->fix[d->nfix++] = *p++; n--; }
            if (d->nfix < 8) break;
            d->nfix = 0; d->clen = be32(d->fix); d->cpos = 0; memcpy(d->ctype, d->fix + 4, 4);
            if (d->clen > 0x7FFFFFFFu) return d_fail(d, QPNG_ERR_CHUNK);
            if (!d->have_info && memcmp(d->ctype, "IHDR", 4)) return d_fail(d, QPNG_ERR_HEADER);
            d->crc = libdeflate_crc32(0, d->ctype, 4);
            const bool idat = !memcmp(d->ctype, "IDAT", 4);
            if (idat) {
                if (d->idat_done) return d_fail(d, QPNG_ERR_CHUNK);
                if (!d->seen_idat) { qpng_status s = d_begin_image(d); if (s) return d_fail(d, s); d->seen_idat = true; }
            } else if (d->seen_idat) d->idat_done = true;
            d->nbody = 0;
            d->state = d->clen ? D_BODY : D_CRC;
        } else if (d->state == D_BODY) {
            size_t k = d->clen - d->cpos < n ? d->clen - d->cpos : n;
            if (d->o.verify_crc) d->crc = libdeflate_crc32(d->crc, p, k);
            if (!memcmp(d->ctype, "IDAT", 4)) {
                if (!d->buffered) { qpng_status s = d_idat(d, k, p); if (s) return d_fail(d, s); }
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
            if (d->have_info && !d->buffered && d->all) { qpng__free(dA(d), d->all); d->all = nullptr; d->nall = d->call = 0; }
        }
    }
    return d->state == D_END ? QPNG_OK : QPNG_MORE_DATA;
}

qpng_status qpng_decoder_finish(qpng_decoder *d, qpng_image *img) {
    if (img) *img = (qpng_image){};
    if (!d) return QPNG_ERR_ARGUMENT;
    if (d->state == D_ERR) return d->err;
    if (d->state != D_END) return QPNG_ERR_STATE;
    if (!d->buffered) {
        if (d->y != d->info.height || !d->zend) return d_fail(d, QPNG_ERR_DEFLATE);
        if (d->o.verify_adler && (d->nztrail < 4 || be32(d->ztrail) != d->adler)) return d_fail(d, QPNG_ERR_ADLER);
        return QPNG_OK;
    }
    qpng_image im;
    qpng_status s = qpng_decode(d->nall, d->all, &d->o, &im);
    if (s) return d_fail(d, s);
    qpng__free(dA(d), d->all); d->all = nullptr; d->nall = d->call = 0;
    if (d->row_cb) {
        for (uint32_t y = 0; y < im.height; y++)
            if (d->row_cb(d->user, y, im.stride, im.pixels + (size_t)y * im.stride)) { s = QPNG_ERR_ABORTED; break; }
        qpng_image_free(&im);
        return s ? d_fail(d, s) : QPNG_OK;
    }
    if (img) *img = im; else qpng_image_free(&im);
    return QPNG_OK;
}

static void decoder_freep(qpng_decoder **dp) { qpng_decoder_free(*dp); *dp = nullptr; }

qpng_status qpng_decode_stream(qpng_read_fn rd, void *ru, const qpng_decode_options *opts, qpng_row_fn row_cb, void *row_user,
                           qpng_image *img) {
    if (img) *img = (qpng_image){};
    if (!rd || (!row_cb && !img)) return QPNG_ERR_ARGUMENT;
    QPNG_CLEANUP(decoder_freep) qpng_decoder *d = nullptr;
    qpng_status s = qpng_decoder_new(opts, row_cb, row_user, &d);
    if (s) return s;
    const qpng_allocator *A = opts ? opts->allocator : nullptr;
    qpng_owned buf = { A, qpng__alloc(A, 65536) };
    if (!buf.p) return QPNG_ERR_NOMEM;
    s = QPNG_MORE_DATA;
    while (s == QPNG_MORE_DATA) {
        size_t k = rd(ru, 65536, buf.p);
        if (k == (size_t)-1) { s = QPNG_ERR_ABORTED; break; }
        if (!k) { s = QPNG_ERR_DEFLATE; break; }   /* truncated: no IEND */
        s = qpng_decoder_push(d, k, buf.p);
    }
    if (s == QPNG_OK) s = qpng_decoder_finish(d, img);
    return s;
}

/* ================================================= encoder ================================================= */
static constexpr size_t E_IDAT_MAX = 262144;
static constexpr size_t E_BATCH = 65536;

struct qpng_encoder {
    const qpng_allocator *A; qpng_allocator a;
    qpng_write_fn write; void *user;
    uint32_t W, H, y; int bpp, filter; bool hybrid, ultra;
    size_t n;                     /* bytes per row */
    qpng_zstream *z;
    uint8_t *rows; uint32_t nrows, max_rows;   /* rows[0] = previous row, then pending source rows */
    uint8_t *filt;                /* filtered batch (1 byte lookback in front) */
    uint8_t *idat; size_t nidat;  /* IDAT chunk being collected: payload at idat + 8, framed in place */
    uint8_t *tail; size_t ntail;  /* metadata after IDAT */
    qpng_status err; bool finished, big;
    size_t sunk;                  /* compressed bytes produced (hybrid filter decision) */
};

static qpng_status e_out(qpng_encoder e[static 1], size_t n, const uint8_t p[n]) {
    return n && e->write(e->user, n, p) ? QPNG_ERR_ABORTED : QPNG_OK;
}
static qpng_status e_flush_idat(qpng_encoder e[static 1]) {
    if (!e->nidat) return QPNG_OK;
    const uint8_t *end = qpng__put_chunk((uint32_t)e->nidat, e->idat, "IDAT", nullptr, false);
    e->nidat = 0;
    return e_out(e, (size_t)(end - e->idat), e->idat);
}
/* qpng_zsink: compressed bytes -> IDAT chunks */
static qpng_status e_sink(void *ctx, size_t n, const uint8_t p[static n]) {
    qpng_encoder *e = ctx;
    e->sunk += n;
    while (n) {
        size_t k = E_IDAT_MAX - e->nidat < n ? E_IDAT_MAX - e->nidat : n;
        memcpy(e->idat + 8 + e->nidat, p, k); e->nidat += k; p += k; n -= k;
        if (e->nidat == E_IDAT_MAX) { qpng_status s = e_flush_idat(e); if (s) return s; }
    }
    return QPNG_OK;
}

static void encoder_freep(qpng_encoder **ep) { qpng_encoder_free(*ep); *ep = nullptr; }

qpng_status qpng_encoder_new(uint32_t w, uint32_t h, qpng_color color, int depth, const qpng_encode_options *opts,
                         qpng_write_fn write, void *wu, qpng_encoder **out) {
    if (!out) return QPNG_ERR_ARGUMENT;
    *out = nullptr;
    qpng_encode_options defo;
    if (!opts) { qpng_encode_options_init(&defo); opts = &defo; }
    if (!write) return QPNG_ERR_ARGUMENT;
    const int ch = qpng__check_encode_opts(opts, w, h, color, depth);
    if (ch < 0) return (qpng_status)ch;
    const int bits = ch * depth;
    size_t n;
    if (qpng__row_bytes(&n, w, bits) || n > (64u << 20)) return QPNG_ERR_LIMITS;

    /* stream configuration */
    const bool zng_ok = qpng_backend_available(QPNG_BACKEND_ZLIBNG);
    const bool fast = opts->preset <= QPNG_PRESET_FAST;
    const bool use_zng = opts->backend == QPNG_BACKEND_ZLIBNG || (opts->backend == QPNG_BACKEND_AUTO && !fast && zng_ok);
    int level = opts->deflate_level;
    if (level && !use_zng && level != QPNG_DEFLATE_PNGULTRA) return QPNG_ERR_UNSUPPORTED;   /* libdeflate levels: not streamable */
    const qpng_zcfg zc = use_zng ? (qpng_zcfg){ .backend = QPNG_BACKEND_ZLIBNG,
                                                .level = level ? level : (int[]){ 1, 2, 6, 9 }[opts->preset],
                                                .depth = opts->search_depth, .nice = opts->nice_length,
                                                .strategy = opts->zlib_strategy }
                                 : (qpng_zcfg){ .backend = QPNG_BACKEND_LIBDEFLATE, .level = QPNG_DEFLATE_PNGULTRA };
    if (!qpng__zcfg_valid(&zc)) return QPNG_ERR_ARGUMENT;

    const qpng_allocator *A = opts->allocator;
    QPNG_CLEANUP(encoder_freep) qpng_encoder *e = qpng__alloc(A, sizeof *e);
    if (!e) return QPNG_ERR_NOMEM;
    *e = (qpng_encoder){};
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
        size_t rows_n, filt_n;
        if (qpng__mul_size(&rows_n, (size_t)e->max_rows + 1, n) || qpng__add_size(&rows_n, rows_n, 64) ||
            qpng__mul_size(&filt_n, e->max_rows, n + 1) || qpng__add_size(&filt_n, filt_n, 64))
            s = QPNG_ERR_LIMITS;
        else {
            e->rows = qpng__alloc(e->A, rows_n);
            e->filt = qpng__alloc(e->A, filt_n);
            e->idat = qpng__alloc(e->A, 12 + E_IDAT_MAX);
            if (!e->rows || !e->filt || !e->idat) s = QPNG_ERR_NOMEM;
        }
    }
    if (!s) { memset(e->rows, 0, n); memset(e->filt, 0, 1); e->z = qpng__zstream_new(&zc, e->A, e_sink, e, &s); }
    if (s) return s;
    if (opts->metadata) {
        s = qpng__meta_serialize(opts->metadata, 2, e->A, &e->tail, &e->ntail);
        if (s) return s;
    }
    /* signature, IHDR, metadata (before PLTE), PLTE, tRNS, metadata (before IDAT): one write */
    QPNG_CLEANUP(qpng__wb_release) qpng__wbuf hd = { .A = e->A };
    uint8_t ihdr[13];
    qpng__put_ihdr(ihdr, w, h, depth, (int)color);
    qpng__wb_put(&hd, sizeof QPNG__SIG, QPNG__SIG);
    qpng__wb_chunk(&hd, "IHDR", sizeof ihdr, ihdr);
    if (opts->metadata) qpng__meta_write(&hd, opts->metadata, 0);
    if (opts->palette) qpng__wb_chunk(&hd, "PLTE", 3u * (size_t)opts->palette_entries, opts->palette);
    if (opts->trns) qpng__wb_chunk(&hd, "tRNS", (size_t)opts->trns_bytes, opts->trns);
    if (opts->metadata) qpng__meta_write(&hd, opts->metadata, 1);
    s = hd.err ? hd.err : e_out(e, hd.n, hd.p);
    if (s) return s;
    *out = QPNG_TAKE(e);
    return QPNG_OK;
}

/* filter + compress the pending rows (rows[1..nrows], rows[0] = predecessor) */
static qpng_status e_compress(qpng_encoder e[static 1], bool final) {
    const size_t len = (size_t)e->nrows * (e->n + 1);
    const int f = e->hybrid && e->big ? QPNG_FILTER_UP : e->filter;
    if (e->nrows) qpng__filter_rows(1, e->nrows + 1, e->n, e->filt + 1, e->rows, e->n, e->bpp, f);
    const size_t before = e->sunk;
    qpng_status s = qpng__zstream_write(e->z, len, e->filt + 1, final);
    if (s) return s;
    e->big = (e->sunk - before) * 4 < len;   /* >= 4:1: graphics-like, hybrid switches to Up (as one-shot FAST) */
    e->filt[0] = e->filt[len];   /* png-ultra looks one byte back */
    if (e->nrows) memcpy(e->rows, e->rows + (size_t)e->nrows * e->n, e->n);
    e->nrows = 0;
    return QPNG_OK;
}

qpng_status qpng_encoder_push_rows(qpng_encoder *e, const uint8_t *src, size_t stride, uint32_t k) {
    if (!e || (!src && k)) return QPNG_ERR_ARGUMENT;
    if (e->err) return e->err;
    if (e->finished || k > e->H - e->y) return QPNG_ERR_STATE;
    if (!stride) stride = e->n;
    if (stride < e->n) return QPNG_ERR_ARGUMENT;
    for (uint32_t i = 0; i < k; i++) {
        if (e->nrows == e->max_rows) {
            qpng_status s = e_compress(e, false);
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
    qpng_status s = e_compress(e, true);
    if (!s) s = e_flush_idat(e);
    if (!s) s = e_out(e, e->ntail, e->tail);
    uint8_t iend[12];
    if (!s) s = e_out(e, (size_t)(qpng__put_chunk(0, iend, "IEND", nullptr, false) - iend), iend);
    e->finished = true;
    return s ? (e->err = s) : QPNG_OK;
}

void qpng_encoder_free(qpng_encoder *e) {
    if (!e) return;
    qpng_allocator ac = e->a;
    const qpng_allocator *A = e->A ? &ac : nullptr;
    qpng__zstream_free(e->z);
    qpng__free(A, e->rows); qpng__free(A, e->filt); qpng__free(A, e->idat); qpng__free(A, e->tail);
    qpng__free(A, e);
}
