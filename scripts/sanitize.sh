#!/bin/sh
# CI-style sanitizer run: ASan+UBSan build (with zlib-ng when vendored), full ctest (incl. fuzz smoke), same for the portable scalar build, longer mutation fuzzing,
# and (when clang is available) short libFuzzer sessions seeded with PngSuite.
# Builds live under /tmp so the source tree stays clean (and concurrent module worktrees do not collide).
#   scripts/sanitize.sh [ITERS] [LIBFUZZER_SECONDS]
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
ITERS=${1:-2000}
SECS=${2:-60}
CC=${CC:-cc}
# Out-of-tree build roots under /tmp (unique per source tree so parallel worktrees do not clash).
TAG=$(printf '%s' "$ROOT" | tr '/.' '__')
B=/tmp/qpng-sanitize-${TAG}-asan
P=/tmp/qpng-sanitize-${TAG}-portable
L=/tmp/qpng-sanitize-${TAG}-libfuzzer
S=/tmp/qpng-sanitize-${TAG}-shared
cmake -S "$ROOT" -B "$B" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER="$CC" \
      -DQPNG_SANITIZE=ON -DQPNG_BUILD_TESTS=ON >/dev/null
cmake --build "$B" -j"${JOBS:-4}"
export ASAN_OPTIONS=detect_leaks=1:abort_on_error=1 UBSAN_OPTIONS=print_stacktrace=1:halt_on_error=1
(cd "$B" && ctest --output-on-failure)
echo "== portable scalar build (-DQPNG_NO_SIMD) without zlib-ng, sanitized"
cmake -S "$ROOT" -B "$P" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER="$CC" -DCMAKE_C_FLAGS=-DQPNG_NO_SIMD \
      -DQPNG_WITH_ZLIBNG=OFF -DQPNG_SANITIZE=ON -DQPNG_BUILD_TESTS=ON >/dev/null
cmake --build "$P" -j"${JOBS:-4}"
(cd "$P" && ctest --output-on-failure)
SEEDS=$(ls "$ROOT"/tests/data/pngsuite/*.png "$ROOT"/tests/data/formats_small/*.png)
echo "== mutation fuzzing: $ITERS mutations per seed"
QPNG_FUZZ_ITERS=$ITERS "$B/fuzz_decode" $SEEDS
QPNG_FUZZ_ITERS=$((ITERS / 10)) "$B/fuzz_roundtrip" $SEEDS
if command -v clang >/dev/null 2>&1; then
  cmake -S "$ROOT" -B "$L" -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_C_COMPILER=clang \
        -DQPNG_SANITIZE=ON -DQPNG_LIBFUZZER=ON -DQPNG_BUILD_TESTS=OFF >/dev/null
  cmake --build "$L" -j"${JOBS:-4}"
  mkdir -p "$L/corpus_decode" "$L/corpus_rt"
  "$L/libfuzzer_decode" -max_total_time="$SECS" "$L/corpus_decode" "$ROOT/tests/data/pngsuite"
  "$L/libfuzzer_roundtrip" -max_total_time="$SECS" "$L/corpus_rt"
else
  echo "clang not found: libFuzzer sessions skipped (standalone mutation fuzzing done above)"
fi
echo "== shared library: exported symbols"
cmake -S "$ROOT" -B "$S" -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$CC" -DQPNG_BUILD_SHARED=ON >/dev/null
cmake --build "$S" -j"${JOBS:-4}"
(cd "$S" && ctest --output-on-failure -R "symbols|api|roundtrip$")
echo "sanitize: OK"
