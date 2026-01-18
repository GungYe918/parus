# Gaupel v0 설계 초안 Draft (통합 레퍼런스, 최종 개정)

> 본 문서는 **Gaupel v0의 단일 레퍼런스 초안(확정안)**이다.
> 목적: 토크나이저, 프리패스, 파서, 타입체커의 v0 구현이 가능하도록 **철학 + 정적 규칙 + 문법 + 예시**를 포함한다.
> 언어명: **Gaupel** (확정)

---

## 0. 설계 목표와 철학

### 0.1 핵심 목표

* **SMP + SIMD 친화**: 병렬 처리/벡터화가 쉬운 데이터/제어 구조 제공.
* **명시적 코드 플로우**: 암묵 복사/암묵 삽입을 최소화하고, 비용/흐름이 코드에 드러나도록 설계.
* **대단위 상태 공유 생산성**: Rust의 단일 소유권이 UI/게임/브라우저에서 유발하는 “빌림-복사 폭증”을 완화.
* **순환참조 문제 실질적 제거**: 포인터로 객체 그래프를 만들지 않고, 표준 라이브러리의 Handle+Store 기반 그래프/ECS를 기본 패턴으로 제공.

### 0.2 “작은 데이터 vs 큰 상태” 이원 모델

* **작은 데이터 전달/공유**: Rust식 move/borrow 철학을 따른다. (0비용 정적 검사, 런타임 카운터 없음)
* **큰 공유 컨텍스트(상태 폭발 영역)**: `class`의 Draft + `pub/sub` 메서드 모델을 사용한다.

  * `sub`: Draft 스냅샷 읽기(상태 변경 금지)
  * `pub`: Draft 초안 쓰기 + `commit` 강제(명시적 발행)

### 0.3 객체지향은 `proto → tablet`

* C++식 “클래스=객체지향”을 채택하지 않는다.
* **proto**는 계약/프로토타입(인터페이스), **tablet**은 구현 테이블(구현체)이다.
* 단, proto 내부에 `class`를 포함하는 것은 허용한다(계약에 상태+권한 모델 포함 가능).

---

## 1. 토큰/기본 규칙

### 1.1 식별자/구문 기본

* 식별자: `[A-Za-z_][A-Za-z0-9_]*`
* 세미콜론: 모든 문장 끝에 필수.
* 스코프: 들여쓰기 무관, 반드시 `{}` 사용.
* 공백/개행: 토큰 분리 외 의미 없음.

### 1.2 주석

* 라인 주석: `// ...`
* 블록 주석: `/* ... */` (중첩 불가)

### 1.3 리터럴

* 정수: `123`, `1_000_000`

  * 접미사: `u | i8 | i16 | i32 | i64 | u8 | u16 | u32 | u64`
* 실수: 접미사 필수

  * `f`(float32), `lf`(float64)
* 불리언: `true | false`
* null: `null`
* 문자열:

  * 기본: `"..."`
  * F-string: `F"""...{expr}..."""` (`\{`, `\}`로 이스케이프)
  * Raw: `R"""..."""` (보간 없음)

> 숫자 구분자 `_`는 숫자 그룹 사이에만 허용(선두/말미/연속 금지).

---

## 2. 프리패스(전처리)와 모듈/FFI

### 2.1 프리패스 `#define` (파일 단위 문자열 치환)

```gaupel
#define GAME_NAME "Gaupel"
#define VER "0.1"
```

* 토큰화 이전에 원문 텍스트 치환(문자열 치환 전용).
* 스코프: 파일 단위.
* 재정의 금지: 동일 NAME 재정의는 에러.
* 함수형 매크로/인자 매크로 없음(v0).

### 2.2 모듈 임포트(절대/상대 경로 규칙)

```gaupel
embed module <engine/core> as core;          // 절대 경로(컴파일 타임 path에 등록되어 있어야 함)
embed module "../engine/core" as core2;      // 상대 경로(현재 파일 기준)
```

규칙:

* `embed module <path> as alias;`

  * `<...>`는 **절대 경로 모듈명**. 빌드 시스템/컴파일러의 모듈 검색 경로에 등록되어 있어야 한다.
