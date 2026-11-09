#!/bin/sh
# Build the compiler (the "seed") from bootstrap/zevic.ll with clang and the runtime only.
#   tools/bootstrap-build.sh OUTPUT        (OUTPUT ends in .exe on Windows)
# Needs no earlier Zevic compiler: this is how a new machine gets its first one.
set -eu
OUT="$1"
CC="${CC:-clang}"
HOME_DIR="$(cd "$(dirname "$0")/.." && pwd)"
RT="$HOME_DIR/runtime"
mkdir -p "$(dirname "$OUT")"
case "$(uname -s 2>/dev/null)" in
    MINGW*|MSYS*|CYGWIN*) FLAGS="-DGC_THREADS -DGC_BUILTIN_ATOMIC -DNO_MSGBOX_ON_ERROR"; LIBS="-lws2_32" ;;
    *) FLAGS="-DGC_THREADS -DGC_BUILTIN_ATOMIC -pthread"; LIBS="-lm -pthread" ;;
esac
# shellcheck disable=SC2086
"$CC" -O1 -w -Wno-override-module "$HOME_DIR/bootstrap/zevic.ll" \
    -x c "$RT/zevic_rt.c" -x c "$RT/third_party/bgc/extra/gc.c" -x none \
    $FLAGS -I"$RT/third_party/bgc/include" $LIBS -o "$OUT"
