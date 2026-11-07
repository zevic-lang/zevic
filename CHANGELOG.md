# Changelog

## Unreleased

- the release archive works with only `bin` on PATH, from any directory: the executables are tested in that setting (a directory name with a space, started by name and by full path), on Linux, macOS and Windows
- Windows: names are UTF-8 throughout. `zevic` and the runtime read the command line, the environment, the program path and the files through the wide-character functions, so non-ASCII user names and folders work (before, the ANSI code page and UTF-8 were mixed)
- `zevic` says where it looked when it cannot find the standard library and the runtime
- the tests are written in C (`tests/zevtest.c`) instead of Python; `make test` and CI no longer need Python (only `tools/release_notes.py` and `tools/gen_convert.py` do)
- the self-hosting compiler (`zevic1`) moved to the `selfhost` branch

- the standard library is now a real package, `Zevic` (`packages/Zevic/` with a `zevic.toml` and `src/`), instead of a flat `stdlib/` directory. The compiler finds it through `ZEVIC_HOME` / `share/zevic/packages/Zevic`; `zevic --print-stdlib` prints its directory; zevpm checks a package's `zevic = ">=..."` against the version of the `Zevic` package. Release archives carry `packages/Zevic/`
- release notes: `tools/release_notes.py` builds them from `CHANGELOG.md` and the commits since the previous tag; CI uses them as the body of every GitHub release (and no longer uses generated notes)

- union types (proposal 0007): `A | B | C`, Kotlin style. Members convert to the union and a union converts to a larger union; `x is T` / `x !is T`; smart casts for non-`mut` locals (`if`/`else`, `&&`, `||`, early `return`); `match` type patterns (`n: I32 =>`, `_: Error =>`) with exhaustiveness checking; `if`/`match` branches convert to the expected union. No `==` or `${}` on unions. zevfmt keeps `A | B` in type arguments together

## 0.3.1

Small language and library additions, and the first Zevic tools. (zevfmt, loop labels, async and HTTP also arrive in this release.)

- `Zevic.Random` (proposal 0005): SplitMix64 `Rng` (`New(seed)`, `Seeded()`, `Int(low, high)` without bias, `Float`, `Bool`, `NextU64`), `Shuffle` and `Pick`; the same seed gives the same numbers everywhere; not for secrets
- `Zevic.Json` (proposal 0006): `Value` (`Null` `Bool` `Number` `Str` `List` `Object`) with `AsBool` `AsF64` `AsI64` `AsString` `AsArray` `AsObject` `Get` `At`; `Parse` (RFC 8259, errors with line and column, 512 levels of nesting), `Stringify` and `Pretty`; objects keep the order of their keys

- `xs[i].F = v` and `xs[i].F += v` work on a `mut` array whose elements are classes or structs (proposal 0004); a struct element is changed and stored back, so its array and index must be simple expressions
- the specification's appendix B is the working wordcount example; a test keeps them equal
- fixes found by the first CI runs: `Http.Server.Close` now stops a waiting `accept` on macOS and Windows too (the listener is polled in short slices); the runtime includes `winsock2.h` before `windows.h`; the Boehm GC no longer asks for executable pages (macOS/arm64 aborted); the collector is built without its Windows message box (it needed user32); on Windows `Fs.Remove` removes directories, `Fs.Rename` replaces an existing file and `Process.Run` of a missing program gives exit code 127, as on POSIX; zevpm builds Windows paths with `/`, keeps its git cache directory names short and starts programs without cmd.exe (command lines are quoted correctly, and read-only files such as git objects can be removed); the test runners time out instead of hanging

Stage 7, first part: zevpm and zevfmt are written in Zevic.

- `zevfmt` (`tools/zevfmt/`): `zevfmt file` prints the formatted source, `zevfmt -w path...` rewrites files, `zevfmt -check path...` lists files that are not formatted (exit code 1). It keeps comments and line breaks, fixes indentation (4 spaces), spacing, blank lines, trailing commas in multi-line lists and the order of imports; it refuses files that do not parse. Formatting is idempotent and never changes the AST (tested on the whole repository); `zevpm fmt` uses it. `make fmt` / `make fmt-check`
- `for (k, v in map)` now gives the (key, value) pair, as the specification says (it used to give an index and a pair)
- loop labels (proposal 0001): `outer@ for (...) { ... break@outer ... continue@outer ... }` for `while`, `loop` and `for`; `defer` runs on the way out; labels do not cross lambdas
- async (proposal 0002): `async { ... }` and `await e` (reserved words now), `Zevic.Async` with `Task`, `Spawn`, `Channel` and `Mutex`; tasks are threads, the collector and the runtime are built with thread support; an `async` block cannot use a `mut` local of the surrounding function (unless it is a class value)
- HTTP (proposal 0003): `Zevic.Http` with `Get`, `Post`, `Send` (plain `http://`, `Content-Length`, chunked and read-to-close bodies, 16 MiB limit, 30 s timeout; no TLS, no redirects) and `Listen` / `Server.Serve` / `Close` (a task per connection). The runtime gets small `zevic_net_*` socket functions (POSIX; Winsock written but not tested), and system calls that the collector interrupts are retried
- `Text.Repeat` takes linear time (it was quadratic)
- fix: `Option<Unit>` (an enum whose payloads are all empty) no longer produces invalid IR
- the Zevic sources of the repository are formatted with zevfmt; CI checks it

- `tools/zevpm/` is a Zevic package (the first real program of the language). It passes the same integration tests as the C version (`zevpm/`, now frozen); the release ships the Zevic one
- `Process.Inherit` (run a program with the shared standard input and output), `Fs.Rename`, `Env.ExePath`, `Env.CurrentDir`, `Array.Contains` and `Array.IndexOf`

## 0.3.0

Stage 6: tools and the second tier of the standard library.

- `zevpm`: `new`, `init`, `build [--release]`, `run [-- args]`, `test [filter]`, `clean`, `get`, `remove`, `update` (`fmt` waits for zevfmt)
- `zevic.toml` (`[package]`, `[deps]`, `[native]`); dependencies from `git` (tag, branch or commit) or a local `path`; downloads are cached in `~/.zevpm` (`ZEVPM_HOME`), the exact commits go to `zevic.lock`; dependencies of dependencies are fetched too, and two different versions of one package are an error; a dependency's `native/` files are built with the program
- `kind = "lib"`: `zevic --lib` makes a static library (with the runtime and the collector inside) and `--emit-header` its C header; `zevpm build` puts them in `build/` and `build/include/`
- tests: `test "name" { }` blocks and `_test.zev` files (ignored in normal builds), `zevic --test`, `Zevic.Test` (`Equal` `NotEqual` `True` `False` `Some` `None` `Ok` `Err` `Panics` `Fail`); a panic inside a test fails only that test; failures show the file and line of the assertion
- `fn Main(args: Array<String>)`
- `Array.Sort`, `Array.SortBy` (stable) and `Array<String>.Join`
- standard library: `Zevic.Math`, `Zevic.Env`, `Zevic.Text`, `Zevic.Time`, `Zevic.Fs`, `Zevic.Process`, and reading from standard input in `Zevic.Console`
- examples 07 to 10 and `examples/apps/wordcount` (a zevpm package with tests); `tests/zevpm_test.py` and `tests/examples_test.py`

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
