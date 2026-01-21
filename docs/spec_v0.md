---

# Gaupel v0 Language Specification (Single Reference, Upgraded)

본 문서는 Gaupel v0를 구현하기 위한 단일 레퍼런스이다. 이 문서만으로 토크나이저, 프리패스, 파서, 타입체커 v0를 구현할 수 있도록 설계 철학, 정적 규칙, 문법, 예시 코드를 포함한다.

주의: 이 문서는 한국어로 작성되며, 한국어 외 문자는 ASCII 및 영문자만 사용한다. 단, Gaupel이 UTF-8 친화적이라는 점을 보여주는 예시에서는 임의의 이모지 등을 포함한다.

---

## 0. 설계 목표와 철학

### 0.1 핵심 목표

* SMP + SIMD 친화: 병렬 처리와 벡터화에 유리한 데이터/제어 구조를 제공한다.
* 명시적 코드 플로우: 암묵 복사, 암묵 삽입, 암묵 실행을 최소화해 비용과 흐름이 코드에서 드러나게 한다.
* 대단위 상태 공유 생산성: Rust의 단일 소유권이 유발하는 빌림/복사 폭증을 완화한다.
* 순환참조 문제 실질적 제거: 포인터 기반 객체 그래프 대신 Handle+Store, ECS 패턴을 강권한다.

예시 (철학이 반영된 스타일, 작은 데이터는 값, 큰 상태는 class)

```gaupel
@pure
export fn demo_small_big() -> void {
  set x = 3;
  set y = x + 2;

  // 큰 상태는 class draft + pub/sub + commit으로 관리되는 방향을 지향
}
```

### 0.2 작은 데이터 vs 큰 상태 (이원 모델)

* 작은 데이터 전달/공유: 기본은 move, 필요 시 copy를 명시한다. (borrow/ref는 v0에서 설계 포함, 구현은 단계적으로)
* 큰 공유 컨텍스트: class draft + pub/sub + commit 모델을 사용한다.

  * sub: draft 스냅샷 읽기 전용
  * pub: draft 수정 + commit 강제

예시

```gaupel
class Counter {
  // draft.count 라는 큰 상태를 가진다고 가정

  fn sub get() -> int {
    return draft.count;
  }

  fn pub inc() -> void {
    draft.count += 1;
    commit;
  }
}
```

### 0.3 타입 정의 모델: field, proto, tablet, acts

* field: POD 저장소. 생성자/소멸자 없음. 함수 포함 금지.
* proto: 인터페이스(계약). 생성자/소멸자 없음.
* tablet: 구현체 타입. 생성자/소멸자 존재(기본), = delete 가능. 상속 표기는 C++ 스타일로 통일.
* acts: 특정 타입에 대한 행동(함수, 연산자)을 정의하는 블록. field 내부 함수 금지 대신 acts로 유도.

예시 (개념 맛보기)

```gaupel
export field Vec2 {
  float32 x;
  float32 y;
}

proto Drawable {
  fn draw() -> void;
}

tablet Sprite : Drawable {
  public:
    let pos: Vec2;
    fn draw() -> void { /* ... */ }
}
```

---

## 1. 문자, 토큰, 기본 규칙

### 1.1 공백, 개행, 세미콜론, 블록

* 공백/개행은 토큰 분리 외 의미가 없다.
* 모든 문장은 세미콜론 ; 으로 끝난다. (block 자체는 문장 종결자를 요구하지 않는다)
* 스코프는 반드시 { } 블록을 사용한다. 들여쓰기는 의미 없다.

예시

```gaupel
fn basic_blocks() -> void {
  set a = 1;
  if (a == 1) { set b = 2; }
}
```

### 1.2 주석

* 라인 주석: // ...
* 블록 주석: /* ... */ (중첩 불가)

예시

```gaupel
fn comments() -> void {
  // line comment
  /* block comment */
  set x = 1;
}
```

### 1.3 식별자 (UTF-8 friendly)

Gaupel은 UTF-8 친화적인 토크나이저/파서를 목표로 한다.

v0 권장 규칙:

* 식별자는 Unicode XID_Start / XID_Continue 규칙을 따른다.
* ASCII만 쓰는 프로젝트도 지원한다.
* 키워드와 충돌하는 식별자는 금지한다.

예시 (UTF-8 식별자, 이모지 포함)

```gaupel
fn utf8_identifiers() -> void {
  let 한글이름: int = 3;
  let 🍣: int = 7;
  set 합 = 한글이름 + 🍣;
}
```

### 1.4 키워드 (v0 핵심)

* 선언/구조: field, proto, tablet, acts, class, fn, let, set, mut, &, &&
* 제어: if, elif, else, switch, case, default, while, loop, break, continue, return
* 상탯값: true, false, null
* 논리: and, or, not, xor
* 시스템: use, module, func::ffi, struct::ffi, commit, recast
* 접근 제한: public, private
* 메모리/수명/복사: **copy, clone, delete**
* slice 관련 타입 표기(토큰이 아니라 타입 표기): **[T]**, **&[T]**, **&mut [T]**
* 논리 연산자 토큰 확정: **and, or, not, xor** (기존 `&&`, `||`는 금지/비권장; `&&`는 escape로 고정)

주의: `delete`는 두 의미를 가진다.
(1) tablet 생성자/소멸자 삭제 문법의 `= delete;` (기존 유지)
(2) 아래 8.6에서 정의하는 “명시적 파괴(delete 문장)” 키워드 (신규)
두 용도는 문맥이 달라 충돌하지 않는다.


예시

```gaupel
fn keywords_demo() -> void {
  set ok = true;
  if (ok and not false) { /* ... */ }
}
```

---

## 2. 리터럴

### 2.1 정수 리터럴

* 예: 123, 1_000_000
* 숫자 구분자 _ 는 숫자 그룹 사이에만 허용한다. (선두/말미/연속 금지)
* 접미사: i8 i16 i32 i64 u8 u16 u32 u64 (및 단축 u, i는 금지 권장. v0에서는 명확함을 우선)

예시

```gaupel
fn int_literals() -> void {
  let a: u32 = 123u32;
  let b: u64 = 1_000_000u64;
}
```

### 2.2 실수 리터럴

* 접미사 필수:

  * f: float32
  * lf: float64

예시

```gaupel
fn float_literals() -> void {
  let x: float32 = 3.14f;
  let y: float64 = 2.71828lf;
}
```

### 2.3 불리언, null

* true, false
* null은 그 자체로만 존재하며 자동 변환이 없다.

예시

```gaupel
fn bool_null() -> void {
  let t: bool = true;
  let f: bool = false;
  // let x: int = null; // error
}
```

### 2.4 문자 리터럴 (char)

* 형태: 'a', '\n', '\t', '\', ''', '"'
* 유니코드 코드포인트 표기(권장): '\u{AC00}' 같은 형태를 지원한다.

예시

```gaupel
fn char_literals() -> void {
  let c1: char = 'a';
  let c2: char = '\n';
  let c3: char = '\u{AC00}';
}
```

### 2.5 문자열 리터럴

* 기본 문자열: "..."
* F-string: F"""...{expr}...""" (보간)
* Raw string: R"""...""" (보간 없음)

F-string 이스케이프:

* { 와 } 로 중괄호를 이스케이프한다.

예시

```gaupel
fn strings() -> void {
  let name: string = "gaupel";
  let msg: string = F"""hello {name}""";
  let raw: string = R"""no {interpolation} here""";
}
```

---

## 3. 프리패스와 project/module/file, `use`, FFI

### 3.1 단위 용어: project / module / file

Gaupel 빌드는 컴파일러가 빌드 시스템을 겸한다. 이를 위해 소스 코드를 다음 3단위로 정의한다.

* **file**: 하나의 소스 파일. 파싱 단위이다.
* **module**: 하나의 폴더(또는 논리적 디렉터리) 안의 file 집합. **컴파일 단위**이다.
* **project**: 여러 module의 집합. 최상위 출력(실행파일/라이브러리)과 빌드 설정을 가진다.

핵심 규칙(전방선언 문제 제거):

* Gaupel은 **module 단위로 “선언 수집 prepass”**를 수행한다.
* 따라서 같은 module 내부에서는:

  * 함수/타입/acts 선언 순서가 의미를 갖지 않는다.
  * 파일 A가 파일 B의 “뒤에 선언된” 함수를 전방선언 없이 호출할 수 있다.
* module 외부로 노출되는 심볼은 `export`로만 결정된다.

---

### 3.1.1 `use` 문 (통합: 별칭/상수/타입/함수 경로)

`use`는 다음 기능을 하나의 키워드로 통합한다.

1. **타입 별칭**

```gaupel
use newT = u32;
```

2. **함수/심볼 별칭(경로 별칭)**

```gaupel
use Math::add = add_i32;
use core::io::print = println;
```

3. **파일 스코프 상수/치환(매크로 함수 금지)**

```gaupel
use PI 3.14f;
use GAME_NAME "Gaupel";
```

제약(v0 강제):

* `use`는 **예약어/토큰을 바꾸지 않는다.**
* `use` 치환 대상은 **식별자(IDENT)** 만 가능하다.
* `use NAME expr;` 형태는 “소스 코드 레벨 치환”이며,

  * 함수형 매크로(인자) 금지
  * 토큰 결합/분해 금지
* 구현 권장:

  * (안전) 토큰화 이후 AST 레벨에서 치환
  * (단순) 토큰화 직후 IDENT 토큰만 치환

---

### 3.1.2 `use module <path> as alias;` (모듈 임포트)

문법:

```gaupel
use module <engine/core> as core;
use module <engine/math> as math;
```

규칙:

* `<...>`는 **모듈 경로**이며, project의 모듈 검색 규칙으로 해석한다.
* `alias`는 필수(권장: v0에서는 강제). 모듈 외부 심볼은 `alias::...`로 접근한다.
* `use module`은 include가 아니라 **의존성 선언**이다.

  * 컴파일러는 이를 기반으로 module dependency graph(빌드 그래프)를 구성한다.
* **순환 의존성 금지(v0 권장)**:

  * module A가 B를 `use module`로 의존하면, B가 다시 A에 의존하는 것은 에러.
  * (향후 확장) “인터페이스 전용 모듈” 분리로 완화 가능.

심볼 접근 예시:

```gaupel
export fn sub init() -> void { ... }

export fn pure add(a: int, b: int) -> int {
  return a + b;
}

export fn sub demo() -> void {
  core::init();
  set x = math::add(a: 1, b: 2);
}
```

---

### 3.1.3 `space {}` 네임스페이스

`space`는 “이름 충돌 방지 및 계층화”를 위한 선언 스코프이다.

문법:

```gaupel
export space engine {
  export space math {
    export fn pure add(a: int, b: int) -> int { return a + b; }
  }
}
```

규칙:

* `space` 내부 선언은 경로로 식별된다: `engine::math::add`
* 같은 module 내부에서 `space`는 파일을 넘어 공유된다(모듈 스코프).
* `export space X { ... }`는 “X 아래의 공개 심볼 트리”를 만든다.
* `space`는 타입/함수/상수/acts/tablet/proto/field/class를 포함할 수 있다.

---

### 3.2 모듈 임포트

문법:

* use module <abs/path> as alias;
* use module "../rel/path" as alias;

규칙:

* <...> 는 절대 모듈명이다. 빌드 시스템이 제공하는 모듈 검색 경로에서 해석한다.
* "..." 는 파일 시스템 기반 상대 경로다.
* alias는 v0에서 필수로 권장하며, 구현 단순화를 위해 v0에서는 alias 생략을 금지로 둘 수 있다.

심볼 접근:

* alias::Name 형태

추가 규칙 (컴파일 단위):

* Gaupel에서 "모듈(module)"은 컴파일 단위이다.
* Rust가 crate를 컴파일 단위로 보듯, Gaupel은 "하나의 모듈(그리고 그 모듈이 포함하는 소스 집합)"을 컴파일 단위로 취급한다.
* use module은 이 컴파일 단위를 불러오는 문법이며, 단순 include가 아니다.
* 따라서 모듈 단위로 심볼 테이블, 의존성 그래프, 증분 빌드 캐시, ABI 경계 정책을 적용하기 쉽다.

예시

```gaupel
use module <engine/core> as core;
use module "../engine/math" as math;

fn use_modules() -> void {
  core::init();
  set v = math::add(a: 1, b: 2);
}
```

### 3.3 FFI 임포트

* extern 키워드는 제공하지 않는다.
* FFI는 항상 use로 명시적으로 가져온다.
* FFI는 ABI 경계이므로 pure/comptime에서 기본 금지다. (타입체커 규칙)

