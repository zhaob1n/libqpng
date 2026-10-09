/* Decode every test PNG (PngSuite + generated format corpus) in the three output modes and compare with the png
 * crate / image-rs reference hashes in data/refs.txt (tests/gen_refs.py). Files the reference rejects must fail. */
#include "pngfast.h"
#include "test_util.h"

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: test_conformance DATA_DIR\n"); return 2; }
    char path[4096]; snprintf(path, sizeof path, "%s/refs.txt", argv[1]);
    FILE *rf = fopen(path, "r"); if (!rf) { perror(path); return 2; }
    char line[8192]; int files = 0, checks = 0, fails = 0;
    static const pf_output modes[3] = { PF_OUTPUT_RAW, PF_OUTPUT_EXPAND, PF_OUTPUT_IMAGERS };
    static const char *mname[3] = { "raw", "expand", "imagers" };
    while (fgets(line, sizeof line, rf)) {
        char name[1024], ref[3][128];
        if (sscanf(line, "%1023s %127s %127s %127s", name, ref[0], ref[1], ref[2]) != 4) continue;
        snprintf(path, sizeof path, "%s/%s", argv[1], name);
        size_t n; unsigned char *buf = slurp(path, &n);
        if (!buf) { fprintf(stderr, "missing %s\n", path); fails++; continue; }
        files++;
        for (int m = 0; m < 3; m++) {
            pf_decode_options o; pf_decode_options_init(&o); o.output = modes[m];
            pf_image im; pf_status s = pf_decode(buf, n, &o, &im);
            char got[128];
            if (s) snprintf(got, sizeof got, "ERR");
            else snprintf(got, sizeof got, "%ux%u:%zu:%016llx", im.width, im.height, im.size, (unsigned long long)fnv1a(im.pixels, im.size));
            checks++;
            if (strcmp(got, ref[m])) { fprintf(stderr, "MISMATCH %s [%s]: got %s (%s) want %s\n", name, mname[m], got, s ? pf_status_string(s) : "ok", ref[m]); fails++; }
            pf_image_free(&im);
        }
        free(buf);
    }
    fclose(rf);
    printf("conformance: %d files, %d checks, %d failures\n", files, checks, fails);
    return fails != 0;
}
