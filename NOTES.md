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

## Open / TODO
* clang not installed on the box: clang -Wall -Wextra build and libFuzzer sessions not run (gcc-only so far).
* Faster FASTEST on graphics (fixed-code single pass like fdeflate UltraFast) if needed.
