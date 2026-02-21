# 12. Builtin Plan Schema Injection

이 문서는 LEI 언어 코어와 호스트(예: Parus 빌드 시스템)의 빌트인 plan 주입 계약을 고정한다.

## 1) 경계 원칙

1. LEI 언어 코어는 `plan` 키워드만 제공한다.
2. `bundle`, `master`, `project` 같은 이름은 LEI 예약어가 아니다.
3. 어떤 이름에 어떤 스키마를 부여할지는 호스트 통합 프로파일이 결정한다.

## 2) Parus 프로파일 기본 주입 대상

Parus 통합 프로파일은 다음 2개만 특수 빌트인 plan으로 주입한다.

1. `bundle`
2. `master`

주의:

1. `project`는 특수 plan이 아니다.
2. LEI 언어 자체에는 `master` 엔트리 규칙이 없다. 이는 Parus 프로파일 규칙이다.

## 3) 작성 패턴 (권장)

bundle 정의:

```lei
export plan json_bundle = bundle & {
  name = "json";
  kind = "lib";
  sources = ["src/json.pr"];
  deps = [];
};
```

루트 master 합성:

```lei
plan merged_master = master & {
  bundles = [json::json_bundle];
};

plan master = merged_master;
```

## 4) 이름 해석 규칙

1. 빌트인 plan은 모듈 초기 심볼 테이블에 읽기 전용 값으로 주입된다.
2. `plan X = ...` 선언의 우변은 좌변 바인딩이 확정되기 전 스코프에서 해석한다.
3. 사용자 정의 심볼이 동일 이름을 먼저 점유하면(예: `let master = ...`) 빌트인 `master`를 가릴 수 있으므로 금지/경고 정책을 두는 것을 권장한다.

## 5) 스키마 적용 규칙

1. 주입된 빌트인 plan은 스키마 제약을 가진 템플릿 plan 값이다.
2. 사용자 patch(`{ ... }`)는 `&`로 합성된다.
3. 스키마 위반/타입 충돌/필수 필드 누락은 경로 포함 진단으로 즉시 실패한다.
4. 스키마 계약은 언어 문법이 아니라 호스트 정책 버전(`*_v1`)으로 관리한다.

## 6) API 모델 권장

호스트 엔진은 C++ API로 빌트인 plan과 스키마를 등록한다.

1. `register_builtin_plan(name, plan_template, schema_descriptor)`
2. `register_plan_schema(name, schema_descriptor)`
3. `freeze_builtin_registry()` 이후 런타임 변경 금지

목표:

1. 언어 코어 오염 없이 스키마 확장 가능
2. 다른 호스트에서도 동일 LEI 엔진 재사용 가능

## 7) LSP/AOT/JIT 일관성

1. LSP, AOT, JIT는 동일한 빌트인 레지스트리 스냅샷을 사용해야 한다.
2. 스냅샷은 버전/해시를 가져야 하며, 불일치 시 진단을 발생시켜야 한다.
3. 자동완성/진단은 주입된 스키마를 기반으로 즉시 반영되어야 한다.

## 8) 보안/결정성

1. 빌트인 주입은 빌드 시작 시 1회만 수행한다.
2. 평가 중 동적 스키마 변경은 금지한다.
3. 동일 입력 + 동일 스키마 스냅샷에서 동일 결과를 보장해야 한다.

## 9) Parus 프로파일 연계

1. 자세한 엔트리/`master` 정책은 `11_PARUS_BUILD_PROFILE.md`를 따른다.
2. 언어/프로파일 경계는 `10_ARCHITECTURE_INDEPENDENCE.md`를 따른다.
