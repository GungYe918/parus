# Parus Const Model (Design Freeze)

문서 버전: `draft-0.1`  
상태: `Design Freeze (TODO Track)`  
목적: `const` 모델을 JIT/AOT 공통 의미론으로 구현하기 위한 결정 완결 기준을 고정한다.

## 1. 목적과 범위

이 문서는 Parus `const` 모델의 표면 문법, 의미론, 진단, 하향 계약을 고정한다.

1. `const`를 "컴파일타임에 값이 확정되는 불변 모델"로 정의한다.
2. `const`와 `comptime`의 역할을 분리한다.
3. JIT/AOT 양쪽에서 동일하게 성립하는 상수 모델을 고정한다.
4. 구현자가 추가 결정을 하지 않아도 되도록 테스트/수용 기준을 명시한다.

비범위:

1. 타입 한정 `const`-correctness (`&const T`, `^&const T`)의 정식 도입
2. `const` generic
3. `const { ... }` 블록 문법 도입

## 2. 비가역 결정 사항 (Non-negotiable)

1. `const` 적용 위치(v1 정식 범위):
1. 전역 상수: `const NAME: T = EXPR;`
1. 지역 상수: `const name: T = EXPR;`
1. 상수 함수: `const fn f(...) -> T { ... }`
1. 정적 상수: `static const NAME: T = EXPR;`
1. `const` 비적용 위치(v1 금지):
1. 타입 한정 `const`(`&const T`, `^&const T`)
1. `const { ... }` 블록
1. `let const`, `mut const` 조합
1. `const`와 `comptime`는 상호 대체하지 않는다.
1. `const`는 값의 성질(확정/불변), `comptime`는 실행 시점 컨텍스트를 의미한다.
1. `static const`가 canonical 표기다. `const static`은 v1에서 비지원으로 고정한다.
1. 타입 한정 `const`는 본문 부록의 draft-only 제안으로만 기록하며 활성 의미론으로 취급하지 않는다.

## 3. 현재 구현 기준 스냅샷 (출발선)

현재 코드베이스 사실 기준:

1. `const` 키워드는 lexer/parser에서 사용자 문법으로 활성화되어 있지 않다.
1. `@pure`, `@comptime`는 `@` 뒤 식별자 속성으로 취급하며, 키워드형 qualifier는 고정하지 않았다.
1. `require(expr)`는 컴파일타임 bool folding을 수행하지만 v0에서 허용 식 형태가 제한되어 있다.
1. 현재 `require(expr)` 허용 최소 형태는 `true/false/not/and/or/==/!=` 중심의 단순 식이다.
1. AST/Tyck/SIR/OIR에 `const item/local/fn/static const`를 위한 정식 전용 노드는 없다.

이 문서는 위 출발선을 기준으로 v1 도입 규칙을 고정한다.

## 4. 표면 문법 고정 (v1)

```ebnf
ConstItemDecl   := "const" Ident ":" Type "=" ConstExpr ";" ;
ConstLocalDecl  := "const" Ident ":" Type "=" ConstExpr ";" ;
ConstFnDecl     := "const" "def" Ident GenericParamClauseOpt FuncParams ConstraintClauseOpt "->" Type Block ;
StaticConstDecl := "static" "const" Ident ":" Type "=" ConstExpr ";" ;

ConstExpr       := Literal
                | Path
                | UnaryConstExpr
                | BinaryConstExpr
                | "(" ConstExpr ")"
                | ConstCallExpr
                | CastConstExpr
                ;
```

문법 고정 규칙:

1. `const` 선언은 항상 초기화식을 가져야 한다.
1. `const fn`은 `const def` 형태로만 선언한다.
1. `static const` 순서만 허용한다.
1. `const static`, `const { ... }`, `let const`, `mut const`는 파싱 단계에서 거부한다.

## 5. 의미론 고정

## 5.1 핵심 정의

1. `const` 값은 컴파일 시점에 완전히 결정 가능해야 한다.
1. `const` 값은 불변이며 런타임 쓰기 경로를 갖지 않는다.
1. `const` 평가 실패는 컴파일 오류다. 런타임 fallback은 없다.

## 5.2 const-evaluable 식 최소 집합 (v1)

v1에서 `ConstExpr`로 허용하는 최소 집합:

