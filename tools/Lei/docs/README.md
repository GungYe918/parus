# LEI Build System Docs

상태: `draft-0.4`

LEI는 범용 빌드 시스템이다. 문법/평가 엔진은 LEI 시스템의 코어(`LEI Core`)이며, 실제 제품 동작은 Build API와 엔진 정책 계층에서 확장된다.

## 구세대 문법 폐기 선언

이 문서 세트는 이전 세대 표면 문법을 폐기한다.

1. 구형 빌드 엔트리 선언 형식
2. 구형 화살표 본문 함수 형식
3. 구형 기본값 오버레이 연산 형식
4. 구형 intrinsic import 형식
5. 구형 고정 단일 intrinsic 객체 형식

## 문서 인덱스

### Core

1. `01_GOALS_AND_NON_GOALS.md`
2. `02_SYNTAX_EBNF.md`
3. `03_SEMANTICS.md`
4. `04_IMPORT_EXPORT_INTRINSIC.md`
5. `13_PROTO_TEMPLATES.md`

### Build API

1. `06_SECURITY_AND_BUDGET.md`
2. `07_DIAGNOSTICS_AND_TESTING.md`
3. `09_INTRINSICS.md`
4. `12_BUILTIN_PLAN_SCHEMA_INJECTION.md`
5. `15_BUILTIN_CONSTANTS_AND_FUNCTIONS.md`

### Engine Policy

1. `05_BUILD_MODEL.md`
2. `10_ARCHITECTURE_INDEPENDENCE.md`
3. `11_PARUS_BUILD_PROFILE.md`
4. `14_LEI_PRODUCT_AND_PROFILE_MODEL.md`
5. `useage.md`

### Progress

1. `08_ROADMAP.md`
2. `TODO_LEI_V0_4_MIGRATION.md`

## 제품 모델 요약

1. `LEI Core`: parser/evaluator/merge semantics
2. `LEI Build API`: builtin function/plan/schema registry, graph extraction contract
3. `LEI Engine Policy`: 엔트리 정책, 예약 plan, 운영 정책 진단

## 빌트인 카탈로그 정본

1. 빌트인 상수/함수의 단일 정본 문서는 `15_BUILTIN_CONSTANTS_AND_FUNCTIONS.md`다.
2. 개념 문서(`09`)와 정책 문서(`14`)는 목록을 중복 정의하지 않고 `15`를 참조한다.

## 핵심 문법 요약

1. 선언: `proto`, `plan`, `export plan`, `let`, `var`, `def`, `assert`, `import`
2. 함수: 블록 기반 `def`
3. 제어: `for`, `if`, `return`
4. 접근: 객체 `.` / 배열 `[]` / import namespace `alias::symbol`
5. 합성: `&`는 엄격 합성(unification)

## 권장 작성 스타일

```lei
proto myBundleProto {
  name: string;
  kind: string = "lib";
  modules: [object];
  deps: [string] = [];
};

export plan json_bundle = bundle & myBundleProto & {
  name = "json";
  modules = [
    module & {
      head = "json";
      sources = ["json/src/json.pr"];
      imports = [];
    },
  ];
};

export plan lint = task & {
  name = "lint";
  run = ["parusc", "--check", "src/main.pr"];
};
```

## 현재 라운드 범위

1. 문서 계약 정합화
2. 제품 정체성 전환(빌드 시스템 중심)
3. 엔트리 plan 루트 그래프 계약 통일(`project/bundles/tasks/codegens`)
