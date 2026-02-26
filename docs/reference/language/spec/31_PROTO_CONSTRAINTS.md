# 19. Proto Constraints (v1)

문서 상태: `Normative`

## 19.1 범위

1. `proto`는 제약 선언 전용이다.
2. `proto` 자체는 런타임 객체/디스패치 테이블을 만들지 않는다.
3. `dyn`은 런타임 다형성 경계 전용 키워드이며, ABI 확정 전까지 비구현이다.
4. 연산자 의미 확장은 `acts` 전용이며 `proto`에서 `operator` 선언은 금지한다.

## 19.2 선언 규칙

```parus
proto ProtoName [: BaseProto, ...] {
  def sig(self, ...) -> Ret;         // signature-only
  def defaulted(self, ...) -> Ret {  // default-body
    ...
  }
} [with require(<expr>)];
```

1. `proto` 멤버는 함수 선언만 허용한다.
2. 멤버 본문 규칙은 all-or-none이다.
3. 전부 본문 없음: 모든 멤버는 구현체에서 필수 구현이다.
4. 전부 본문 있음: 모든 멤버는 기본 구현이며 구현체에서 재정의 가능하다.
5. 일부만 본문 있는 혼합 형태는 오류다(`ProtoMemberBodyMixNotAllowed`).
6. `with require(...)`는 생략 가능하며 생략 시 `require(true)`가 암묵 삽입된다.

## 19.3 require 식 규칙 (v1)

1. 파서는 `require(...)`에 일반 표현식을 허용한다.
2. 타입체커는 현재 v1에서 단순 컴파일타임 bool 식만 허용한다.
3. v1 허용식:
4. `true`, `false`
5. `not expr` 또는 `!expr`
6. `expr and expr`
7. `expr or expr`
8. 괄호
9. 이 외 식은 `ProtoRequireExprTooComplex`로 진단한다.

## 19.4 적용 대상

1. `class Name : ProtoA, ...` 선언으로 proto 제약을 부착할 수 있다.
2. `field Name : ProtoA, ...` 선언도 허용된다.
3. 함수 제네릭 제약은 `with [T: ProtoName]`로 선언한다.

## 19.5 구현 충족 검사

1. 구현체(`class`/`field`)는 proto가 요구한 필수 시그니처를 충족해야 한다.
2. 기본 구현이 있는 proto 멤버는 구현체가 생략할 수 있다.
3. proto 상속이 있으면 상위 proto의 필수 시그니처까지 포함해 검사한다.
4. 시그니처 매칭은 `Self`/`&Self`를 구현체 타입 기준으로 정규화해 비교한다.

## 19.6 진단 코드

1. `ProtoMemberBodyMixNotAllowed`
2. `ProtoOperatorNotAllowed`
3. `ProtoRequireTypeNotBool`
4. `ProtoRequireExprTooComplex`
5. `ProtoImplTargetNotSupported`
6. `ProtoImplMissingMember`
7. `ProtoConstraintUnsatisfied`
