# SIR 설계 문서 (v0)

## 1. 목적
SIR(Structured Intermediate Representation)은 AST의 문법 정보를 최대한 보존하면서도,
후단(OIR/코드생성)에서 필요한 정적 정보를 명확하게 제공하기 위한 중간 표현이다.

핵심 목표는 다음과 같다.

- AST의 구조적 제어 흐름(`if/while/loop/block`)을 손실 없이 보존한다.
- 이름 해석 결과(SymbolId)와 타입체크 결과(TypeId)를 값/문장 노드에 부착한다.
- 후단이 다시 파서를 흉내 내지 않도록, 호출 인자/캐스트/선언 타입 정보를 명시한다.
- OIR lowering 전에 SIR 자체 무결성 검증(verify)으로 변환 오류를 조기 차단한다.

## 2. 모듈 구조
SIR 모듈(`parus::sir::Module`)은 다음 배열 기반 저장소를 가진다.

- `values`: 표현식 값 노드 (`Value`)
- `stmts`: 문장 노드 (`Stmt`)
- `blocks`: 블록 노드 (`Block`)
- `funcs`: 함수 노드 (`Func`)
- `args`: 호출 인자 노드 (`Arg`)
- `params`: 함수 파라미터 (`Param`)
- `attrs`: 함수 attribute (`Attr`)

### 2.1 Value
`Value`는 리터럴, 연산, 호출, 캐스트, 구조화 표현식(`IfExpr`, `BlockExpr`, `LoopExpr`)을 표현한다.

주요 필드:

- `kind`: 값 종류
- `type`: tyck 결과 타입(TypeId)
- `a,b,c`: kind별 피연산자 슬롯
- `sym`: 식별자 값의 심볼 바인딩
- `place`: place 분류 (`NotPlace/Local/Index/...`)
- `effect`: 효과 분류 (`Pure/MayWrite/Unknown`)
- `cast_to`: 캐스트 목표 타입

### 2.2 Stmt
`Stmt`는 실행 단위 문장을 나타낸다.

- `ExprStmt`, `VarDecl`, `IfStmt`, `WhileStmt`, `Return`, `Break`, `Continue` 지원
- `VarDecl`은 `is_set`, `is_mut`, `declared_type`, `init`를 보존
- 제어문은 `a/b`에 하위 블록 참조를 저장

### 2.3 Block
`Block`은 문장 슬라이스를 가진다.

- `stmt_begin`, `stmt_count`는 **해당 블록의 직계 문장만** 가리킨다.
- 중첩 블록 문장은 별도 `Block`에 저장되며, 부모 슬라이스와 겹치지 않는다.

### 2.4 Func
함수 선언 메타데이터를 보존한다.

- `name`, `sym`, `sig`, `ret`
- qualifier(`is_pure`, `is_comptime`, `is_export`, `fn_mode` 등)
- `attr`/`param` 슬라이스
- `entry` 블록

## 3. 현재 lowering 정책

### 3.1 타입/심볼 반영
- `NameResolveResult`를 통해 `Expr/Stmt/Param -> SymbolId`를 복원한다.
- `TyckResult`의 `expr_types`를 `Value.type`으로 반영한다.
- `VarDecl.declared_type`는 가능한 한 심볼/사용지점 타입을 우선 사용해 확정한다.

### 3.2 제어 흐름 보존
- AST의 `if/while/loop/block`은 SIR에서도 구조화된 노드로 유지한다.
- 블록 lowering 시 직계 문장 슬롯을 먼저 예약해, 중첩 블록으로 인한 슬라이스 오염을 방지한다.

### 3.3 효과(effect) 전파
- 기본 효과는 값 종류별 분류(`Assign/PostfixInc -> MayWrite`, `Call -> Unknown`)를 사용한다.
- 이후 child value 및 하위 block을 통해 효과를 합성 전파한다.

## 4. SIR Verify
`sir::verify_module`은 다음 무결성 항목을 점검한다.

- 블록 문장 슬라이스 범위 유효성
- 문장이 여러 블록에 중복 소속되는지(슬라이스 overlap) 검사
- 함수의 entry/attr/param 슬라이스 유효성
- 문장의 value/block 참조 유효성
- 값 노드의 child/value/arg/block 참조 유효성

이 검증은 AST->SIR 변환 버그를 OIR 단계 이전에 조기 차단하는 안전장치다.

## 5. 최적화 이점

- **구조 보존 기반 분석 용이성**: CFG를 강제하기 전에도 블록 단위 지역 분석이 쉽다.
- **타입/심볼 부착**: alias 분석, mut 분석, 호출 규약 검증의 기반 정보가 이미 존재한다.
- **효과 정보 활용**: `Pure/MayWrite/Unknown`을 활용해 보수적 DCE/순서 최적화 준비가 가능하다.
- **호출 인자 구조 보존**: named-group/labeled 호출 정규화 이전 단계로 디버깅과 lowering 추적이 쉽다.

## 6. 향후 구현 권장

1. Call 인자 정규화 레이어 추가
- 파라미터 순서에 맞춘 normalized arg vector를 SIR에 추가해 OIR에서 재해석을 줄인다.

2. 제어문 확장 lowering
- `switch`를 SIR 구조 노드로 승격하거나, 최소한 케이스 블록 정보를 보존한다.

3. effect lattice 고도화
- `MayReadMem/MayTrap` 등 세분화로 안전한 코드 이동/결합 최적화 범위를 확대한다.

4. 값 수명/사용자 정보(use-def) 인덱스
- OIR 전처리로 use-list를 생성해 dead value 제거, CSE 기반을 강화한다.

5. SIR canonical pass
- 불필요한 `BlockExpr` 중첩 정리, trivial block folding, 단순 cast 정규화 등을 수행한다.
