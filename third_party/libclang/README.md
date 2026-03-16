# Vendored libclang C API Headers

This directory holds the `clang-c` public headers used by Parus C-import preparation.

## Source
- Upstream project: LLVM/Clang (`libclang` C API)
- Typical local source on macOS/Homebrew: `/opt/homebrew/opt/llvm/include/clang-c`

## Refresh
Run:

```bash
./scripts/setup_libclang_third_party.sh
```

This syncs:
- `third_party/libclang/include/clang-c/*`
- `third_party/libclang/LICENSE.TXT` (if available)

## Notes
- This folder intentionally vendors headers only.
- Runtime linking still uses a discovered `libclang` shared library path from CMake.
