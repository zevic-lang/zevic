# baremetal: a kernel in Zevic

A minimal RISC-V kernel for QEMU's `virt` machine, built with `--freestanding` (proposal 0013): no runtime, no GC, no libc.

- `native/boot.S`: the entry point; sets the stack and calls `kmain`
- `src/Main.zev`: the UART driver (`Unsafe.At` and `WriteVolatile`), `zevic_panic`, and `kmain`
- `linker.ld`: the memory map (RAM starts at `0x80000000`)
- `zevic.toml`: `kind = "freestanding"`

```
zevpm build                       # build/Kernel.elf
qemu-system-riscv64 -machine virt -bios none -kernel build/Kernel.elf -nographic
```

It prints `Zevic 2026`. `sh run.sh` (from the repository root) builds it by hand and checks that no runtime symbol was linked; with QEMU installed it also boots it.
