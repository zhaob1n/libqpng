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
