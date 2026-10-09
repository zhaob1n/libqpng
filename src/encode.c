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
    struct libdeflate_compressor *c = libdeflate_alloc_compressor(LIBDEFLATE_PNG_FAST_LEVEL);
    if (c && level == PF_DEFLATE_PNGFAST_RLE) libdeflate_set_png_min_match(c, 0);
    if (c && level == PF_DEFLATE_PNGFAST_HIGH) libdeflate_set_png_params(c, 0, 9);
    return c;
}

/*
 * Presets (measured choices; see NOTES.md and README "Presets"):
 *  FASTEST  Up filter + run-only png-fast deflate.
 *  FAST     adaptive filter + png-fast deflate.
 *  DEFAULT  probe: adaptive + png-fast.
 *           photo-like (ratio < GRAPHICS_RATIO): keep, except palette and 16-bit, which also try libdeflate
 *             level 6 and keep the smaller stream;
 *           graphics-like: also try filter None (png-fast), keep the better filter and recompress it with
 *             level 6 (level 7 for alpha / 16-bit content).
 *  BEST     photo-like: level 10 (near-optimal parsing); graphics-like: best filter at level 9.
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
    case PF_PRESET_FASTEST: filter = PF_FILTER_UP; level = PF_DEFLATE_PNGFAST_RLE; break;
    case PF_PRESET_FAST: case PF_PRESET_DEFAULT: filter = PF_FILTER_ADAPTIVE; level = PF_DEFLATE_PNGFAST; break;
    case PF_PRESET_BEST: filter = PF_FILTER_ADAPTIVE; level = 10; break;
    default: return PF_ERR_ARGUMENT;
    }
    if (opts->filter >= 0) { if (opts->filter > PF_FILTER_ADAPTIVE) return PF_ERR_ARGUMENT; filter = opts->filter; }
    if (opts->deflate_level) level = opts->deflate_level;
    if (!((level >= 1 && level <= 12) || level == PF_DEFLATE_PNGFAST || level == PF_DEFLATE_PNGFAST_RLE || level == PF_DEFLATE_PNGFAST_HIGH))
        return PF_ERR_ARGUMENT;

    uint8_t *filt = pf__alloc(A, rawn + 64);
    if (!filt) return PF_ERR_NOMEM;
    pf__filter_image(filt, pixels, stride, h, rowbytes, bpp, filter);

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
    if (!probe) {
        zn = zcompress(level, filt, rawn, z, zcap, &st);
    } else {
        /* probe with png-fast; photo-like data under DEFAULT is mostly finished after this step */
        const int best = opts->preset == PF_PRESET_BEST;
        const int heavy = depth == 16 || color == PF_COLOR_GRAY_ALPHA || color == PF_COLOR_RGBA;
        zn = zcompress(PF_DEFLATE_PNGFAST, filt, rawn, z, zcap, &st);
        if (zn && (double)rawn >= (double)zn * GRAPHICS_RATIO) {
            /* graphics-like: try filter None too (often much better for synthetic content) */
            uint8_t *alt = pf__alloc(A, rawn + 64), *tmp = pf__alloc(A, zcap);
            if (!alt || !tmp) st = PF_ERR_NOMEM, zn = 0;
            else {
                pf__filter_image(alt, pixels, stride, h, rowbytes, bpp, PF_FILTER_NONE);
                size_t an = zcompress(PF_DEFLATE_PNGFAST, alt, rawn, tmp, zcap, &st);
                if (an && an < zn) { uint8_t *t = filt; filt = alt; alt = t; }
                zn = an ? zcompress(best ? 9 : heavy ? 7 : 6, filt, rawn, z, zcap, &st) : 0;
            }
            pf__free(A, alt); pf__free(A, tmp);
        } else if (zn && best) {
            zn = zcompress(level, filt, rawn, z, zcap, &st);
        } else if (zn && (depth == 16 || color == PF_COLOR_PALETTE)) {
            /* the png-fast parser is weakest on 16-bit and palette photos: keep the better of it and level 6 */
            uint8_t *tmp = pf__alloc(A, zcap);
            if (!tmp) st = PF_ERR_NOMEM, zn = 0;
            else {
                size_t an = zcompress(6, filt, rawn, tmp, zcap, &st);
                if (an && an < zn) { memcpy(z, tmp, an); zn = an; }
                if (!an) zn = 0;
            }
            pf__free(A, tmp);
        }
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
