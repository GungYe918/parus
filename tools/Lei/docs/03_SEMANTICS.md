# 03. Semantics

## 평가 모델

1. 모듈은 선언과 실행 컨텍스트를 가진다.
2. import는 DAG 순서로 해석된다.
3. 같은 입력/옵션이면 동일한 출력을 생성해야 한다.
4. LEI Core는 엔트리 이름(`master` 등)을 하드코딩하지 않는다.

## 값 모델

1. 스칼라: `int`, `float`, `string`, `bool`
2. 컨테이너: object, array
3. 함수 값: `def`로 선언된 블록 함수
4. proto 값: `proto` 선언으로 생성된 템플릿/스키마 값
5. plan 값: `plan` 선언으로 생성된 명명된 플랜 값

## 가변성

1. `let`은 재대입 불가다.
2. `var`는 재대입 가능하다.
3. `=`은 명시 대입이다.

## 접근 의미

1. `obj.field`는 객체 필드 접근이다.
2. `arr[idx]`는 배열 인덱스 접근이다.
3. `alias::symbol`은 import namespace 접근이다.

## `&` (엄격 합성) 규칙

1. `A & B`는 새 값을 만든다. 원본은 변경되지 않는다.
2. object-object는 재귀 합성한다.
3. 같은 키의 scalar는 타입과 값이 동일할 때만 통과한다.
4. array-array는 길이가 같아야 하며, 각 인덱스를 재귀 합성한다.
5. 타입 불일치 또는 값 충돌은 실패다.
6. 충돌 진단은 경로를 포함해야 한다.
   예: `project.name`, `bundles[2].deps[0]`
7. `&`는 덮어쓰기가 아니다.
8. scalar-scalar 합성(예: `foo.name & bar.name`)은 동일값 제약이며, 값 변경 용도가 아니다.
9. 단일 필드 변경은 객체 patch로 수행한다.
   예: `project = project & { name = "new-name"; };`

`&`는 LEI 시스템 전체의 결정성 보장을 위한 핵심 연산이다.

## `proto` 합성 규칙

1. `proto`는 plan에 합성 가능한 제약/기본값 템플릿이다.
2. `export plan x = MyProto & { ... };`에서 `MyProto`의 필드 제약을 먼저 적용한다.
3. `proto` 필드에 기본값이 있으면 patch에 필드가 없을 때 기본값이 채워진다.
4. patch에 필드가 있으면 `proto` 타입 제약을 만족해야 한다.
5. 필수 필드(기본값 없는 필드)가 최종 결과에 없으면 실패한다.
6. `proto`와 빌트인 plan(`bundle`, `module`, `task` 등)은 동일한 `&` 연산으로 연쇄 합성 가능하다.
7. 프로젝트 메타 필드(`project`)는 proto를 통해 타입/필수 필드를 고정하는 방식을 권장한다.

## Built-in plan과 스키마 의미 부여

1. LEI Core 자체는 `bundle`, `module`, `master`, `task`, `codegen`의 의미를 내장하지 않는다.
2. LEI Build API는 빌트인 plan 템플릿/스키마 주입 지점을 제공한다.
3. Host Profile(예: parus)은 Build API를 사용해 특수 plan 의미를 부여한다.
4. 어떤 plan이 특수한지는 Core가 아니라 Profile이 결정한다.

## 함수 의미

1. 함수는 블록 본문을 가진다.
2. `return`으로 값을 반환한다.
3. 재귀 호출은 금지한다.
4. 중첩 함수는 금지한다.

## 제어 의미

1. `if`는 조건 블록 분기다.
2. `for`는 예산 제한 하에서만 실행된다.
3. 예산 초과는 즉시 실패한다.

## 오류 규칙

1. 미정의 식별자, 타입 충돌, 합성 충돌은 즉시 실패한다.
2. 실패는 silent fallback 없이 결정적 진단으로 보고한다.
