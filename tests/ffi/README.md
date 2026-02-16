# FFI Manual Smoke

## Prerequisite

Build once from repo root:

```bash
./run.sh
```

## 1) Hello, World (`text` + `extern "C"` `printf` wrapper)

```bash
/usr/bin/clang -c tests/ffi/hello_printf_wrapper.c -o /tmp/hello_printf_wrapper.o
./build/compiler/parusc/parusc tests/ffi/hello_printf.pr -Xparus -emit-object -o /tmp/hello_printf.o
/usr/bin/clang++ /tmp/hello_printf.o /tmp/hello_printf_wrapper.o -o /tmp/hello_printf.bin
/tmp/hello_printf.bin
```

Expected output:

```text
Hello, World
```

Note:
- C wrapper receives Parus `text` as `{data,len}` (UTF-8 span compatible).
- Output check is byte-exact (`Hello, World` without newline).

## 2) Scalar extern call

```bash
/usr/bin/clang -c tests/ffi/extern_arith_wrapper.c -o /tmp/extern_arith_wrapper.o
./build/compiler/parusc/parusc tests/ffi/extern_arith.pr -Xparus -emit-object -o /tmp/extern_arith.o
/usr/bin/clang++ /tmp/extern_arith.o /tmp/extern_arith_wrapper.o -o /tmp/extern_arith.bin
/tmp/extern_arith.bin
echo $?
```

Expected exit code: `0`

## 3) C calling Parus `export "C"`

```bash
/usr/bin/clang -c tests/ffi/export_to_c_main.c -o /tmp/export_to_c_main.o
./build/compiler/parusc/parusc tests/ffi/export_to_c.pr -Xparus -emit-object -o /tmp/export_to_c.o
/usr/bin/clang++ /tmp/export_to_c.o /tmp/export_to_c_main.o -o /tmp/export_to_c.bin
/tmp/export_to_c.bin
echo $?
```

Expected exit code: `0`

## 4) Extern global roundtrip

```bash
/usr/bin/clang -c tests/ffi/extern_global_counter_wrapper.c -o /tmp/extern_global_counter_wrapper.o
./build/compiler/parusc/parusc tests/ffi/extern_global_counter.pr -Xparus -emit-object -o /tmp/extern_global_counter.o
/usr/bin/clang++ /tmp/extern_global_counter.o /tmp/extern_global_counter_wrapper.o -o /tmp/extern_global_counter.bin
/tmp/extern_global_counter.bin
echo $?
```

Expected exit code: `0`
