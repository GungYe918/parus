# 09. Intrinsics

## 개요

LEI intrinsic은 특정 고정 키워드를 의미하지 않는다. intrinsic은 LEI Build API가 제공하는 공용 빌트인 함수/값 레지스트리다.

중요:

1. intrinsic 레지스트리와 built-in plan 스키마 레지스트리는 별개다.
2. `bundle`, `master`, `task`, `codegen`은 intrinsic 함수가 아니라 plan 템플릿 주입 계약에 속한다.
3. `proto`는 intrinsic이 아니라 LEI Core 문법(사용자 템플릿 선언)이다.
4. built-in plan 규칙은 `12_BUILTIN_PLAN_SCHEMA_INJECTION.md`를 따른다.
5. 실제 빌트인 상수/함수 목록 정본은 `15_BUILTIN_CONSTANTS_AND_FUNCTIONS.md`를 따른다.

## 기본 원칙

1. intrinsic은 LEI Core 문법이 아니라 Build API 실행 계약이다.
2. intrinsic은 import 없이 전역에서 사용할 수 있다.
3. intrinsic 이름은 예약 식별자/엔진 정책 이름과 충돌하면 안 된다.

## 금지 사항

1. 구형 intrinsic 전용 import 형식 금지
2. 단일 고정 내장 객체를 Core 문법으로 강제하는 방식 금지
3. 비결정적 intrinsic(시간/랜덤/환경 의존) 기본 제공 금지
4. 내부 포맷 구조/ABI 레이아웃을 상수로 직접 노출하는 방식 금지

## 사용 예시 (가정)

```lei
let sources = glob(["src/**/*.pr"]);
let ok = semver.satisfies("1.2.0", "^1.0.0");
let n = path.normalize("./src/../src/main.pr");
```

## API 계약

1. `BuiltinRegistry`는 intrinsic 등록 지점이다.
2. `register_value(name, factory)`로 값 intrinsic을 등록한다.
3. `register_native_function(name, callback)`로 함수 intrinsic을 등록한다.
4. intrinsic은 순수/결정성 계약을 만족해야 한다.

## 레지스트리 분리 권장

1. intrinsic: `BuiltinRegistry`(함수/값)
2. plan 템플릿/스키마: `BuiltinPlanRegistry`(plan_template/schema)
3. proto 선언: parser/type 시스템의 Core 선언 테이블
4. 세 레이어는 초기화 단계에서만 변경 가능하도록 freeze 정책을 둔다.

## 카탈로그 관리

1. 카탈로그의 canonical 위치는 `15_BUILTIN_CONSTANTS_AND_FUNCTIONS.md`다.
2. 이 문서는 개념/등록 API만 다루고 목록을 중복 정의하지 않는다.
3. 내부 포맷 출력 정책(`parlib/.a/.so/.dylib`)은 상수 노출이 아니라 플랜 스키마 필드로 다룬다.
