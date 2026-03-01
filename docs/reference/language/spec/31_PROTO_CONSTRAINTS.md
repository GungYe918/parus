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
5. class에서 proto 기본 구현을 사용하는 dot 호출은 첫 파라미터 `self`가 있는 멤버만 허용한다.
6. class/proto 멤버 경로 호출(`Class::m`, `Proto::m`)은 허용하지 않는다.

예시:

```parus
proto Identifiable {
  def id(self) -> i32 {
    return 7i32;
  }
};

class User : Identifiable {
  id_value: i32;
  init() = default;
}

def main() -> i32 {
  set u = User();
  return u.id();
}
```

## 19.6 제네릭 결합 규칙 (v1)

1. `proto` 선언은 제네릭 파라미터를 가질 수 있다.
2. `class`는 `class A : Proto<i32>` 형태로 concrete proto를 구현할 수 있다.
3. generic proto 멤버의 `Self`는 구현 class 기준 concrete 타입으로 정규화한다.
4. proto default impl이 있을 때 class에서 멤버를 생략하면 default가 사용된다.
5. `acts` 제네릭은 owner 타입 표기만 허용한다:
6. 허용: `acts for Vec<T> with [T: Proto] { ... }`
7. 금지: `acts for Vec<T> <T> { ... }`

예시:

```parus
proto Holder<T> {
  def get(self) -> T;
};

class IntHolder: Holder<i32> {
  init() = default;
  def get(self) -> i32 { return 1i32; }
}
```

## 19.7 진단 코드

1. `ProtoMemberBodyMixNotAllowed`
2. `ProtoOperatorNotAllowed`
3. `ProtoRequireTypeNotBool`
4. `ProtoRequireExprTooComplex`
5. `ProtoImplTargetNotSupported`
6. `ProtoImplMissingMember`
7. `ProtoConstraintUnsatisfied`
8. `GenericTypePathArityMismatch`
9. `GenericTypePathTemplateNotFound`
10. `GenericDeclConstraintUnsatisfied`
11. `GenericUnknownTypeParamInConstraint`
12. `GenericActsOverlap`
13. `ActsGenericClauseRemoved`
