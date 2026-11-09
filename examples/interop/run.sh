#!/bin/sh
# Builds the Zevic library (zevlib/wordlib.zev) and calls it from C++, Rust, Go and Python;
# then calls Rust and C++ code from Zevic. A language whose toolchain is missing is skipped.
# usage: examples/interop/run.sh [path-to-zevic]   (run from the repository root)
set -eu
ZEVIC=${1:-build/zevic}
HERE=examples/interop
OUT=${TMPDIR:-/tmp}/zevic-interop.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
ZEVIC=$(cd "$(dirname "$ZEVIC")" && pwd)/$(basename "$ZEVIC")

# the library: a static archive with its header, and a position independent copy for Python
"$ZEVIC" $HERE/zevlib/wordlib.zev --lib --emit-header "$OUT/wordlib.h" -o "$OUT/libwordlib.a"
have() { command -v "$1" >/dev/null 2>&1; }

echo "== C++ -> Zevic"
if have clang++ || have g++; then
    CXX=$(command -v clang++ || command -v g++)
    "$CXX" -I"$OUT" $HERE/cpp/host.cpp "$OUT/libwordlib.a" -lpthread -lm -o "$OUT/host_cpp"
    "$OUT/host_cpp"
else echo "skipped (no C++ compiler)"; fi

echo "== Rust -> Zevic"
if have rustc; then
    rustc -O $HERE/rust/host.rs -L "$OUT" -l static=wordlib -C link-arg=-lpthread -C link-arg=-lm -o "$OUT/host_rs"
    "$OUT/host_rs"
else echo "skipped (no rustc)"; fi

echo "== Go (cgo) -> Zevic"
if have go; then
    cp $HERE/go/main.go "$OUT/main.go"
    sed -i 's#\.\./wordlib\.h#wordlib.h#; s#\${SRCDIR}/\.\.#${SRCDIR}#' "$OUT/main.go"
    (cd "$OUT" && CC=${CC:-clang} go run main.go)
else echo "skipped (no go)"; fi

echo "== Python (ctypes) -> Zevic"
if have python3 && [ "$(uname -s)" = Linux ]; then
    "$ZEVIC" $HERE/zevlib/wordlib.zev --lib --pic -o "$OUT/libwordlib_pic.a"
    clang -shared -fPIC -Wl,--whole-archive "$OUT/libwordlib_pic.a" -Wl,--no-whole-archive -o "$OUT/libwordlib.so" -lpthread -lm
    python3 $HERE/python/host.py "$OUT/libwordlib.so"
else echo "skipped (needs python3 on Linux)"; fi

echo "== Zevic -> Rust"
if have rustc; then
    rustc --crate-type staticlib -O $HERE/rust/fib.rs -o "$OUT/libfib.a"
    "$ZEVIC" $HERE/rust/main.zev "$OUT/libfib.a" -lpthread -ldl -lm -o "$OUT/zev_rs"
    "$OUT/zev_rs"
else echo "skipped (no rustc)"; fi

echo "== Zevic -> C++"
if have clang++ || have g++; then
    "$ZEVIC" $HERE/cpp/main.zev $HERE/cpp/stats.cpp -o "$OUT/zev_cpp"
    "$OUT/zev_cpp"
else echo "skipped (no C++ compiler)"; fi
