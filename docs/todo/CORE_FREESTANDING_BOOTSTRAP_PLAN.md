# Parus Core Freestanding Bootstrap Plan

문서 상태: `active`

primitive별 실제 구현 우선순위와 후속 phase는 `docs/todo/CORE_PRIMITIVE_ROADMAP.md`를 따른다.  
`core::text`의 현재 구현 경계와 언어 코어 기반 surface는 `docs/todo/CORE_TEXT_PLAN.md`를 따른다.  
`Impl::*`와 `inst`의 장기 역할 분리는 `docs/todo/INST_V2_MODEL.md`를 따른다.

## 목표

`std`가 올라가기 전에, `core`를 다음 성질로 고정한다.

1. freestanding에서 동작해야 한다.
2. OS, syscall, thread runtime, allocator, heap 할당에 의존하지 않는다.
3. `Parus` 언어 builtin과 가장 가까운 표면만 먼저 제공한다.
4. primitive builtin(`i32`, `usize`, `f32`, `bool`, `char`, `text`)에는 `acts for`를 사용해 메서드형 API를 붙인다.
5. `core::ext`는 C ABI 경계 전용 surface로 유지하고, 일반-purpose `core`와 역할을 분리한다.

## Rust `core` 참고 기준

로컬 Rust `core`(`library/core/src/lib.rs`)에서 freestanding 초기에 유효한 축은 대체로 다음이다.

1. `cmp`
2. `clone`
3. `convert`
4. `default`
5. `marker`
6. `mem`
7. `num`
8. `ops`
9. `option`
10. `range`
11. `slice`
12. `str`
13. `hint`
14. `panic`

하지만 Parus는 그대로 가져오지 않는다.

Parus 차이는 다음과 같다.

1. `Option`은 별도 타입이 아니라 builtin nullable `T?`가 있다.
2. `str`는 별도 DST가 아니라 builtin `text`가 있다.
3. `Result`는 현재 core 강제 대상이 아니다.
4. `alloc`, `fmt`, `io`, `sync`, `task`, `future`, `net`, `time`은 초기 `core` 범위 밖이다.
5. primitive API는 trait보다 `acts for builtin`이 더 자연스럽다.

따라서 Parus 초기 `core`는 Rust `core`의 모듈 이름보다, 아래 역할 분리로 보는 편이 맞다.

1. 언어-인접 모듈
2. primitive acts
3. freestanding utility
4. generic helper

## 비범위

초기 `core`에서 명시적으로 제외한다.

1. heap 소유 타입: `String`, `Vec`, `Box`, `CString`
2. OS/API 의존 타입: file, process, env, socket, time source
3. thread/async/runtime surface
4. formatting engine, printf-style formatting
5. allocator API
6. panic formatting/pretty backtrace
7. C import 확장 기능

이 항목들은 `std`, `alloc`, runtime, 또는 별도 `c bridge` 레이어로 보낸다.

## 설계 원칙

### 1. core는 두 레인으로 나눈다

1. pure Parus로 구현 가능한 레인
2. compiler/codegen contract가 필요한 레인

초기 구현은 pure Parus 레인을 먼저 닫고, compiler-owned 레인은 `Impl::*` attached binding으로 표면을 먼저 고정한 뒤 순차 구현한다.

### 2. primitive acts는 `core` 번들 안에서만 정의한다

현재 컴파일러 정책상 builtin type의 `acts for`는 다음 조건에서만 허용된다.

1. owner가 builtin type이어야 한다
2. bundle이 `core`여야 한다
3. 파일 시작에 `$![Impl::Core];` marker가 있어야 한다

따라서 primitive API는 `sysroot/core` 안의 dedicated source 파일에서만 정의한다.

### 3. alloc 없는 값 위주로 간다

초기 `core` API는 아래 값만 다룬다.

1. builtin scalar
2. `text`
3. `T?`
4. `T[N]`
5. `T[]`
6. `enum`/`struct` value
7. borrow `&T`, `&mut T`

### 4. generic은 “얕게” 시작한다