* `embed module "..." as alias;`

  * `"../..."`는 **상대 경로**. 파일 시스템 기반으로 해석한다.
* alias는 필수로 권장(충돌 방지). alias 누락은 v0에서 금지로 두어도 된다(구현 단순화).

심볼 접근 규칙(v0 기본):

* 모듈 공개 심볼은 `alias::Name`으로 접근한다.

  * 예: `core::init();`, `core::Sprite`

### 2.3 FFI는 `embed ...::ffi`로만 (언어명 비노출)

```gaupel
embed func::ffi<int (int, int)> c_add;
embed struct::ffi Vec2 { float32 x; float32 y; }
```

규칙:

* `extern` 키워드는 제공하지 않는다.
* FFI는 항상 `embed`로 명시적으로 가져온다.
* FFI는 ABI 경계이므로 `pure`/`comptime`에서 기본적으로 금지된다(§4.2).

---

## 3. 타입 시스템

### 3.1 원시 타입(고정 크기)

* 정수: `int8 int16 int32 int64`, `uint8 uint16 uint32 uint64`
* 부동: `float32 float64`
* `bool`, `string`
* 별칭(구현 선택): `int = int32`, `float = float32`

> `any`는 **삭제(미지원)**.

### 3.2 null (정규화/암묵 변환 없음)

* `null`은 **그 자체로만** 존재한다.
* 숫자/불리언 등으로 자동 변환하지 않는다.
* `null`을 사용할 수 있는 타입은 **명시적으로 nullable**이어야 한다.

nullable 표기(v0):

* `T?` : nullable 타입

  * 예: `string?`, `Vec2?`, `Handle<Node>?`, `ref<Texture>?`

정적 규칙:

* `T`(non-nullable)에 `null` 대입/반환/인자 전달 → 컴파일 에러
* `T?`에만 `null` 허용
* `if (x == null)` 비교는 `T?`에서만 허용

> 참고: Rust의 `&T`가 non-null인 것처럼, **Gaupel의 borrow 참조(`&T`, `&mut T`)는 항상 non-null**이다.
> nullable 참조가 필요하면 `ref<T>?` 또는 `Handle<T>?`를 사용한다.

### 3.3 배열/리스트

* 고정 배열: `T[N]`
* 가변 리스트: `T[]`

---

## 4. 함수 선언(확정 형태) + qualifier

### 4.1 선언 형식(확정)

* 반환 타입은 **반드시 `@returns`로 명시**한다(강제).
* `fn` 뒤에 qualifier를 나열한다(파서 단순화).
* 파라미터 선언은 `name: Type`만 사용한다.
* 호출은 위치 인자 또는 라벨 인자 중 하나만 사용(혼합 금지).

형식:

```gaupel
@returns Type
fn [qualifier...]* Name(p1: Type, p2: Type, ...) [ : Mode ] {
  ...
}
```

### 4.2 qualifier (fn 뒤)

* `pure`: 부수효과 금지(정적 규칙 기반).
* `comptime`: 컴파일 타임 호출 전용(런타임 호출 금지).

`pure` 정적 규칙(v0 보수적 정의):

* 금지:

  * `global` 쓰기
  * `commit`
  * `pub` 호출
  * `embed ...::ffi` 호출(기본 금지)
  * I/O 성격 표준 라이브러리 호출(효과 함수)
* 허용:

  * 지역 계산
  * 불변 데이터 읽기
  * `sub` 호출(단, sub가 Draft 읽기만 한다는 조건)

`comptime` 정적 규칙(v0):

* 런타임 호출 금지(컴파일 에러).
* 내부적으로 `pure`에 준하는 제약이 적용된다(FFI/I/O/전역 변경 금지).

### 4.3 Mode (권한 모드)

* `: sub` / `: pub`는 **오직 `class {}` 내부에서만** 허용.
* 일반 함수에는 `: sub/: pub`를 사용할 수 없음(컴파일 에러).

### 4.4 라벨 인자 호출 규칙(확정)

* 위치 호출만 쓰거나, 라벨 호출만 쓴다.
* 혼합 호출 금지:

  * `f(1, b: 2)` → 컴파일 에러

---

## 5. 변수/가변성/소유권: Rust 규칙 그대로 + Gaupel의 `ref`

