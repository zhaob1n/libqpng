#!/bin/sh
# Link test: libpngfast.a together with stock static zlib-ng (native zng_* API), zlib-compat zlib-ng and libdeflate
# (built once from the vendored sources without pngfast's renaming, i.e. upstream symbol names) must link without
# duplicate symbols, and both sides must work.
#   tests/link_test.sh LIBPNGFAST.a SOURCE_ROOT WORKDIR CC
set -eu
LIB=$1; ROOT=$2; W=$3; CC=$4
mkdir -p "$W"
for flavour in zng:OFF zcompat:ON; do
  d=${flavour%%:*}; compat=${flavour##*:}
  if [ ! -f "$W/$d/.built" ]; then
    cmake -S "$ROOT/third_party/zlib-ng" -B "$W/$d" -DBUILD_SHARED_LIBS=OFF -DBUILD_TESTING=OFF -DZLIB_COMPAT=$compat \
          -DWITH_GZFILEOP=OFF -DCMAKE_BUILD_TYPE=Release -DCMAKE_C_COMPILER="$CC" >/dev/null 2>&1
    nice -n 10 cmake --build "$W/$d" -j4 >/dev/null 2>&1
    touch "$W/$d/.built"
  fi
done
if [ ! -f "$W/libdeflate.a" ]; then
  mkdir -p "$W/ld"
  for f in "$ROOT"/third_party/libdeflate/lib/*.c "$ROOT"/third_party/libdeflate/lib/x86/*.c; do
    "$CC" -O2 -c -I"$ROOT/third_party/libdeflate" -DLIBDEFLATE_STATIC "$f" -o "$W/ld/$(basename "$f" .c).o"
  done
  ar rcs "$W/libdeflate.a" "$W"/ld/*.o
fi
"$CC" -O2 -I"$ROOT/include" -I"$W/zng" -I"$ROOT/third_party/libdeflate" -DLIBDEFLATE_STATIC \
      "$ROOT/tests/link_test.c" "$LIB" "$W/zng/libz-ng.a" "$W/libdeflate.a" -o "$W/link_test" -lm
"$W/link_test"
# zlib-compat flavour (deflate_medium, crc32_* ... with plain zlib names)
"$CC" -O2 -I"$ROOT/include" -I"$W/zcompat" -DPF_LINK_ZLIB_COMPAT "$ROOT/tests/link_test.c" "$LIB" \
      "$W/zcompat/libz.a" -o "$W/link_test_compat" -lm
"$W/link_test_compat"
