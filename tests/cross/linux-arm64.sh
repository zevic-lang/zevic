#!/bin/sh
# Cross-build for Linux arm64 from Linux x64 and run the result with qemu-user.
# Needs: gcc-aarch64-linux-gnu libc6-dev-arm64-cross qemu-user lld (Debian/Ubuntu package names).
# Usage: tests/cross/linux-arm64.sh [path-to-zevic [path-to-zevpm]]
set -eu
ZEVIC="${1:-build/zevic}"
ZEVPM="${2:-build/zevpm}"
SYSROOT=/usr/aarch64-linux-gnu   # the run-time libraries for qemu; for the build clang finds the Debian cross layout through --gcc-install-dir
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
# clang needs to be told where the cross GCC keeps crtbegin.o and libgcc: a small wrapper is the ZEVIC_CLANG for this target
GCCDIR="$(ls -d /usr/lib/gcc-cross/aarch64-linux-gnu/* | sort -V | tail -n 1)"
printf '#!/bin/sh\nexec clang --gcc-install-dir=%s "$@"\n' "$GCCDIR" > "$TMP/clang-arm64"
chmod +x "$TMP/clang-arm64"
export ZEVIC_CLANG="$TMP/clang-arm64"
for ex in 01_hello 02_fizzbuzz 03_factorial 05_shapes; do
    "$ZEVIC" --target linux-arm64 "examples/$ex/src/Main.zev" -o "$TMP/$ex"
    case "$(file -b "$TMP/$ex")" in
        *aarch64*|*ARM\ aarch64*) ;;
        *) echo "FAIL $ex: not an arm64 executable: $(file -b "$TMP/$ex")"; exit 1 ;;
    esac
    qemu-aarch64 -L "$SYSROOT" "$TMP/$ex" > "$TMP/$ex.out"
    if ! cmp -s "$TMP/$ex.out" "tests/examples_expected/$ex.txt"; then
        echo "FAIL $ex: output differs"
        diff "$TMP/$ex.out" "tests/examples_expected/$ex.txt" | head
        exit 1
    fi
    echo "ok   $ex (arm64, qemu)"
done

# the same through zevpm: build/linux-arm64/<Name>
ZEVIC="$(cd "$(dirname "$ZEVIC")" && pwd)/$(basename "$ZEVIC")"
ZEVPM="$(cd "$(dirname "$ZEVPM")" && pwd)/$(basename "$ZEVPM")"
export ZEVIC
(cd "$TMP" && "$ZEVPM" new pkg >/dev/null && cd pkg && "$ZEVPM" build --target linux-arm64 >/dev/null)
case "$(file -b "$TMP/pkg/build/linux-arm64/Pkg")" in
    *aarch64*|*ARM\ aarch64*) ;;
    *) echo "FAIL zevpm: not an arm64 executable"; exit 1 ;;
esac
test "$(qemu-aarch64 -L "$SYSROOT" "$TMP/pkg/build/linux-arm64/Pkg")" = "Hello from Pkg!"
echo "ok   zevpm build --target linux-arm64"
