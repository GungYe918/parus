# Parus Nullable Model v0 (Hybrid)

문서 버전: `v0.0.1`  
상태: `Normative (Single Source of Truth for Nullable Semantics)`

이 문서는 Parus의 nullable(`T?`) 의미 규칙의 단일 신뢰 기준이다.  
nullable 관련 사항에서 `docs/reference/language/SPEC.md`와 충돌하면, 본 문서를 우선한다.

---

## 1. 목표

1. `T?`를 값 타입 기반으로 모델링하여 성능 저하를 최소화한다.
2. `let a: T? = v` 같은 직관적 코드를 허용하여 DX를 개선한다.
3. 전역 암시 승격을 피해서 타입 추론/오버로드 안정성을 유지한다.

---

## 2. 코어 모델

1. `T?`는 `T` 또는 `null`을 담는 1급 값 타입이다.
2. `null`은 값 부재를 나타내는 리터럴이다.
3. nullable은 기본적으로 heap object가 아니며 inline 값 표현을 우선한다.

---

## 3. 승격 정책 (Hybrid)

### 3.1 허용: 대입 경계에서만 `T -> T?`

다음 경계에서 암시 주입을 허용한다.

1. let/set 초기화
2. 대입(`=`)
3. struct initializer 멤버 값
4. 함수 인자 전달
5. return
6. default parameter
7. `??=` rhs

### 3.2 금지: 표현식 전역 승격

1. `T <: T?`를 전역 서브타입 관계로 두지 않는다.
2. 일반 연산자 해소/오버로드 해소 단계에서 자동 승격하지 않는다.

---

## 4. 연산 규칙

### 4.1 비교

1. `x == null`, `x != null` 비교는 nullable 문맥에서 허용한다.

### 4.2 Null coalescing (`??`)

1. `lhs`는 `T?`여야 한다.
2. `rhs`는 `T`로 대입 가능해야 한다.
3. 결과 타입은 `T`다.

### 4.3 Null coalescing assign (`??=`)

1. lhs는 `T?` place여야 한다.
2. rhs는 `T`로 대입 가능해야 한다.
3. rhs가 plain `T`인 경우 nullable Some으로 주입한다.

### 4.4 Cast

1. `as`, `as?`, `as!`의 기본 의미는 기존 cast 규칙을 따른다.
2. nullable 자동 해소(unwrapping)는 `as`에서 수행하지 않는다.

---

## 5. infer integer(`{integer}`)와 nullable

1. unsuffixed integer literal은 내부적으로 `{integer}` placeholder를 갖는다.
2. nullable 대상(`T?`)으로 대입할 때 infer 컨텍스트는 `T`(element)를 사용한다.
3. 사용자 진단에서는 `{integer}` 내부명을 직접 노출하지 않는다.

---

## 6. 내부 표현/하강

1. 내부 타입 시스템의 optional은 `Kind::kOptional(elem)`로 표현한다.
2. LLVM 단계의 canonical 값 표현은 `{ i1, Elem }`를 기본으로 한다.
3. Some/None 생성은 의미 보존이 되는 canonical 경로로 lowering한다.

---

## 7. ABI/FFI 경계

1. C ABI(`c-v0`)에서는 optional 직접 값 전달을 허용하지 않는다.
2. `layout(c)` struct 멤버에 optional(`T?`)은 금지한다.
3. nullable은 Parus Internal ABI 규칙으로 다룬다.

---

## 8. 성능 원칙

1. nullable 자체를 heap allocation의 이유로 사용하지 않는다.
2. 최적화 경로에서 scalar replacement / dead-tag 제거를 허용한다.
3. AOT/JIT 모두 동일 nullable 의미를 유지해야 한다.

---

## 9. 구현 체크리스트

다음을 통과해야 nullable hybrid 규칙 준수로 본다.

1. `let a: i32? = 5;`가 정상 동작한다.
2. `struct A { x: i32? }` + `A{ x: 10 }`가 정상 동작한다.
3. 표현식 전역 승격 없이 대입 경계 주입만 허용된다.
4. `{integer}` 내부명이 사용자 진단에 직접 노출되지 않는다.
5. optional 값 표현이 OIR/LLVM에서 일관된다.

---

## 10. 변경 이력

### v0.0.1

1. nullable 하이브리드 모델(경계 주입 + 전역 승격 보류) 확정
2. infer integer와 optional element 컨텍스트 해소 규칙 고정
3. 사용자 진단에서 내부 placeholder 노출 금지 원칙 추가
