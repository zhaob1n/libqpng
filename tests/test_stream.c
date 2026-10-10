/* test_stream.c - streaming decode with random chunk boundaries (image mode, row mode, pull reader) must equal
 * the one-shot decode in every output mode; streaming encode (every preset, each backend, random row batches)
 * must round trip; error paths (truncation, CRC, callbacks, state). Usage: test_stream files... */
#include "pngfast.h"
#include "test_util.h"
static int fails;
static uint32_t rng = 12345;
static uint32_t rnd(void) { rng = rng * 1103515245u + 12345u; return rng >> 8; }

typedef struct { uint8_t *buf; size_t stride, size; uint32_t next; int bad, abort_at; } rows_t;
static int on_row(void *u, uint32_t y, const uint8_t *row, size_t n) {
    rows_t *r = u;
    if (r->abort_at >= 0 && (int)y == r->abort_at) return 1;
    if (y != r->next || n != r->stride || (size_t)(y + 1) * n > r->size) { r->bad = 1; return 0; }
    memcpy(r->buf + (size_t)y * n, row, n); r->next++;
    return 0;
}
typedef struct { const uint8_t *p; size_t n, pos; } reader_t;
static size_t rd(void *u, uint8_t *b, size_t cap) {
    reader_t *r = u; size_t k = 1 + rnd() % 5000; if (k > cap) k = cap; if (k > r->n - r->pos) k = r->n - r->pos;
    memcpy(b, r->p + r->pos, k); r->pos += k; return k;
}

static pf_status push_random(pf_decoder *d, const uint8_t *b, size_t n) {
    pf_status s = PF_MORE_DATA;
    for (size_t pos = 0; pos < n && s == PF_MORE_DATA;) {
        size_t k = rnd() % 4 == 0 ? 1 : 1 + rnd() % (rnd() % 2 ? 64 : 20000);
        if (k > n - pos) k = n - pos;
        s = pf_decoder_push(d, b + pos, k); pos += k;
    }
    return s;
}

static void decode_file(const uint8_t *b, size_t n, const char *name) {
    for (int mode = 0; mode < 3; mode++) {
        pf_decode_options o; pf_decode_options_init(&o); o.output = (pf_output)mode; o.verify_adler = mode == 2;
        pf_image ref;
        pf_status rs = pf_decode(b, n, &o, &ref);
        /* image mode */
        pf_decoder *d; CHECK(pf_decoder_new(&o, NULL, NULL, &d) == PF_OK);
        pf_status s = push_random(d, b, n);
        pf_image im;
        if (s == PF_OK) s = pf_decoder_finish(d, &im);
        if (rs == PF_OK) {
            if (s != PF_OK || im.size != ref.size || memcmp(im.pixels, ref.pixels, ref.size)) {
                fprintf(stderr, "image-mode mismatch %s mode %d: %s\n", name, mode, pf_status_string(s)); fails++;
            }
            if (s == PF_OK) pf_image_free(&im);
        } else CHECK(s != PF_OK);
        pf_decoder_free(d);
        /* row mode */
        rows_t r; memset(&r, 0, sizeof r); r.abort_at = -1;
        if (rs == PF_OK) { r.stride = ref.stride; r.size = ref.size; r.buf = malloc(ref.size + 1); }
        CHECK(pf_decoder_new(&o, on_row, &r, &d) == PF_OK);
        s = push_random(d, b, n);
        pf_info info;
        if (s == PF_OK) { CHECK(pf_decoder_info(d, &info) == PF_OK); s = pf_decoder_finish(d, NULL); }
        if (rs == PF_OK) {
            if (s != PF_OK || r.bad || r.next != ref.height || memcmp(r.buf, ref.pixels, ref.size)) {
                fprintf(stderr, "row-mode mismatch %s mode %d: %s (rows %u)\n", name, mode, pf_status_string(s), r.next); fails++;
            }
        } else CHECK(s != PF_OK);
        pf_decoder_free(d);
        /* pull reader */
        if (rs == PF_OK) {
            reader_t rr = { b, n, 0 }; pf_image im2;
            s = pf_decode_stream(rd, &rr, &o, NULL, NULL, &im2);
            CHECK(s == PF_OK && im2.size == ref.size && !memcmp(im2.pixels, ref.pixels, ref.size));
            if (s == PF_OK) pf_image_free(&im2);
        }
        free(r.buf);
        if (rs == PF_OK) pf_image_free(&ref);
    }
}

typedef struct { uint8_t *p; size_t n, cap; int fail_after; } sink_t;
static int wr(void *u, const uint8_t *d, size_t n) {
    sink_t *s = u;
    if (s->fail_after >= 0 && s->n + n > (size_t)s->fail_after) return 1;
    if (s->n + n > s->cap) { s->cap = (s->n + n) * 2; s->p = realloc(s->p, s->cap); }
    memcpy(s->p + s->n, d, n); s->n += n; return 0;
}

