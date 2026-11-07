#!/bin/sh
# Build a .deb from an unpacked release directory.
#   sh packaging/make-deb.sh RELEASE_DIR OUT_DIR
set -eu
SRC="$1"; OUT="$2"
VERSION="$(cat "$SRC/VERSION")"
ROOT="$(mktemp -d)"
mkdir -p "$ROOT/usr/lib/zevic" "$ROOT/usr/bin" "$ROOT/DEBIAN"
cp -R "$SRC/bin" "$SRC/packages" "$SRC/runtime" "$SRC/examples" "$SRC/LICENSE" "$SRC/README.md" "$SRC/CHANGELOG.md" "$SRC/VERSION" "$ROOT/usr/lib/zevic/"
for t in zevic zevpm zevfmt; do ln -s "../lib/zevic/bin/$t" "$ROOT/usr/bin/$t"; done
SIZE="$(du -sk "$ROOT/usr" | cut -f1)"
cat > "$ROOT/DEBIAN/control" <<CTL
Package: zevic
Version: $VERSION
Section: devel
Priority: optional
Architecture: amd64
Depends: clang (>= 1:15)
Installed-Size: $SIZE
Maintainer: rimsky-yamatov <ja.yamatov.dev@gmail.com>
Description: Zevic programming language: compiler, package manager and formatter
 zevic compiles Zevic to LLVM IR and builds it with clang; zevpm is the
 package manager and zevfmt the formatter. The standard library is the
 Zevic package.
CTL
mkdir -p "$OUT"
dpkg-deb --root-owner-group --build "$ROOT" "$OUT/zevic_${VERSION}_amd64.deb"
rm -rf "$ROOT"
