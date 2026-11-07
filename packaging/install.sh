#!/bin/sh
# Install zevic from an unpacked release archive (Linux, macOS).
#   sh install.sh [--prefix DIR]     default DIR: $HOME/.local
# Copies the release to DIR/lib/zevic and links zevic, zevpm, zevfmt into DIR/bin.
# Uninstall: sh install.sh --uninstall [--prefix DIR]
set -eu
PREFIX="${HOME:-/usr/local}/.local"
UNINSTALL=0
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift 2 ;;
        --uninstall) UNINSTALL=1; shift ;;
        -h|--help) sed -n '2,6p' "$0"; exit 0 ;;
        *) echo "install.sh: unknown option $1" >&2; exit 2 ;;
    esac
done
LIB="$PREFIX/lib/zevic"
BIN="$PREFIX/bin"
if [ "$UNINSTALL" = 1 ]; then
    for t in zevic zevpm zevfmt; do rm -f "$BIN/$t"; done
    rm -rf "$LIB"
    echo "zevic removed from $PREFIX"
    exit 0
fi
SRC="$(cd "$(dirname "$0")" && pwd)"
if [ ! -x "$SRC/bin/zevic" ] || [ ! -f "$SRC/runtime/zevic_rt.c" ]; then
    echo "install.sh: run it from the unpacked release (bin/ and runtime/ must be next to it)" >&2
    exit 1
fi
rm -rf "$LIB"
mkdir -p "$LIB" "$BIN"
cp -R "$SRC/bin" "$SRC/packages" "$SRC/runtime" "$LIB/"
for f in examples LICENSE README.md CHANGELOG.md VERSION; do
    [ -e "$SRC/$f" ] && cp -R "$SRC/$f" "$LIB/"
done
for t in zevic zevpm zevfmt; do ln -sf "$LIB/bin/$t" "$BIN/$t"; done
echo "zevic $(cat "$LIB/VERSION") installed in $LIB"
case ":$PATH:" in
    *":$BIN:"*) ;;
    *) echo "add $BIN to PATH:  export PATH=\"$BIN:\$PATH\"" ;;
esac
command -v clang >/dev/null 2>&1 || echo "note: clang 15 or newer is required to compile Zevic programs"
