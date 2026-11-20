#!/bin/sh
# Install zevic from an unpacked release archive (Linux, macOS).
#   sh install.sh [--prefix DIR] [--with-zig | --no-zig] [--zig-version V]
#                                    default DIR: $HOME/.local
# --with-zig  also installs Zig (checked against ziglang.org's SHA-256) and the
#             zevic-cc wrapper, so `ZEVIC_CLANG=zevic-cc zevic --target windows-x64 ...`
#             can cross-compile without clang cross toolchains. Asked in a terminal.
# Copies the release to DIR/lib/zevic and links zevic, zevpm, zevfmt into DIR/bin.
# Uninstall: sh install.sh --uninstall [--prefix DIR]
set -eu
PREFIX="${HOME:-/usr/local}/.local"
UNINSTALL=0
ZIG=ask
ZIGVER=0.17.1
while [ $# -gt 0 ]; do
    case "$1" in
        --prefix) PREFIX="$2"; shift 2 ;;
        --with-zig) ZIG=1; shift ;;
        --no-zig) ZIG=0; shift ;;
        --zig-version) ZIGVER="$2"; shift 2 ;;
        --uninstall) UNINSTALL=1; shift ;;
        -h|--help) sed -n '2,9p' "$0"; exit 0 ;;
        *) echo "install.sh: unknown option $1" >&2; exit 2 ;;
    esac
done
LIB="$PREFIX/lib/zevic"
BIN="$PREFIX/bin"
if [ "$UNINSTALL" = 1 ]; then
    for t in zevic zevpm zevfmt zevic-cc; do rm -f "$BIN/$t"; done
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
install_zig() {
    case "$(uname -s)" in Linux) zos=linux ;; Darwin) zos=macos ;; *) echo "zig: unsupported OS" >&2; return 1 ;; esac
    case "$(uname -m)" in x86_64|amd64) zarch=x86_64 ;; arm64|aarch64) zarch=aarch64 ;; *) echo "zig: unsupported CPU" >&2; return 1 ;; esac
    key="$zarch-$zos"
    base="https://ziglang.org/download"
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    echo "zig $ZIGVER ($key): looking up checksum"
    curl -fsSL "$base/index.json" -o "$tmp/index.json" || { echo "zig: cannot fetch $base/index.json" >&2; return 1; }
    # index.json is pretty-printed: "<ver>": { ... "<key>": { "tarball": "...", "shasum": "..." } }
    entry="$(awk -v ver="\"$ZIGVER\"" -v key="\"$key\"" '
        $1 == ver ":" { inver = 1; next }
        inver && /^  "/ { inver = 0 }
        inver && $1 == key ":" { inkey = 1; next }
        inkey && $1 == "\"tarball\":" { gsub(/[",]/, "", $2); url = $2 }
        inkey && $1 == "\"shasum\":" { gsub(/[",]/, "", $2); print url, $2; exit }
    ' "$tmp/index.json")"
    [ -n "$entry" ] || { echo "zig: no build of $ZIGVER for $key" >&2; return 1; }
    url="${entry% *}"; want="${entry#* }"
    echo "zig: downloading $url"
    curl -fsSL "$url" -o "$tmp/zig.tar.xz" || { echo "zig: download failed" >&2; return 1; }
    if command -v sha256sum >/dev/null 2>&1; then got="$(sha256sum "$tmp/zig.tar.xz" | cut -d' ' -f1)"
    else got="$(shasum -a 256 "$tmp/zig.tar.xz" | cut -d' ' -f1)"; fi
    [ "$got" = "$want" ] || { echo "zig: checksum mismatch (got $got, want $want)" >&2; return 1; }
    mkdir -p "$LIB/zig"
    tar -xJf "$tmp/zig.tar.xz" -C "$LIB/zig" --strip-components=1 || { echo "zig: cannot unpack (is xz installed?)" >&2; return 1; }
    cat > "$LIB/bin/zevic-cc" <<W
#!/bin/sh
# zig cc as a C compiler for zevic: translates LLVM triples to zig targets.
# usage: ZEVIC_CLANG=zevic-cc zevic --target windows-x64 prog.zev -o prog.exe
n=\$#
while [ \$n -gt 0 ]; do
    a="\$1"; shift; n=\$((n - 1))
    case "\$a" in
        --target=*)
            t="\${a#--target=}"; arch="\${t%%-*}"
            [ "\$arch" = arm64 ] && arch=aarch64
            case "\$t" in
                *windows*|*mingw*) zt="\$arch-windows-gnu" ;;
                *darwin*|*apple*) zt="\$arch-macos" ;;
                *linux*) zt="\$arch-linux-gnu" ;;
                *) zt="\$t" ;;
            esac
            set -- "\$@" -target "\$zt" ;;
        *) set -- "\$@" "\$a" ;;
    esac
done
exec "$LIB/zig/zig" cc "\$@"
W
    chmod +x "$LIB/bin/zevic-cc"
    ln -sf "$LIB/bin/zevic-cc" "$BIN/zevic-cc"
    echo "zig $ZIGVER installed in $LIB/zig; use ZEVIC_CLANG=zevic-cc with --target"
}
if [ "$ZIG" = ask ]; then
    ZIG=0
    if [ -t 0 ] && [ -t 1 ]; then
        printf 'Install Zig for cross-compiling (--target)? [y/N] '
        read -r ans || ans=n
        case "$ans" in y|Y|yes) ZIG=1 ;; esac
    fi
fi
echo "zevic $(cat "$LIB/VERSION") installed in $LIB"
case ":$PATH:" in
    *":$BIN:"*) ;;
    *) echo "add $BIN to PATH:  export PATH=\"$BIN:\$PATH\"" ;;
esac
command -v clang >/dev/null 2>&1 || echo "note: clang 15 or newer is required to compile Zevic programs"
if [ "$ZIG" = 1 ]; then
    ( install_zig ) || echo "note: zig was not installed; zevic itself is ready"
fi
