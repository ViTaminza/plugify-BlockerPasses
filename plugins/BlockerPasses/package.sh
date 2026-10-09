#!/bin/bash
# Assembles build/package/BlockerPasses: the library and manifest from build/, everything else from dist/.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../.." && pwd)
OUT="$ROOT/build/package/BlockerPasses"
rm -rf "$OUT"
mkdir -p "$OUT/bin"
cp "$ROOT/build/plugins/BlockerPasses/libBlockerPasses.so" "$OUT/bin/"
cp "$ROOT/build/plugins/BlockerPasses/BlockerPasses.pplugin" "$OUT/"
if [ -d "$HERE/dist" ]; then cp -r "$HERE/dist/." "$OUT/"; fi
echo "package: $OUT"