예시

```gaupel
use func::ffi<int (int, int)> c_add;
use struct::ffi Vec2C { float32 x; float32 y; }

fn call_c(a: int, b: int) -> int {
  return c_add(a: a, b: b);
}
```

### 3.3.1 FFI-safe 타입

**FFI-safe로 허용되는 타입(v0 권장):**

* 고정 폭 정수/부동소수/bool/char
* `field` 중에서:

  * 멤버가 모두 FFI-safe이고
  * 레이아웃이 C와 호환(정렬/패딩 규칙 고정)인 것
* `T[N]` (T가 FFI-safe)
* 불투명 핸들: `handle<T>`는 C에서 `void*` 또는 `struct Handle_T*`로 취급(불투명)

**FFI에서 금지(v0 강권):**

* `class` 타입
* `tablet` 값 전달(생성자/소멸자/가상 디스패치 등 개입 가능)
* `string`, `T[]`, `closure`
* `&T`, `&mut T` (이미 규칙상 “FFI로 전달 금지”라고 했으니 확정)

즉, FFI 경계에서는:

* “POD 데이터(field)” + “불투명 핸들(handle)”만 통과한다.

### 3.3.2 Gaupel의 개념이 C에서 어떻게 대응되는가

* `field` -> `struct` (C 호환 레이아웃)
* `handle<T>` -> `void*`(또는 `struct Handle_T*`) + 생성/해제 함수 세트
* `tablet` -> C에서 직접 값으로는 못 다루고, 반드시 `handle<tablet>`로만 다룬다
* `class`/`draft`/`pub-sub` -> **FFI로 직접 노출하지 않는다**
  대신:

  * “C가 호출할 수 있는 pub/sub wrapper 함수”를 제공하고,
  * 상태 접근은 wrapper 내부에서만 일어나게 한다.

예:

```gaupel
class Counter {
  fn inc() : pub { draft.count += 1u32; commit; }
}

fn counter_inc_ffi(h: handle<Counter>) -> void {
  // 내부적으로 h의 Counter 인스턴스에 대해 pub 호출
}
```

### 3.3.3 C 말고 다른 언어와의 FFI는 어떻게?

v0 권장 전략은 단순하다:

* **기본 ABI는 C ABI 하나로 통일**
* Rust/C++/Zig/Swift/Python 등은 각자의 방식으로 **C ABI를 소비**하게 한다.

언어별로는 대략 이렇게 된다:

* Rust: `extern "C"` + `#[repr(C)]` + `*mut c_void` 핸들
* C++: `extern "C"` + 포인터 핸들
* Zig: `extern` import + `[*]u8`/`?*anyopaque` 핸들
* Swift: C 헤더 브리징 + `OpaquePointer`/`UnsafeMutableRawPointer`
* Python: CPython extension 또는 ctypes/cffi로 C ABI 소비(핸들 기반)

이렇게 하면 “Gaupel 의미론”을 타 언어로 억지로 이식하지 않고, ABI는 깔끔하게 유지된다.

## 3.4 `-ffreestanding` / `-fno-std` 경계 정의

### 3.4.1 두 모드의 목적

* `-ffreestanding` : OS/호스트 환경을 가정하지 않는다.

  * 표준 라이브러리는 “freestanding 프로파일”로 링크 가능(선택)
  * 원자/스핀락/기본 핸들/Result 정도는 제공 가능
* `-fno-std` : 표준 라이브러리 **전체 미사용**.

  * `core`(컴파일러 내장 최소 모듈)만 사용 가능
  * 런타임 의존 기능은 전부 금지

---

### 3.4.2 예외 기능과의 경계(v0 강제)

예외 메커니즘(`?`, `throw`, `try...catch`, `attempt`)은 런타임 지원이 필요할 수 있으므로 v0에서는 다음처럼 고정한다.

* `-fno-std` 에서는:

  * `? 함수` 선언 자체를 **금지**(또는 선언은 허용하되 본문에서 throw/try/attempt 전부 금지로 시작해도 되지만, v0는 “금지”가 더 단단함)
  * `throw`, `try...catch`, `attempt` 전부 **컴파일 에러**
  * 에러 핸들링은 **Result<T> + switch**만 사용
* `-ffreestanding` 에서는:

  * 표준 라이브러리 freestanding 프로파일이 “EH 지원”을 포함하는 경우에만 `? / throw / attempt` 허용
  * 그렇지 않으면 `-fno-std`와 동일하게 금지

이 경계 덕분에:

* 커널/펌웨어/부트로더 같은 환경은 Result 기반으로 매우 예측 가능한 코드 생성이 가능
* 사용자 앱/툴링 환경은 ? 함수로 더 편한 전파 모델을 쓸 수 있다

---

## 4. 타입 시스템 (v0 정의 + 향후 확장 방향)

### 4.1 원시 타입 (fixed-width)

* 정수: int8 int16 int32 int64, uint8 uint16 uint32 uint64
* 부동: float32 float64
* bool, char, string
* 별칭(구현 선택): int = int32, float = float32

예시

```gaupel
fn primitives() -> void{
  let a: int32 = 1i32;
  let b: uint64 = 2u64;
  let c: bool = true;
  let d: char = 'Z';
  let s: string = "hi";
}
```

### 4.2 nullable 타입 (T?)

* null은 그 자체로만 존재한다.
* nullable은 T? 로 표기한다.
* non-nullable T 에 null을 대입/반환/전달하면 에러다.
* if (x == null) 비교는 T? 에서만 허용한다.

예시

```gaupel
fn nullable_demo() -> void {
  let a: int? = null;
  let b: int? = 3i32;

  if (a == null) {
    // ...
  }
}
```

### 4.3 배열과 리스트

* 고정 배열: T[N]
* 가변 리스트: T[]

예시

```gaupel
fn arrays_lists() -> void {
  let xs: int[3] = [1, 2, 3];
  // let ys: int[] = [1, 2, 3]; // 리스트 리터럴 문법은 v0에서 선택 구현
}
```

### 4.4 타입 시스템 작동 원리 (v0 -> 향후)

v0에서 타입 시스템은 다음 순서로 작동하는 것을 목표로 한다.

1. 파서가 AST를 만든다.
2. 타입체커가 심볼 테이블을 만들고, 선언을 수집한다.
3. 타입체커가 표현식을 하향식/상향식으로 검사하며 타입을 확정한다.
4. 연산자, 호출, 변환 규칙을 적용한다.
5. 오류는 Span 기반 진단으로 보고한다.

향후 확장 (v1+ 로드맵 성격, v0에 포함되는 개념만 선명히):

* 제네릭 타입: TypeName<T> 형태
* 제네릭 함수: fn foo<T>(...) ...
* 제네릭 특수화와 monomorphization (컴파일 시 실체화) 혹은 제한된 형태의 dictionary passing 중 택1
* 타입 추론 강화: set 바인딩의 추론, 간단한 반환 타입 추론 (v0에서는 강제 유지)
* trait 비슷한 개념을 acts/proto/tablet 조합으로 자연스럽게 확장

예시 (향후 제네릭의 목표 형태, v0에서는 파싱만 선행 가능)

```gaupel
// v1+ 목표 예시
// field Vec<T> { T x; T y; }
```

---

## 5. 바인딩과 가변성: let, set, mut

### 5.1 선언은 let/set로만 한다 (v0 강제)

* let: 타입 명시 바인딩
* set: 타입 추론 바인딩
* mut: 바인딩을 가변으로 만든다

규칙:

* v0에서는 "C 스타일 선언" (int x = 1;) 을 허용하지 않는다. (문법 단순화)
* v0에서는 선언 시 초기화를 강권한다. (미초기화는 분석 난이도가 급상승)

예시

```gaupel
fn bindings() -> void{
  let a: int = 1i32;
  set b = a + 2;
  mut set c = 0;
  c = c + 1;
}
```

### 5.2 스코프와 섀도잉

* 같은 스코프에서 동일 이름 재선언은 금지(권장). 구현이 쉬워진다.
* 다른 스코프에서는 허용 가능하다.

예시

```gaupel
fn shadowing() -> void {
  set x = 1;
  if (x == 1) {
    set x = 2; // 다른 스코프, 허용 가능(정책 선택)
  }
}
```

---

## 6. 함수 선언: @attribute, qualifier, 호출 규칙, non-?` / `?` 함수, 예외 허용 범위, 호출 제약

### 6.1 함수 선언 기본형 (v0 확정)

문법:

* @attribute
* [export] fn [mode] [qualifier] Name(params...) -> Type Block

파라미터:

* name: Type
* 기본값은 v0에서 선택 구현. (파서가 쉬워서 넣을 수는 있으나 타입체커는 복잡해진다)

예시

```gaupel
@pure
export fn add(a: int, b: int) -> u32 {
  return a + b;
}
```

### 6.2 qualifier: pure, comptime

#### 6.2.1 pure

정의:

* pure 함수는 관측 가능한 부수효과가 없어야 한다.

v0 보수 규칙:

* 금지:

  * commit, recast 사용
  * pub 호출 (큰 상태 수정 경로)
  * FFI 호출 (use ...::ffi)
  * I/O 성격 표준 라이브러리 호출
  * && 사용

* 허용:

  * 지역 계산
  * 불변 데이터 읽기

예시

```gaupel
@pure
fn clamp(p: int, lo: int, hi: int) -> int {
  if (p < lo) { return lo; }
  if (p > hi) { return hi; }
  return p;
}
```

#### 6.2.2 comptime (정의 보강)

정의:

* comptime 함수는 컴파일 타임에만 호출되는 함수다.
* 런타임 호출은 금지된다.

comptime의 목적:

* 상수 계산
* 코드 생성 보조 (향후)
* 정적 테이블 생성, 전처리 대체

v0 규칙 (단순하고 강한 제약):

* comptime 함수는 pure 규칙을 기본으로 포함한다.
* 추가로 금지:

  * heap 할당 (표준 라이브러리 호출 포함)
  * 파일/시간/환경 의존 값 접근
  * 전역 가변 상태 접근
* 허용:

  * 정수/실수/문자/문자열 조합
  * 배열 리터럴, 단순 루프 (컴파일 타임 실행기에서 지원하는 범위 내)

comptime 호출 위치 (v0 권장):

* let 초기화 식
* field 초기값 (향후)
* 배열 크기 N 계산 (지원 시)

예시

```gaupel
@comptime
fn pow2(n: int) -> int {
  // 단순 루프는 comptime 엔진이 지원한다고 가정
  mut set r = 1;
  mut set i = 0;
  while (i < n) {
    r = r * 2;
    i = i + 1;
  }
  return r;
}


fn use_comptime() -> void {
  let x: int = pow2(a: 5); // error: comptime 함수는 런타임에서 호출 불가
  // v0에서는 컴파일 타임 컨텍스트에서만 호출되도록 별도 문맥 규칙이 필요
}
```

v0에서의 실용 규칙(권장):

* "comptime 호출"은 다음 형태로만 허용한다.

  * comptime set NAME = expr;
  * 또는 배열 길이, type parameter 등 컴파일러가 지정한 위치
* v0 구현이 단단해질 때까지는 "런타임 본문 안에서 comptime 호출"을 전부 금지하는 방식이 안전하다.

예시 (권장 형태, v0에 넣기 좋은 문법)

```gaupel
// v0 권장 추가 문법(선택):
// comptime set N = pow2(a: 5);
```

### 6.3 호출 규칙: 위치 인자 vs 라벨 인자

* 한 호출에서 위치 인자만 쓰거나 라벨 인자만 쓴다.
* 혼합은 금지다.

예시

```gaupel
fn f(a: int, b: int) -> int { return a + b; }

fn calls() -> void {
  set x = f(1, 2);         // ok: positional
  set y = f(a: 1, b: 2);   // ok: labeled
  // set z = f(1, b: 2);   // error: mixed
}
```

### 6.4 `non-?` / `?` 함수, 예외 허용 범위, 호출 제약

### 6.4.1 함수 “예외 허용” 표기: 이름 접미 `?`

Gaupel은 함수 단위로 “예외(throw) 경로”를 **정적 분리**한다.

* **non-? 함수**: 함수 이름에 `?`가 없다.

  * `throw`, `try...catch` **전부 금지**
  * 컴파일러는 해당 함수를 **nounwind(비언와인드)**로 취급할 수 있다.
* **? 함수**: 함수 이름에 `?`가 붙는다.

  * `throw`, `try...catch` **허용**
  * 호출 그래프 상 예외 전파 경로가 존재할 수 있다.

문법 예시:

