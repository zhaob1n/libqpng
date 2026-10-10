/* test_metadata.c - ancillary chunks: write -> read round trip of every field, PngSuite ancillary files
 * (values from the PngSuite documentation), re-encode round trips, limits against compressed bombs, validation.
 * Usage: test_metadata PNGSUITE_DIR [files to re-encode...] */
#include "pngfast.h"
#include "test_util.h"
static int fails;

static uint8_t *enc(const pf_metadata *m, pf_color color, int depth, size_t *pn, pf_status *st) {
    static uint8_t px[16 * 16 * 8];
    for (size_t i = 0; i < sizeof px; i++) px[i] = (uint8_t)(i * 13);
    static const uint8_t pal[6] = { 0, 0, 0, 255, 255, 255 };
    pf_encode_options o; pf_encode_options_init(&o); o.metadata = m;
    if (color == PF_COLOR_PALETTE) { o.palette = pal; o.palette_entries = 2; for (size_t i = 0; i < sizeof px; i++) px[i] &= 1; }
    uint8_t *png = NULL;
    *st = pf_encode(px, 16, 16, 0, color, depth, &o, &png, pn);
    return png;
}

static void full_roundtrip(void) {
    static uint8_t icc[5000];
    for (size_t i = 0; i < sizeof icc; i++) icc[i] = (uint8_t)(i % 7);
    static const uint8_t exif[] = { 'M', 'M', 0, 42, 0, 0, 0, 8, 0, 0 };
    static const uint8_t priv[] = { 1, 2, 3 };
    pf_text tx[4] = {
        { PF_TEXT_TEXT, "Title", "hello", 0, NULL, NULL, 0 },
        { PF_TEXT_ZTXT, "Comment", "compressed compressed compressed", 0, NULL, NULL, 0 },
        { PF_TEXT_ITXT, "Description", "gr\xc3\xbc\xc3\x9f" "e", 0, "de", "Beschreibung", 1 },
        { PF_TEXT_ITXT, "Author", "x", 0, "", "", 0 } };
    pf_chunk ch[3] = { { "prVa", PF_CHUNK_BEFORE_IDAT, priv, 3 }, { "prVb", PF_CHUNK_BEFORE_IDAT, priv, 2 },
                       { "prVc", PF_CHUNK_AFTER_IDAT, NULL, 0 } };
    pf_metadata m; memset(&m, 0, sizeof m);
    m.has_gama = 1; m.gamma = 45455;
    m.has_chrm = 1; for (int i = 0; i < 8; i++) m.chrm[i] = 1000u * (uint32_t)(i + 1);
    m.has_cicp = 1; m.cicp[0] = 9; m.cicp[1] = 16; m.cicp[2] = 0; m.cicp[3] = 1;
    m.has_mdcv = 1; for (int i = 0; i < 6; i++) m.mdcv_primaries[i] = (uint16_t)(100 * i + 1);
    m.mdcv_white[0] = 15635; m.mdcv_white[1] = 16450; m.mdcv_max_lum = 10000000; m.mdcv_min_lum = 1;
    m.has_clli = 1; m.clli_max_cll = 1000; m.clli_max_fall = 400;
    m.has_phys = 1; m.phys_x = 2835; m.phys_y = 2836; m.phys_unit = 1;
    m.has_time = 1; m.time_year = 2026; m.time_month = 10; m.time_day = 10; m.time_hour = 22; m.time_minute = 30; m.time_second = 59;
    m.has_bkgd = 1; m.bkgd_size = 6; m.bkgd[1] = 10; m.bkgd[3] = 20; m.bkgd[5] = 30;
    m.has_sbit = 1; m.sbit_size = 3; m.sbit[0] = 5; m.sbit[1] = 6; m.sbit[2] = 5;
    m.iccp_name = "test profile"; m.iccp = icc; m.iccp_size = sizeof icc;
    m.exif = exif; m.exif_size = sizeof exif;
    m.texts = tx; m.text_count = 4; m.chunks = ch; m.chunk_count = 3;
    size_t pn; pf_status st;
    uint8_t *png = enc(&m, PF_COLOR_RGB, 8, &pn, &st);
    CHECK(st == PF_OK);
    if (st) return;
    pf_image im; CHECK(pf_decode(png, pn, NULL, &im) == PF_OK); pf_image_free(&im);
    pf_metadata r;
    CHECK(pf_read_metadata(png, pn, NULL, &r) == PF_OK);
    CHECK(r.has_gama && r.gamma == 45455);
    CHECK(r.has_chrm && r.chrm[7] == 8000);
    CHECK(r.has_cicp && r.cicp[1] == 16 && r.cicp[3] == 1);
    CHECK(r.has_mdcv && r.mdcv_primaries[5] == 501 && r.mdcv_white[1] == 16450 && r.mdcv_max_lum == 10000000 && r.mdcv_min_lum == 1);
    CHECK(r.has_clli && r.clli_max_cll == 1000 && r.clli_max_fall == 400);
    CHECK(r.has_phys && r.phys_x == 2835 && r.phys_y == 2836 && r.phys_unit == 1);
    CHECK(r.has_time && r.time_year == 2026 && r.time_second == 59);
    CHECK(r.has_bkgd && r.bkgd_size == 6 && r.bkgd[5] == 30);
    CHECK(r.has_sbit && r.sbit_size == 3 && r.sbit[1] == 6);
    CHECK(r.iccp && r.iccp_size == sizeof icc && !memcmp(r.iccp, icc, sizeof icc) && !strcmp(r.iccp_name, "test profile"));
    CHECK(r.exif && r.exif_size == sizeof exif && !memcmp(r.exif, exif, sizeof exif));
    CHECK(r.text_count == 4);
    for (size_t i = 0; i < r.text_count && i < 4; i++) {
        CHECK(r.texts[i].kind == tx[i].kind && !strcmp(r.texts[i].keyword, tx[i].keyword) && !strcmp(r.texts[i].text, tx[i].text));
        CHECK(r.texts[i].text_size == strlen(tx[i].text));
        if (tx[i].kind == PF_TEXT_ITXT)
            CHECK(!strcmp(r.texts[i].language, tx[i].language) && !strcmp(r.texts[i].translated_keyword, tx[i].translated_keyword) &&
                  r.texts[i].compressed == tx[i].compressed);
    }
    CHECK(r.chunk_count == 3);
    for (size_t i = 0; i < r.chunk_count && i < 3; i++)
        CHECK(!strcmp(r.chunks[i].type, ch[i].type) && r.chunks[i].location == ch[i].location && r.chunks[i].size == ch[i].size &&
              (!ch[i].size || !memcmp(r.chunks[i].data, ch[i].data, ch[i].size)));
    CHECK(r.skipped == 0);
    /* re-encode from what was read: identical file */
    size_t pn2; uint8_t *png2 = enc(&r, PF_COLOR_RGB, 8, &pn2, &st);
    CHECK(st == PF_OK && pn2 == pn && !memcmp(png, png2, pn));
    pf_buffer_free(png2, NULL);
    /* limits: the ICC profile and the zTXt exceed a 20-byte per-chunk limit */
    pf_metadata_options mo; pf_metadata_options_init(&mo); mo.max_chunk_decompressed = 20;
    pf_metadata r2; CHECK(pf_read_metadata(png, pn, &mo, &r2) == PF_OK);
    CHECK(!r2.iccp && r2.text_count == 3 && r2.skipped == 2);   /* iCCP + zTXt (33 bytes); the 7-byte compressed iTXt fits */
    pf_metadata_free(&r2);
    mo.max_chunk_decompressed = 0; mo.max_chunks = 2; mo.keep_unknown = 0;
    CHECK(pf_read_metadata(png, pn, &mo, &r2) == PF_OK && r2.text_count == 2 && r2.chunk_count == 0);
    pf_metadata_free(&r2);
    /* corrupt ancillary CRC: chunk skipped, rest intact; allocator hooks: no leaks */
    int gl; const uint8_t *g = find_chunk(png, pn, "gAMA", &gl);
    CHECK(g != NULL);
    if (g) {
        png[g - png + 1] ^= 1;
        CHECK(pf_read_metadata(png, pn, NULL, &r2) == PF_OK && !r2.has_gama && r2.skipped == 1 && r2.text_count == 4);
        pf_metadata_free(&r2);
    }
    pf_metadata_free(&r); CHECK(r.internal_ == NULL);
    pf_buffer_free(png, NULL);
    /* validation */
    pf_text bad = { PF_TEXT_TEXT, "", "x", 0, NULL, NULL, 0 };
    pf_metadata v; memset(&v, 0, sizeof v); v.texts = &bad; v.text_count = 1;
    CHECK(enc(&v, PF_COLOR_RGB, 8, &pn, &st) == NULL && st == PF_ERR_ARGUMENT);
    bad.keyword = " lead"; CHECK(enc(&v, PF_COLOR_RGB, 8, &pn, &st) == NULL && st == PF_ERR_ARGUMENT);
    memset(&v, 0, sizeof v); v.has_bkgd = 1; v.bkgd_size = 6;
    CHECK(enc(&v, PF_COLOR_PALETTE, 8, &pn, &st) == NULL && st == PF_ERR_ARGUMENT);
    pf_chunk crit = { "CRIT", PF_CHUNK_BEFORE_IDAT, priv, 3 };
    memset(&v, 0, sizeof v); v.chunks = &crit; v.chunk_count = 1;
    CHECK(enc(&v, PF_COLOR_RGB, 8, &pn, &st) == NULL && st == PF_ERR_ARGUMENT);
    memcpy(crit.type, "tRNS", 4); CHECK(enc(&v, PF_COLOR_RGB, 8, &pn, &st) == NULL && st == PF_ERR_ARGUMENT);
}

