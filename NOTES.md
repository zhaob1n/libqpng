# pngfast engineering notes (progress log)

Box is shared with other agents: all headline numbers are taken with `bench/run.py` (load check, `taskset -c 3`,
rotating config order per round, per-file medians). Times are CST.

## 2026-10-09 21:16  phase start: project structure
* Repo `/workspace/pngfast`: `include/pngfast.h` (stable API), `src/{common,decode,unfilter,expand,filter_enc,encode}.c`,
  vendored modified libdeflate (`third_party/libdeflate`, symbols renamed `pf_libdeflate_*`), CMake + pkg-config,
  CLI, bench tool, Rust reference tool (`tools/rsref`), tests (PngSuite + 40-file format corpus vs png crate
  references, round trips, API/limits/allocator), fuzz targets, sanitizer script.
* Decoder ported from png-next v3 unchanged in substance. Dropped experimental switches that measured no gain:
  PF2_HUGE (hugepages), PF2_NO_FUSE, PF_NO_WAVEFRONT, PF_SCALAR, PF2_NO_QUAD, PF2_QUAD_ALL, PF3_NO_PAIRS12, the
  generic (non-3/4 bpp) quad-Paeth kernel and the `flags` argument of unfilter_row.
* New check: IDAT chunks must be consecutive (spec; png crate also rejects).

## Why png-fast deflate (581 MB/s raw) did not make end-to-end encode much faster
Instrumented old encoder (pngfast3_enc), photo rgb8 (kodim01/03/05 + cathedral, 20 encodes, 113 MB raw):

| stage | adaptive filter | Paeth only |
|---|---|---|
| alloc + filter | 122.7 ms (31%) | 64.1 ms |
| deflate (png-fast) | 266.1 ms (67%) = 427 MB/s, of which Adler-32 9.5 ms | 275 ms |
| chunks + CRC | 6.2 ms (2%) | - |

1. The 581 MB/s figure was measured on different IDAT data (deflate-bench set); on the format corpus the
   png-fast compressor runs at ~430 MB/s.
2. The old adaptive filter materialised all 5 filtered rows, summed them and memcpy'd the winner: ~1/3 of e2e.
=> Fix: fused SIMD (SSE2/AVX2 runtime-dispatched) cost pass that computes all five |residual| sums in one sweep
   without storing anything, then writes only the chosen filter straight into the deflate input.
   New profile (photo rgb8, FAST): deflate_flush_block 29%, png-fast parser 18%, literal histogram 17%,
   filter cost pass 12%, filter write 5%, Adler-32 4%, CRC 2% (rest: source decode in the bench tool).
   Encoder e2e on those 4 photos 68.0 -> 60.6 ms (same bytes); adaptive now costs about the same as Paeth-only.
   The remainder is deflate itself (~70%): Huffman histogram + bit writer, which are already tight
   (tried: sampled histogram -> +0.46% size, no measurable gain; merged codeword/length table -> no gain; both reverted).

## Encoder presets (21:30-22:00)
* png crate (git b2f10c6) presets: Fastest = Up + fdeflate UltraFast, Fast = Adaptive + UltraFast,
  Balanced = Adaptive + flate2 L6 (miniz_oxide or zlib-rs), High = Adaptive + flate2 L9.
* Run-only mode got a SWAR probe (4 run starts per 64-bit load): graphics FASTEST +25-40% speed.
* Level sweep (adaptive filter, libdeflate levels vs png Balanced/High) showed:
  - photos: png-fast already beats Balanced in size for rgb8/rgba8/ga8/g8/sub8; loses 0.4% (p8) and 2-3% (16-bit)
    -> DEFAULT also tries level 6 for palette / 16-bit and keeps the smaller stream.
  - libdeflate L9 is *worse* than png-fast on photos (23.38 vs 22.86 MB on rgb8); L10 (near-optimal) is the
    first level that is clearly smaller -> BEST photo = L10.
  - graphics: filter choice dominates; best of {adaptive, None} at L6 beats png Balanced overall
    (1.995 vs 2.028 MB on 40 graphics files) -> DEFAULT/BEST probe with png-fast (cheap), try filter None,
    keep the better filter, recompress at L6/L7 (alpha, 16-bit) or L9 (BEST).
  - alpha graphics: zlib-9 (png High with zlib-rs) finds 4-5% smaller streams than libdeflate L9-L10 even with
    max_search_depth 4096 (tested, reverted) - an LZ-parsing difference, not search effort.

