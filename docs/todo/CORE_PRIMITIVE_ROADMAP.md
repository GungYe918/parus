# Parus Core Primitive Roadmap

문서 상태: `active`

이 문서는 `sysroot/core`의 primitive-facing surface를 단계적으로 채우기 위한 구현 로드맵이다.  
umbrella 방향성은 `docs/todo/CORE_FREESTANDING_BOOTSTRAP_PLAN.md`가 맡고, 이 문서는 실제 구현 우선순위와 phase 구분을 맡는다.

## 원칙

1. `core`는 freestanding여야 한다.
2. `core`는 OS, allocator, heap, thread runtime에 의존하지 않는다.
3. builtin primitive API는 `acts for builtin`을 우선 사용한다.
4. Pure Parus only로 닫히는 surface를 먼저 구현한다.
5. compiler intrinsic이 필요한 surface는 문서로 먼저 고정하고, 구현은 별도 phase로 미룬다.

## Primitive Coverage Matrix

| 영역 | 현재 구현 | 아직 비어 있는 표면 |
| --- | --- | --- |
| `core::cmp` | `Ordering`, `ordering_less/equal/greater` | generic helper, Eq/Ord proto, compare free fn |
| signed int (`i8..i128`, `isize`) | `cmp`, `min`, `max`, `clamp`, `is_zero`, `abs`, `signum`, `is_positive`, `is_negative`, `div_euclid`, `rem_euclid` | `abs_diff`, checked/wrapping/saturating, bit ops, endian helpers, `pow` |
| unsigned int (`u8..u128`, `usize`) | `cmp`, `min`, `max`, `clamp`, `is_zero`, `div_euclid`, `rem_euclid`, `is_power_of_two` | checked/wrapping/saturating, bit ops, endian helpers, `pow` |
| float (`f32`, `f64`) | `partial_cmp`, `min`, `max`, `clamp`, `abs`, `is_nan`, `signum` | `total_cmp`, classification, rounding, `copysign`, `mul_add` |
| `char` | `cmp`, ASCII digit/alpha/alnum/upper/lower/hexdigit/whitespace/control, case conversion, `eq_ignore_ascii_case` | `len_utf8`, `to_digit`, Unicode classification |
| `bool` | `cmp` | logical helper surface는 아직 없음 |
| `text` | 없음 | `is_empty`, `len_bytes`부터 시작 필요 |
| `core::mem` | 없음 | `size_of`, `align_of`, `swap`, `replace`, `take` |
| `core::hint` | 없음 | `unreachable`, `spin_loop` |
| `core::range` | 없음 | range value/helper surface |
| `core::iter` | 없음 | iterator proto/type/adapter 전체 |

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
2. `core::bool::cmp`
3. integer primitive acts의 기본 비교/경계/유클리드 나눗셈
4. float primitive acts의 `partial_cmp`, `min/max/clamp`, `abs`, `is_nan`, `signum`
5. `char` ASCII helper

### Phase 2: intrinsic-backed design freeze

문서만 먼저 고정하고 구현은 후속 라운드로 넘기는 영역:

1. `core::mem`
   - `size_of<T>() -> usize`
   - `align_of<T>() -> usize`
   - 이후 `swap`, `replace`, `take`
2. `core::hint`
   - `unreachable() -> never`
   - `spin_loop() -> void`
3. `core::num` 2차
   - checked/wrapping/saturating arithmetic
   - bit ops / endian helpers
   - `total_cmp`, float classification, rounding, `copysign`, `mul_add`
4. `core::text`
   - 최소 표면: `is_empty`, `len_bytes`
5. `core::range`
6. `core::iter`

## Recommended Build Order

1. primitive acts hardening
2. `core::text`
3. `core::mem`
4. `core::hint`
5. `core::range`
6. `core::iter`
7. 필요 시 `core::slice`
8. 이후 `core::ops`, `core::convert`, `core::marker`

## Notes For Future Rounds

1. `text`는 builtin surface 확인 전까지 구현 보류
2. `ptr/borrow` utility surface는 memory/intrinsic contract 없이 무리해서 열지 않는다
3. `float` total ordering은 bit-level lowering contract가 먼저 필요하다
4. iterator는 `core`에 두되, generic/borrow/loop protocol 안정화 이후에 도입한다
