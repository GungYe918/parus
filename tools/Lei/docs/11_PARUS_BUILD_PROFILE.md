# 11. Parus Build Profile (Framework Rules)

이 문서는 LEI 언어 규칙이 아니라 Parus 빌드 시스템 통합 규칙을 고정한다.

빌트인 plan 주입 계약 자체는 `12_BUILTIN_PLAN_SCHEMA_INJECTION.md`를 따른다.

## 목적

1. LEI는 순수한 플랜 합성 DSL로 유지한다.
2. Parus 빌드 시스템이 요구하는 엔트리/마스터 해석 규칙을 별도 프로파일로 관리한다.

## 엔트리 파일 규칙

1. 프로젝트 루트의 `config.lei`를 엔트리 파일로 사용한다.
2. `config.lei`는 프로젝트 메타데이터(이름/버전/프로필)와 최종 마스터 플랜을 정의한다.

## bundle config 재사용 규칙

1. 각 bundle은 자신의 `config.lei` 또는 `<folder>.lei`에서 고유 이름 plan(예: `json_bundle`)을 export한다.
2. 각 bundle plan은 Parus가 주입한 빌트인 `bundle` plan과 `&` 합성해 생성한다.
3. 상위 프로젝트는 하위 bundle의 export plan들을 import해 명시적으로 `&` 합성한다.
4. 합성 결과는 현재 프로젝트의 canonical plan이며, 빌드 매니저는 이 값을 기준으로 그래프를 생성한다.

## master 선택 규칙 (Parus)

1. `config.lei` 안의 `plan master`를 기본 엔트리 plan으로 선택한다.
2. CLI `--plan <name>`이 제공되면 해당 plan을 엔트리로 사용한다.
3. CLI 지정 plan이 없고 `master`가 없으면 에러로 종료한다.
4. `plan master`는 일반적으로 빌트인 `master` plan과 patch를 `&` 합성해 생성한다.

## master export 정책 (Parus)

1. `master`는 export 금지다.
2. `export plan master`가 발견되면 정책 위반 에러(`P_*`)를 발생시킨다.
3. 하위 bundle 파일에서 export된 `master`를 import/재export하는 것도 정책 위반이다.

## import 경로 정책

1. 통합 예시에서는 상대 경로(`./`, `../`)만 사용한다.
2. 엔트리 파일/하위 파일 모두 동일하게 상대 경로를 권장한다.

## 결정성/재현성 조건

1. 동일한 `config.lei` + 동일한 CLI 옵션은 동일한 엔트리 plan을 선택해야 한다.
2. `&` 충돌은 빌드 전에 경로 포함 진단으로 즉시 실패한다.
3. 암묵 fallback(다른 plan 자동 선택)은 금지한다.

## 예시 스니펫

```lei
// json/json.lei
export plan json_bundle = bundle & {
  name = "json";
  kind = "lib";
  sources = ["src/json.pr"];
  deps = [];
};

// config.lei
import json from "./json/json.lei";

plan merged_master = master & {
  bundles = [json::json_bundle];
};

plan master = merged_master;
```
