#!/bin/sh
# Cross-build for Linux arm64 from Linux x64 and run the result with qemu-user.
# Needs: gcc-aarch64-linux-gnu libc6-dev-arm64-cross qemu-user lld (Debian/Ubuntu package names).
# Usage: tests/cross/linux-arm64.sh [path-to-zevic]
set -eu
ZEVIC="${1:-build/zevic}"
SYSROOT=/usr/aarch64-linux-gnu
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
for ex in 01_hello 02_fizzbuzz 03_factorial 05_shapes; do
    "$ZEVIC" --target linux-arm64 --sysroot "$SYSROOT" "examples/$ex/src/Main.zev" -o "$TMP/$ex"
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
