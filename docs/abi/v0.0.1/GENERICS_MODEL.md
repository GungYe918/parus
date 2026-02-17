# Parus Generics Model (Adopted Scope)

문서 버전: `v0.0.1`  
상태: `Adopted Scope (v1+ Target, v0 Documentation Baseline)`

이 문서는 Parus 제네릭/제약 표기에서 현재 합의된 범위만 기록한다.  
세부 타입 추론/실체화 알고리즘은 후속 문서에서 확정한다.

충돌 우선순위:

1. OOP/다형성 의미 충돌 시 `docs/abi/v0.0.1/OOP_MODEL.md` 우선
2. ABI 경계 의미 충돌 시 `docs/abi/v0.0.1/ABI.md` 우선
3. 일반 언어 표면 문법 충돌 시 `docs/spec_v0.md` 갱신을 통해 정합화

---

## 1. 목표

1. 제네릭 제약 표기를 직관적으로 유지한다.
2. `<>` 과밀을 억제하면서 표현력을 확보한다.
3. `proto`를 capability 계약의 중심으로 사용한다.
4. 기본 경로는 정적 디스패치(모노모피제이션)로 유지한다.

---

## 2. 표기 규칙 (합의안)

### 2.1 단일 표기: `with [ ... ]` 제약 절

```parus
def contains<T>(xs: &[T], x: &T) with [T: Hashable, T: Equatable] -> bool { ... }
```

### 2.2 강제 규칙

1. 제약 절은 `with [ConstraintList]` 형태만 허용한다.
2. `[]` 단독 제약 절은 사용하지 않는다.
3. `with T: P, U: Q` 형태(브래킷 없는 with)는 사용하지 않는다.
4. 제약이 1개여도 `with [T: P]`처럼 브래킷을 유지한다.

---

## 3. 왜 `with [ ... ]`를 단일화하는가

1. 반환 화살표 `->`가 함수 시그니처에 한 번만 등장해 가독성이 좋다.
2. 제약 집합이 시각적으로 분리되어 스캔 속도가 높다.
3. Rust식 복잡한 `<>` 중첩을 완화하기 쉽다.
4. "제약 시작 토큰(with) + 제약 범위 토큰([])"를 동시에 고정해 파서/포매터/LSP 규칙을 단순화한다.

---

## 4. `proto`와의 결합

1. 제네릭 제약의 canonical 단위는 `proto`다.
2. `proto` 상속 관계를 제약 검사에 반영한다.
3. 함수 제약은 "필요 capability"만 표현하고, 구현 전략(`acts`, inherent)은 해소 단계가 결정한다.

예시:

```parus
proto Drawable {
  def draw(self d: &Self, ctx: &mut RenderCtx) -> void;
}

def render_all<T>(items: &[T], ctx: &mut RenderCtx) with [T: Drawable] -> void {
  ...
}
```

---

## 5. 디스패치 및 성능 원칙

1. 기본은 정적 디스패치다.
2. 제네릭 함수는 기본적으로 모노모피제이션 대상으로 본다.
3. 런타임 동적 다형성은 `dyn` 도입 이후 명시 opt-in 경로로 분리한다.

---

## 6. `dyn`과의 관계 (v1+ 준비 규칙)

1. `dyn Proto`는 정적 제네릭과 별도 경계로 취급한다.
2. `dyn`은 간접 호출 비용이 있으므로 타입 표면에서 명시되어야 한다.
3. 기본 API는 정적 제약(`with [ ... ]`)을 우선하고, 필요할 때만 `dyn`을 선택한다.

---

## 7. 설계 지침 (`<>` 과밀 방지)

1. 타입 파라미터 선언과 제약을 분리한다.
2. 제약은 `with [ ... ]` 절에 모은다.
3. 호출부 `<...>` 명시는 추론 실패 시에만 요구한다.
4. 제약 이름은 capability 중심(`Drawable`, `Hashable`)으로 짧고 명확하게 짓는다.

비권장:

```parus
def f<T: P<T>, U: Q<T, U>, V: R<U, V>>(...) -> ...
```

권장:

```parus
def f<T, U, V>(...) with [T: P, U: Q<T>, V: R<U>] -> ...
```

---

## 8. 현재 비범위

1. 제약 만족도 추론의 상세 알고리즘
2. 특수화 우선순위/부분 특수화 규칙
3. dyn trait object ABI 상세
4. coherence/orphan-like 규칙의 최종 고정

---

## 9. 변경 이력

### v0.0.1

1. 제약 표기를 `with [ ... ]` 단일 문법으로 고정
2. `[]` 단독/브래킷 없는 `with` 표기는 비권장 수준이 아니라 정본 범위에서 제외
3. `proto` 중심 capability 제약 모델을 제네릭 정본 방향으로 고정
4. 정적 디스패치 우선 + `dyn` 명시 opt-in 분리 원칙 확정
