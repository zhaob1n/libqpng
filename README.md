# pngfast

Fast, small PNG decoder and encoder in C11, built on a vendored and modified
[libdeflate](https://github.com/ebiggers/libdeflate). Single header API, no other dependencies,
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
ctest --test-dir build              # api, conformance, roundtrip, fuzz smoke
cmake --install build --prefix /usr/local   # installs pngfast.h, libpngfast, pngfast.pc, CMake package
```

Options: `PNGFAST_BUILD_SHARED` (default OFF), `PNGFAST_BUILD_TESTS`, `PNGFAST_BUILD_TOOLS`,
`PNGFAST_SANITIZE` (ASan+UBSan), `PNGFAST_LIBFUZZER` (clang), `PNGFAST_CORPUS_DIR` (extra round-trip corpus).
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

## Layout

| path | contents |
|---|---|
| `include/pngfast.h` | public API (versioned, documented) |
| `src/decode.c` | chunk parsing, limits, fused inflate -> unfilter driver, Adam7 |
| `src/unfilter.c` | scalar / SSE2 / AVX2 un-filter kernels, 2-row wavefronts, AVX2 quad-Paeth |
| `src/expand.c` | output transforms (sub-byte unpack, palette / tRNS expansion, 16-bit swap) |
| `src/filter_enc.c` | encoder filters + fused one-pass adaptive cost (SSE2/AVX2, portable scalar) |
| `src/encode.c` | validation, presets / content probing, chunk writer |
| `src/common.c` | CPU dispatch, allocator hooks, status strings |
| `third_party/libdeflate` | libdeflate v1.26 + pngfast changes ([PNGFAST_CHANGES.md](third_party/libdeflate/PNGFAST_CHANGES.md), [patch](third_party/libdeflate/pngfast-changes.patch)); symbols renamed `pf_libdeflate_*` |
| `tests/` | conformance (vs png crate refs), round trips, API tests, data |
| `fuzz/` | `fuzz_decode`, `fuzz_roundtrip` (libFuzzer / AFL++ / standalone mutation driver) |
| `bench/` | `pfbench` (C), `run.py` (driver), `results/` |
| `tools/` | `pngfast-cli`, `rsref` (Rust: png crate / image-rs / fdeflate reference + benchmark) |
| `scripts/` | `sanitize.sh`, `gen_libdeflate_patch.sh` |

## Presets

| preset | what it does | compared against |
|---|---|---|
| FASTEST | Up filter + png-ultra (single pass, runs only, sampled per-block Huffman codes), streamed in L2-sized row chunks | png `Compression::Fastest` (Up + fdeflate UltraFast) |
| FAST | adaptive filter (Up after strongly compressible chunks) + png-ultra, streamed | png `Fast` (Adaptive + UltraFast) |
| DEFAULT | row-sample probe (1/6 of rows): photos -> adaptive + png-fast (palette: also Paeth; 16-bit: also L6); graphics -> Up-biased adaptive vs None chosen on the sample, then libdeflate L4-L8 with tuned search and minimum match 3 | png `Balanced` (Adaptive + flate2 L6, miniz_oxide or zlib-rs) |
| BEST | same probe; photos: libdeflate L8; graphics: L9, or bounded near-optimal L10 (depth 48, nice 96) for alpha / 16-bit | png `High` (Adaptive + flate2 L9) |

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

## Results (2026-10-09, CST, Intel AVX-512 box shared with other agents, load 0.2-1.7, core 3, medians)

pngfast 1.0 (this repo, gcc 12 -O2) vs png crate git b2f10c6 / image-rs 0.25.9 / fdeflate (Rust 1.x release).
`png-*` = png crate with flate2's default miniz_oxide backend, `pngz-*` = same with the `zlib-rs` feature.
Cells: MB/s of decoded image bytes @ compression ratio. Raw CSVs: `bench/results/final/`.

### Encode, fast presets (png Fastest = Up + fdeflate UltraFast, png Fast = Adaptive + UltraFast)

| group | png-fastest | pf-fastest | png-fast | pf-fast |
|---|---|---|---|---|
| graphics/16bit | 1787 @ 8.309 | 1066 @ 12.509 | 998 @ 10.968 | 790 @ 28.546 |
| graphics/g8 | 2157 @ 9.729 | 1844 @ 13.571 | 992 @ 11.795 | 624 @ 29.096 |
| graphics/ga8 | 879 @ 5.499 | 826 @ 8.331 | 613 @ 6.810 | 494 @ 15.219 |
| graphics/p8 | 1835 @ 9.648 | 1621 @ 13.476 | 1063 @ 11.574 | 589 @ 23.974 |
| graphics/rgb8 | 2113 @ 11.001 | 2158 @ 14.669 | 1346 @ 13.721 | 935 @ 31.236 |
| graphics/rgba8 | 1021 @ 5.858 | 708 @ 8.598 | 689 @ 7.741 | 592 @ 17.634 |
| graphics/sub8 | 1686 @ 7.702 | 1304 @ 10.478 | 781 @ 8.790 | 479 @ 25.862 |
| photo/16bit | 327 @ 1.312 | 408 @ 1.444 | 265 @ 1.348 | 227 @ 1.535 |
| photo/g8 | 473 @ 1.726 | 589 @ 1.822 | 379 @ 1.861 | 447 @ 1.966 |
| photo/ga8 | 466 @ 2.341 | 479 @ 2.569 | 407 @ 2.483 | 342 @ 2.724 |
| photo/p8 | 385 @ 1.750 | 341 @ 1.993 | 299 @ 1.755 | 218 @ 1.976 |
| photo/rgb8 | 457 @ 1.675 | 574 @ 1.766 | 363 @ 1.796 | 446 @ 1.888 |
| photo/rgba8 | 458 @ 1.936 | 521 @ 2.010 | 327 @ 2.063 | 443 @ 2.142 |
| photo/sub8 | 338 @ 1.509 | 375 @ 2.170 | 278 @ 1.538 | 227 @ 2.148 |
| ALL | 418 @ 1.703 | 488 @ 1.890 | 330 @ 1.778 | 297 @ 2.031 |

### Encode, strong presets (png Balanced = Adaptive + flate2 L6, High = Adaptive + flate2 L9)

| group | png-balanced | pngz-balanced | pf-default | png-high | pngz-high | pf-best |
|---|---|---|---|---|---|---|
| graphics/16bit | 178 @ 39.708 | 287 @ 37.603 | 132 @ 40.699 | 62 @ 40.573 | 35 @ 42.345 | 68 @ 42.145 |
| graphics/g8 | 188 @ 32.794 | 274 @ 33.606 | 169 @ 35.053 | 68 @ 32.715 | 39 @ 35.154 | 78 @ 37.186 |
| graphics/ga8 | 78 @ 18.187 | 117 @ 18.661 | 66 @ 18.181 | 24 @ 19.145 | 13 @ 20.689 | 25 @ 19.357 |
| graphics/p8 | 154 @ 26.299 | 200 @ 27.102 | 150 @ 29.760 | 57 @ 26.269 | 34 @ 28.386 | 62 @ 32.764 |
| graphics/rgb8 | 153 @ 34.483 | 365 @ 32.704 | 194 @ 38.186 | 40 @ 34.573 | 37 @ 36.173 | 109 @ 39.153 |
| graphics/rgba8 | 112 @ 24.980 | 145 @ 24.841 | 84 @ 24.860 | 35 @ 25.895 | 17 @ 27.964 | 34 @ 25.817 |
| graphics/sub8 | 108 @ 28.214 | 126 @ 28.987 | 129 @ 30.794 | 44 @ 29.524 | 25 @ 32.881 | 53 @ 35.146 |
| photo/16bit | 11 @ 1.565 | 30 @ 1.584 | 41 @ 1.586 | 6 @ 1.574 | 10 @ 1.569 | 12 @ 1.618 |
| photo/g8 | 11 @ 1.776 | 38 @ 1.811 | 480 @ 1.966 | 9 @ 1.778 | 10 @ 1.766 | 11 @ 1.966 |
| photo/ga8 | 8 @ 2.615 | 22 @ 2.642 | 324 @ 2.724 | 3 @ 2.661 | 6 @ 2.677 | 6 @ 2.853 |
| photo/p8 | 15 @ 1.936 | 37 @ 1.961 | 107 @ 2.037 | 7 @ 1.942 | 11 @ 1.942 | 7 @ 2.099 |
| photo/rgb8 | 11 @ 1.770 | 39 @ 1.796 | 452 @ 1.888 | 9 @ 1.773 | 12 @ 1.769 | 12 @ 1.927 |
| photo/rgba8 | 10 @ 2.012 | 32 @ 2.024 | 398 @ 2.142 | 5 @ 2.023 | 8 @ 2.019 | 9 @ 2.196 |
| photo/sub8 | 9 @ 1.974 | 31 @ 1.996 | 219 @ 2.148 | 4 @ 1.991 | 6 @ 1.996 | 7 @ 2.202 |
| ALL | 13 @ 2.022 | 37 @ 2.045 | 67 @ 2.086 | 7 @ 2.032 | 10 @ 2.029 | 12 @ 2.130 |

### Decode (image-rs API vs pngfast IMAGERS output; png crate IDENTITY vs pngfast RAW; Adam7 copies included)

| group | imagers | png-raw | v3-imagers | pf-imagers | pf-raw |
|---|---|---|---|---|---|
| graphics/16bit | 720 | 1045 | 1210 | 938 | 1575 |
| graphics/g8 | 265 | 874 | 679 | 658 | 727 |
| graphics/ga8 | 532 | 453 | 820 | 690 | 545 |
| graphics/p8 | 377 | 1417 | 1116 | 745 | 2168 |
| graphics/rgb8 | 536 | 848 | 1375 | 1009 | 1431 |
| graphics/rgba8 | 661 | 685 | 1407 | 1119 | 1240 |
| graphics/sub8 | 148 | 350 | 353 | 235 | 286 |
| photo/16bit | 198 | 236 | 277 | 271 | 272 |
| photo/g8 | 108 | 179 | 241 | 251 | 222 |
| photo/ga8 | 221 | 230 | 383 | 353 | 277 |
| photo/p8 | 255 | 347 | 336 | 365 | 430 |
| photo/rgb8 | 176 | 203 | 287 | 282 | 293 |
| photo/rgba8 | 231 | 212 | 326 | 338 | 367 |
| photo/sub8 | 127 | 310 | 232 | 235 | 339 |
| ALL | 226 | 283 | 348 | 336 | 352 |

Graphics rows re-run at low load with 15 reps (the small graphics files are noise-sensitive):

| group | imagers | png-raw | v3-imagers | pf-imagers | pf-raw |
|---|---|---|---|---|---|
| graphics/16bit | 778 | 1152 | 948 | 951 | 1279 |
| graphics/g8 | 264 | 947 | 694 | 733 | 1019 |
| graphics/ga8 | 544 | 514 | 762 | 807 | 839 |
| graphics/p8 | 442 | 1615 | 1126 | 1156 | 2668 |
| graphics/rgb8 | 554 | 971 | 1294 | 1393 | 1899 |
| graphics/rgba8 | 730 | 737 | 1343 | 1437 | 1396 |
| graphics/sub8 | 170 | 382 | 325 | 384 | 397 |
| ALL | 583 | 958 | 919 | 953 | 1223 |

`v3-imagers` is the pre-cleanup prototype, kept only as a regression check.

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
* **png Fastest on graphics**: pngfast FASTEST compresses 33-52% better but is 6-40% slower on 6 of 7 graphics groups
  (16-bit 1066 vs 1787 MB/s, rgba8 708 vs 1021). fdeflate UltraFast writes a fixed Huffman code in one pass;
  pngfast builds per-block optimal codes (histogram + bit writer = ~40% of the time). Photo p8: 11% slower
  (but 14% smaller).
* **png Balanced with zlib-rs on graphics**: zlib-rs L6 is 1.3-2.2x faster than DEFAULT on 6 of 7 graphics
  groups; DEFAULT is 3-17% smaller except ga8 (-2.6%) and rgba8 (tie). Against miniz_oxide Balanced, DEFAULT
  is smaller on 5/7 graphics groups and slower on 5/7.
* **png High with zlib-rs on alpha graphics**: BEST is faster everywhere but 6-8% larger on ga8/rgba8 and 0.5%
  on 16-bit graphics (zlib's lazy parser beats libdeflate L9/L10 on that content even at search depth 4096).
* **png Fast**: no single pngfast preset dominates it everywhere; FAST is always much smaller (graphics
  ratio 2-3x) but slower on graphics; FASTEST dominates png Fast (faster *and* smaller) on every graphics group
  and on photo 16-bit/ga8/p8/sub8, FAST dominates it on photo g8/rgb8/rgba8.
* Raw deflate on graphics: fdeflate UltraFast 1041 MB/s vs pngfast run-only 796 MB/s (ratio 1.81 vs 2.89).
