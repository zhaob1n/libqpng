# pngfast

Fast, small PNG decoder and encoder in C11, built on a vendored and modified
[libdeflate](https://github.com/ebiggers/libdeflate), plus an optional vendored [zlib-ng](https://github.com/zlib-ng/zlib-ng)
encoder backend for graphics. Single header API, no other dependencies,
runtime CPU dispatch on x86-64 (SSE2 baseline, AVX2 / BMI2 / AVX-512 where available; never `-march=native`).
Other architectures (or `-DPNGFAST_NO_SIMD`) build portable scalar kernels with identical output.

* **Decode**: all PNG color types / bit depths, Adam7, tRNS, 16-bit; outputs raw samples, "expanded"
  (png crate `EXPAND` semantics), or image-rs layout (8-bit RGBA/RGB/LA/L, 16-bit native endian).
  Inflate and un-filtering are fused (rows are un-filtered while still in cache), literal pairs are decoded
  two at a time, Paeth/Avg use 2-row wavefronts and an AVX2 4-row Paeth kernel.
* **Encode**: four presets (FASTEST/FAST/DEFAULT/BEST) plus explicit filter / deflate-level overrides,
  fused SIMD adaptive filter selection, a PNG-specialised single-pass deflate ("png-fast") and libdeflate
  levels 1-12, content probing (photo vs graphics) for DEFAULT and BEST.
* **Safety**: CRC checking (on by default), optional Adler-32 checking, dimension and output-size limits against
  decompression bombs, allocator hooks, fuzz targets, ASan/UBSan script.
* Output is bit-exact with the png crate / image-rs on PngSuite and a 40-file format corpus (648 checks).

## Build

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build              # api, conformance, roundtrip, metadata, stream, apng, symbols, link_test, fuzz smoke
cmake --install build --prefix /usr/local   # installs pngfast.h, libpngfast, pngfast.pc, CMake package
```

Options: `PNGFAST_WITH_ZLIBNG` (default ON when `third_party/zlib-ng` exists; OFF builds a libdeflate-only
library with the pngfast 1.0 encoder policy), `PNGFAST_BUILD_SHARED` (default OFF), `PNGFAST_BUILD_TESTS`, `PNGFAST_BUILD_TOOLS`,
`PNGFAST_HIDE_SYMBOLS` (default ON: the library exports only the `pf_*` API, see below), `PNGFAST_SANITIZE` (ASan+UBSan), `PNGFAST_LIBFUZZER` (clang), `PNGFAST_CORPUS_DIR` (extra round-trip corpus).
Consumers: `pkg-config --cflags --libs pngfast` or `find_package(pngfast)` + `pngfast::pngfast`.

## API

```c
#include <pngfast.h>

/* decode */
pf_decode_options dopt;
pf_decode_options_init(&dopt);
dopt.output = PF_OUTPUT_IMAGERS;          /* or PF_OUTPUT_RAW / PF_OUTPUT_EXPAND */
dopt.max_output_bytes = 256u << 20;       /* bomb limit (default 1 GiB) */
pf_image img;
pf_status s = pf_decode(png, png_size, &dopt, &img);
if (s != PF_OK) { fprintf(stderr, "%s\n", pf_status_string(s)); return 1; }
/* img.pixels: img.height rows of img.stride bytes; img.channels, img.bit_depth, img.color_type */
pf_image_free(&img);

/* encode */
pf_encode_options eopt;
pf_encode_options_init(&eopt);
eopt.preset = PF_PRESET_DEFAULT;          /* FASTEST, FAST, DEFAULT, BEST */
/* optional: eopt.filter = PF_FILTER_PAETH; eopt.deflate_level = 9 (1..12, or PF_DEFLATE_PNGFAST*) */
/* optional: eopt.backend = PF_BACKEND_ZLIBNG (or LIBDEFLATE; default AUTO); with ZLIBNG, deflate_level is a
 * zlib-ng level 1..9 and eopt.zlib_strategy a PF_ZSTRATEGY_*; pf_backend_available() tells what is built in */
/* palette images: eopt.palette = rgb_triplets; eopt.palette_entries = n; optional eopt.trns / trns_bytes */
uint8_t *out; size_t out_size;
s = pf_encode(pixels, width, height, 0 /* stride, 0 = packed */, PF_COLOR_RGBA, 8, &eopt, &out, &out_size);
/* ... */
pf_buffer_free(out, NULL);                /* same allocator as eopt.allocator */
```

`pf_read_info()` parses IHDR without decoding. Custom allocation: fill a `pf_allocator {alloc, free, user}`
and pass it in either options struct; decoded images remember their allocator.
Encoding accepts samples in PNG byte order (16-bit big-endian, sub-byte depths packed MSB first) - exactly
what `PF_OUTPUT_RAW` decoding returns.

### Metadata (ancillary chunks)

```c
pf_metadata meta;                                  /* tEXt/zTXt/iTXt, iCCP, sRGB, gAMA, cHRM, cICP, mDCV, cLLI, */
if (pf_read_metadata(png, png_size, NULL, &meta) == PF_OK) {   /* pHYs, tIME, bKGD, sBIT, eXIf, unknown chunks */
    for (size_t i = 0; i < meta.text_count; i++) printf("%s: %s\n", meta.texts[i].keyword, meta.texts[i].text);
    if (meta.iccp) use_profile(meta.iccp, meta.iccp_size);       /* decompressed */
    eopt.metadata = &meta;                         /* write everything back (unknown chunks at their place) */
    pf_encode(pixels, w, h, 0, PF_COLOR_RGB, 8, &eopt, &out, &out_size);
    pf_metadata_free(&meta);
}
/* writing from scratch: */
pf_text t = { PF_TEXT_ITXT, "Description", "gr\xc3\xbc\xc3\x9f" "e", 0, "de", "Beschreibung", 1 };
pf_metadata m = {0}; m.texts = &t; m.text_count = 1; m.has_phys = 1; m.phys_x = m.phys_y = 2835; m.phys_unit = 1;
eopt.metadata = &m;
```

Limits (`pf_metadata_options`): 16 MiB per decompressed zTXt/iTXt/iCCP, 64 MiB in total, 4096 text + unknown
chunks; malformed / duplicate / bad-CRC / over-limit ancillary chunks are skipped and counted in `meta.skipped`.
Values are reported as stored: **no color management** is applied (gAMA, cHRM, iCCP, sRGB, cICP, mDCV, cLLI, sBIT
and bKGD are informational).

### Streaming

```c
/* decode: push bytes as they arrive; rows are delivered through a callback */
static int on_row(void *user, uint32_t y, const uint8_t *row, size_t n) { consume(row, n); return 0; }
pf_decoder *dec; pf_decoder_new(&dopt, on_row, user, &dec);    /* row_cb NULL: whole image at finish */
while ((n = recv(sock, buf, sizeof buf, 0)) > 0) {
    s = pf_decoder_push(dec, buf, n);           /* PF_MORE_DATA, PF_OK after IEND, or an error */
    if (pf_decoder_info(dec, &info) == PF_OK) { /* header available as soon as IHDR arrived */ }
    if (s != PF_MORE_DATA) break;
}
if (s == PF_OK) s = pf_decoder_finish(dec, NULL /* or &img in image mode */);
pf_decoder_free(dec);
/* or pull: pf_decode_stream(read_fn, file, &dopt, on_row, user, NULL); */

/* encode: rows in, bytes out through a write callback, bounded memory */
static int wr(void *f, const uint8_t *p, size_t n) { return fwrite(p, 1, n, f) != n; }
pf_encoder *enc; pf_encoder_new(w, h, PF_COLOR_RGBA, 8, &eopt, wr, file, &enc);
for (y = 0; y < h; y += k) pf_encoder_push_rows(enc, rows + y * stride, stride, k);
pf_encoder_finish(enc); pf_encoder_free(enc);
```

Trade-offs: libdeflate (used by the one-shot decoder) cannot inflate incrementally. Row mode on a non-interlaced
image with zlib-ng built in is truly incremental (zlib-ng raw inflate into a one-row buffer, O(row) memory).
Image mode, interlaced images and libdeflate-only builds buffer the compressed input and decode it with the
one-shot path once IEND arrived (memory: file + image; image mode is then as fast as `pf_decode`). The encoder
compresses FASTEST/FAST with libdeflate's streamed png-ultra (as one-shot) and DEFAULT/BEST with zlib-ng L6/L9
(png-ultra without zlib-ng); it filters row by row (no whole-image probing), so output can be a bit larger than
`pf_encode`'s. Measurements in NOTES.md (Phase 5).

### APNG

```c
pf_apng *a;
if (pf_apng_open(png, size, NULL, &a) == PF_OK) {           /* png must stay valid until pf_apng_free */
    pf_apng_info ai; pf_apng_get_info(a, &ai);                /* num_frames, num_plays, canvas size */
    const uint8_t *canvas; pf_frame_info fi;
    while (pf_apng_next_frame(a, &canvas, &fi) == PF_OK)       /* composed RGBA8 canvas, width * 4 stride */
        show(canvas, fi.delay_num, fi.delay_den ? fi.delay_den : 100);
    pf_image raw; pf_apng_raw_frame(a, 1, NULL, &raw, &fi);   /* one frame's own pixels, any output layout */
    pf_image_free(&raw);
    pf_apng_free(a);
}
/* encode: frames of their own size + offsets, dispose / blend ops, delays */
pf_apng_frame fr[2] = { { px0, 0, { W, H, 0, 0, 1, 10, PF_DISPOSE_NONE, PF_BLEND_SOURCE } },
                        { px1, 0, { 16, 16, 8, 8, 1, 10, PF_DISPOSE_PREVIOUS, PF_BLEND_OVER } } };
pf_encode_apng(W, H, PF_COLOR_RGBA, 8, fr, 2, 0 /* loop forever */, NULL, 0, &eopt, &out, &out_size);
```

Semantics follow the APNG spec / browsers (canvas starts transparent black, first-frame DISPOSE_PREVIOUS acts as
BACKGROUND, 8-bit source-over blending). Invalid animation structure (sequence numbers, frame count, regions, ...)
gives `PF_ERR_APNG`; `pf_decode` ignores APNG chunks and returns the default image. Files without acTL open as a
one-frame still image. Tested against Philip Taylor's APNG suite; raw frames and fcTL fields are identical to the
png crate's for every valid suite file.

### Symbol hygiene

`libpngfast.a` / `libpngfast.so` export only the functions declared `PF_API` in `pngfast.h` (export list generated
from the header): the shared library uses a linker version script, the static archive is one partially linked
object (`ld -r`) whose other globals are localized with `objcopy`. The vendored libdeflate and zlib-ng (including
zlib-ng internals such as `deflate_medium`, `crc32_*`, `functable`) therefore never clash with a system zlib,
zlib-ng or libdeflate; ctest `symbols` (nm check) and `link_test` (links against stock static zlib-ng in both
flavours and libdeflate) enforce it. `-DPNGFAST_HIDE_SYMBOLS=OFF` (or non-ELF platforms) skips the localization.

## Layout

| path | contents |
|---|---|
| `include/pngfast.h` | public API (versioned, documented) |
| `src/decode.c` | chunk parsing, limits, fused inflate -> unfilter driver, Adam7 |
| `src/unfilter.c` | scalar / SSE2 / AVX2 un-filter kernels, 2-row wavefronts, AVX2 quad-Paeth |
| `src/expand.c` | output transforms (sub-byte unpack, palette / tRNS expansion, 16-bit swap) |
| `src/filter_enc.c` | encoder filters + fused one-pass adaptive cost (SSE2/AVX2, portable scalar) |
| `src/encode.c` | validation, presets / content probing, backend policy (`graphics_cfg` / `photo_cfg`), chunk writer |
| `src/deflate_backend.{c,h}` | the encoder's only deflate interface: libdeflate (levels, png-fast, streamed png-ultra) and zlib-ng |
| `src/metadata.c` | ancillary chunk reader / writer, limits |
| `src/stream.c` | `pf_decoder` (push decoding) and `pf_encoder` (row-push encoding) |
| `src/apng.c` | APNG validation, frame iterator / composition, raw frames, `pf_encode_apng` |
| `src/common.c` | CPU dispatch, allocator hooks, status strings |
| `third_party/zlib-ng` | optional, unmodified zlib-ng `develop` f18efb8, native `zng_*` API ([PNGFAST_VENDOR.md](third_party/zlib-ng/PNGFAST_VENDOR.md)) |
| `third_party/libdeflate` | libdeflate v1.26 + pngfast changes ([PNGFAST_CHANGES.md](third_party/libdeflate/PNGFAST_CHANGES.md), [patch](third_party/libdeflate/pngfast-changes.patch)); symbols renamed `pf_libdeflate_*` |
| `tests/` | conformance (vs png crate refs), round trips, API tests, data |
| `fuzz/` | `fuzz_decode` (+ metadata), `fuzz_roundtrip`, `fuzz_stream` (streaming vs one-shot, APNG) (libFuzzer / AFL++ / standalone mutation driver) |
| `bench/` | `pfbench` (C), `run.py` (driver), `results/` |
| `tools/` | `pngfast-cli`, `rsref` (Rust: png crate / image-rs / fdeflate reference + benchmark) |
| `scripts/` | `sanitize.sh`, `check_symbols.sh`, `gen_libdeflate_patch.sh` |

## Presets

| preset | what it does | compared against |
|---|---|---|
| FASTEST | Up filter + png-ultra (single pass, runs only, sampled per-block Huffman codes), streamed in L2-sized row chunks | png `Compression::Fastest` (Up + fdeflate UltraFast) |
| FAST | adaptive filter (Up after strongly compressible chunks) + png-ultra, streamed | png `Fast` (Adaptive + UltraFast) |
| DEFAULT | row-sample probe (1/6 of rows): photos -> adaptive + png-fast (palette: also Paeth; 16-bit: also libdeflate L6); graphics -> Up-biased adaptive vs None chosen on the sample, then **zlib-ng level 6** (without zlib-ng: libdeflate L4-L8 with tuned search and minimum match 3) | png `Balanced` (Adaptive + flate2 L6, miniz_oxide or zlib-rs) |
| BEST | same probe; photos: libdeflate L8; graphics: **zlib-ng level 9, chain 1024** (without zlib-ng: libdeflate L9, or bounded near-optimal L10 for alpha / 16-bit) | png `High` (Adaptive + flate2 L9) |

### Backends

`pf_encode_options.backend`:

| backend | what runs |
|---|---|
| `PF_BACKEND_AUTO` (default) | per preset and content (measured, see NOTES.md "Phase 4"): libdeflate png-ultra for FASTEST/FAST, png-fast / libdeflate L6-L8 for photos, zlib-ng L6 (DEFAULT) / L9 chain 1024 (BEST) for graphics. Without zlib-ng: same as LIBDEFLATE |
| `PF_BACKEND_LIBDEFLATE` | libdeflate only: exactly the pngfast 1.0 output |
| `PF_BACKEND_ZLIBNG` | every stream through zlib-ng (FASTEST L1 = deflate_quick, FAST L2, DEFAULT L6, BEST L9; filter probing unchanged); `deflate_level` 1..9 and `zlib_strategy` (FILTERED, HUFFMAN_ONLY, RLE) select explicitly. `PF_ERR_UNSUPPORTED` when not built in |

Why two backends: libdeflate's PNG-specific single-pass compressors (png-ultra, png-fast) are unbeatable for speed
and for photos, but on synthetic content (UI, text, alpha graphics, 16-bit) zlib's lazy parser and zlib-ng's
deflate_medium find long far matches that libdeflate's L6-L9 miss; libdeflate needed its slow near-optimal
parser to match zlib-9 sizes there. Decoding always uses the patched libdeflate (faster than zlib-ng inflate, see
Results).

The adaptive filter is the libpng "minimum sum of absolute signed residuals" heuristic over all five filters
(the png crate's Adaptive omits None).

## Design notes

* **Decoder**: one allocation for the output; IDAT data is gathered (or used in place when it is a single
  chunk), inflated into the output buffer offset by a few rows, and the libdeflate progress callback
  un-filters each finished row band immediately (fused). The litlen table has 12 bits and entries can carry
  two literals. Avg/Paeth for bpp 1-8 run as 2-row wavefronts to overlap the serial dependency; bpp 3/4 Paeth
  can process 4 rows at once with AVX2. Format conversion runs one row behind un-filtering.
* **Encoder**: filtering writes directly into the deflate input; the adaptive filter evaluates all five
  filters in a single SIMD pass without storing them. png-fast deflate is a single-pass fdeflate-style parser
  (runs, 8-byte hashed matches, skip-ahead over incompressible regions) that feeds libdeflate's block
  writer, so every block still gets an optimal dynamic Huffman code. Output goes straight behind the IDAT
  header; checksums use libdeflate's PCLMUL/AVX-512 CRC-32 and AVX2/VNNI Adler-32.

## Testing

* `ctest`: API (allocator leak accounting, limits, CRC/signature/truncation errors, argument validation, padded
  strides), conformance (PngSuite + formats corpus, 3 output modes, FNV-64 of every output vs png crate /
  image-rs references in `tests/data/refs.txt`; regenerate with `tests/gen_refs.py`), round trips (every
  preset, every filter, several deflate levels), fuzz smoke runs.
* `scripts/sanitize.sh [ITERS] [SECONDS]`: ASan+UBSan build + ctest (SIMD and portable scalar builds), mutation
  fuzzing; libFuzzer sessions when clang is installed. Every encoder stream in the tests is also inflated by
  stock zlib when it is available (libdeflate alone accepts some invalid streams).
* Fuzzing with AFL++: `afl-fuzz -i tests/data/pngsuite -o out -- build/fuzz_decode`.

## Benchmarks

```sh
cmake --build build -j
(cd tools/rsref && cargo build --release && CARGO_TARGET_DIR=target-zlibrs cargo build --release --features zlibrs)
python3 bench/run.py encode  --rounds 3 --reps 3      # pngfast presets vs png crate presets
python3 bench/run.py decode  --rounds 3 --reps 3 --interlaced   # vs image-rs / png crate
python3 bench/run.py deflate --rounds 3 --reps 5      # raw zlib streams vs fdeflate
```

`run.py` refuses to start when the 1-minute load average exceeds `--max-load`, pins to one core
(`--cpu`, default 3), rotates configuration order per round and reports per-file medians aggregated per group
(MB/s of decoded image bytes; ratio = raw / encoded). Corpus: `PNGFAST_CORPUS` (default the png-next format
corpus: kodim01/03/05, cathedral, screenshot, graphics in 20 color type / depth variants each, plus Adam7
copies). Results of the last run are in `bench/results/`.

## Results (2026-10-10, CST, Intel AVX-512 box shared with other agents, load 0.7-1.5, core 3, medians)

pngfast 1.1 (gcc 12 -O2, zlib-ng backend built in) vs png crate git b2f10c6 / image-rs 0.25.9 / fdeflate.
`png-*` = png crate with flate2's default miniz_oxide backend, `pngz-*` = same with the `zlib-rs` feature.
Cells: MB/s of decoded image bytes @ compression ratio. Raw CSVs / tables: `bench/results/phase4/` (encode_matrix,
decode, inflate; backend sweeps). Phase 3 (libdeflate only) results: `bench/results/phase3_*.md`.

### Encode, fast presets (png Fastest = Up + fdeflate UltraFast, png Fast = Adaptive + UltraFast)

| group | png-fastest | pf-fastest | png-fast | pf-fast |
|---|---|---|---|---|
| graphics/16bit | 1912 @ 8.309 | 1905 @ 11.835 | 1181 @ 10.968 | 1887 @ 11.903 |
| graphics/g8 | 2146 @ 9.729 | 2938 @ 13.007 | 1329 @ 11.795 | 2586 @ 13.004 |
| graphics/ga8 | 895 @ 5.499 | 1278 @ 8.060 | 672 @ 6.810 | 1241 @ 8.173 |
| graphics/p8 | 1896 @ 9.648 | 2324 @ 12.772 | 1106 @ 11.574 | 2117 @ 12.772 |
| graphics/rgb8 | 2384 @ 11.001 | 3744 @ 14.087 | 1454 @ 13.721 | 3395 @ 14.134 |
| graphics/rgba8 | 1096 @ 5.858 | 1149 @ 8.390 | 835 @ 7.741 | 1094 @ 8.468 |
| graphics/sub8 | 1664 @ 7.702 | 1754 @ 10.287 | 974 @ 8.790 | 1275 @ 10.418 |
| photo/16bit | 362 @ 1.312 | 547 @ 1.435 | 299 @ 1.348 | 563 @ 1.470 |
| photo/g8 | 531 @ 1.726 | 836 @ 1.808 | 422 @ 1.861 | 664 @ 1.951 |
| photo/ga8 | 554 @ 2.341 | 605 @ 2.522 | 443 @ 2.483 | 581 @ 2.669 |
| photo/p8 | 425 @ 1.750 | 468 @ 1.978 | 344 @ 1.755 | 532 @ 1.968 |
| photo/rgb8 | 486 @ 1.675 | 773 @ 1.755 | 397 @ 1.796 | 670 @ 1.874 |
| photo/rgba8 | 497 @ 1.936 | 754 @ 1.985 | 420 @ 2.063 | 633 @ 2.113 |
| photo/sub8 | 385 @ 1.509 | 524 @ 2.158 | 308 @ 1.538 | 512 @ 2.158 |
| ALL | 461 @ 1.703 | 670 @ 1.875 | 375 @ 1.778 | 659 @ 1.936 |

### Encode, strong presets (png Balanced = Adaptive + flate2 L6, High = Adaptive + flate2 L9)

| group | png-balanced | pngz-balanced | pf-default | png-high | pngz-high | pf-best |
|---|---|---|---|---|---|---|
| graphics/16bit | 191 @ 39.708 | 317 @ 37.603 | 426 @ 40.674 | 67 @ 40.573 | 41 @ 42.345 | 96 @ 45.950 |
| graphics/g8 | 194 @ 32.794 | 296 @ 33.606 | 431 @ 37.847 | 75 @ 32.715 | 48 @ 35.154 | 109 @ 39.817 |
| graphics/ga8 | 80 @ 18.187 | 127 @ 18.661 | 155 @ 19.257 | 26 @ 19.145 | 15 @ 20.689 | 31 @ 20.553 |
| graphics/p8 | 157 @ 26.299 | 230 @ 27.102 | 316 @ 31.178 | 62 @ 26.269 | 41 @ 28.386 | 87 @ 33.758 |
| graphics/rgb8 | 167 @ 34.483 | 437 @ 32.704 | 589 @ 36.298 | 43 @ 34.573 | 46 @ 36.173 | 139 @ 42.242 |
| graphics/rgba8 | 123 @ 24.980 | 162 @ 24.841 | 216 @ 25.930 | 39 @ 25.895 | 19 @ 27.964 | 43 @ 28.031 |
| graphics/sub8 | 112 @ 28.214 | 145 @ 28.987 | 273 @ 34.192 | 49 @ 29.524 | 29 @ 32.881 | 84 @ 37.836 |
| photo/16bit | 12 @ 1.565 | 34 @ 1.584 | 46 @ 1.586 | 6 @ 1.574 | 11 @ 1.569 | 24 @ 1.592 |
| photo/g8 | 12 @ 1.776 | 42 @ 1.811 | 350 @ 1.966 | 9 @ 1.778 | 12 @ 1.766 | 30 @ 1.966 |
| photo/ga8 | 9 @ 2.615 | 24 @ 2.642 | 331 @ 2.724 | 3 @ 2.661 | 7 @ 2.677 | 10 @ 2.727 |
| photo/p8 | 18 @ 1.936 | 41 @ 1.961 | 113 @ 2.037 | 8 @ 1.942 | 11 @ 1.942 | 22 @ 2.055 |
| photo/rgb8 | 13 @ 1.770 | 44 @ 1.796 | 489 @ 1.888 | 10 @ 1.773 | 14 @ 1.769 | 31 @ 1.913 |
| photo/rgba8 | 10 @ 2.012 | 38 @ 2.024 | 359 @ 2.142 | 6 @ 2.023 | 9 @ 2.019 | 21 @ 2.178 |
| photo/sub8 | 11 @ 1.974 | 34 @ 1.996 | 213 @ 2.148 | 4 @ 1.991 | 6 @ 1.996 | 18 @ 2.163 |
| ALL | 14 @ 2.022 | 41 @ 2.045 | 79 @ 2.086 | 8 @ 2.032 | 12 @ 2.029 | 26 @ 2.101 |

### Encode with the backend forced (`#ld` = PF_BACKEND_LIBDEFLATE = pngfast 1.0, `#zng` = PF_BACKEND_ZLIBNG)

| group | pf-default-ld | pf-default-zng | pf-best-ld | pf-best-zng | pf-fastest-zng | pf-fast-zng |
|---|---|---|---|---|---|---|
| graphics/16bit | 359 @ 39.272 | 422 @ 40.674 | 16 @ 45.479 | 52 @ 46.764 | 1575 @ 21.807 | 941 @ 31.189 |
| graphics/g8 | 335 @ 34.830 | 444 @ 37.847 | 109 @ 37.438 | 75 @ 40.225 | 1679 @ 20.433 | 855 @ 29.368 |
| graphics/ga8 | 142 @ 18.748 | 147 @ 19.257 | 8 @ 21.817 | 16 @ 21.360 | 937 @ 9.901 | 567 @ 14.159 |
| graphics/p8 | 325 @ 29.531 | 312 @ 31.178 | 87 @ 33.045 | 65 @ 33.998 | 1464 @ 17.227 | 748 @ 23.100 |
| graphics/rgb8 | 449 @ 35.737 | 578 @ 36.298 | 165 @ 39.789 | 78 @ 42.838 | 2004 @ 22.508 | 1105 @ 31.253 |
| graphics/rgba8 | 191 @ 25.586 | 212 @ 25.930 | 11 @ 28.025 | 21 @ 29.080 | 1194 @ 13.772 | 700 @ 18.820 |
| graphics/sub8 | 292 @ 29.289 | 268 @ 34.192 | 71 @ 36.038 | 52 @ 37.988 | 1409 @ 16.604 | 630 @ 22.954 |
| photo/16bit | 45 @ 1.586 | 36 @ 1.589 | 23 @ 1.592 | 12 @ 1.574 | 123 @ 1.174 | 80 @ 1.562 |
| photo/g8 | 526 @ 1.966 | 47 @ 1.812 | 30 @ 1.966 | 13 @ 1.767 | 123 @ 1.196 | 80 @ 1.786 |
| photo/ga8 | 316 @ 2.724 | 25 @ 2.644 | 10 @ 2.727 | 8 @ 2.679 | 187 @ 1.592 | 121 @ 2.449 |
| photo/p8 | 103 @ 2.037 | 36 @ 2.004 | 22 @ 2.055 | 12 @ 1.987 | 149 @ 1.481 | 100 @ 1.918 |
| photo/rgb8 | 461 @ 1.888 | 48 @ 1.798 | 31 @ 1.913 | 15 @ 1.770 | 128 @ 1.195 | 79 @ 1.772 |
| photo/rgba8 | 341 @ 2.142 | 39 @ 2.026 | 20 @ 2.178 | 10 @ 2.021 | 128 @ 1.327 | 94 @ 1.958 |
| photo/sub8 | 215 @ 2.148 | 37 @ 2.046 | 18 @ 2.163 | 7 @ 2.044 | 159 @ 1.422 | 98 @ 1.988 |
| ALL | 78 @ 2.085 | 44 @ 2.055 | 21 @ 2.101 | 13 @ 2.038 | 153 @ 1.460 | 99 @ 2.006 |

AUTO equals `#ld` for FASTEST/FAST and photos (same code path; differences are noise) and `#zng` for DEFAULT
graphics; BEST graphics uses zlib-ng L9 with chain 1024 (between the two BEST columns in speed).

### Decode (image-rs API vs pngfast IMAGERS output; png crate IDENTITY vs pngfast RAW)

Decoder unchanged in 1.1; re-measured as a no-regression check (phase 3 ALL: pf-imagers 389 / imagers 261,
now 359 / 252 under higher load, same ratio within noise; every group still ahead).

| group | pf-imagers | pf-raw | imagers | png-raw |
|---|---|---|---|---|
| graphics/16bit | 1403 | 2519 | 972 | 1546 |
| graphics/g8 | 831 | 1151 | 291 | 1000 |
| graphics/ga8 | 961 | 948 | 618 | 603 |
| graphics/p8 | 1824 | 5248 | 1047 | 2867 |
| graphics/rgb8 | 1658 | 2371 | 693 | 1541 |
| graphics/rgba8 | 1877 | 1834 | 1065 | 1083 |
| graphics/sub8 | 560 | 5108 | 286 | 2680 |
| photo/16bit | 302 | 346 | 233 | 265 |
| photo/g8 | 270 | 300 | 126 | 193 |
| photo/ga8 | 419 | 433 | 251 | 253 |
| photo/p8 | 413 | 548 | 318 | 398 |
| photo/rgb8 | 310 | 341 | 190 | 225 |
| photo/rgba8 | 351 | 402 | 261 | 263 |
| photo/sub8 | 283 | 553 | 170 | 380 |
| ALL | 359 | 423 | 252 | 304 |

Inflate (MB/s) incl. zlib-ng: libdeflate stays the decoder (patched pair decoding 570 vs zlib-ng 363 MB/s).

| group | libdeflate | pf-pairs | zlib-ng | fdeflate |
|---|---|---|---|---|
| stream from fdeflate-1 | 383 | 664 | 367 | 517 |
| stream from fdeflate-ultrafast | 391 | 640 | 379 | 515 |
| stream from libdeflate-pf | 387 | 643 | 378 | 573 |
| stream from orig | 389 | 553 | 362 | 459 |
| stream from zng6 | 383 | 426 | 334 | 331 |
| ALL | 387 | 570 | 363 | 462 |

### Raw zlib (filtered IDAT data; fdeflate vs pngfast's libdeflate)

| group | fdeflate-ultrafast | fdeflate-rle | fdeflate-1 | fdeflate-6 | pf-rle | pf-fast | pf-6 |
|---|---|---|---|---|---|---|---|
| graphics | 1041 @ 1.805 | 544 @ 2.721 | 281 @ 2.947 | 74 @ 3.561 | 796 @ 2.894 | 618 @ 3.125 | 87 @ 3.638 |
| photo | 460 @ 1.549 | 399 @ 1.665 | 399 @ 1.665 | 34 @ 1.640 | 672 @ 1.678 | 599 @ 1.678 | 55 @ 1.637 |
| ALL | 518 @ 1.594 | 422 @ 1.805 | 368 @ 1.824 | 38 @ 1.839 | 693 @ 1.832 | 603 @ 1.849 | 59 @ 1.840 |

| group | fdeflate | libdeflate | pf-pairs |
|---|---|---|---|
| stream from fdeflate-1 | 474 | 350 | 618 |
| stream from fdeflate-ultrafast | 470 | 357 | 528 |
| stream from libdeflate-pf | 501 | 370 | 558 |
| stream from orig | 403 | 361 | 477 |
| stream from zng6 | 299 | 362 | 391 |
| ALL | 415 | 360 | 502 |

### Where the png crate still wins
* **png High with zlib-rs, graphics ga8**: BEST 31 MB/s @ 20.55 vs 15 @ 20.69 (0.7% larger, 2x faster).
  zlib-ng chain 1536 reaches 20.83 but then loses speed to miniz High (26 vs 26-28 MB/s).
* **png Fastest, graphics 16-bit**: speed tie (1905 vs 1912 MB/s) at 42% better ratio.
* Everything else: each pngfast preset is faster *and* smaller than its png counterpart with either flate2
  backend in every group (phase-3 losses on DEFAULT rgb8/16-bit and BEST alpha/16-bit are closed by zlib-ng).
* Raw deflate on graphics: fdeflate UltraFast is still faster as a raw compressor (table above: 1041 vs 796 MB/s).

## License

MIT; see `LICENSE`. Bundled third-party code keeps its own license: `third_party/libdeflate` (MIT) and `third_party/zlib-ng` (zlib license).
