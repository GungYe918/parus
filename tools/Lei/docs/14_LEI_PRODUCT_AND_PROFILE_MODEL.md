# 14. LEI Product And Engine Policy Model

## 목적

이 문서는 LEI를 단일 빌드 시스템 제품으로 정의하고, Core/API/Engine Policy 계층 경계를 고정한다.

## 제품 정의

1. LEI는 CMake/Meson과 같은 범용 빌드 시스템이다.
2. LEI 표면은 단일 제품 경험을 제공한다.
3. 엔트리/예약 이름 정책은 엔진 정책 계층에서 상시 적용한다.

## 3계층 모델

1. LEI Core
   1. 문법 EBNF
   2. 평가 semantics
   3. `&` strict merge
2. LEI Build API
   1. `BuiltinRegistry` (intrinsic)
   2. `BuiltinPlanRegistry` (plan/schema)
   3. graph extraction contract
   4. 빌트인 상수/함수 카탈로그 (`15_BUILTIN_CONSTANTS_AND_FUNCTIONS.md`)
3. LEI Engine Policy
   1. 엔트리 선택
   2. reserved plan 정책
   3. 운영 진단/보안 가드

## 엔진 정책 작성 원칙

1. Core 문법을 수정하지 않는다.
2. Build API로 필요한 빌트인 plan/스키마를 주입한다.
3. 엔트리 정책은 분기 옵션이 아니라 엔진 상시 규칙으로 정의한다.
4. 정책 위반은 `L_*`/`B_*` 진단으로 보고한다.
5. 결정성은 엔진 성질/정책으로 유지하며, 전용 상태 상수로 별도 노출하지 않는다.

## 엔진 기본 정책 선언

1. 기본 엔트리는 `plan master`다.
2. CLI `--plan <name>`으로 override 가능하다.
3. canonical graph source는 `entry_plan` 루트(`project/bundles/tasks/codegens`)다.
4. `export plan master`는 엔진 정책 위반이다.
5. 예약 식별자 `bundle/module/master/task/codegen`은 선언 위치에서 금지된다.
6. 예외로 `plan master ...` 선언만 허용된다.

## 공개 전략

1. LEI 구현체를 분리 공개하지 않는 단계에서도 Core EBNF/semantics는 공개 가능하다.
2. 외부 구현체는 Core 문법 + Build API 계약을 따라 독립적으로 구현할 수 있다.
3. 엔진 정책은 구현체별로 동일하게 재현되어야 한다.
4. 빌트인 상수/함수 공개 계약은 `15_BUILTIN_CONSTANTS_AND_FUNCTIONS.md`를 단일 기준으로 사용한다.
