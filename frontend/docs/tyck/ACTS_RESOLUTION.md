# Tyck: Acts Resolution

## 목적

`acts for`/`acts Name for` 해소 규칙, self 제약, dot/path 호출 경계를 정리한다.

## 현재 구현 (코드 근거)

1. acts decl 검사: `frontend/src/tyck/stmt/type_check_stmt.cpp`
2. acts 인덱스/선택 스택: `frontend/src/tyck/core/type_check_entry.cpp`
3. call 해소: `frontend/src/tyck/expr/type_check_expr_call_cast.cpp`

## 핵심 규칙

1. `acts for T` member는 첫 파라미터 self 필수
2. 일반 `acts Name {}` member는 self 금지
3. operator 선언은 acts-for 계열만 허용
4. lexical scope acts 선택 스택 사용 (`use T with acts(...)`)

## 해소 우선순위

1. dot call: receiver type + active acts selection
2. explicit path call: `T::acts(Set)::member(...)`
3. explicit path는 owner가 type symbol이어야 하며 value path 금지

## 충돌 규칙

1. default/named 동일 시그니처 중복 시 에러
2. 같은 scope에서 동일 owner 타입의 상충 selection 에러

## 진단/오류 복구

1. self 누락/오용을 선언 지점에서 즉시 진단
2. dot 호출에서 self 없는 acts member를 호출하면 path call 사용 가이드 제공
3. unknown acts set/name은 owner 타입 포함 메시지로 보고

## 제약/비범위 (v0)

1. runtime acts dispatch 없음
2. acts inheritance 없음
3. proto와 acts 직접 결합 규칙 없음

## 미래 설계 (v1+)

1. explicit acts path에서 alias rewrite 완전 통합
2. method/operator 공통 후보 엔진 추상화
