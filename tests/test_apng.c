/* test_apng.c - APNG: the APNG test suite (tests/data/apng, https://philip.html5.org/tests/apng/tests.html: final
 * composed frame vs the documented expectation, invalid files rejected), encode -> decode round trips with every
 * dispose / blend op against a straightforward reference compositor, raw frames, still images.
 * Usage: test_apng APNG_DIR [still.png...] */
#include "qpng.h"
#include "test_util.h"
static int fails;

static qpng_status last_frame(const uint8_t *b, size_t n, uint8_t **canvas, qpng_apng_info *ai) {
    qpng_apng *a; qpng_status s = qpng_apng_open(b, n, NULL, &a);
    if (s) return s;
    qpng_apng_get_info(a, ai);
    const uint8_t *c = NULL, *t; qpng_frame_info fi; uint32_t k = 0;
    while ((s = qpng_apng_next_frame(a, &t, &fi)) == QPNG_OK) { c = t; k++; }
    if (s == QPNG_ERR_STATE && k == ai->num_frames) s = QPNG_OK;
    if (!s) { size_t sz = (size_t)ai->image.width * ai->image.height * 4; *canvas = malloc(sz); memcpy(*canvas, c, sz); }
    qpng_apng_free(a);
    return s;
}

/* every pixel within tol of rgba (alpha_only: only alpha compared) */
static int all_px(const uint8_t *c, size_t npx, int r, int g, int b, int al, int tol, int alpha_only) {
    for (size_t i = 0; i < npx; i++, c += 4) {
        if (abs(c[3] - al) > tol) return 0;
        if (!alpha_only && (abs(c[0] - r) > tol || abs(c[1] - g) > tol || abs(c[2] - b) > tol)) return 0;
    }
    return 1;
}

static void suite(const char *dir) {
    char path[4096];
    for (int i = 0; i <= 60; i++) {
        snprintf(path, sizeof path, "%s/%03d.png", dir, i);
        size_t n; uint8_t *b = slurp(path, &n);
        if (!b) { fprintf(stderr, "missing %s\n", path); fails++; continue; }
        uint8_t *c = NULL; qpng_apng_info ai;
        qpng_status s = last_frame(b, n, &c, &ai);
        const size_t np = s ? 0 : (size_t)ai.image.width * ai.image.height;
        int ok = 1;
        if (i <= 24 || (i >= 33 && i <= 38)) {   /* valid files: check the final frame */
            ok = s == QPNG_OK;
            if (ok) switch (i) {
            case 8: case 12: case 14: case 18: ok = all_px(c, np, 0, 0, 0, 0, 0, 1); break;
            case 15: ok = c[3] == 255 && c[2] == 255 && c[(np / 2 + ai.image.width / 2) * 4 + 3] == 0; break;
            case 19: ok = all_px(c, np, 0, 0, 0, 0, 8, 1); break;
            case 22: ok = all_px(c, np, 0, 0, 0, 255, 0, 1) && c[1] > 100 && c[1] < 255 && c[0] == 0 && all_px(c, np, c[0], c[1], c[2], 255, 0, 0); break;
            case 23: ok = all_px(c, np, 229, 0, 0, 255, 1, 0); break;   /* 0.9 red over red; "nearly black" only via gAMA 1/2200 (no color management) */
            case 33: case 38: ok = c[0] == 0 && c[1] == 0 && c[2] > 0 && c[2] < 200 && c[3] == 255 && all_px(c, np, c[0], c[1], c[2], 255, 0, 0); break;
            case 34: ok = c[3] == 255 && c[0] == c[1] && c[0] < 255 && c[(np / 2 + ai.image.width / 2) * 4] == 255; break;
            case 35: ok = c[3] == 255 && c[0] == c[1] && c[1] == c[2] && c[0] > 50 && c[0] < 200 && all_px(c, np, c[0], c[1], c[2], 255, 0, 0); break;
            default: ok = all_px(c, np, 0, 255, 0, 255, 0, 0);
            }
        } else if (i >= 25 && i <= 32) {         /* timing files: valid, several frames */
            ok = s == QPNG_OK && ai.num_frames >= 2;
            if (ok && i == 28) {   /* zero denominator is reported as stored */
                qpng_apng *a; qpng_apng_open(b, n, NULL, &a); qpng_image im; qpng_frame_info fi;
                ok = qpng_apng_raw_frame(a, 0, NULL, &im, &fi) == QPNG_OK; qpng_image_free(&im); qpng_apng_free(a);
            }
            if (ok && i == 30) ok = ai.num_plays == 0;
            if (ok && i == 31) ok = ai.num_plays == 1;
            if (ok && i == 32) ok = ai.num_plays == 2;
        } else if (i == 39 || i == 41) {         /* no (usable) acTL: a still image */
            ok = s == QPNG_OK && !ai.animated && ai.num_frames == 1 && all_px(c, np, 0, 255, 0, 255, 0, 0);
        } else if (i == 42) {
            ok = 1;   /* disabled in the suite */
        } else {                                 /* invalid structure: rejected */
            ok = i >= 59 ? s != QPNG_OK : s == QPNG_ERR_APNG;   /* 059/060: fdAT data size mismatch, found when decoding */
        }
        if (!ok) { fprintf(stderr, "apng suite %03d: %s\n", i, s ? qpng_status_string(s) : "wrong pixels"); fails++; }
        free(c); free(b);
    }
}

