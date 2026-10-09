#!/bin/sh
# Regenerate third_party/libdeflate/pngfast-changes.patch against upstream libdeflate v1.26.
#   scripts/gen_libdeflate_patch.sh [UPSTREAM_GIT_DIR]   (default: clones https://github.com/ebiggers/libdeflate)
set -eu
ROOT=$(cd "$(dirname "$0")/.." && pwd)
V=$ROOT/third_party/libdeflate
UP=${1:-}
T=$(mktemp -d); trap 'rm -rf "$T"' EXIT
if [ -z "$UP" ]; then git clone -q https://github.com/ebiggers/libdeflate "$T/up"; UP=$T/up; fi
mkdir -p "$T/a/lib" "$T/b/lib"
for f in libdeflate.h lib/deflate_compress.c lib/deflate_decompress.c lib/decompress_template.h; do
  git -C "$UP" show v1.26:"$f" > "$T/a/$f"
  cp "$V/$f" "$T/b/$f"
done
(cd "$T" && diff -ru a b > "$V/pngfast-changes.patch") || true
echo "wrote $V/pngfast-changes.patch"
