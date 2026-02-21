# 09. Intrinsics

## 개요

LEI intrinsic은 특정 고정 키워드를 의미하지 않는다. intrinsic은 호스트가 등록하는 공용 빌트인 함수/값 레지스트리다.

중요:

1. intrinsic 레지스트리와 built-in plan 스키마 레지스트리는 별개다.
2. `bundle`, `master`, `task`, `codegen`은 intrinsic 함수가 아니라 plan 템플릿 주입 계약에 속한다.
3. `proto`는 intrinsic이 아니라 LEI 언어 문법(사용자 템플릿 선언)이다.
4. built-in plan 규칙은 `12_BUILTIN_PLAN_SCHEMA_INJECTION.md`를 따른다.

## 기본 원칙

1. intrinsic은 LEI 문법이 아니라 실행 환경 계약이다.
2. intrinsic은 import 없이 전역에서 사용할 수 있다.
3. intrinsic 이름은 언어 예약어와 충돌하면 안 된다.

## 금지 사항

1. 구형 intrinsic 전용 import 형식 금지
2. 단일 고정 내장 객체를 언어 수준 계약으로 강제하는 방식 금지
3. 비결정적 intrinsic(시간/랜덤/환경 의존) 기본 제공 금지

## 사용 예시 (가정)

```lei
let sources = glob(["src/**/*.pr"]);
let d = dep("json", "^1.0");
let p = profile("release", { opt: 3, debug: false });
```

## API 계약

1. `BuiltinRegistry`는 intrinsic 등록 지점이다.
2. `register_value(name, factory)`로 값 intrinsic을 등록한다.
3. `register_native_function(name, callback)`로 함수 intrinsic을 등록한다.
4. intrinsic은 순수/결정성 계약을 만족해야 한다.

## 레지스트리 분리 권장

1. intrinsic: `BuiltinRegistry`(함수/값)
2. plan 템플릿/스키마: `BuiltinPlanRegistry`(plan_template/schema)
3. proto 선언: 파서/타입 시스템의 언어 레벨 선언 테이블
4. 세 레이어는 초기화 단계에서만 변경 가능하도록 freeze 정책을 둔다.

## 확장 규칙

1. intrinsic 추가 시 문서 업데이트가 필수다.
2. 보안/결정성 검증을 통과해야 한다.
3. Parus 통합 정책과 언어 코어 정책은 분리해 기술한다.
