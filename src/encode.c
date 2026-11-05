/* encode.c - PNG encoder: validation, presets, filtering, deflate (vendored libdeflate), chunk writing. */
#include "deflate_backend.h"
#include "metadata.h"
#include <stdio.h>

void qpng_encode_options_init(qpng_encode_options *o) {
    memset(o, 0, sizeof *o);
    o->preset = QPNG_PRESET_DEFAULT; o->filter = QPNG_FILTER_AUTO;
}

void qpng_buffer_free(void *buf, const qpng_allocator *a) { qpng__free(a, buf); }

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
static constexpr int GRAPHICS_RATIO = 6;

/* serialised ancillary chunks per location (before PLTE, before IDAT, after IDAT) */
typedef struct { const qpng_allocator *A; uint8_t *b[3]; size_t n[3]; } meta_chunks;
static void meta_release(meta_chunks *m) {
    for (int l = 0; l < 3; l++) { qpng__free(m->A, m->b[l]); m->b[l] = nullptr; }
}

/* content class from the probe, used by the backend policy */
typedef struct { int best, alpha, depth, bpp, filter_none; } qpng_content;

/*
 * Final-stream policy for probed graphics (DEFAULT / BEST). backend = the user's choice (AUTO: measured per
 * preset and content, NOTES.md "Phase 4"). Every entry is a measured choice; libdeflate graphics paths fix the
 * minimum match at 3 (libdeflate would raise it because few literal values are in use: -1.5%, faster).
 */
static qpng_zcfg graphics_cfg(const qpng_content *c, int backend, int zlevel, int zstrategy, int depth, int nice) {
    if (backend == QPNG_BACKEND_ZLIBNG)
        return (qpng_zcfg){ .backend = QPNG_BACKEND_ZLIBNG, .level = zlevel ? zlevel : c->best ? 9 : 6,
                            .depth = depth, .nice = nice, .strategy = zstrategy };
#ifdef QPNG_POLICY_OVERRIDE   /* development: QPNG_POLICY="b:level:depth:nice:minlen:strategy" for the final graphics stream */
    { const char *e = getenv("QPNG_POLICY"); int v[6] = { 0 };
      if (e && sscanf(e, "%d:%d:%d:%d:%d:%d", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) >= 2)
          return (qpng_zcfg){ .backend = v[0], .level = v[1], .depth = v[2], .nice = v[3], .minlen = v[4],
                              .strategy = v[5] }; }
#endif
    if (backend == QPNG_BACKEND_AUTO && qpng__backend_available(QPNG_BACKEND_ZLIBNG)) {
        /* zlib-ng on the probed filtering (Phase 4 sweep, 40 graphics files, core 3, medians): level 6
         * (deflate_medium) is smaller than every libdeflate 4-8 setting below AND 1.1-1.3x faster on 5 of 7 groups;
         * level 9 with chain 1024 (default 4096) keeps ~98% of level 9's gain at 2x its speed and beats the
         * libdeflate near-optimal L10 in speed (2-5x) at -1..+6% size. */
        return c->best ? (qpng_zcfg){ .backend = QPNG_BACKEND_ZLIBNG, .level = 9, .depth = 1024, .nice = 258 }
                       : (qpng_zcfg){ .backend = QPNG_BACKEND_ZLIBNG, .level = 6 };
    }
    /* libdeflate only (QPNG_BACKEND_LIBDEFLATE or built without zlib-ng): qpng 1.0 policy */
    if (!c->best) {
        /* alpha 8-bit: lazy2 + long matches; None-filtered 8-bit RGB (flat UI, text = long exact repeats): greedy;
         * other RGB: shallower search; rest: level 5 */
        if (c->alpha && c->depth == 8)
            return (qpng_zcfg){ .backend = QPNG_BACKEND_LIBDEFLATE, .level = 8, .depth = 48, .nice = 258, .minlen = 3 };
        if (!c->alpha && c->bpp >= 3)
            return c->filter_none && c->depth == 8
                       ? (qpng_zcfg){ .backend = QPNG_BACKEND_LIBDEFLATE, .level = 4, .depth = 4, .nice = 16, .minlen = 3 }
                       : (qpng_zcfg){ .backend = QPNG_BACKEND_LIBDEFLATE, .level = 5, .depth = 8, .nice = 32, .minlen = 3 };
        return (qpng_zcfg){ .backend = QPNG_BACKEND_LIBDEFLATE, .level = 5, .minlen = 3 };
    }
    /* BEST: bounded near-optimal for alpha / 16-bit, else level 9 */
    return c->alpha || c->depth == 16
               ? (qpng_zcfg){ .backend = QPNG_BACKEND_LIBDEFLATE, .level = 10, .depth = 48, .nice = 96, .minlen = 3 }
               : (qpng_zcfg){ .backend = QPNG_BACKEND_LIBDEFLATE, .level = 9, .minlen = 3 };
}

