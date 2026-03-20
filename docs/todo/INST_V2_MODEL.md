# Parus `inst` V2/V3 Model

문서 상태: `todo`

이 문서는 `inst`를 현재의 좁은 bool predicate에서, 장기적으로 Parus의 compile-time directive/metaprogramming/codegen 축으로 확장하기 위한 청사진을 고정한다.

현재 immediate implementation은 `Impl::*`를 우선 사용한다.  
그 이유는 `size_of`, `align_of`, `spin_loop` 같은 surface가 지금 당장 필요한데, 이들은 현재 `inst`나 macro로는 표현할 수 없는 compiler/layout contract를 필요로 하기 때문이다.

관련 문서:

1. `/Users/gungye/workspace/Lang/gaupel/docs/todo/CORE_PRIMITIVE_ROADMAP.md`
2. `/Users/gungye/workspace/Lang/gaupel/docs/todo/CORE_FREESTANDING_BOOTSTRAP_PLAN.md`

## 1. 역할 분리

Parus는 장기적으로 compile-time 관련 표면을 네 축으로 분리한다.

1. `Impl::*`
   - compiler가 의미를 책임지는 core/std surface binding
   - 라이브러리는 이름/위치/공개 표면을 소유한다
   - 예: `Impl::SpinLoop`, `Impl::SizeOf`, `Impl::AlignOf`
2. `inst`
   - compile-time Parus evaluator/query/codegen 축
   - proto와 무관한 directive/metaprogramming 레인
3. `macro`
   - token/AST 재작성
   - runtime/ABI/layout 의미를 직접 소유하지 않는다
4. `proto`
   - 제약/공급 모델
   - compile-time evaluator나 code generation 채널이 아니다

이 네 축은 의도적으로 역할이 겹치지 않아야 한다.

## 2. 왜 지금은 `Impl::*`를 먼저 쓰는가

다음 표면은 현재 `inst`나 macro로 구현할 수 없다.

1. `core::mem::size_of<T>()`
2. `core::mem::align_of<T>()`
3. `core::hint::spin_loop()`

이유:

1. `inst` v1은 bool predicate 전용이고, 값 모델이 매우 좁다.
2. `macro`는 token 재작성이라 concrete ABI layout이나 lowered type metadata를 볼 수 없다.
3. imported C layout, target ABI, enum/class lowered shape, target-specific spin hint는 compiler contract가 필요하다.

따라서 현재는 다음 모델이 맞다.

1. core/std가 공식 surface를 선언한다
2. compiler가 `Impl::*` key를 보고 의미를 구현한다
3. fake body는 두지 않는다

## 3. `inst` 단계 구분

### v1: 현재 단계

현재 `inst`는 `$[]` predicate를 위한 좁은 bool evaluator다.

지원 범위:

1. bool/int/string literal
2. ident
3. `not`, `and`, `or`
4. `==`, `!=`
5. 다른 `inst` 호출
6. `let`, `if/else`, `return`

비지원:

1. typed return
2. type/layout query
3. item emission
4. symbol reflection
5. compiler-owned directive orchestration

### v2: typed compile-time function

`inst v2`의 목표는 “컴파일타임 Parus 함수”다.

핵심 목표:

1. typed return 도입
2. 제한된 compile-time 값 모델 도입
3. 제한된 compiler query surface 도입
4. `Impl::*` 일부를 장기적으로 대체할 수 있는 기반 마련

### v3: item/codegen axis

`inst v3`의 목표는 item emission과 directive orchestration까지 포함한 compile-time metaprogramming 축이다.

핵심 목표:

1. item/code block generation
2. conditional emission
3. compiler directive orchestration
4. 필요한 범위의 structured code generation

## 4. `inst v2` 값 모델

`inst v2`는 unrestricted runtime evaluation이 아니라, 제한된 compile-time 값만 다룬다.

후보 값 모델:

1. `bool`
2. 정수 scalar (`i*`, `u*`, `isize`, `usize`)
3. `char`
4. `text`
5. small enum
6. `T?`
7. type-descriptor
8. tuple-like small product value

제외:

1. heap object
2. actor/class runtime instance
3. borrow/raw pointer runtime 값
4. OS handle
5. arbitrary side-effectful state

## 5. `inst v2` compiler query surface

장기적으로 필요한 최소 query surface는 다음과 같다.

1. `compiler.layout_of(T)`
   - `size`
   - `align`
   - field layout summary
2. `compiler.target`
   - abi family
   - pointer width
   - endianness
3. `compiler.symbol`
   - 경로 존재 여부
   - kind/type summary
4. `compiler.module`
   - current bundle/module/file metadata

이 query surface는 unrestricted reflection이 아니라, 명시적 capability를 가진 제한된 API여야 한다.

## 6. `Impl::*`와의 관계

장기적으로는 일부 `Impl::*` surface를 `inst` 기반 표면으로 옮길 수 있다.

예:

```parus
inst align_of(T: type) -> usize {
    return compiler.layout_of(T).align;
}
```

하지만 이건 `inst`가 이미 다음을 지원할 때만 가능하다.

1. `type` 값 모델
2. typed `usize` return
3. compiler query
4. deterministic const-eval

그 전까지는 `Impl::*`가 올바른 자리다.

## 7. Non-goals

`inst`에 넣지 않을 것:

1. ordinary runtime library surface 대체
2. unsafe backend hook 남발
3. 임의의 heap/OS/runtime side effect
4. ABI primitive를 fake compile-time body로 우회 구현하는 것

즉 `inst`는 “무엇이든 가능한 escape hatch”가 아니라, language-level compile-time programming lane이어야 한다.

## 8. Recommended Order

추천 순서는 다음과 같다.

1. `Impl::SpinLoop`, `Impl::SizeOf`, `Impl::AlignOf` 정착
2. core fake body 제거
3. `unreachable` 의미 분리 결정
4. `inst v2` 타입/값 모델 설계
5. `inst v2` compiler query surface 설계
6. 이후 item emission과 code generation을 `v3`로 확장

## 9. Open Questions

다음은 별도 라운드에서 확정한다.

1. `inst`가 generic parameter와 value parameter를 어떻게 받는가
2. `type` 값을 문법적으로 어떻게 표현하는가
3. `inst` 내부에서 path/symbol lookup을 어느 수준까지 허용하는가
4. `inst`와 macro의 조합 순서를 어떻게 고정하는가
5. `inst` 결과를 item emission으로 연결할 때 hygiene와 source mapping을 어떻게 보장하는가
