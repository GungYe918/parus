# Pass: Name Resolution

## 목적

AST의 식별자/경로를 symbol table과 연결하고, 후속 tyck가 재탐색 없이 해소 결과를 사용할 수 있게 한다.

## 현재 구현 (코드 근거)

1. 구현: `frontend/src/passes/name_resolve.cpp`
2. 결과 구조: `frontend/include/parus/passes/NameResolve.hpp`
3. 심볼 저장소: `frontend/include/parus/sema/SymbolTable.hpp`

## 핵심 동작

1. scope push/pop 가드(`ScopeGuard`)로 lexical scope 관리
2. `import/use as` alias rewrite 후 lookup
3. namespace stack 기반 qualified lookup
4. `ExprKind::kIdent`를 `ResolvedSymbol`에 매핑
5. param/local/fn/type binding kind 분류

## acts 경로 예외

1. `T::acts(A)::member` 형태는 name_resolve에서 일반 symbol lookup을 강제하지 않는다.
2. 이유: acts 선택/검증은 tyck의 acts 해소 단계에서 수행해야 false positive를 줄일 수 있음.

## 진단/오류 복구

1. duplicate/shadowing 정책은 `NameResolveOptions`로 제어
2. unresolved ident는 `kUndefinedName`
3. id-space(AST ExprId/StmtId) 혼동을 막기 위해 range 검증 보조 함수를 사용

## 제약/비범위 (v0)

1. cross-file/bundle 전체 심볼 해소는 제한적
2. explicit acts owner alias(`use Vec2 as v2`)의 완전 경로 rewrite는 tyck와 협업 필요

## 미래 설계 (v1+)

1. import/use/acts alias 통합 resolver 모듈화
2. NameResolve result를 SIR lowering의 primary input으로 승격