```gaupel
fn parse_u32(text: string) -> Result<u32> { ... }     // non-?
fn read_file?(path: string) -> bytes { ... }          // ?
```

> `?`는 반환 타입(nullable)과 무관하며, **오직 예외 메커니즘(throw/catch) 허용 여부**만 뜻한다.

---

### 6.4.2 non-? 함수에서의 금지 규칙(강제)

non-? 함수 본문에서는 아래가 **컴파일 에러**다.

* `throw ...;`
* `try { ... } catch (...) { ... }`
* `try` / `catch` 키워드가 포함된 모든 문장/구문
* **? 함수 호출** (단, 아래 6.X.4 “예외=>Result 브리지”를 통해서만 예외를 값으로 받는 것은 허용)

예시:

```gaupel
fn bad() -> void {
  throw Err("nope");          // error
}

fn also_bad() -> void {
  try { ... } catch (e) { }   // error
}
```

---

### 6.4.3 ? 함수에서의 허용 규칙(기본)

? 함수는 `throw`, `try...catch`를 사용할 수 있으며, **잡히지 않은 예외는 자동 전파**된다.

```gaupel
fn open_config?(path: string) -> Config {
  set bytes = read_file?(path: path);   // 여기서 throw되면 자동 전파
  return parse_config?(bytes: bytes);   // 자동 전파
}
```

---

### 6.4.4 예외 => `Result<T>` 브리지: `attempt` (추가, non-?에서도 사용 가능)

non-? 함수는 예외를 직접 다룰 수 없으므로, **예외를 값(`Result<T>`)으로 포획**하는 브리지를 제공한다.

* `attempt Expr;` 는 **Expr 실행 중 발생한 throw를 잡아** `Result<T>`로 만든다.
* Expr은 보통 `? 함수 호출`이 된다.
* `attempt` 자체는 **try...catch 문법이 아니다**. (non-?에서 허용)

형태:

```gaupel
set r: Result<T> = attempt some_throwing_call?(...);
```

예시:

```gaupel
fn load_config(path: string) -> Result<Config> {
  set bytes_r = attempt read_file?(path: path);   // Result<bytes>
  switch (bytes_r) {
    case Ok(bytes): {
      // parse는 non-? Result 기반 API를 사용한다고 가정
      return parse_config(bytes: bytes);
    }
    case Err(e): {
      return Err(e);
    }
  }
}
```

`attempt`는 컴파일러가 제공하는 최소 런타임(EH 포획 경계)로 lowering 되며,
**-fno-std 모드에서는 금지**(3.X 참조)로 두는 것을 v0 정책으로 한다.


---

## 7. 제어 흐름: if, switch, while, loop(iter), loop(for)

### 7.1 if / elif / else

문법:

* if (cond) Block
* elif (cond) Block (0개 이상)
* else Block (0 또는 1)

규칙:

* cond는 반드시 bool 타입이어야 한다. (truthy 금지)

예시

```gaupel
fn sign(x: int) -> int {
  if (x < 0) { return -1; }
  elif (x == 0) { return 0; }
  else { return 1; }
}
```

### 7.2 switch

문법:

```gaupel
switch (expr) {
  case LIT: { ... }
  case LIT: { ... }
  default: { ... }
}
```

규칙:

* fallthrough 금지
* case 라벨은 리터럴만 허용 (int, string, bool, char 등)
* default는 선택이지만 권장
* `case Err(name): { ... }` 등 타입 패턴 매칭

*여기서 `name`은 새 바인딩(스코프는 해당 case 블록).

예시:

```gaupel
fn demo(r: Result<u32>) -> u32 {
  switch (r) {
    case Ok(v): {
      return v;
    }
    case Err(e): {
      // e: Error
      return 0u32;
    }
  }
}
```

예시

```gaupel
fn classify(c: char) -> int {
  switch (c) {
    case 'a': { return 1; }
    case 'b': { return 2; }
    default: { return 0; }
  }
}
```

### 7.3 while (expr) Block

문법:

* while (cond) { ... }

규칙:

* cond는 bool이어야 한다.
* break, continue 사용 가능
* v0에서 while은 표현식이 아니라 문장이다.

예시

```gaupel
fn sum_to(n: int) -> int {
  mut set i = 0;
  mut set s = 0;
  while (i <= n) {
    s = s + i;
    i = i + 1;
  }
  return s;
}
```

### 7.4 loop(iter: i in IterableExpr) Block

문법:

* loop(iter: name in iterable) { ... }

정의:

* 반복자 루프를 단일 문법으로 통일한다.
* 기존 loop(while:), loop(until:) 문법은 v0에서 삭제한다.

IterableExpr 허용 (v0):

* 고정 배열 T[N]
* 가변 리스트 T[] (리스트 구현 여부에 따라)
* 범위 a..b, a..:b
* 표준 라이브러리 iterator (Handle/Store/ECS)

반복 변수의 타입:

* iterable의 요소 타입으로 결정된다.
* v0에서는 요소를 copy로 넘길지, 참조로 넘길지 정책을 고정해야 한다.

  * 단순 정책(권장): 요소는 값으로 바인딩한다. 큰 요소는 Handle/ref를 사용한다.

범위 문법:

* a..b: b 미만
* a..:b: b 이하

예시 (배열)

```gaupel
fn sum_arr(xs: int[4]) -> int {
  mut set s = 0;
  loop(iter: v in xs) {
    s = s + v;
  }
  return s;
}
```

예시 (범위)

```gaupel
fn sum_range(n: int) -> int {
  mut set s = 0;
  loop(iter: i in 0..:n) {
    s = s + i;
  }
  return s;
}
```

### 7.5 loop(for: RangeExpr) Block (추가)

문법:

* loop(for: 1..:5) { ... }
* loop(for: a..b) { ... }
* loop(for: a..:b) { ... }

정의:

* loop(iter: ...)는 "컨테이너/반복자" 기반 반복을 의미한다.
* loop(for: ...)는 "정수 범위 기반 반복"을 더 직접적으로 나타내는 별도 표기다.
* 둘은 의미가 겹칠 수 있으나, 파서와 사용자 가독성을 위해 둘 다 제공한다.
* loop(for: ...)는 내부적으로 "암시적 인덱스 변수"를 제공하는 방식이 가능하다.

v0 권장 규칙 (단순):

* loop(for: R)에는 반드시 이름을 포함해 명시한다. (권장 문법)

  * loop(for: i in 1..:5) { ... }
* 또는 정말 축약 형태를 허용한다면, 컴파일러가 기본 이름 i를 제공한다.

  * loop(for: 1..:5) { ... }  // i가 자동 생성된다고 가정

예시 (명시 이름 버전, 권장)

```gaupel
fn sum_for() -> int {
  mut set s = 0;
  loop(for: i in 1..:5) {
    s = s + i;
  }
  return s;
}
```

예시 (축약 버전, 선택 구현)

```gaupel
fn sum_for_short() -> int {
  mut set s = 0;
  loop(for: 1..:5) {
    // i는 컴파일러가 자동 제공한다고 가정
    s = s + i;
  }
  return s;
}
```

---

## 8. 표현식과 연산자, 파이프 << 와 hole _

### 8.1 기본 연산자

* 산술: + - * / %
* 비교: == != < <= > >=
* 논리: and or ! not xor
* 대입: = += -= *= /= %=
* 인덱싱: []
* 삼항: cond ? a : b (중첩 금지, v0 강제)
* 범위: a..b, a..:b
* 증감: x++, ++x

증감 연산자 규칙 (v0 권장):

* x++ : 후위 증가. 결과는 "증가 전 값"이다.
* ++x : 전위 증가. 결과는 "증가 후 값"이다.
* x가 mut 바인딩이어야 한다.
* acts 기반으로도 모델링할 수 있으나, v0에서는 기본 수치 타입에 대해 내장으로 제공해도 된다.

예시

```gaupel
fn exprs() -> void {
  set a = 1 + 2 * 3;
  set ok = (a >= 7) and (a != 0);
  set r = ok ? 10 : 20; // 중첩은 금지

  mut set x = 10;
  set y = x++;  // y=10, x=11
  set z = ++x;  // x=12, z=12
}
```

### 8.2 파이프 연산자 << 와 hole _

정의:

* lhs << CallExpr 형태만 허용한다.
* CallExpr 내부에 _ 가 정확히 1개 있어야 한다.
* _ 는 라벨 인자 자리에서만 허용한다.
* 변환 규칙:

  * lhs << f(a: _, b: 2)  ==  f(a: lhs, b: 2)

예시

```gaupel
fn add(a: int, b: int) -> int { return a + b; }

fn mul(x: int, y: int) -> int { return x * y; }

fn pipe_demo() -> void {
  set r = 1 << add(a: _, b: 2) << mul(x: _, y: 10);
}
```

### 8.3 borrow `&`와 escape `&&`

### 8.3.1 목적: 작은 데이터는 값, 공유/탈출은 명시적으로

Gaupel은 기본적으로 **값(move) 기반**이다.
Rust급의 복잡한 수명 추론을 v0에서 강제하지 않되, C/C++의 “참조/포인터가 슬쩍 탈출하여 수명 버그를 만드는 문제”를 피하기 위해, 다음 두 연산자를 제공한다.

* `&` : **비탈출(non-escaping) 접근 권한(borrow capability)** 생성
* `&&` : **소유권 탈출(escape) 승격**. 값(특히 tablet)을 “핸들 기반 소유”로 이동

이 두 연산자는 런타임 오버헤드를 만들기 위한 것이 아니라, **컴파일 타임 정적 규칙으로 안전성을 확보**하기 위한 장치다.

---

### 8.3.2 borrow `&`의 의미론

##### (1) `&`는 포인터가 아니라 “접근 권한 토큰”

`&x`는 `x`의 주소를 외부에 노출하는 포인터가 아니다.
`&x`는 “이 스코프 안에서 x에 접근할 수 있는 권한”을 나타내는 **borrow 값**을 만든다.

borrow는 두 종류다.

* `&T` : 읽기 전용 borrow
* `&mut T` : 쓰기 가능 borrow (배타적)

##### (2) borrow의 핵심 제약: 비탈출(non-escaping)

v0의 단순하고 강한 규칙:

borrow 값(`&T`, `&mut T`)은 **절대 탈출할 수 없다**.

탈출 금지에는 아래가 포함된다.

* 함수 반환값으로 반환 금지
* 전역/클래스 draft/field/tablet 멤버에 저장 금지
* 배열/리스트/클로저 캡처에 저장 금지
* FFI로 전달 금지(ABI 경계)
* `&&`로 승격 금지(= borrow를 escape로 바꾸는 것 금지)

즉 borrow는 “잠깐 쓰고 끝내는” 용도다.

##### (3) borrow의 수명: 렉시컬(블록) 기반

v0에서는 borrow 수명을 **렉시컬 스코프(블록) 단위**로 본다.

* `set r = &x;` 하면 `r`은 그 블록 끝까지 유효하다.
* 더 짧은 수명을 원하면 작은 `{ ... }` 블록을 사용한다.

(향후 v1+에서는 NLL(마지막 사용점 기반 단축) 같은 최적화를 넣을 수 있으나, v0 규칙은 단순성을 우선한다.)

##### (4) `&mut`의 배타 규칙(충돌 규칙)

동일 스코프/동일 수명 구간에서:

* `&mut x`가 존재하는 동안, `x`에 대한 다른 borrow(`&x` 또는 `&mut x`)는 금지된다.
* 또한 `x` 자체에 대한 직접 접근(읽기/쓰기)도 제한될 수 있다.
  v0 권장 규칙: `&mut x`가 살아있는 동안 `x`를 직접 읽거나 쓰는 행위는 금지한다.

이 규칙은 런타임 락이 아니라 **컴파일 타임 검사**다.

##### (5) borrow 대상은 “place expression”이어야 한다

`&` / `&mut`는 임시값에 붙을 수 없다. 반드시 저장 위치(place)를 가리켜야 한다.

v0에서 place expression의 최소 집합:

* `Ident`
* `place . Ident` (필드 접근)
* `place [ Expr ]` (인덱싱, 해당 표현식이 place를 반환하는 경우)

예:

* `&(a + b)` 는 에러 (임시값)
* `&arr[i]` 는 가능(배열 요소가 place인 경우)

---

### 8.3.3 escape `&&`의 의미론

##### (1) `&&x`는 “소유권을 탈출 가능한 소유 객체로 이동”

`&&x`는 `x`의 소유권을 소비(consumed)하고, 결과를 **핸들 기반 소유**로 승격한다.