## 22:00  FAST / DEFAULT / BEST tuning
* First full matrix (bench/results/encode_fast.md) showed FAST losing *speed* to png Fast on photo ga8/p8/sub8
  (129-149 vs 265-330 MB/s). Cause: hashed matches on filtered photos are short and frequent (ga8: 711K
  matches of ~8.7 bytes in 12 MB), each resets the skip-ahead. Adaptive filter + run-only parsing was
  *smaller and 2-3.5x faster* on every 8-bit photo group (only 16-bit lost 4%).
  -> png-fast now adapts per block (see third_party/libdeflate/PNGFAST_CHANGES.md "adaptive hashing").
  First attempt (match coverage) never triggered; second (gain vs block length) mis-fired on screenshots
  (+23% size); final rule (gain vs literal cost, literal-dominated blocks only, backoff) keeps graphics at
  2.633 MB (was 2.683) and makes photo ga8 3x faster (40.7 vs 122 ms, smaller).
* Palette min-match 4/5 no longer helps once the adaptive filter is used (sizes within 0.5%): removed.
* DEFAULT for 8-bit palette photos: trying Paeth-everywhere with png-fast beats adding level 6
  (7.066 MB vs 7.267 MB, 2x faster; png Balanced/zlib-rs 7.337 MB).
* BEST keeps the smaller of the png-fast probe and the L9/L10 stream (L10 alone was not always smaller).

## 22:06-22:14  resume after interruption
* Decoder regression found vs v3 prototype (photo ga8 269 vs 381 MB/s, g8 187 vs 226): caused by CMake's
  Release -O3 (v3 was built -O2); -O3 slows the 2-row wavefront kernels. Release default is now -O2
  (encoder speed unchanged within noise). Re-checked in the final decode matrix (v3-imagers column).
* clang 19 installed via apt: `-Wall -Wextra` clean on gcc 12 and clang 19, ctest passes with both.
* scripts/sanitize.sh (gcc ASan+UBSan): ctest 5/5, 1000 mutations/seed decode + 100/seed round trip: clean.
* libFuzzer (clang, ASan+UBSan, library now instrumented with -fsanitize=fuzzer-no-link):
  fuzz_decode 75 s = 2.39M execs, cov 883, no findings; fuzz_roundtrip 45 s = 45K execs, cov 1713, no findings.
* ctest incl. roundtrip_corpus (160-file format corpus, 13 encoder variants each): 6/6 pass.

## 22:14-22:40  final matrices (bench/results/final/, summarised in README "Results")
* Load 1.2-1.7 during the long runs (other agents), 0.2-1.2 for the graphics decode re-run.
* Decode: pngfast IMAGERS beats image-rs in every group (ALL 336 vs 226 MB/s; photo g8 2.3x, ga8 1.6x);
  RAW beats png IDENTITY in every group at low load. Cleaned decoder = v3 prototype or better (quiet run).
* Encode: FASTEST/FAST/DEFAULT/BEST vs png Fastest/Fast/Balanced/High (miniz + zlib-rs): photos dominated
  (faster and smaller) except FASTEST p8 speed (-11%) and BEST p8 speed vs zlib-rs High (7 vs 11 MB/s).
  Graphics losses listed in README "Where the png crate still wins".
* Raw deflate: run-only 693 MB/s @ 1.832 vs fdeflate UltraFast 518 @ 1.594; inflate +21% vs fdeflate.

## Open / TODO
* FASTEST on graphics: single-pass writer with a predicted (previous-block) Huffman code to skip the
  histogram pass, or an fdeflate-style fixed code for high-ratio blocks.
* DEFAULT on graphics vs zlib-rs Balanced: cheaper filter trial (row-sampled probe) and a faster
  lazy matcher tuned for synthetic content; alpha graphics parsing gap vs zlib-9 (BEST).
