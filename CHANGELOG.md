# Changelog

## 0.2.0

Stage 5: closures and memory.

- lambdas and closures: `(x: I32) => x + n`, `fn(I32) -> I32` types, named functions as values; captured variables are shared (a counter made by `MakeCounter()` keeps its own state); the type of a lambda parameter must be written
- `defer`, and `params` (a parameter that collects the trailing arguments into an array)
- `Iterable` gets `Each`, `EachIndexed`, `Map`, `Filter`, `Reduce`, `Find`, `Any`, `All`, `Count`
- garbage collection: the runtime allocates through the Boehm collector (vendored in `runtime/third_party/bgc`, bdwgc v8.2.12)
- the driver compiles the runtime and the collector once into a cache (`~/.cache/zevic`, `%LOCALAPPDATA%\zevic\cache` on Windows, or `ZEVIC_CACHE`); the second build of a program takes a fraction of a second
- C callbacks: a named function or a lambda that captures nothing can be passed to C as a `fn` type (`qsort`)
- `Handle<T>`: lets C hold a number for a Zevic object (`Handle.New`, `h.Id()`, `Handle.Get<T>(id)`, `h.Release()`); registered objects are kept alive
- a number literal that must infer a type parameter waits for the other arguments (`a.Reduce(0, f)`)

## 0.1.0

Stage 4: classes, interfaces and C.

- C interop: `extern "C" { fn ... }`, C types (`CInt`, `CLong`, `CSizeT`, `CDouble`, `CString`, `CVoid`, `*T`), conversions (`x.ToCInt()`, `c.ToI32()`, `ToCIntWrap()`, `"..".ToC()`, `String.FromC`, `String.FromCLossy`), `NewString`, `Free`, `&x`, `Null`, `Array.Ptr()`, variadic C functions, `extern struct`
- `export "C"` functions, `--emit-header` writes a C header for them
- link C and C++ files (`.c`, `.cpp`, `.o`, `.a`), `-I` `-D` `-L` `-l`, `--cc`, `--cxx`
- `interface`: required methods, default methods, extending interfaces, interface values (a class is referenced, a `struct` or `enum` is copied to the heap) and dynamic calls
- generic constraints (`<T: Comparable + Hashable>`)
- standard `Equatable`, `Hashable`, `Comparable`, `Iterator`, `Iterable`; the built-in types satisfy the first three
- `for (x in v)` over anything with `Iter()`; `Array` is `Iterable`
- `Map<K, V>` (insertion ordered) and `Set<T>`, written in Zevic
- an unsuffixed number literal becomes the C type that is expected (`let n: CInt = 5`)
- a `loop` without `break` has type `Never`

## 0.0.3

Stage 3: data.

- `struct`, `class` and `enum` (positional and named-field variants, `match` with guards, ranges and nested patterns, exhaustiveness check)
- `Option` and `Result` written in Zevic (`Zevic.Core`), the `?` operator, `Context`, `Error` and `ErrorKind`
- `Array<T>` written in Zevic on top of `Intrinsics.Buf<T>`; `a[i]`, `a[i] = v`, `[1, 2, 3]` and `for (x in a)` / `for (i, x in a)`
- generics with monomorphisation (no constraints yet)
- tuples and tuple patterns
- `==` and `!=` for structs, tuples, enums, arrays and classes (`Equals` method or reference)
- conversions that can fail return `Result` (`I64.ToI32()`, `String.ToI32()`, `String.ToF64()`, ...), `WrapAdd` / `WrapSub` / `WrapMul`, `Clamp`
- F64 is printed the short way (`1000.0`, `0.1`)
- spec: how a variant with named fields is built

## 0.0.2

Stage 2: numbers, control flow and functions (FizzBuzz, factorial, primes); overflow checks, `Panic`, `Printf`.

## 0.0.1

Stage 1: Hello World. zevic0, the first compiler (written in C), emits textual LLVM IR and builds it with clang.