> v0에서 **borrow 규칙은 Rust와 동일한 의미론**을 따른다.
> 즉, “런타임 카운터(RefCell 같은 것)” 없이 **정적(컴파일타임) borrow checker**로 처리한다.
> (구현 기법은 NLL 포함 Rust식 데이터플로우 분석을 사용한다.)

### 5.1 바인딩과 `mut`

* 변수는 기본 불변.
* `mut` 바인딩만 재대입/수정 가능.

```gaupel
int x = 1;
x = 2;          // 에러

mut int y = 1;
y = 2;          // OK
```

`set`(추론)도 동일:

```gaupel
set a = 10;
mut set b = 10;
```

### 5.2 Move가 기본, Copy는 명시

* 값 전달/대입은 기본적으로 **move(소유권 이동)**이다.
* 암묵 복사는 없다.
* 복사는 `copy expr`로만 가능.

Copy 가능한 타입(v0 기본):

* 원시 수치/불리언
* `func<...>` 함수 포인터
* `Handle<T>`(표준 핸들)
* `field`(조건 충족 시)
* 고정 배열 `T[N]` (T가 Copy일 때)

```gaupel
mut int a = 1;
set b = copy a;      // OK
```

### 5.3 Borrow: `&` / `&mut` (Rust 동일 의미론)

* `&x` : 불변 borrow → 타입 `&T`
* `&mut x` : 가변 borrow → 타입 `&mut T` (x는 `mut`여야 함)

정적 규칙(= Rust):

* **공유 불변 참조는 여러 개 가능**
* **가변 참조는 동시에 하나만 가능**
* 불변 참조가 살아있으면 가변 참조 불가, 가변 참조가 살아있으면 어떤 참조도 불가
* 수명(lifetime)은 컴파일러가 추론하며, Rust의 NLL처럼 “마지막 사용 지점”까지로 단축될 수 있다.
* 참조는 non-null이며 `null`이 될 수 없다.

> 제한(실용적 v0):
>
> * `field` 내부에는 `&T`, `&mut T`를 넣을 수 없다(POD 제약).
> * 전역에 `&T`를 두는 것은 금지(수명/초기화 복잡도 방지).
> * 함수 반환 타입으로 `&T`는 허용하되, Rust의 elision 규칙처럼 “입력 참조에 묶인 반환”만 허용하는 보수 모드로 시작해도 된다.

예:

```gaupel
mut int x = 1;
set r = &x;          // &int
set m = &mut x;      // 에러: r이 살아있으면 불가
```

### 5.4 Gaupel `ref<T>` (간단 공유용 우회 경로)

Rust는 “간단히 공유하고 싶을 때도” 수명/소유권 제약 때문에 설계가 꼬일 수 있다.
Gaupel는 이를 완화하기 위해 **`ref<T>`를 제공**한다.

#### 5.4.1 ref의 정의

* `ref<T>`는 **저장/복사/전달 가능한 안전 참조 핸들**이다.
* `ref<T>`는 **borrow(`&T`)가 아니다.**
* `ref<T>`는 보통 표준 라이브러리의 저장소(예: ResourceStore, GraphStore 등)에서 얻는다.
* `ref<T>` 자체는 Copy(가벼운 핸들)로 취급하는 것을 기본으로 한다.

#### 5.4.2 ref의 접근 방식(명시적)

* `ref<T>`에서 값을 읽을 때는 명시적으로 `get()`을 호출해 `&T`를 얻는다.

  * 예: `set t = tex.get();` (`t`는 `&Texture`)
* `ref<T>`는 기본적으로 **읽기 공유**를 위한 도구다.
* **변경 가능한 공유 상태**는 `class Draft + pub/sub` 또는 ECS/Graph의 커밋 모델로 처리한다.

> 설계 의도: “공유하고 싶어서 borrow 지옥/복사 폭증으로 가는 경로”를 `ref`로 우회하고,
> 실제 변경은 `pub/commit`에서만 일어나게 하여 SMP에서 안정성을 확보한다.

---

## 6. class + Draft + pub/sub (확정)

### 6.1 class의 정의

