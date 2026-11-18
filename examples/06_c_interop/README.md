# C interop

Zevic calls C (`extern "C"`) and C calls Zevic (`export "C"`).

```sh
cd examples/06_c_interop && zevpm run
```

Add `--emit-header zevic.h` to get a C header for the `export "C"` functions.
