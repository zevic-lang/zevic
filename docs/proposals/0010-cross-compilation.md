# Proposal 0010: cross-compilation (`--target`)

- Status: accepted (2026-11-21; steps 1 and 2 and the Zig support are implemented, see the specification)
- Date: 2026-11-20

## Motivation

`zevic` builds for the machine it runs on. To give a user on Linux arm64 or Windows arm64 an executable, someone has to run `zevic` on that kind of machine. The release archives of `zevic` itself get around it by building on a runner of each system, but a Zevic program written by a user cannot. A program that is meant for several systems (a command-line tool, say) needs a way to build for another system from one machine, and a CI job that builds for five systems should not need five runners.

How `zevic` builds today helps: it writes LLVM IR, and clang compiles and links it together with the runtime (`runtime/zevic_rt.c` and the vendored Boehm GC), both compiled from source. Both steps can be told a target. What is left is a few places that assume the host.

## Proposal

### The option

```
zevic --target linux-arm64 app.zev -o app
zevpm build --target windows-x64
```

`--target` takes a **name** from the table, or a full **LLVM triple** (`aarch64-unknown-linux-gnu`). The names are the same as in the release archives, so a name is also what the user sees on the download page.

| Name | Triple | Executable |
| --- | --- | --- |
| `linux-x64` | `x86_64-unknown-linux-gnu` | `app` |
| `linux-arm64` | `aarch64-unknown-linux-gnu` | `app` |
| `macos-x64` | `x86_64-apple-darwin` | `app` |
| `macos-arm64` | `arm64-apple-darwin` | `app` |
| `windows-x64` | `x86_64-pc-windows-msvc` | `app.exe` |
| `windows-arm64` | `aarch64-pc-windows-msvc` | `app.exe` |

Without `--target`, the host is the target, as today. `--target host` says so explicitly.

### What changes in the compiler

1. **The size of `CLong`** is taken from the target (4 on Windows, 8 on the others), not from the host. (`Main.zev` passes `4` or `8` to the code generator by looking at the host.)
2. **The name of the executable** (`.exe`) and the system libraries (`-lws2_32` for Windows targets) follow the target.
3. ~~The IR gets a `target triple` line.~~ Not needed: clang is given `--target`, and the IR has no triple of its own to override.
4. **clang is called with `--target=<triple>`**, and with `--sysroot` when one is given (see below), for the IR, the runtime, the GC, and the C and C++ files of a package.
5. **The cache of the runtime objects** (`zevic_rt`, `bgc`) is keyed by the target too, so building for two targets does not recompile every time.
6. **`Env.IsWindows()`** and the other run-time checks stay as they are: they ask the system the program runs on, which is the target.

### The C toolchain of the target

Compiling needs the target's C headers and libraries (a *sysroot*). `zevic` does not ship one. It looks, in this order:

1. `--sysroot <dir>` or `ZEVIC_SYSROOT`
2. a `ZEVIC_CLANG` that already knows the target. **`zig cc`** is the recommended one: it carries headers and libraries for Linux, Windows and macOS, so `zevic` runs it directly when `ZEVIC_CLANG` points to `zig` (the LLVM triples are turned into zig targets: `x86_64-windows-gnu`, `aarch64-macos`, `aarch64-linux-gnu` and so on), and uses the Zig that the installers can put next to `zevic` when `--target` is given and no compiler is named
3. the system's own cross packages (for example `gcc-aarch64-linux-gnu` on Debian). Debian's layout does not work with `--sysroot`; a wrapper given as `ZEVIC_CLANG` adds `--gcc-install-dir=/usr/lib/gcc-cross/aarch64-linux-gnu/<version>` instead (`tests/cross/linux-arm64.sh` does so, and runs the result under `qemu-user`)

If none of these works, the error says what to install, as it does for a missing clang today. The linker for a cross build is lld (`-fuse-ld=lld`).

Not planned: building for macOS from Linux or Windows without a macOS SDK (the SDK's license does not allow shipping it). With `zig cc` it can work, and the documentation says so.

### `zevpm`

- `zevpm build --target <name>` writes to `build/<name>/` (a normal build keeps `build/`).
- `zevpm run` and `zevpm test` with `--target` are errors, unless the target is the host, because the program cannot run here. (Running under `qemu-user` can be added later.)
- **`[native]` libraries**: `libs = ["curl"]` names a library of the host. For a cross build the library of the target must be found by the sysroot. The way to say that a library is different per target is left to the next section.

### Not in this proposal

- Conditional compilation (`#if` by target, or a `Target.OS` constant): there is no such feature today, and the runtime checks are enough for the standard library.
- WebAssembly and other non-hosted targets (see "future considerations" in the specification).
- Building every target in one command (`--all-targets`).

## Alternatives

- **Only add runners** (Linux arm64, Windows arm64 and so on) to the CI and the release workflow. It needs no change in the compiler, and is planned anyway for the archives of `zevic`. It does not help the users of the language, who have one machine.
- **Ship a cross toolchain with `zevic`.** Heavy (hundreds of MB), and the macOS SDK cannot be shipped.
- **Make `zevic` call `zig` itself.** It would tie the compiler to one project. (Decided otherwise, after a trial: a wrapper script cannot be started by `CreateProcess` on Windows, so `zevic` accepts `zig` as a compiler, as one option next to clang. clang stays the default.)

## Impact

- `tools/zevic1`: the option, the target table, the size of `CLong`, the executable suffix, the libraries, the cache key, the triple in the IR. `bootstrap/zevic.ll` is regenerated.
- `tools/zevpm`: `--target` and `build/<name>/`.
- The specification: a chapter "Cross-compilation" and the names in the table. **The names and the `--target` option become part of the public interface that 1.0 freezes** (proposal 0009), so they are to be settled before 1.0.
- Tests: golden tests for the host target and for a wrong name; CI jobs that build `linux-arm64` from `linux-x64` with `gcc-aarch64-linux-gnu`, run it with `qemu-user`, and build `windows-x64` from Linux with `zig cc` (build only).
- `Env.IsWindows()` and the rest of the standard library: no change.

## Result of the trial with Zig

`zig cc` (0.17.1) built `examples/01_hello` for all five targets tried from Linux x64: `windows-x64` (PE32+), `windows-arm64` (PE32+ ARM64), `macos-x64` and `macos-arm64` (Mach-O), `linux-arm64` (ELF aarch64). The runtime and the GC compile as they are. The programs were not run. Windows targets are MinGW-ABI.

## Open questions

- Is the `[native]` section per target (`[native.windows-x64]`) needed, or is the sysroot enough?
- Do we require `zig cc` for non-Linux targets, or support more than one toolchain?
- Should `zevpm build --release --target X` produce the archive (`.tar.gz` or `.zip`) as the release workflow does?
- A wrong `CLong` size is silent for a hand-written `extern "C"` function. Should the compiler warn when a `CLong` signature meets a Windows target?
