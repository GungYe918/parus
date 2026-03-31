# Parus Core Primitive Roadmap

문서 상태: `reference`

이 문서는 `sysroot/core`의 primitive-facing surface를 단계적으로 채우기 위한 구현 로드맵이다.  
umbrella 방향성은 `docs/todo/CORE_FREESTANDING_BOOTSTRAP_PLAN.md`가 맡고, 이 문서는 실제 구현 우선순위와 phase 구분을 맡는다.  
`Impl::*`와 `inst`의 역할 분리는 `docs/todo/INST_V2_MODEL.md`를 따른다.

현재 상태 고정:

1. `~` core semantics는 owner-handle model 기준으로 freeze-ready다.
2. Recoverable 예외 코어는 non-unwind 값 채널로 고정되었다.
3. panic 기본 정책은 `abort/trap`이며, unwind는 hosted/interop 후속 레이어 과제다.
4. C ABI / `cimport` 경계는 항상 non-throwing이며, Recoverable은 명시적 wrapper에서만 변환한다.
5. alloc-backed dynamic owner container surface(`Vec<(~T)?>` 등)는 core가 아니라 alloc/std 후속 과제다.

## 원칙

1. `core`는 freestanding여야 한다.
2. `core`는 OS, allocator, heap, thread runtime에 의존하지 않는다.
3. builtin primitive API는 `acts for builtin`을 우선 사용한다.
4. Pure Parus only로 닫히는 surface를 먼저 구현한다.
5. compiler-owned surface는 fake body 대신 `Impl::*` binding으로 선언한다.
6. `Impl::*`는 body 유무로 ownership을 나눈다.
   - body 없음 + recognized key => compiler-owned
   - body 있음 => library-owned
7. 장기적인 compiler query / metaprogramming 확장은 `inst`가 맡고, immediate ABI/layout surface는 `Impl::*`가 맡는다.

## Primitive Coverage Matrix

| 영역 | 현재 구현 | 아직 비어 있는 표면 |
| --- | --- | --- |
| `core::cmp` | `Ordering`, `ordering_less/equal/greater`, `Ordering::{is_less,is_equal,is_greater,reverse}`, generic `min/max/clamp` | Eq/Ord proto, compare free fn family beyond `Comparable` |
| signed int (`i8..i128`, `isize`) | `cmp`, `min`, `max`, `clamp`, `is_zero`, `is_even`, `is_odd`, `pow`, `abs`, `signum`, `is_positive`, `is_negative`, `div_euclid`, `rem_euclid` | `abs_diff`, checked/wrapping/saturating, bit ops, endian helpers |
| unsigned int (`u8..u128`, `usize`) | `cmp`, `min`, `max`, `clamp`, `is_zero`, `is_even`, `is_odd`, `pow`, `div_euclid`, `rem_euclid`, `is_power_of_two` | checked/wrapping/saturating, bit ops, endian helpers |
| float (`f32`, `f64`) | `partial_cmp`, `min`, `max`, `clamp`, `abs`, `is_nan`, `signum` | `total_cmp`, classification, rounding, `copysign`, `mul_add` |
| `char` | `cmp`, `len_utf8`, ASCII digit/alpha/alnum/upper/lower/hexdigit/whitespace/control, case conversion, `eq_ignore_ascii_case`, `to_digit` | Unicode classification |
| `bool` | `cmp`, `is_true`, `is_false` | richer logical helper surface |
| `text` | `is_empty`, `len_bytes`, `as_ptr` | prefix/suffix, ASCII-insensitive compare, bytes/iteration, indexing/slicing |
| `core::slice` | `len`, `is_empty`, `as_ptr`, `subslice`, `take/drop_front`, `take/drop_back`, `equal`, `starts_with`, `ends_with` | acts-based slice owner API, text byte-view bridge |
| `core::convert` | `Into<T>`, constrained free `into` helper | concrete impl inventory, fallible conversion, future handle-oriented conversion layer |
| `core::mem` | `size_of`, `align_of`, `swap`, `replace` | `take`, raw memory API, uninit/MaybeUninit |
| `core::hint` | `unreachable`, `spin_loop` | `black_box`, branch hint, `assume` |
| `core::range` | `Range<T>`, `RangeInclusive<T>`, generic constructor, `is_empty`, `contains`, `contains_range`, `intersects`, `is_singleton`, iter bridge | `len`, step/stride, richer sugar |
| `core::iter` | `Iterator`, `IterSource*`, `RangeIter`, `RangeInclusiveIter`, `for`/`loop` bridge | adapter layer, callable ergonomics, builtin source special-case 제거 |