/* reference compositor (float), for the encode round trip */
static void ref_compose(float *cv, float *saved, uint32_t W, const qpng_apng_frame *f, int first, const qpng_frame_info *prev) {
    if (prev && prev->dispose_op == QPNG_DISPOSE_BACKGROUND)
        for (uint32_t y = 0; y < prev->height; y++) for (uint32_t x = 0; x < prev->width * 4; x++) cv[((prev->y_offset + y) * W + prev->x_offset) * 4 + x] = 0;
    if (prev && prev->dispose_op == QPNG_DISPOSE_PREVIOUS)
        for (uint32_t y = 0; y < prev->height; y++) for (uint32_t x = 0; x < prev->width * 4; x++) { size_t o = ((prev->y_offset + y) * W + prev->x_offset) * 4 + x; cv[o] = saved[o]; }
    (void)first;
    for (uint32_t y = 0; y < f->info.height; y++) for (uint32_t x = 0; x < f->info.width; x++) {
        size_t o = ((f->info.y_offset + y) * W + f->info.x_offset + x) * 4;
        saved[o] = cv[o]; saved[o + 1] = cv[o + 1]; saved[o + 2] = cv[o + 2]; saved[o + 3] = cv[o + 3];
        const uint8_t *s = (const uint8_t *)f->pixels + y * f->stride + x * 4;
        float as = s[3] / 255.f, ad = cv[o + 3] / 255.f;
        if (f->info.blend_op == QPNG_BLEND_SOURCE) { for (int k = 0; k < 4; k++) cv[o + k] = s[k]; continue; }
        float oa = as + ad * (1 - as);
        for (int k = 0; k < 3; k++) cv[o + k] = oa > 0 ? (s[k] * as + cv[o + k] * ad * (1 - as)) / oa : 0;
        cv[o + 3] = oa * 255.f;
    }
}

