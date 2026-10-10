#!/bin/sh
# CI-style sanitizer run: ASan+UBSan build (with zlib-ng when vendored), full ctest (incl. fuzz smoke), same for the portable scalar build, longer mutation fuzzing,
# and (when clang is available) short libFuzzer sessions seeded with PngSuite.
#   scripts/sanitize.sh [ITERS] [LIBFUZZER_SECONDS]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ITERS=${1:-2000}
SECS=${2:-60}
CC=${CC:-cc}
B=$ROOT/build-asan
cmake -S "$ROOT" -B "$B" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER="$CC" \
      -DPNGFAST_SANITIZE=ON -DPNGFAST_BUILD_TESTS=ON >/dev/null
cmake --build "$B" -j"${JOBS:-4}"
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
(cd "$B" && ctest --output-on-failure)
echo "== portable scalar build (-DPNGFAST_NO_SIMD) without zlib-ng, sanitized"
P=$ROOT/build-asan-portable
cmake -S "$ROOT" -B "$P" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER="$CC" -DCMAKE_C_FLAGS=-DPNGFAST_NO_SIMD \
      -DPNGFAST_WITH_ZLIBNG=OFF -DPNGFAST_SANITIZE=ON -DPNGFAST_BUILD_TESTS=ON >/dev/null
cmake --build "$P" -j"${JOBS:-4}"
(cd "$P" && ctest --output-on-failure)
SEEDS=$(ls "$ROOT"/tests/data/pngsuite/*.png "$ROOT"/tests/data/formats_small/*.png)
echo "== mutation fuzzing: $ITERS mutations per seed"
PF_FUZZ_ITERS=$ITERS "$B/fuzz_decode" $SEEDS
PF_FUZZ_ITERS=$((ITERS / 10)) "$B/fuzz_roundtrip" $SEEDS
if command -v clang >/dev/null 2>&1; then
  L=$ROOT/build-libfuzzer
  cmake -S "$ROOT" -B "$L" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang \
        -DPNGFAST_SANITIZE=ON -DPNGFAST_LIBFUZZER=ON -DPNGFAST_BUILD_TESTS=OFF >/dev/null
  cmake --build "$L" -j"${JOBS:-4}"
  mkdir -p "$L/corpus_decode" "$L/corpus_rt"
  "$L/libfuzzer_decode" -max_total_time="$SECS" "$L/corpus_decode" "$ROOT/tests/data/pngsuite"
  "$L/libfuzzer_roundtrip" -max_total_time="$SECS" "$L/corpus_rt"
else
  echo "clang not found: libFuzzer sessions skipped (standalone mutation fuzzing done above)"
fi
echo "sanitize: OK"
