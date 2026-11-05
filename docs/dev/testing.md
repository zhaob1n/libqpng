# Testing

* `ctest`: api, conformance, roundtrip, metadata, stream, apng, symbols,
  link_test, fuzz smoke. Run with and without zlib-ng (`-DQPNG_WITH_ZLIBNG=OFF`),
  on GCC 16 and Clang 23.
* `scripts/sanitize.sh`: ASan+UBSan + ctest (SIMD and `-DQPNG_NO_SIMD`), mutation
  fuzzing; libFuzzer when clang is available.
* `scripts/check_symbols.sh`: no symbol outside the public API.
* Encoder output for a given preset/filter/level must stay byte-identical to
  master across modern-C refactors unless a commit deliberately changes it.
