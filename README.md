# pngfast

Fast, small PNG decoder and encoder in C11 (x86-64), built on a vendored and modified
[libdeflate](https://github.com/ebiggers/libdeflate). Single header API, no other dependencies,
runtime CPU dispatch (SSE2 baseline, AVX2 / BMI2 / AVX-512 where available; never `-march=native`).

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
| `src/filter_enc.c` | encoder filters + fused one-pass adaptive cost (SSE2/AVX2) |
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
| FASTEST | Up filter, run-only png-fast deflate | png `Compression::Fastest` (Up + fdeflate UltraFast) |
| FAST | adaptive filter, png-fast deflate | png `Fast` (Adaptive + UltraFast) |
| DEFAULT | probe with FAST; photos: keep (palette/16-bit: also libdeflate L6, keep smaller); graphics (ratio >= 6): also try filter None, recompress best filter at L6 (L7 for alpha / 16-bit) | png `Balanced` (Adaptive + flate2 L6, miniz_oxide or zlib-rs) |
| BEST | photos: libdeflate L10 (near-optimal parse); graphics: filter trial + L9 | png `High` (Adaptive + flate2 L9) |

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
* `scripts/sanitize.sh [ITERS] [SECONDS]`: ASan+UBSan build, ctest, mutation fuzzing; libFuzzer sessions when
  clang is installed.
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
