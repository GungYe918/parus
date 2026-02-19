# Parus Acts Model v0

문서 버전: `v0.0.0-draft`  
상태: `Guide (Companion)`

이 문서는 Parus `acts` 기능의 철학과 사용 모델을 설명하는 안내 문서다.  
규범(normative) 규칙은 `docs/reference/language/SPEC.md`를 우선한다.

---

## 1. 목적

`acts`는 Parus에서 "행동"을 타입 저장소(`field`)와 분리해 기술하기 위한 장치다.

1. 저장소/레이아웃(`field`)과 연산/행동(`acts`) 분리
2. dot 호출/연산자 해석을 명시적 규칙으로 고정
3. 스코프 기반 정책 선택으로 실험/최적화 정책을 안전하게 교체

---

## 2. 핵심 철학

1. `field`는 데이터, `acts`는 동작이다.
2. 연산자 의미는 runtime 동적 디스패치가 아니라 compile-time 해소로 결정한다.
3. `acts` 선택은 lexical scope에서만 영향을 준다.
4. default 동작은 항상 존재 가능하며, named acts는 선택적으로 덮어쓴다.

---

## 3. acts 블록의 세 형태

1. 일반 acts 네임스페이스

```parus
acts Math {
  def add(a: i32, b: i32) -> i32 { return a + b; }
}
```

2. default 부착 acts(`acts for`)

```parus
acts for Vec2 {
  operator(+)(self, rhs: Vec2) -> Vec2 { ... }
}
```

3. named 부착 acts(`acts Name for`)

```parus
acts FastMath for Vec2 {
  operator(+)(self, rhs: Vec2) -> Vec2 { ... }
}
```

---

## 4. acts 선택 문법 (`with`)

v0 선택 문법:

```parus
use Vec2 with acts(FastMath);
use Vec2 with acts(default);

set mut v = Vec2{ x: 42, y: 42 } with acts(FastMath);
```

규칙:

1. `use T with acts(Name);`는 현재 lexical scope에서 타입 `T`의 named acts를 활성화한다.
2. `use T with acts(default);`는 해당 스코프에서 default acts만 사용하도록 복귀한다.
3. 파일/함수/블록 어디서든 선언할 수 있다.
4. 내부 블록 선언은 외부 선언을 가리고(block exit 시 해제된다).
5. 바인딩 sugar(`let/set ... = expr with acts(...)`)는 해당 바인딩의 dot/operator 해소 우선순위에 적용된다.

---

## 4.1 2-레인 모델 (v0 고정)

1. 레인 A: `acts for T` / `acts Name for T`
   - 확장 메서드/연산자 전용
   - 모든 함수 첫 파라미터에 `self` 계열 필수
   - dot/operator 해소 대상
2. 레인 B: `acts Name { ... }`
   - 일반 네임스페이스 함수 전용
   - `self` 금지, operator 금지
   - 경로 호출 전용

경로 호출 예시:

```parus
acts(Math)::add(1, 2);
use acts(Math) as m;
m::add(1, 2);

use Vec2 as v2;
set v = v2{ x: 1, y: 2 };
v2::acts(FastMath)::add(v, 1);
```

---

## 5. 해석 규칙 (named-first fallback)

타입 `T`에 대해 named acts가 선택된 경우:

1. named acts(`acts Name for T`)에서 먼저 찾는다.
2. 없으면 default acts(`acts for T`)에서 찾는다.
3. 그래도 없으면 builtin 또는 타입 에러로 처리한다.

`use T with acts(default);`인 경우:

1. default acts만 조회한다.
2. named acts는 조회하지 않는다.

---

## 6. 충돌 규칙

1. 활성 named acts와 default acts에 동일 시그니처가 동시에 존재하면 컴파일 에러.
2. 같은 lexical scope에서 동일 타입 `T`에 서로 다른 named acts를 동시에 활성화하면 컴파일 에러.
3. 오류 진단은 반드시 두 선언 위치(named/default)를 함께 보여야 한다.
4. `acts for T` 함수에 `self`가 없으면 컴파일 에러.
5. 일반 `acts Name {}` 함수에 `self`가 있으면 컴파일 에러.

---

## 7. 연산자 규칙

1. 연산자 선언은 `operator(...)` 문법만 허용.
2. `acts for T` 또는 `acts Name for T`에서만 operator 선언 가능.
3. 일반 `acts A {}`에서는 operator 선언 금지.
4. 연산자 해석도 메서드와 동일하게 named-first fallback 규칙을 따른다.

---

## 8. 예시

```parus
field Vec2 {
  x: i32;
  y: i32;
}

acts for Vec2 {
  def add(self, rhs: i32) -> i32 { return self.x + rhs; }
}

acts A for Vec2 {
  def sub(self, rhs: i32) -> i32 { return self.y - rhs; }
}

def demo(v: Vec2) -> i32 {
  set a = v.add(1);                // default
  {
    use Vec2 with acts(A);
    set b = v.sub(1);              // A from inner scope
  }
  use Vec2 with acts(default);
  return a;
}
```

---

## 9. v0 비범위

1. runtime 동적 acts 바인딩
2. proto 기반 acts 계약 강제
3. acts의 원격/지연 로딩

위 항목은 v1+에서 별도 설계 문서로 다룬다.

---

## 10. proto / 제네릭 / dyn 경계 (정렬 규칙)

이 문서는 `acts` 안내 문서이며, 아래 규칙은 정본 문서와의 충돌 방지를 위한 정렬 규칙이다.

1. `proto`는 계약(시그니처) 전용이며 연산자 재정의를 담당하지 않는다.
2. 연산자/행동 선택은 `acts`가 담당한다.
3. 제네릭 제약 표기는 `with [ ... ]` 단일 문법을 사용한다.
4. `[]` 단독 또는 브래킷 없는 `with` 표기는 정본에서 사용하지 않는다.
5. `dyn`은 v1+에서 명시 opt-in 경계로만 도입하며, v0 `acts` 해소는 정적 디스패치만 사용한다.

정본 문서:

1. `docs/reference/language/SPEC.md`
2. `docs/reference/abi/v0.0.1/OOP_MODEL.md`
3. `docs/reference/abi/v0.0.1/GENERICS_MODEL.md`

구현 문서:

1. `frontend/docs/parse/FIELD_AND_ACTS_DECL.md`
2. `frontend/docs/tyck/ACTS_RESOLUTION.md`
