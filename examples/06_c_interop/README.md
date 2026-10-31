# C interop

Zevic calls C (`extern "C"`) and C calls Zevic (`export "C"`).

```sh
zevic examples/06_c_interop/main.zev examples/06_c_interop/native.c -o interop
./interop
```

Add `--emit-header zevic.h` to get a C header for the `export "C"` functions.