* `x`는 사용 불가(소유권 이동)
* 결과는 “표준 라이브러리의 handle 계열 타입”으로 lowering 된다.

핵심: `&&`는 C++의 논리 AND가 아니다.
논리 연산은 `and/or/not/xor` 키워드만 사용한다.

##### (2) `&&`의 대상도 place expression이어야 한다

`&&`는 임시값에 붙을 수 없다.

* `&&(a + b)` : 에러
* `&&obj` : 가능(소유권 이동)

##### (3) lowering(내려쓰기) 규칙: 언어 코어는 타입을 내장하지 않는다

`&&x`의 결과 타입은 언어 코어에 내장하지 않는다. 대신 컴파일러가 아래처럼 lowering 한다고 본다.

* `&&x`  ==>  `handle::from_move(x)` (개념적)
* 이때 어떤 실제 타입이 선택되는지는 “표준 라이브러리의 규약”에 의해 결정된다.

  * 기본은 `handle<T>`(유니크 소유)로 시작하는 것을 권장
  * 이후 `arc<T>`(공유 소유) 같은 것으로 확장 가능

##### (4) `pure` / `comptime` 제약 (v0 강제 권장)

* `pure` 함수 안에서는 `&&` 사용 금지 (할당/참조카운트 등 관측 가능한 효과 가능성 때문)
* `comptime` 함수 안에서는 `&&` 사용 금지

(향후 v1+에서 “순수한 핸들(무할당)” 같은 형태를 도입하면 완화 가능)

---

### 8.3.4 예시

##### 예시 1: 읽기 전용 borrow

```gaupel
fn sum2(a: &int, b: &int) -> int {
  return a + b;
}

fn demo_read_borrow() -> void {
  set x = 10;
  set y = 20;
  set s = sum2(a: &x, b: &y);
}
```

##### 예시 2: `&mut`로 수정 권한 위임

```gaupel
fn inc(x: &mut int) -> void {
  x = x + 1;
}

fn demo_write_borrow() -> void {
  mut set a = 0;
  inc(x: &mut a);
  // a == 1
}
```

##### 예시 3: borrow 비탈출 규칙(금지 사례)

```gaupel
fn bad_return_ref() -> &int {
  set x = 3;
  return &x;   // error: borrow 값은 함수 밖으로 탈출할 수 없다
}

fn bad_store_global() -> void {
  // draft.someRef = &x; // error: draft/field/tablet 멤버 저장 금지
}
```

##### 예시 4: `&&`로 소유권 탈출(핸들 승격)

```gaupel
tablet File {
  public:
    fn close() -> void { /* ... */ }
}

fn open_file() -> handle<File> {
  set f = File();   // 스택/스코프 소유
  return &&f;       // f 소비, handle로 승격하여 반환
}
```

##### 예시 5: `&mut` 배타 규칙(충돌)

```gaupel
fn demo_exclusive() -> void {
  mut set x = 1;

  set r = &mut x;
  // set s = &x;      // error: &mut x 살아있는 동안 다른 borrow 불가
  // set t = x + 1;   // error(v0 권장): &mut x 살아있는 동안 직접 접근 금지

  r = r + 1;
}
```

##### 예시 6: 논리 연산은 키워드만 사용

```gaupel
fn demo_logic() -> void {
  set a = true;
  set b = false;

  if (a and not b) {
    // ...
  }

  // if (a && b) { } // error: &&는 escape 연산자 예약, and를 사용할 것
}
```

### 8.3.5 `&&` 구현 지침(의미론 고정 + fat pointer ABI)

`&&`는 **항상** “탈출(escape) 핸들(handle) 생성”을 의미한다.
상황(컨텍스트)에 따라 의미가 바뀌지 않는다. 즉, “어떤 경우에는 move, 어떤 경우에는 refcount” 같은 **숨은 의미 변화는 금지**한다.

#### (1) 결과 ABI: `&&`는 항상 fat pointer(handle)로 표현된다

v0 권장: handle은 다음과 같은 **fat pointer ABI**를 가진다.

* `(ptr, meta)` 2워드 구조를 기본으로 한다.

  * `ptr`: 실제 데이터 주소(또는 객체 헤더 주소)
  * `meta`: drop/clone 등 동작을 수행하기 위한 메타(간단히는 vtable 포인터 또는 typeinfo 포인터)

필요 시 확장(향후):

* slice handle처럼 길이가 필요하면 `(ptr, len, meta)`로 확장할 수 있으나, **v0에서는 `&&` handle은 (ptr, meta) 고정**을 권장한다.

  * slice는 아래 8.4의 **borrow slice 타입 `&[T]` / `&mut [T]`**로 해결한다.

#### (2) lowering 규칙(개념적)

`&&x`는 개념적으로 다음처럼 lowering 된다.

* `&&x`  ==>  `__make_handle_from_move(ptr_of(x), meta_of(T))`
* 여기서 `T`는 `x`의 정적 타입이며, `meta_of(T)`는 컴파일러가 생성/배치하는 타입 메타를 가리킨다.

중요: “표준 라이브러리 타입(handle<T>)”의 이름을 언어 코어가 강제하지 않는다.
하지만 **ABI/의미론은 고정**이므로, freestanding에서도 `&&` 자체는 문제없이 동작한다.

#### (3) 최적화 허용 범위(의미 보존만)

컴파일러는 다음 최적화를 **허용**한다(의미는 동일).

* handle을 메모리 객체로 만들지 않고 레지스터(SSA 값)로만 보관
* `meta`가 정적으로 확정되면 간접 호출 대신 직접 호출/인라인
* escape analysis로 중간 handle 생성 단계를 제거(호출자 sret 공간에 직접 move-construct)
* 즉시 파괴되는 handle 제거(dead handle elimination)

단, 아래는 **금지**(의미 변화):

* 어떤 경우에는 “그냥 포인터”, 어떤 경우에는 “소유 핸들”처럼 표현 자체를 바꿔 관측 가능한 의미가 달라지는 것
* borrow를 `&&`로 승격하여 탈출시키는 것(아래 8.3.2의 “borrow 비탈출” 규칙 유지)

#### (4) 제약 재확인

* `&&`는 place expression에만 적용 가능
* `&&`는 borrow(`&T`, `&mut T`, `&[T]`, `&mut [T]`)에 적용 불가
* `pure` / `comptime` 안에서 `&&` 사용 금지(v0 강제 권장)

---

### 8.4 slice borrow: `&[T]`, `&mut [T]` 와 슬라이싱 문법

Gaupel은 Rust 스타일의 slice/view 표현력을 원하지만, 표준 라이브러리 특수 타입(view<T>)에 언어 규칙을 붙이면 “코어/표준 경계”가 흐려질 수 있다.
따라서 v0에서는 slice를 **언어 코어의 borrow 타입 확장**으로 직접 제공한다.

#### (1) slice 타입 정의

* `&[T]` : 읽기 전용 slice borrow
* `&mut [T]` : 쓰기 가능 slice borrow(배타적)

slice borrow는 일반 borrow와 동일한 성질을 가진다.

* **비탈출(non-escaping)**: 반환/저장/캡처/FFI 전달/`&&` 승격 금지
* **렉시컬 수명**: 블록 스코프 기반
* **배타 규칙**: `&mut [T]`가 살아있는 동안 같은 base에 대한 다른 접근 제한

#### (2) 슬라이싱(slicing) 문법: `&x[a..b]`, `&x[a..:b]`

슬라이싱은 “값을 꺼내서 뭘 한다”가 아니라, 그 자체가 **하나의 조작(권한+범위) 생성**으로 인식된다.

권장 문법(최소 형태):

* `set s = &x[a..b];`
* `set s = &x[a..:b];`
* `set s = &mut x[a..:b];`

여기서 `x`는 다음 중 하나여야 한다(v0):

* 고정 배열 `T[N]`
* (선택) 가변 리스트 `T[]` — v0에서 리스트가 들어오면 동일 규칙 적용 가능

범위 규칙은 기존 range와 동일:

* `a..b` : b 미만
* `a..:b` : b 이하

#### (3) 결과 타입 규칙

* `&x[a..b]`의 타입은 `&[T]`
* `&mut x[a..b]`의 타입은 `&mut [T]`

여기서 `T`는 `x`의 요소 타입이다.

#### (4) 기본 제약(단순하고 강한 v0 규칙)

* 인덱스 `a`, `b`는 정수 타입이어야 한다(v0에서는 int 계열로 고정 권장).
* `a`, `b`의 런타임 범위 검사는 구현 선택:

  * v0 권장: debug 빌드에서만 검사 + release에서는 제거 가능(또는 최소 검사)
* slice는 **항상 비탈출**이므로, “slice를 반환하고 싶다”면 반드시 복사/핸들을 사용해야 한다:

  * 작은 데이터면 `return copy ...;`
  * 큰 데이터면 표준 handle 타입을 사용(또는 API 설계를 바꾸기)

#### (5) 예시: 요청한 형태 그대로

```gaupel
fn slice_demo() -> void {
  let x: int[8] = [0,1,2,3,4,5,6,7];

  // 슬라이스 생성(읽기 전용)
  set arr = &x[1..:5];     // 타입: &[int]  (요소: 1,2,3,4,5)

  // 슬라이스 생성(쓰기 가능)
  mut set y: int[8] = [0,0,0,0,0,0,0,0];
  set win = &mut y[2..:4]; // 타입: &mut [int] (요소 슬롯: y[2],y[3],y[4])

  // 금지: slice 탈출
  // return arr;           // error: &[T]는 비탈출
  // return &&arr;         // error: borrow를 escape로 승격 금지
}
```

#### (6) 함수 파라미터에서의 사용 예시

```gaupel
fn sum(xs: &[int]) -> int {
  mut set s = 0;
  // v0에서는 slice 반복을 단순화하기 위해 표준 라이브러리 helper가 필요할 수 있음
  // 최소 구현: xs[i] 인덱싱을 허용(범위 내라고 가정하거나 디버그 검사)
  mut set i = 0;
  while (i < xs.len) {     // len 접근 문법은 v0에서 선택: 내장 또는 표준 규약
    s = s + xs[i];
    i = i + 1;
  }
  return s;
}

fn use_sum() -> void {
  let a: int[6] = [10,20,30,40,50,60];
  set mid = &a[1..:4];     // &[int] (20,30,40,50)
  set r = sum(xs: mid);
}
```

> 구현 메모(v0): `&[T]`는 내부적으로 `(ptr, len)` 형태의 borrow 값으로 lowering 하면 된다.
> 이는 “표준 라이브러리 특수 타입”이 아니라 **언어 코어 타입**이므로 freestanding에서도 경계가 깨지지 않는다.

### 8.5 `copy` / `clone` 연산자(키워드) 정의

Gaupel은 “암묵 복사 금지(또는 최소화)”를 목표로 하므로, 복사/복제는 **연산자(키워드)로 명시**한다.
`copy()` / `clone()` 같은 함수 형태는 제공하지 않는다(유저 라이브러리 함수처럼 보여 언어 철학과 충돌).

#### (1) 문법(권장): prefix 연산자

* `copy Expr`
* `clone Expr`

우선순위: 단항 연산자 레벨(예: `not`, unary `-`)과 동일하게 취급한다.
즉, `copy a + b`는 `(copy a) + b`로 해석된다(혼동 방지를 위해 괄호를 권장).

#### (2) 의미론

* `copy` : **얕은 복사(shallow copy)**, 값의 “비트 단위 복사”에 가까운 복사

  * POD(field) 및 기본 수치 타입처럼 “복사 비용과 의미가 명확한 타입”에 적합
* `clone` : **깊은 복사(deep clone)**, 소유 리소스가 있으면 새로 복제

  * string, handle 기반 컨테이너, 리소스 객체 등

v0 권장 정책:

* `copy`는 “Copy 가능 타입”에서만 허용
* `clone`은 “Clone 가능 타입”에서만 허용

#### (3) 타입체커 규칙(acts 연동)

`copy Expr` / `clone Expr`는 다음처럼 처리한다.

1. Expr의 타입을 T로 확정한다.
2. `acts T`에서 다음 op를 찾는다.

   * `op(copy)`  (copy용)
   * `op(clone)` (clone용)
3. 정확히 1개가 매칭되면 그 반환 타입이 결과 타입이다(일반적으로 T).
4. 없으면 에러.

즉, `copy/clone`은 “언어 내장 매직”이 아니라 **acts의 op 매핑으로 표준화**된다.
(단, 기본 수치 타입/단순 field에 대해서는 컴파일러가 내장 acts를 제공해도 된다.)

#### (4) place 요구 여부(v0 권장)