/* zTXt bomb: 64 MiB of 'a' (~64 KiB compressed) must be refused by the default 16 MiB per-chunk limit */
static void bomb(void) {
    const size_t big = 64u << 20;
    char *s = malloc(big + 1);
    if (!s) return;
    memset(s, 'a', big); s[big] = 0;
    pf_text tx = { PF_TEXT_ZTXT, "Bomb", s, big, NULL, NULL, 0 };
    pf_metadata m; memset(&m, 0, sizeof m); m.texts = &tx; m.text_count = 1;
    size_t pn; pf_status st; uint8_t *png = enc(&m, PF_COLOR_GRAY, 8, &pn, &st);
    free(s);
    CHECK(st == PF_OK && pn < (1u << 20));
    pf_metadata r;
    CHECK(pf_read_metadata(png, pn, NULL, &r) == PF_OK && r.text_count == 0 && r.skipped == 1);
    pf_metadata_free(&r);
    pf_buffer_free(png, NULL);
}

static pf_metadata rd(const char *dir, const char *name) {
    char path[4096]; snprintf(path, sizeof path, "%s/%s", dir, name);
    size_t n; uint8_t *b = slurp(path, &n);
    pf_metadata m; memset(&m, 0, sizeof m);
    if (!b) { fprintf(stderr, "missing %s\n", path); fails++; return m; }
    CHECK(pf_read_metadata(b, n, NULL, &m) == PF_OK);
    free(b);
    return m;
}
static const pf_text *text(const pf_metadata *m, const char *k) {
    for (size_t i = 0; i < m->text_count; i++) if (!strcmp(m->texts[i].keyword, k)) return &m->texts[i];
    return NULL;
}