generic surface가 유용하더라도, 초기에 너무 많은 combinator를 넣지 않는다.

초기 generic은 아래 정도로 제한한다.

1. simple helper free function
2. `Ordering`, `Range` 같은 small value enum/struct
3. `T?`/`T[]` helper

## 목표 모듈 구조

초기 목표 구조는 다음과 같다.

1. `core::ext`
2. `core::cmp`
3. `core::num`
4. `core::char`
5. `core::text`
6. `core::mem`
7. `core::hint`
8. `core::ops`

현재 최소 구현이 들어간 모듈:

1. `core::ext`
2. `core::cmp`
3. `core::bool`
4. `core::num`
5. `core::char`
6. `core::text`
7. `core::mem`
8. `core::hint`

선택적 2차 모듈:

1. `core::range`
2. `core::convert`
3. `core::marker`
4. `core::panic`

의도적으로 보류:

1. `core::fmt`
2. `core::hash`
3. `core::slice`
4. `core::result`

`core::iter`는 보류 대상이지만, 계층상으로는 장기적으로 `std`가 아니라 `core`에 둔다. 구체적인 도입 순서는 roadmap 문서를 따른다.

## 1차 함수셋

### A. `core::cmp`

초기 surface:

1. `export enum Ordering { case Less, case Equal, case Greater }`
2. free fn:
   - `min<T>(a: T, b: T) -> T`
   - `max<T>(a: T, b: T) -> T`
   - `clamp<T>(x: T, lo: T, hi: T) -> T`

2차 surface:

1. `proto Eq`
2. `proto Ord`
3. `cmp(self, rhs: &Self) -> Ordering`

초기에는 generic constraint보다 primitive acts와 simple free fn이 더 중요하다.

### B. `core::num`

`core::num`은 서브셋을 강하게 자른다.

#### B-1. int-like primitive acts 1차

대상:

1. `i8`, `i16`, `i32`, `i64`, `i128`
2. `u8`, `u16`, `u32`, `u64`, `u128`
3. `isize`, `usize`

1차에 넣을 메서드:

1. `min(self, rhs: Self) -> Self`
2. `max(self, rhs: Self) -> Self`
3. `clamp(self, lo: Self, hi: Self) -> Self`
4. `is_zero(self) -> bool`

signed 전용 1차:

1. `abs(self) -> Self`
2. `signum(self) -> Self`
3. `is_positive(self) -> bool`
4. `is_negative(self) -> bool`

이 집합은 pure Parus 연산자/조건식으로 구현 가능하다.

#### B-2. int-like primitive acts 2차

compiler intrinsic 또는 overflow 정책 고정 후 추가:

1. `checked_add/sub/mul/div/rem`
2. `wrapping_add/sub/mul`
3. `saturating_add/sub/mul`
4. `count_ones`
5. `leading_zeros`
6. `trailing_zeros`
7. `rotate_left/right`
8. `swap_bytes`
9. `reverse_bits`
10. `to_le/be/ne`
11. `from_le/be/ne`
12. `pow`

이 함수들은 언어/백엔드 intrinsic contract를 먼저 정해야 한다.

#### B-3. float-like primitive acts 1차

대상:

1. `f32`
2. `f64`
3. `f128` (lane support가 있는 경우)

1차 메서드:

1. `min(self, rhs: Self) -> Self`
2. `max(self, rhs: Self) -> Self`
3. `clamp(self, lo: Self, hi: Self) -> Self`
4. `abs(self) -> Self`

2차 메서드:

1. `is_nan(self) -> bool`
2. `is_finite(self) -> bool`
3. `is_infinite(self) -> bool`
4. `floor(self) -> Self`
5. `ceil(self) -> Self`
6. `round(self) -> Self`
7. `trunc(self) -> Self`
8. `fract(self) -> Self`
9. `sqrt(self) -> Self`
10. `mul_add(self, a: Self, b: Self) -> Self`
11. `copysign(self, sign: Self) -> Self`

`abs/min/max/clamp`만 먼저 넣는 이유는 freestanding core 초기에 가장 자주 쓰이고, 구현도 단순하기 때문이다.

