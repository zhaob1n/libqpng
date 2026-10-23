# Vendored libdeflate: qpng modifications

Base: upstream libdeflate **v1.26** (commit `92e6a0d`), MIT licence (`COPYING`).
Full diff against upstream: [`qpng-changes.patch`](qpng-changes.patch) (regenerate with
`scripts/gen_libdeflate_patch.sh`). Only four files differ from upstream:
`libdeflate.h`, `lib/deflate_compress.c`, `lib/deflate_decompress.c`, `lib/decompress_template.h`.
The gzip wrapper sources (`gzip_*.c`) are not vendored.

All symbols are renamed `libdeflate_*` -> `qpng_libdeflate_*` at compile time (`qpng_rename.h`, force-included),
so qpng links cleanly next to a system libdeflate. Nothing from this library is part of qpng's public API.

## Compressor: "png-fast" mode (`LIBDEFLATE_PNG_FAST_LEVEL` = 100)

A single-pass parser for PNG-filtered scanlines in the style of fdeflate, emitting ordinary DEFLATE through
libdeflate's own block machinery (per-block optimal dynamic Huffman codes, stored/static/dynamic choice):

* offset-1 run detection (5 equal bytes) with backward/forward extension;
* hashed matches of >= `min_len` bytes (default 8, verified by a 64-bit compare), single-entry table,
  inside-of-match indexing;
* literal skip-ahead: after k literals the probe step grows by `k >> skip_shift` (incompressible regions
  are almost free);
* short matches (< 8) are cost-gated against the running literal cost estimate of the previous block;
* **adaptive hashing**: after each block the parser estimates what its hashed matches saved (match length x
  current literal cost - match cost). If that is under 1/8 of the block's literal cost and literals are at
  least a quarter of the block (filtered photos: only runs pay), the next blocks are parsed run-only
  (3, doubling per consecutive failed probe up to 48) before hashing is re-tried. Photos: 1.2-3x faster and
  slightly smaller; graphics unchanged;
* **run-only mode** (`min_len == 0`): no hash table at all; a SWAR test checks 4 candidate run starts per
  64-bit load. Used by qpng's FASTEST preset.

API: `libdeflate_set_png_params(c, block_len, skip_shift)`, `libdeflate_set_png_min_match(c, min_len)`.
`libdeflate_alloc_compressor(100)` is the only way to select the mode; levels 0..12 use upstream parsers.

## Compressor: "png-ultra" mode (`LIBDEFLATE_PNG_ULTRA_LEVEL` = 101)

Single pass, run-only parse (offset-1 runs found with 4x64-bit SWAR compares, everything else literal), no
per-block optimal-code pass: each block's litlen/offset codes are built from a sampled histogram (a strided
subset of the block plus the run lengths found), so the block is read once for counting and once for coding.
Blocks grow adaptively (64 KiB, up to 256 KiB while data compresses well). Litlen symbols 286/287 are never
given codes (stock zlib rejects them). Output room is checked in chunks, so no scratch buffer is needed.
Streaming API for row-chunked input (qpng filters an L2-sized slice of rows at a time and never
materialises the filtered image):
`libdeflate_png_ultra_compress_chunk(c, &state, in, len, is_final, out, avail)` - a zeroed
`struct libdeflate_png_ultra_state` carries the bit buffer between chunks (runs may only reference the previous
chunk's last byte, `in[-1]`); returns whole bytes written (raw DEFLATE). Used by qpng's FASTEST and FAST presets.

## Compressor: match finder parameters for levels 2-12

`libdeflate_set_search_params(c, max_search_depth, nice_match_length)` overrides the per-level defaults
(0 = keep). qpng uses it for content-tuned DEFAULT/BEST levels (e.g. L8 depth 48 nice 258 for alpha
graphics; L10 depth 48 nice 96 for alpha / 16-bit graphics in BEST - bounded near-optimal: nice 258 is ~2x
slower on long-run data because every position offers 258-byte matches whose lengths are all costed).

## Compressor: fixed minimum match length for levels 2-12

`libdeflate_set_min_match_len(c, n)` (3..9; 0 = upstream heuristic). Upstream raises the minimum match length
up to 9 when few distinct literal values are in use; filtered PNG graphics use few literals yet still profit
from 3-byte matches (token-level comparison with zlib-rs level 9 on an RGBA screenshot). qpng sets 3 on
its probed graphics paths: -1.5% (DEFAULT) / -0.6% (BEST) on the graphics corpus, slightly faster.

## Decompressor

* **Progress callback** - `libdeflate_deflate_decompress_progress(..., cb, ctx, step)`: the fast loop also
  exits every `step` output bytes and calls `cb(ctx, out_next)`. qpng un-filters completed rows from the
  callback while they are still in L1/L2 ("fused inflate -> unfilter"). The output buffer is no longer
  `restrict` (the callback reads it).
* **Literal pairs** - `libdeflate_set_pair_literals(d, 1)`: main-table entries may decode two literals at
  once (12-bit main table when pairs are on; 11 bits otherwise, i.e. upstream behaviour when off).
  Entry layout documented at `HUFFDEC_LITERAL_PAIR` in `deflate_decompress.c`.

Both features are opt-in; with them off the decompressor behaves (and performs) like upstream.

## Not changed

Checksums (`adler32`, `crc32` incl. the VPCLMULQDQ/AVX-512 paths), CPU feature detection and the level 0-12
parsers are upstream code. qpng uses `libdeflate_crc32` / `libdeflate_adler32` for chunk CRCs and zlib checksums.
