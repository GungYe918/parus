# SIR Model (v0)

## 목적

SIR(Structured IR)은 AST 의미를 보존하면서 OIR/Backend로 안전하게 전달하기 위한 중간 표현이다.

## 코드 근거

1. 구조 정의: `frontend/include/parus/sir/SIR.hpp`
2. 빌더 엔트리: `frontend/include/parus/sir/Builder.hpp`
3. decl lowering: `frontend/src/sir/lower/sir_builder_decl.cpp`
4. expr/stmt lowering: `frontend/src/sir/lower/sir_builder_expr_stmt.cpp`
5. verify: `frontend/src/sir/verify/sir_verify.cpp`

## 핵심 저장소

1. `values`
2. `stmts`
3. `blocks`
4. `funcs`
5. `args`
6. `params`

각 노드는 인덱스 기반 참조를 사용하고, lowering 단계에서 타입/심볼 정보를 부착한다.

## 설계 포인트

1. AST의 구조적 제어흐름(`if/while/loop/block`) 보존
2. NameResolve/Tyck 결과를 값 노드에 동기화
3. 후단에서 파서 재해석 없이 call/place/effect 정보를 활용 가능

## verify 게이트

1. block stmt slice 유효성
2. 함수 entry/param/attr slice 유효성
3. value/stmt 참조 인덱스 유효성

## 제약/비범위 (v0)

1. full canonical form 강제 없음
2. region/lifetime 전용 메타 미도입
