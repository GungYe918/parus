# C Import (libclang) Preparation Plan

> Note: This document started as a v1 prep note.  
> Current implementation has moved to v2.2 partial support (`-I/-isystem`, `-D/-U/-include/-imacros`, limited variadic, union manual gate, C ABI positional-only, object-like macro constants, anonymous decl synthetic names).

## Scope in this round
- Enable user-facing C-header import syntax: `import "Header.h" as alias;`.
- Build libclang-backed declaration manifest for C functions/types and inject into external symbol surface.
- Support include path flags for cimport: `-I`, `-isystem`.
- Support limited C variadic calls (no Parus-side format bridge).
- Add union metadata import and `manual[get/set]` gate for union dot access.

## Current C mapping status target (v1)
| C feature | Target in v1 |
|---|---|
| function declaration | supported |
| global variable declaration | supported |
| named struct | supported |
| named enum | supported |
| typedef (scalar/pointer/record alias) | supported |
| function pointer typedef | partial |
| object-like macro | partial (constant-only) |
| function-like macro | partial (strict promotable subset only) |
| union | partial (`manual[get/set]` gated dot access) |
| bitfield | not supported |
| anonymous record/enum | partial (synthetic `__anon_*` name import) |
| variadic call lowering | partial (ABI-safe scalar/pointer only) |

## What is required for full support of currently unsupported/partial items

### 1) `union` full support
- Add a first-class union type node in Parus type/ABI metadata.
- Preserve C field offsets and max-size alignment semantics.
- Enforce active-field read/write diagnostics in safe mode.
- Provide explicit unsafe escape hatch for reinterpret access.
- Extend OIR/SIR memory model for union load/store legality checks.

### 2) bitfield full support
- Add bitfield metadata (base type, bit width, bit offset, signedness).
- Lower reads/writes with mask/shift + target-endian rules.
- Add C ABI backend tests for packing and cross-compiler parity.

### 3) function-like macro support
- Current v2.2+ strict mode:
  - Promote only single-call forwarding forms (`CALLEE(args...)`).
  - Argument forms are limited to direct parameter forwarding or simple cast forwarding (`(T)param`).
  - `DirectAlias` uses original C symbol without shim.
  - `ShimForward` auto-generates a C shim object and links it.
- Excluded in this round:
  - token paste/stringize (`##`, `#`)
  - statement macro / GNU extension dependent macro
  - variadic function-like macro

### 4) function-pointer alias full support
- Extend function type identity in symbol and overload resolution.
- Support pointer-to-function call path from imported aliases.

### 5) variadic calls
- Add explicit vararg ABI bridge path for C calling convention.
- Gate argument type set to ABI-safe primitives/pointers.

## Build/dependency policy
- Vendored headers live in `third_party/libclang/include/clang-c`.
- Shared library path is discovered at CMake configure time.
- CMake option gates:
  - `PARUS_ENABLE_CIMPORT`
  - `PARUS_CIMPORT_REQUIRE_LIBCLANG`
  - `PARUS_LIBCLANG_ROOT`, `PARUS_LIBCLANG_INCLUDE_DIR`, `PARUS_LIBCLANG_LIBRARY`

## Next implementation step
1. Parse `import "Header.h" as alias;` into dedicated AST kind.
2. Call libclang to build a declaration manifest.
3. Inject manifest symbols into name-resolve external surface.
4. Map supported declarations to Parus types/symbols.
5. Add diagnostics for unsupported features listed above.