v0에서는 단순화를 위해 아래 중 하나를 선택한다(둘 다 가능하지만, v0는 하나로 고정 추천).

* 정책 A(단순/보수): `copy/clone`은 place expression에만 허용

  * `copy x` ok, `copy (a+b)` error
* 정책 B(표현력): rvalue도 허용

  * `copy (a+b)` ok (임시값 복사)

Gaupel의 “명시적 비용” 철학상 정책 A가 더 보수적이고 구현도 쉽다.

#### (5) 예시(반환/대입/초기화)

```gaupel
fn demo_copy_return(a: int) -> int {
  // int는 내장 copy 가능
  return copy a;
}

fn demo_copy_assign() -> void {
  let x: int = 3;
  let y: int = copy x;   // 명시적 복사
}

fn demo_clone_assign() -> void {
  let s: string = "hi";
  let t: string = clone s;   // 깊은 복제(새 버퍼)
}
```

#### (6) 경고

컴파일러는 `copy` 사용에 대해 휴리스틱 경고를 낼 수 있다.

예:

* 큰 배열/큰 field를 `copy`로 통째로 복사하려는 경우
* 반복문에서 큰 값을 `copy`로 계속 생성하는 경우

이 경고는 오류가 아니라 “비용 경고”로 둔다.

#### (7) acts 예시: 사용자 정의 타입의 copy/clone

```gaupel
field Big {
  u32 a;
  u32 b;
  u32 c;
  u32 d;
}

acts for Big {
  operator(copy)(self: Big) -> Big {
    // field는 POD이므로 단순 복사로 충분
    return __intrin_memcpy_big(x: self);
  }
}
```

### 8.6 `delete` 문장(명시적 파괴) 정의

`delete`는 스코프 기반 파괴(RAII)를 기본으로 하되, “리소스를 지금 당장 닫고 싶다” 같은 경우를 위해 제공한다.
(예: 파일 핸들 조기 해제, GPU 리소스 조기 반환)

#### (1) 문법

* `delete place;`

`delete`는 **문장(statement)** 이다. 표현식이 아니다.

#### (2) 의미론

* `delete x;`는 x를 **소비(consumed)** 하며, 이후 x는 사용 불가이다.
* `delete`는 내부적으로 `acts T`의 `op(drop)` 또는 tablet destructor/handle drop으로 lowering 된다.

권장 lowering:

* `delete x;`  ==>  `__drop(T, x)` 또는 `acts T op(drop)`

#### (3) 적용 대상(v0 권장)

v0에서 구현 난이도를 낮추기 위해 다음처럼 제한을 권장한다.

* 허용: `&&`로 생성된 handle(escape 소유 객체), 또는 명확한 소유 객체(tablet 값)
* 금지: borrow(`&T`, `&mut T`, `&[T]`, `&mut [T]`)에 대한 delete
* 금지: `pure` / `comptime` 내부에서 delete

> 주의: tablet 생성자/소멸자 삭제 문법의 `= delete;`는 “선언” 문맥이고,
> 여기의 `delete x;`는 “문장” 문맥이다. 둘은 충돌하지 않는다.

### 8.7 연산자 전반의 acts op 매핑 확장 지침(표준 키 목록)

표현식에서 사용하는 연산자 대부분은 `acts`의 `op(...)`로 해석 가능해야 한다.
v0에서 구현을 튼튼하게 만들기 위해, **op 키 문자열을 표준화**한다.

#### (1) v0 표준 op 키(권장)

이 목록은 “문법 토큰”과 “op 키”를 1:1로 연결하기 위한 최소 표준이다.

* 산술: `op(+)`, `op(-)`, `op(*)`, `op(/)`, `op(%")`
* 비교: `op(==)`, `op(!=)`, `op(<)`, `op(<=)`, `op(>)`, `op(>=)`
* 논리(키워드 기반): `op(and)`, `op(or)`, `op(not)`, `op(xor)`
* 단항: `op(-unary)`, `op(not)(또는 not와 통일)`
* 복사/복제: `op(copy)`, `op(clone)`
* 파괴: `op(drop)`
* 증감(v1+ 확장 키 예시): `op(++pre)`, `op(++post)` 등

v0에서는 증감(++/--)을 기본 수치 타입에 내장으로 제공해도 되며,
장기적으로는 위 확장 키로 acts에 편입 가능하다.

#### (2) 규칙

* 동일 타입의 acts 블록에서 같은 `op(...)`는 **정확히 1개만** 허용한다.
* 여러 후보가 생기면 모호성 에러(ambiguous)로 보고한다.

#### (3) 예시: copy/clone/drop까지 포함한 acts

```gaupel
acts string {
  fn do_clone(self: string) : op(clone) -> string { return __intrin_string_clone(s: self); }

  fn drop(self: string) : op(drop) -> void { __intrin_string_drop(s: self); }
}
```

### 8.8 예외 메커니즘: `throw`, `try...catch`, 예외 타입, 전파 규칙 (추가)

### 8.8.1 예외 값의 타입: 표준 `Error`만 허용(v0 고정)

Gaupel의 `throw`는 “아무거나 던지기”를 금지한다. v0에서는 다음으로 고정한다.

* `throw`에 실리는 값은 반드시 표준 라이브러리의 `Error` 타입이어야 한다.

  * 권장: `Error`는 “메시지 + 코드 + 원인 체인”을 담을 수 있는 불투명 핸들
  * freestanding에서도 동작 가능하도록 `Error`는 **할당 없이도 생성 가능한 경로**(고정 문자열, 코드 기반)를 제공해야 한다.

예시(표준 라이브러리 관례 예시):

```gaupel
// std::error::Error 가 있다고 가정
use std::error::Error;

fn fail?( ) -> void {
  throw Error::from_msg(msg: "boom");
}
```

---

### 8.8.2 `throw` 문장 규칙(? 함수 전용)

* `throw Expr;` 는 **문장(statement)** 이다.
* **? 함수 내부에서만 허용**된다.
* Expr의 타입은 `Error`여야 한다(또는 `Error`로의 명시적 변환이 있어야 한다).

```gaupel
fn read_file?(path: string) -> bytes {
  if (path == "") {
    throw Error::from_code(code: 12);   // ok
  }
  // ...
}
```

---

### 8.8.3 `try...catch` 문법과 규칙(? 함수 전용)

문법(권장 최소형):

```gaupel
try {
  // throwing code
} catch (e: Error) {
  // handler
}
```

규칙:

* `try...catch`는 **? 함수 내부에서만 허용**
* catch 파라미터 타입은 v0에서 `Error`로 고정(패턴/다형 catch는 v1+)

예시:

```gaupel
fn load_user?(id: u32) -> User {
  try {
    return fetch_user?(id: id);
  } catch (e: Error) {
    // 복구 또는 변환
    throw Error::wrap(msg: "load_user failed", cause: e);
  }
}
```

---

### 8.8.4 전파 규칙(암묵 전파)과 “명시성”의 경계

? 함수는 예외를 암묵 전파할 수 있다. 단, non-?로 빠져나가는 경계에서는 항상 명시적이어야 한다.

* `? -> ?` : 암묵 전파 허용
* `? -> non-?` : 직접 호출 금지(6.X.2), 반드시 `attempt`로 값으로 포획

이 규칙 덕분에 컴파일러는 **non-? 영역을 강한 최적화/단순 CFG**로 유지할 수 있다.


---

## 9. class, draft, pub/sub, commit, recast

### 9.0 draft의 실체: “스냅샷 + 스테이징(draft) 버퍼” 모델(명문화)

class는 “큰 공유 상태”를 제공한다. v0의 핵심 구현 모델은 다음이다.

* **Published Snapshot**: sub가 읽는 불변 스냅샷
* **draft Buffer**: pub가 수정하는 스테이징 버퍼(아직 발행되지 않음)

즉, pub에서 `draft.x = ...`는 “즉시 공유 메모리를 찌르기”가 아니라,
**스테이징 버퍼에 변경을 기록**하는 동작이다. `commit;`에서만 발행된다.

이 모델은 OS 의존이 아니다.

* 멀티스레드 동기화는 표준 라이브러리(또는 런타임)가 제공하는 최소 프리미티브(원자적 포인터 스왑, 스핀락/뮤텍스)만으로 구현 가능
* freestanding에서도 동일

### 9.1 class 정의

* class는 대단위 공유 컨텍스트 단위다.
* draft는 class가 보유하는 상태의 논리적 이름이다.
* draft 접근은 sub/pub 모드 함수에서만 허용한다.

예시

```gaupel
class Game {
  // draft.score 같은 상태가 존재한다고 가정

  fn sub score() -> int { return draft.score; }
}
```

### 9.2 sub 규칙

* draft 읽기만 가능
* draft에 대한 대입, 가변 변경 금지
* recast; 는 sub 내부에서 "현재 sub가 보는 draft 스냅샷을 재구성(recast)하여 최신 상태로 다시 캐스팅/재해석"하는 제어 구문으로 사용된다.

recast (추가 설명):

* sub가 보는 draft는 “함수 진입 시점의 스냅샷”이다(렉시컬 일관성).
* `recast;`는 **현재 스레드/호출이 들고 있는 스냅샷 핸들을 최신으로 다시 잡는** 제어 구문이다.


또한 pub/sub에서 commit, recast는 단순 함수 호출이 아니라 "제어 구문"으로 취급한다 (추가):

* continue가 루프 흐름을 바꾸는 것처럼, commit/recast는 "상태 흐름을 바꾸는 제어 구문"이다.
* 예를 들어, pub에서 commit은 "draft 변경분을 발행"하고 pub의 상태 단계를 종료시키는 의미를 갖는다.
* sub에서 recast는 "draft 관찰 뷰"를 재설정하는 의미를 갖는다.
* 따라서 if 내부에서 continue를 쓰듯이, if 내부에서 recast/commit을 쓸 수 있으며, 이는 "제어적 의미"를 가진다.

  * 단, pub의 최종 commit 강제 규칙은 그대로 적용된다. (if 내부 commit은 최종 commit으로 인정되지 않는다)

예시:

```gaupel
class Counter {
  fn sub get() -> u32 {
    // 오래된 스냅샷이면 갱신하고 싶을 때
    if (draft.stale) {
      recast;
    }
    return draft.count;
  }
}
```

### 9.3 pub 규칙

핵심 규칙:

* pub 함수는 draft 수정 가능
* 최종 commit은 "함수 본문 최상위 블록의 마지막 유효 문장" 이어야 한다.
* 분기/중첩 블록 내부의 commit은 최종 commit 검사에서 무시한다.
* pub는 draft를 수정할 수 있다. 단, v0의 흐름 규칙은 더 엄격히 고정한다.
* pub는 내부적으로 “발행 가능한 변경 집합”을 만든다.
* `commit;`을 실행하면 현재 변경 집합이 발행되고, pub는 계속 진행 가능(아래 9.X.5)


v0 권장:

* pub는 void return
* pub 내부 return 금지

추가 (제어 구문 성격 강조):

* pub에서 commit은 단순 문장 종료가 아니라 "발행 단계"를 수행하는 제어 구문이다.
* if 안에서 commit을 쓰는 것은 "조기 발행"처럼 보일 수 있으나, v0의 최종 commit 규칙에서는 최종으로 인정되지 않는다.
* 따라서 v0에서는 if 내부 commit은 허용하되 경고를 권장하거나, 아예 금지로 시작해도 된다. (정책 선택)
* mut 가능하되, commit 전후로 borrow가 살아있으면 에러

예시

```gaupel
class Counter {
  fn pub add(delta: int) -> void {
    if (delta < 0) {
      // commit; // 있어도 최종 commit으로는 인정되지 않음 (정책에 따라 경고/에러 가능)
    }
    draft.count += delta;
    commit;
  }
}
```

### 9.3.1 pub에서 `&&`는 “핸들 생성(승격)”이며 draft로는 “대입”만 한다

pub의 핵심은 “draft 변경 + 최종 commit”이다. `&&`는 “소유권 탈출”이므로 pub에서 흔히 쓰일 수 있다:

예:

```gaupel
fn add_sprite() : pub {
  set s = Sprite();
  draft.sprite = &&s;   // handle로 승격해 draft에 저장
  commit;
}
```

### 9.3.2 pub/sub에서 draft에 대한 “move-out” 금지

draft는 “공유 상태의 저장소”이므로, v0에서는 draft에서 값을 꺼내 move 하는 것을 금지하는 게 안전하다.

**v0 규칙:**