## Constraint Surface

`core::constraints`는 현재 builtin family proto surface를 맡는다.

1. `Comparable`
2. `BinaryInteger`
3. `SignedInteger`
4. `UnsignedInteger`
5. `BinaryFloatingPoint`

이 다섯 proto는 지금은 compiler-owned satisfaction table을 사용한다.  
다만 아래 두 조건 중 하나가 들어오면 library-only 후보로 다시 검토한다.

1. builtin/external type에 대한 proto conformance 선언 표면
2. `acts`/operator availability에서 structural proto satisfaction을 유도하는 규칙

## Float Comparison Policy

`f32/f64`는 정수와 같은 total order를 기본 비교로 두지 않는다.

1. 기본 비교는 `partial_cmp(self, rhs) -> core::cmp::Ordering?`
2. NaN이 개입하면 `partial_cmp`는 `null`
3. `total_cmp(self, rhs) -> core::cmp::Ordering`는 intrinsic/bit-level contract가 필요하므로 Phase 2 이후로 미룬다
4. 기존 `float.cmp()`는 seed 단계 정리 대상으로 보고 유지하지 않는다

Phase 1의 `min/max/clamp/signum`은 Pure Parus only 구현을 우선한 표면이다.  
NaN와 ordered-bound 정책은 Phase 2에서 intrinsic-backed 표면이 들어올 때 다시 검토한다.

## Iterator Placement Decision

`iterator`는 장기적으로 `std`가 아니라 `core` 계층에 둔다.

이유:

1. iterator는 alloc/OS/thread에 의존하지 않는 freestanding abstraction이다
2. range, slice, text view, adapter는 `core`에서 조합될 수 있다
3. `std`는 I/O, OS, runtime 표면 위에 서야 하고, iterator는 그보다 더 아래 계층이다

현재 V1 foundation은 이미 도입되었다. 후속 라운드의 핵심은 adapter와 protocol-first 일반화다.

## Phase Split

### Phase 1: Pure Parus only

이미 구현되었거나, 지금 기준으로 compiler 변경 없이 닫을 수 있는 영역:

1. `core::cmp::Ordering`
2. `core::bool::{cmp,is_true,is_false}`
3. integer primitive acts의 기본 비교/경계/유클리드 나눗셈
4. float primitive acts의 `partial_cmp`, `min/max/clamp`, `abs`, `is_nan`, `signum`
5. `char` ASCII helper, `to_digit`, `len_utf8`
6. `text` 최소 view acts (`is_empty`, `len_bytes`, `as_ptr`)
7. `core::slice` free-function view helper
8. `core::ext::CStr -> text` bridge
9. `core::mem::{size_of, align_of, swap, replace}`
10. `core::hint::{unreachable, spin_loop}`
11. `core::cmp` generic `min/max/clamp`
12. integer `is_even/is_odd/pow`
13. `core::convert` contract-first surface (`Into<T>` + constrained free `into` helper)

### Phase 2: intrinsic-backed design freeze

문서만 먼저 고정하고 구현은 후속 라운드로 넘기는 영역:

1. `core::mem`
   - 이후 `take`
   - raw memory copy/set
   - uninit/MaybeUninit 계열
2. `core::hint`
   - `black_box`
   - branch prediction hint
   - `assume` 계열
