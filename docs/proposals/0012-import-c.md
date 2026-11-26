# Proposal 0012: `import C` and `zevic bind` (no border with C)

- Status: stage 1 implemented (`zevic bind`); stage 2 (`import C`) accepted, not yet implemented
- Date: 2026-11-26

## Motivation

Zevic aims to be used, and a language is used when its libraries are there. C has the libraries. Today a Zevic package that wraps a C library (sqlite, raylib, SDL2, curl) writes the `extern "C"` declarations by hand, and for what `extern` cannot express (a struct passed by value, a macro) writes a C shim. The goal: **there is no border with C**. The test of it: the sqlite and raylib wrappers have **0 lines of C**.

`extern` stays, for the single function and for what the compiler cannot read. `import C` joins it.

## Stage 1: `zevic bind` (implemented)

`zevic bind header.h --module Name --strip-prefix p_ -o src/Name.zev` writes a module (spec: "`zevic bind`"). clang gives the syntax tree (`-ast-dump=json`) and the `#define`s (`-dD -E`). The module is committed with the package, so users of the package need no clang.

- Capitalized `extern` aliases are public, so the module has no wrapper functions and a callback can be passed straight to C.
- `Null` for a callback.
- Checked with `sqlite3.h` (Open, Exec, Prepare, Step, Close from Zevic) and `zlib.h`.

## Stage 2: `import C`

```
import C "sqlite3.h" as Sqlite3 (prefix: "sqlite3_", constants: "SQLITE_")
```

The compiler does what `zevic bind` does, at compile time, and no file is committed: the header is read by clang (found by `-I`, `[native] include_dirs`), and the names are used as members of the module `Sqlite3`. The mapping is the one of stage 1, with these additions, which stage 1 cannot do because they need the compiler:

1. **Structs by value.** The compiler generates a small C file (a shim) next to the user's code: for `Vector2 Vector2Add(Vector2 a, Vector2 b)` it writes `void zevic_shim_Vector2Add(Vector2 *a, Vector2 *b, Vector2 *out) { *out = Vector2Add(*a, *b); }`, compiles it with clang, and calls it. The C compiler does the calling convention of the platform, which is the part that is hard to get right by hand. The user never sees the shim.
2. **Out-parameters** (`int *out`, `sqlite3 **db`): the pointer form works today. A later step may offer `(T, CInt)` results.
3. **Callbacks with a `void *user`**: the `Handle` convention of the spec stays; `import C` types the callback.
4. **Macros** that are not plain numbers (`SQLITE_ABORT_ROLLBACK`, a constant expression of other macros): evaluated by clang (`__builtin_constant_p` or a generated `static const`).
5. **Variadic functions** (`printf`-like): bound by a shim with a fixed argument list, one per call shape the program uses.

Open questions: how an `import C` package is published (the header is needed at the user's build time, or `zevic bind` output is committed), and `zevpm` keys (`[native] headers`).

## Not in this proposal

C++ headers (`import C++`): later, after the C case has proved itself.

## Success metric

sqlite and raylib packages with 0 lines of C, and the examples of them unchanged in behavior.