* `&&draft.x` 금지
* `set y = draft.x;` 는 Copy 가능한 타입이면 허용(복사/읽기)
* handle은 “값 복사(핸들 복제)”가 아니라 **핸들 규약**에 따른다(예: handle은 move-only, arc는 clone 등)

이 규칙을 넣으면 draft의 일관성/원자성 모델이 깨지는 걸 막을 수 있어.

### 9.3.3 commit의 경계 정의(강제): “publish barrier” + “borrow/alias 리셋 지점”

`commit;`은 단순한 문장이 아니라, **상태 경계**다.

commit 경계에서의 규칙(강제):

1. **commit을 넘는 borrow 금지**

   * pub에서 `&mut` 포함 모든 borrow 값(`&T`, `&mut T`, `&[T]`, `&mut [T]`)이 살아있는 상태로 `commit;` 실행 경로에 도달하면 에러
2. **draft에 대한 alias 규칙(배타)**

   * 같은 pub의 동일 스테이징 구간에서, 동일 draft 셀(예: `draft.x`, `draft.arr[i]`)에 대해

     * `&mut` alias가 2개 이상 생기면 에러
     * `&mut`이 존재하는 동안 동일 셀에 대한 읽기/쓰기 직접 접근도 에러(v0 보수 규칙)
3. **draft move-out 금지 유지**

   * `&&draft.x` 금지
   * draft에서 꺼내는 건 “읽기(copy/clone/handle 규약)”만 허용

예시(금지):

```gaupel
class Bad {
  fn pub f() -> void {
    set r = &mut draft.count;
    draft.count = 1u32;
    commit;              // error: borrow 살아있음
  }
}
```


### 9.3.4 commit 시점 제약: “commit을 넘는 borrow 금지”를 명문화

이미 본문에 힌트가 있으니, 딱 한 줄 규칙으로 고정:

* pub 블록에서 `commit;`이 실행되는 경로에서, `&mut` borrow(또는 어떤 borrow든)가 살아있으면 에러.

### 9.3.5 pub에서 “commit 여러 번 허용”(요청 반영, v0 확정)

pub는 한 번의 호출 안에서 여러 번 commit할 수 있다. 의미는 다음으로 고정한다.

* `commit;`을 실행하면 **현재까지의 draft 변경이 발행**된다.
* 그 즉시 pub는 **새 스테이징 단계**로 진입한다.

  * 이후의 `draft.*` 접근/수정은 “방금 발행된 최신 스냅샷”을 베이스로 하는 새로운 draft로 간주한다.
* 즉, pub 내부는 다음처럼 여러 “발행 단계”를 가질 수 있다:

```gaupel
class Progress {
  fn pub run() -> void {
    draft.step = 1u32;
    commit;          // step=1 발행

    draft.step = 2u32;
    commit;          // step=2 발행

    draft.step = 3u32;
    commit;          // step=3 발행 (최종)
  }
}
```

이 규칙은 멀티스레드에서도 안전한 모델을 유지한다.

* sub는 각자 잡은 스냅샷을 읽으며, 필요할 때만 `recast;`로 최신을 본다.
* pub의 각 commit은 “원자적 스냅샷 교체(또는 버전 증가)”로 구현된다.

---

### 9.3.6 pub 함수의 return 정책(요청 반영, v0 확정)

pub는 “상태 발행을 반드시 명시”해야 하므로, 반환 정책을 다음으로 고정한다.

* pub는 **일반 `return`을 금지**한다. (조기 탈출이 commit 규칙을 깨기 쉬움)
* pub에서 값을 반환하려면 아래 단 하나만 허용한다:

#### (1) `commit return Expr;` (신규 문법, pub 전용)

* 의미: “현재 draft를 발행하고, Expr 값을 반환하며, pub를 종료”
* `commit return ...;` 는 **함수 최상위 블록에서만**, 그리고 **마지막 문장으로만** 허용한다.

예시:

```gaupel
class Counter {
  fn pub add(delta: u32) -> u32 {
    draft.count += delta;
    commit return draft.count;     // 발행 + 반환
  }
}
```

#### (2) void pub는 `commit;`으로 종료

```gaupel
class Counter {
  fn pub inc() -> void {
    draft.count += 1u32;
    commit;                        // 발행 + 종료(암묵 return)
  }
}
```

> 결과적으로 “pub의 종료는 항상 commit과 결합”된다.
> 컴파일러 입장에선 CFG가 매우 단순해지고, 규칙 위반 진단도 쉬워진다.

---

### 9.3.7 “draft에 뭔가를 넣는(insert/append) 동작”의 위치: 표준 컨테이너는 **handle로 draft에 저장**(권장)

draft는 “데이터 레코드 + 핸들”로 유지한다(기존 철학 강화).

* draft 내부에 동적 컨테이너를 값으로 두지 않는다.
* draft에는 `handle<Vec<T>>` 같은 핸들만 두고,
* 실제 push/append는 핸들이 가리키는 구현체(tablet/컨테이너)에 대해 수행한다.

예시(권장 패턴):

```gaupel
class Scene {
  // draft.sprites: handle<SpriteList> 라고 가정

  fn pub add_sprite(s: handle<Sprite>) -> void {
    // SpriteList는 tablet이고, draft에는 handle만 저장
    draft.sprites.push(x: s);   // push는 handle/tablet 쪽 메서드
    commit;
  }

  fn sub count() -> u32 {
    return draft.sprites.len();
  }
}
```

이 패턴은 OS 의존이 없다.

* freestanding에서는 컨테이너가 내부적으로 “사용자 제공 allocator” 또는 “고정 풀”을 쓰면 된다.
* draft는 여전히 “스냅샷/발행 모델”만 담당한다.

---

### 9.3.8 멀티스레드 안전성 요약(문서에 포함, 구현 가이드)

v0에서 class는 다음 성질을 목표로 한다.

* sub: 락 없이 읽을 수 있다(스냅샷 포인터/버전 기반)
* pub: 단일 writer(혹은 내부 락)로 직렬화된다
* commit: 원자적 스냅샷 교체(atomic swap) + 필요 시 epoch 기반 reclamation(표준 라이브러리 구현 영역)

즉, 사용자는 OS 없이도 일관된 규칙을 얻는다.

* “sub는 읽기 전용 스냅샷”
* “pub는 draft 수정”
* “commit은 publish barrier”

---

### 9.4.1 “선언(Decl) 포함”과 “값(Instance) 포함”을 구분한다

* `field/proto/tablet/acts/class`는 서로 **중첩 선언**(nested declaration)을 허용할 수 있다.
  (이건 네임스페이스/스코프 설탕이라 구현 난이도 낮음)
* 하지만 “값으로 포함(멤버로 저장)”은 별도 규칙을 따른다.

> v0 권장: 중첩 선언은 허용하되, ABI/가시성 규칙은 “그냥 내부 스코프의 top-level”로 취급한다.

### 9.4.2 class(draft 상태)에 포함 가능한 것

class의 draft는 “대단위 공유 상태”이므로, v0에서는 **복잡한 생명주기/소유권이 끼는 값**을 draft에 직접 넣지 않는 게 안정적이다.

**v0 규칙:**

* draft에 직접 포함 가능:

  * 원시 타입, `char`, `bool`
  * `field` (단, field가 FFI-safe/POD 조건을 만족할 때)
  * 고정 배열 `T[N]` (T가 POD일 때)
  * `handle<T>` 같은 **핸들 계열**(표준 라이브러리 타입, 불투명 소유)
* draft에 직접 포함 금지:

  * `tablet` “값 인스턴스”
  * `class` 인스턴스
  * `string`, `T[]` 등 동적 소유/할당이 필요한 것(정책상 v0 금지 권장)
  * `&T`, `&mut T` (borrow)

**의도:** draft는 “데이터 레코드 + 핸들”로 유지하고, 리소스 생명주기/소유권은 handle로만 표현한다.

### 9.4.3 tablet 내부에 포함 가능한 것

tablet은 “일반 구현 타입”이므로 멤버 포함을 폭넓게 허용할 수 있다. 다만 class는 예외로 두는 걸 추천한다.

**v0 규칙(권장):**

* tablet에 포함 가능:

  * field, 원시 타입, 고정 배열, handle<T>, string(허용 여부 정책)
  * 다른 tablet 인스턴스(가능) *하지만 생성자/소멸자 호출이 존재*
* tablet에 포함 금지:

  * class 인스턴스 (draft/publish 모델과 충돌: “객체 내부에 또 공유상태 머신”이 들어오면 모델이 무너짐)
  * borrow 타입 멤버(`&T`, `&mut T`) (비탈출 원칙과 충돌)

> **중요 포인트:** tablet을 class draft에 “값으로” 넣는 건 금지하되, tablet은 어디서든 **handle<tablet>**로는 담을 수 있다.
> 즉 “큰 상태 = handle로 들고, draft에는 handle만 저장”이 기본 패턴.

---

## 10. 타입 정의: field, proto, tablet, 접근 제한자

### 10.1 field (POD storage)

정의:

* field는 POD 데이터 블록이다.
* 생성자/소멸자 없다.
* field 내부에는 값만 있다. 함수 선언/정의 금지.
* field는 proto/tablet/class 내부에 포함 가능하다.
* field는 기본적으로 copy 가능 조건을 가진다. (모든 멤버가 copy 가능일 때)

추가 (field 타입 제한 파라미터, 추가):

* field는 선택적으로 타입 파라미터/제약을 가질 수 있다.
* 문법 예:

  * field<T> Name { ... }
  * field<u32, i32> Name { ... }
  * field<typename T> Name { ... }  // v1+ 목표 형태, v0에서는 파싱만 선행 가능
* 의미:

  * field<u32, i32> D { ... } 는 "D가 담을 수 있는 멤버 타입의 후보를 제한"하는 선언으로 해석한다.
  * v0에서는 가장 단순하게 "field 본문에서 등장하는 타입이 <> 목록에 포함되어야 한다"로 시작할 수 있다.
  * 향후에는 "특정 슬롯이 오직 해당 타입만 허용" 같은 더 세밀한 제약으로 확장 가능하다.

허용 멤버 (v0):

* 수치, bool, char
* 고정 배열 T[N] (T가 POD)
* 다른 field
* Handle<T> (표준 라이브러리 타입, 설계 포함)

금지 (v0):

* string (가변/복잡 소유권)
* T[] (가변 리스트)
* class 인스턴스
* &T, &mut T (borrow 참조)
* closure<...>

예시

```gaupel
field Vec2 {
  float32 x;
  float32 y;
}

field Particle {
  Vec2 pos;
  Vec2 vel;
  uint32 id;
}
```

예시 (타입 제한 field, 추가)

```gaupel
field<u32, i32> OnlyInts {
  u32 a;
  i32 b;
  // float32 c; // error: field<>에 포함되지 않은 타입
}
```

### 10.2 proto (interface only)

정의:

* proto는 계약이다.
* 생성자/소멸자 없다.
* 함수 시그니처를 선언한다.
* v0에서 proto는 구현을 담지 않는다.
* proto 안에 field 선언은 "인터페이스가 요구하는 데이터 슬롯"으로만 해석하며, 실제 레이아웃은 tablet에서 확정되는 방식이 안전하다. (v0에서는 단순화를 위해 proto field는 선택 기능으로 둘 수 있다)

추가 (proto field의 의미 보강, 추가):

* proto 안의 field 선언은 "실제 메모리 배치"가 아니라 "field로 향하는 접근 경로"로 해석할 수 있다.
* 즉, proto는 인터페이스이므로, proto의 field는 "이 인터페이스를 만족하는 구현체가 제공해야 하는 field 접근"을 뜻한다.
* 구현 관점에서 이는 "proto vtable 같은 인터페이스 객체 내부에 field에 대한 포인터/핸들 슬롯이 존재한다"는 의미로 해석할 수 있다.
* v0에서는 이 기능을 파싱만 제공하고, 의미 부여(ABI, 레이아웃)는 보수적으로 나중으로 미룰 수 있다.

예시

```gaupel
proto Drawable {
  fn draw() -> void;
}
```

### 10.3 tablet (implementation type)

정의:

* `tablet`은 구현체 타입이다. (생성자/소멸자 + 메서드 + proto 구현)
* 표기: `tablet Name [: Proto1, Proto2, ...] { ... }`
* v0 권장: **상속은 proto에 대해서만 허용**한다.

  * `tablet`끼리의 상속(필드/레이아웃 상속)은 v1+로 미룬다. (ABI/생성자 체인이 복잡해짐)

예시

```gaupel
proto Drawable {
  fn draw() -> void;
}

tablet Sprite : Drawable {
  public:
    let pos: Vec2;

    fn draw() -> void {
      // ...
    }
}
```