3. `core::num` 2차
   - checked/wrapping/saturating arithmetic
   - bit ops / endian helpers
   - `total_cmp`, float classification, rounding, `copysign`, `mul_add`
4. `core::text` 2차
   - prefix/suffix helper
   - bytes/iteration
   - indexing/slicing을 동반하는 richer view API
5. `core::range`
   - step/stride, size-aware helper
6. `core::iter`
   - `map/filter/fold/reduce`
   - container/source bridge generalization
7. `core::ops`, `core::marker`, `core::panic`

## Recommended Build Order

1. primitive acts hardening
2. `core::slice`
3. `core::text` 2차 확장
4. `core::iter` adapter 계층
5. 이후 `core::ops`, `core::marker`, `core::panic`

현재 문서는 primitive surface reference와 후속 후보만 유지한다.  
직접 구현 우선순위는 이 문서가 아니라 개별 feature freeze 문서와 active roadmap이 맡는다.

## Notes For Future Rounds

1. `text` 최소 view surface는 이제 언어 코어 확장 위에서 구현되었다
   - prefix deref `*`
   - builtin view fields `.len`, `.data`
   - `text{ data, len }`
2. `text` 길이를 sentinel scan으로 다시 계산하는 표면은 도입하지 않는다
   - `text`는 `{ptr,len}` metadata view로 유지
   - sentinel scan은 `CStr` 계층의 책임으로 남긴다
3. `core::mem::{size_of, align_of}`는 현재 source body를 두지 않고 `Impl::SizeOf`, `Impl::AlignOf` attached binding으로 선언한다
4. `core::hint::spin_loop`는 현재 source body를 두지 않고 `Impl::SpinLoop` attached binding으로 선언한다
5. 같은 `Impl::*` key라도 body가 있으면 ordinary library function으로 간주하고, compiler는 semantics를 책임지지 않는다
6. `core::hint::unreachable`는 checked trap/unchecked UB 의미가 아직 고정되지 않았으므로 bootstrap 단계에서 fake body를 유지한다
7. `ptr/borrow` utility surface는 memory contract 없이 무리해서 더 넓히지 않는다
8. `float` total ordering은 bit-level lowering contract가 먼저 필요하다
9. `core::range`는 이제 value layer + iter bridge를 함께 가진다
   - `loop (x in 1..4)` / `loop (x in 1..:4)` builtin lowering은 계속 유지한다
   - library `Range<T>` iteration은 protocol-first 경로로 열려 있다
10. `core::slice`는 현재 free function-only다
   - sized array owner-container surface는 현재 compiler-owned array family method(`swap`, `replace`, `take`, `put`)로 제공한다
   - source-level `acts for T[]` / `acts for T[N]` family는 const generic이 들어온 뒤 다시 검토한다
11. `core::convert` v1은 contract-first Into-only surface로 고정한다
   - `Into<T>`와 free `into<T, U>(value: U) with [U: Into<T>] -> T`를 제공한다
   - `AsRef` / `AsMut`는 public surface에서 제거한다
   - borrow return conversion은 현재 no-aliasing 방향과 맞지 않으므로 `core::convert` 범위 밖으로 둔다
   - 현재 `~` core semantics는 freeze-ready이며, 이후 확장은 alloc/std layer의 dynamic owner-container surface로만 분리한다
   - installed/external bundle 경계에서도 constrained `convert::into` helper가 직접 동작해야 한다
   - concrete impl inventory는 실제 수요가 생기는 후속 작업에서 추가한다
12. 예외 코어는 non-unwind Recoverable 채널로 고정되었다
   - `throw`, `try...catch`, `try expr`는 ordinary stack unwind를 사용하지 않는다
   - `?` 함수 ABI는 hidden `exc_ctx*`를 사용하고, TLS는 root exception context에만 남긴다
   - panic 기본 정책은 `abort/trap`이다
   - foreign unwind/C++ exception bridge는 hosted/interop 후속 과제다
