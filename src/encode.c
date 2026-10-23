/* encode.c - PNG encoder: validation, presets, filtering, deflate (vendored libdeflate), chunk writing. */
#include "deflate_backend.h"
#include "metadata.h"
#include <stdio.h>

void qpng_encode_options_init(qpng_encode_options *o) {
    memset(o, 0, sizeof *o);
    o->preset = QPNG_PRESET_DEFAULT; o->filter = QPNG_FILTER_AUTO;
}

void qpng_buffer_free(void *buf, const qpng_allocator *a) { qpng__free(a, buf); }

static uint8_t *put_chunk(uint8_t *p, const char *type, const uint8_t *data, uint32_t len) {
    wbe32(p, len); memcpy(p + 4, type, 4);
    if (data && len) memcpy(p + 8, data, len);
    wbe32(p + 8 + len, libdeflate_crc32(0, p + 4, len + 4));
    return p + 12 + len;
}

/*
 * Presets (measured choices; see NOTES.md and README "Presets"):
 *  FASTEST  Up filter + png-ultra (single pass, runs only, sampled Huffman codes), streamed in row chunks.
 *  FAST     adaptive filter + png-ultra / png-fast hybrid, streamed.
 *  DEFAULT  1. probe: Up-biased adaptive filter (no Paeth for opaque images) + png-fast on a row sample;
 *              ratio >= GRAPHICS_RATIO means graphics-like content. The sample doubles as the filter trial.
 *           2. opaque graphics: same sample with filter None (4% allowance at depth <= 8), keep the winner;
 *              alpha graphics keep the Up-biased adaptive filter; photos: adaptive + png-fast over the image,
 *              8-bit palette photos also try Paeth.
 *           3. graphics: zlib-ng level 6 (libdeflate-only builds: tuned libdeflate L4-L8, see graphics_cfg);
 *              16-bit photos: also libdeflate L6, keep the smaller; other photos: keep the png-fast stream.
 *  BEST     steps 1-2, then graphics: zlib-ng level 9, chain 1024 (libdeflate-only: L10/48/96 or L9);
 *           photos: libdeflate L8, keeping the png-fast stream if smaller.
 * Backend choice per stream: graphics_cfg / photo_cfg (deflate_backend.h); qpng_encode_options.backend forces one.
 * An explicit filter or deflate_level in qpng_encode_options disables the probing.
 */
#define GRAPHICS_RATIO 6

/* content class from the probe, used by the backend policy */
typedef struct { int best, alpha, depth, bpp, filter_none; } qpng_content;

/*
 * Final-stream policy for probed graphics (DEFAULT / BEST). backend = the user's choice (AUTO: measured per
 * preset and content, NOTES.md "Phase 4"). Every entry is a measured choice; libdeflate graphics paths fix the
 * minimum match at 3 (libdeflate would raise it because few literal values are in use: -1.5%, faster).
 */
