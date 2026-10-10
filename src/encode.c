/* encode.c - PNG encoder: validation, presets, filtering, deflate (vendored libdeflate), chunk writing. */
#include "internal.h"

void pf_encode_options_init(pf_encode_options *o) {
    memset(o, 0, sizeof *o);
    o->preset = PF_PRESET_DEFAULT; o->filter = PF_FILTER_AUTO;
}

void pf_buffer_free(void *buf, const pf_allocator *a) { pf__free(a, buf); }

static uint8_t *put_chunk(uint8_t *p, const char *type, const uint8_t *data, uint32_t len) {
    wbe32(p, len); memcpy(p + 4, type, 4);
    if (data && len) memcpy(p + 8, data, len);
    wbe32(p + 8 + len, libdeflate_crc32(0, p + 4, len + 4));
    return p + 12 + len;
}

static struct libdeflate_compressor *make_compressor(int level) {
    if (level < PF_DEFLATE_PNGFAST) return libdeflate_alloc_compressor(level);
    if (level == PF_DEFLATE_PNGULTRA) return libdeflate_alloc_compressor(LIBDEFLATE_PNG_ULTRA_LEVEL);
    struct libdeflate_compressor *c = libdeflate_alloc_compressor(LIBDEFLATE_PNG_FAST_LEVEL);
    if (c && level == PF_DEFLATE_PNGFAST_RLE) libdeflate_set_png_min_match(c, 0);
    if (c && level == PF_DEFLATE_PNGFAST_HIGH) libdeflate_set_png_params(c, 0, 9);
    return c;
}

/*
 * Presets (measured choices; see NOTES.md and README "Presets"):
 *  FASTEST  Up filter + run-only png-fast deflate.
 *  FAST     adaptive filter + png-fast deflate (hashing adapts per block).
 *  DEFAULT  1. probe: adaptive + png-fast; ratio >= GRAPHICS_RATIO means graphics-like content.
 *           2. filter trial with png-fast: graphics -> also filter None, 8-bit palette -> also Paeth.
 *           3. graphics: recompress the winner at level 6 (7 for alpha / 16-bit);
 *              16-bit photos: also level 6, keep the smaller; other photos: keep the png-fast stream.
 *  BEST     steps 1-2, then level 9 (graphics) or 10 (photos, near-optimal parsing).
 * An explicit filter or deflate_level in pf_encode_options disables the probing.
 */
#define GRAPHICS_RATIO 6

static size_t zcompress(int level, const uint8_t *src, size_t n, uint8_t *dst, size_t cap,
                        pf_status *st) {
    struct libdeflate_compressor *c = make_compressor(level);
    if (!c) { *st = PF_ERR_NOMEM; return 0; }
    size_t zn = libdeflate_zlib_compress(c, src, n, dst, cap);
    libdeflate_free_compressor(c);
    if (!zn) *st = PF_ERR_ENCODE;
    return zn;
}

/*
 * png-ultra streaming: rows are filtered into a small (L2-sized) buffer and compressed chunk by chunk, so the
 * filtered image is never materialised. Chunks are ~64 KiB, 256 KiB once the data compresses >= 4:1.
 * hybrid: chunks following a >= 4:1 chunk (graphics-like) use the Up filter instead of 'filter' (FAST preset).
 * Writes a zlib stream to dst; returns its size or 0.
 */
#define ULTRA_CHUNK      65536
#define ULTRA_CHUNK_BIG  262144
#define ULTRA_MIN_TAIL   4096
static size_t zcompress_ultra(const uint8_t *px, size_t stride, uint32_t h, size_t n, int bpp, int filter,
                              int hybrid, uint8_t *dst, size_t cap, const pf_allocator *A, pf_status *st) {
    const size_t row = n + 1;
    const size_t max_rows = (ULTRA_CHUNK_BIG + ULTRA_MIN_TAIL) / row + 2;
    struct libdeflate_compressor *c = make_compressor(PF_DEFLATE_PNGULTRA);
    uint8_t *buf = pf__alloc(A, max_rows * row + 1 + 64);
    struct libdeflate_png_ultra_state us = { 0, 0, 0 };
    uint32_t adler = 1;
    size_t zn = 2, target = ULTRA_CHUNK;
    if (!c || !buf || cap < 6) { *st = c && buf ? PF_ERR_ENCODE : PF_ERR_NOMEM; zn = 0; goto done; }
    dst[0] = 0x78; dst[1] = 0x01;
    for (uint32_t y = 0; y < h;) {
        uint32_t k = (uint32_t)(target / row ? target / row : 1);
        if (k > h - y) k = h - y;
        if ((size_t)(h - y - k) * row < ULTRA_MIN_TAIL && (size_t)(h - y) <= max_rows) k = h - y;   /* no tiny tail */
        const size_t len = (size_t)k * row;
        pf__filter_rows(buf + 1, px, stride, y, y + k, n, bpp, hybrid && target == ULTRA_CHUNK_BIG ? PF_FILTER_UP : filter);
        adler = libdeflate_adler32(adler, buf + 1, len);
        y += k;
        size_t o = libdeflate_png_ultra_compress_chunk(c, &us, buf + 1, len, y == h, dst + zn, cap - zn - 4);
        if (!o) { *st = PF_ERR_ENCODE; zn = 0; goto done; }
        zn += o;
        buf[0] = buf[len];   /* the parser may look one byte back */
        target = o * 4 < len ? ULTRA_CHUNK_BIG : ULTRA_CHUNK;
    }
    wbe32(dst + zn, adler); zn += 4;
done:
    pf__free(A, buf);
    libdeflate_free_compressor(c);
    return zn;
}

