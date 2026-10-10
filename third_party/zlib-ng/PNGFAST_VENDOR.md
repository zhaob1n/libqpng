# zlib-ng (vendored, unmodified)

* Source: https://github.com/zlib-ng/zlib-ng branch `develop`, commit f18efb822a2fc24a1209a836d51bb217476fc08e
  (2026-10-07). Imported 2026-10-10 with `git clone --depth 1`; `.git`, `test/` and `doc/` removed, nothing else
  changed.
* Built by pngfast's CMake when `PNGFAST_WITH_ZLIBNG=ON` (default when this directory exists): native API
  (`zng_*`, `ZLIB_COMPAT=OFF`, so no clash with a system zlib), no gzip file API, runtime CPU detection,
  no native-instruction flags. Its objects are folded into libpngfast.
* Update: replace the directory with a newer checkout (same removals), record the commit here, rebuild, run ctest.
