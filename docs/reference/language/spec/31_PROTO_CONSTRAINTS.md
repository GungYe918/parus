# 19. Proto Constraints (v1)

문서 상태: `Normative`

## 19.1 범위

1. `proto`는 제약 선언 전용이다.
2. `proto`는 런타임 객체/디스패치 테이블을 만들지 않는다.
3. `dyn`은 런타임 다형성 경계 전용 키워드이며, ABI 확정 전까지 비구현이다.
4. 연산자 의미 확장은 `acts` 전용이며 `proto`에서 `operator` 선언은 금지한다.
5. Rust식 `#[derive(...)]`/proc-macro 블랙박스 모델은 채택하지 않는다. 컴파일타임 제약은 `proto`로 명시한다.
6. `$[]`, `$![]`는 `proto` 대체가 아니라 compiler directive 계층으로 분리한다(`$[If(...)]` builtin + `inst` 호출).

## 19.2 선언 규칙

```parus
proto ProtoName [: BaseProto, ...] {
  require struct(Path);
  require acts(IOOps);
  require type Item;
  require hash(v: Self) -> u64;
  provide def id() -> i32 { return 0i32; }
  provide const VERSION: i32 = 1i32;
};
```

1. `proto` 본문 항목은 `require` 또는 `provide`로 시작해야 한다.
2. `proto ... with require(...)` 꼬리 문법은 제거되었다.
3. `proto A: B, C`에서 `:` 뒤 대상은 `proto`만 허용한다.
4. `require` 항목에는 `proto(...)`를 허용하지 않는다.
5. `require type Name;`는 required associated type slot을 선언한다.
6. `require` 함수 시그니처는 `def` 없이 `require foo(...) -> T;` 형태를 사용한다.
7. `provide def`는 본문이 필수다.
8. `provide const`만 변수 제공으로 허용한다.
9. `provide const`는 정적(read-only) 상수로 취급하며 쓰기/가변 상태를 만들 수 없다.
10. proto `require/provide` 함수에서 `self` 리시버 파라미터는 금지한다.

## 19.3 require/provide 항목 의미론

1. `require struct/enum/class/actor/acts(Path);`는 선언 존재 + 가시성 + 종류 일치를 요구한다.
2. `require type Name;`는 구현체가 채워야 하는 associated type을 선언한다.
3. `require foo(...) -> T;`는 proto 계약 함수를 선언한다.
4. `provide def`는 계약을 충족시키는 기본 구현을 제공한다.
5. `provide const`는 인스턴스 필드가 아니라 프로그램 정적 수명 상수다.
6. `provide const` 초기화식은 컴파일타임 평가 가능해야 한다.

## 19.4 상속/클로저 규칙

1. `proto A: B, C`이면 `A`는 `B`, `C`의 요구사항을 모두 승계한다.
2. 승계는 transitive closure로 계산한다.
3. `child provide`가 `inherited require` 함수 시그니처를 만족하면 해당 요구는 충족된 것으로 본다.
4. `effective required fn set = closure(require fn) - closure(provide fn)`을 사용한다.

## 19.5 적용 대상과 충족 검사

1. `class Name : ProtoA, ...` 선언으로 proto 제약을 부착할 수 있다.
2. `struct Name : ProtoA, ...`, `enum Name : ProtoA, ...`도 선언상 허용된다.
3. 적용 가능한 proto는 effective required fn set과 required associated type set이 모두 충족되어야 한다.
4. `struct/enum`도 default `acts for T`를 통해 함수 requirement와 associated type requirement를 충족할 수 있다.
   * 함수 requirement는 default acts method가 맡는다.
   * associated type requirement는 acts header witness(`<... is Assoc>`)가 맡는다.
5. 시그니처 매칭은 `Self`와 `Self::Assoc`를 구현체 concrete 타입 + acts binding 기준으로 정규화해 비교한다.
6. class/proto 멤버 경로 호출(`Class::m`, `Proto::m`)은 허용하지 않는다.
7. proto `provide` 멤버 접근/호출은 `->`만 사용한다(`v->id()`, `v->Proto.id()`, `v->CONST`).
8. `.`와 `->`는 교차 fallback하지 않는다.

예시:

```parus
proto Identifiable {
  provide def id() -> i32 { return 7i32; }
};

class User : Identifiable {
  id_value: i32;
  init() = default;
};

def main() -> i32 {
  set u = User();
  return u->id();
}
```

## 19.6 require 타깃 심볼 가시성

1. 같은 파일 선언은 허용한다.
2. 같은 모듈 타 파일 선언은 `export`된 심볼만 허용한다.
3. 모듈 밖 선언은 `import/alias`로 반입된 경로 + `export`된 심볼만 허용한다.

