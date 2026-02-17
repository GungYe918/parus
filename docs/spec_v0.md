---

# Parus v0 Language Specification (Single Reference, Upgraded)

본 문서는 Parus v0를 구현하기 위한 단일 레퍼런스이다. 이 문서만으로 토크나이저, 프리패스, 파서, 타입체커 v0를 구현할 수 있도록 설계 철학, 정적 규칙, 문법, 예시 코드를 포함한다.

주의: 이 문서는 한국어로 작성되며, 한국어 외 문자는 ASCII 및 영문자만 사용한다. 단, Parus이 UTF-8 친화적이라는 점을 보여주는 예시에서는 임의의 이모지 등을 포함한다.

ABI/FFI 정본 우선순위:

* ABI/FFI 관련 규칙의 단일 정본은 `docs/abi/v0.0.1/ABI.md`다.
* 본 문서의 ABI/FFI 항목은 요약/연결 목적이며, 충돌 시 `ABI.md`를 우선 적용한다.
* 문자열 타입/변환/C 경계 타입 정본은 `docs/abi/v0.0.1/STRING_MODEL.md`다.
* 스택/힙/정적 저장소 및 `&&` 비힙 규칙 정본은 `docs/abi/v0.0.1/STORAGE_POLICY.md`다.
* 본 문서의 과거 `string` 표기는 구버전 예시일 수 있으며, 우선 규칙은 `text`(빌트인) + 표준 라이브러리 `String`을 따른다.

---

## 0. 설계 목표와 철학

### 0.1 핵심 목표

* SMP + SIMD 친화: 병렬 처리와 벡터화에 유리한 데이터/제어 구조를 제공한다.
* 명시적 코드 플로우: 암묵 복사, 암묵 삽입, 암묵 실행을 최소화해 비용과 흐름이 코드에서 드러나게 한다.
* 대단위 상태 공유 생산성: Rust의 단일 소유권이 유발하는 빌림/복사 폭증을 완화한다.
* 순환참조 문제 실질적 제거: 포인터 기반 객체 그래프 대신 Handle+Store, ECS 패턴을 강권한다.
* 기본적으로 모든 연산은 move기반이며, 복사 시 명시적으로 copy/clone을 요구한다.

예시 (철학이 반영된 스타일, 작은 데이터는 값, 큰 상태는 class)

```parus
@pure
export def demo_small_big() -> void {
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

```parus
class Counter {
  // draft.count 라는 큰 상태를 가진다고 가정

  def sub get() -> int {
    return draft.count;
  }

