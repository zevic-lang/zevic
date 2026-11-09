# Interop with other languages

Zevic speaks the C ABI, so anything that can call C can use a Zevic library, and Zevic can call
anything that exposes `extern "C"` functions.

| Direction | How | Files |
|---|---|---|
| C++ → Zevic | `zevic --lib` gives `libwordlib.a` and a header; link it | `cpp/host.cpp` |
| Rust → Zevic | `extern "C"` block, `-l static=wordlib` | `rust/host.rs` |
| Go → Zevic | cgo with `#include` of the header | `go/main.go` |
| Python → Zevic | `--lib --pic`, make a shared library, `ctypes` | `python/host.py` |
| Zevic → Rust | `rustc --crate-type staticlib`, pass the `.a` to `zevic` | `rust/fib.rs`, `rust/main.zev` |
| Zevic → C++ | pass the `.cpp` to `zevic`; the door is `extern "C"` | `cpp/stats.cpp`, `cpp/main.zev` |

The Zevic library is `zevlib/wordlib.zev` (`export "C"` functions). The runtime and the garbage
collector are inside the archive; the host calls `zevic_init(argc, argv)` once first.

Run everything (a language whose toolchain is missing is skipped), from the repository root:

```sh
sh examples/interop/run.sh            # or: make test-interop
```

Notes:
- Pointers and lengths cross the boundary separately (`*F64` and `CSizeT`); strings are `CString`.
- Shared libraries (Python) need `--pic`; the shared-library step above is written for Linux.
- Linking Rust needs `-lpthread -ldl -lm` on Linux.
