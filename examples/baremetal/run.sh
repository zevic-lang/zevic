#!/bin/sh
# Builds the kernel (a freestanding RISC-V image, no runtime, no GC) and checks the result.
# With qemu-system-riscv64 installed, it also boots it and checks the output.
# usage: examples/baremetal/run.sh [path-to-zevic]   (run from the repository root)
set -eu
ZEVIC=${1:-build/zevic}
HERE=examples/baremetal
OUT=${TMPDIR:-/tmp}/zevic-baremetal.$$
mkdir -p "$OUT"
trap 'rm -rf "$OUT"' EXIT
"$ZEVIC" --freestanding --target riscv64-unknown-elf -T $HERE/linker.ld --entry _start \
    $HERE/src/Main.zev $HERE/native/boot.S -o "$OUT/kernel.elf"
if command -v readelf >/dev/null 2>&1; then
    readelf -h "$OUT/kernel.elf" | grep -q "RISC-V" || { echo "not a RISC-V image"; exit 1; }
fi
if command -v nm >/dev/null 2>&1; then
    if nm "$OUT/kernel.elf" | grep -E " [TtDdBbRr] (zevic_(rt|gc)|GC_|malloc|printf)" ; then
        echo "the runtime was linked"; exit 1
    fi
fi
echo "built: no runtime, no GC"
if command -v qemu-system-riscv64 >/dev/null 2>&1; then
    got=$(timeout 3 qemu-system-riscv64 -machine virt -bios none -kernel "$OUT/kernel.elf" -nographic -serial stdio 2>/dev/null | head -1 || true)
    [ "$got" = "Zevic 2026" ] || { echo "unexpected output: $got"; exit 1; }
    echo "booted: $got"
else
    echo "qemu-system-riscv64 not found: boot check skipped"
fi