static void pngsuite(const char *dir) {
    pf_metadata m;
    m = rd(dir, "g03n0g16.png"); CHECK(m.has_gama && m.gamma == 35000); pf_metadata_free(&m);
    m = rd(dir, "g25n2c08.png"); CHECK(m.has_gama && m.gamma == 250000); pf_metadata_free(&m);
    m = rd(dir, "ccwn2c08.png"); CHECK(m.has_chrm && m.chrm[0] == 31270 && m.chrm[1] == 32900); pf_metadata_free(&m);
    m = rd(dir, "cdfn2c08.png"); CHECK(m.has_phys && m.phys_x == 1 && m.phys_y == 4 && m.phys_unit == 0); pf_metadata_free(&m);
    m = rd(dir, "cdun2c08.png"); CHECK(m.has_phys && m.phys_x == 1000 && m.phys_y == 1000 && m.phys_unit == 1); pf_metadata_free(&m);
    m = rd(dir, "cm7n0g04.png"); CHECK(m.has_time && m.time_year == 1970 && m.time_month == 1 && m.time_day == 1); pf_metadata_free(&m);
    m = rd(dir, "cm9n0g04.png"); CHECK(m.has_time && m.time_year == 1999 && m.time_month == 12 && m.time_day == 31); pf_metadata_free(&m);
    m = rd(dir, "cs3n2c16.png"); CHECK(m.has_sbit && m.sbit_size == 3 && m.sbit[0] == 13); pf_metadata_free(&m);
    m = rd(dir, "cs5n3p08.png"); CHECK(m.has_sbit && m.sbit_size == 3 && m.sbit[0] == 5); pf_metadata_free(&m);
    m = rd(dir, "bgbn4a08.png"); CHECK(m.has_bkgd && m.bkgd_size == 2 && m.bkgd[0] == 0 && m.bkgd[1] == 0); pf_metadata_free(&m);
    m = rd(dir, "bgyn6a16.png"); CHECK(m.has_bkgd && m.bkgd_size == 6 && m.bkgd[0] == 0xff && m.bkgd[2] == 0xff && m.bkgd[4] == 0); pf_metadata_free(&m);
    m = rd(dir, "tbbn3p08.png"); CHECK(m.has_bkgd && m.bkgd_size == 1); pf_metadata_free(&m);
    m = rd(dir, "exif2c08.png"); CHECK(m.exif && m.exif_size > 8 && m.exif[0] == 'M' && m.exif[1] == 'M'); pf_metadata_free(&m);
    m = rd(dir, "ct1n0g04.png"); CHECK(m.text_count == 6 && text(&m, "Title") && text(&m, "Title")->kind == PF_TEXT_TEXT &&
                                       !strcmp(text(&m, "Title")->text, "PngSuite")); pf_metadata_free(&m);
    m = rd(dir, "ctzn0g04.png"); CHECK(m.text_count >= 2 && text(&m, "Copyright") && text(&m, "Copyright")->kind == PF_TEXT_ZTXT &&
                                       strstr(text(&m, "Copyright")->text, "Willem")); pf_metadata_free(&m);
    m = rd(dir, "cten0g04.png"); CHECK(text(&m, "Title") && text(&m, "Title")->kind == PF_TEXT_ITXT &&
                                       !strcmp(text(&m, "Title")->language, "en")); pf_metadata_free(&m);
    m = rd(dir, "ctjn0g04.png"); CHECK(text(&m, "Title") && !strcmp(text(&m, "Title")->language, "ja")); pf_metadata_free(&m);
    m = rd(dir, "ps1n0g08.png"); CHECK(m.chunk_count >= 1 && !strcmp(m.chunks[0].type, "sPLT") &&
                                       m.chunks[0].location == PF_CHUNK_BEFORE_IDAT); pf_metadata_free(&m);
}