### C. `core::char`

primitive acts 대상: `char`

1차 메서드:

1. `is_ascii(self) -> bool`
2. `is_ascii_digit(self) -> bool`
3. `is_ascii_alpha(self) -> bool`
4. `is_ascii_alnum(self) -> bool`
5. `to_ascii_lower(self) -> char`
6. `to_ascii_upper(self) -> char`

2차 메서드:

1. `len_utf8(self) -> usize`
2. `to_digit(self, radix: u32) -> u32?`

초기 `char`는 ASCII 중심으로만 닫는다. full Unicode 분류는 `std` 이후다.

### D. `core::text`

primitive acts 대상: `text`

`text`는 builtin 비소유 UTF-8 view이므로, 초기 `core::text`는 아주 작게 잡는다.

현재 최소 surface는 다음 언어 코어 확장 위에서 구현된다.

1. prefix deref `*`
2. builtin view fields `.len`, `.data`
3. `text{ data, len }` zero-copy view constructor

1차 메서드:

1. `is_empty(self) -> bool`
2. `len_bytes(self) -> usize`

2차 메서드:

1. `starts_with(self, prefix: text) -> bool`
2. `ends_with(self, suffix: text) -> bool`
3. `eq_ignore_ascii_case(self, rhs: text) -> bool`

보류:

1. substring/slicing
2. splitting iterator
3. UTF-8 decoding iterator
4. owned conversion

즉 초기 `core::text`는 freestanding view API만 담당하고, 이후 std의 `String`과 책임이 겹치지 않게 유지한다.

### E. `core::mem`

초기 `mem`은 위험한 low-level API보다 “언어-adjacent query”부터 연다.

1차 free fn:

1. `size_of<T>() -> usize`
2. `align_of<T>() -> usize`

현재 구현 정책:

1. `size_of<T>()`는 compiler-owned 경로에서는 fake body를 두지 않는다
2. `$![Impl::SizeOf] export def size_of<T>() -> usize;`로 선언한다
3. 같은 key에 body가 있으면 library-owned ordinary function으로 취급한다
3. compiler가 ABI/layout contract를 책임진다

2차 free fn:

1. `swap<T>(a: &mut T, b: &mut T) -> void`
2. `replace<T>(slot: &mut T, value: T) -> T`
3. `take<T>(slot: &mut T) -> T`

명시 보류:

1. `transmute`
2. `zeroed`
3. `uninitialized`
4. `MaybeUninit`
5. raw memory copy/set APIs

이들은 ABI/UB 규칙이 먼저 더 단단해져야 한다.

### F. `core::hint`

초기 free fn:

1. `unreachable() -> never`
2. `spin_loop() -> void`

이 둘은 freestanding/core에서 유용하고, `std` 의존이 없다.

현재 구현 정책:

1. `spin_loop()`는 compiler-owned 경로에서는 fake body를 두지 않는다
2. `$![Impl::SpinLoop] export def spin_loop() -> void;`로 선언한다
3. 같은 key에 body가 있으면 library-owned ordinary function으로 취급한다
4. `unreachable()`는 의미가 아직 trap/unchecked로 고정되지 않았으므로 별도 라운드까지 fake body를 유지한다

### G. `core::ops`

`ops`는 trait 복제보다 “언어 연산자 contract를 문서화한 alias layer”로 시작한다.

초기에는 구현보다 문서/구조 목적이 크다.

1. `copy` / `clone` / `drop` operator contract 정리
2. primitive builtin fast-path와 acts override 경계 정리
3. 이후 generic proto surface(`Add`, `Sub`, `Mul` 등)는 보류

Parus는 이미 operator와 acts가 언어 코어에 있으므로, Rust의 `core::ops`를 그대로 흉내내기보다 얇게 두는 것이 맞다.

## 초기 제외 대상

### 1. `Option`

Parus에는 builtin nullable `T?`가 있으므로 `core::option::Option<T>`를 초기 코어에 두지 않는다.

