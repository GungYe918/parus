# LEI DSL Docs

상태: `draft-0.3`

LEI는 빌드 플랜 합성을 위한 독립 DSL이다. 이 디렉터리는 LEI 언어 명세와 Parus 통합 프로파일을 분리해 기록한다.

## 구세대 문법 폐기 선언

이 문서 세트는 이전 세대 표면 문법을 폐기한다.

1. 구형 빌드 엔트리 선언 형식
2. 구형 화살표 본문 함수 형식
3. 구형 기본값 오버레이 연산 형식
4. 구형 intrinsic import 형식
5. 구형 고정 단일 intrinsic 객체 형식

## 문서 인덱스

1. `01_GOALS_AND_NON_GOALS.md`
2. `02_SYNTAX_EBNF.md`
3. `03_SEMANTICS.md`
4. `04_IMPORT_EXPORT_INTRINSIC.md`
5. `05_BUILD_MODEL.md`
6. `06_SECURITY_AND_BUDGET.md`
7. `07_DIAGNOSTICS_AND_TESTING.md`
8. `08_ROADMAP.md`
9. `09_INTRINSICS.md`
10. `10_ARCHITECTURE_INDEPENDENCE.md`
11. `11_PARUS_BUILD_PROFILE.md`
12. `12_BUILTIN_PLAN_SCHEMA_INJECTION.md`
13. `useage.md`

## 분리 원칙

1. LEI 언어 규칙: 표면 문법, 의미론, 진단, 보안/예산
2. Parus 통합 프로파일: `config.lei`, `master` 엔트리 해석, CLI override 정책
3. `master`, `bundle`은 LEI 키워드가 아니다. Parus 빌드 시스템이 주입하는 빌트인 plan 이름이다.

## 핵심 문법 요약

1. 선언: `plan`, `export plan`, `let`, `var`, `def`, `assert`, `import`
2. 함수: 블록 기반 `def`
3. 제어: `for`, `if`, `return`
4. 접근: 객체 `.` / 배열 `[]` / import namespace `alias::symbol`
5. 합성: `&`는 엄격 합성(unification)

## 권장 빌드 DSL 작성 스타일

```lei
export plan json = bundle & {
  name = "json";
  kind = "lib";
  sources = ["src/json.pr"];
  deps = [];
};
```

## 구현 범위 (이번 라운드)

1. 문서만 개정한다.
2. 코드/테스트 구현 변경은 다음 라운드에서 진행한다.