* `class`는 “대단위 공유 컨텍스트” 단위.
* `pub/sub` 함수는 **무조건 class 안에서만 선언 가능**.
* `pub/sub`가 없는 함수는 Draft 접근 불가.

### 6.2 Draft 의미

* `Draft`는 해당 class가 보유하는 컨텍스트 상태.
* `sub/pub` 함수에서만 Draft 접근 가능.

### 6.3 sub 함수 규칙(정적)

* Draft 읽기만 가능.
* Draft 필드 대입/가변 호출 금지.
* 함수 시작 시 받은 스냅샷을 신뢰.
* 필요 시 `refresh;`로 명시적 갱신(선택).

### 6.4 pub 함수 규칙(정적, **최종 확정**)

* pub는 Draft 수정 가능.
* **`commit;`은 최소 1회 “최종 커밋”으로 보장되어야 한다.**
* 타입체커의 commit 검사 규칙은 아래와 같다:

#### 6.4.1 “최종 commit” 규칙(핵심)

* pub 함수 본문(함수의 최상위 블록)에서,

  * **최상위 레벨에 있는 `commit;`만** 최종 commit 후보로 인정한다.
  * `if/elif/else`, `switch`, `while`, `loop`, 중괄호로 중첩된 블록 내부의 `commit;`은
    **최종 commit 검사에서 무조건 무시**한다. (있어도 “최종 commit”으로 카운트하지 않음)

* 최종 commit은 다음 조건을 만족해야 한다:

  1. 함수 본문 최상위 블록에서 실행 흐름이 도달 가능한 위치에 존재
  2. **최상위 블록의 마지막 유효 문장**이 `commit;` 이어야 한다
     (즉, commit 뒤에 같은 레벨의 문장이 존재하면 최종 commit이 아님)

이 규칙은 “분기 안의 commit은 최종 commit이 아닐 수 있다”는 요구를 만족하며,
C에서 `int` 함수가 “마지막에 확실히 return이 있어야 한다”는 스타일 강제와 유사한 강제성을 제공한다.

#### 6.4.2 추가 제약(v0 권장)

* pub 함수는 `@returns void`를 강하게 권장한다.
  (최종 commit을 “마지막 문장”으로 강제하면, `return`과 동시 사용이 어색해지므로 v0에서는 pub를 커밋 종료형으로 쓰는 것이 안정적이다.)
* pub 함수 내부의 `return`은 v0에서 금지로 두어도 된다(구현 단순화).

---

## 7. proto → tablet 객체지향(확정)

* proto: 계약/인터페이스 정의.
* tablet: 구현 테이블(구현체).
* proto 내부에 class 포함 가능.

---

## 8. 람다/콜백 (전역 람다 금지 유지)

### 8.1 람다 문법(함수 스코프 전용)

```gaupel
[captures] (params...) { body }
```

* capture-item: `name` | `mut name`
* 타입:

  * 캡처 없는 함수 포인터: `func<Ret(Args...)>`
  * 캡처 가능한 클로저 값: `closure<Ret(Args...)>` (move-only 권장)

### 8.2 전역 람다 금지(확정)

* 람다는 함수 스코프 내부에서만 생성/존재 가능.
* 전역에 `closure` 값을 둘 수 없음.
* 전역 콜백은 `func<...>`만 허용하거나, class Draft 슬롯에 저장(pub로 설정).

---

## 9. 제어 흐름: if / switch / while / loop(iter)

### 9.1 if

```gaupel
if (cond) { ... } elif (cond) { ... } else { ... }
```

* `cond`는 반드시 `bool`.

### 9.2 switch

```gaupel
switch (expr) {
  case 1: { ... }
  default: { ... }
}
```

* fallthrough 금지.
* 라벨은 리터럴(Int/String/Bool)만 허용.

### 9.3 while (분리 확정)

```gaupel
while (cond) { ... }
```

* `cond`는 반드시 `bool` (정수 truthy 없음).

### 9.4 loop(iter: ...) (반복자 루프 확정)

기존 `loop(while:)`, `loop(until:)`는 **삭제**.

대신 반복자/컨테이너 반복은 다음 한 형태로 통일:

```gaupel
loop(iter: i in IterableExpr) { Body }
```

`IterableExpr`:

