# Pass Pipeline

## 목적

parse 이후 frontend pass 순서, 입력/출력 계약, 실패 모드를 고정한다.

## 현재 구현 (코드 근거)

1. 파이프라인 엔트리: `frontend/src/passes/passes.cpp`
2. top-level item 검사: `frontend/src/passes/check_top_level_decl_only.cpp`
3. expression pass 예시(pipe-hole): `frontend/src/passes/check_pipe_hole.cpp`
4. name resolve: `frontend/src/passes/name_resolve.cpp`

## 순서

1. `check_top_level_decl_only`
2. `name_resolve_stmt_tree`
3. expr walker 기반 pass (`check_pipe_hole` 등)

## 입출력 계약

1. 입력: AST + program root stmt id
2. 출력: `PassResults`
   - `sym` (`sema::SymbolTable`)
   - `name_resolve` (`NameResolveResult`)
3. 진단은 공용 `diag::Bag`에 누적

## 실패 모드

1. parse 단계 오류가 있으면 p0가 pass 진입 전에 중단
2. pass 단계 오류는 가능한 누적 후 tyck 단계 진입 여부를 호출자가 결정

## 제약/비범위 (v0)

1. pass dependency graph 자동 해석 없음(고정 순서)
2. incremental pass invalidation 없음

## 미래 설계 (v1+)

1. pass registry + dependency resolver
2. frontend-only mode에서 pass profile 출력