1. 리터럴: bool, 정수, 부동소수, char, text, null(타입 문맥 허용 시)
1. 단항: `+`, `-`, `not`, `!` (타입 규칙 충족 시)
1. 이항:
1. 산술: `+ - * / %`
1. 비교: `== != < <= > >=`
1. 논리: `and or`
1. 다른 `const item` 참조
1. `const fn` 호출(모든 인자가 const-evaluable일 때)
1. 명시 캐스트(타입체커가 허용하는 안전/정의된 변환만)

비허용:

1. I/O, 시간, 랜덤, 외부 상태 읽기
1. `static mut` 읽기
1. 힙 할당/해제
1. 비결정적 동작

## 5.3 초기화/순환 규칙

1. 모든 `const` 선언은 초기화 필수다.
1. 선언 단위 내부 및 모듈 간 순환 의존은 금지한다.
1. 순환 탐지는 심볼 그래프 SCC 기준으로 수행한다.
1. 순환이 발견되면 첫 진단에서 사이클 경로를 함께 보고한다.

## 5.4 결정성 규칙

1. 같은 소스 + 같은 컴파일 옵션이면 `const` 결과는 백엔드/JIT 여부와 무관하게 동일해야 한다.
1. overflow/NaN 처리 정책은 타깃 독립으로 고정한다.
1. 타깃 독립 규칙을 만족하지 못하는 연산은 `const` 문맥에서 거부한다.

## 5.5 `const fn` 규칙

1. `const fn` 본문은 const-evaluable 연산만 사용해야 한다.
1. `const fn` 내부에서 비`const fn` 호출은 금지한다.
1. `const fn`의 부수효과(전역 쓰기, I/O, throw)는 금지한다.
1. 재귀는 허용하되, 컴파일러 예산(깊이/스텝) 초과 시 컴파일 오류로 처리한다.

## 5.6 `static const` 규칙

1. `static const`는 read-only 정적 저장소 심볼이다.
1. 값으로만 사용되면 상수 폴딩 가능하다.
1. 주소를 취하면 심볼을 유지한다.
1. JIT/AOT 모두에서 주소 정체성 규칙이 동일해야 한다.

## 5.7 `const`와 `comptime` 관계

1. `const`는 값 특성, `comptime`은 실행 시점 컨텍스트다.
1. `comptime` 블록/함수는 내부에서 `const` 값을 사용할 수 있다.
1. `const` 선언만으로 임의 컴파일타임 제어 흐름을 대체하지 않는다.
1. `require/cfg/comptime` 계열의 판정식은 동일 const evaluator를 공유한다.

## 6. JIT/AOT 공통 계약

## 6.1 프론트엔드 계약

1. const 평가는 Tyck 단계에서 완료되어야 한다.
1. 미해결 const 값이 IR로 내려가면 컴파일 실패다.
1. evaluator는 대상 백엔드에 의존하지 않는 순수 계산기로 동작한다.

## 6.2 SIR/OIR 계약

1. `const item`은 SIR/OIR에서 const-data로 표현 가능해야 한다.
1. `static const`는 "주소 가능 상수 심볼" 메타를 유지한다.
1. 값 대체 가능한 사용 지점에서는 literal/const-data folding을 허용한다.

## 6.3 AOT/JIT 계약

1. AOT: `.rodata` 또는 동등한 read-only 세그먼트에 배치한다.
1. JIT: constant pool 또는 동등한 read-only 슬롯에 배치한다.
1. 주소 관찰 가능 경로에서 AOT/JIT의 동작 의미는 동등해야 한다.

## 7. 진단 규칙 고정 (코드 후보)

필수 진단 범주:

1. `kConstInitializerRequired`: const 선언에 초기화식 없음
1. `kConstExprNotEvaluable`: const 식에 비허용 연산 포함
1. `kConstCycleDetected`: const 의존 순환
1. `kConstFnCallsNonConstFn`: const fn이 non-const fn 호출
1. `kConstFnHasSideEffect`: const fn에 부수효과 포함
1. `kStaticConstOrderInvalid`: `const static` 등 비허용 순서
1. `kConstTypeQualifierNotEnabled`: `&const T`, `^&const T` 사용
1. `kConstBlockNotSupported`: `const { ... }` 사용

진단 정책:

