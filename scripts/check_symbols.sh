#!/bin/sh
# Fail if a qpng library exports any symbol outside the public API (include/qpng.h, QPNG_API).
#   scripts/check_symbols.sh EXPORTS_FILE LIB...      (EXPORTS_FILE: build dir qpng.exports, one name per line)
# Static archives: defined global symbols of every member; shared objects: the dynamic symbol table.
# Sanitizer runtimes inject ___asan_*/__ubsan_*/__sancov_* globals into instrumented libs; ignore those.
set -eu
EXPORTS=$1; shift
bad=0
# ASan/UBSan/coverage runtime symbols (and C++ mangled sanitizer helpers) are not part of libqpng.
is_sanitizer_sym() {
  case $1 in
    __asan_*|___asan_*|__ubsan_*|___ubsan_*|__sancov_*|___sancov_*|asan.*|ubsan.*|sancov.*|__lsan_*|___lsan_*|__tsan_*|___tsan_*) return 0 ;;
    _Z*asan*|_Z*ubsan*|_Z*sancov*|_Z*lsan*) return 0 ;;
  esac
  return 1
}
for lib in "$@"; do
  case $lib in
    *.so*) syms=$(nm -D --defined-only "$lib" | awk 'NF>=3 && $2 != "A" {sub(/@.*/, "", $3); print $3}') ;;
    *)     syms=$(nm -g --defined-only "$lib" 2>/dev/null | awk 'NF==3 {print $3}') ;;
  esac
  n=0
  for s in $syms; do
    if is_sanitizer_sym "$s"; then continue; fi
    n=$((n + 1))
    if ! grep -qx "$s" "$EXPORTS"; then echo "$lib: unexpected exported symbol: $s"; bad=1; fi
  done
  [ "$n" -gt 0 ] || { echo "$lib: no exported symbols found"; bad=1; }
  echo "$lib: $n exported symbols checked"
done
exit $bad
