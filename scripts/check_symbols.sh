#!/bin/sh
# Fail if a pngfast library exports any symbol outside the public API (include/pngfast.h, PF_API).
#   scripts/check_symbols.sh EXPORTS_FILE LIB...      (EXPORTS_FILE: build dir pngfast.exports, one name per line)
# Static archives: defined global symbols of every member; shared objects: the dynamic symbol table.
set -eu
EXPORTS=$1; shift
bad=0
for lib in "$@"; do
  case $lib in
    *.so*) syms=$(nm -D --defined-only "$lib" | awk 'NF>=3 && $2 != "A" {sub(/@.*/, "", $3); print $3}') ;;
    *)     syms=$(nm -g --defined-only "$lib" 2>/dev/null | awk 'NF==3 {print $3}') ;;
  esac
  n=0
  for s in $syms; do
    n=$((n + 1))
    if ! grep -qx "$s" "$EXPORTS"; then echo "$lib: unexpected exported symbol: $s"; bad=1; fi
  done
  [ "$n" -gt 0 ] || { echo "$lib: no exported symbols found"; bad=1; }
  echo "$lib: $n exported symbols checked"
done
exit $bad