/* photos: second stream to try against the png-fast one (level 0 = none). BEST: level 8 (L10 is 2x slower for
 * ~1.5%; L8 already beats zlib-rs 9); DEFAULT 16-bit: level 6 (png-fast is weakest on noisy low bytes). */
static qpng_zcfg photo_cfg(const qpng_content *c, int backend, int zlevel, int zstrategy, int depth, int nice) {
    if (backend == QPNG_BACKEND_ZLIBNG)
        return (qpng_zcfg){ .backend = QPNG_BACKEND_ZLIBNG, .level = zlevel ? zlevel : c->best ? 9 : 6,
                            .depth = depth, .nice = nice, .strategy = zstrategy };
    if (c->best) return (qpng_zcfg){ .backend = QPNG_BACKEND_LIBDEFLATE, .level = 8 };
    if (c->depth == 16) return (qpng_zcfg){ .backend = QPNG_BACKEND_LIBDEFLATE, .level = 6 };
    return (qpng_zcfg){ .backend = QPNG_BACKEND_LIBDEFLATE };
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
    const qpng_zcfg qpng = (qpng_zcfg){ .backend = QPNG_BACKEND_LIBDEFLATE, .level = QPNG_DEFLATE_QPNG };
    return qpng__zcompress(&qpng, scratch, off, zbuf, zcap, nullptr, st);
}

int qpng__check_encode_opts(const qpng_encode_options o[static 1], uint32_t w, uint32_t h, qpng_color color, int depth) {
    int ch;
    switch (color) {
    case QPNG_COLOR_GRAY: ch = 1; break;
    case QPNG_COLOR_PALETTE: ch = 1; break;
    case QPNG_COLOR_GRAY_ALPHA: ch = 2; break;
    case QPNG_COLOR_RGB: ch = 3; break;
    case QPNG_COLOR_RGBA: ch = 4; break;
    default: return QPNG_ERR_ARGUMENT;
    }
    const bool sub8 = depth == 1 || depth == 2 || depth == 4;
    if (!(depth == 8 || (depth == 16 && color != QPNG_COLOR_PALETTE) || (sub8 && ch == 1))) return QPNG_ERR_ARGUMENT;
    if (!w || !h || w > 0x7FFFFFFFu || h > 0x7FFFFFFFu) return QPNG_ERR_ARGUMENT;
    if ((color == QPNG_COLOR_PALETTE && !o->palette) || (o->palette && (o->palette_entries < 1 || o->palette_entries > 256)))
        return QPNG_ERR_ARGUMENT;
    if (o->trns && (o->trns_bytes < 1 || color == QPNG_COLOR_GRAY_ALPHA || color == QPNG_COLOR_RGBA ||
                    (color == QPNG_COLOR_GRAY && o->trns_bytes != 2) || (color == QPNG_COLOR_RGB && o->trns_bytes != 6) ||
                    (color == QPNG_COLOR_PALETTE && o->trns_bytes > o->palette_entries)))
        return QPNG_ERR_ARGUMENT;
    if (o->metadata) { qpng_status ms = qpng__meta_validate(o->metadata, (int)color); if (ms != QPNG_OK) return ms; }
    if (o->backend < QPNG_BACKEND_AUTO || o->backend > QPNG_BACKEND_ZLIBNG || (unsigned)o->preset > QPNG_PRESET_BEST ||
        o->filter < QPNG_FILTER_AUTO || o->filter > QPNG_FILTER_ADAPTIVE ||
        o->zlib_strategy < 0 || o->zlib_strategy > QPNG_ZSTRATEGY_RLE ||
        o->search_depth < 0 || o->search_depth > 65535 || o->nice_length < 0 || o->nice_length > 258)
        return QPNG_ERR_ARGUMENT;
    if (!qpng_backend_available((qpng_backend)o->backend)) return QPNG_ERR_UNSUPPORTED;
    return ch;
}

