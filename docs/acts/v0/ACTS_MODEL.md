# Parus Acts Model v0

문서 버전: `v0.0.0-draft`  
상태: `Guide (Companion)`

이 문서는 Parus `acts` 기능의 철학과 사용 모델을 설명하는 안내 문서다.  
규범(normative) 규칙은 `docs/spec_v0.md`를 우선한다.

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

2. default 부착 acts

```parus
acts for Vec2 {
  operator(+)(self a: Vec2, rhs: Vec2) -> Vec2 { ... }
}
```

3. named 부착 acts

```parus
acts FastMath for Vec2 {
  operator(+)(self a: Vec2, rhs: Vec2) -> Vec2 { ... }
}
```

---

## 4. acts 선택 문법 (`with`)

v0 선택 문법:

```parus
use Vec2 with acts(FastMath);
use Vec2 with acts(default);
```

규칙:

1. `use T with acts(Name);`는 현재 lexical scope에서 타입 `T`의 named acts를 활성화한다.
2. `use T with acts(default);`는 해당 스코프에서 default acts만 사용하도록 복귀한다.
3. 파일/함수/블록 어디서든 선언할 수 있다.
4. 내부 블록 선언은 외부 선언을 가리고(block exit 시 해제된다).

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
  def add(self v: &Vec2, rhs: i32) -> i32 { return v.x + rhs; }
}

acts A for Vec2 {
  def sub(self v: &Vec2, rhs: i32) -> i32 { return v.y - rhs; }
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