/* every file: read metadata, decode, re-encode with that metadata, read back: same fields */
static void reencode_all(int argc, char **argv) {
    int files = 0;
    for (int i = 2; i < argc; i++) {
        size_t n; uint8_t *b = slurp(argv[i], &n);
        if (!b) continue;
        pf_metadata m; pf_image im;
        if (pf_read_metadata(b, n, NULL, &m) != PF_OK) { free(b); continue; }
        if (pf_decode(b, n, NULL, &im) != PF_OK) { pf_metadata_free(&m); free(b); continue; }
        int pl = 0, tl = 0; pf_encode_options o; pf_encode_options_init(&o);
        o.palette = find_chunk(b, n, "PLTE", &pl); o.palette_entries = o.palette ? pl / 3 : 0;
        if (im.color_type != PF_COLOR_PALETTE) { o.palette = NULL; o.palette_entries = 0; }
        o.trns = find_chunk(b, n, "tRNS", &tl); o.trns_bytes = o.trns ? tl : 0;
        if (im.color_type == PF_COLOR_GRAY_ALPHA || im.color_type == PF_COLOR_RGBA) { o.trns = NULL; o.trns_bytes = 0; }
        o.metadata = &m;
        uint8_t *png; size_t pn;
        pf_status st = pf_encode(im.pixels, im.width, im.height, im.stride, (pf_color)im.color_type, im.bit_depth, &o, &png, &pn);
        if (st == PF_OK) {
            pf_metadata r; CHECK(pf_read_metadata(png, pn, NULL, &r) == PF_OK);
            CHECK(r.text_count == m.text_count && r.chunk_count == m.chunk_count && r.has_gama == m.has_gama &&
                  r.gamma == m.gamma && r.has_time == m.has_time && r.exif_size == m.exif_size && r.iccp_size == m.iccp_size &&
                  r.has_bkgd == m.has_bkgd && r.has_sbit == m.has_sbit && r.has_phys == m.has_phys && r.has_chrm == m.has_chrm);
            for (size_t k = 0; k < r.text_count && k < m.text_count; k++)
                CHECK(r.texts[k].text_size == m.texts[k].text_size && !memcmp(r.texts[k].text, m.texts[k].text, m.texts[k].text_size));
            pf_metadata_free(&r); pf_buffer_free(png, NULL); files++;
        } else if (st != PF_ERR_ARGUMENT) { fprintf(stderr, "reencode %s: %s\n", argv[i], pf_status_string(st)); fails++; }
        pf_image_free(&im); pf_metadata_free(&m); free(b);
    }
    if (argc > 2) CHECK(files > 100);
    printf("metadata: %d files re-encoded with their metadata\n", files);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: test_metadata PNGSUITE_DIR [files...]\n"); return 2; }
    full_roundtrip();
    bomb();
    pngsuite(argv[1]);
    reencode_all(argc, argv);
    printf("metadata: %d failures\n", fails);
    return fails != 0;
}