1. 실패는 경고가 아니라 컴파일 오류로 처리한다.
1. 원인 지점과 사용 지점을 함께 표시한다.
1. const cycle은 순환 경로(최소 2노드 이상)를 함께 출력한다.

## 8. 예제

## 8.1 성공 예제 (v1 기대)

```parus
const PAGE_SIZE: i32 = 4096i32;
```

```parus
const K: i32 = 2i32 * 3i32 + 1i32;
```

```parus
const A: i32 = 10i32;
const B: i32 = A + 5i32;
```

```parus
const def add1(x: i32) -> i32 {
  return x + 1i32;
}
const N: i32 = add1(41i32);
```

```parus
def f() -> i32 {
  const t: i32 = 7i32 * 6i32;
  return t;
}
```

```parus
static const ABI_VERSION: i32 = 1i32;
```

## 8.2 실패 예제 (v1 기대)

```parus
const X: i32;
// error: kConstInitializerRequired
```

```parus
const static X: i32 = 1i32;
// error: kStaticConstOrderInvalid
```

```parus
const def bad_io() -> i32 {
  extern_print("x");
  return 1i32;
}
// error: kConstFnHasSideEffect
```

```parus
def g(x: i32) -> i32 { return x; }
const Y: i32 = g(1i32);
// error: kConstFnCallsNonConstFn 또는 kConstExprNotEvaluable
```

```parus
const A: i32 = B + 1i32;
const B: i32 = A + 1i32;
// error: kConstCycleDetected
```

```parus
def h() -> void {
  const {
    require(true);
  }
}
// error: kConstBlockNotSupported
```

```parus
def q(p: &const i32) -> i32 { return 0i32; }
// error: kConstTypeQualifierNotEnabled
```

## 9. 테스트/수용 기준

## 9.1 문서 자체 수용

1. 본 문서만으로 parser/tyck/SIR/OIR/backend 구현 범위를 결정할 수 있어야 한다.
1. 구현자가 추가 정책 결정을 요구하지 않아야 한다.

## 9.2 예제 기반 컴파일 시나리오

1. `const` 전역/지역 선언 성공
1. `static const` 선언 성공
1. 비결정 연산 포함 const 초기화식 실패
1. 순환 참조 실패
1. `const {}` 사용 실패
1. `&const T`, `^&const T` 사용 실패(draft-only)

## 9.3 JIT/AOT 의미 일치 시나리오

1. 동일 소스에서 상수 값 관찰 결과가 JIT/AOT에서 동일
1. 주소 관찰 가능한 `static const`의 정체성 규칙이 JIT/AOT에서 동일

## 10. 도입 로드맵

1. v1: `const item/local/fn/static const` 파싱 + 타입체크 + evaluator 최소 집합
1. v1: SIR/OIR const-data 표현 + AOT/JIT 배치 계약 연결
1. v1.1+: evaluator 확장(연산자/캐스트/진단 품질)
1. v2+: 타입 한정 `const` (`&const T`, `^&const T`) 검토

## 11. 부록 A: 타입 한정 `const` 초안 (Draft-only, 비정본)

이 부록은 논의 초안이며, 구현 활성 규칙이 아니다.

문법 후보:

```ebnf
TypeConstRef  := "&" "const" Type ;
TypeConstEsc  := "^&" "const" Type ;
```

보류 사유:

1. borrow/escape 모델(`&`, `&mut`, `^&`)과 const-correctness 결합 규칙이 아직 고정되지 않았다.
1. `self`, acts, class/proto 메서드 해석과 타입 한정 const의 상호작용 정의가 필요하다.
1. API 표면에서 mutability/aliasing 진단 체계를 먼저 정비해야 한다.

현재 고정:

1. `&const T`, `^&const T`는 v1에서 파싱/의미론 모두 비활성이다.
1. 사용 시 "draft-only/not enabled" 진단을 발생시킨다.

## 12. 명시적 가정과 기본값

1. 본 문서는 TODO 설계 고정이며, 정본(spec) 승격은 구현 완료 후 진행한다.
1. 문서 언어는 기존 TODO 문서와 동일하게 한국어 중심으로 유지한다.
1. `const`/`comptime` 역할 분리는 변경하지 않는다.
1. 타입 한정 const는 초안 부록으로만 유지한다.
1. canonical 표기는 `static const`로 고정한다.