  def pub inc() -> void {
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

```parus
export field Vec2 {
  float32 x;
  float32 y;
}

proto Drawable {
  def draw() -> void;
}

tablet Sprite : Drawable {
  public:
    let pos: Vec2;
    def draw() -> void { /* ... */ }
}
```

---

## 1. 문자, 토큰, 기본 규칙

### 1.1 공백, 개행, 세미콜론, 블록

* 공백/개행은 토큰 분리 외 의미가 없다.
* 모든 문장은 세미콜론 ; 으로 끝난다. (block 자체는 문장 종결자를 요구하지 않는다)
* 스코프는 반드시 { } 블록을 사용한다. 들여쓰기는 의미 없다.

예시

```parus
export def basic_blocks() -> void {
  set a = 1;
  if (a == 1) { set b = 2; }
}
```

### 1.2 주석

* 라인 주석: // ...
* 블록 주석: /* ... */ (중첩 불가)

예시

```parus
def comments() -> void {
  // line comment
  /* block comment */
  set x = 1;
}
```

### 1.3 식별자 (UTF-8 friendly)

Parus은 UTF-8 친화적인 토크나이저/파서를 목표로 한다.

v0 권장 규칙:

* 식별자는 Unicode XID_Start / XID_Continue 규칙을 따른다.
* ASCII만 쓰는 프로젝트도 지원한다.
* 키워드와 충돌하는 식별자는 금지한다.
* 이모지를 사용할 경우 백틱 사이에 넣어야 한다. 

예시 (UTF-8 식별자, 이모지 포함)

```parus
def utf8_identifiers() -> void {
  let 한글이름: int = 3;
  let `🍣`: int = 7;
  set 합 = 한글이름 + `🍣`;
}
```

### 1.4 키워드 (v0 핵심)

* 선언/구조: field, proto, tablet, acts, class, def, let, set, mut, &, &&
* 제어: if, elif, else, switch, case, default, while, loop, break, continue, return
* 상탯값: true, false, null
* 논리: and, or, not, xor
* 시스템: import, use, nest, extern, export, layout, align, commit, recast
* 접근 제한: public, private
* 메모리/수명/복사: **copy, clone, delete**
* slice 관련 타입 표기(토큰이 아니라 타입 표기): **[T]**, **&[T]**, **&mut [T]**
* 논리 연산자 토큰 확정: **and, or, not, xor** (기존 `&&`, `||`는 금지/비권장; `&&`는 escape로 고정)

주의: `delete`는 두 의미를 가진다.
(1) tablet 생성자/소멸자 삭제 문법의 `= delete;` (기존 유지)
(2) 아래 8.6에서 정의하는 “명시적 파괴(delete 문장)” 키워드 (신규)
두 용도는 문맥이 달라 충돌하지 않는다.


예시

```parus
def keywords_demo() -> void {
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

```parus
def int_literals() -> void {
  let a: u32 = 123u32;
  let b: u64 = 1_000_000u64;
}
```

### 2.2 실수 리터럴

* 접미사 필수:

  * f: float32
  * lf: float64

예시

```parus
def float_literals() -> void {
  let x: float32 = 3.14f;
  let y: float64 = 2.71828lf;
}
```

### 2.3 불리언, null

* true, false
* null은 그 자체로만 존재하며 자동 변환이 없다.

예시

```parus
def bool_null() -> void {
  let t: bool = true;
  let f: bool = false;
  // let x: int = null; // error
}
```

### 2.4 문자 리터럴 (char)

* 형태: 'a', '\n', '\t', '\', ''', '"'
* 유니코드 코드포인트 표기(권장): '\u{AC00}' 같은 형태를 지원한다.

예시

```parus
def char_literals() -> void {
  let c1: char = 'a';
  let c2: char = '\n';
  let c3: char = '\u{AC00}';
}
```

### 2.5 문자열 리터럴

* 기본 문자열: `"..."` (v0 활성)
* 기본 문자열 리터럴의 타입은 항상 `text`다.
* Raw/F-string 문법은 v0에서 예약 상태다.
* `R"""..."""`, `F"""..."""`는 lexer/parser에서 문자열 리터럴로 활성화한다.
* `F"""..."""`의 `{expr}` 보간 lowering은 v0에서 미구현이며, 현재는 본문 텍스트를 그대로 `text`로 취급한다.
* 표준 라이브러리 `String`이 링크되어도 `""` 기본 타입은 바뀌지 않는다.
* `text -> String` 암시 변환은 금지한다(명시 API 변환만 허용).

예시

```parus
def strings() -> void {
  let raw_name: text = "parus";
  // let owned: String = String::from_text(src: raw_name); // std 링크 후 명시 변환
}
```

---

## 3. 프리패스와 단위 계층, `import`/`use`, FFI

### 3.1 단위 용어: file / module / bundle / project

Parus 빌드는 다음 4단위를 가진다.

* **file**: 하나의 소스 파일(`*.pr`). 파싱 단위이다.
* **module**: bundle 내부의 논리 코드 그룹(심볼 경로/가시성 단위).
* **bundle**: Rust crate 대응 컴파일/배포 단위. `parus.toml`을 가진다.
* **project**: 여러 bundle을 묶는 최상위 단위(workspace).

권장 계층:

```text
file -> module -> bundle -> project
```

핵심 규칙(전방선언 문제 제거):

* Parus은 **bundle 단위 선언 수집 prepass**를 수행한다.
* 따라서 같은 bundle 내부에서는 함수/타입/acts 선언 순서가 의미를 갖지 않는다.
* 같은 bundle 안에서는 파일 A가 파일 B의 뒤 선언 심볼을 전방선언 없이 참조할 수 있다.
* bundle 외부로 노출되는 심볼은 `export`로만 결정된다.

### 3.1.1 의존성 선언: manifest 우선

의존성은 소스 코드가 아니라 bundle manifest(`parus.toml`)에 선언한다.

```toml
[package]
name = "app"

[dependencies]
foo = { path = "../foo" }
goo = { path = "../goo" }
```

소스 코드의 `import`는 manifest에 선언된 의존성 키를 활성화하는 용도다.

### 3.1.2 `import` 문 (의존성/경로 alias 도입)

문법:

```parus
import foo;
import goo as g;
import foo::net as fnet;
```

규칙(v0 고정):

* `import`는 **include가 아니다**. 현재 스코프에 alias 심볼 1개를 도입한다.
* alias 생략 시 마지막 경로 세그먼트를 alias로 사용한다.
* 외부 심볼은 자동 평탄화되지 않는다. 항상 `alias::...`로 접근한다.
* `import`는 파일 스코프에서만 허용한다.
* `use module ...` 문법은 즉시 폐기한다.

심볼 접근 예시:

```parus
import foo;
import goo as g;

def demo() -> void {
  foo::math::add(a: 1, b: 2);
  g::io::print(msg: "ok");
}
```

### 3.1.3 `use` 문 (로컬 alias/치환 전용)

`use`는 로컬 축약과 치환에만 사용한다.

1. 타입 별칭

```parus
use NewT = u32;
```

2. 경로 축약(alias)

```parus
use foo::math::add as add_i32;
use foo::io::print = println;
```

3. 파일 스코프 상수/치환

```parus
use PI 3.14f;
use GAME_NAME "Parus";
```

제약(v0 강제):

* `use`는 예약어/토큰을 바꾸지 않는다.
* `use NAME expr;` 치환 대상은 식별자(IDENT)만 가능하다.
* 함수형 매크로(인자), 토큰 결합/분해는 금지한다.

### 3.1.4 `nest` 네임스페이스

`space` 키워드는 폐기하고 `nest`로 대체한다.

문법:

```parus
// 파일 기본 네임스페이스 지시어
nest engine::math;

// 블록형 선언
nest engine {
  nest math {
    export def add(a: i32, b: i32) -> i32 { return a + b; }
  }
}
```

규칙(v0):

* `nest` 내부 선언은 `a::b::c` 경로로 식별된다.
* `nest foo;` 파일 지시어는 파일당 1회만 허용한다.
* `nest` 내부에 `use`를 둘 수 있다(lexical alias).
* `import`는 파일 스코프에서만 허용한다.

### 3.2 `.` / `::` 접근 규칙

Parus v0는 경로 접근과 값 접근을 분리한다.

* `::`는 **경로(path) 접근 전용**이다.
  * `foo::math::add`
  * `Type::factory`
* `.`는 **값(value) 접근 전용**이다.
  * `obj.field`
  * `obj.method(...)`

금지 규칙:

* `obj::field` 금지
* `alias.obj` 금지
* 경로 접근에 `.` 사용 금지, 값 접근에 `::` 사용 금지

호출 lowering 규칙(v0):

* `obj.method(x)`는 메서드 해소 후 UFCS 형태로 lowering 가능하다.
* `Type::method(obj, x)`와 의미가 같더라도 표면 문법 규칙(`.` vs `::`)은 유지한다.

### 3.3 FFI 선언

* ABI/FFI 관련 정본은 `docs/abi/v0.0.1/ABI.md`를 따른다.
* 본 절은 `ABI.md`의 요약이며, 충돌 시 본 절 해석을 중단하고 `ABI.md`를 적용한다.
* `use func::ffi`, `use struct::ffi` 문법은 폐기한다.
* FFI 경계 함수/전역은 `extern "C"` / `export "C"`로 선언한다.
* FFI는 ABI 경계이므로 pure/comptime에서 기본 금지다. (타입체커 규칙)

예시

```parus
extern "C" def c_add(a: i32, b: i32) -> i32;
extern "C" static mut errno: i32;

export "C" def p_add(a: i32, b: i32) -> i32 {
  return a + b;
}
```

### 3.3.1 FFI-safe 타입 (`c-v0`)

허용:

* 정수: `i8/i16/i32/i64`, `u8/u16/u32/u64`, `isize/usize`
* 부동소수: `f32/f64`
* 포인터: `ptr T`, `ptr mut T` (T가 FFI-safe일 때)
* `layout(c)`를 만족하는 `field`

금지:

* borrow/escape (`&`, `&mut`, `&&`)
* optional, class, tablet 직접 값 전달
* 구현 의존 내부 타입

### 3.3.2 Parus의 개념이 C에서 어떻게 대응되는가

* `field` -> `struct` (C 호환 레이아웃)
* `handle<T>` -> `void*`(또는 `struct Handle_T*`) + 생성/해제 함수 세트
* `tablet` -> C에서 직접 값으로는 못 다루고, 반드시 `handle<tablet>`로만 다룬다
* `class`/`draft`/`pub-sub` -> **FFI로 직접 노출하지 않는다**
  대신:

  * “C가 호출할 수 있는 pub/sub wrapper 함수”를 제공하고,
  * 상태 접근은 wrapper 내부에서만 일어나게 한다.

예:

```parus
class Counter {
  def inc() : pub { draft.count += 1u32; commit; }
}

def counter_inc_ffi(h: handle<Counter>) -> void {
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

이렇게 하면 “Parus 의미론”을 타 언어로 억지로 이식하지 않고, ABI는 깔끔하게 유지된다.

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
* bool, char, text
* 별칭(구현 선택): int = int32, float = float32

예시

```parus
def primitives() -> void{
  let a: int32 = 1i32;
  let b: uint64 = 2u64;
  let c: bool = true;
  let d: char = 'Z';
  let s: text = "hi";
}
```

### 4.2 nullable 타입 (T?)

* null은 그 자체로만 존재한다.
* nullable은 T? 로 표기한다.
* non-nullable T 에 null을 대입/반환/전달하면 에러다.
* if (x == null) 비교는 T? 에서만 허용한다.

예시

```parus
def nullable_demo() -> void {
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

```parus
def arrays_lists() -> void {
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
* 제네릭 함수: def foo<T>(...) ...
* 제네릭 특수화와 monomorphization (컴파일 시 실체화) 혹은 제한된 형태의 dictionary passing 중 택1
* 타입 추론 강화: set 바인딩의 추론, 간단한 반환 타입 추론 (v0에서는 강제 유지)
* trait 비슷한 개념을 acts/proto/tablet 조합으로 자연스럽게 확장

예시 (향후 제네릭의 목표 형태, v0에서는 파싱만 선행 가능)

```parus
// v1+ 목표 예시
// field Vec<T> { T x; T y; }
```

### 4.5 storage class 키워드: `static`

Parus v0에서는 “값이 어디에 저장되고(저장소/수명), 소유권이 어떻게 제한되는지(유일성)”를 명시적으로 표현하기 위해 `static`을 제공한다.

* `static` : **정적 저장소(static storage)** 를 의미한다. 스코프를 벗어나도 저장이 유지되며(프로그램 수명), 해당 place의 주소/저장 위치는 컴파일 타임에 고정된다.

#### (1) 문법(권장)

`static`은 변수/전역/필드 선언에 붙는 **storage class**로 취급한다.

```parus
static G: i32 = 3i32;
static mut UART0: Uart? = null;
```

권장 순서(가독성 규칙):

```
static mut Name: T = Init;
```

*의미론상 순서는 중요하지 않으나, v0 문서/스타일은 위 순서를 권장한다.*

#### (2) `static`의 의미(저장 기간/초기화)

v0 권장(= freestanding 친화) 규칙:

* `static` 선언은 **반드시 초기화 값을 가져야 한다.**
* `static`의 초기화 표현식은 **`comptime`에서 평가 가능한 형태로 제한**하는 것이 권장된다.

  * 예: 리터럴, 배열/튜플/field 리터럴, `null`, `comptime` 함수 호출(허용하는 구현 한정)
  * 금지(권장): 런타임 함수 호출, 힙 할당, I/O, FFI 등 “동적 초기화”가 필요한 것

이 규칙은 다음을 보장한다.

* `no-std` / freestanding 환경에서 런타임 초기화 의존을 만들지 않는다.
* 초기화 순서/재진입 문제를 최소화한다.

#### (3) `T?`(nullable)와 `static/unique`의 결합 패턴(v0 권장)

freestanding에서 “나중에 초기화”가 필요한 전역 자원은 다음 패턴을 권장한다.

```parus
static mut LOGGER: Logger? = null;

def init_logger() -> void {
  LOGGER = Logger::init(); // 런타임 초기화(명시적)
}
```

* `static` 초기값으로는 `null`을 사용(상수 초기화).
* 이후 초기화는 명시적 init 단계에서 수행.

#### (5) 주의(동시성/재진입)

`static mut`는 데이터 레이스/재진입 문제가 생길 수 있다. v0에서는 동시성 모델을 최소화할 수 있으므로, 구현/프로젝트 정책으로 다음 중 하나를 택해 고정하는 것을 권장한다.

* 단일 스레드/단일 코어 가정(예: bare-metal 초기 단계)
* `critical` 같은 구문/프리미티브 내부에서만 `static mut` 접근 허용(미래 확장)

이 규칙은 `static` 자체의 의미와는 별개이며, 접근 안전성 정책이다.

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

```parus
def bindings() -> void{
  let a: int = 1i32;
  set b = a + 2;
  set mut c = 0;
  c = c + 1;
}
```

### 5.2 스코프와 섀도잉

* 같은 스코프에서 동일 이름 재선언은 금지(권장). 구현이 쉬워진다.
  단, 함수는 6.1.7의 오버로딩 규약을 만족하면 같은 이름 공존을 허용한다.
* 다른 스코프에서는 허용 가능하다.

예시

```parus
def shadowing() -> void {
  set x = 1;
  if (x == 1) {
    set x = 2; // 다른 스코프, 허용 가능(정책 선택)
  }
}
```

---

## 6. 함수 선언: @attribute, qualifier, 호출 규칙, non-?` / `?` 함수, 예외 허용 범위, 호출 제약

## 6.1 함수 선언 기본형 (확장판, v0 기준)

Parus의 함수는 **(1) 선언 형식이 단순**하면서도, **(2) 호출 해소가 100% 정적(컴파일 타임)**으로 끝나도록 설계한다.
특히 Parus은 **“라벨 이름도 시그니처(오버로딩 키)에 포함”**되므로, named-parameter의 편의성을 넣어도 호출 해소를 흐리지 않도록 규칙을 강하게 고정한다.

---

### 6.1.1 선언 헤더(머리) 구성

함수 선언은 다음 요소들로 구성된다.

* `@attribute` : 0개 이상 (예: `@pure`, `@comptime` 등)
* `[export]` : 선택
* `def`
* `[mode]` : 선택 (`sub`, `pub` 등. class 문맥에서 의미 있음)
* `[qualifier]` : 선택 (`pure`, `comptime` 등. 또는 `@pure` 같은 attribute로만 둘 수도 있음)
* `Name`
* `(...)` : 파라미터 목록 (아래 6.1.2~)
* `-> ReturnType`
* `Block`

**권장 고정 문법(요약)**

```ebnf
FuncDecl :=
  Attribute* ExportOpt "def" ModeOpt QualifierOpt Ident FuncParams "->" Type Block

ExportOpt := "export" | ε
ModeOpt   := "sub" | "pub" | ε
QualifierOpt := "pure" | "comptime" | ε   // (또는 attribute로만 두는 정책도 가능)

FuncParams := "(" PositionalParamsOpt NamedGroupOpt ")"
```

---

### 6.1.2 파라미터 모델: “위치 파라미터” + “named group 파라미터”

Parus은 파라미터를 **두 구역**으로 나눈다.

1. **위치 파라미터(positional params)**
2. **Dart식 named parameter group `{ ... }`**

함수 선언에서 파라미터는 아래 형태를 가진다.

#### (A) 위치 파라미터(기존)

* 형태: `name: Type` 또는 `name: Type = DefaultExpr`
* 순서가 의미를 가진다.
* 호출에서는 `f(1, 2)`처럼 위치 인자로 매칭 가능.

```ebnf
PositionalParam := Ident ":" Type DefaultOpt
DefaultOpt := "=" Expr | ε
PositionalParamsOpt := (PositionalParam ("," PositionalParam)*)? 
```

#### (B) named parameter group `{ ... }` (신규, Dart 스타일)

* 형태: `{ label: Type, label2: Type = DefaultExpr, ... }`
* **이 그룹에 들어간 파라미터는 “라벨로만” 전달 가능**하다.
* 순서가 의미가 없다.
* **기본값이 있으면 생략 가능**, 기본값이 없으면 v0에서는 기본적으로 “필수”로 간주(아래 6.1.4).

```ebnf
NamedGroupOpt := (","? "{" NamedParam ("," NamedParam)* "}")? | ε
NamedParam := Ident ":" Type DefaultOpt
```

> **중요:** `{}` 그룹은 “문법적으로 분리된 구역”이기 때문에, “혼합 호출 금지” 원칙을 유지하면서도
> “positional + named” 조합을 **명시적으로** 허용할 수 있다(아래 6.1.5 참고).

---

### 6.1.3 함수 선언 예시 (기본형)

#### 1) 위치 파라미터만

```parus
@pure
export def add(a: int, b: int) -> int {
  return a + b;
}
```

#### 2) named group만 (전부 라벨 전달)

```parus
export def make_window({w: int, h: int, title: string = "Parus"}) -> handle<Window> {
  // ...
}
```

#### 3) 위치 + named group (권장 “실전형”)

```parus
export def spawn_entity(
  world: handle<World>,
  kind: EntityKind,
  { x: float32 = 0.0f, y: float32 = 0.0f, tag: string? = null }
) -> handle<Entity> {
  // ...
}
```

---

### 6.1.4 기본값(default) 규칙 (오버로딩과 양립하도록 고정)

기본값은 “편의 기능”이지만, 오버로딩과 섞이면 모호성이 생기기 쉬워서 **호출 해소 규칙을 먼저 고정**한다.

#### (1) 기본값의 위치

v0 권장 정책(가장 단단함):

* **named group `{}` 안에서만 기본값을 적극 권장**
* 위치 파라미터의 기본값도 허용은 가능하지만, v0에서는 아래 (3)의 “해소 우선순위”를 반드시 적용해야 한다.

#### (2) 기본값의 타입 제약(강권)

* `DefaultExpr`는 **해당 파라미터 타입에 대입 가능**해야 한다.
* v0에서 구현을 단단히 하려면:

  * `@pure` 또는 `@comptime` 가능한 **컴파일타임 상수식**으로 제한하는 걸 권장
  * (특히 `string`, `handle`, `class` 관련은 런타임 비용/의미가 커질 수 있음)

#### (3) 오버로딩 + 기본값: 호출 해소 우선순위(필수 규칙)

어떤 호출 `f(...)`에 대해 후보 함수가 여러 개면, 아래 순서로 고른다.

1. **(최우선) “기본값 채우기 없이” 정확히 매칭되는 후보**만 남긴다.

   * positional 인자 수/타입/순서가 정확히 맞고
   * (라벨 호출이면) 라벨 세트가 정확히 맞는 것
2. 1에서 후보가 0개라면, 그때만 **기본값 채우기를 허용**하여 매칭을 시도한다.
3. 그 결과가 정확히 1개면 선택, 2개 이상이면 **ambiguous 에러**.

이 규칙 하나로,

* “기본값 때문에 원래 존재하던 오버로드가 가려지는 문제”
* “호출이 여러 오버로드로 동시에 해석되는 문제”
  를 깔끔하게 차단할 수 있다.

---

### 6.1.5 호출 문법과 라벨 규약 (positional / labeled / named-group)

Parus 호출은 **세 가지 모드**만 허용한다.

#### (A) 위치 호출 (positional call)

* 형태: `f(Expr, Expr, ...)`
* 오직 위치 파라미터에만 매칭된다.
* `{}` 그룹 파라미터는 이 방식으로 전달할 수 없다.

```parus
set x = add(1, 2);
```

#### (B) 라벨 호출 (labeled call, 기존)

* 형태: `f(a: Expr, b: Expr, ...)`
* **한 호출 안에서 전부 라벨이어야 한다** (기존 규칙 유지)
* 라벨 이름이 시그니처에 포함되므로, **라벨 이름 불일치 = 매칭 실패**다.

```parus
set y = add(a: 1, b: 2);
```

> 기존 규칙(중요): `f(1, b: 2)` 같은 **혼합은 금지**.

#### (C) 위치 + named-group 호출 (신규, Dart 스타일)

* 형태: `f(Positional..., { name: Expr, ... })`
* `{ ... }` 내부는 **반드시 라벨**로만 작성
* `{ ... }`는 선언에 named group이 있을 때만 사용 가능
* `{ ... }`는 “명시적 구역”이므로, 이것은 *혼합 금지 규칙의 예외가 아니라*, **별도 모드**로 취급한다.

```parus
set e = spawn_entity(
  world,
  EntityKind::Orc,
  { x: 10.0f, y: 20.0f }
);
```

---

### 6.1.6 named group 매칭 규칙 (필수/선택, 중복, 누락)

`def f(a: int, {x: int = 0, y: int}) -> ...` 같은 선언이 있을 때:

* `{}` 안 파라미터 전달은 **라벨로만** 가능
* `{}` 안 라벨은 **중복 금지**
* `{}` 안 라벨은 **선언에 존재하지 않으면 에러**
* `{}` 안에서:

  * **기본값이 있는 파라미터는 생략 가능**
  * **기본값이 없는 파라미터는 v0에서는 필수(생략 시 에러)**

예:

```parus
export def f(a: int, {x: int = 0, y: int}) -> int { return a + x + y; }

set ok  = f(1, {y: 2});        // ok: x는 default 0
set bad = f(1, {x: 3});        // error: y가 누락(필수)
set bad2 = f(1, {z: 9});       // error: z는 선언되지 않음
```

---

### 6.1.7 함수 오버로딩 표준 규약 (정본, v0 고정)

이 섹션은 Parus 함수 오버로딩의 **유일한 정본 규약**이다.
문서의 다른 섹션(예: 13장)은 이 규약을 요약만 하며, 의미를 추가/변경하지 않는다.

#### (A) 오버로드 집합과 선언 경계

오버로드 집합(Overload Set)의 키:

* `bundle::(nest...)::name`
* 리시버 종류(`self` / `&self` / `&mut self` / 없음)
* 모드(`sub/pub/none`)

같은 오버로드 집합 안에서 여러 함수를 둘 수 있으나, 아래 규칙을 만족해야 한다.

#### (B) 시그니처 유일성 키(Declaration Key)

선언 충돌을 판정하는 키:

* 오버로드 집합 키
* 위치 파라미터 목록(선언 순서 고정):
  * 개수
  * 각 항목의 `label + type`
* named-group 파라미터 목록(선언 순서 고정):
  * 개수
  * 각 항목의 `label + type + required/optional(default 유무)`

제외 항목(키에 포함하지 않음):

* 반환 타입
* 기본값 표현식 본문
* `export`, `pure`, `comptime`, `commit`, `recast`

#### (C) 선언 시점 충돌 금지(사전 모호성 차단)

아래는 **호출이 없어도** 선언 시점에서 컴파일 에러로 막는다.

1. Declaration Key가 동일한 함수 2개 이상
2. 반환 타입만 다른 함수(인자 키 동일)
3. 같은 오버로드 집합에서, 위치 호출 관점으로 구분 불가능한 쌍
   (예: 위치 타입열이 동일하고 named-group이 없는 함수 2개)
4. labeled 호출 관점으로 구분 불가능한 쌍
   (라벨 집합+타입이 동일한 함수 2개)
5. 함수 1개 내부에서 파라미터 라벨 중복(위치/named-group 전체 기준)

#### (D) 호출 형태별 후보 필터

호출 `C`에 대해 후보를 모은 뒤, 먼저 호출 형태로 필터한다.

1. positional call: `f(e1, e2, ...)`
   * named-group 없는 후보만 대상
2. labeled call: `f(a: e1, b: e2, ...)`
   * named-group 없는 후보만 대상
   * 라벨 집합이 후보의 위치 파라미터 라벨 집합과 일치해야 함
3. positional + named-group call: `f(e1, {x: e2, ...})`
   * named-group 있는 후보만 대상
   * `{}` 그룹은 명시적으로 존재해야 함 (자동 `{}` 가정 금지)

#### (E) 오버로딩 해소 순서(결정적)

형태 필터 후, 다음 2단계로 해소한다.

1. 단계 A: 기본값 채우기 없이 exact match 시도
2. 단계 B: 단계 A 후보가 0개일 때만 기본값 채우기 허용

판정:

* 후보 1개: 선택
* 후보 0개: not found
* 후보 2개 이상: ambiguous

추가 고정:

* 기본값은 해소 우선순위에만 영향, 오버로드 키에는 영향 없음
* 기본값 채우기로 인해 2개 이상이 살아나면 반드시 ambiguous

#### (F) 맹글링(ABI 심볼) 규약

맹글링은 위 Declaration Key를 기반으로 생성한다.

권장 포맷(v0):

```
p$<BundleId>$<Path>$<BaseName>$M<Mode>$R<Recv>$S<ParamSig>$H<Hash>
```

구성:

* `BundleId`: bundle 식별자
* `Path`: `nest` 경로(`::` -> `__`)
* `BaseName`: 함수 원이름
* `Mode`: `none|sub|pub`
* `Recv`: `none|self|ref_self|mut_self`
* `ParamSig`: 위치/ named-group 파라미터를 canonical 문자열로 직렬화
* `Hash`: canonical 문자열의 고정 해시(충돌 완화)

요구사항:

* canonical 문자열은 컴파일러 버전/플랫폼과 무관하게 안정적이어야 함
* 디버깅을 위해 `BaseName`, `Path`는 사람이 읽을 수 있어야 함

#### (G) no-mangle (`export "C"`) 제약

* `export "C"`(no-mangle export)는 오버로드 집합에 함수가 1개일 때만 허용
* 같은 C 심볼 이름으로 2개 이상 export가 가능해지는 조합은 금지
* `export "C"`는 FFI 경계 함수에만 사용한다

#### (H) 예시

```parus
def add(a: i32, b: i32) -> i32 { return a + b; }
def add(a: i32, {b: i32 = 0}) -> i32 { return a + b; }

set x = add(1, 2);         // positional -> 첫 번째
set y = add(1, {b: 3});    // named-group -> 두 번째
// set z = add(1);         // 두 번째는 '{}' 자동 가정 금지. 첫 번째도 인자 부족 -> not found
```

---


### 6.2 qualifier: pure, comptime

#### 6.2.1 pure

정의:

* pure 함수는 관측 가능한 부수효과가 없어야 한다.

v0 보수 규칙:

* 금지:

  * commit, recast 사용
  * pub 호출 (큰 상태 수정 경로)
  * FFI 호출 (`extern "C"` 선언 함수 호출)
  * I/O 성격 표준 라이브러리 호출
  * && 사용

* 허용:

  * 지역 계산
  * 불변 데이터 읽기

예시

```parus
@pure
def clamp(p: int, lo: int, hi: int) -> int {
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

```parus
@comptime
def pow2(n: int) -> int {
  // 단순 루프는 comptime 엔진이 지원한다고 가정
  set mut r = 1;
  set mut i = 0;
  while (i < n) {
    r = r * 2;
    i = i + 1;
  }
  return r;
}


def use_comptime() -> void {
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

```parus
// v0 권장 추가 문법(선택):
// comptime set N = pow2(a: 5);
```

### 6.3 호출 규칙: 위치 인자 vs 라벨 인자

* 한 호출에서 위치 인자만 쓰거나 라벨 인자만 쓴다.
* 혼합은 금지다.

예시

```parus
def f(a: int, b: int) -> int { return a + b; }

def calls() -> void {
  set x = f(1, 2);         // ok: positional
  set y = f(a: 1, b: 2);   // ok: labeled
  // set z = f(1, b: 2);   // error: mixed
}
```

### 6.4 `non-?` / `?` 함수, 예외 허용 범위, 호출 제약

### 6.4.1 함수 “예외 허용” 표기: 이름 접미 `?`

Parus은 함수 단위로 “예외(throw) 경로”를 **정적 분리**한다.

* **non-? 함수**: 함수 이름에 `?`가 없다.

  * `throw`, `try...catch` **전부 금지**
  * 컴파일러는 해당 함수를 **nounwind(비언와인드)**로 취급할 수 있다.
* **? 함수**: 함수 이름에 `?`가 붙는다.

  * `throw`, `try...catch` **허용**
  * 호출 그래프 상 예외 전파 경로가 존재할 수 있다.

문법 예시:

```parus
def parse_u32(text: string) -> Result<u32> { ... }     // non-?
def read_file?(path: string) -> bytes { ... }          // ?
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

```parus
def bad() -> void {
  throw Err("nope");          // error
}

def also_bad() -> void {
  try { ... } catch (e) { }   // error
}
```

---

### 6.4.3 ? 함수에서의 허용 규칙(기본)

? 함수는 `throw`, `try...catch`를 사용할 수 있으며, **잡히지 않은 예외는 자동 전파**된다.

```parus
def open_config?(path: string) -> Config {
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

```parus
let r: Result<T> = attempt some_throwing_call?(...);
```

예시:

```parus
def load_config(path: string) -> Result<Config> {
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

## 7. 제어 흐름: if, switch, while, loop

### 7.1 if / elif / else

### 문법

* `if (cond) Block`
* `elif (cond) Block` (0개 이상)
* `else Block` (0 또는 1)

### 규칙

* `cond`는 반드시 `bool` 타입이어야 한다. (truthy 금지)
* `if`는 **표현식(expression)** 이다.
* 각 분기의 결과 타입은 서로 동일해야 한다.

  * 모든 분기가 값을 반환하지 않는 경우, 전체 타입은 `unit`이다.

### 예시

```parus
def sign(x: int) -> int {
  if (x < 0) { return -1; }
  elif (x == 0) { return 0; }
  else { return 1; }
}
```

---

## 7.2 switch

### 문법

```parus
switch (expr) {
  case LIT: { ... }
  case LIT: { ... }
  default: { ... }
}
```

### 규칙

* fallthrough 금지
* `case` 라벨은 리터럴만 허용
  (int, string, bool, char 등)
* `default`는 선택 사항이나 강력히 권장
* 타입 패턴 매칭 허용

  ```parus
  case Err(name): { ... }
  ```

  * `name`은 새 바인딩이며, 스코프는 해당 `case` 블록 내부

### 예시

```parus
def demo(r: Result<u32>) -> u32 {
  switch (r) {
    case Ok(v): { return v; }
    case Err(e): {
      // e: Error
      return 0u32;
    }
  }
}
```

---

## 7.3 while (statement 루프)

### 문법

```parus
while (cond) { ... }
```

### 규칙

* `cond`는 반드시 `bool` 타입이어야 한다.
* `while`은 **문장(statement)** 이다.

  * 값이 없으며, 표현식으로 사용될 수 없다.
* `break;`, `continue;` 사용 가능
* **`break expr;`는 허용되지 않는다.**

### 의미

* `while`은 **단순 반복 및 제어 흐름**을 위한 구조이다.
* 반복 종료 후 어떤 값도 생성하지 않는다.

### 예시

```parus
def sum_to(n: int) -> int {
  set mut i = 0;
  set mut s = 0;
  while (i <= n) {
    s = s + i;
    i = i + 1;
  }
  return s;
}
```

---

### 7.3.1 `do` 기반 명시 스코프와 `do ... while`

#### 7.3.1.1 배경

기존 `{ ... }` 단독 블록은 긴 함수에서 “단순 스코프 시작”과 “제어문 본문/표현식 블록”의 시각적 구분이 약하다.
Parus은 이미 `loop { ... }`처럼 키워드 기반 블록 문법을 사용하므로, 일반 스코프도 같은 계열로 맞춘다.

#### 7.3.1.2 문법

```parus
do { ... }                      // 일반 스코프(명시적)
do { ... } while (cond);        // do-while (최소 1회 실행)
```

#### 7.3.1.3 규칙

* `do { ... }`는 **문장(statement)** 이며 값을 만들지 않는다.
* `do { ... } while (cond);`에서 `cond`는 반드시 `bool`이어야 한다.
* `do ... while`은 본문을 최소 1회 실행한 뒤 조건을 검사한다.
* `break;`, `continue;`는 가장 가까운 반복문(`while`, `do ... while`, `loop`)에 적용된다.

#### 7.3.1.4 단계적 적용(호환 정책)

* v0 호환 구간에서는 단독 `{ ... }` 블록 문장을 **허용**하되, 스타일/경고로 `do { ... }`를 권장한다.
* strict 모드(차기 단계)에서는 단독 블록 문장에 `do`를 요구할 수 있다.
* 표현식 블록(예: `if` 표현식 분기 내부 블록)은 기존 `{ ... }` 문법을 유지한다.

---

## 7.4 loop (표현식 루프)

`loop`는 반복을 통해 **값을 산출할 수 있는 표현식 루프**이다.

v0에서 `loop(cond)` 문법은 삭제되며,
`loop`는 다음 두 형태만을 가진다.

### 7.4.1 문법

* 무한 루프: `loop { ... }`
* 순회 루프: `loop (name in iterable) { ... }`

---

## 7.5 loop의 값 반환 규칙

### 7.5.1 break / continue

* `continue;` : 다음 반복으로 진행
* `break`는 두 형태를 가진다:

  * `break;`

    * 현재 `loop`를 종료
    * 결과값은 **`null`**
  * `break expr;`

    * 현재 `loop`를 종료
    * 결과값은 `expr`

> `break`와 `continue`는 **가장 가까운 반복문**에만 적용된다.
> (v0에서는 라벨을 지원하지 않는다.)

---

### 7.5.2 loop는 표현식이다

* `loop { ... }` 및 `loop (x in xs) { ... }`는 **표현식**이다.
* 따라서 변수 대입, 반환, 인자 위치에서 사용 가능하다.

```parus
set x = loop {
  break 42;
};
```

---

### 7.5.3 종료 가능한 loop의 결과 타입

순회 루프는 **자연 종료**가 가능하다.

* `loop (name in iterable) { ... }`

  * iterable이 소진되면 자연 종료
* 자연 종료 시 결과값은 **`null`**

따라서:

* `break expr;` -> 결과는 `T`
* `break;` 또는 자연 종료 -> 결과는 `null`

결과적으로, 순회 루프의 타입은 **`T?`** 이다.

---

## 7.6 변수 선언과 타입 규칙

### 7.6.1 선언 방식

* 타입 추론 선언: `set x = expr;`
* 타입 명시 선언: `let x: T = expr;`
* 가변 선언: `set mut`, `let mut`

---

### 7.6.2 loop 결과 대입 시 타입 강제

종료 가능한 `loop`의 결과를 변수에 대입할 경우:

* 대상 변수는 반드시 `T?` 타입이어야 한다.
* 그렇지 않으면 컴파일 에러이다.

#### 예시 (정상)

```parus
set found = loop (v in xs) {
  if (v == 42) { break v; }
};
// found : int?
```

```parus
let found: int? = loop (v in xs) {
  if (v == 42) { break v; }
};
```

#### 예시 (에러)

```parus
let found: int = loop (v in xs) {
  if (v == 42) { break v; }
};
// 에러: 이 loop는 자연 종료 시 null을 반환할 수 있음
```

> v0 권장 규칙:
> `T`에 대입하려면 컴파일러가 **자연 종료 경로가 없음을 증명**할 수 있어야 한다.

### 7.6.3 타입 캐스팅 (as, as?, as!)

Parus은 Rust와 유사하게 암시적 타입 변환을 허용하지 않으며, 타입이 다른 값들 사이의 연산(i32 + i64 등)은 컴파일 에러이다.
명시적 변환이 필요한 경우 `as` 계열 캐스팅 연산자를 사용한다.

`as` 계열 캐스팅은 다음 세 가지 의미를 가진다.

* 값 변환을 수행하는 **일반 캐스팅 (`as`)**
* 실패 가능성이 있는 캐스팅을 안전하게 수행하는 **옵셔널 캐스팅 (`as?`)**
* 실패 시 런타임 오류를 발생시키는 **강제 캐스팅 (`as!`)**

이 설계는 Swift, C# 등의 safe cast/forced cast 모델과 유사하며, 향후 다운캐스트·런타임 타입 검사·참조 타입 변환 등으로 확장될 수 있도록 정의된다.

⸻

#### 7.6.3.1 캐스팅 연산자 종류와 의미

• 일반 캐스팅: `expr as T`
• 명시적 타입 변환을 수행한다.
• optional(`T?`) 값을 자동으로 해소(unwrapping)하지 않는다.
• 캐스팅 실패 가능성이 없는 변환에 사용된다.

예:

```parus
let a: i32 = 1;
let b: i64 = a as i64;
```

---

• 옵셔널 안전 캐스팅: `expr as? T`
• 캐스팅이 실패하면 결과는 `null`이다.
• 성공하면 `T` 값을 optional로 감싼 `T?`를 반환한다.
• 입력 값이 `null`이면 결과도 `null`이다.
• 결과 타입은 항상 `T?`로 정규화된다.

이 연산자는 주로 다음 상황을 위해 존재한다.

* 런타임 타입 검사 기반 다운캐스트 (v1 이후)
* 실패 가능 변환
* nullable 값 안전 변환

예:

```parus
let x: i32? = null;
let y: i64? = x as? i64;   // null 유지

let a: i32 = 1;
let b: i64? = a as? i64;   // 성공 -> i64?
```

---

• 강제 캐스팅: `expr as! T`
• 캐스팅이 실패하면 런타임 trap(크래시)이 발생한다.
• 성공하면 `T` 값을 반환한다.
• 컴파일러가 실패 가능성을 정적으로 확정할 수 있으면 컴파일 에러로 처리할 수 있다.

예:

```parus
let x: i32? = null;
let z: i64 = x as! i64;   // 런타임 trap
```

---

#### 7.6.3.2 타입 규칙(결과 타입)

• `expr as T` -> 결과 타입은 `T`
• `expr as? T` -> 결과 타입은 `T?`
• 단, `T`가 이미 optional이면 결과는 `T?` 유지 (`T??` 없음)
• `expr as! T` -> 결과 타입은 `T`

Optional 중첩은 허용되지 않으며, 항상 단일 optional로 정규화된다.

⸻

#### 7.6.3.3 Optional 값과 캐스팅

`as`는 optional 값을 자동으로 해소하지 않는다.

```parus
let x: i32? = null;

let a: i32  = x as i32;   // 에러: optional 자동 해소 없음
let b: i32? = x as? i32;  // OK: 실패 시 null
let c: i32  = x as! i32;  // OK(런타임): 실패 시 trap
```

---

Optional 값에 대해 변환을 수행할 때는 **optional chaining 기반 캐스팅을 권장한다.**

예:

```parus
let x: i32? = 10;
let y: i64? = x?.(as i64);
```

의미:

* `x == null` -> 결과 `null`
* 값 존재 -> `as i64` 변환 후 optional로 반환

이는 optional 값을 변환할 때 가장 명확하고 권장되는 패턴이다.

(향후 표준 라이브러리에서 `map` 등의 함수형 변환 API가 제공될 수 있다.)

⸻

#### 7.6.3.4 v0 허용 변환 범위(권장)

v0 단계에서는 `as` 계열 캐스팅을 기본 스칼라 타입 변환에 한정하는 것을 권장한다.

• 정수 ↔ 정수
• 정수 ↔ 부동소수점
• bool ↔ 숫자 변환은 권장하지 않으며 필요 시 표준 라이브러리 함수로 제공
• 문자열/객체/참조 타입 캐스팅 및 다운캐스트는 v1 이후 확장 예정

또한:

* overflow/precision loss 여부는 캐스팅 자체에서 검사하지 않는다.
* checked/saturating/wrapping 변환은 표준 라이브러리 API로 제공한다.

⸻

#### 7.6.3.5 예시

```parus
let a: i32 = 1;
let b: i64 = a as i64;

let x: i32? = null;
let y: i64? = x as? i64;     // 안전 캐스트
let z: i64  = x as! i64;     // 실패 시 trap

let x: i32? = 10;
let y: i64? = x?.(as i64);   // optional chaining cast

let a: i32 = 10;
let b: i32? = a as? i32;     // safe cast -> optional
```

---

## 7.7 loop 형태별 상세 규칙

### 7.7.1 무한 루프: `loop { ... }`

* 자연 종료 경로 없음
* `break expr;` 또는 `break;`로만 종료 가능
* `break;` 사용 시 결과는 `null`

예시:

```parus
def retry() -> int? {
  set attempts = 0;
  loop {
    attempts = attempts + 1;
    if (attempts >= 3) { break 42; }
  }
}
```

---

### 7.7.2 순회 루프: `loop (name in iterable) { ... }`

#### 허용 iterable (v0)

* 고정 배열 `T[N]`
* 가변 리스트 `T[]`
* 범위 `a..b`, `a..:b`
* 표준 라이브러리 iterator

#### 반복 변수

* 타입은 iterable의 요소 타입
* v0 정책: 요소는 **값으로 바인딩**

  * 큰 요소는 Handle / ref 타입 사용 권장

예시:

```parus
def sum_arr(xs: int[4]) -> int {
  set mut s = 0;
  loop (v in xs) {
    s = s + v;
  }
  return s;
}
```

```parus
def find_positive(xs: int[]) -> int? {
  loop (v in xs) {
    if (v > 0) { break v; }
  }
}
```

---

## 7.8 범위 표현식

* `a..b`  : `b` 미만
* `a..:b` : `b` 이하

```parus
def sum_range(n: int) -> int {
  set mut s = 0;
  loop (i in 0..:n) {
    s = s + i;
  }
  return s;
}
```

---

## 7.9 기대 효과

1. **역할 분리로 인한 명확성**

   * `while` : 단순 반복(statement)
   * `loop`  : 값 산출 가능한 반복(expression)

2. **모호성 제거**

   * 자연 종료 시 결과는 항상 `null`
   * 값 존재 여부는 `T?`로 타입에 명시됨

3. **조기 종료 + 값 반환의 1급 지원**

   * “찾으면 탈출하며 값 반환” 패턴이 언어 차원에서 직접 표현됨

4. **타입 기반 안전성**

   * 종료 가능한 `loop` 결과를 `T`에 대입하는 실수를 컴파일 타임에 차단

5. **확장 여지 확보**

   * 라벨 기반 `break`/`continue`는 v1에서 자연스럽게 확장 가능

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

```parus
def exprs() -> void {
  set a = 1 + 2 * 3;
  set ok = (a >= 7) and (a != 0);
  set r = ok ? 10 : 20; // 중첩은 금지

  set mut x = 10;
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

```parus
def add(a: int, b: int) -> int { return a + b; }

def mul(x: int, y: int) -> int { return x * y; }

def pipe_demo() -> void {
  set r = 1 << add(a: _, b: 2) << mul(x: _, y: 10);
}
```

### 8.3 borrow `&`와 escape `&&`

---

### 8.3.1 목적: 작은 데이터는 값, 공유/탈출은 명시적으로

Parus은 기본적으로 **값(move) 기반**이다. v0에서는 Rust급의 복잡한 수명 추론을 강제하지 않되, C/C++의 “참조/포인터가 슬쩍 탈출하여 수명 버그를 만드는 문제”를 구조적으로 차단한다.

이를 위해 두 연산자를 제공한다.

* `&`  : **비탈출(non-escaping) 접근 권한(borrow capability)** 생성
* `&&` : **소유권 탈출(escape) 승격**. 값을 “탈출 가능한 소유 객체”로 승격

이 연산자들은 런타임 오버헤드를 만들기 위한 것이 아니라, **컴파일 타임 정적 규칙 + 표현(ABI) 고정**을 통해 안전성과 최적화를 동시에 얻기 위한 장치다.

또한 Parus에서 `&&`는 논리 AND가 아니며, 논리 연산은 `and/or/not/xor` 키워드만 사용한다.

---

### 8.3.2 borrow `&`의 의미론

#### (1) `&`는 포인터가 아니라 “접근 권한 토큰”

`&x`는 `x`의 주소를 외부에 노출하는 포인터가 아니다.
`&x`는 “이 스코프 안에서 x에 접근할 수 있는 권한”을 나타내는 **borrow 값**을 만든다.

borrow는 두 종류다.

* `&T` : 읽기 전용 borrow
* `&mut T` : 쓰기 가능 borrow (배타적)

#### (2) borrow의 핵심 제약: 비탈출(non-escaping)

v0의 단순하고 강한 규칙:

> borrow 값(`&T`, `&mut T`)은 **절대 탈출할 수 없다.**

탈출 금지에는 아래가 포함된다.

* 함수 반환값으로 반환 금지
* 전역/클래스 draft/field/tablet 멤버에 저장 금지
* 배열/리스트/클로저 캡처에 저장 금지
* FFI로 전달 금지(ABI 경계)
* `&&`로 승격 금지(= borrow를 escape로 바꾸는 것 금지)

즉 borrow는 “잠깐 쓰고 끝내는” 용도다.

#### (3) borrow의 수명: 렉시컬(블록) 기반

v0에서는 borrow 수명을 **렉시컬 스코프(블록)** 로 본다.

* `set r = &x;` 하면 `r`은 그 블록 끝까지 유효하다.
* 더 짧은 수명을 원하면 `{ ... }` 블록을 사용한다.

(향후 v1+에서는 NLL(마지막 사용점 기반 단축) 같은 최적화를 도입할 수 있으나, v0은 단순성을 우선한다.)

#### (4) `&mut`의 배타 규칙(충돌 규칙)

동일 스코프/동일 수명 구간에서:

* `&mut x`가 존재하는 동안, `x`에 대한 다른 borrow(`&x` 또는 `&mut x`)는 금지된다.
* 또한 `x` 자체에 대한 직접 접근(읽기/쓰기)도 제한될 수 있다.
  v0 권장 규칙: `&mut x`가 살아있는 동안 `x`를 직접 읽거나 쓰는 행위는 금지한다.

이 규칙은 런타임 락이 아니라 **컴파일 타임 검사**다.

#### (5) borrow 대상은 “place expression”이어야 한다

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

#### (1) `&&x`는 “소유권을 탈출 가능한 소유 핸들”로 승격

`&&x`는 `x`의 소유권을 소비(consumed)하고, 결과를 **escape handle** 로 만든다.

* `x`는 사용 불가(소유권 이동)
* 결과는 **고정된 ABI(3word)** 로 표현되는 “소유 핸들 값”이다.

핵심: `&&`는 “라이브러리 타입을 호출하는 문법 설탕”이 아니다.
`&&`는 **언어 코어 의미론 + ABI가 고정된 핵심 연산자**이며, freestanding에서도 자체적으로 완결된다.

#### (2) `&&`의 대상도 place expression이어야 한다

`&&`는 임시값에 붙을 수 없다.

* `&&(a + b)` : 에러
* `&&obj` : 가능(소유권 이동)

#### (3) `&&`는 borrow에 적용할 수 없다

borrow는 비탈출이므로, 아래는 금지된다.

* `&&(&x)` : 에러
* `&&r` (r: &T) : 에러

#### (4) `pure` / `comptime` 제약 (v0 강제 권장)

* `pure` 함수 안에서는 `&&` 사용 금지
* `comptime` 함수 안에서는 `&&` 사용 금지

(향후 v1+에서 “순수 핸들” 계열을 정식 도입할 경우 완화 가능)

---

### 8.3.4 예시

#### 예시 1: 읽기 전용 borrow

```parus
def sum2(a: &i32, b: &i32) -> i32 {
  return a + b;
}

def demo_read_borrow() -> void {
  let x: i32 = 10;
  let y: i32 = 20;
  let s: i32 = sum2(a: &x, b: &y);
}
```

#### 예시 2: `&mut`로 수정 권한 위임

```parus
def inc(x: &mut i32) -> void {
  x = x + 1;
}

def demo_write_borrow() -> void {
  let mut a: i32 = 0;
  inc(x: &mut a);
}
```

#### 예시 3: borrow 비탈출 규칙(금지 사례)

```parus
def bad_return_ref() -> &i32 {
  let x: i32 = 3;
  return &x;   // error: borrow 값은 함수 밖으로 탈출할 수 없다
}

def bad_store_global() -> void {
  // draft.someRef = &x; // error: draft/field/tablet 멤버 저장 금지
}
```

#### 예시 4: `&&`로 소유권 탈출(escape)

```parus
tablet File {
  public:
    def close() -> void { /* ... */ }
}

def open_file() -> Handle<File> {
  set f = File();
  return &&f;
}
```

> 주의: 위 예시의 `Handle<File>`은 “표면 문법상의 표기”일 뿐이며, `&&`의 결과 ABI는 아래 8.3.5에서 **ptr/meta/ctrl 3word**로 고정된다.
> (언어는 특정 라이브러리 이름에 의존하지 않는다.)

#### 예시 5: `&mut` 배타 규칙(충돌)

```parus
def demo_exclusive() -> void {
  let mut x: i32 = 1;

  set r = &mut x;
  // set s = &x;     // error: &mut x 살아있는 동안 다른 borrow 불가
  // set t = x + 1;  // error(v0 권장): &mut x 살아있는 동안 직접 접근 금지

  r = r + 1;
}
```

#### 예시 6: 논리 연산은 키워드만 사용

```parus
def demo_logic() -> void {
  set a = true;
  set b = false;

  if (a and not b) {
    // ...
  }

  // if (a && b) { } // error: &&는 escape 연산자, and를 사용할 것
}
```

---

### 8.3.5 `&&` 구현 지침 (의미론 고정 + 3word fat handle ABI)

`&&`는 **항상** “탈출 가능한 소유 핸들(handle) 생성”을 의미한다.
컨텍스트에 따라 의미가 달라지는 동작(예: 어떤 경우엔 단순 move, 어떤 경우엔 refcount 등)은 v0에서 금지한다.

### (1) 결과 ABI: `&&`는 항상 3word(handle3)로 표현된다

v0에서 escape handle은 다음 3word로 고정된다.

* **`ptr`**  : 객체(또는 객체 헤더) 주소
* **`meta`** : 타입 메타 포인터(type meta). drop/move/clone 등의 정적 정보를 제공
* **`ctrl`** : 최적화/소거를 위한 제어 워드(태그/비트필드)

즉 `&&x`의 결과는 언제나:

```
handle3 = (ptr, meta, ctrl)
```

이 3word는 **언어 차원의 ABI**이며, 라이브러리 타입 이름과 무관하게 유지된다.

---

### (2) `ptr` / `meta` / `ctrl`의 의미(정확한 정의)

#### 2.1 `ptr` (word0)

* 기본적으로 “소유 대상 객체의 주소”다.
* 객체가 “직접 객체”인지 “헤더를 가진 박스 객체”인지는 `ctrl`의 태그로 구분한다.

#### 2.2 `meta` (word1)

* `meta`는 **정적 타입 `T`에 대해 컴파일러가 생성한 타입 메타 객체의 주소**다.
* 타입 메타에는 최소로 다음 정보가 포함된다(개념적).

  * `drop_fn(ptr)` : 파괴자 호출/자원 해제 코드의 진입점 (없으면 null)
  * `move_fn(dst, src)` : 이동 생성(필요하면)
  * `size`, `align` : 타입 크기/정렬 (일부 최적화에 사용)
  * (선택) `flags` : trivial drop/trivial move 등의 성질

> 중요한 점: `meta`는 “런타임에서 찾아오는 것”이 아니라, **컴파일러가 생성한 정적 데이터**를 가리킨다.
> 따라서 `meta` 로딩 자체는 런타임 비용이 아니라 “상수 포인터 로드”에 가깝다.

#### 2.3 `ctrl` (word2)

`ctrl`은 “소거와 인라이닝이 진짜 잘 먹히는 형태”를 목표로, 아래처럼 **하위 비트를 태그로 고정**하고, 나머지는 “증명 가능한 추가 정보”로 사용한다.

권장 비트 레이아웃(v0):

```
ctrl[ 2:0 ] = KIND (3-bit tag)
ctrl[ 3 ]   = HAS_DROP (1 if drop needed, else 0)
ctrl[ 4 ]   = MOVED_OUT (compiler-only SSA flag; stored only when materialized)
ctrl[ 5 ]   = PINNED (future; v0 reserved = 0)
ctrl[15:6 ] = AUX_SMALL (10-bit small aux, meaning depends on KIND)
ctrl[63:16] = AUX_LARGE (48-bit aux / pointer / index; meaning depends on KIND)
```

KIND 태그 정의(v0):

* `KIND=0 (TRIVIAL)`

  * “소유하지만 드랍이 관측 불가 / 드랍 필요 없음” 또는 “드랍이 no-op”
  * `HAS_DROP=0`이 강제되는 것이 권장
  * 소거 최적화가 가장 잘 먹히는 형태

* `KIND=1 (STACK_SLOT)`

  * `ptr`는 “현재 함수의 스택 프레임 내 슬롯 주소”
  * `AUX_SMALL`에 “frame slot id(또는 offset/8 단위 인코딩)”을 넣을 수 있음
  * 이 handle은 함수 밖으로 탈출 가능하지만, 탈출 시점에 **caller slot로 재로워링**되거나 **복사/이동으로 구체화**됨

* `KIND=2 (CALLER_SLOT)`

  * `ptr`는 “호출자(상위) 프레임이 제공한 sret/out 공간”
  * `&&`가 곧바로 “반환/인자 전달”로 이어질 때 가장 강력한 소거 패턴을 제공
  * `AUX_*`는 보통 0 (불필요)

* `KIND=3 (HEAP_BOX)`

  * v0에서는 기본적으로 “명시적 박싱 문법이 있을 때만” 생성되는 것을 권장
  * `ptr`는 헤더/박스 주소
  * (v0 철학상) “자동 힙 할당으로 도망”은 금지/비권장. 즉 `&&` 자체가 힙을 의미하지 않는다.

* `KIND=4..7` : 예약(미래 확장: shared/refcount, region, pinned, extern 등)

> 설계 의도: `ctrl`은 “런타임 동작을 바꾸는 스위치”가 아니라, **컴파일러가 소거/인라이닝을 하기 위한 증거(Proof)와 컨텍스트**다.
> 따라서 최적화 레벨이 낮아도 의미는 동일하며, 최적화 레벨이 높아질수록 `ctrl` 기반 소거가 강해진다.

---

### (3) lowering 규칙(표현식 -> IR 관점)

`&&x`는 다음 성질을 만족하도록 lowering 된다.

#### 3.1 기본 lowering(개념)

* 입력: `x`는 place expression, 정적 타입 `T`
* 출력: `handle3(ptr, meta, ctrl)`

개념적으로:

1. `ptr = address_of(x)` 또는 “move source 주소”
2. `meta = &__gpl_type_meta_T` (컴파일러 생성 정적 메타)
3. `ctrl = make_ctrl(kind=STACK_SLOT or CALLER_SLOT, has_drop=...)`

여기서 핵심은:

* `meta`는 **항상 정적으로 결정**된다.
* `ctrl.kind`는 컨텍스트 기반으로 결정된다(특히 반환/인자 전달 경로면 CALLER_SLOT로).
* 어떠한 경우에도 `&&` 자체가 “런타임 함수 호출”로 정의되지 않는다.

#### 3.2 반환 위치 최적화 기본 정책(v0 권장)

함수에서 `return &&x;` 패턴이 보이면, 컴파일러는 기본적으로:

* `x`를 **caller sret 공간에 직접 move-construct**
* 결과 handle은 `KIND=CALLER_SLOT`로 만든다.
* 이후 handle 자체는 곧바로 소거 가능(아래 최적화 케이스 참조)

---

### (4) `&&` 최적화 허용 범위(의미 보존만)

컴파일러는 아래 최적화를 허용한다(의미는 동일, 관측 불가):

* handle을 메모리 객체로 만들지 않고 레지스터/SSA 값으로만 보관
* `meta`가 정적으로 확정되면 간접 호출 대신 직접 호출/인라인
* escape 분석으로 중간 handle 생성 단계를 제거
* 즉시 파괴되는 handle 제거(dead handle elimination)
* `ctrl.kind`가 증명되면, 해당 경로로만 특수화하여 분기 제거

단, 아래는 **금지**(의미 변화):

* 어떤 경우에는 “그냥 포인터”, 어떤 경우에는 “소유 핸들”처럼 표현을 바꿔 관측 가능한 의미가 달라지는 것
* borrow를 `&&`로 승격하여 탈출시키는 것(8.3.2의 borrow 비탈출 유지)

---

### 8.3.6 컴파일러 최적화 케이스 5개 (3word 기반 소거 패턴 포함)

아래 최적화들은 모두 “의미 보존(escape handle 의미는 유지)”을 전제로 하며, 관측 가능한 런타임 효과를 추가하지 않는다.

---

#### 최적화 1) Return-by-escape: caller-slot 직접 구성 + handle 소거

패턴:

```parus
def make() -> Handle<T> {
  set x = T();
  return &&x;
}
```

lowering 핵심:

* `x`를 **caller의 반환 슬롯(sret)** 에 직접 move-construct
* 결과 handle은 `KIND=CALLER_SLOT`, `ptr = sret_ptr`
* `meta`는 상수 포인터
* `ctrl`은 상수(`CALLER_SLOT | HAS_DROP?`)

소거:

* 반환 ABI가 handle3라도, 실제로는 `ptr/meta/ctrl` 모두 상수/이미 알려진 값으로 정리되어

  * 호출자에서 즉시 “그냥 반환 슬롯을 가진 값”으로 취급 가능
  * 중간 handle 값을 **레지스터에조차 만들 필요가 없음**

---

#### 최적화 2) Forwarding escape: `&&` 생성 후 즉시 인자 전달(중간 handle 제거)

패턴:

```parus
def g(h: Handle<T>) -> void { /* ... */ }

def f() -> void {
  set x = T();
  g(h: &&x);
}
```

lowering:

* `&&x`는 `KIND=STACK_SLOT`로 생성될 수 있으나,
* 바로 호출 인자로 들어가므로 컴파일러는 `g`의 ABI에 맞춰 **직접 전달 형태로 재작성** 가능

소거:

* `&&x`라는 “중간 handle 값”을 만들지 않고,

  * `(ptr=&x, meta=&meta_T, ctrl=STACK_SLOT|...)` 를 호출 규약에 맞춰 바로 배치
* `meta`가 상수면 간접 없이 직접 경로로 최적화 가능

---

#### 최적화 3) Dead handle elimination: 즉시 파괴되는 escape 제거

패턴:

```parus
def f() -> void {
  set x = T();
  set h = &&x;
  // h를 더 이상 쓰지 않음
}
```

의미:

* handle이 생성되었으나 사용되지 않는다.
* “escape로 만들었다”는 사실 자체는 관측 불가(순수한 의미 토큰이므로).

소거:

* `h` 생성 자체를 제거하고,
* `x`는 원래 스코프 종료 시점에 파괴되므로(drop이 있다면) 원래 의미 유지

ctrl 기반 증명:

* `ctrl.kind=STACK_SLOT`이고 `h`가 외부로 전달되지 않음이 증명되면
  -> handle 생성은 100% 제거 가능

---

#### 최적화 4) Meta direct-call/inlining: meta 기반 간접 호출 제거

패턴:

```parus
def drop_it(h: Handle<T>) -> void {
  // 스코프 종료 시 드랍
}
```

일반적으로 handle drop은 “meta->drop_fn(ptr)” 형태가 될 수 있다.

하지만 v0에서:

* `meta`는 정적으로 확정된 `&__gpl_type_meta_T`
* `drop_fn`도 정적 주소(또는 null)

최적화:

* `meta` 역참조를 제거하고 `drop_T(ptr)`로 **직접 호출**
* trivial drop이면 호출 자체를 제거 (`HAS_DROP=0` 또는 meta flag 기반)

ctrl 연계:

* `ctrl.HAS_DROP=0`이면 drop 경로는 완전히 제거 가능
* `KIND=CALLER_SLOT`이면 drop 책임이 “해당 스코프에 있는지”도 더 쉽게 결정됨(특수화)

---

#### 최적화 5) Stack-slot -> caller-slot 재로워링: 탈출 시점에서만 구체화

패턴:

```parus
def pass() -> Handle<T> {
  set x = T();
  set h = &&x;     // 여기서는 STACK_SLOT
  return h;        // 여기서 탈출
}
```

v0 기본 lowering:

* 처음 `h`는 `(ptr=&x, meta=meta_T, ctrl=STACK_SLOT|...)`

탈출 시점 최적화:

* 반환 경로에서 `x`를 caller-slot에 직접 move-construct
* `h`를 새로 만들지 않고, `ctrl.kind`를 `CALLER_SLOT`로 “재로워링”
* `STACK_SLOT` handle은 결과적으로 소거되고, 최종 결과는 return-by-escape와 동일한 형태가 됨

핵심:

* 힙 할당 같은 런타임 전략으로 도망치지 않고,
* **탈출 경계에서만** out-slot으로 구체화하여 “0 오버헤드”를 유지한다.

---

### 8.3.7 제약 재확인 (v0 고정)

* `&` / `&mut` / `&&` 는 place expression에만 적용 가능
* borrow(`&T`, `&mut T`, `&[T]`, `&mut [T]`)는 절대 탈출 불가
* `&&`는 borrow에 적용 불가
* 논리 연산은 `and/or/not/xor` 키워드만 사용 (`&&`는 escape 예약)
* `pure` / `comptime` 내부에서 `&&` 사용 금지(v0 강제 권장)

---

#### (부록) 타입 표기 관련 주의(v0)

* 기존 C 스타일 이름 `int`, `float` 등은 금지(컴파일러 빌트인에 해당 타입 없음)

  * 정수는 `i32`, `i64` 등
  * 부동소수는 `f32`, `f64` 등
* `text`는 빌트인 문자열 슬라이스 타입으로 항상 사용 가능
* `String`은 표준 라이브러리 타입이며, std 링크가 없으면 사용할 수 없다
* null 가능 타입 표기는 `T?`로 하며, `&i32?`는 허용한다.

  * 우선 파싱 규칙은 “타입 접미 `?`가 `&`보다 우선 결합”으로 둔다: `&i32?`는 `&(i32?)`로 해석
  * 다른 해석을 원하면 괄호로 명시한다: `(&i32)?` 또는 `&(i32?)`

(이 파싱/우선순위 규칙은 8.2 타입 문법 섹션에서 최종 고정한다.)

### 8.3.8 `&&` 장수명(escaping)과 `static` 저장소 연계

`&&`는 “소유권을 탈출 가능한 handle3로 승격”하지만, v0에서 `&&` 자체가 힙 할당이나 암묵 런타임 호출을 발생시키지 않으므로, **탈출한 값이 어디에 저장되는지(storage)** 가 명확해야 한다. 이 섹션은 `&&`가 “장수명”으로 사용되는 경우에, 저장소를 `static`/`unique`와 어떻게 연계하는지에 대한 v0 지침을 고정한다.

#### (1) v0 핵심 원칙: “장수명 `&&`는 장수명 place에서만”

v0에서 `&&x`의 대상 `x`는 place expression이어야 하며, 그 place는 아래 중 하나여야 한다.

* **경계 직결(caller-slot)**: `return &&x;`, `f(arg: &&x)`처럼 `&&`가 곧바로 반환/인자 전달로 이어져 컴파일러가 `KIND=CALLER_SLOT`로 lowering 할 수 있는 경우
* **정적 저장소(static place)**: `static`으로 선언된 전역/정적 place(프로그램 수명)인 경우
* (선택, v0 확장) **고정 풀(pool place)**: 구현이 제공하는 정적 풀 슬롯(이 문서에서는 기본 규칙만 다룬다)

이 원칙은 다음을 보장한다.

* 힙/런타임이 없는 freestanding에서도 `&&`가 안전하게 동작한다.
* “스택 슬롯을 가리키는 handle이 함수 밖으로 나가서 dangling” 되는 구조를 원천 차단한다.

#### (2) `static` place에서의 `&&`: 장수명 탈출의 기본 해법

`static`으로 선언된 place는 프로그램 수명 동안 유효하므로, 다음 패턴은 v0에서 장수명 `&&`의 표준적인 형태다.

```parus
static mut G: i32 = 7i32;

def get_g() -> Handle<i32> {
  // G는 static place이므로 &&로 탈출 가능
  return &&G;
}
```

권장 규칙(v0):

* `&&`가 “장수명으로 사용”되는 경로(반환/저장/캡처 등)에서, 대상 place가 `static`이 아니면 컴파일 에러로 보고한다.
* 단, `return &&x;` 같은 caller-slot 최적화 경로는 예외로 허용한다(스택에 남지 않으므로).


여기서 중요한 점:

* `static`은 “유일 소유”를 **컴파일 타임 규칙으로 고정**한다.
* `&&`는 힙/암묵 호출이 없으므로, “take 결과를 장수명으로 저장”하려면

  * `return &&u;`처럼 caller-slot 경계로 넘기거나,
  * 정적 place 자체를 `&&`로 넘기는 형태(`return &&UART0;`)를 사용해야 한다.

#### (3) nullable(`T?`)와 take(consume) 표준 패턴(v0 권장)

* (권장 A) 컴파일러 내장: `unwrap_move(place: T?) -> T`

  * 전제: `place != null`
  * 동작: `T` 값을 소유로 꺼내고, 원본 place는 `null`로 만든다(consume).
* (권장 B) v0 표준 라이브러리(단, no-std에서도 제공 가능한 “core” 레벨로 배치)

예시(권장 A 형태):

```parus
unique static mut LOG: Logger? = null;

def take_log() -> Handle<Logger> {
  if (LOG == null) { throw Error::from_code(code: 2); }
  set v = unwrap_move(LOG);
  return &&v; // caller-slot
}
```

이 패턴은 다음을 만족한다.

* `static` 초기화는 항상 상수(`null`)로 가능
* 초기화/해제/재초기화가 명시적
* `unique` moved-out 모델과 자연스럽게 일치

#### (4) 저장/캡처/전역 보관 시의 규칙(핵심 제약)

borrow(`&T`, `&mut T`, slice borrow 등)는 8.3.2 규칙에 의해 저장/캡처/반환이 금지된다. 장수명 보관을 하고 싶다면 `&&`를 사용해야 한다. 단, v0에서 `&&`는 힙으로 도망치지 않으므로 다음을 강제한다.

* 장수명 보관 대상이 되는 `&&x`에서 `x`는:

  * (A) caller-slot 경계로 즉시 넘어가거나, 또는
  * (B) `static`(또는 구현이 허용한 pool/region) place여야 한다.

금지 사례(의도 설명):

```parus
def bad_escape_store() -> void {
  set x = T();

  set h = &&x;
  // 여기서 h를 전역/컨테이너/클로저에 저장하려 하면:
  // x는 스택 place이므로 함수 종료 후 dangling이 된다.
  // 따라서 v0에서는 "장수명 저장" 컨텍스트에서 &&x를 허용하지 않는다.
}
```

권장 진단 메시지(v0):

*error: escaping handle requires long-lived storage (static/pool/region or caller-slot).
help: move value into a static/pool slot, or return it directly (return-by-escape), or use std::Handle<T> (heap-capable) when available.*

#### (6) no-std/freestanding에서의 실무 패턴 요약

v0(no-std)에서 “장수명”을 만드는 기본 패턴은 아래 둘로 정리된다.

1. **전역 유일 자원**: `static T? = null` + `init()` + `take()`(unwrap_move) + `return &&local`(caller-slot)
2. **다수 리소스**: `static` 풀/테이블(고정 배열) + ID 보관 + 접근 시 `&`/`&mut`로 잠깐 빌림
   *(풀/ID 패턴은 storage 문서 또는 별도 섹션에서 확장)*

이 섹션의 목적은 `&&`가 힙 없이도 “경계와 저장소”를 통해 장수명 의미를 유지하도록 하는 것이다.

---

### 8.4 slice borrow: `&[T]`, `&mut [T]` 와 슬라이싱 문법

Parus은 Rust 스타일의 slice/view 표현력을 원하지만, 표준 라이브러리 특수 타입(view<T>)에 언어 규칙을 붙이면 “코어/표준 경계”가 흐려질 수 있다.
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

```parus
def slice_demo() -> void {
  let x: int[8] = [0,1,2,3,4,5,6,7];

  // 슬라이스 생성(읽기 전용)
  set arr = &x[1..:5];     // 타입: &[int]  (요소: 1,2,3,4,5)

  // 슬라이스 생성(쓰기 가능)
  let mut y: int[8] = [0,0,0,0,0,0,0,0];
  set win = &mut y[2..:4]; // 타입: &mut [int] (요소 슬롯: y[2],y[3],y[4])

  // 금지: slice 탈출
  // return arr;           // error: &[T]는 비탈출
  // return &&arr;         // error: borrow를 escape로 승격 금지
}
```

#### (6) 함수 파라미터에서의 사용 예시

```parus
def sum(xs: &[int]) -> int {
  set mut s = 0;
  // v0에서는 slice 반복을 단순화하기 위해 표준 라이브러리 helper가 필요할 수 있음
  // 최소 구현: xs[i] 인덱싱을 허용(범위 내라고 가정하거나 디버그 검사)
  set mut i = 0;
  while (i < xs.len) {     // len 접근 문법은 v0에서 선택: 내장 또는 표준 규약
    s = s + xs[i];
    i = i + 1;
  }
  return s;
}

def use_sum() -> void {
  let a: int[6] = [10,20,30,40,50,60];
  set mid = &a[1..:4];     // &[int] (20,30,40,50)
  set r = sum(xs: mid);
}
```

> 구현 메모(v0): `&[T]`는 내부적으로 `(ptr, len)` 형태의 borrow 값으로 lowering 하면 된다.
> 이는 “표준 라이브러리 특수 타입”이 아니라 **언어 코어 타입**이므로 freestanding에서도 경계가 깨지지 않는다.

### 8.5 `copy` / `clone` 연산자(키워드) 정의

Parus은 “암묵 복사 금지(또는 최소화)”를 목표로 하므로, 복사/복제는 **연산자(키워드)로 명시**한다.
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

Parus의 “명시적 비용” 철학상 정책 A가 더 보수적이고 구현도 쉽다.

#### (5) 예시(반환/대입/초기화)

```parus
def demo_copy_return(a: int) -> int {
  // int는 내장 copy 가능
  return copy a;
}

def demo_copy_assign() -> void {
  let x: int = 3;
  let y: int = copy x;   // 명시적 복사
}

def demo_clone_assign() -> void {
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

```parus
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

```parus
acts string {
  def do_clone(self: string) : op(clone) -> string { return __intrin_string_clone(s: self); }

  def drop(self: string) : op(drop) -> void { __intrin_string_drop(s: self); }
}
```

### 8.8 예외 메커니즘: `throw`, `try...catch`, 예외 타입, 전파 규칙 (추가)

### 8.8.1 예외 값의 타입: 표준 `Error`만 허용(v0 고정)

Parus의 `throw`는 “아무거나 던지기”를 금지한다. v0에서는 다음으로 고정한다.

* `throw`에 실리는 값은 반드시 표준 라이브러리의 `Error` 타입이어야 한다.

  * 권장: `Error`는 “메시지 + 코드 + 원인 체인”을 담을 수 있는 불투명 핸들
  * freestanding에서도 동작 가능하도록 `Error`는 **할당 없이도 생성 가능한 경로**(고정 문자열, 코드 기반)를 제공해야 한다.

예시(표준 라이브러리 관례 예시):

```parus
// std::error::Error 가 있다고 가정
use std::error::Error;

def fail?( ) -> void {
  throw Error::from_msg(msg: "boom");
}
```

---

### 8.8.2 `throw` 문장 규칙(? 함수 전용)

* `throw Expr;` 는 **문장(statement)** 이다.
* **? 함수 내부에서만 허용**된다.
* Expr의 타입은 `Error`여야 한다(또는 `Error`로의 명시적 변환이 있어야 한다).

```parus
def read_file?(path: string) -> bytes {
  if (path == "") {
    throw Error::from_code(code: 12);   // ok
  }
  // ...
}
```

---

### 8.8.3 `try...catch` 문법과 규칙(? 함수 전용)

문법(권장 최소형):

```parus
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

```parus
def load_user?(id: u32) -> User {
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

```parus
class Game {
  // draft.score 같은 상태가 존재한다고 가정

  def sub score() -> int { return draft.score; }
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

```parus
class Counter {
  def sub get() -> u32 {
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

```parus
class Counter {
  def pub add(delta: int) -> void {
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

```parus
def add_sprite() : pub {
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

```parus
class Bad {
  def pub f() -> void {
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

```parus
class Progress {
  def pub run() -> void {
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

```parus
class Counter {
  def pub add(delta: u32) -> u32 {
    draft.count += delta;
    commit return draft.count;     // 발행 + 반환
  }
}
```

#### (2) void pub는 `commit;`으로 종료

```parus
class Counter {
  def pub inc() -> void {
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

```parus
class Scene {
  // draft.sprites: handle<SpriteList> 라고 가정

  def pub add_sprite(s: handle<Sprite>) -> void {
    // SpriteList는 tablet이고, draft에는 handle만 저장
    draft.sprites.push(x: s);   // push는 handle/tablet 쪽 메서드
    commit;
  }

  def sub count() -> u32 {
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

field 리터럴/초기화 규칙 (v0):

* 생성 문법: `TypePath{ name: expr, ... }`
* 모든 멤버를 반드시 명시 초기화해야 한다.
  * 누락 멤버: 컴파일 오류
  * 중복 멤버: 컴파일 오류
  * 존재하지 않는 멤버: 컴파일 오류
* `TypePath{}`는 멤버 수가 0인 field에서만 허용한다.
* optional 멤버(`T?`)도 생략은 불가하며 `name: null`을 명시해야 한다.
* non-optional 멤버에 `null`을 넣으면 컴파일 오류다.
* `field` 멤버 선언에는 `mut`를 도입하지 않는다.
  * 가변성은 바인딩에서만 표현한다 (`let mut v: Vec2 = ...; v.x = ...;`)
* `layout(c)` field 멤버에는 optional(`T?`)을 허용하지 않는다.

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

```parus
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

```parus
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

```parus
proto Drawable {
  def draw() -> void;
}
```

### 10.3 tablet (implementation type)

정의:

* `tablet`은 구현체 타입이다. (생성자/소멸자 + 메서드 + proto 구현)
* 표기: `tablet Name [: Proto1, Proto2, ...] { ... }`
* v0 권장: **상속은 proto에 대해서만 허용**한다.

  * `tablet`끼리의 상속(필드/레이아웃 상속)은 v1+로 미룬다. (ABI/생성자 체인이 복잡해짐)

예시

```parus
proto Drawable {
  def draw() -> void;
}

tablet Sprite : Drawable {
  public:
    let pos: Vec2;

    def draw() -> void {
      // ...
    }
}
```

---

### 10.3.1 멤버와 접근 제한자: `public:` / `private:`

* `tablet` 본문은 “멤버 목록”이다.
* 멤버 종류(v0):

  * 데이터 멤버: `let name: Type;` 또는 `let mut name: Type;`(선택)
  * 메서드: `def ... { ... }`
  * 생성자/소멸자: `construct`, `destruct` (아래 10.3.3)
* 접근 제한자:

  * C++ 스타일 `public:` / `private:` 라벨을 사용한다.
  * 라벨은 “이후 멤버들에 적용되는 모드”를 바꾼다.
  * v0 기본 접근은 `private`로 둔다.

예시

```parus
tablet A {
  let x: int;        // private

  public:
    def get_x() -> int { return self.x; }

  private:
    def helper() -> void { ... }
}
```

---

### 10.3.2 메서드의 `self` 규칙 (v0 단순/강제)

Parus은 borrow 설계가 있기 때문에, 메서드의 수신자(receiver)를 **명시적으로 단순화**한다.

* `tablet` 내부의 `def name(...) -> R { ... }` 는 **항상 인스턴스 메서드**다.
* 메서드에는 암묵 수신자 `self`가 존재한다.
* v0에서 수신자 타입은 아래 둘 중 하나다.

  * `def name(...)`  : `self`는 `&Self` (읽기 전용)
  * `def mut name(...)` : `self`는 `&mut Self` (수정 가능)

즉, “메서드가 객체를 바꾸려면 반드시 `mut`를 써야 한다.”
이 규칙 하나로 C++의 암묵 변경 가능성 + Rust의 복잡한 추론 사이에서 깔끔하게 중간 지점을 잡는다.

예시

```parus
tablet Counter {
  public:
    let mut n: int;

    def get() -> int {            // self: &Counter
      return self.n;
    }

    def mut inc() -> void {       // self: &mut Counter
      self.n += 1;
    }
}
```

추가 규칙(v0):

* `self`는 예약 식별자다(키워드 취급 권장).
* `pure/comptime` 함수 안에서는 `def mut` 메서드 호출을 금지할 수 있다(권장).
  (관측 가능한 상태 변경을 정적으로 차단)

---

### 10.3.3 생성자/소멸자: `construct`, `destruct` (v0 단일 정의 규칙)

일반 함수 오버로딩은 허용한다. 다만 v0 단순화를 위해 생성자/소멸자는 타입당 단일 정의만 허용한다.

* 생성자(선택):

  * `def construct(params...) -> void { ... }`
* 소멸자(선택):

  * `def destruct() -> void { ... }`

호출/동작:

* `set x = T(args...)` 는 `T.construct(args...)`를 호출해 `x`를 초기화한다.
* 스코프 종료 시 `destruct()`가 호출된다(존재한다면).
* `def construct(...) = delete;` 로 생성을 금지할 수 있다.
* `def destruct() = delete;` 는 v0에서는 **금지 권장**(파괴 금지 객체는 모델이 꼬임). 대신 “drop이 없는 handle” 같은 타입으로 해결.

예시

```parus
tablet File {
  public:
    let fd: int;

    def construct(path: string) -> void {
      // open...
    }

    def destruct() -> void {
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
  * `mut` 여부 동일 (`def` vs `def mut`)
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

* proto 내부 메서드는 기본이 `def`(즉 `self: &Proto`)이고,
* 수정 가능한 메서드는 반드시 `def mut`로 선언한다.

예시

```parus
proto Stream {
  def read(buf: &mut [u8]) -> u32;
  def mut seek(pos: u64) -> void;   // self: &mut Stream
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

### 11.2 acts의 세 형태: `acts A {}` vs `acts for T {}` vs `acts Foo for T {}`

Parus v0에는 acts 블록이 **세 가지 형태**로 존재한다.

#### (1) 일반 acts(네임스페이스): `acts A { ... }`

* `acts A {}`는 **A라는 이름의 acts 네임스페이스(행동 묶음)** 를 만든다.
* 이 블록 안의 선언은 **항상 스코프 호출(정적 호출)** 로 사용한다.

호출 예시:

```parus
acts Math {
  def add(a: i32, b: i32) -> i32 { return a + b; }
}

def demo() -> void {
  set x = Math::add(1, 2);
}
```

규칙:

* `acts A` 내부 함수는 **dot 호출 sugar의 대상이 아니다.**
* `acts A` 내부에 operator 선언은 불가능하다.
  (연산자 재정의는 **반드시 타입 부착형**에서만 가능)

---

#### (2) 기본 부착형 acts(기본 동작/기본 연산자): `acts for T { ... }`

* `acts for T {}`는 **타입 T에 행동을 부착**한다.
* 이 acts는 **해당 타입의 “기본 acts”** 로 취급되며, **use 없이 자동 활성**이다.

> **field 부착의 의미**:
> `field Foo { ... }`가 있다고 해서 “부착 문법”이 따로 있는 게 아니라,
> **`acts for Foo { ... }`를 정의하는 행위 자체가 Foo에 부착**이다.

핵심 규칙(v0):

* 타입 `T`에 대한 **기본 acts는 프로그램 전체에서 하나의 집합**이다.
* 구현 편의상 v0에서는:

  * `acts for T`를 여러 블록으로 “쪼개어” 쓸 수 있더라도 **한 파일 안에서만** 허용한다.
  * 파일을 넘어 분산(partial)하는 것은 금지한다.
* 동일한 시그니처(동일 함수 시그니처, 동일 operator key/시그니처)를 중복 정의하면 에러.

호출(메서드/UFCS) 예시:

```parus
acts for Packet {
  def checksum(self p: &Packet, seed: u32) -> u32 { return p.crc + seed; }
}

def demo(p: Packet) -> void {
  set a = p.checksum(5u32);          // dot 호출
  set b = Packet::checksum(p, 5u32); // UFCS 호출
}
```

---

#### (3) 이름 있는 부착형 acts(선택 가능한 “행동/연산자 세트”): `acts Foo for T { ... }`

* `acts Foo for T {}`는 **타입 T에 부착되지만 기본 활성은 아니다.**
* 이 acts는 **이름이 Foo인 “선택 가능한 acts 세트”** 이다.
* 사용하려면 **반드시** 아래처럼 명시해야 한다:

```parus
use acts Foo for T;
```

중요 규칙(v0):

* 같은 타입 `T`에 대해 `acts Foo for T`, `acts Goo for T`가 동시에 존재할 수 있다.
* 그러나 **한 스코프에서 동시에 둘 이상 활성화할 수 없다.**

  * `use acts Foo for T;`와 `use acts Goo for T;`를 같은 스코프에 두면 에러
* 선택 acts에서 정의되지 않은 연산자는 **fallback**으로 기본 동작을 따른다(11.7).

---

### 11.3 export 규칙: “acts 블록 단위 export”만 존재

v0에서 export는 다음으로 고정한다.

* `export`는 **acts 블록 전체**에만 적용된다.
* acts 내부의 개별 함수/연산자만 export하는 기능은 **없다**.

예시:

```parus
export acts for Packet {
  def checksum(self a: &Packet, foo: i32) -> u32 { ... }
  def verify(self a: &Packet) -> bool { ... }
}
```

금지 예시(존재하지 않는 문법):

```parus
acts for Packet {
  export def checksum(...) -> u32 { ... } // 금지: acts 내부 개별 export 없음
}
```

---

### 11.4 `acts for T` / `acts Foo for T`의 리시버(self) 규칙과 호출 규칙

#### (1) `self`는 “파라미터 이름”이 아니라 **리시버 마커(modifier)** 다

```parus
def checksum(self a: &T, seed: u32) -> u32 { ... }
```

여기서

* `self` : 예약된 **리시버 마커**
* `a` : 유저가 고르는 **진짜 파라미터 이름**
* `&T`, `&mut T`, `T` 등은 리시버 전달 방식을 의미

**강제 규칙(v0):**

* `self` 마커는 **반드시 첫 번째 파라미터 앞**에만 올 수 있다.
* `self`가 붙은 파라미터는 **반드시 타입이 T(또는 &T / &mut T 등 T 기반)** 이어야 한다.
* `self`가 붙은 함수만이 “메서드(리시버 보유)”로 취급된다.

#### (2) dot 호출: 리시버 인자는 **암묵 전달(첫 인자 생략)**

아래는 동일 의미:

```parus
x.f(1, 2);
T::f(x, 1, 2);
```

즉, dot 호출은 `T::f(x, ...)`로 lowering 된다.

#### (3) “self가 없는 함수”는 메서드가 아니며, 호출은 자유(정적 dot sugar 허용)

```parus
acts for T {
  def make(a: i32, b: i32) -> T { ... } // self 없음
}

set t1 = T::make(1,2);
set t2 = someT.make(1,2); // 허용: T::make(1,2)로 lowering (리시버 전달 의미 없음)
```

---

### 11.5 `acts for T` 내부 함수의 “T 인자 요구” 규칙

`acts ... for T` 안에서 “T에 붙는 동작”을 정의할 때,

* **T를 인자로 받는다면 반드시 `self`로 리시버를 선언**한다.

예:

```parus
acts for T {
  def checksum(self x: &T, seed: u32) -> u32 { ... }
}
```

반대로, T를 받지 않으면 `self`를 쓰지 않는다:

```parus
acts for T {
  def make_seed(x: i32, y: i32) -> u64 { ... } // T 인자 없음
}
```

---

### 11.6 연산자 오버로딩: `operator(...)`만 유일하게 존재 (기존 op("TOKEN") 완전 폐기)

v0에서 연산자 정의 방식은 **오직 하나**만 존재한다.

* 기존의 `: op("TOKEN")` 매핑 방식은 **언어에서 삭제**된다.
* 연산자는 항상 다음 형태로 선언한다:

```parus
operator(+)(self a: T, rhs: T) -> T { ... }
operator(==)(self a: &T, rhs: &T) -> bool { ... }
operator(++pre)(self x: &mut T) -> T { ... }
operator(++post)(self x: &mut T) -> T { ... }
```

#### (1) operator 선언은 `def` 키워드를 붙이지 않는다

* `operator(...)`는 acts 내부의 **특수 선언**이며, 문법적으로 `def`을 사용하지 않는다.

#### (2) operator는 **타입 부착형에서만 허용**

* `operator(...)` 선언은 아래 둘에서만 허용:

  * `acts for T { ... }`
  * `acts Foo for T { ... }`
* `acts A { ... }`(일반 acts) 내부에서 operator 선언은 **금지**한다.

#### (3) operator의 “리시버(self)”는 항상 존재한다

* 모든 operator 선언은 해당 타입에 바인딩되므로 첫 파라미터는 항상 `self`를 사용한다.

#### (4) `++`는 `(++pre)`, `(++post)`만 존재한다

* `operator(++pre)` : `++x`
* `operator(++post)` : `x++`

---

### 11.7 연산자/메서드 해석(리졸브) 규칙 요약 (v0)

표현식에서 연산자나 dot-호출을 해석할 때 타입체커는 **“활성 acts 세트”** 를 사용한다.

#### (1) 활성 acts 세트 결정 규칙

타입이 `T`일 때, 현재 스코프에서:

1. `use acts Foo for T;`가 존재하면 -> **활성 세트 = Foo**
2. 없으면 -> **활성 세트 = 기본 acts (`acts for T`)**

추가 규칙(v0):

* 같은 스코프에서 `use acts Foo for T;`와 `use acts Goo for T;`가 동시에 있으면 **에러**
* “섞기”는 불가능하다(의도적으로 단순화).

#### (2) operator 해석 순서(fallback 포함)

피연산자 타입이 `T`이고 현재 활성 세트가 `S`라면:

1. `acts S for T` 안에서 `operator(KEY)` 찾기
2. 없으면 `acts for T`(기본 acts)에서 찾기
3. 그래도 없으면 primitive 등에서 가능한 **builtin 기본 연산**으로 처리(가능한 경우)
4. 전부 없으면 타입 에러

#### (3) 메서드(dot)/UFCS 해석도 동일한 “활성 세트”를 따른다

`x.method(...)`나 `T::method(x, ...)`도

* 먼저 활성 세트(예: `acts Foo for T`)에서 찾고,
* 없으면 기본 acts(`acts for T`)에서 찾는다.

---

### 11.8 예시 모음 (v0 스타일, “확정된 규칙” 반영)

#### (1) field + 기본 acts for(자동 부착/자동 활성)

```parus
export field Foo {
  u32 v;
}

export acts for Foo {
  def inc(self x: &mut Foo) -> void { x.v = x.v + 1u32; }

  operator(+)(self a: Foo, rhs: Foo) -> Foo {
    return Foo{ v: a.v + rhs.v };
  }
}
```

사용:

```parus
def demo(mut a: Foo, b: Foo) -> Foo {
  a.inc();          // dot -> Foo::inc(a)
  set c = a + b;    // 자동: acts for Foo의 operator(+)
  return c;
}
```

---

#### (2) 선택 acts Foo for T + use로 세트 선택 + fallback

```parus
export acts FooMath for Foo {
  // +만 “다른 의미”로 재정의
  operator(+)(self a: Foo, rhs: Foo) -> Foo {
    // 예: saturating add 같은 정책
    return Foo{ v: __intrin_u32_saturating_add(a.v, rhs.v) };
  }

  def describe(self x: &Foo) -> string {
    return F"""Foo(v={x.v})""";
  }
}
```

사용(기본 세트 vs 선택 세트):

```parus
def demo2(a: Foo, b: Foo) -> void {
  // 기본 세트(acts for Foo)로 동작
  set x = a + b;

  // 여기서부터 FooMath 세트 활성
  use acts FooMath for Foo;

  set y = a + b;         // FooMath의 + 사용
  set s = a.describe();  // FooMath의 메서드 사용

  // FooMath에 없는 연산자는 fallback:
  // 예: operator(==)가 FooMath에 없으면 -> acts for Foo -> builtin 순으로 탐색
}
```

**주의(v0 규칙)**: 같은 스코프에 아래 둘을 동시에 두면 에러

```parus
use acts FooMath for Foo;
use acts GooMath for Foo; // 에러: acts 세트 혼합 금지
```

---

#### (3) primitive 기본 연산은 코어 라이브러리의 `acts for u32`가 담당

```parus
export acts for u32 {
  operator(+)(self a: u32, rhs: u32) -> u32 { return __intrin_u32_add(a, rhs); }
  operator(==)(self a: u32, rhs: u32) -> bool { return __intrin_u32_eq(a, rhs); }
}
```

## 12. 람다/콜백 (전역 람다 금지)

### 12.1 람다 문법 (함수 스코프 전용)

* [captures] (params...) { body }
* capture: name 또는 mut name

타입:

* 캡처 없는 함수 포인터: func<Ret(Args...)>
* 캡처 가능한 클로저 값: closure<Ret(Args...)> (move-only 권장)

예시

```parus
def lambdas() -> void {
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

```parus
// v0: 전역에 closure 금지
// set g = [](x: int) { return x; }; // error
```

---

## 13. 심볼, ABI, 디버깅, 맹글링 규칙

### 13.1 오버로딩 규약(라벨 포함)

정본 규약은 **6.1.7 함수 오버로딩 표준 규약**을 따른다.
즉, 이 섹션은 중복 정의를 하지 않으며 아래만 확인한다.

* 오버로드 집합 키: `bundle::(nest...)::name + receiver + mode`
* 선언 충돌은 Declaration Key 기준으로 사전 차단
* 호출 해소는 2단계(exact -> default-fill) 결정적 규칙 적용
* 반환 타입만 다른 오버로드는 금지
* ambiguous는 런타임이 아니라 컴파일 타임 에러

---

### 13.2 맹글링 규칙(사람 친화 + 안정 링크)

목표:

* 디버거에서 원 이름이 읽히도록 유지
* 링크 충돌 방지 + 오버로딩 지원

권장 심볼 포맷(v0, 정본은 6.1.7(F)):

```
p$<BundleId>$<Path>$<BaseName>$M<Mode>$R<Recv>$S<ParamSig>$H<Hash>
```

* `BundleId`: bundle 이름을 정규화한 ID(예: `engine_core`)
* `Path`: `nest` 경로를 `__`로 연결 (예: `math__vec`)
* `Mode`: `none|sub|pub`
* `Recv`: `none|self|ref_self|mut_self`
* `ParamSig`: 파라미터 canonical 문자열(위치 + named-group + required/optional)

  * 예: `a$i32_b$i32`
* `Hash`: 위 정보를 canonical 문자열로 만든 뒤 짧은 해시(충돌 방지)

예시:

* `p$engine_core$math$add$Mnone$Rnone$Sa$i32_b$i32$H9f2c1a`

추가 규칙(v0 고정):

* `export`된 심볼은 기본적으로 위 규칙으로 맹글링된다.
* `export "C"`(no-mangle)는 6.1.7(G) 제약을 그대로 따른다.

---

## 14. 구현 체크리스트 (v0)

### 14.1 프리패스

* #define 텍스트 치환
* import 해석, alias 심볼 테이블 구성
* bundle 단위로 선언/의존성 그래프 구성 (추가)
* `extern "C"` / `export "C"` 선언 수집
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

```parus
field Vec2 {
  float32 x;
  float32 y;
}

field<u32, i32> OnlyInts {
  u32 a;
  i32 b;
}

proto Drawable {
  def draw() -> void;
}

tablet Sprite : Drawable {
  public:
    let pos: Vec2;

    def draw() -> void {
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
  def sub get() -> u32 {
    // 필요하면 관찰 뷰를 재설정
    recast;
    return draft.count;
  }

  def pub inc() -> void {
    draft.count += 1u32;
    commit;
  }
}

def f(a: int) -> int { return a; }

def f(a: int, b: int) -> int { return a + b; }

def main() -> void {
  let xs: u32[3] = [1u32, 2u32, 3u32];

  set mut s = 0u32;
  loop(iter: v in xs) {
    s = s + v;
  }

  // loop(for: ...) 예시 (명시 이름 버전)
  set mut t = 0;
  loop(for: i in 1..:5) {
    t = t + i;
  }

  set mut x = 10;
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


## 16. EBNF 테이블 (v0 전체 문서 반영, 파서 제작 가능 수준)

> 표기 규칙
> `ε` = empty, `?` = optional, `*` = 0+ 반복, `+` = 1+ 반복
> `A | B` = 선택, `( ... )` = 그룹, `'tok'` = 리터럴 토큰
> **세미콜론 `;`은 “문장 종결자”**이며, `Block`은 종결자 없이 문장으로 취급된다.

---

### 16.1 Lexical (토큰 레벨)

```ebnf
Letter        := /* Unicode XID_Start (권장) 또는 구현상 IDENT_START */
LetterCont    := /* Unicode XID_Continue (권장) 또는 구현상 IDENT_CONT */

Ident         := Letter LetterCont* | BacktickIdent ;
BacktickIdent := "`" /* UTF-8 any (incl emoji) except ` and newline */ "`" ;

IntLit        := Digit (Digit | "_")* IntSuffix? ;
IntSuffix     := "i8" | "i16" | "i32" | "i64" | "u8" | "u16" | "u32" | "u64" ;

FloatLit      := Digit (Digit | "_" | ".")* FloatSuffix ;
FloatSuffix   := "f" | "lf" ;

BoolLit       := "true" | "false" ;
NullLit       := "null" ;

CharLit       := "'" CharBody "'" ;
CharBody      := /* 'a' | escape sequences | '\u{HEX+}' etc. */ ;

StringLit     := NormalString | FString | RawString ;
NormalString  := "\"" /* ... */ "\"" ;
FString       := "F\"\"\"" /* ... { Expr } ... */ "\"\"\"" ;
RawString     := "R\"\"\"" /* ... no interpolation ... */ "\"\"\"" ;

Comment       := LineComment | BlockComment ;
LineComment   := "//" /* to end of line */ ;
BlockComment  := "/*" /* ... */ "*/" ;   /* non-nested */

WS            := /* spaces/tabs/newlines */ ;
```

---

### 16.2 프로그램 구조: project/bundle/module/file 단위

```ebnf
File          := FileItem* EOF ;

FileItem      := ImportStmt
               | UseStmt
               | NamespaceDecl
               | Decl
               | ";"              /* 허용: 빈 문장(파일 스코프) */
               ;
```

---

### 16.3 경로 / 네임스페이스(nest)

```ebnf
Path          := Ident ("::" Ident)* ;

NamespaceDecl := ExportOpt "nest" Path ( ";" | BlockNamespace ) ;
BlockNamespace:= "{" NamespaceItem* "}" ;

NamespaceItem := UseStmt
               | NamespaceDecl
               | Decl
               | ";" ;
```

### 16.4 `import`/`use` 문

```ebnf
ImportStmt    := "import" Path ("as" Ident)? ";" ;

UseStmt       := "use" UseBody ";" ;

UseBody       := UseTypeAlias
               | UsePathAlias
               | UseTextSubst
               ;

UseTypeAlias  := Ident "=" Type ;
UsePathAlias  := Path "=" Ident ;              /* 경로 별칭 */
UseTextSubst  := Ident Expr ;                  /* IDENT 단위 치환(매크로 함수 금지) */
```

---

### 16.5 선언(Decl) 전체

```ebnf
Decl          := FuncDecl
               | FieldDecl
               | ProtoDecl
               | TabletDecl
               | ActsDecl
               | ClassDecl
               | GlobalVarDecl
               ;

ExportOpt     := "export" | ε ;

Attribute     := "@" Ident
               | "@{" AttrItem ("," AttrItem)* "}" ;
AttrItem      := Ident (":" AttrValue)? ;
AttrValue     := Ident | IntLit | FloatLit | StringLit | BoolLit | NullLit ;

AccessMod     := "public" ":" | "private" ":" ;
```

---

### 16.6 전역/정적 변수 선언

```ebnf
GlobalVarDecl    := CAbiGlobalDecl | NormalGlobalDecl ;

CAbiGlobalDecl   := LinkPrefix "static" MutOpt Ident ":" Type CAbiInitOpt ";" ;
LinkPrefix       := ("extern" | "export") "\"C\"" ;
CAbiInitOpt      := "=" Expr | ε ;              /* extern 경로에서는 ε */

NormalGlobalDecl := StorageOpt MutOpt VarKw IdentVarDecl ";" ;
StorageOpt       := "static" | ε ;
MutOpt           := "mut" | ε ;
VarKw            := "let" | "set" ;

IdentVarDecl     := LetDecl | SetDecl ;
LetDecl          := Ident ":" Type "=" Expr ;
SetDecl          := Ident "=" Expr ;
```

---

### 16.7 함수 선언 (attribute/export/mode/qualifier/예외접미 `?`/params/named-group)

```ebnf
FuncDecl      := CAbiFuncDecl | NormalFuncDecl ;

CAbiFuncDecl  := LinkPrefix "def" FuncName FuncParams "->" Type (Block | ";") ;
NormalFuncDecl:= Attribute* ExportOpt "def" ModeOpt QualifierOpt FuncName FuncParams "->" Type Block ;

FuncName      := Ident QMarkOpt ;
QMarkOpt      := "?" | ε ;                     /* 예외 허용 함수 표기 */

ModeOpt       := "sub" | "pub" | ε ;           /* class 문맥에서 의미 있음 */
QualifierOpt  := "pure" | "comptime" | ε ;     /* 또는 attribute로만 쓰는 정책도 가능 */

FuncParams    := "(" ParamSectionOpt ")" ;

ParamSectionOpt :=
                 ε
               | PositionalParamsOpt NamedGroupOpt
               ;

PositionalParamsOpt :=
                 PosParam ("," PosParam)* ("," )?
               | ε ;

PosParam      := Ident ":" Type DefaultOpt ;

NamedGroupOpt := "{" NamedParamListOpt "}" | ε ;
NamedParamListOpt :=
                 NamedParam ("," NamedParam)* ("," )?
               | ε ;

NamedParam    := Ident ":" Type DefaultOpt ;

DefaultOpt    := "=" Expr | ε ;
```

---

### 16.8 타입(Type) 문법 (nullable, 배열/리스트, borrow/slice, handle, 제네릭(파싱 선행))

```ebnf
Type          := TypePrimary TypeSuffix* ;

TypePrimary   := "(" Type ")" 
               | "handle" "<" Type ">"
               | SliceType
               | PathType
               | ArrayOrListType
               ;

PathType      := Path GenericArgsOpt ;
GenericArgsOpt:= "<" Type ("," Type)* ">" | ε ;

ArrayOrListType :=
                 PathType "[" ArrayLenOpt "]"
               | "(" Type ")" "[" ArrayLenOpt "]" ;      /* 괄호 타입도 지원 */

ArrayLenOpt   := Expr | ε ;                 /* empty => list T[] */
               /* T[N] : ArrayLenOpt=Expr, T[] : ArrayLenOpt=ε */

SliceType     := RefPrefix "[" Type "]" ;   /* &[T], &mut [T] */

RefPrefix     := "&" MutOpt ;               /* &, &mut */

TypeSuffix    := "?" ;                      /* nullable: T? (접미가 &보다 우선 결합 권장) */
```

> **주의(우선순위 정책 반영)**: `&i32?`는 문법적으로 `RefPrefix`가 `TypePrimary`의 한 형태가 아니라, `SliceType` 외에는 `&`가 “표현식 연산자”에 가까워 충돌 소지가 있으므로, 타입에서 `&T` 자체를 원한다면 아래 “RefType” 확정을 선택해도 된다. (아래 16.15 모호점 참고)

---

### 16.9 클래스 / field / proto / tablet / acts

```ebnf
FieldDecl     := "field" FieldLayoutOpt FieldAlignOpt Ident BlockField ;
FieldLayoutOpt:= "layout" "(" "c" ")" | ε ;
FieldAlignOpt := "align" "(" IntLit ")" | ε ;
BlockField    := "{" FieldMember* "}" ;
FieldMember   := Ident ":" Type ";" ;       /* field 내부 함수 금지 */

ProtoDecl     := ExportOpt "proto" Ident BlockProto ;
BlockProto    := "{" ProtoMember* "}" ;
ProtoMember   := FuncSig ";" ;              /* 본문 없는 시그니처 */

TabletDecl    := ExportOpt "tablet" Ident InheritOpt BlockTablet ;
InheritOpt    := ":" Type ("," Type)* | ε ;

BlockTablet   := "{" TabletMember* "}" ;
TabletMember  := AccessMod
               | FuncDecl
               | CtorDecl
               | DtorDecl
               | MemberVarDecl
               | ";" ;

MemberVarDecl := MutOpt ("let" | "set") IdentVarDecl ";" ;

CtorDecl      := "construct" CtorSigOpt CtorBodyOrDelete ;
CtorSigOpt    := FuncParamsOpt ;            /* 선택: construct(...) */
FuncParamsOpt := "(" ParamSectionOpt ")" | ε ;
CtorBodyOrDelete := Block | "= delete" ";" ;

DtorDecl      := "destruct" DtorBodyOrDelete ;
DtorBodyOrDelete := Block | "= delete" ";" ;

ActsDecl      := ExportOpt "acts" Type BlockActs ;
BlockActs     := "{" ActsMember* "}" ;
ActsMember    := FuncDecl
               | OperatorDecl
               | ";" ;

OperatorDecl  := "op" OperatorName FuncParams "->" Type Block ;
OperatorName  := Ident | PunctOp ;

PunctOp       := "+" | "-" | "*" | "/" | "%" | "==" | "!=" | "<" | "<=" | ">" | ">="
               | "[]" | "()" | "++" | "=" | "+=" | "-=" | "*=" | "/=" | "%="
               | "and" | "or" | "xor" | "not" ;
```

---

### 16.10 class + draft/pub/sub + commit/recast (구문 레벨)

```ebnf
ClassDecl     := ExportOpt "class" Ident BlockClass ;

BlockClass    := "{" ClassMember* "}" ;

ClassMember   := AccessMod
               | FuncDecl
               | MemberVarDecl
               | ";" ;
```

---

### 16.11 문장(Statement) / 블록 / 바인딩

```ebnf
Block         := "{" Stmt* "}" ;

Stmt          := ";"
               | Block
               | VarStmt
               | IfExprStmt
               | WhileStmt
               | LoopStmt
               | SwitchStmt
               | BreakStmt
               | ContinueStmt
               | ReturnStmt
               | ThrowStmt
               | TryCatchStmt
               | AttemptStmt
               | CommitStmt
               | RefreshStmt
               | RecastStmt
               | DeleteStmt
               | ExprStmt
               ;

VarStmt       := StorageOpt MutOpt VarKw IdentVarDecl ";" ;

ExprStmt      := Expr ";" ;
```

---

### 16.12 if / while / loop / switch

```ebnf
IfExprStmt    := IfExpr ";"? ;          /* if는 표현식이지만, stmt로도 사용 가능 */

IfExpr        := "if" "(" Expr ")" Block ElifPart* ElsePartOpt ;
ElifPart      := "elif" "(" Expr ")" Block ;
ElsePartOpt   := "else" Block | ε ;

WhileStmt     := "while" "(" Expr ")" Block ;

LoopStmt      := "loop" LoopBody ;
LoopBody      := Block
               | "(" Ident "in" Expr ")" Block ;

SwitchStmt    := "switch" "(" Expr ")" "{" SwitchCase* DefaultCaseOpt "}" ;
SwitchCase    := "case" CaseLabel ":" Block ;
DefaultCaseOpt:= "default" ":" Block | ε ;

CaseLabel     := Literal
               | Pattern ;

Pattern       := Ident "(" PatternArgsOpt ")" ;
PatternArgsOpt:= Ident
               | Ident ":" Ident
               | ε ;
```

---

### 16.13 break/continue/return/throw/try-catch/attempt/commit/delete/recast

```ebnf
BreakStmt     := "break" BreakValueOpt ";" ;
BreakValueOpt := Expr | ε ;

ContinueStmt  := "continue" ";" ;

ReturnStmt    := "return" ReturnValueOpt ";" ;
ReturnValueOpt:= Expr | ε ;

ThrowStmt     := "throw" Expr ";" ;

TryCatchStmt  := "try" Block "catch" "(" CatchBinder ")" Block ;
CatchBinder   := Ident (":" Type)? ;    /* 예: catch(e: Error) */

AttemptStmt   := "attempt" Expr ";" ;   /* 또는 "set r = attempt expr;"를 Expr로 취급할 수도 있음 */

CommitStmt    := "commit" ";" ;
RecastStmt    := "recast" RecastArgOpt ";" ;
RecastArgOpt  := Expr | ε ;

DeleteStmt    := "delete" PlaceExpr ";" ;
```

---

### 16.14 표현식(Expression) (우선순위/결합 규칙 포함)

> 아래는 **프랫/우선순위 파서 제작 가능한 형태**로 계층을 분리했다.
> (v0 규칙: 삼항 `?:` 중첩 금지, `<<` 파이프는 RHS가 CallExpr이며 hole `_` 1개 강제)

```ebnf
Expr          := AssignExpr ;

AssignExpr    := TernaryExpr (AssignOp AssignExpr)? ;
AssignOp      := "=" | "+=" | "-=" | "*=" | "/=" | "%=" ;

TernaryExpr   := PipeExpr ("?" PipeExpr ":" PipeExpr)? ; /* v0: nested ternary 금지(검사로 제약) */

PipeExpr      := OrExpr ( "<<" PipeRhs )* ;
PipeRhs       := CallExpr ;  /* v0 강제: RHS는 CallExpr */

OrExpr        := XorExpr ( "or" XorExpr )* ;
XorExpr       := AndExpr ( "xor" AndExpr )* ;
AndExpr       := EqExpr  ( "and" EqExpr )* ;

EqExpr        := RelExpr ( ("==" | "!=") RelExpr )* ;
RelExpr       := AddExpr ( ("<" | "<=" | ">" | ">=") AddExpr )* ;

AddExpr       := MulExpr ( ("+" | "-") MulExpr )* ;
MulExpr       := PrefixExpr ( ("*" | "/" | "%") PrefixExpr )* ;

PrefixExpr    := PrefixOp PrefixExpr
               | PostfixExpr ;

PrefixOp      := "not" | "!" | "+" | "-" | "++"
               | BorrowOp
               | EscapeOp
               ;

BorrowOp      := "&" MutOpt ;            /* &x, &mut x */
EscapeOp      := "&&" ;                  /* &&x */

PostfixExpr   := PrimaryExpr PostfixOp* ;
PostfixOp     := CallSuffix
               | IndexSuffix
               | MemberSuffix
               | PostIncSuffix ;

CallSuffix    := "(" ArgListOpt ")" ;
IndexSuffix   := "[" Expr "]" ;
MemberSuffix  := "." Ident ;
PostIncSuffix := "++" ;

PrimaryExpr   := Literal
               | IdentRef
               | Hole
               | ParenExpr
               | ArrayLiteral
               | LambdaExpr
               ;

ParenExpr     := "(" Expr ")" ;

IdentRef      := Path ;                  /* 변수/함수/네임스페이스 접근 */
Hole          := "_" ;                    /* 파이프/호출 규칙에서 “라벨 인자 자리”로만 허용(검사 패스) */

Literal       := IntLit | FloatLit | BoolLit | NullLit | CharLit | StringLit ;

ArrayLiteral  := "[" ExprListOpt "]" ;
ExprListOpt   := (Expr ("," Expr)*)? ("," )? ;

ArgListOpt    := ArgList | ε ;
ArgList       := PosArgsOpt NamedArgsOpt ;    /* 혼합 규칙은 아래에서 제약 */

PosArgsOpt    := (Expr ("," Expr)*)? ("," )? | ε ;

NamedArgsOpt  := NamedGroupArgs | LabeledArgs | ε ;

LabeledArgs   := LabelArg ("," LabelArg)* ("," )? ;
LabelArg      := Ident ":" ExprOrHole ;
ExprOrHole    := Expr | "_" ;

NamedGroupArgs:= ("," )? "{" NamedGroupArgListOpt "}" ;
NamedGroupArgListOpt := (NamedGroupArg ("," NamedGroupArg)*)? ("," )? ;
NamedGroupArg := Ident ":" Expr ;

LambdaExpr    := CaptureList LambdaParamsOpt LambdaRetOpt Block ;
CaptureList   := "[" CaptureItemsOpt "]" ;
CaptureItemsOpt := (CaptureItem ("," CaptureItem)*)? ("," )? ;
CaptureItem   := MutOpt Ident ;

LambdaParamsOpt := "(" ParamSectionOpt ")" ;     /* v0: 예시가 [](x:int){...} 형태 */
LambdaRetOpt  := ("->" Type)? ;
```

---

### 16.15 Place Expression (place 제약: `&`, `&&`, delete 대상)

```ebnf
PlaceExpr     := PlaceAtom PlaceSuffix* ;

PlaceAtom     := IdentRef | "(" PlaceExpr ")" ;

PlaceSuffix   := "." Ident
               | "[" Expr "]" ;
```

---