---

### 10.3.1 멤버와 접근 제한자: `public:` / `private:`

* `tablet` 본문은 “멤버 목록”이다.
* 멤버 종류(v0):

  * 데이터 멤버: `let name: Type;` 또는 `mut let name: Type;`(선택)
  * 메서드: `fn ... { ... }`
  * 생성자/소멸자: `construct`, `destruct` (아래 10.3.3)
* 접근 제한자:

  * C++ 스타일 `public:` / `private:` 라벨을 사용한다.
  * 라벨은 “이후 멤버들에 적용되는 모드”를 바꾼다.
  * v0 기본 접근은 `private`로 둔다.

예시

```gaupel
tablet A {
  let x: int;        // private

  public:
    fn get_x() -> int { return self.x; }

  private:
    fn helper() -> void { ... }
}
```

---

### 10.3.2 메서드의 `self` 규칙 (v0 단순/강제)

Gaupel은 borrow 설계가 있기 때문에, 메서드의 수신자(receiver)를 **명시적으로 단순화**한다.

* `tablet` 내부의 `fn name(...) -> R { ... }` 는 **항상 인스턴스 메서드**다.
* 메서드에는 암묵 수신자 `self`가 존재한다.
* v0에서 수신자 타입은 아래 둘 중 하나다.

  * `fn name(...)`  : `self`는 `&Self` (읽기 전용)
  * `fn mut name(...)` : `self`는 `&mut Self` (수정 가능)

즉, “메서드가 객체를 바꾸려면 반드시 `mut`를 써야 한다.”
이 규칙 하나로 C++의 암묵 변경 가능성 + Rust의 복잡한 추론 사이에서 깔끔하게 중간 지점을 잡는다.

예시

```gaupel
tablet Counter {
  public:
    mut let n: int;

    fn get() -> int {            // self: &Counter
      return self.n;
    }

    fn mut inc() -> void {       // self: &mut Counter
      self.n += 1;
    }
}
```

추가 규칙(v0):

* `self`는 예약 식별자다(키워드 취급 권장).
* `pure/comptime` 함수 안에서는 `fn mut` 메서드 호출을 금지할 수 있다(권장).
  (관측 가능한 상태 변경을 정적으로 차단)

---

### 10.3.3 생성자/소멸자: `construct`, `destruct` (오버로딩 금지 철학과 정합)

Gaupel은 “이름 오버로딩 금지” 철학이 있으므로, 생성자도 **단 하나만** 허용하는 게 깔끔하다.

* 생성자(선택):

  * `fn construct(params...) -> void { ... }`
* 소멸자(선택):

  * `fn destruct() -> void { ... }`

호출/동작:

* `set x = T(args...)` 는 `T.construct(args...)`를 호출해 `x`를 초기화한다.
* 스코프 종료 시 `destruct()`가 호출된다(존재한다면).
* `fn construct(...) = delete;` 로 생성을 금지할 수 있다.
* `fn destruct() = delete;` 는 v0에서는 **금지 권장**(파괴 금지 객체는 모델이 꼬임). 대신 “drop이 없는 handle” 같은 타입으로 해결.

예시

```gaupel
tablet File {
  public:
    let fd: int;

    fn construct(path: string) -> void {
      // open...
    }

    fn destruct() -> void {
      // close...
    }
}
```

---

### 10.3.4 proto 구현 규칙(정적 검사, v0 고정)

* `tablet X : ProtoA, ProtoB` 는 “X가 각 proto의 모든 메서드를 구현해야 함”을 뜻한다.
* 구현 체크는 prepass 이후 타입체커 단계에서 수행한다.
* 시그니처 일치 조건(v0):

  * 함수명 동일
  * 파라미터 타입 동일
  * 반환 타입 동일
  * `mut` 여부 동일 (`fn` vs `fn mut`)
  * `? 함수` 여부는 **proto에서는 v0 금지 권장**
    (예외 전파가 인터페이스 경계를 넘으면 ABI/최적화 모델이 확 흔들림)

---

### 10.3.5 proto 값/참조/핸들 사용 규칙(동적 디스패치 경계)

v0에서 proto는 “값으로 들고 다니는 타입”이 아니라 **참조/핸들로만 쓰는 계약**으로 고정하는 게 안전하다.

권장 규칙(v0):

* `ProtoType` 자체를 값으로 선언 금지:

  * `let d: Drawable;` 금지
* 허용:

  * `&Drawable` (non-escaping borrow로서의 다형성)
  * `handle<Drawable>` (escape 가능한 다형성)
* 업캐스트는 암묵 허용:

  * `handle<Sprite>`를 `handle<Drawable>` 파라미터에 전달 가능
  * `&Sprite`를 `&Drawable` 파라미터에 전달 가능

이 모델은 네가 이미 정의한 `handle`의 `(ptr, meta)` ABI와 잘 맞는다.

* `meta`를 “vtable 포인터”로 쓰면 된다.

---

## 10.4 `proto` (interface) — v0에서 더 단단하게 못 박기

너 문서의 proto 섹션에 이 한 줄만 추가해도 구현 난이도가 확 줄어:

* proto 내부 메서드는 기본이 `fn`(즉 `self: &Proto`)이고,
* 수정 가능한 메서드는 반드시 `fn mut`로 선언한다.

예시

```gaupel
proto Stream {
  fn read(buf: &mut [u8]) -> u32;
  fn mut seek(pos: u64) -> void;   // self: &mut Stream
}
```

---

## 11. acts: 행동 묶음과 타입 부착(메서드/연산자)

### 11.1 acts의 목적

* **field는 저장소, acts는 행동**이다. (field/tablet 내부에 “행동을 강제”하지 않고, 행동은 acts로 분리한다.)
* acts는 다음을 정의한다.

  1. **일반 함수(행동)**: 특정 기능의 표준 동작 집합을 제공
  2. **연산자(operator) 구현**: `+`, `==`, `++` 같은 토큰 연산의 의미를 타입별로 정의
* v0에서 acts는 “구조(타입)와 행동(함수/연산자)을 분리”하여

  * 파서/타입체커를 단순하게 유지하고
  * “어디에 메서드가 붙는지”를 명시적으로 드러내는 것을 목표로 한다.

---

### 11.2 acts의 두 형태: `acts A {}` vs `acts for A {}`

Gaupel v0에는 acts 블록이 **두 가지 형태**로 존재한다.

#### (1) 일반 acts: `acts A { ... }`

* `acts A {}`는 **A라는 이름의 acts 네임스페이스(행동 묶음)** 를 만든다.
* 이 블록 안의 선언은 **항상 스코프 호출(정적 호출)** 로 사용한다.

호출 예시:

```gaupel
acts Math {
  fn add(a: i32, b: i32) -> i32 { return a + b; }
}

fn demo() -> void {
  set x = Math::add(1, 2);
}
```

규칙:

* `acts A` 내부 함수는 **dot 호출 sugar의 대상이 아니다.**
* `acts A` 내부에 operator 선언은 불가능하다. (필요시 field에 타입을 정의하고 acts for로 규칙을 정의한다.)

#### (2) 부착형 acts: `acts for A { ... }`

* `acts for A {}`는 **타입 A에 행동을 부착**한다.
* 이 블록 안의 함수는 **dot 호출**과 **타입-스코프 호출(UFCS 스타일)** 둘 다 가능하다.

예시(메서드 형태):

```gaupel
acts for Packet {
  fn checksum(self a: &Packet, foo: i32) -> u32 { ... }
}

fn demo(p: Packet) -> void {
  set c = p.checksum(5);          // dot 호출 (리시버 암묵 전달)
  set d = Packet::checksum(p, 5); // 타입-스코프 호출 (UFCS)
}
```

---

### 11.3 export 규칙: “acts 블록 단위 export”만 존재

v0에서 export는 다음으로 고정한다.

* `export`는 **acts 블록 전체**에만 적용된다.
* acts 내부의 개별 함수/연산자만 export하는 기능은 **없다**.

예시:

```gaupel
export acts for Packet {
  fn checksum(self a: &Packet, foo: i32) -> u32 { ... }
  fn verify(self a: &Packet) -> bool { ... }
}
```

금지 예시(존재하지 않는 문법):

```gaupel
acts for Packet {
  export fn checksum(...) -> u32 { ... } // 금지: acts 내부 개별 export 없음
}
```

---

### 11.4 `acts for A`의 리시버(self) 규칙과 호출 규칙

#### (1) `self`는 “파라미터 이름”이 아니라 **리시버 마커(modifier)** 다

`acts for A` 내부에서 “이 함수가 A에 부착된 메서드이며, 첫 인자가 리시버다”를 표시하려면 아래 형태를 쓴다:

```gaupel
fn checksum(self a: &A, foo: i32) -> u32 { ... }
```

여기서

* `self` : 예약된 **리시버 마커** (컴파일러가 강제)
* `a` : 유저가 고르는 **진짜 파라미터 이름** (라벨 슬롯 자유)
* `&A`, `&mut A`, `A` 등은 리시버의 전달 방식을 의미

**강제 규칙(v0):**

* `self` 마커는 **반드시 첫 번째 파라미터 앞**에만 올 수 있다.
* `self`가 붙은 파라미터는 **반드시 타입이 A(또는 &A / &mut A 등 A 기반)** 이어야 한다.
* `self`가 붙은 함수만이 “메서드(리시버 보유)”로 취급된다.

#### (2) dot 호출: 리시버 인자는 **암묵 전달(첫 인자 생략)**

다음이 정확히 동일 의미다.

```gaupel
myA.checksum(5);
A::checksum(myA, 5);
```

즉, dot 호출은

* `myA.checksum(5)`  ==  `A::checksum(myA, 5)` 로 lowering 된다.

#### (3) “self가 없는 함수”는 메서드가 아니며, 호출은 자유

`acts for A` 안에 있어도 `self`가 없으면 그 함수는 “부착된 유틸리티(정적 함수)”로 취급한다.

```gaupel
acts for A {
  fn foo(x: i32, y: i32) -> i32 { ... }  // self 없음
}
```

호출은 아래 둘 다 허용:

```gaupel
set r1 = A::foo(1, 2);
set r2 = myA.foo(1, 2);  // 허용: dot-정적 호출(문법 설탕)
```

v0 권장 해석(단순 규칙):

* `myA.foo(1,2)`는 단순히 `A::foo(1,2)`로 lowering 된다.
  (리시버 전달/오버로드 같은 추가 의미는 없다.)

---

### 11.5 `acts for A` 내부 함수의 “A 인자 요구” 규칙

`acts for A` 안에 정의된 함수가 “A를 인자로 받는 동작”이라면 **반드시 `self`를 사용**해 메서드로 선언한다.

예:

```gaupel
acts for A {
  fn checksum(self a: &A, foo: i32) -> u32 { ... }
}
```

호출:

```gaupel
A myA;
set c = myA.checksum(5);
set d = A::checksum(myA, 5);
```

반대로, `acts for A` 안에 있어도 A를 인자로 받지 않는다면 `self`를 쓰지 않는다:

```gaupel
acts for A {
  fn make_seed(x: i32, y: i32) -> u64 { ... } // A 인자 없음
}
```

호출:

```gaupel
set s1 = A::make_seed(1, 2);
set s2 = myA.make_seed(1, 2); // 허용(정적 dot)
```

---

### 11.6 연산자 오버로딩: `operator(...)`만 유일하게 존재 (기존 op("TOKEN") 완전 폐기)

v0에서 연산자 정의 방식은 **오직 하나**만 존재한다.

* 기존의 `: op("TOKEN")` 매핑 방식은 **언어에서 삭제**된다.
* 연산자는 항상 다음 형태로 선언한다:

```gaupel
operator(+)(self a: A, rhs: A) -> A { ... }
operator(==)(self a: &A, rhs: &A) -> bool { ... }
operator(++pre)(self x: &mut A) -> A { ... }
operator(++post)(self x: &mut A) -> A { ... }
```

#### (1) operator 선언은 `fn` 키워드를 붙이지 않는다

* `operator(...)`는 acts 내부의 **특수 선언**이며, 함수 선언이지만 문법적으로 `fn`을 사용하지 않는다.

#### (2) operator는 **`acts for Type`에서만 허용**

* `operator(...)` 선언은 **반드시** `acts for T { ... }` 블록 내부에만 올 수 있다.
* `acts X { ... }`(일반 acts) 내부에서 operator 선언은 **금지**한다.

#### (3) operator의 “리시버(self)”는 항상 존재한다

