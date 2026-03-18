# C Import (libclang) Preparation Plan

> Note: This document started as a v1 prep note.  
> Current implementation has moved to v2.3+ freeze support (`-I/-isystem`, `-D/-U/-include/-imacros`, limited variadic + manual[abi], union manual gate, C ABI positional-only, object-like macro const-expression subset, function-like strict promotion + chain/cycle diagnostics, anonymous decl synthetic names, external C global/TLS import).

## Scope in this round
- Enable user-facing C-header import syntax: `import "Header.h" as alias;`.
- Build libclang-backed declaration manifest for C functions/types and inject into external symbol surface.
- Import external-linkage C global variables (including `_Thread_local`/`__thread`) with TLS metadata.
- Support include path flags for cimport: `-I`, `-isystem`.
- Support limited C variadic calls (no Parus-side format bridge).
- Add union metadata import and `manual[get/set]` gate for union dot access.

## Current C mapping status target (v1)
| C feature | Target in v1 |
|---|---|
| function declaration | supported |
| global variable declaration | supported (with const/tls metadata) |
| named struct | supported |
| named enum | supported |
| typedef (scalar/pointer/fnptr/record alias) | supported (`scalar/pointer/fnptr` transparent, record nominal) |
| function pointer typedef | supported (fixed-arg alias call path) |
| object-like macro | partial (constant-only) |
| function-like macro | partial (strict promotable subset + 1-step macro-chain) |
| union | supported subset (`manual[get/set]` gated typed raw overlay; no active-field tracking) |
| bitfield | supported subset (direct IR/LLVM lowering; exotic/ambiguous layout hard error) |
| anonymous record/enum | partial (synthetic `__anon_*` name import) |
| variadic call lowering | supported subset (`manual[abi]`, ABI-safe scalar/raw ptr/CStr/plain string literal, unsuffixed int literal -> i32) |

## What is required for full support of currently unsupported/partial items

### 1) `union` future safety work
- Add a first-class union type node in Parus type/ABI metadata.
- Preserve C field offsets and max-size alignment semantics.
- Enforce active-field read/write diagnostics in safe mode.
- Provide explicit unsafe escape hatch for reinterpret access.
- Extend OIR/SIR memory model for union load/store legality checks.

### 2) bitfield future coverage work
- Expand currently accepted exact-layout subset to more packed/target-specific cases.
- Add more target-endian / cross-compiler parity tests.

### 3) function-like macro support
- Current manifest/recipe strict mode:
  - Promote only single-call forwarding forms (`CALLEE(args...)`).
  - Argument forms are limited to direct parameter forwarding or simple cast forwarding (`(T)param`).
  - `DirectAlias` uses original C symbol without shim.
  - `IRWrapperCall` lowers reorder/cast forwarding directly into Parus IR/OIR wrapper bodies.
- Excluded in this round:
  - token paste/stringize (`##`, `#`)
  - statement macro / GNU extension dependent macro
  - variadic function-like macro

### 4) function-pointer alias future work
- Extend function type identity so variadic function-pointer aliases carry ABI metadata directly.
- Keep indirect call lowering aligned with C ABI metadata for alias-heavy APIs.

### 5) variadic calls future work
- Keep direct lowering only; format bridge stays out of scope.
- Extend metadata path if variadic function-pointer aliases become a priority.

## Build/dependency policy
- Vendored headers live in `third_party/libclang/include/clang-c`.
- Shared library path is discovered at CMake configure time.
- `cimport` 결과는 target/sysroot/sdk/include/define fingerprint 기반 manifest cache로 저장한다.
- Parus compiler는 generated C shim을 만들지 않으며 C compiler/clang driver를 호출하지 않는다.
- CMake option gates:
  - `PARUS_ENABLE_CIMPORT`
  - `PARUS_CIMPORT_REQUIRE_LIBCLANG`
  - `PARUS_LIBCLANG_ROOT`, `PARUS_LIBCLANG_INCLUDE_DIR`, `PARUS_LIBCLANG_LIBRARY`

## Next implementation step
1. Expand unsupported macro handling into explicit `c bridge` library affordances.
2. Expand bitfield coverage beyond the current exact-layout subset.
3. Keep manifest schema stable across compiler/LSP cache readers.
4. Add more target-specific ABI regression coverage.
