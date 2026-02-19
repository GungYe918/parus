# Tyck: Assign Coercion and Nullable

## 목적

`T?` 하이브리드 모델과 중앙 대입 coercion 엔진 동작을 정리한다.

## 현재 구현 (코드 근거)

1. `can_assign_`, `classify_assign_with_coercion_`: `frontend/src/tyck/expr/type_check_expr_core.cpp`
2. API/enum: `frontend/include/parus/tyck/TypeCheck.hpp`
3. let/assign/return/call/default/field-init 통합 호출: `frontend/src/tyck/stmt/type_check_stmt.cpp`, `frontend/src/tyck/expr/type_check_expr_call_cast.cpp`

## 모델 (v0)

1. `T?`는 optional value type
2. 전역 `T <: T?` 승격은 허용하지 않음
3. 대입 경계에서만 `T -> T?` 주입 허용

## CoercionPlan

1. `Exact`
2. `NullToOptionalNone`
3. `LiftToOptionalSome`
4. `InferThenExact`
5. `InferThenLiftToOptionalSome`
6. `Reject`

## 적용 경계 (`AssignSite`)

1. `LetInit`
2. `SetInit`
3. `Assign`
4. `FieldInit`
5. `CallArg`
6. `Return`
7. `DefaultArg`
8. `NullCoalesceAssign`

## 진단 정책

1. 내부 추론 타입명 `{integer}`를 사용자 메시지에 직접 노출하지 않음
2. mismatch는 expected/got + site context로 보고

## 제약/비범위 (v0)

1. optional niche optimization은 backend 타겟별 최적화 보류
2. `some(...)`/`none` 표면 문법 없음

## 미래 설계 (v1+)

1. coercion trace 디버그 모드
2. optional lowering canonical form 통합(backend와 shared)