* 모든 operator 선언은 “해당 타입에 바인딩되는 연산”이므로, 첫 파라미터는 항상 `self` 리시버 마커를 사용한다.
* 단항/이항/증감은 파라미터 개수로 구분한다.

#### (4) `++`는 `(++pre)`, `(++post)`만 존재한다

증감 연산은 토큰을 직접 쓰지 않고, 바인딩 키를 아래처럼 고정한다.

* `operator(++pre)` : `++x`
* `operator(++post)` : `x++`

예시:

```gaupel
acts for i32 {
  operator(++pre)(self x: &mut i32) -> i32 {
    x = x + 1;
    return x;
  }

  operator(++post)(self x: &mut i32) -> i32 {
    set old = x;
    x = x + 1;
    return old;
  }
}
```

---

### 11.7 연산자 해석(리졸브) 규칙 요약 (v0)

표현식에서 연산자를 만나면 타입체커는 다음 순서로 해석한다.

* 피연산자 타입이 `T`라면, 컴파일러는 `acts for T`에서 해당 `operator(KEY)`를 찾는다.
* 후보가 없으면 에러.
* 후보가 2개 이상이면 에러(모호성 금지).

v0에서는 다음을 권장/강제한다.

* 같은 `acts for T` 안에서 동일한 `operator(KEY)`는 **정확히 1개만** 허용
* “연산자 오버로드”로 인한 이름 충돌/다중 후보는 전부 컴파일 에러로 처리

---

### 11.8 예시 모음 (v0 스타일)

#### (1) 타입 부착 메서드 + UFCS

```gaupel
export field Packet {
  u32 len;
  u32 crc;
}

export acts for Packet {
  fn checksum(self p: &Packet, seed: u32) -> u32 {
    // ...
    return p.crc + seed;
  }
}

fn demo(pkt: Packet) -> void {
  set a = pkt.checksum(5u32);
  set b = Packet::checksum(pkt, 5u32);
}
```

#### (2) self 없는 유틸 함수(정적 dot 허용)

```gaupel
export acts for Packet {
  fn make(seed: u32, len: u32) -> Packet {
    // ...
    set p = Packet{ len: len, crc: seed };
    return p;
  }
}

fn demo2() -> void {
  set p1 = Packet::make(3u32, 10u32);
  set p2 = p1.make(3u32, 10u32); // 허용: Packet::make(...)로 lowering
}
```

#### (3) operator 정의

```gaupel
export acts for u32 {
  operator(+)(self a: u32, rhs: u32) -> u32 { return __intrin_u32_add(a, rhs); }
  operator(==)(self a: u32, rhs: u32) -> bool { return __intrin_u32_eq(a, rhs); }
}
```

---

## 12. 람다/콜백 (전역 람다 금지)

### 12.1 람다 문법 (함수 스코프 전용)

* [captures] (params...) { body }
* capture: name 또는 mut name

타입:

* 캡처 없는 함수 포인터: func<Ret(Args...)>
* 캡처 가능한 클로저 값: closure<Ret(Args...)> (move-only 권장)

예시

```gaupel
fn lambdas() -> void {
  set f = [](x: int) { return x + 1; };
  set y = f(3);
}
```

### 12.2 람다 금지사항

* 람다는 함수 스코프 내부에서만 생성/존재 가능
* 전역에는 closure 값을 둘 수 없다
* 전역 콜백은 func<...>만 허용하거나 class draft 슬롯에 저장
* borrow 값(&T, &mut T)의 캡처
* borrow를 인자로 받는 클로저는 “즉시 호출” 같은 특별 규칙 없이 그냥 금지(v0)

예시

```gaupel
// v0: 전역에 closure 금지
// set g = [](x: int) { return x; }; // error
```

---

## 13. 심볼, ABI, 디버깅, 맹글링 규칙

### 13.1 오버로딩 규약(라벨 포함)

Gaupel은 함수 오버로딩을 허용한다. 단, **라벨 인자 이름도 시그니처에 포함**된다.

시그니처 유일성 키(v0):

* 함수의 전체 경로: `module::(space...)::name`
* 파라미터 개수(arity)
* 각 파라미터의 **타입**
* 각 파라미터의 **라벨 이름**
* 반환 타입
* (권장) `sub/pub` 모드도 시그니처에 포함 (상태 접근 의미가 달라 ABI/규칙이 달라질 수 있음)

호출 해소 규칙:

* 후보 집합을 모은 뒤,
* “타입 + 라벨”이 정확히 1개에만 매칭되면 선택
* 0개면 not found, 2개 이상이면 ambiguous 에러

---

### 13.2 맹글링 규칙(사람 친화 + 안정 링크)

목표:

* 디버거에서 원 이름이 읽히도록 유지
* 링크 충돌 방지 + 오버로딩 지원

권장 심볼 포맷(v0):

```
<ModuleId>__<Path>__<BaseName>__P<ParamSig>__R<RetSig>__H<Hash>
```

* `ModuleId`: 모듈 경로를 정규화한 짧은 ID(예: `engine_core`)
* `Path`: `space` 경로를 `__`로 연결 (예: `math__vec`)
* `ParamSig`: 파라미터를 좌=>우 순서로 나열, 각 항목은 `label$type`

  * 예: `a$i32_b$i32`
* `RetSig`: 반환 타입 (예: `i32`, `u32`, `void`)
* `Hash`: 위 정보를 canonical 문자열로 만든 뒤 짧은 해시(충돌 방지)

예시:

* `engine_core__math__add__Pa$i32_b$i32__Ri32__H9f2c1a`
* 오버로딩(라벨이 다르면 다른 심볼):

  * `...__Pa$x_i32__...` vs `...__Pleft$i32_right$i32__...`

추가 규칙(v0 권장):

* `export`된 심볼은 기본적으로 위 규칙으로 맹글링된다.
* “정확히 이 이름으로 외부 ABI에 노출”이 필요하면(FFI 등),

  * 별도 어트리뷰트(예: `@cabi`)로 “no-mangle export”를 제공하는 것을 권장한다.
  * (이건 지금 문서에 “확장 포인트”로만 남겨도 됨)

---

## 14. 구현 체크리스트 (v0)

### 14.1 프리패스

* #define 텍스트 치환
* use module 해석, alias 심볼 테이블 구성
* 모듈을 컴파일 단위로 취급하는 의존성 그래프 구성 (추가)
* use ...::ffi 수집
* F-string 분절 (문자열 내부 expr 파싱 준비)

### 14.2 파서

* let/set/mut 바인딩
* while, loop(iter: i in expr)
* loop(for: ...) (추가)
* range a..b, a..:b
* pipe << 와 hole _ 규칙 진단
* field/proto/tablet/acts/class 파싱

  * field<T> 파싱 (추가)
* public/private 섹션 파싱
* commit/recast 문장 파싱

  * class 모드 제약은 타입체커에서
* x++ / ++x 파싱 (추가)
* 오버로딩 허용에 따른 선언 수집/디스패치 기반 마련 (추가)

### 14.3 타입체커

* if/while cond bool 강제
* loop(iter) iterable 타입 확인
* loop(for) range 타입 확인 (정수 범위 또는 정해진 범위 타입) (추가)
* 삼항 중첩 금지 검사
* class sub/pub 제약
* pub 최종 commit 검사
* pure/comptime 제약

  * pure/comptime에서 recast 금지 포함 (추가)
* acts 기반 연산자 해석
* 오버로딩 해소:

  * 호출 시 후보 집합을 구성
  * 정확히 1개 매칭이면 선택
  * 0개면 not found, 여러 개면 ambiguous 에러
* commit/recast를 제어 구문으로 취급하는 흐름 모델 반영

---

## 15. 종합 예시 (여러 기능 한 번에)

```gaupel
field Vec2 {
  float32 x;
  float32 y;
}

field<u32, i32> OnlyInts {
  u32 a;
  i32 b;
}

proto Drawable {
  fn draw() -> void;
}

tablet Sprite : Drawable {
  public:
    let pos: Vec2;

    fn draw() -> void {
      // draw using pos
    }

  private:
    let secret: int;
}

acts for u32 {
  operator(+)(self: u32, rhs: u32) -> u32 {
    return __intrin_u32_add(a: self, b: rhs);
  }

  operator(==) -> u32 {
    return __intrin_u32_eq(a: self, b: rhs);
  }
}

class Counter {
  fn sub get() -> u32 {
    // 필요하면 관찰 뷰를 재설정
    recast;
    return draft.count;
  }

  fn pub inc() -> void {
    draft.count += 1u32;
    commit;
  }
}

fn f(a: int) -> int { return a; }

fn f(a: int, b: int) -> int { return a + b; }

fn main() -> void {
  let xs: u32[3] = [1u32, 2u32, 3u32];

  mut set s = 0u32;
  loop(iter: v in xs) {
    s = s + v;
  }

  // loop(for: ...) 예시 (명시 이름 버전)
  mut set t = 0;
  loop(for: i in 1..:5) {
    t = t + i;
  }

  mut set x = 10;
  set y = x++;
  set z = ++x;

  if (s == 6u32) {
    // ok
  }

  set a = f(a: 1);
  set b = f(a: 1, b: 2);
}
```

---


# 주의! 언어 스펙 변경으로 인해 이전버전의 EBNF와 호환되지 않음. 본문 내용의 정보를 우선적으로 신뢰할 것.
## 16. EBNF (v0 핵심 요약, 구현 기준)

아래는 구현을 위한 핵심 뼈대다. 상세 확장은 문서의 문법 설명을 따른다.

```ebnf
CompilationUnit = { TopLevelDecl } eof ;

TopLevelDecl =
    DefineDecl
  | ImportDecl
  | UseFFIDecl
  | FieldDecl
  | ProtoDecl
  | TabletDecl
  | ActsDecl
  | ClassDecl
  | FuncDecl
  ;

DefineDecl = "#define" Ident StringLit ;

ImportDecl = "use" "module" ( AbsPath | StringLit ) "as" Ident ";" ;

UseFFIDecl =
    "use" "func::ffi" "<" FfiSig ">" Ident ";"
  | "use" "struct::ffi" Ident "{" FieldList "}" ;


FuncDecl = ReturnsDecl "fn" { Qualifier } Ident "(" [ ParamList ] ")" [ Mode ] Block ;
Qualifier = "pure" | "comptime" ;
Mode = ":" ( "sub" | "pub" ) ;

ParamList = Param { "," Param } ;
Param = [ "mut" ] Ident ":" Type ;

FieldDecl = "field" [ "<" TypeList ">" ] Ident "{" FieldMembers "}" ;
TypeList = Type { "," Type } ;

ProtoDecl = "proto" Ident "{" { ProtoMember } "}" ;
ProtoMember = ReturnsDecl "fn" Ident "(" [ ParamList ] ")" ";" ;

TabletDecl = "tablet" Ident [ ":" Ident ] "{" { TabletSection } "}" ;
TabletSection = AccessLabel ":" { TabletMember } ;
AccessLabel = "public" | "private" ;
TabletMember = FuncDecl | VarDecl ;

ActsDecl = "acts" TypeName "{" { ActsMember } "}" ;
ActsMember = FuncDecl [ OpBind ] ;
OpBind = ":" "op" "(" StringLit ")" ;

ClassDecl = "class" Ident "{" { ClassMember } "}" ;
ClassMember = FuncDecl | VarDecl ;

VarDecl =
    "let" Ident ":" Type "=" Expr ";"
  | "set" Ident "=" Expr ";"
  | "mut" "let" Ident ":" Type "=" Expr ";"
  | "mut" "set" Ident "=" Expr ";"
  ;

Stmt =
    VarDecl
  | IfStmt
  | SwitchStmt
  | WhileStmt
  | LoopIterStmt
  | LoopForStmt
  | ReturnStmt
  | BreakStmt
  | ContinueStmt
  | CommitStmt
  | RecastStmt
  | ExprStmt
  | DeleteStmt
  ;

WhileStmt = "while" "(" Expr ")" Block ;

LoopIterStmt = "loop" "(" "iter" ":" Ident "in" Expr ")" Block ;

// loop(for:)는 v0에서 2가지 형태 중 하나 또는 둘 다 선택 구현 가능
LoopForStmt =
    "loop" "(" "for" ":" Ident "in" RangeExpr ")" Block
  | "loop" "(" "for" ":" RangeExpr ")" Block
  ;

RangeExpr = Expr ".." [ ":" ] Expr ;

CommitStmt = "commit" ";" ;
RecastStmt = "recast" ";" ;

// Expr 내에 x++ / ++x 같은 증감 연산자 규칙이 추가된다 (상세는 본문 규칙 참조)
```