qpng_status qpng_encode(const void *pixels, uint32_t w, uint32_t h, size_t stride, qpng_color color, int depth,
                    const qpng_encode_options *opts, uint8_t **outp, size_t *outn) {
    qpng_encode_options defo;
    if (!outp || !outn) return QPNG_ERR_ARGUMENT;
    *outp = nullptr; *outn = 0;
    if (!opts) { qpng_encode_options_init(&defo); opts = &defo; }
    const qpng_allocator *A = opts->allocator;
    if (!pixels) return QPNG_ERR_ARGUMENT;
    const int ch = qpng__check_encode_opts(opts, w, h, color, depth);
    if (ch < 0) return ch;
    const int bits = ch * depth, bpp = bits >= 8 ? bits / 8 : 1;
    const size_t rowbytes = ((size_t)w * (size_t)bits + 7) / 8;
    if (!stride) stride = rowbytes;
    if (stride < rowbytes) return QPNG_ERR_ARGUMENT;
    if ((uint64_t)h * (rowbytes + 1) > ((uint64_t)1 << 40)) return QPNG_ERR_LIMITS;
    const size_t rawn = (size_t)h * (rowbytes + 1);

    const int backend = opts->backend;
    const bool zng = backend == QPNG_BACKEND_ZLIBNG;
    const bool probe = opts->filter < 0 && !opts->deflate_level &&
                      (opts->preset == QPNG_PRESET_DEFAULT || opts->preset == QPNG_PRESET_BEST);
    int filter, level;
    switch (opts->preset) {   /* zlib-ng forced: FASTEST 1 (deflate_quick), FAST 2, DEFAULT 6, BEST 9 */
    case QPNG_PRESET_FASTEST: filter = QPNG_FILTER_UP; level = zng ? 1 : QPNG_DEFLATE_PNGULTRA; break;
    case QPNG_PRESET_FAST: filter = QPNG_FILTER_ADAPTIVE; level = zng ? 2 : QPNG_DEFLATE_PNGULTRA; break;
    case QPNG_PRESET_DEFAULT: filter = QPNG_FILTER_ADAPTIVE; level = zng ? 6 : QPNG_DEFLATE_QPNG; break;
    case QPNG_PRESET_BEST: filter = QPNG_FILTER_ADAPTIVE; level = zng ? 9 : 10; break;
    default: unreachable();   /* qpng__check_encode_opts */
    }
    if (opts->filter >= 0) filter = opts->filter;
    if (opts->deflate_level) level = opts->deflate_level;
    const qpng_zcfg explicit_cfg = (qpng_zcfg){
        .backend = zng ? QPNG_BACKEND_ZLIBNG : QPNG_BACKEND_LIBDEFLATE, .level = level,
        .depth = opts->search_depth, .nice = opts->nice_length, .strategy = zng ? opts->zlib_strategy : 0,
    };
    if (!qpng__zcfg_valid(&explicit_cfg)) return QPNG_ERR_ARGUMENT;

    const bool stream = !probe && !zng && level == QPNG_DEFLATE_PNGULTRA;
    qpng_owned filt = { A, nullptr };
    if (!stream) {
        filt.p = qpng__alloc(A, rawn + 64);
        if (!filt.p) return QPNG_ERR_NOMEM;
        if (!probe) qpng__filter_image(filt.p, pixels, stride, h, rowbytes, bpp, filter);   /* probe: filtered below */
    }

    /* worst-case bound over all compression levels */
    const size_t zcap = qpng__zbound(rawn);
    const size_t nchunks = zcap / 0x40000000u + 1;   /* IDAT chunks are split at 1 GiB */
    /* ancillary chunks, serialised up front (sizes needed for the output bound) */
    QPNG_CLEANUP(meta_release) meta_chunks m = { .A = A };
    const size_t *mn = m.n;
    for (int l = 0; opts->metadata && l < 3; l++) {
        qpng_status ms = qpng__meta_serialize(opts->metadata, l, A, &m.b[l], &m.n[l]);
        if (ms != QPNG_OK) return ms;
    }
    const size_t cap = 8 + 25 + (opts->palette ? 12 + 3 * (size_t)opts->palette_entries : 0) +
                       (opts->trns ? 12 + (size_t)opts->trns_bytes : 0) + 12 * nchunks + zcap + 12 + mn[0] + mn[1] + mn[2];
    qpng_owned out_ = { A, qpng__alloc(A, cap) };
    uint8_t *const out = out_.p;
    if (!out) return QPNG_ERR_NOMEM;
    uint8_t *p = out;
    memcpy(p, QPNG__SIG, sizeof QPNG__SIG); p += sizeof QPNG__SIG;
    uint8_t ihdr[13];
    qpng__put_ihdr(ihdr, w, h, depth, (int)color);
    p = qpng__put_chunk(p, "IHDR", ihdr, 13);
    if (mn[0]) { memcpy(p, m.b[0], mn[0]); p += mn[0]; }
    if (opts->palette) p = qpng__put_chunk(p, "PLTE", opts->palette, 3u * (uint32_t)opts->palette_entries);
    if (opts->trns) p = qpng__put_chunk(p, "tRNS", opts->trns, (uint32_t)opts->trns_bytes);
    if (mn[1]) { memcpy(p, m.b[1], mn[1]); p += mn[1]; }
    /* single IDAT (bound < 1 GiB): compress in place behind the IDAT header. Otherwise compress to the end of the
     * buffer and lay the chunks out in front of the data. */
    uint8_t *z = nchunks == 1 ? p + 8 : out + cap - zcap - 12;
    qpng_status st = QPNG_OK;
    size_t zn;
    if (stream) {
        zn = qpng__zcompress_ultra_rows(pixels, stride, h, rowbytes, bpp, filter, opts->filter < 0 && !opts->deflate_level,
                                      z, zcap, A, &st);
    } else if (!probe) {
        zn = qpng__zcompress(&explicit_cfg, filt.p, rawn, z, zcap, A, &st);
    } else {
        /* 1. content probe on a row sample (Up-biased adaptive filter, png-fast): ratio >= GRAPHICS_RATIO = graphics */
        const bool best = opts->preset == QPNG_PRESET_BEST;
        const bool alpha = color == QPNG_COLOR_GRAY_ALPHA || color == QPNG_COLOR_RGBA;
        const int gfx_filter = alpha ? QPNG__FILTER_ADAPTIVE_GFX : QPNG__FILTER_ADAPTIVE_FLAT;   /* probe + trial filter */
        size_t sraw = 0, sz = sample_size(pixels, stride, h, rowbytes, bpp, gfx_filter, filt.p, z, zcap,
                                            &sraw, &st);
        const bool graphics = sz && (double)sraw >= (double)sz * GRAPHICS_RATIO;
        qpng_content cc = { best, alpha, depth, bpp, 0 };
        const qpng_zcfg qpng = (qpng_zcfg){ .backend = QPNG_BACKEND_LIBDEFLATE, .level = QPNG_DEFLATE_QPNG };
        if (!sz) return st != QPNG_OK ? st : QPNG_ERR_ENCODE;
        qpng_owned tmp_ = { A, best || zng || !graphics ? qpng__alloc(A, zcap) : nullptr };
        uint8_t *const tmp = tmp_.p;
        if (!tmp && (best || zng || !graphics)) return QPNG_ERR_NOMEM;
        zn = 1;   /* "no error so far" until a stream exists */
        /* 2. filter choice. Graphics without alpha: filter None often wins (sample decides); alpha graphics:
         *    adaptive always won in the corpus. Photos: png-fast stream with adaptive filtering; 8-bit palette
         *    photos also try Paeth everywhere (full trial, smaller stream kept). */
        int f = gfx_filter;
        if (zn && graphics) {
            if (!alpha) {
                size_t nraw, gz = sz;   /* the probe sample already used this filter */
                size_t nz = sample_size(pixels, stride, h, rowbytes, bpp, QPNG_FILTER_NONE, filt.p, z, zcap, &nraw, &st);
                if (!nz) zn = 0;
                /* the row sample cuts the long-range matches None relies on (text, repeated UI elements): give
                 * None a 4% allowance at depth <= 8 (16-bit samples are large enough; measured on the corpus) */
                else if (depth <= 8 ? nz * 100 <= gz * 104 : nz < gz) f = QPNG_FILTER_NONE;
            }
            cc.filter_none = f == QPNG_FILTER_NONE;
            if (zn) qpng__filter_image(filt.p, pixels, stride, h, rowbytes, bpp, f);
        } else if (zn && !graphics) {
            qpng__filter_image(filt.p, pixels, stride, h, rowbytes, bpp, QPNG_FILTER_ADAPTIVE);
            zn = qpng__zcompress(&qpng, filt.p, rawn, z, zcap, A, &st);
            if (zn && color == QPNG_COLOR_PALETTE && depth == 8) {
                qpng_owned alt = { A, qpng__alloc(A, rawn + 64) };
                if (!alt.p) return QPNG_ERR_NOMEM;
                qpng__filter_image(alt.p, pixels, stride, h, rowbytes, bpp, QPNG_FILTER_PAETH);
                size_t an = qpng__zcompress(&qpng, alt.p, rawn, tmp, zcap, A, &st);
                if (!an) zn = 0;
                else if (an < zn) { void *t = filt.p; filt.p = alt.p; alt.p = t; memcpy(z, tmp, an); zn = an; }
            }
        }
        /* 3. final compression of the chosen filtering (policy: graphics_cfg / photo_cfg) */
        if (zn && graphics) {
            const qpng_zcfg g = graphics_cfg(&cc, backend, opts->deflate_level, opts->zlib_strategy, opts->search_depth,
                                           opts->nice_length);
            zn = qpng__zcompress(&g, filt.p, rawn, z, zcap, A, &st);
        } else if (zn) {
            /* photos: the png-fast stream in z is kept if the second stream is not smaller (zlib-ng forced: replaced) */
            const qpng_zcfg ph = photo_cfg(&cc, backend, opts->deflate_level, opts->zlib_strategy, opts->search_depth,
                                         opts->nice_length);
            if (ph.level) {
                size_t an = qpng__zcompress(&ph, filt.p, rawn, tmp, zcap, A, &st);
                if (!an) zn = 0;
                else if (an < zn || zng) { memcpy(z, tmp, an); zn = an; }
            }
        }
    }
    qpng__free(A, qpng__take(&filt));   /* the largest buffer: before the IDAT layout touches more pages */
    if (!zn) return st != QPNG_OK ? st : QPNG_ERR_ENCODE;
    for (size_t done = 0; done < zn;) {
        uint32_t len = (uint32_t)((zn - done) < 0x40000000u ? zn - done : 0x40000000u);
        if (p + 8 != z + done) memmove(p + 8, z + done, len);   /* p + 8 <= z + done always holds: chunks only add 12-byte overhead */
        p = qpng__put_chunk(p, "IDAT", nullptr, len);
        done += len;
    }
    if (mn[2]) { memcpy(p, m.b[2], mn[2]); p += mn[2]; }
    p = qpng__put_chunk(p, "IEND", nullptr, 0);
    *outn = (size_t)(p - out);
    *outp = qpng__realloc_shrink(A, qpng__take(&out_), cap, *outn);
    return QPNG_OK;
}

int qpng_backend_available(qpng_backend b) { return b == QPNG_BACKEND_AUTO || qpng__backend_available((int)b); }
