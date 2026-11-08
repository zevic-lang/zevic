#!/bin/sh
# Build the compiler from bootstrap/zevic.ll with clang and the runtime only.
#   tools/bootstrap-build.sh OUTPUT
set -eu
OUT="$1"
CC="${CC:-clang}"
HOME_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RT="$HOME_DIR/runtime"
mkdir -p "$(dirname "$OUT")"
"$CC" -O1 -w -Wno-override-module "$HOME_DIR/bootstrap/zevic.ll" \
    -x c "$RT/zevic_rt.c" -x c "$RT/third_party/bgc/extra/gc.c" -x none \
    -DGC_THREADS -DGC_BUILTIN_ATOMIC -pthread -I"$RT/third_party/bgc/include" \
    -lm -o "$OUT"