static qpng_zcfg graphics_cfg(const qpng_content *c, int backend, int zlevel, int zstrategy, int depth, int nice) {
    if (backend == QPNG_BACKEND_ZLIBNG)
        return qpng__zcfg(QPNG_BACKEND_ZLIBNG, zlevel ? zlevel : c->best ? 9 : 6, depth, nice, 0, zstrategy);
#ifdef QPNG_POLICY_OVERRIDE   /* development: QPNG_POLICY="b:level:depth:nice:minlen:strategy" for the final graphics stream */
    { const char *e = getenv("QPNG_POLICY"); int v[6] = { 0 };
      if (e && sscanf(e, "%d:%d:%d:%d:%d:%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) >= 2)
          return qpng__zcfg(v[0], v[1], v[2], v[3], v[4], v[5]); }
#endif
    if (backend == QPNG_BACKEND_AUTO && qpng__backend_available(QPNG_BACKEND_ZLIBNG)) {
        /* zlib-ng on the probed filtering (Phase 4 sweep, 40 graphics files, core 3, medians): level 6
         * (deflate_medium) is smaller than every libdeflate 4-8 setting below AND 1.1-1.3x faster on 5 of 7 groups;
         * level 9 with chain 1024 (default 4096) keeps ~98% of level 9's gain at 2x its speed and beats the
         * libdeflate near-optimal L10 in speed (2-5x) at -1..+6% size. */
        return c->best ? qpng__zcfg(QPNG_BACKEND_ZLIBNG, 9, 1024, 258, 0, QPNG_ZSTRATEGY_DEFAULT)
                       : qpng__zcfg(QPNG_BACKEND_ZLIBNG, 6, 0, 0, 0, QPNG_ZSTRATEGY_DEFAULT);
    }
    /* libdeflate only (QPNG_BACKEND_LIBDEFLATE or built without zlib-ng): qpng 1.0 policy */
    if (!c->best) {
        /* alpha 8-bit: lazy2 + long matches; None-filtered 8-bit RGB (flat UI, text = long exact repeats): greedy;
         * other RGB: shallower search; rest: level 5 */
        if (c->alpha && c->depth == 8) return qpng__zcfg(QPNG_BACKEND_LIBDEFLATE, 8, 48, 258, 3, 0);
        if (!c->alpha && c->bpp >= 3)
            return c->filter_none && c->depth == 8 ? qpng__zcfg(QPNG_BACKEND_LIBDEFLATE, 4, 4, 16, 3, 0)
                                                   : qpng__zcfg(QPNG_BACKEND_LIBDEFLATE, 5, 8, 32, 3, 0);
        return qpng__zcfg(QPNG_BACKEND_LIBDEFLATE, 5, 0, 0, 3, 0);
    }
    /* BEST: bounded near-optimal for alpha / 16-bit, else level 9 */
    return c->alpha || c->depth == 16 ? qpng__zcfg(QPNG_BACKEND_LIBDEFLATE, 10, 48, 96, 3, 0)
                                      : qpng__zcfg(QPNG_BACKEND_LIBDEFLATE, 9, 0, 0, 3, 0);
}

/* photos: second stream to try against the png-fast one (level 0 = none). BEST: level 8 (L10 is 2x slower for
 * ~1.5%; L8 already beats zlib-rs 9); DEFAULT 16-bit: level 6 (png-fast is weakest on noisy low bytes). */
static qpng_zcfg photo_cfg(const qpng_content *c, int backend, int zlevel, int zstrategy, int depth, int nice) {
    if (backend == QPNG_BACKEND_ZLIBNG)
        return qpng__zcfg(QPNG_BACKEND_ZLIBNG, zlevel ? zlevel : c->best ? 9 : 6, depth, nice, 0, zstrategy);
    if (c->best) return qpng__zcfg(QPNG_BACKEND_LIBDEFLATE, 8, 0, 0, 0, 0);
    if (c->depth == 16) return qpng__zcfg(QPNG_BACKEND_LIBDEFLATE, 6, 0, 0, 0, 0);
    return qpng__zcfg(QPNG_BACKEND_LIBDEFLATE, 0, 0, 0, 0, 0);
}

/*
 * Row sample (images >= 512 rows: 32-row strips out of every 192; >= 256 rows: 16 of every 64; smaller: whole),
 * filtered with 'filter' into 'scratch' and compressed with png-fast into 'zbuf': returns the compressed size
 * (0 on error) and the sample size in *raw. The caller passes its full-image filter and output buffers (always
 * large enough), so the probe adds no allocations and pre-faults pages the final pass reuses.
 */
static size_t sample_size(const uint8_t *px, size_t stride, uint32_t h, size_t n, int bpp, int filter,
                          uint8_t *scratch, uint8_t *zbuf, size_t zcap, size_t *raw, qpng_status *st) {
    /* long strips keep the vertical match context that Up/None rely on */
    const uint32_t strip = h >= 512 ? 32 : 16, period = h >= 512 ? 192 : h >= 256 ? 64 : strip;
    const size_t row = n + 1;
    size_t off = 0;
    for (uint32_t y = 0; y < h; y += period) {
        const uint32_t k = h - y < strip ? h - y : strip;
        qpng__filter_rows(scratch + off, px, stride, y, y + k, n, bpp, filter);
        off += (size_t)k * row;
    }
    *raw = off;
    const qpng_zcfg qpng = qpng__zcfg(QPNG_BACKEND_LIBDEFLATE, QPNG_DEFLATE_QPNG, 0, 0, 0, 0);
    return qpng__zcompress(&qpng, scratch, off, zbuf, zcap, NULL, st);
}

qpng_status qpng_encode(const void *pixels, uint32_t w, uint32_t h, size_t stride, qpng_color color, int depth,
                    const qpng_encode_options *opts, uint8_t **outp, size_t *outn) {
    qpng_encode_options defo;
    if (!outp || !outn) return QPNG_ERR_ARGUMENT;
    *outp = NULL; *outn = 0;
    if (!opts) { qpng_encode_options_init(&defo); opts = &defo; }
    const qpng_allocator *A = opts->allocator;
    int ch;
    switch (color) {
    case QPNG_COLOR_GRAY: ch = 1; if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) return QPNG_ERR_ARGUMENT; break;
    case QPNG_COLOR_PALETTE: ch = 1; if (depth != 1 && depth != 2 && depth != 4 && depth != 8) return QPNG_ERR_ARGUMENT; break;
    case QPNG_COLOR_RGB: ch = 3; goto d816;
    case QPNG_COLOR_GRAY_ALPHA: ch = 2; goto d816;
    case QPNG_COLOR_RGBA: ch = 4;
    d816: if (depth != 8 && depth != 16) return QPNG_ERR_ARGUMENT; break;
    default: return QPNG_ERR_ARGUMENT;
    }
    if (!pixels || !w || !h || w > 0x7FFFFFFFu || h > 0x7FFFFFFFu) return QPNG_ERR_ARGUMENT;
    if (color == QPNG_COLOR_PALETTE && (!opts->palette || opts->palette_entries < 1 || opts->palette_entries > 256))
        return QPNG_ERR_ARGUMENT;
    if (opts->palette && (opts->palette_entries < 1 || opts->palette_entries > 256)) return QPNG_ERR_ARGUMENT;
    if (opts->trns && (opts->trns_bytes < 1 || color == QPNG_COLOR_GRAY_ALPHA || color == QPNG_COLOR_RGBA ||
                       (color == QPNG_COLOR_GRAY && opts->trns_bytes != 2) || (color == QPNG_COLOR_RGB && opts->trns_bytes != 6) ||
                       (color == QPNG_COLOR_PALETTE && opts->trns_bytes > opts->palette_entries)))
        return QPNG_ERR_ARGUMENT;
    if (opts->metadata) { qpng_status ms = qpng__meta_validate(opts->metadata, color); if (ms != QPNG_OK) return ms; }
    const int bits = ch * depth, bpp = bits >= 8 ? bits / 8 : 1;
    const size_t rowbytes = ((size_t)w * bits + 7) / 8;
    if (!stride) stride = rowbytes;
    if (stride < rowbytes) return QPNG_ERR_ARGUMENT;
    if ((uint64_t)h * (rowbytes + 1) > ((uint64_t)1 << 40)) return QPNG_ERR_LIMITS;
    const size_t rawn = (size_t)h * (rowbytes + 1);

    const int backend = opts->backend;
    if (backend < QPNG_BACKEND_AUTO || backend > QPNG_BACKEND_ZLIBNG) return QPNG_ERR_ARGUMENT;
    if (!qpng_backend_available((qpng_backend)backend)) return QPNG_ERR_UNSUPPORTED;
    if (opts->zlib_strategy < 0 || opts->zlib_strategy > QPNG_ZSTRATEGY_RLE) return QPNG_ERR_ARGUMENT;
    const int zng = backend == QPNG_BACKEND_ZLIBNG;
    const int probe = opts->filter < 0 && !opts->deflate_level &&
                      (opts->preset == QPNG_PRESET_DEFAULT || opts->preset == QPNG_PRESET_BEST);
    int filter, level;
    switch (opts->preset) {   /* zlib-ng forced: FASTEST 1 (deflate_quick), FAST 2, DEFAULT 6, BEST 9 */
    case QPNG_PRESET_FASTEST: filter = QPNG_FILTER_UP; level = zng ? 1 : QPNG_DEFLATE_PNGULTRA; break;
    case QPNG_PRESET_FAST: filter = QPNG_FILTER_ADAPTIVE; level = zng ? 2 : QPNG_DEFLATE_PNGULTRA; break;
    case QPNG_PRESET_DEFAULT: filter = QPNG_FILTER_ADAPTIVE; level = zng ? 6 : QPNG_DEFLATE_QPNG; break;
    case QPNG_PRESET_BEST: filter = QPNG_FILTER_ADAPTIVE; level = zng ? 9 : 10; break;
    default: return QPNG_ERR_ARGUMENT;
    }
    if (opts->filter >= 0) { if (opts->filter > QPNG_FILTER_ADAPTIVE) return QPNG_ERR_ARGUMENT; filter = opts->filter; }
    if (opts->deflate_level) level = opts->deflate_level;
    if (opts->search_depth < 0 || opts->search_depth > 65535 || opts->nice_length < 0 || opts->nice_length > 258)
        return QPNG_ERR_ARGUMENT;
    const qpng_zcfg explicit_cfg = qpng__zcfg(zng ? QPNG_BACKEND_ZLIBNG : QPNG_BACKEND_LIBDEFLATE, level, opts->search_depth,
                                          opts->nice_length, 0, zng ? opts->zlib_strategy : 0);
    if (!qpng__zcfg_valid(&explicit_cfg)) return QPNG_ERR_ARGUMENT;

    const int stream = !probe && !zng && level == QPNG_DEFLATE_PNGULTRA;
    uint8_t *filt = NULL;
    if (!stream) {
        filt = qpng__alloc(A, rawn + 64);
        if (!filt) return QPNG_ERR_NOMEM;
        if (!probe) qpng__filter_image(filt, pixels, stride, h, rowbytes, bpp, filter);   /* probe: filtered below */
    }

    /* worst-case bound over all compression levels */
    const size_t zcap = qpng__zbound(rawn);
    const size_t nchunks = zcap / 0x40000000u + 1;   /* IDAT chunks are split at 1 GiB */
    /* ancillary chunks, serialised up front (sizes needed for the output bound) */
    uint8_t *mb[3] = { NULL, NULL, NULL }; size_t mn[3] = { 0, 0, 0 };
    if (opts->metadata) {
        qpng_status ms = QPNG_OK;
        for (int l = 0; l < 3 && ms == QPNG_OK; l++) ms = qpng__meta_serialize(opts->metadata, l, A, &mb[l], &mn[l]);
        if (ms != QPNG_OK) { for (int l = 0; l < 3; l++) qpng__free(A, mb[l]); qpng__free(A, filt); return ms; }
    }
    const size_t cap = 8 + 25 + (opts->palette ? 12 + 3 * (size_t)opts->palette_entries : 0) +
                       (opts->trns ? 12 + (size_t)opts->trns_bytes : 0) + 12 * nchunks + zcap + 12 + mn[0] + mn[1] + mn[2];
    uint8_t *out = qpng__alloc(A, cap);
    if (!out) { for (int l = 0; l < 3; l++) qpng__free(A, mb[l]); qpng__free(A, filt); return QPNG_ERR_NOMEM; }
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    uint8_t *p = out;
    memcpy(p, sig, 8); p += 8;
    uint8_t ihdr[13];
    wbe32(ihdr, w); wbe32(ihdr + 4, h); ihdr[8] = (uint8_t)depth; ihdr[9] = (uint8_t)color; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    p = put_chunk(p, "IHDR", ihdr, 13);
    if (mn[0]) { memcpy(p, mb[0], mn[0]); p += mn[0]; }
    if (opts->palette) p = put_chunk(p, "PLTE", opts->palette, 3u * (uint32_t)opts->palette_entries);
    if (opts->trns) p = put_chunk(p, "tRNS", opts->trns, (uint32_t)opts->trns_bytes);
    if (mn[1]) { memcpy(p, mb[1], mn[1]); p += mn[1]; }
    /* single IDAT (bound < 1 GiB): compress in place behind the IDAT header. Otherwise compress to the end of the
     * buffer and lay the chunks out in front of the data. */
    uint8_t *z = nchunks == 1 ? p + 8 : out + cap - zcap - 12;
    qpng_status st = QPNG_OK;
    size_t zn;
    if (stream) {
        zn = qpng__zcompress_ultra_rows(pixels, stride, h, rowbytes, bpp, filter, opts->filter < 0 && !opts->deflate_level,
                                      z, zcap, A, &st);
    } else if (!probe) {
        zn = qpng__zcompress(&explicit_cfg, filt, rawn, z, zcap, A, &st);
    } else {
        /* 1. content probe on a row sample (Up-biased adaptive filter, png-fast): ratio >= GRAPHICS_RATIO = graphics */
        const int best = opts->preset == QPNG_PRESET_BEST;
        const int alpha = color == QPNG_COLOR_GRAY_ALPHA || color == QPNG_COLOR_RGBA;
        const int gfx_filter = alpha ? QPNG__FILTER_ADAPTIVE_GFX : QPNG__FILTER_ADAPTIVE_FLAT;   /* probe + trial filter */
        size_t sraw = 0, sz = sample_size(pixels, stride, h, rowbytes, bpp, gfx_filter, filt, z, zcap,
                                            &sraw, &st);
        const int graphics = sz && (double)sraw >= (double)sz * GRAPHICS_RATIO;
        qpng_content cc = { best, alpha, depth, bpp, 0 };
        const qpng_zcfg qpng = qpng__zcfg(QPNG_BACKEND_LIBDEFLATE, QPNG_DEFLATE_QPNG, 0, 0, 0, 0);
        uint8_t *tmp = NULL;
        zn = sz ? 1 : 0;   /* "no error so far" until a stream exists */
        if (zn && (best || zng || !graphics)) {
            tmp = qpng__alloc(A, zcap);
            if (!tmp) { st = QPNG_ERR_NOMEM; zn = 0; }
        }
        /* 2. filter choice. Graphics without alpha: filter None often wins (sample decides); alpha graphics:
         *    adaptive always won in the corpus. Photos: png-fast stream with adaptive filtering; 8-bit palette
         *    photos also try Paeth everywhere (full trial, smaller stream kept). */
        int f = gfx_filter;
        if (zn && graphics) {
            if (!alpha) {
                size_t nraw, gz = sz;   /* the probe sample already used this filter */
                size_t nz = sample_size(pixels, stride, h, rowbytes, bpp, QPNG_FILTER_NONE, filt, z, zcap, &nraw, &st);
                if (!nz) zn = 0;
                /* the row sample cuts the long-range matches None relies on (text, repeated UI elements): give
                 * None a 4% allowance at depth <= 8 (16-bit samples are large enough; measured on the corpus) */
                else if (depth <= 8 ? nz * 100 <= gz * 104 : nz < gz) f = QPNG_FILTER_NONE;
            }
            cc.filter_none = f == QPNG_FILTER_NONE;
            if (zn) qpng__filter_image(filt, pixels, stride, h, rowbytes, bpp, f);
        } else if (zn && !graphics) {
            qpng__filter_image(filt, pixels, stride, h, rowbytes, bpp, QPNG_FILTER_ADAPTIVE);
            zn = qpng__zcompress(&qpng, filt, rawn, z, zcap, A, &st);
            if (zn && color == QPNG_COLOR_PALETTE && depth == 8) {
                uint8_t *alt = qpng__alloc(A, rawn + 64);
                if (!alt) { st = QPNG_ERR_NOMEM; zn = 0; }
                else {
                    qpng__filter_image(alt, pixels, stride, h, rowbytes, bpp, QPNG_FILTER_PAETH);
                    size_t an = qpng__zcompress(&qpng, alt, rawn, tmp, zcap, A, &st);
                    if (!an) zn = 0;
                    else if (an < zn) { uint8_t *t = filt; filt = alt; alt = t; memcpy(z, tmp, an); zn = an; }
                    qpng__free(A, alt);
                }
            }
        }
        /* 3. final compression of the chosen filtering (policy: graphics_cfg / photo_cfg) */
        if (zn && graphics) {
            const qpng_zcfg g = graphics_cfg(&cc, backend, opts->deflate_level, opts->zlib_strategy, opts->search_depth,
                                           opts->nice_length);
            zn = qpng__zcompress(&g, filt, rawn, z, zcap, A, &st);
        } else if (zn) {
            /* photos: the png-fast stream in z is kept if the second stream is not smaller (zlib-ng forced: replaced) */
            const qpng_zcfg ph = photo_cfg(&cc, backend, opts->deflate_level, opts->zlib_strategy, opts->search_depth,
                                         opts->nice_length);
            if (ph.level) {
                size_t an = qpng__zcompress(&ph, filt, rawn, tmp, zcap, A, &st);
                if (!an) zn = 0;
                else if (an < zn || zng) { memcpy(z, tmp, an); zn = an; }
            }
        }
        qpng__free(A, tmp);
    }
    qpng__free(A, filt);
    if (!zn) { for (int l = 0; l < 3; l++) qpng__free(A, mb[l]); qpng__free(A, out); return st != QPNG_OK ? st : QPNG_ERR_ENCODE; }
    for (size_t done = 0; done < zn;) {
        uint32_t len = (uint32_t)((zn - done) < 0x40000000u ? zn - done : 0x40000000u);
        if (p + 8 != z + done) memmove(p + 8, z + done, len);   /* p + 8 <= z + done always holds: chunks only add 12-byte overhead */
        p = put_chunk(p, "IDAT", NULL, len);
        done += len;
    }
    if (mn[2]) { memcpy(p, mb[2], mn[2]); p += mn[2]; }
    for (int l = 0; l < 3; l++) qpng__free(A, mb[l]);
    p = put_chunk(p, "IEND", NULL, 0);
    *outn = (size_t)(p - out);
    *outp = qpng__realloc_shrink(A, out, cap, *outn);
    return QPNG_OK;
}

int qpng_backend_available(qpng_backend b) { return b == QPNG_BACKEND_AUTO || qpng__backend_available((int)b); }
