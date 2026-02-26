# Parus OOP Pattern Model v0.0.1

문서 버전: `v0.0.1`  
상태: `Normative (Single Source of Truth for OOP Semantics)`

이 문서는 Parus의 OOP/다형성/행동 부착 모델의 단일 신뢰 기준이다.  
OOP 관련 사항에서 `docs/reference/language/SPEC.md` 또는 다른 안내 문서와 충돌하면 본 문서를 우선한다.
제네릭/제약 표기의 세부 합의 범위는 `docs/reference/abi/v0.0.1/GENERICS_MODEL.md`를 따른다.

---

## 1. 목적

Parus OOP 모델은 아래 목표를 동시에 만족해야 한다.

1. 정적 디스패치 기반의 예측 가능한 성능
2. `field`/`acts`/`proto`/`class`/`actor` 역할 분리
3. 멀티스레딩 핵심 모델(`actor`, `commit`, `recast`)의 불변식 보호
4. v1+ 제네릭 확장 시 문법 복잡도(`<>` 과밀) 최소화

---

## 2. 코어 원칙

1. v0의 호출 해소는 100% 컴파일타임 정적 디스패치로 끝나야 한다.
2. `dyn` 기반 런타임 동적 디스패치는 v0 범위 밖이다.
3. `acts`는 상속/다형성 수단이 아니라 "행동/연산자 정책 부착" 수단이다.
4. `proto`는 인터페이스 계약의 중심이며, 연산자 재정의를 담당하지 않는다.
5. `actor`는 상태머신(`draft/pub/sub/commit/recast`)이므로 외부 행동 주입을 제한한다.

---

## 3. 요소별 역할 분리

### 3.1 `field`

1. 순수 데이터 저장소(POD 중심)다.
2. 레이아웃/ABI 중심 타입이다.
3. 값 의미(value semantics) 중심 경량 타입이다.

### 3.2 `acts`

1. 타입에 부착되는 행동/연산자 집합이다.
2. 정적 해소 대상으로만 사용한다.
3. 다형성/상속/런타임 디스패치 수단이 아니다.

### 3.3 `proto`

1. 인터페이스 계약(시그니처 집합)이다.
2. 구현 본문/연산자 정의를 포함하지 않는다.
3. 다른 `proto`를 상속(확장)할 수 있다.
4. v1+에서 제네릭 제약의 중심으로 확장한다.

### 3.4 `class`

1. 구현체 타입이다.
2. `proto`를 구현한다.
3. 생성/소멸 수명주기(`init`/`deinit`)를 갖는다.

### 3.5 `actor`

1. 멀티스레딩 공유 상태 모델의 중심이다.
2. `draft/pub/sub/commit/recast` 불변식을 유지해야 한다.
3. 외부 `acts for` 부착 대상이 아니다.

---

## 4. 정적 디스패치 모델 (v0 고정)

1. 함수/메서드/연산자 해소는 컴파일타임에 단일 후보로 결정되어야 한다.
2. 모호성은 런타임으로 미루지 않고 컴파일 에러로 처리한다.
3. `proto` 호출도 v0에서는 정적 경계로 제한한다(`dyn` 없음).

---

## 5. `self`와 `Self` 규칙 (고정)

### 5.1 `self` (리시버 마커)

1. `self`는 값 이름이 아니라 리시버 마커다.
2. 함수 파라미터의 첫 위치에서만 허용한다.
3. 허용 형태:
   - `self` (기본: read-only receiver)
   - `self mut` (mutable receiver)
4. `self move`는 v0에서 보류한다.
5. 의미:
   - `self`: `&Self`로 해석
   - `self mut`: `&mut Self`로 해석

### 5.2 `Self` (문맥 타입 이름)

1. `Self`는 타입 위치에서만 쓰는 contextual 타입 이름이다.
2. `Self`는 일반 전역 키워드가 아니라 아래 문맥에서만 특별 해석한다.
   - `proto` 본문
   - `class` 본문
   - `acts for T` / `acts Name for T` 본문
3. `Self`는 현재 소유 타입으로 치환된다.
   - `proto Drawable` 내부: `Self`는 구현체 타입 자리
   - `class Sprite` 내부: `Self == Sprite`
   - `acts for Vec2` 내부: `Self == Vec2`

### 5.3 왜 둘 다 필요한가

1. `self`는 호출/리시버 전달 의미를 표현한다.
2. `Self`는 타입 시그니처를 간결화해 제네릭 소음을 줄인다.
3. `self`만 두면 `proto`/제약 시그니처가 장황해지고 DX가 악화된다.

예시:

```parus
proto Cloneable {
  def clone(self) -> Self;
}

acts for Vec2 {
  def len2(self) -> i32 { return self.x * self.x + self.y * self.y; }
}
```

---

## 6. `acts` 모델 (v0 고정)

### 6.1 세 가지 선언 형태

1. `acts Name { ... }` (일반 네임스페이스 acts)
2. `acts for T { ... }` (default acts)
3. `acts Name for T { ... }` (named acts)

### 6.2 부착 대상 제한

