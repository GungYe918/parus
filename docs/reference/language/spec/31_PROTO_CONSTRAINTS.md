# 19. Proto Constraints (v1)

문서 상태: `Normative`

## 19.1 범위

1. `proto`는 제약 선언 전용이다.
2. 런타임 객체/디스패치 테이블을 만들지 않는다.
3. 연산자 의미는 변경하지 않는다(`acts`와 분리).

## 19.2 선언 규칙

```parus
proto ProtoName [: BaseProto, ...] {
  def sig(self, ...) -> Ret;
  def sig2?(...) -> Ret;
} with require(<simple-bool-expr>);
```

1. proto member는 함수 시그니처만 허용한다.
2. proto member에 본문을 쓰면 오류다.
3. proto tail `with require(...)`는 필수다.
4. `require(...)`는 타입이 `bool`이어야 한다.

## 19.3 require 식 규칙 (v1)

허용:
1. `true`
2. `false`
3. `!expr`
4. `expr && expr`
5. `expr || expr`
6. 괄호

금지:
1. 식별자 참조
2. 함수 호출
3. 매크로 호출
4. 멤버 접근
5. 비교/산술 연산

## 19.4 적용 대상

1. `class`은 `class Name : ProtoA, ...`로 구현 선언 가능.
2. `field`는 `field Name : ProtoA, ...` 문법을 허용한다.
3. 함수 제네릭 제약은 `with [T: ProtoName]`로 선언한다.

## 19.5 구현 충족 검사

1. 구현체는 proto가 요구한 함수 시그니처를 모두 충족해야 한다.
2. proto 상속이 있으면 상위 proto 요구 시그니처도 포함해 검사한다.
3. 누락/불일치 시 타입체크 오류를 발생시킨다.

## 19.6 Self / self

1. `Self`: 구현체 타입 플레이스홀더
2. `self`: 리시버 파라미터 표기
3. 시그니처 매칭 시 `Self`/`&Self`는 구현체 타입 기준으로 정규화해 비교한다.

## 19.7 진단 코드

1. `ProtoMemberBodyNotAllowed`
2. `ProtoOperatorNotAllowed`
3. `ProtoRequireMissing`
4. `ProtoRequireTypeNotBool`
5. `ProtoRequireExprTooComplex`
6. `ProtoImplTargetNotSupported`
7. `ProtoImplMissingMember`
8. `ProtoConstraintUnsatisfied`

