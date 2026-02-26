# Frontend Pipeline

## 목적

Parus frontend의 실행 순서와 단계별 계약을 고정한다.
증분 파싱 구현 세부는 `pipeline/INCREMENTAL_PARSING.md`를 우선 참조한다.

## 현재 구현 (코드 근거)

1. 컴파일 엔트리: `compiler/parusc/src/p0/P0Compiler.cpp`
2. 파서: `frontend/src/parse/stmt/parse_stmt_core.cpp`
3. pass 실행: `frontend/src/passes/passes.cpp`
4. 타입체커: `frontend/src/tyck/core/type_check_entry.cpp`
5. capability check: `frontend/src/cap/*`
6. SIR 생성: `frontend/src/sir/lower/sir_builder_decl.cpp`, `frontend/src/sir/lower/sir_builder_expr_stmt.cpp`
7. OIR 생성/검증/패스: `frontend/src/oir/oir_builder.cpp`, `frontend/src/oir/oir_verify.cpp`, `frontend/src/oir/oir_passes.cpp`

## 단계 흐름

1. Lex: source -> token stream
2. Parse: token -> AST root(`StmtKind::kBlock`)
3. Parse error gate: parse 에러가 있으면 이후 단계 중단
4. AST passes: top-level item check, name resolve, expr checks
5. Tyck: 타입 확정, 대입 coercion, acts 해소
6. CAP: capability 제약 점검
7. SIR build + verify
8. OIR build + verify + optimization pass
9. backend로 OIR 전달

## 진단/오류 복구

1. parser는 진행 보장 가드(`cursor_.pos()` 변화 검사)를 사용해 무한 루프를 방지
2. pass/tyck는 가능한 한 계속 진행 후 진단 누적
3. p0는 각 게이트에서 error 존재 시 조기 반환

## 제약/비범위 (v0)

1. frontend-only 독립 바이너리는 `parusd`가 담당 (`parusc`는 `-fsyntax-only` 제공)
2. dyn 기반 런타임 다형성은 v0 비범위
3. proto/class 정적 의미와 lowering은 구현 중이며 dyn ABI는 후속 라운드에서 고정한다

## 미래 설계 (v1+)

1. incremental parse/tyck 캐시
2. frontend diagnostic artifact 표준화(JSON schema versioning)
3. SIR canonical pass와 OIR pre-lowering pass 분리 강화