1. 허용: `field`, `class`
2. 금지: `actor`, `proto`
3. 이유:
   - `actor`: commit/recast 상태머신 불변식 보호
   - `proto`: 계약과 행동 정책의 역할 분리 유지

### 6.3 선택 문법 (`with`)

```parus
use Vec2 with acts(FastMath);
use Vec2 with acts(default);

set mut v = Vec2{ x: 42, y: 42 } with acts(FastMath);
```

규칙:

1. lexical scope 기반으로 작동한다.
2. 파일/함수/블록 어디서든 선언 가능하다.
3. 내부 스코프 선언은 외부를 shadowing하며 블록 종료 시 해제된다.
4. 바인딩 sugar(`let/set ... = expr with acts(...)`)는 해당 바인딩의 dot/operator 해소 우선순위에 적용된다.
5. 명시 경로 선택도 허용한다:
   - `Vec2::acts(FastMath)::add(v, 1, 2)`
   - `Vec2::acts(default)::add(v, 1, 2)`
6. alias 정합:
   - `use Vec2 as v2; v2::acts(FastMath)::add(...)`가 유효해야 한다.
   - `use acts(Math) as m; m::add(...)`가 유효해야 한다.

### 6.4 해소 순서와 충돌 규칙

dot/연산자 해소 순서:

1. inherent method
2. named acts
3. default acts

강제 규칙:

1. 동일 시그니처 다중 후보는 자동 선택 금지, 컴파일 에러
2. named/default 동일 시그니처 중복은 컴파일 에러
3. inherent vs acts 동일 시그니처 중복도 컴파일 에러(가림 허용 안 함)
4. `acts for T`/`acts Name for T` 함수는 첫 파라미터 `self` 계열이 필수다.
5. 일반 `acts Name {}` 함수는 `self`를 가질 수 없다(정적 경로 호출 전용).

정적 경로 호출:

1. 타입 부착 acts 함수: `Vec2::acts(FastMath)::add(v, 1, 2)`
2. 일반 acts 네임스페이스 함수: `acts(Math)::add(1, 2)`
3. 일반 acts alias: `use acts(Math) as m; m::add(1, 2)`

### 6.5 연산자 규칙

1. 연산자 선언은 `operator(...)` 문법만 허용
2. 연산자는 `acts for T`/`acts Name for T`에서만 선언
3. `proto`/`actor`에 연산자 재정의 금지

### 6.6 상속/다형성 금지

1. `acts` 사이의 상속 금지
2. `acts` 다형성/런타임 선택 금지
3. `acts`는 정적 정책 선택 계층으로 고정

---

## 7. `proto` 모델 (v0 + v1+ 확장 방향)

### 7.1 v0 규칙

1. `proto`는 계약 시그니처 집합이다.
2. 구현 본문 금지
3. 연산자 선언 금지
4. 저장 필드 선언 금지(v0 단순화; 접근 계약은 함수로 표현)
5. `proto` 상속 허용 (`proto B : A { ... }`)
6. 구현 권한은 v0에서 `class`에 한정한다.
7. `dyn` 미도입 상태에서는 정적 검증/정적 해소를 우선한다.

예시:

```parus
proto Drawable {
  def draw(self, ctx: &mut RenderCtx) -> void;
}
```

```parus
proto Hashable {
  def hash(self) -> u64;
}

proto Equatable {
  def eq(self, b: &Self) -> bool;
}

proto Keyable : Hashable, Equatable {
  def key_id(self) -> u64;
}
```

### 7.2 v1+ 방향

1. 제네릭 제약의 중심으로 확장
2. 제약 표기는 `with [ ... ]` 단일 문법으로 고정
3. 구현 권한 확장 후보: `field`의 제한적 `proto` 구현 허용
4. 런타임 다형성(`dyn`)은 명시 opt-in으로 분리

예시(미래 문법 초안):

```parus
def render<T>(x: &T) with [T: Drawable] -> void {
  x.draw(ctx: ...);
}
```

### 7.3 `dyn` 통합 방향(v1+)

1. 기본 경로는 계속 정적 디스패치(`proto` 제약 + 제네릭)다.
2. `dyn Proto`는 런타임 다형성 경계에서만 명시적으로 사용한다.
3. `dyn` 경계는 간접 호출 비용이 있으므로 API 표면에서 타입으로 드러나야 한다.
4. `Self` 반환 같은 시그니처는 `dyn` 경계에서 별도 제약/금지 규칙을 둔다.

---

## 8. `class` 모델 (구현체 + 생명주기)

### 8.1 역할

1. `proto` 구현의 기본 구현체 타입
2. 상태/메서드/수명주기 보유
3. 필요한 경우 `acts for TabletType` 부착 가능

### 8.2 `init`/`deinit` 규칙

표면 문법(정본):

1. 생성자: `def init(...) -> void`
2. 소멸자: `def deinit() -> void`

내부 구현 전환 규칙:

1. 컴파일러 내부 lowering은 기존 `construct`/`destruct`와 동등 의미로 처리할 수 있다.
2. 전환기 동안 `construct`/`destruct`는 호환 별칭으로 취급 가능하나, 문서/예제는 `init`/`deinit`를 기준으로 한다.

