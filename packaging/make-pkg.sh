#!/bin/sh
# Build a macOS .pkg from an unpacked release directory.
#   sh packaging/make-pkg.sh RELEASE_DIR OUT_DIR
# Installs into /usr/local/zevic and links the tools into /usr/local/bin.
set -eu
SRC="$1"; OUT="$2"
VERSION="$(cat "$SRC/VERSION")"
ROOT="$(mktemp -d)"
SCRIPTS="$(mktemp -d)"
mkdir -p "$ROOT/usr/local/zevic"
cp -R "$SRC/bin" "$SRC/packages" "$SRC/runtime" "$SRC/examples" "$SRC/LICENSE" "$SRC/README.md" "$SRC/CHANGELOG.md" "$SRC/VERSION" "$ROOT/usr/local/zevic/"
cat > "$SCRIPTS/postinstall" <<'PI'
#!/bin/sh
mkdir -p /usr/local/bin
for t in zevic zevpm zevfmt; do ln -sf /usr/local/zevic/bin/$t /usr/local/bin/$t; done
exit 0
PI
chmod +x "$SCRIPTS/postinstall"
mkdir -p "$OUT"
pkgbuild --root "$ROOT" --scripts "$SCRIPTS" --identifier dev.zevic.zevic --version "$VERSION" --install-location / "$OUT/zevic-${VERSION}-macos-arm64.pkg"
rm -rf "$ROOT" "$SCRIPTS"