## 19.7 제네릭 결합 규칙 (v2)

1. `proto` 선언은 제네릭 파라미터를 가질 수 있다.
2. `class`는 `class A : Proto<i32>` 형태로 concrete proto를 구현할 수 있다.
3. generic proto 멤버의 `Self`는 구현 class 기준 concrete 타입으로 정규화한다.
4. `acts` 제네릭은 owner 타입 표기만 허용한다.
5. 허용: `acts for Vec<T> with [T: Proto] { ... }`
6. 금지: `acts for Vec<T> <T> { ... }`
7. `with [...]` constraint atom은 다음 두 가지를 지원한다.
   - `T: SomeProto`
   - `T == TypeExpr`
8. `T == U`는 `TypeExpr`의 한 형태로 허용한다.
9. `with [...]` 안의 comma는 AND 의미만 가진다.
10. 새 constraint 절 문법이나 bool expression 기반 constraint language는 도입하지 않는다.

예시:

```parus
proto Holder<T> {
  provide def get_default(v: T) -> T { return v; }
};

class IntHolder: Holder<i32> {
  init() = default;
  def get(self) -> i32 { return 1i32; }
};

def only_i32<T>(x: T) with [T == i32] -> i32 {
  return 1i32;
}

def same<T, U>(x: T, y: U) with [T == U] -> bool {
  return true;
}
```

## 19.8 builtin proto family

primitive family classifier는 `core::constraints/proto.pr`의 builtin `use Foo;` 선언으로 노출되지만, 만족 여부 일부는 compiler가 builtin type에 대해 특별취급한다.

현재 builtin proto:

1. `Comparable`
2. `BinaryInteger`
3. `SignedInteger`
4. `UnsignedInteger`
5. `BinaryFloatingPoint`
6. `Step`

규칙:

1. signed integer primitive는 `SignedInteger`, `BinaryInteger`, `Comparable`를 만족한다.
2. unsigned integer primitive는 `UnsignedInteger`, `BinaryInteger`, `Comparable`를 만족한다.
3. `Step`은 discrete successor가 있는 primitive family filter다.
4. `i*`, `u*`, `isize`, `usize`, `char`는 `Step`을 만족한다.
5. `char`는 `Comparable`도 만족한다.
6. `f32`, `f64`는 `BinaryFloatingPoint`를 만족한다.
7. user-defined type이 이 builtin proto를 직접 `:`로 구현하는 것은 금지한다.
8. 이 proto들은 primitive family filter 용도로만 우선 사용한다.
9. 장기적으로 builtin/external type proto conformance나 structural satisfaction from `acts`가 생기면 library-only surface로 내릴 후보들이다.

## 19.9 의존 순환 금지

1. `type -> proto`, `proto -> proto`, `proto -> type/acts`, `acts -> owner type` 에지를 합친 그래프에서 순환은 금지한다.
2. 직접/간접 순환(`type <-> proto`, `proto <-> proto`, `proto -> acts -> type -> proto`)은 모두 hard error다.
3. 진단은 `ProtoDependencyCycle`을 사용한다.

## 19.10 진단 코드

1. `ProtoOperatorNotAllowed`
2. `ProtoMemberBodyNotAllowed`
3. `ProtoImplTargetNotSupported`
4. `ProtoImplMissingMember`
5. `ProtoConstraintUnsatisfied`
6. `GenericTypePathArityMismatch`
7. `GenericTypePathTemplateNotFound`
8. `GenericDeclConstraintUnsatisfied`
9. `GenericUnknownTypeParamInConstraint`
10. `GenericActsOverlap`
11. `ActsGenericClauseRemoved`
12. `GenericActorDeclNotSupportedV1`
13. `ProtoSelfParamForbidden`
14. `ProtoArrowMemberNotFound`
15. `ProtoArrowMemberAmbiguous`
16. `ProtoArrowQualifierRequired`
17. `ProtoDependencyCycle`
18. `GenericConstraintTypeMismatch`

## 19.11 예외 채널 마커 proto (v0)

예외 채널 분류는 아래 마커 proto를 사용한다.

```parus
proto Recoverable {};
proto Unrecoverable {};
```

규칙:

1. `throw` payload 타입은 `Recoverable`을 만족해야 한다.
2. panic/foreign fault로 분류되는 payload 타입은 `Unrecoverable`을 만족해야 한다.
3. 하나의 타입에 `Recoverable`와 `Unrecoverable`를 동시에 부착하는 것은 금지한다.
4. 권장 구현: 에러 payload는 `enum`/`struct` 값 타입으로 선언한다.
