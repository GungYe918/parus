# Frontend Internal Docs

이 디렉터리는 Parus 프론트엔드 구현 문서다.
언어 의미 정본은 `docs/reference/language/*`이며, 이 문서는 구현 세부와 코드 경로를 설명한다.

## 독자

1. 프론트엔드/타입체커/SIR/OIR 하강 경로를 수정하는 컴파일러 개발자
2. LSP/진단/패스 동작을 코드 기준으로 추적해야 하는 개발자

## 인덱스

1. `pipeline/FRONTEND_PIPELINE.md`: 전체 frontend 파이프라인
2. `parse/TOP_LEVEL_ITEMS.md`: top-level item, empty item(`;`) 정책
3. `parse/USE_IMPORT_NEST.md`: `use`/`import`/`nest` 파싱 규칙
4. `parse/FIELD_AND_ACTS_DECL.md`: `field`, `acts` 선언 파싱
5. `passes/NAME_RESOLVE.md`: 이름 해소와 alias rewrite
6. `passes/PASS_PIPELINE.md`: pass 순서/입출력 계약
7. `tyck/ASSIGN_COERCION_AND_NULLABLE.md`: nullable + 중앙 coercion
8. `tyck/ACTS_RESOLUTION.md`: acts 해소 규칙
9. `sir/SIR.md`: SIR 개요(이전 문서 이관)
10. `sir/SIRandOIR.md`: SIR/OIR 설계 노트(이전 문서 이관)
11. `sir/SIR_LOWERING.md`: AST -> SIR lowering 세부
12. `oir/OIR_HANDOFF.md`: SIR -> OIR handoff 경계

## 구현 우선순위 규칙

1. 언어/ABI 규칙 충돌 시 `docs/reference/language/SPEC.md`, `docs/reference/abi/*` 우선
2. 구현 동작 확인은 이 문서의 코드 근거 섹션과 실제 소스 동시 확인
3. 설계 초안과 구현이 충돌하면 구현 PR에서 둘 중 하나를 즉시 정합화
