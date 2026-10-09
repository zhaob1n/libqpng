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

static struct libdeflate_compressor *make_compressor(int level, int color, int depth) {
    if (level >= PF_DEFLATE_PNGFAST) {
        struct libdeflate_compressor *c = libdeflate_alloc_compressor(LIBDEFLATE_PNG_FAST_LEVEL);
        if (!c) return NULL;
        if (level == PF_DEFLATE_PNGFAST_RLE) libdeflate_set_png_min_match(c, 0);
        else {
            if (level == PF_DEFLATE_PNGFAST_HIGH) libdeflate_set_png_params(c, 0, 9);
            /* unfiltered palette / low-depth data repeats in short strings: allow (cost-gated) 4/5-byte matches */
            if (color == PF_COLOR_PALETTE || depth < 8) libdeflate_set_png_min_match(c, depth == 8 ? 4 : 5);
        }
        return c;
    }
    return libdeflate_alloc_compressor(level);
}

/* Preset -> (filter, deflate level). See README "Presets" for the measurements behind these choices. */
static void preset_params(pf_preset preset, int color, int depth, int *filter, int *level) {
    (void)color; (void)depth;
    switch (preset) {
    case PF_PRESET_FASTEST: *filter = PF_FILTER_UP; *level = PF_DEFLATE_PNGFAST_RLE; break;
    case PF_PRESET_FAST: *filter = PF_FILTER_ADAPTIVE; *level = PF_DEFLATE_PNGFAST; break;
    case PF_PRESET_BEST: *filter = PF_FILTER_ADAPTIVE; *level = 9; break;
    default: *filter = PF_FILTER_ADAPTIVE; *level = PF_DEFLATE_PNGFAST; break;
    }
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

    int filter, level;
    preset_params(opts->preset, color, depth, &filter, &level);
    if (opts->filter >= 0) { if (opts->filter > PF_FILTER_ADAPTIVE) return PF_ERR_ARGUMENT; filter = opts->filter; }
    if (opts->deflate_level) level = opts->deflate_level;
    if (!((level >= 1 && level <= 12) || level == PF_DEFLATE_PNGFAST || level == PF_DEFLATE_PNGFAST_RLE || level == PF_DEFLATE_PNGFAST_HIGH))
        return PF_ERR_ARGUMENT;

    uint8_t *filt = pf__alloc(A, rawn + 64);
    if (!filt) return PF_ERR_NOMEM;
    pf__filter_image(filt, pixels, stride, h, rowbytes, bpp, filter);

    struct libdeflate_compressor *c = make_compressor(level, color, depth);
    if (!c) { pf__free(A, filt); return PF_ERR_NOMEM; }
    const size_t zcap = libdeflate_zlib_compress_bound(c, rawn);
    const size_t nchunks = zcap / 0x40000000u + 1;   /* IDAT chunks are split at 1 GiB */
    const size_t cap = 8 + 25 + (opts->palette ? 12 + 3 * (size_t)opts->palette_entries : 0) +
                       (opts->trns ? 12 + (size_t)opts->trns_bytes : 0) + 12 * nchunks + zcap + 12;
    uint8_t *out = pf__alloc(A, cap);
    if (!out) { libdeflate_free_compressor(c); pf__free(A, filt); return PF_ERR_NOMEM; }
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
    size_t zn = libdeflate_zlib_compress(c, filt, rawn, z, zcap);
    libdeflate_free_compressor(c);
    pf__free(A, filt);
    if (!zn) { pf__free(A, out); return PF_ERR_ENCODE; }
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
