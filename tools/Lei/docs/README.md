# LEI DSL Docs

상태: `draft-0.1`

LEI는 Parus 빌드 시스템 전용 DSL이다. 이 문서는 LEI v0의 설계 고정 문서를 모은다.

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

## 구현 범위 (v0)

1. 문법: `{}`/`[]` 기반, 들여쓰기 의미 없음
2. 타입: `int`, `float`, `string`, `bool`
3. 선언: `import`, `let`, `const`, `def`, `assert`, `export build`
4. 모듈: `export` + named `import`
5. intrinsic: `base` 자동 제공(import 불필요)
6. 보안: 비튜링 정책, 효과/동적실행 금지