# 03. Semantics

## 평가 모델

1. LEI 프로그램은 단일 `export build` 값을 산출한다.
2. 모든 값은 불변이다.
3. 평가 순서는 선언 순서 + import DAG 순서를 따른다.
4. `const`는 순수식으로 평가 가능해야 한다.

## 값 모델

1. 스칼라: `int`, `float`, `string`, `bool`
2. 컨테이너: object, array
3. 함수 값: `def` 선언으로 생성되는 닫힌 표현식(closure)

## 연산 의미

1. `&` (구조 병합)
   1. object-object 병합만 허용
   2. 같은 key가 모두 object면 재귀 병합
   3. 같은 key가 서로 다른 스칼라면 충돌 오류
2. `?=` (기본값 오버레이)
   1. 좌변이 object면 없는 key만 우변에서 채운다.
   2. 비-object에서는 좌변이 “비어있음/미정” 개념이 없으므로 오류.

## 제어식

1. `if then else`는 식이다.
2. `match`는 첫 매칭 arm을 선택한다.
3. `_` arm은 fallback이다.

## 함수(def)

1. 본문은 expression-only (`=> Expr`)이다.
2. 재귀 호출은 런타임에서 금지한다.
3. 호출 스택에서 동일 함수 재진입 시 에러.

## 오류 규칙

1. 타입 불일치, 미정의 식별자, 병합 충돌은 즉시 실패.
2. 실패는 silent fallback 없이 결정적 진단을 생성한다.

