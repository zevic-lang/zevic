# Proposal 0013: `--freestanding` (writing a kernel)

- Status: accepted (2026-11-26; implemented)
- Date: 2026-11-26

## Motivation

Zevic compiles to LLVM IR and links a small C runtime (`zevic_rt`) and a garbage collector into every program. That is right for applications, and wrong for an operating system kernel, firmware or a boot loader: there is no libc, no heap at the start, no threads, and the author must own the entry point and the memory map. Proposal 0011 gave manual memory in `unsafe`; what was missing was a way to say "do not link the runtime".

## Proposal

```
zevic --freestanding --target riscv64-unknown-elf -T linker.ld --entry _start kernel.zev boot.S -o kernel.elf
```

- `--freestanding`: no `zevic_rt`, no GC, no libc, no startup files (`-ffreestanding -nostdlib -static`, `lld`). Nothing calls `Main`.
- `-T`/`--linker-script`, `--entry`, `--cflag`, `--ldflag`; `.S`, `.s` and `.c` files as inputs. `-o x.o` stops after the object.
- The generated code is checked: if a function needs the runtime (`zevic_alloc`, strings, arrays, classes, closures, …), the compiler stops with the name of the function and the reason. `struct`, numbers, pointers, `unsafe` and `extern "C"` remain.
- Panics (division by zero, overflow, bounds, `unwrap`) call `zevic_panic(msg, len, file, line)`, which the program must define with `export "C"`. If the code can panic and the function is missing, it is a compile error with the exact signature.
- The code is compiled with `noredzone` (interrupts use the stack) and without stack protector. RISC-V 64 uses `-mcmodel=medany`.
- Memory-mapped I/O: `Unsafe.At<T>(address)`, `p.Address()`, `p.ReadVolatile(i)`, `p.WriteVolatile(i, v)`.
- In `zevic.toml`: a `[freestanding]` section (its presence is what makes the package freestanding, because `kind` says bin or lib, not what the code runs on) with `target`, `linker_script`, `entry`, `cflags`, `ldflags`. `zevpm build` makes `build/<Name>.elf`; `native/*.S` is assembled. `zevpm run` and `zevpm test` refuse.

## Not in this proposal

- A usable allocator or the standard library without an OS (a future `Zevic.Core` split could allow `String` and `Array` on a user-supplied allocator).
- Interrupt handlers (`export "C"` plus assembly stubs work today), inline assembly, a testing story for kernels (an emulator in CI).
- Targets other than the ones clang supports.
