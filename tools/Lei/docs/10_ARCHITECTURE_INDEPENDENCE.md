# 10. Layered Architecture (SoC)

## 목적

LEI는 단일 빌드 시스템 제품으로 제공된다. 내부적으로는 관심사 분리(SoC)를 위해 Core/API/Engine Policy 계층을 사용한다.

## 계층 모델

1. LEI Core
   1. 문법/의미론/진단/예산
   2. `proto`, `plan`, `export plan`, `import alias`, `&` 규칙
2. LEI Build API
   1. builtin function/plan/schema registry
   2. graph extraction contract
3. LEI Engine Policy
   1. 엔트리 선택 정책(`master`, `--plan`)
   2. reserved plan 정책
   3. 운영 정책 진단(`L_*`/`B_*`)

## 코드 경계

1. LEI 구현 코드는 `tools/Lei/include/lei`, `tools/Lei/src`에 둔다.
2. Core 규칙은 엔진 정책을 분기 옵션으로 노출하지 않는다.
3. 엔진 정책은 런타임에서 상시 적용한다.
4. 빌트인 plan 주입 계약은 `12_BUILTIN_PLAN_SCHEMA_INJECTION.md`에서 다룬다.
5. 제품/엔진 정책 모델은 `14_LEI_PRODUCT_AND_PROFILE_MODEL.md`를 따른다.

## 1회 복제 정책 (현행)

1. Parus frontend 파서 코어에서 필요한 로직은 1회만 복제했다.
2. 스냅샷 기준 커밋: `287a83b`.
3. 복제 후 Parus frontend 변경을 자동 동기화하지 않는다.

## 빌드 정책

1. 루트 빌드에서 `PARUS_BUILD_LEI`로 LEI 포함 여부를 제어한다.
2. LEI는 standalone configure/build를 유지한다.

## 장기 계획

1. LEI를 별도 저장소로 분리한다.
2. 분리 이후에도 Build API 계약은 유지한다.
3. parus 연계는 LEI 엔진 정책 위에서 동작하는 기본 워크플로로 유지한다.