* 고정 배열 `T[N]`
* 가변 리스트 `T[]`
* 범위 `a..b`, `a..:b`
* 표준 라이브러리 그래프/ECS iterator(핸들 기반)

---

## 10. field + SIMD 힌트

### 10.1 field 정의

* `field`는 POD 데이터 블록 정의용 구조체.
* 내부에는 **POD 값** 또는 **함수 포인터(`func<...>`)**만 포함 가능.

허용(기본):

* 수치/불리언
* 고정 배열 `T[N]`(T가 POD)
* 다른 `field`
* `func<...>`
* `Handle<T>`

금지(v0):

* `string`
* `T[]` 가변 리스트
* `class` 인스턴스
* borrow 참조 `&T`, `&mut T`
* `closure<...>` (전역/저장 복잡도 방지)

예:

```gaupel
@with_simd
field Particle {
  float32 x; float32 y;
  float32 vx; float32 vy;
  func<void (int)> onHit;
}
```

---

## 11. 연산자 및 표현식

### 11.1 기본 연산자

* 산술: `+ - * / %`
* 비교: `== != < <= > >=`
* 논리: `&& || ! ^` 및 `and or not xor`
* 대입: `= += -= *= /= %=`
* 인덱싱: `[]`
* 삼항: `cond ? a : b` (중첩 금지)
* 범위: `a..b`(미만), `a..:b`(이하)

### 11.2 파이프 연산자 `<<` + `_` 자리표시자(확정)

`<<`는 **파이프(값 주입) 연산자**.

형식:

```gaupel
lhs << f(a: _, b: 2);
```

정적 규칙:

* `lhs << CallExpr`만 허용.
* CallExpr 내부 `_`는 **정확히 1개**.
* `_`는 **라벨 인자 자리에서만** 허용(`f(_,2)` 금지).
* 변환:

  * `lhs << f(a: _, b: 2)` ≡ `f(a: lhs, b: 2)`

체이닝:

```gaupel
set r = 1 << add(a: _, b: 2) << mul(x: _, y: 10);
```

---

## 12. 표준 라이브러리(강권): Graph + ECS

* 객체 그래프/상태 구조를 포인터로 엮지 않는다.
* 표준 라이브러리로 제공:

  1. Handle+Store 기반 객체 그래프
  2. 범용 ECS(World + Query + Command/Commit)

핵심:

* 사이클은 “그래프 사이클”로 존재 가능하지만, 메모리 누수 사이클(RC cycle)로 이어지지 않게 설계한다.

---

## 13. 심볼/ABI/디버깅 규칙

* 오버로딩 금지.
* 심볼은 가능한 한 원 이름이 보이게 노출.
* 충돌 방지:

  * 클래스 메서드: `Class__method`
  * 네임스페이스: `ns__sub__name`
* 필요 시 태그/해시를 붙이되 원 이름 유지.

---

## 14. 예시

### 14.1 while + loop(iter) + pipe

```gaupel
@returns int
fn add(a: int, b: int) {
  return a + b;
}

@returns int
fn mul(x: int, y: int) {
  return x * y;
}

@returns void
fn main() {
  mut int p = 0;
  while (p < 3) { p += 1; }

  int[3] arr = [1,2,3];
  loop(iter: x in arr) {
    log(msg: F"""x={x}""");
  }

  set r = 1 << add(a: _, b: 2) << mul(x: _, y: 10);
  log(msg: F"""r={r}""");
}
```

### 14.2 pub/sub + 최종 commit 강제

```gaupel
class Game {
  @returns int
  fn score() : sub {
    return Draft.score;
  }

  @returns void
  fn addScore(delta: int) : pub {
    if (delta < 0) {
      // commit이 있어도 "최종 commit 검사"에서는 무시됨(분기 내부)
      // commit;  // (허용 여부는 구현 선택: 허용하되 경고 추천)
    }

    Draft.score += delta;

    // 최종 commit: 함수 최상위 블록의 마지막 문장이어야 함
    commit;
  }
}
```

### 14.3 borrow(Rust) + ref(공유 우회)

