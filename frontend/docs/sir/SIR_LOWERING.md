# SIR Lowering

## 목적

AST/NameResolve/Tyck 결과를 SIR 모듈로 내리는 실제 lowering 계약을 문서화한다.

## 현재 구현 (코드 근거)

1. decl lowering: `frontend/src/sir/lower/sir_builder_decl.cpp`
2. expr/stmt lowering: `frontend/src/sir/lower/sir_builder_expr_stmt.cpp`
3. entry API: `frontend/include/parus/sir/Builder.hpp`

## 입력 계약

1. AST root stmt
2. SymbolTable
3. NameResolveResult
4. TyckResult
5. TypePool

## 출력 계약

1. `sir::Module`
2. 함수/블록/값/문장 테이블 일관성
3. call, field init, acts 관련 메타가 SIR 노드에 반영

## acts 관련 lowering 메모

1. dot 호출은 tyck에서 확정된 호출 대상(symbol/stmt id)을 기반으로 lowering
2. receiver 주입이 필요한 경우 call arg에 self 인자 선행 삽입
3. explicit acts path 호출도 최종적으로 일반 call target으로 정규화

## 진단/오류 복구

1. SIR build 단계는 가능한 best-effort로 모듈 구성
2. 이후 `sir::verify_module`에서 구조 위반을 실패 처리

## 제약/비범위 (v0)

1. SIR canonical optimization은 제한적
2. 고급 region/lifetime 정보는 미도입

## 미래 설계 (v1+)

1. call arg canonical form 저장
2. effect lattice 세분화
3. frontend-lsp incremental SIR cache