* BEST p8 photos: L10 is slow (7 MB/s); a level-9/10 hybrid could match zlib-rs High speed.
* Optional: 3-4 literals per decode lookup; non-x86 (NEON) kernels (build currently #errors on non-x86-64).

## Phase 3 (Oct 10, from 09:25) — closing the gaps
* Gap 1/2 DONE: png-ultra (libdeflate level 101): single pass, offset-1 runs only, per-block Huffman code
  from a sampled dry run (every 8th 512-byte chunk, floor 1 per symbol, symbols 286/287 excluded!),
  4 literals per SWAR probe, 32-byte run extension, block rewound to stored if larger.
  Blocks 64 KiB, 256 KiB after a >= 4:1 block. Chunk API (libdeflate_png_ultra_compress_chunk) lets pngfast
  filter rows into an L2-sized buffer and compress as it goes: no full filtered image -> no 200 page
  faults per MB (was 26% of FASTEST time on graphics). FASTEST = Up + ultra stream; FAST = adaptive
  (Up after a >= 4:1 chunk) + ultra stream. Both now beat png Fastest / Fast in every group (speed+size).
* Bug caught by the new zlib check: litlen symbols 286/287 got codes -> zlib rejects; libdeflate accepts.
  tests/test_deflate.c + roundtrip now inflate every stream with stock zlib.
* Gap 3/4 analysis: zlib-rs L6 (deflate_medium) / L9 compress PNG graphics better than libdeflate L6 / L9
  on the same filtered bytes (graphics.g8: zrs6 9565 vs ld6 10634; rgba8: zrs9 15593 vs ld9 19150).
  Not filtering: png crate adaptive == ours (tested its Up/Sub/Avg/Paeth tie order: no change).
  Not block size (zlib memLevel test), not min match (forced 3..6: no gain), backward match extension
  over literals: +0.3% only (dropped). Deep chains help (ld9/4096/258) but are slow; near-optimal
  (L10+) beats zrs9 in size but bt-matchfinder is pathological on repetitive data (5-8 MB/s).
  CAREFUL: never `git checkout` a file with uncommitted work (lost an hour-old edit once).
* 10:00-10:30 Gap 3 DONE (DEFAULT vs zlib-rs Balanced, graphics): faster AND not larger in every group.
  - probe sample doubles as the filter trial (2 sampled png-fast runs instead of 3; sample written into the
    final filter/output buffers: no extra allocations).
  - sample = 32-row strips every 192 rows (h >= 512; 16/64 for h >= 256): same decisions as 16/64 on the
    corpus, half the cost. Sampling biases against filter None (long-range repeats cut): None wins at <= 104%
    of the Up-biased sample size for depth <= 8 (fixed screenshot.g8: 35.8K -> 32.7K).
  - new internal filter mode PF__FILTER_ADAPTIVE_FLAT = Up-biased adaptive without Paeth for opaque graphics:
    smaller (graphics.rgb8 -1.7%, g16 -1.3%) and the cost pass skips the costliest predictor. Alpha keeps
    Paeth (+5-8% without it).
  - final levels: alpha 8-bit L8/48/258; None-filtered 8-bit RGB L4/4/16 (long exact repeats); other RGB
    L5/8/32; rest L5. Measured (cpu 1, 5 rounds): 16bit 378-388 vs 335 MB/s @ 38.35 vs 37.60; g8 385 vs 296;
    ga8 133 vs 126 @ 18.79 vs 18.66; p8 341 vs 235; rgb8 456-465 vs 437-441 @ 35.37 vs 32.70;
    rgba8 181-184 vs 176 @ 25.50 vs 24.84; sub8 312 vs 145.
* Gap 4: BEST alpha/16-bit graphics = L10 depth 48 nice 96 (bounded near-optimal): ratio 45.2/21.6/27.9 vs
  zrs High 42.3/20.7/28.0 (rgba8 tie within 0.1%... see final matrix), 2x faster than L10/64/258 but still
  slower than zrs High (16 vs 40, 8 vs 15, 11 vs 20 MB/s). Lazy L7-9 even at depth 256 cannot reach zrs9
  size on ga8/rgba8; forcing smaller blocks in the lazy parser changed nothing. Root cause of near-optimal
  slowness: deflate_find_min_cost_path + bt matchfinder on long runs (every length 3..nice costed).
  BEST photos: L8 instead of L10: 2x faster, ~1.5% larger, still smaller and faster than zrs High in every
  photo group (p8 21 vs 11 MB/s @ 2.055 vs 1.942).
* Item 5: portable scalar build (PF_X86 = x86-64 && !PNGFAST_NO_SIMD): scalar filter costs/rows, scalar
  two-row unfilter pairs for every bpp; byte-identical encoder output; ctest 7/7 with -DPNGFAST_NO_SIMD;
  sanitize.sh also runs the portable build. (No aarch64 sysroot on the box to cross-compile.)
