# Vendored libdeflate: pngfast modifications

Base: upstream libdeflate **v1.26** (commit `92e6a0d`), MIT licence (`COPYING`).
Full diff against upstream: [`pngfast-changes.patch`](pngfast-changes.patch) (regenerate with
`scripts/gen_libdeflate_patch.sh`). Only four files differ from upstream:
`libdeflate.h`, `lib/deflate_compress.c`, `lib/deflate_decompress.c`, `lib/decompress_template.h`.
The gzip wrapper sources (`gzip_*.c`) are not vendored.

All symbols are renamed `libdeflate_*` -> `pf_libdeflate_*` at compile time (`pf_rename.h`, force-included),
so pngfast links cleanly next to a system libdeflate. Nothing from this library is part of pngfast's public API.

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
  64-bit load. Used by pngfast's FASTEST preset.

API: `libdeflate_set_png_params(c, block_len, skip_shift)`, `libdeflate_set_png_min_match(c, min_len)`.
`libdeflate_alloc_compressor(100)` is the only way to select the mode; levels 0..12 are unchanged.

## Decompressor

* **Progress callback** - `libdeflate_deflate_decompress_progress(..., cb, ctx, step)`: the fast loop also
  exits every `step` output bytes and calls `cb(ctx, out_next)`. pngfast un-filters completed rows from the
  callback while they are still in L1/L2 ("fused inflate -> unfilter"). The output buffer is no longer
  `restrict` (the callback reads it).
* **Literal pairs** - `libdeflate_set_pair_literals(d, 1)`: main-table entries may decode two literals at
  once (12-bit main table when pairs are on; 11 bits otherwise, i.e. upstream behaviour when off).
  Entry layout documented at `HUFFDEC_LITERAL_PAIR` in `deflate_decompress.c`.

Both features are opt-in; with them off the decompressor behaves (and performs) like upstream.

## Not changed

Checksums (`adler32`, `crc32` incl. the VPCLMULQDQ/AVX-512 paths), CPU feature detection and levels 0-12 are
upstream code. pngfast uses `libdeflate_crc32` / `libdeflate_adler32` for chunk CRCs and zlib checksums.
