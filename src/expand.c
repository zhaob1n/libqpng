/* expand.c - per-row output transforms applied right after unfiltering (rows still hot in cache):
 * palette -> RGB8/RGBA8 (LUT), gray 1/2/4 -> 8-bit scaled (+alpha) via a per-packed-byte LUT, tRNS -> alpha for
 * gray/RGB 8/16, 16-bit big-endian -> native. Semantics follow the png crate's Transformations::EXPAND exactly
 * (including its gray tRNS low-byte compare and black/opaque out-of-range palette indices). */
#include "internal.h"

void pf__xform_setup(pf_xform *c, int color, int depth, int ich, uint32_t W, pf_output output,
                     const uint8_t *plte, uint32_t plen, const uint8_t *trns, uint32_t tlen,
                     int *och_, int *odepth_, int *ocolor_) {
    const int expand = output != PF_OUTPUT_RAW, native16 = output == PF_OUTPUT_IMAGERS && depth == 16;
    memset(c, 0, sizeof *c);
    c->depth = depth; c->ich = ich; c->W = W; c->native16 = native16;
    int och = ich, odepth = depth, ocolor = color;
    if (expand && color == 3) {
        c->xf = XF_PAL; c->alpha = !!trns; och = c->alpha ? 4 : 3; odepth = 8; ocolor = c->alpha ? 6 : 2;
        for (int i = 0; i < 256; i++) c->lut[i] = 0xFF000000u;
        for (uint32_t i = 0; i < plen; i++) c->lut[i] = 0xFF000000u | plte[3 * i] | plte[3 * i + 1] << 8 | (uint32_t)plte[3 * i + 2] << 16;
        if (trns && tlen <= plen) for (uint32_t i = 0; i < tlen; i++) c->lut[i] = (c->lut[i] & 0x00FFFFFFu) | (uint32_t)trns[i] << 24;
    } else if (expand && color == 0 && depth < 8) {
        c->xf = XF_GRAY; c->alpha = !!trns; c->scale = 255 / ((1 << depth) - 1); och = c->alpha ? 2 : 1; odepth = 8; ocolor = c->alpha ? 4 : 0;
        if (trns) c->trns[0] = trns[1];
        for (int b = 0; b < 256; b++) for (int k = 0; k < 8 / depth; k++) {
            uint32_t v = (uint32_t)(b >> (8 - depth * (k + 1))) & ((1u << depth) - 1);
            if (c->alpha) { c->glut[b][2 * k] = (uint8_t)(v * c->scale); c->glut[b][2 * k + 1] = v == c->trns[0] ? 0 : 255; }
            else c->glut[b][k] = (uint8_t)(v * c->scale);
        }
    } else if (expand && trns && (color == 0 || color == 2)) {
        och = ich + 1; ocolor = color == 0 ? 4 : 6;
        if (depth == 8) { c->xf = XF_TRNS8; for (int k = 0; k < ich; k++) c->trns[k] = trns[2 * k + 1]; }
        else { c->xf = XF_TRNS16; memcpy(c->trns, trns, 2 * ich); }
    } else if (native16) c->xf = XF_SWAP16;
    *och_ = och; *odepth_ = odepth; *ocolor_ = ocolor;
}

/* transform one unfiltered row of w pixels (input format) into output pixels at o */
void pf__xform_row(const pf_xform *c, const uint8_t *s, uint8_t *o, uint32_t w) {
    const int d = c->depth;
    switch (c->xf) {
    case XF_PAL:
        if (c->alpha) {
            if (d == 8) for (uint32_t i = 0; i < w; i++) st32(o + 4 * i, c->lut[s[i]]);
            else if (d == 4) for (uint32_t i = 0; i < w; i++) st32(o + 4 * i, c->lut[pf__unpack(s, i, 4)]);
            else if (d == 2) for (uint32_t i = 0; i < w; i++) st32(o + 4 * i, c->lut[pf__unpack(s, i, 2)]);
            else for (uint32_t i = 0; i < w; i++) st32(o + 4 * i, c->lut[pf__unpack(s, i, 1)]);
        } else {
            uint32_t i = 0;
            if (d == 8) { for (; i + 1 < w; i++) st32(o + 3 * i, c->lut[s[i]]); }
            else if (d == 4) { for (; i + 1 < w; i++) st32(o + 3 * i, c->lut[pf__unpack(s, i, 4)]); }
            else if (d == 2) { for (; i + 1 < w; i++) st32(o + 3 * i, c->lut[pf__unpack(s, i, 2)]); }
            else { for (; i + 1 < w; i++) st32(o + 3 * i, c->lut[pf__unpack(s, i, 1)]); }
            if (i < w) { uint32_t v = c->lut[d == 8 ? s[i] : pf__unpack(s, i, d)]; memcpy(o + 3 * i, &v, 3); }
        }
        break;
    case XF_GRAY:   /* 1/2/4-bit gray -> 8-bit (scaled), optionally + alpha */
    {   /* one table lookup per packed input byte; the last (partial) byte is done per pixel */
        const uint32_t spb = 8 / d, full = w / spb, ob = spb * (c->alpha ? 2 : 1);
#define GL(N) for (uint32_t j = 0; j < full; j++) memcpy(o + (size_t)j * N, c->glut[s[j]], N); break;
        switch (ob) { case 2: GL(2) case 4: GL(4) case 8: GL(8) case 16: GL(16) }
#undef GL
        for (uint32_t i = full * spb; i < w; i++) {
            uint32_t v = pf__unpack(s, i, d);
            if (c->alpha) { o[2 * i] = (uint8_t)(v * c->scale); o[2 * i + 1] = v == c->trns[0] ? 0 : 255; } else o[i] = (uint8_t)(v * c->scale);
        }
        break; }
    case XF_TRNS8: {
        const int ch = c->ich;
        if (ch == 1) for (uint32_t i = 0; i < w; i++) { o[2 * i] = s[i]; o[2 * i + 1] = s[i] == c->trns[0] ? 0 : 255; }
        else for (uint32_t i = 0; i < w; i++) {
            const uint8_t *p = s + 3 * i; uint8_t *q = o + 4 * i;
            q[0] = p[0]; q[1] = p[1]; q[2] = p[2]; q[3] = (p[0] == c->trns[0] && p[1] == c->trns[1] && p[2] == c->trns[2]) ? 0 : 255;
        }
        break; }
    case XF_TRNS16: {
        const int ch = c->ich, sw = c->native16;
        for (uint32_t i = 0; i < w; i++) {
            const uint8_t *p = s + 2 * ch * i; uint8_t *q = o + 2 * (ch + 1) * i;
            int t = !memcmp(p, c->trns, 2 * ch);
            for (int k = 0; k < ch; k++) { q[2 * k] = p[2 * k + sw]; q[2 * k + 1] = p[2 * k + 1 - sw]; }
            q[2 * ch] = q[2 * ch + 1] = t ? 0 : 255;
        }
        break; }
    }
}
void pf__swap16(uint8_t *p, size_t n) {   /* SSE2: (x << 8) | (x >> 8) per 16-bit lane */
    size_t i = 0;
#if PF_X86
    for (; i + 16 <= n; i += 16) {
        __m128i v = _mm_loadu_si128((const __m128i *)(p + i));
        _mm_storeu_si128((__m128i *)(p + i), _mm_or_si128(_mm_slli_epi16(v, 8), _mm_srli_epi16(v, 8)));
    }
#endif
    for (; i + 1 < n; i += 2) { uint8_t t = p[i]; p[i] = p[i + 1]; p[i + 1] = t; }
}