pf_status pf_encode(const void *pixels, uint32_t w, uint32_t h, size_t stride, pf_color color, int depth,
                    const pf_encode_options *opts, uint8_t **outp, size_t *outn) {
    pf_encode_options defo;
    if (!outp || !outn) return PF_ERR_ARGUMENT;
    *outp = NULL; *outn = 0;
    if (!opts) { pf_encode_options_init(&defo); opts = &defo; }
    const pf_allocator *A = opts->allocator;
    int ch;
    switch (color) {
    case PF_COLOR_GRAY: ch = 1; if (depth != 1 && depth != 2 && depth != 4 && depth != 8 && depth != 16) return PF_ERR_ARGUMENT; break;
    case PF_COLOR_PALETTE: ch = 1; if (depth != 1 && depth != 2 && depth != 4 && depth != 8) return PF_ERR_ARGUMENT; break;
    case PF_COLOR_RGB: ch = 3; goto d816;
    case PF_COLOR_GRAY_ALPHA: ch = 2; goto d816;
    case PF_COLOR_RGBA: ch = 4;
    d816: if (depth != 8 && depth != 16) return PF_ERR_ARGUMENT; break;
    default: return PF_ERR_ARGUMENT;
    }
    if (!pixels || !w || !h || w > 0x7FFFFFFFu || h > 0x7FFFFFFFu) return PF_ERR_ARGUMENT;
    if (color == PF_COLOR_PALETTE && (!opts->palette || opts->palette_entries < 1 || opts->palette_entries > 256))
        return PF_ERR_ARGUMENT;
    if (opts->palette && (opts->palette_entries < 1 || opts->palette_entries > 256)) return PF_ERR_ARGUMENT;
    if (opts->trns && (opts->trns_bytes < 1 || color == PF_COLOR_GRAY_ALPHA || color == PF_COLOR_RGBA ||
                       (color == PF_COLOR_GRAY && opts->trns_bytes != 2) || (color == PF_COLOR_RGB && opts->trns_bytes != 6) ||
                       (color == PF_COLOR_PALETTE && opts->trns_bytes > opts->palette_entries)))
        return PF_ERR_ARGUMENT;
    const int bits = ch * depth, bpp = bits >= 8 ? bits / 8 : 1;
    const size_t rowbytes = ((size_t)w * bits + 7) / 8;
    if (!stride) stride = rowbytes;
    if (stride < rowbytes) return PF_ERR_ARGUMENT;
    if ((uint64_t)h * (rowbytes + 1) > ((uint64_t)1 << 40)) return PF_ERR_LIMITS;
    const size_t rawn = (size_t)h * (rowbytes + 1);

    const int probe = opts->filter < 0 && !opts->deflate_level &&
                      (opts->preset == PF_PRESET_DEFAULT || opts->preset == PF_PRESET_BEST);
    int filter, level;
    switch (opts->preset) {
    case PF_PRESET_FASTEST: filter = PF_FILTER_UP; level = PF_DEFLATE_PNGULTRA; break;
    case PF_PRESET_FAST: filter = PF_FILTER_ADAPTIVE; level = PF_DEFLATE_PNGULTRA; break;
    case PF_PRESET_DEFAULT: filter = PF_FILTER_ADAPTIVE; level = PF_DEFLATE_PNGFAST; break;
    case PF_PRESET_BEST: filter = PF_FILTER_ADAPTIVE; level = 10; break;
    default: return PF_ERR_ARGUMENT;
    }
    if (opts->filter >= 0) { if (opts->filter > PF_FILTER_ADAPTIVE) return PF_ERR_ARGUMENT; filter = opts->filter; }
    if (opts->deflate_level) level = opts->deflate_level;
    if (!((level >= 1 && level <= 12) || level == PF_DEFLATE_PNGFAST || level == PF_DEFLATE_PNGFAST_RLE || level == PF_DEFLATE_PNGFAST_HIGH ||
          level == PF_DEFLATE_PNGULTRA))
        return PF_ERR_ARGUMENT;

    const int stream = !probe && level == PF_DEFLATE_PNGULTRA;
    uint8_t *filt = NULL;
    if (!stream) {
        filt = pf__alloc(A, rawn + 64);
        if (!filt) return PF_ERR_NOMEM;
        pf__filter_image(filt, pixels, stride, h, rowbytes, bpp, filter);
    }

    /* worst-case bound over all compression levels */
    const size_t zcap = libdeflate_zlib_compress_bound(NULL, rawn);
    const size_t nchunks = zcap / 0x40000000u + 1;   /* IDAT chunks are split at 1 GiB */
    const size_t cap = 8 + 25 + (opts->palette ? 12 + 3 * (size_t)opts->palette_entries : 0) +
                       (opts->trns ? 12 + (size_t)opts->trns_bytes : 0) + 12 * nchunks + zcap + 12;
    uint8_t *out = pf__alloc(A, cap);
    if (!out) { pf__free(A, filt); return PF_ERR_NOMEM; }
    static const uint8_t sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    uint8_t *p = out;
    memcpy(p, sig, 8); p += 8;
    uint8_t ihdr[13];
    wbe32(ihdr, w); wbe32(ihdr + 4, h); ihdr[8] = (uint8_t)depth; ihdr[9] = (uint8_t)color; ihdr[10] = ihdr[11] = ihdr[12] = 0;
    p = put_chunk(p, "IHDR", ihdr, 13);
    if (opts->palette) p = put_chunk(p, "PLTE", opts->palette, 3u * (uint32_t)opts->palette_entries);
    if (opts->trns) p = put_chunk(p, "tRNS", opts->trns, (uint32_t)opts->trns_bytes);
    /* single IDAT (bound < 1 GiB): compress in place behind the IDAT header. Otherwise compress to the end of the
     * buffer and lay the chunks out in front of the data. */
    uint8_t *z = nchunks == 1 ? p + 8 : out + cap - zcap - 12;
    pf_status st = PF_OK;
    size_t zn;
    if (stream) {
        zn = zcompress_ultra(pixels, stride, h, rowbytes, bpp, filter, opts->filter < 0 && !opts->deflate_level, z, zcap, A,
                             &st);
    } else if (!probe) {
        zn = zcompress(level, filt, rawn, z, zcap, &st);
    } else {
        /* 1. probe: adaptive filter + png-fast, written straight to the output */
        const int best = opts->preset == PF_PRESET_BEST;
        const int heavy = depth == 16 || color == PF_COLOR_GRAY_ALPHA || color == PF_COLOR_RGBA;
        zn = zcompress(PF_DEFLATE_PNGFAST, filt, rawn, z, zcap, &st);
        const int graphics = zn && (double)rawn >= (double)zn * GRAPHICS_RATIO;
        /* 2. filter trial where the residual-sum heuristic is known to mislead: synthetic content (try None)
         *    and 8-bit palette indices (try Paeth everywhere). The png-fast size decides. */
        const int alt_filter = graphics ? PF_FILTER_NONE
                             : (color == PF_COLOR_PALETTE && depth == 8) ? PF_FILTER_PAETH : -1;
        uint8_t *alt = NULL, *tmp = NULL;
        if (zn && (alt_filter >= 0 || best || depth == 16)) {
            tmp = pf__alloc(A, zcap);
            if (!tmp) { st = PF_ERR_NOMEM; zn = 0; }
        }
        if (zn && alt_filter >= 0) {
            alt = pf__alloc(A, rawn + 64);
            if (!alt) { st = PF_ERR_NOMEM; zn = 0; }
            else {
                pf__filter_image(alt, pixels, stride, h, rowbytes, bpp, alt_filter);
                size_t an = zcompress(PF_DEFLATE_PNGFAST, alt, rawn, tmp, zcap, &st);
                if (!an) zn = 0;
                else if (an < zn) {
                    uint8_t *t = filt; filt = alt; alt = t;
                    memcpy(z, tmp, an); zn = an;
                }
            }
        }
        /* 3. final compression of the chosen filtering */
        if (zn && graphics && !best) {
            zn = zcompress(heavy ? 7 : 6, filt, rawn, z, zcap, &st);
        } else if (zn && (best || depth == 16)) {
            /* BEST: level 9 (graphics) / 10 (photos). DEFAULT 16-bit photos: level 6, png-fast is weakest
             * there (noisy low bytes). Either way the png-fast stream already in z is kept if smaller. */
            size_t an = zcompress(best ? (graphics ? 9 : 10) : 6, filt, rawn, tmp, zcap, &st);
            if (!an) zn = 0;
            else if (an < zn) { memcpy(z, tmp, an); zn = an; }
        }
        pf__free(A, alt); pf__free(A, tmp);
    }
    pf__free(A, filt);
    if (!zn) { pf__free(A, out); return st != PF_OK ? st : PF_ERR_ENCODE; }
    for (size_t done = 0; done < zn;) {
        uint32_t len = (uint32_t)((zn - done) < 0x40000000u ? zn - done : 0x40000000u);
        if (p + 8 != z + done) memmove(p + 8, z + done, len);   /* p + 8 <= z + done always holds: chunks only add 12-byte overhead */
        p = put_chunk(p, "IDAT", NULL, len);
        done += len;
    }
    p = put_chunk(p, "IEND", NULL, 0);
    *outn = (size_t)(p - out);
    *outp = pf__realloc_shrink(A, out, cap, *outn);
    return PF_OK;
}
