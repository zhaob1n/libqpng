/* qpngbench - qpng side of the benchmark (bench/run.py drives this and tools/rsref).
 *   qpngbench decode raw|expand|imagers REPS file.png...
 *   qpngbench encode PRESET[:FILTER[:LEVEL]] REPS file.png...   (source decoded to raw untimed; same format re-encoded)
 *   qpngbench deflate LEVEL REPS rawfile...    zlib compress with the vendored libdeflate: 1..12 | pf | pfrle | pfhigh | ultra
 *   qpngbench inflate stock|pairs|zng REPS orig:stream.zlib...   (zng: vendored zlib-ng, if built)
 *   PRESET suffixes (encode): "#ld" / "#zng" force the backend; "@B.L.D.N.M.S" sets QPNG_POLICY (final graphics
 *   stream: backend, level, depth, nice, minlen, strategy) in builds with -DQPNG_POLICY_OVERRIDE.
 *   deflate LEVEL "zL" / "zLsS": zlib-ng level L (strategy S).
 * CSV: tool,op,param,file,rep,width,height,ns,bytes. Each timing covers setup + allocation + work; 1 warm-up rep.
 * DUMP_DIR=dir writes the last encoded PNG / compressed stream. */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "qpng.h"
#include "qpng_libdeflate.h"
#ifdef QPNG_HAVE_ZLIBNG
#  include <zlib-ng.h>
#endif

static long long now_ns(void) { struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t); return t.tv_sec * 1000000000LL + t.tv_nsec; }
static unsigned char *slurp(const char *p, size_t *n) {
    FILE *f = fopen(p, "rb"); if (!f) { perror(p); exit(2); }
    fseek(f, 0, SEEK_END); long sz = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)sz + 64); if (!b || fread(b, 1, (size_t)sz, f) != (size_t)sz) exit(2);
    fclose(f); *n = (size_t)sz; return b;
}
static const char *base(const char *p) { const char *s = strrchr(p, '/'); return s ? s + 1 : p; }
static void dump(const char *file, const char *tag, const void *d, size_t n) {
    const char *dir = getenv("DUMP_DIR"); if (!dir) return;
    char path[4096]; snprintf(path, sizeof path, "%s/%s.%s", dir, base(file), tag);
    FILE *f = fopen(path, "wb"); if (f) { fwrite(d, 1, n, f); fclose(f); }
}
static const unsigned char *find_chunk(const unsigned char *b, size_t n, const char *type, int *len) {
    for (size_t i = 8; i + 12 <= n;) {
        size_t L = (size_t)b[i] << 24 | (size_t)b[i + 1] << 16 | (size_t)b[i + 2] << 8 | b[i + 3];
        if (!memcmp(b + i + 4, "IDAT", 4)) break;
        if (!memcmp(b + i + 4, type, 4)) { *len = (int)L; return b + i + 8; }
        i += 12 + L;
    }
    return NULL;
}
static int parse_preset(const char *s) {
    return !strncmp(s, "fastest", 7) ? QPNG_PRESET_FASTEST : !strncmp(s, "fast", 4) ? QPNG_PRESET_FAST :
           !strncmp(s, "best", 4) ? QPNG_PRESET_BEST : QPNG_PRESET_DEFAULT;
}
static unsigned g_depth, g_nice;   /* deflate op: LEVEL/DEPTH/NICE overrides the level's search params */
static int deflate_level(const char *s) {
    const char *sl = strchr(s, '/');
    if (sl) { g_depth = (unsigned)atoi(sl + 1); const char *s2 = strchr(sl + 1, '/'); if (s2) g_nice = (unsigned)atoi(s2 + 1); }
    return !strcmp(s, "pf") ? LIBDEFLATE_PNG_FAST_LEVEL : !strcmp(s, "pfrle") ? -1 : !strcmp(s, "pfhigh") ? -2 :
           !strcmp(s, "ultra") ? -3 : atoi(s);
}