필요한 helper는 별도 free function으로 나눈다.

예:

1. `is_some<T>(x: T?) -> bool`
2. `is_none<T>(x: T?) -> bool`
3. `unwrap_or<T>(x: T?, fallback: T) -> T`

하지만 이것도 1차 primitive acts보다 뒤다.

### 2. `Result`

현재 spec은 `Result`를 core 필수로 강제하지 않는다.

따라서 `Result<T, E>`는 `std` 또는 추후 generic core 확장으로 미룬다.

### 3. `slice` 알고리즘

Parus에는 `T[]`가 있지만, Rust `core::slice` 수준의 알고리즘 표면을 초기에 들이면 generic/borrow/iterator 부담이 커진다.

초기에는 제외한다.

## 실제 구현 순서

### Phase 0. 구조 분리

1. `core::ext` 유지
2. `core.lei`를 다중 모듈 구조로 바꿀 준비
3. 새 source 파일:
   - `core/cmp.pr`
   - `core/num_int.pr`
   - `core/num_float.pr`
   - `core/char.pr`
   - `core/text.pr`
   - `core/mem.pr`
   - `core/hint.pr`

### Phase 1. pure Parus primitive acts

먼저 구현할 파일:

1. int-like acts
2. float-like small acts
3. char ASCII acts
4. `Ordering`
5. `text::{is_empty,len_bytes}` surface placeholder

여기서 중요한 원칙:

1. pure Parus로 구현 가능한 것부터 넣는다
2. overflow/endianness/bit-twiddle은 아직 넣지 않는다

### Phase 2. compiler intrinsic contract

다음 intrinsic family를 언어/백엔드 contract로 확정한다.

1. `size_of/align_of` (`Impl::*` binding으로 먼저 고정)
2. bit ops
3. checked/wrapping/saturating arithmetic
4. float classification and rounding
5. hint/unreachable/spin

이 단계가 끝나야 `core::mem`, `core::hint`, `core::num` 2차 함수셋이 닫힌다.

### Phase 3. generic helper

그 다음에만 다음을 넣는다.

1. optional helper free fn
2. `Eq`/`Ord` proto
3. small range helper

## 구현 판단 기준

새 `core` API를 넣을 때는 아래 질문을 먼저 통과해야 한다.

1. freestanding에서 동작하는가
2. allocator 없이 쓸 수 있는가
3. OS/runtime dependency가 없는가
4. pure Parus 또는 compiler intrinsic contract로 닫을 수 있는가
5. `std`가 이 API 위에 실제로 올라갈 수 있는가

이 기준을 못 통과하면 `core`가 아니라 `std` 또는 runtime 계층으로 보낸다.

## 추천 1차 구현 세트

실제로 다음 작업으로 바로 구현할 세트는 아래가 가장 좋다.

1. `core::cmp::Ordering`
2. `acts for` int-like builtin:
   - `min`
   - `max`
   - `clamp`
   - `is_zero`
   - `abs/signum/is_positive/is_negative` (signed only)
3. `acts for` float-like builtin:
   - `min`
   - `max`
   - `clamp`
   - `abs`
4. `acts for char`:
   - ASCII predicate/transform subset
5. `core::text`:
   - `is_empty`
   - `len_bytes`

이 세트는 `std`가 나중에 다음을 만들 때 바로 기반이 된다.

1. formatting 전 준비용 수치 helper
2. path/parser/tokenizer 류 ASCII helper
3. `String` 이전 단계의 text utility
4. error/compare/sort 기반 enum helper

## 구현 후 다음 단계

위 1차 세트가 끝나면 다음은 이 순서가 맞다.

1. `size_of/align_of`는 현재 `Impl::SizeOf`, `Impl::AlignOf`로 선언하고, bodyless면 compiler-owned / body 있으면 library-owned로 나눈다. 장기적으로는 `inst` query surface 성숙 이후 이관 가능성을 검토한다
2. optional helper free fn
3. text prefix/suffix API
4. checked/wrapping/saturating arithmetic
5. `std::c`
6. `std::io`