static void encode_file(const uint8_t *b, size_t n, const char *name) {
    pf_image src;
    if (pf_decode(b, n, NULL, &src) != PF_OK) return;
    int pl = 0, tl = 0; pf_encode_options o; pf_encode_options_init(&o);
    o.palette = find_chunk(b, n, "PLTE", &pl); o.palette_entries = o.palette ? pl / 3 : 0;
    if (src.color_type != PF_COLOR_PALETTE) { o.palette = NULL; o.palette_entries = 0; }
    o.trns = find_chunk(b, n, "tRNS", &tl); o.trns_bytes = o.trns ? tl : 0;
    if (o.trns && ((src.color_type == PF_COLOR_GRAY && tl != 2) || (src.color_type == PF_COLOR_RGB && tl != 6) ||
                   (src.color_type == PF_COLOR_PALETTE && tl > o.palette_entries) || src.color_type >= 4)) { o.trns = NULL; o.trns_bytes = 0; }
    for (int v = 0; v < 4 * 3; v++) {
        pf_encode_options e = o; e.preset = (pf_preset)(v % 4); e.backend = v / 4;
        if (!pf_backend_available((pf_backend)e.backend)) continue;
        sink_t sk = { NULL, 0, 0, -1 };
        pf_encoder *enc;
        pf_status s = pf_encoder_new(src.width, src.height, (pf_color)src.color_type, src.bit_depth, &e, wr, &sk, &enc);
        if (s) { fprintf(stderr, "encoder_new %s v%d: %s\n", name, v, pf_status_string(s)); fails++; continue; }
        for (uint32_t y = 0; y < src.height && !s;) {
            uint32_t k = 1 + rnd() % 70; if (k > src.height - y) k = src.height - y;
            s = pf_encoder_push_rows(enc, src.pixels + (size_t)y * src.stride, src.stride, k); y += k;
        }
        if (!s) s = pf_encoder_finish(enc);
        CHECK(pf_encoder_push_rows(enc, src.pixels, 0, 1) == PF_ERR_STATE || s);
        pf_encoder_free(enc);
        pf_image back;
        if (s || pf_decode(sk.p, sk.n, NULL, &back) != PF_OK) { fprintf(stderr, "stream encode %s v%d: %s\n", name, v, pf_status_string(s)); fails++; free(sk.p); continue; }
        if (back.size != src.size || memcmp(back.pixels, src.pixels, src.size)) { fprintf(stderr, "stream encode mismatch %s v%d\n", name, v); fails++; }
        pf_image_free(&back); free(sk.p);
    }
    pf_image_free(&src);
}

static void errors(const uint8_t *b, size_t n) {
    pf_decoder *d; pf_image im;
    /* truncated: finish before IEND */
    CHECK(pf_decoder_new(NULL, NULL, NULL, &d) == PF_OK);
    CHECK(pf_decoder_push(d, b, n / 2) == PF_MORE_DATA);
    CHECK(pf_decoder_finish(d, &im) == PF_ERR_STATE);
    pf_decoder_free(d);
    /* header first */
    CHECK(pf_decoder_new(NULL, NULL, NULL, &d) == PF_OK);
    pf_info info;
    CHECK(pf_decoder_push(d, b, 20) == PF_MORE_DATA && pf_decoder_info(d, &info) == PF_MORE_DATA);
    CHECK(pf_decoder_push(d, b + 20, 20) == PF_MORE_DATA && pf_decoder_info(d, &info) == PF_OK && info.width > 0);
    pf_decoder_free(d);
    /* bad CRC (sticky) */
    uint8_t *c = malloc(n); memcpy(c, b, n); c[n - 20] ^= 0x40;
    CHECK(pf_decoder_new(NULL, NULL, NULL, &d) == PF_OK);
    pf_status s = pf_decoder_push(d, c, n);
    CHECK(s == PF_ERR_CRC || s == PF_ERR_DEFLATE);
    CHECK(pf_decoder_push(d, c, 1) == s);
    pf_decoder_free(d); free(c);
    /* row callback abort */
    pf_decode_options o; pf_decode_options_init(&o);
    CHECK(pf_decode(b, n, &o, &im) == PF_OK);
    rows_t r; memset(&r, 0, sizeof r); r.stride = im.stride; r.size = im.size; r.buf = malloc(im.size); r.abort_at = 1;
    CHECK(pf_decoder_new(&o, on_row, &r, &d) == PF_OK);
    s = pf_decoder_push(d, b, n); if (s == PF_OK) s = pf_decoder_finish(d, NULL);
    CHECK(s == PF_ERR_ABORTED);
    pf_decoder_free(d); free(r.buf);
    /* write callback abort, too many rows */
    sink_t sk = { NULL, 0, 0, 100 }; pf_encoder *e;
    CHECK(pf_encoder_new(im.width, im.height, (pf_color)im.color_type, im.bit_depth, NULL, wr, &sk, &e) == PF_OK);
    s = pf_encoder_push_rows(e, im.pixels, im.stride, im.height); if (!s) s = pf_encoder_finish(e);
    CHECK(s == PF_ERR_ABORTED);
    pf_encoder_free(e); free(sk.p);
    sk.fail_after = -1; sk.p = NULL; sk.n = sk.cap = 0;
    CHECK(pf_encoder_new(im.width, im.height, (pf_color)im.color_type, im.bit_depth, NULL, wr, &sk, &e) == PF_OK);
    CHECK(pf_encoder_finish(e) == PF_ERR_STATE);
    CHECK(pf_encoder_push_rows(e, im.pixels, im.stride, im.height + 1) == PF_ERR_STATE);
    pf_encoder_free(e); free(sk.p);
    pf_encode_options eo; pf_encode_options_init(&eo); eo.deflate_level = 6; eo.backend = PF_BACKEND_LIBDEFLATE;
    CHECK(pf_encoder_new(im.width, im.height, (pf_color)im.color_type, im.bit_depth, &eo, wr, &sk, &e) == PF_ERR_UNSUPPORTED);
    pf_image_free(&im);
}

int main(int argc, char **argv) {
    int files = 0;
    for (int i = 1; i < argc; i++) {
        size_t n; uint8_t *b = slurp(argv[i], &n);
        if (!b) { fprintf(stderr, "missing %s\n", argv[i]); fails++; continue; }
        decode_file(b, n, argv[i]);
        encode_file(b, n, argv[i]);
        if (i == 1) errors(b, n);
        free(b); files++;
    }
    printf("stream: %d files, %d failures\n", files, fails);
    return fails != 0;
}