static void roundtrip(void) {
    enum { W = 37, H = 23, NF = 7 };
    static uint8_t px[NF][W * H * 4];
    qpng_apng_frame fr[NF];
    uint32_t rng = 7;
    for (int i = 0; i < NF; i++) {
        qpng_frame_info *f = &fr[i].info;
        memset(f, 0, sizeof *f);
        if (i == 0) { f->width = W; f->height = H; }
        else { f->width = 1 + (rng = rng * 69069 + 1) % W; f->height = 1 + (rng = rng * 69069 + 1) % H;
               f->x_offset = (rng = rng * 69069 + 1) % (W - f->width + 1); f->y_offset = (rng = rng * 69069 + 1) % (H - f->height + 1); }
        f->dispose_op = (uint8_t)(i % 3); f->blend_op = (uint8_t)(i % 2 ? QPNG_BLEND_OVER : QPNG_BLEND_SOURCE);
        if (i == 0) f->dispose_op = QPNG_DISPOSE_NONE;
        f->delay_num = (uint16_t)(i + 1); f->delay_den = 30;
        for (size_t k = 0; k < sizeof px[i]; k++) px[i][k] = (uint8_t)((rng = rng * 69069 + 1) >> 24);
        fr[i].pixels = px[i]; fr[i].stride = (size_t)f->width * 4;
    }
    for (int def = 0; def < 2; def++) {
        uint8_t *png; size_t n;
        CHECK(qpng_encode_apng(W, H, QPNG_COLOR_RGBA, 8, fr, NF, 3, def ? px[1] : NULL, (size_t)W * 4, NULL, &png, &n) == QPNG_OK);
        qpng_apng *a; CHECK(qpng_apng_open(png, n, NULL, &a) == QPNG_OK);
        qpng_apng_info ai; qpng_apng_get_info(a, &ai);
        CHECK(ai.animated && ai.num_frames == NF && ai.num_plays == 3 && ai.default_image_is_frame == !def);
        static float cv[W * H * 4], sv[W * H * 4];
        memset(cv, 0, sizeof cv);
        const uint8_t *c; qpng_frame_info fi; int maxd = 0;
        for (int i = 0; i < NF; i++) {
            CHECK(qpng_apng_next_frame(a, &c, &fi) == QPNG_OK);
            CHECK(fi.width == fr[i].info.width && fi.x_offset == fr[i].info.x_offset && fi.delay_num == fr[i].info.delay_num &&
                  fi.blend_op == fr[i].info.blend_op);
            ref_compose(cv, sv, W, &fr[i], i == 0, i ? &fr[i - 1].info : NULL);
            for (size_t k = 0; k < sizeof cv / sizeof *cv; k++) {
                int d = abs((int)c[k] - (int)(cv[k] + 0.5f));
                if (c[k | 3] == 0 && (k & 3) != 3) d = 0;   /* color of fully transparent pixels is irrelevant */
                if (d > maxd) maxd = d;
            }
            /* keep the reference in 8 bits like the decoder */
            for (size_t k = 0; k < sizeof cv / sizeof *cv; k++) cv[k] = c[k];
            qpng_image im; CHECK(qpng_apng_raw_frame(a, (uint32_t)i, NULL, &im, &fi) == QPNG_OK);
            CHECK(im.size == (size_t)fi.width * fi.height * 4 && !memcmp(im.pixels, px[i], im.size));
            qpng_image_free(&im);
        }
        CHECK(maxd <= 1);
        CHECK(qpng_apng_next_frame(a, &c, &fi) == QPNG_ERR_STATE);
        qpng_apng_rewind(a); CHECK(qpng_apng_next_frame(a, &c, &fi) == QPNG_OK);
        qpng_apng_free(a);
        qpng_image im; CHECK(qpng_decode(png, n, NULL, &im) == QPNG_OK && !memcmp(im.pixels, def ? px[1] : px[0], (size_t)W * H * 4));
        qpng_image_free(&im);
        qpng_buffer_free(png, NULL);
    }
    /* invalid: frame outside the canvas, frame 0 not covering the canvas without a separate default image */
    uint8_t *png; size_t n;
    fr[2].info.x_offset = W;
    CHECK(qpng_encode_apng(W, H, QPNG_COLOR_RGBA, 8, fr, NF, 0, NULL, 0, NULL, &png, &n) == QPNG_ERR_ARGUMENT);
    fr[2].info.x_offset = 0;
    CHECK(qpng_encode_apng(W, H, QPNG_COLOR_RGBA, 8, fr + 1, NF - 1, 0, NULL, 0, NULL, &png, &n) == QPNG_ERR_ARGUMENT);
}

static void stills(int argc, char **argv) {
    for (int i = 2; i < argc; i++) {
        size_t n; uint8_t *b = slurp(argv[i], &n);
        if (!b) continue;
        qpng_image ref; qpng_decode_options o; qpng_decode_options_init(&o);
        if (qpng_decode(b, n, &o, &ref) == QPNG_OK) {
            qpng_apng *a; CHECK(qpng_apng_open(b, n, NULL, &a) == QPNG_OK);
            qpng_image im; qpng_frame_info fi;
            CHECK(qpng_apng_raw_frame(a, 0, NULL, &im, &fi) == QPNG_OK && im.size == ref.size && !memcmp(im.pixels, ref.pixels, ref.size));
            qpng_image_free(&im);
            const uint8_t *c; CHECK(qpng_apng_next_frame(a, &c, &fi) == QPNG_OK);
            qpng_apng_free(a); qpng_image_free(&ref);
        }
        free(b);
    }
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    suite(argv[1]);
    roundtrip();
    stills(argc, argv);
    printf("apng: %d failures\n", fails);
    return fails != 0;
}