예시:

```parus
class Sprite : Drawable {
  def init(tex: Handle<Texture>, pos: Vec2) -> void { ... }
  def deinit() -> void { ... }
  def draw(self, ctx: &mut RenderCtx) -> void { ... }
}
```

### 8.3 상속 규칙

1. v0에서 `class`은 `proto` 구현만 허용한다.
2. `class` 간 레이아웃 상속은 v1+ 보류.

---

## 9. `actor` 모델 (상태머신 보호)

1. `actor`는 대규모 공유 상태/동시성 제어의 핵심이다.
2. 행동은 inherent method(`pub`/`sub`)로만 제공한다.
3. `acts for ClassType`는 금지한다.
4. 이유: 외부 행동 주입이 `commit`/`recast` 불변식을 손상시킬 수 있기 때문.

예시:

```parus
actor Scene {
  pub def tick(dt: f32) -> void {
    // draft update
    commit;
  }

  sub def snapshot() -> SceneSnap {
    recast;
    ...
  }
}
```

---

## 10. 포함(Composition) 규칙

1. `field` 안에 `field` 포함: 허용
2. `class` 안에 `field` 포함: 허용
3. `actor` draft에 `field` 포함: 허용(기존 draft 규칙 준수)
4. `proto` 안의 저장 필드: v0 금지 (함수 계약으로 대체)

권장:

1. `actor` draft는 복잡 객체 값 대신 handle 중심으로 구성
2. 공유 상태 경계에서는 단순 레코드 + handle 패턴 유지

---

## 11. `with` 키워드 사용 정책

### 11.1 v0

1. `with`는 acts 선택에 사용한다.
2. 문법: `use T with acts(NameOrDefault);`

### 11.2 v1+

1. 신규 키워드 추가 없이 제네릭 제약에 재사용한다.
2. 단일 표기: `def f<T>(x: &T) with [T: Drawable] -> ...`
3. `with T: P`(브래킷 없는 with)와 `[]` 단독 제약 절은 허용하지 않는다.
4. 제약이 1개여도 반드시 `with [T: P]`를 사용한다.

---

## 12. `<>` 과밀 방지 지침

1. 타입 파라미터 선언과 제약을 분리한다.
2. 제약은 `with [ ... ]` 절로 고정한다.
3. 호출부에서 명시 `<...>`는 추론이 실패할 때만 쓰도록 설계한다.
4. API 설계 시 반환/인자에서 추론 가능한 형태를 우선한다.

비권장:

```parus
def f<T: P<T>, U: Q<T, U>, V: R<U, V>>(...) -> ...
```

권장:

```parus
def f<T, U, V>(...) with [T: P, U: Q<T>, V: R<U>] -> ...
```

---

## 13. Canonical OOP 작성 패턴

### 13.1 값 타입 + 연산

```parus
field Vec2 {
  x: i32;
  y: i32;
}

acts for Vec2 {
  operator(+)(self, rhs: Self) -> Self {
    return Vec2{ x: self.x + rhs.x, y: self.y + rhs.y };
  }
}
```

### 13.2 인터페이스 + 구현체

```parus
proto Drawable {
  def draw(self, ctx: &mut RenderCtx) -> void;
}

class Sprite : Drawable {
  def init(pos: Vec2) -> void { ... }
  def deinit() -> void { ... }
  def draw(self, ctx: &mut RenderCtx) -> void { ... }
}
```

### 13.3 공유 상태 오케스트레이션

```parus
actor Scene {
  pub def add_sprite(h: Handle<Sprite>) -> void {
    // draft에 handle 저장
    commit;
  }

  sub def read_stats() -> SceneStats {
    recast;
    ...
  }
}
```

---

## 14. 구현 체크리스트

다음을 통과해야 본 OOP 모델 준수로 본다.

1. `acts for` 부착 대상 제한(`field`/`class`)이 강제된다.
2. `actor`에 `acts for`를 시도하면 컴파일 에러가 난다.
3. `proto`에서 연산자 선언이 금지된다.
4. `proto` 상속과 구현 요구사항 closure 검사가 일관 동작한다.
5. `self`/`Self` 규칙이 타입체커에서 일관 강제된다.
6. `use T with acts(...)` lexical scope 선택이 일관 동작한다.
7. dot/연산자 해석에서 모호성이 런타임이 아닌 컴파일 단계에서 제거된다.
8. `class` 생명주기 표면 규칙(`init`/`deinit`)이 문서와 구현 계획에서 일치한다.

---

## 15. 변경 이력

### v0.0.1

1. OOP 역할 분리(`field`/`acts`/`proto`/`class`/`actor`) 정본 고정
2. `acts` 최소 모델(v0) + `proto` 중심 확장(v1+) 로드맵 고정
3. `self`와 `Self`의 문맥 의미를 정식 규칙으로 고정
4. `with`의 v0(acts 선택) / v1+(제약) 사용 원칙 고정
5. `init`/`deinit` 표면 규칙과 내부 전환 원칙(construct/destruct 동등 의미) 정의
