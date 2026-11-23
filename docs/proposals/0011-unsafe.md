# Proposal 0011: `unsafe` and manual memory

- Status: accepted (2026-11-23; implemented, see "Manual memory" in the specification)
- Date: 2026-11-23

## Motivation

Zevic's memory is managed by the GC, and that is right for almost every program. A few things cannot be written with it, or only badly: a table of numbers that must not be scanned or moved, a buffer shared with C, a data structure whose lifetime the program wants to decide (an arena, a pool), a first step toward writing a kernel. Today there is nothing between "everything is GC" and `extern "C"` (calling `malloc` and `free` and keeping `*CVoid`, which has no way to read or write). `unsafe` was reserved as a word, and the specification moved it and a `NoGC` mode to future considerations.

This proposal adds the smallest useful part: **manual memory in `unsafe` blocks, next to the GC**. A `NoGC` mode stays a future item.

## Proposal

```
import Zevic.Unsafe

unsafe {
    let p: *I64 = Unsafe.Alloc<I64>(4)
    p.Write(0, 10)
    p.Offset(1).Write(0, 20)
    Unsafe.Free(p)
}
```

- `unsafe { ... }` is an ordinary block (it has a value) that allows the operations below. A lambda written inside is not unsafe.
- `Unsafe.Alloc<T>(n)` (zero-filled), `Unsafe.Free(p)`, `Unsafe.Cast<U>(p)`; and on a pointer `*T`: `Read(i)`, `Write(i, v)`, `Offset(n)`, `IsNull()`. All but `IsNull` are errors outside `unsafe`.
- The pointer type is the existing `*T` of the C interop. No new type.
- `Alloc` calls `calloc` and `Free` calls `free`: the memory is not GC memory.
- **`T` must not hold a reference the GC manages.** The GC does not scan manual memory, so a `String`, class, `Array`, function or interface kept there could be collected while the program still uses it. The compiler rejects such a `T` (numbers, `Bool`, `Char`, pointers, and structs, tuples and enums made only of those are allowed). This is checked in `Alloc`, `Read` and `Write`.
- No range check, no use-after-free check, no double-free check. That is what `unsafe` says.
- `extern "C"` calls stay as they are: they do not need `unsafe` (changing that would break every existing program).

## Alternatives

- **A `NoGC` build mode.** Much bigger (the whole standard library allocates with the GC) and it is not "manual memory" for a program that wants both. Left for later.
- **Arenas only (`Arena.New()`, `arena.Free()`).** Safer, and can be built on this proposal as a library. It does not cover shared buffers or C.
- **Let manual memory hold GC references, with the GC scanning it** (or with pinning by the user). Convenient, but it needs registration of regions, costs time in every collection, and a mistake corrupts the GC's own state, which is very hard to trace. The type check above keeps the GC safe at no cost.
- **`unsafe fn`.** Not needed for this step; a function that needs it can use a block inside.
- **Pointer-to-integer conversions.** Left out until a use appears.

## Impact

- Lexer: `unsafe` is a keyword (it was reserved, so no valid program changes). Parser: `unsafe` followed by a block; the block carries the name `unsafe`. `zevfmt` needed no change.
- Code generation: a counter of enclosing `unsafe` blocks in the function context; `Unsafe.*` are `extern "intrinsic"` and `*T` gets methods, all turned into `calloc`, `free`, `getelementptr`, `load` and `store`.
- Standard library: the new module `Zevic.Unsafe` (declarations only).
- Specification: the section "Manual memory", the keyword list, the non-goals and the future considerations.

## Tests

`tests/golden/130-unsafe` (allocation, structs, `Offset`, `Cast`, floats, `Bool`, zero fill, a function taking `*I64`) and `131-err-unsafe` (use outside `unsafe`, `String` and a class as `T`, a wrong value type in `Write`).