```gaupel
@returns void
fn demo() {
  mut int x = 1;
  set r = &x;
  // set m = &mut x; // 에러(= Rust 규칙)

  // ref는 표준 저장소에서 받는다고 가정
  // set tex: ref<Texture> = Assets::getTexture(id: 3);
  // set t = tex.get(); // t: &Texture  (명시적)
}
```

---

## 15. 구현 체크리스트(v0, 확정)

### 15.1 프리패스

* `#define` 치환(파일 단위, 재정의 금지)
* 모듈 임포트 해석:

  * `<abs>`는 모듈 검색 경로에서 해석
  * `"rel"`은 파일 기준 경로 해석
* `embed ...::ffi` 수집(FFI 심볼 테이블)
* F-string `{expr}` 분절

### 15.2 파서

* `@returns` 강제
* `fn` 뒤 qualifier 목록 파싱
* `: sub/: pub`는 class 내부에서만 허용
* `while` / `loop(iter: ...)`
* `a..b`, `a..:b`
* `<<` 파이프 + `_` 규칙(정적 진단)

### 15.3 타입체커

* if/while 조건식 bool 강제
* loop(iter)에서 IterableExpr 타입 확인
* 삼항 중첩 금지
* `sub`: Draft 변경 금지
* `pub`: **최종 commit 검사**

  * 최상위 블록 마지막 문장 `commit;` 강제
  * 분기/중첩 내부 commit은 검사에서 무시
* `pure`/`comptime` 금지 목록 검사
* borrow: Rust 규칙 그대로(정적 검사, 런타임 카운터 없음)
* `ref<T>`: 명시적 `get()`로만 `&T` 획득(권장)

---

## 16. (EBNF 초안) 핵심 구문 요약

```ebnf
CompilationUnit = { TopLevelDecl } <eof> ;

TopLevelDecl =
    Preproc
  | ImportDecl
  | EmbedFFIDecl
  | ProtoDecl
  | TabletDecl
  | ClassDecl
  | FuncDecl
  | VarDecl
  ;

Preproc        = DefineDecl ;
DefineDecl     = "#define" Identifier StringLiteral ;

ImportDecl     = "embed" "module" ( AbsModulePath | StringLiteral ) "as" Identifier ";" ;
AbsModulePath  = "<" PathToken ">" ;

EmbedFFIDecl   = "embed" ( FuncFFI | StructFFI ) ;
FuncFFI        = "func::ffi" "<" FfiSig ">" Identifier ";" ;
StructFFI      = "struct::ffi" Identifier "{" FieldList "}" ;

ReturnsDecl    = "@returns" Type ;
FuncDecl       = ReturnsDecl "fn" { Qualifier } Identifier "(" [ ParamList ] ")" [ Mode ] Block ;
Qualifier      = "pure" | "comptime" ;
Mode           = ":" ( "sub" | "pub" ) ;  // class 내부에서만 허용(정적 규칙)

ParamList      = Param { "," Param } ;
Param          = [ "mut" ] Identifier ":" Type [ "=" Expr ] ;

ClassDecl      = "class" Identifier "{" { ClassMember } "}" ;
ClassMember    = FuncDecl | VarDecl | ... ;

Statement      =
    VarDecl | IfStmt | SwitchStmt
  | WhileStmt | LoopIterStmt
  | ExprStmt | ReturnStmt | BreakStmt | ContinueStmt
  | CommitStmt | RefreshStmt
  ;

WhileStmt      = "while" "(" Expr ")" Block ;
LoopIterStmt   = "loop" "(" "iter" ":" Identifier "in" IterExpr ")" Block ;

IterExpr       = Expr | RangeExpr ;
RangeExpr      = Expr ".." [ ":" ] Expr ;

CommitStmt     = "commit" ";" ;
RefreshStmt    = "refresh" ";" ;

Expr           = PipeExpr ;
PipeExpr       = AssignExpr { "<<" CallWithHole } ;
CallWithHole   = CallExpr ; // 정적 규칙: '_' 정확히 1개 + 라벨 인자 자리에서만

CallExpr       = Primary "(" [ ArgList ] ")" ;
ArgList        = Arg { "," Arg } ;
Arg            = Identifier ":" Expr | "_" ; // '_'는 라벨 자리에서만 허용(정적 규칙)
```