int main(int argc, char **argv) {
    if (argc < 5) { fprintf(stderr, "usage: see source header\n"); return 2; }
    const char *op = argv[1], *param = argv[2]; int reps = atoi(argv[3]);
    for (int fi = 4; fi < argc; fi++) {
        const char *file = argv[fi];
        if (!strcmp(op, "inflate")) {
            char *o = strdup(file), *s = strchr(o, ':'); if (!s) return 2; *s++ = 0;
            size_t n, sn; unsigned char *orig = slurp(o, &n), *st = slurp(s, &sn), *out = malloc(n + 1);
            for (int r = 0; r <= reps; r++) {
                size_t got = 0; long long t0 = now_ns();
                int rc;
#ifdef QPNG_HAVE_ZLIBNG
                if (!strcmp(param, "zng")) {
                    zng_stream z; memset(&z, 0, sizeof z);
                    zng_inflateInit(&z); z.next_in = st; z.avail_in = (uint32_t)sn; z.next_out = out; z.avail_out = (uint32_t)n;
                    rc = zng_inflate(&z, Z_FINISH) != Z_STREAM_END; got = n - z.avail_out; zng_inflateEnd(&z);
                } else
#endif
                {
                struct libdeflate_decompressor *d = libdeflate_alloc_decompressor();
                if (!strcmp(param, "pairs")) libdeflate_set_pair_literals(d, 1);
                rc = libdeflate_zlib_decompress(d, st, sn, out, n, &got);
                libdeflate_free_decompressor(d);
                }
                long long dt = now_ns() - t0;
                if (rc || got != n || memcmp(out, orig, n)) { fprintf(stderr, "FAIL inflate %s\n", s); return 1; }
                if (r) printf("qpng,inflate,%s,%s,%d,0,0,%lld,%zu\n", param, o, r, dt, n);
            }
            free(orig); free(st); free(out); free(o); continue;
        }
        size_t n; unsigned char *buf = slurp(file, &n);
        if (!strcmp(op, "deflate")) {
            int lv = param[0] == 'z' ? 0 : deflate_level(param);
            size_t cap = n + n / 8 + 65536; unsigned char *out = malloc(cap), *chk = malloc(n + 1);
            for (int r = 0; r <= reps; r++) {
                long long t0 = now_ns();
                size_t m;
#ifdef QPNG_HAVE_ZLIBNG
                if (param[0] == 'z') {
                    const char *ss = strchr(param, 's');
                    unsigned long dl = (unsigned long)cap;
                    zng_stream z; memset(&z, 0, sizeof z);
                    zng_deflateInit2(&z, atoi(param + 1), Z_DEFLATED, 15, 9, ss ? atoi(ss + 1) : 0);
                    z.next_in = buf; z.avail_in = (uint32_t)n; z.next_out = out; z.avail_out = (uint32_t)dl;
                    m = zng_deflate(&z, Z_FINISH) == Z_STREAM_END ? dl - z.avail_out : 0; zng_deflateEnd(&z);
                } else
#endif
                {
                struct libdeflate_compressor *c = libdeflate_alloc_compressor(lv == -3 ? LIBDEFLATE_PNG_ULTRA_LEVEL : lv < 0 ? LIBDEFLATE_PNG_FAST_LEVEL : lv);
                if (lv == -1) libdeflate_set_png_min_match(c, 0);
                if (lv == -2) libdeflate_set_png_params(c, 0, 9);
                if (g_depth || g_nice) libdeflate_set_search_params(c, g_depth, g_nice);
                m = libdeflate_zlib_compress(c, buf, n, out, cap);
                libdeflate_free_compressor(c);
                }
                long long dt = now_ns() - t0;
                size_t got = 0; struct libdeflate_decompressor *d = libdeflate_alloc_decompressor();
                if (!m || libdeflate_zlib_decompress(d, out, m, chk, n, &got) || got != n || memcmp(chk, buf, n)) { fprintf(stderr, "FAIL deflate %s\n", file); return 1; }
                libdeflate_free_decompressor(d);
                if (r == reps) { char tag[64]; snprintf(tag, sizeof tag, "qpng-%s.zlib", param); dump(file, tag, out, m); }
                if (r) printf("qpng,deflate,%s,%s,%d,0,0,%lld,%zu\n", param, file, r, dt, m);
            }
            free(out); free(chk); free(buf); continue;
        }
        if (!strcmp(op, "decode")) {
            qpng_decode_options o; qpng_decode_options_init(&o);
            o.output = !strcmp(param, "expand") ? QPNG_OUTPUT_EXPAND : !strcmp(param, "imagers") ? QPNG_OUTPUT_IMAGERS : QPNG_OUTPUT_RAW;
            for (int r = 0; r <= reps; r++) {
                qpng_image im; long long t0 = now_ns();
                qpng_status s = qpng_decode(buf, n, &o, &im);
                long long dt = now_ns() - t0;
                if (s) { fprintf(stderr, "FAIL decode %s: %s\n", file, qpng_status_string(s)); return 1; }
                if (r == reps) dump(file, "qpng.raw", im.pixels, im.size);
                if (r) printf("qpng,decode,%s,%s,%d,%u,%u,%lld,%zu\n", param, file, r, im.width, im.height, dt, im.size);
                qpng_image_free(&im);
            }
            free(buf); continue;
        }
        if (!strcmp(op, "encode")) {
            qpng_image im; qpng_status s = qpng_decode(buf, n, NULL, &im);
            if (s) { fprintf(stderr, "FAIL source decode %s\n", file); return 1; }
            qpng_encode_options o; qpng_encode_options_init(&o);
            o.preset = parse_preset(param);
            if (strstr(param, "#zng")) o.backend = QPNG_BACKEND_ZLIBNG;
            if (strstr(param, "#ld")) o.backend = QPNG_BACKEND_LIBDEFLATE;
            const char *pol = strchr(param, '@');
            if (pol) {   /* "@B.L.D.N.M.S" (dots: run.py splits configs on commas) */
                char e[64]; snprintf(e, sizeof e, "%s", pol + 1);
                for (char *q = e; *q; q++) if (*q == '.') *q = ':';
                setenv("QPNG_POLICY", e, 1);
            } else unsetenv("QPNG_POLICY");
            char pp[128]; snprintf(pp, sizeof pp, "%s", param); pp[strcspn(pp, "@#")] = 0;
            const char *c1 = strchr(pp, ':');
            if (c1) { o.filter = atoi(c1 + 1); const char *c2 = strchr(c1 + 1, ':'); if (c2) { o.deflate_level = deflate_level(c2 + 1) < 0 ? 100 - deflate_level(c2 + 1) : deflate_level(c2 + 1);
                                                                                         o.search_depth = (int)g_depth; o.nice_length = (int)g_nice; } }
            int pl = 0, tl = 0;
            o.palette = find_chunk(buf, n, "PLTE", &pl); o.palette_entries = o.palette ? pl / 3 : 0;
            o.trns = find_chunk(buf, n, "tRNS", &tl); o.trns_bytes = o.trns ? tl : 0;
            for (int r = 0; r <= reps; r++) {
                uint8_t *png; size_t pn; long long t0 = now_ns();
                s = qpng_encode(im.pixels, im.width, im.height, im.stride, (qpng_color)im.color_type, im.bit_depth, &o, &png, &pn);
                long long dt = now_ns() - t0;
                if (s) { fprintf(stderr, "FAIL encode %s: %s\n", file, qpng_status_string(s)); return 1; }
                if (r == reps) { char tag[96]; snprintf(tag, sizeof tag, "qpng-%s.png", param); dump(file, tag, png, pn); }
                if (r) printf("qpng,encode,%s,%s,%d,%u,%u,%lld,%zu\n", param, file, r, im.width, im.height, dt, pn);
                qpng_buffer_free(png, NULL);
            }
            qpng_image_free(&im); free(buf); continue;
        }
        fprintf(stderr, "unknown op %s\n", op); return 2;
    }
    return 0;
}
