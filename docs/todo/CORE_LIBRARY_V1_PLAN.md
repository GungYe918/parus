# Core Library v1 Plan (`core::ext` First)

문서 상태: `active`

## 목표

1. `core::cabi`를 제거하고 `core::ext`를 C interop 표면으로 고정한다.
2. `use c_void;`류 core builtin use를 실제 builtin 타입 등록 경로로 사용한다.
3. `vaList`를 opaque C ABI 경계 타입으로 고정한다.

## v1 범위

1. `sysroot/core/config.lei`
2. `sysroot/core/core.lei`
3. `sysroot/core/ext/ext.lei`
4. `sysroot/core/ext/types.pr`
5. `sysroot/core/ext/errors.pr`
6. `sysroot/core/ext/cstr.pr`

## 공개 표면(`core::ext`)

1. 원시 타입: `c_void`, `c_char`, `c_schar`, `c_uchar`, `c_short`, `c_ushort`, `c_int`, `c_uint`, `c_long`, `c_ulong`, `c_longlong`, `c_ulonglong`, `c_float`, `c_double`, `c_size`, `c_ssize`, `c_ptrdiff`
2. opaque 타입: `vaList`
3. 문자열 경계 타입: `CStr`
4. 에러 타입: `FromBytesWithNulError`, `FromBytesUntilNulError`
5. CStr helper:
   - static: `from_ptr`, `from_raw_parts`, `from_bytes_with_nul`, `from_bytes_until_nul`, `from_bytes_with_nul_unchecked`
   - instance: `as_ptr`, `to_bytes`, `to_bytes_with_nul`, `len`, `len_with_nul`, `is_empty`, `count_bytes`
   - free forwarding wrapper: 유지(legacy core caller compatibility)

타입 매핑 원칙:

1. `c_*` 타입군은 대응 C ABI 원시 타입과 transparent alias로 동작한다.
2. `c_long/c_ulong` 폭은 target ABI를 따른다.
3. `c_size/c_ssize/c_ptrdiff`는 target pointer-width를 따른다.
4. `vaList`는 opaque 타입이며 C ABI 파라미터 경계에서만 허용한다.

## 비범위

1. `$c_str` 매크로
2. alloc 의존 타입(`CString`)
3. C 매크로 고급 변환(`##`, `#`, statement macro)

## Rust `core::ffi` parity (v1.5)

| Rust `core::ffi` surface | Parus `core::ext` status | Notes |
| --- | --- | --- |
| `c_void`, `c_char`, `c_int`, ... | Implemented | Exposed via `use c_*;` builtin aliases |
| `VaList` | Implemented (restricted) | Opaque type, C ABI parameter boundary only |
| `CStr` | Implemented (expanded subset) | Borrowed view API with static/instance helpers |
| `FromBytesWithNulError` | Implemented | Minimal error enum |
| `FromBytesUntilNulError` | Implemented | Minimal error enum |
| `CString` | Excluded | Requires allocator/owned buffer model |
| `c_str!` macro | Excluded | Deferred to macro round |

미지원(다음 라운드) 항목:

| 영역 | 상태 | 비고 |
| --- | --- | --- |
| `CString`/owned C string | 미지원 | alloc 의존 |
| `CStr::to_str` UTF-8 변환 | 미지원 | text/UTF-8 policy 확정 후 추가 |
| `c_str!`-equivalent compile-time macro | 부분 | 현재 `$c""/$cr""` literal-only 경로 |
| raw literal strict `cr` semantics 분리 | 미지원 | 현재 `cr`는 `c` alias |

## 안전 규칙

1. core `.pr` 파일은 `$![Impl::Core];`를 첫 item으로 둔다.
2. `use c_*;`는 core bundle의 `ext` 모듈에서만 허용한다.
3. `vaList`는 C ABI 시그니처 경계에서만 허용한다.
4. `core::ext`의 public builtin/function 주석은 영어로 작성한다.
