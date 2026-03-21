# Parus Core Primitive Roadmap

문서 상태: `active`

이 문서는 `sysroot/core`의 primitive-facing surface를 단계적으로 채우기 위한 구현 로드맵이다.  
umbrella 방향성은 `docs/todo/CORE_FREESTANDING_BOOTSTRAP_PLAN.md`가 맡고, 이 문서는 실제 구현 우선순위와 phase 구분을 맡는다.  
`Impl::*`와 `inst`의 역할 분리는 `docs/todo/INST_V2_MODEL.md`를 따른다.

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
| `core::cmp` | `Ordering`, `ordering_less/equal/greater` | generic helper, Eq/Ord proto, compare free fn |
| signed int (`i8..i128`, `isize`) | `cmp`, `min`, `max`, `clamp`, `is_zero`, `abs`, `signum`, `is_positive`, `is_negative`, `div_euclid`, `rem_euclid` | `abs_diff`, checked/wrapping/saturating, bit ops, endian helpers, `pow` |
| unsigned int (`u8..u128`, `usize`) | `cmp`, `min`, `max`, `clamp`, `is_zero`, `div_euclid`, `rem_euclid`, `is_power_of_two` | checked/wrapping/saturating, bit ops, endian helpers, `pow` |
| float (`f32`, `f64`) | `partial_cmp`, `min`, `max`, `clamp`, `abs`, `is_nan`, `signum` | `total_cmp`, classification, rounding, `copysign`, `mul_add` |
| `char` | `cmp`, ASCII digit/alpha/alnum/upper/lower/hexdigit/whitespace/control, case conversion, `eq_ignore_ascii_case`, `to_digit` | `len_utf8`, Unicode classification |
| `bool` | `cmp`, `is_true`, `is_false` | richer logical helper surface |
| `text` | `is_empty`, `len_bytes`, `as_ptr` | prefix/suffix, ASCII-insensitive compare, bytes/iteration, indexing/slicing |
| `core::mem` | `size_of`, `align_of`, `swap`, `replace` | `take`, raw memory API, uninit/MaybeUninit |
| `core::hint` | `unreachable`, `spin_loop` | `black_box`, branch hint, `assume` |
| `core::range` | `Range<T>`, `RangeInclusive<T>`, integer constructor, `is_empty`, `contains` | `len`, step/stride, iterator bridge, syntax sugar |
| `core::iter` | 없음 | iterator proto/type/adapter 전체 |

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

다만 지금 바로 도입하지는 않는다.

선행 조건:

1. `core::range`
2. `core::text` 최소 view surface
3. 필요한 경우 `core::slice`
4. loop protocol과 borrow/generic 규칙 안정화

## Phase Split

### Phase 1: Pure Parus only

이미 구현되었거나, 지금 기준으로 compiler 변경 없이 닫을 수 있는 영역:

1. `core::cmp::Ordering`
2. `core::bool::{cmp,is_true,is_false}`
3. integer primitive acts의 기본 비교/경계/유클리드 나눗셈
4. float primitive acts의 `partial_cmp`, `min/max/clamp`, `abs`, `is_nan`, `signum`
5. `char` ASCII helper와 `to_digit`
6. `text` 최소 view acts (`is_empty`, `len_bytes`, `as_ptr`)
7. `core::ext::CStr -> text` bridge
8. `core::mem::{size_of, align_of, swap, replace}`
9. `core::hint::{unreachable, spin_loop}`

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
   - library-only value layer
   - `Range<T>` / `RangeInclusive<T>`
   - integer constructor + `is_empty` / `contains`
6. `core::iter`

## Recommended Build Order

1. primitive acts hardening
2. `core::text` 2차 확장
3. `core::range`를 loop/iter 바닥으로 확장
4. `core::iter`
5. 필요 시 `core::slice`
6. 이후 `core::ops`, `core::convert`, `core::marker`

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
9. `core::range` v1은 library-only value layer다
   - `loop (x in 1..4)` / `loop (x in 1..:4)` builtin lowering은 그대로 유지한다
   - `loop (x in range(...))`는 아직 열지 않는다
10. iterator는 `core`에 두되, generic/borrow/loop protocol 안정화 이후에 도입한다
