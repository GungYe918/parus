# Parus ABI Specification v0.0.1

문서 버전: `v0.0.1`  
상태: `Normative (Single Source of Truth)`

이 문서는 Parus ABI 구현의 단일 신뢰 기준이다.  
`docs/reference/language/SPEC.md`와 충돌 시, ABI 관련 사항은 본 문서를 우선한다.
OOP/다형성/행동 부착 관련 사항은 `docs/reference/abi/v0.0.1/OOP_MODEL.md`를 우선한다.
제네릭/제약 표기 관련 사항은 `docs/reference/abi/v0.0.1/GENERICS_MODEL.md`를 우선한다.

---

## 1. 목적

Parus ABI는 아래 목표를 동시에 만족해야 한다.

1. C FFI 경계에서 안정적인 호출/링크 보장
2. 내부 최적화(OIR/LLVM) 자유도 유지
3. DOD/벡터화 친화 구조를 해치지 않는 레이아웃 정책 유지

---

## 2. ABI 계층

Parus는 ABI를 2계층으로 나눈다.

1. **Parus Internal ABI**
   - 프론트엔드/중간 IR(SIR/OIR)/백엔드 내부용
   - 성능/최적화를 위해 변경 가능
   - 외부 언어에 안정성 약속하지 않음
2. **Parus C ABI (`c-v0`)**
   - C/타 언어 FFI 경계용
   - 본 문서에서 안정 subset을 강하게 고정

---

## 3. 버전 규칙

Parus ABI 문서 버전은 `MAJOR.MINOR.PATCH`를 사용한다.

1. `MAJOR`
   - Parus ABI가 크게 변경될 때 증가
   - C ABI 라인은 별도 공지 없으면 유지
2. `MINOR`
   - ABI 규약이 중간 수준으로 변경/확장될 때 증가
3. `PATCH`
   - 문서 표현 수정만 있을 때 증가 (ABI 의미 완전 동일)

현재 버전:

- Parus ABI 문서: `v0.0.1`
- C ABI 라인: `c-v0`

---

## 4. C ABI 선언 문법 (고정)

FFI는 `use ...::ffi`로 선언하지 않는다.  
FFI 경계는 선언 키워드로 고정한다.

### 4.1 외부 심볼 가져오기

```parus
extern "C" def puts(s: *const u8) -> i32;
extern "C" static mut errno: i32;
```

### 4.2 심볼 내보내기

```parus
export "C" def parus_add(a: i32, b: i32) -> i32 {
    return a + b;
}
```

규칙:

1. `export "C"` / `extern "C"`는 함수/전역 심볼 선언에만 적용
2. `struct` 선언에 `export`/`extern`을 붙이지 않는다

---

## 5. 레이아웃 규칙 (struct)

FFI와 메모리 모델은 레이아웃 키워드로 명시한다.

```parus
struct layout(c) align(16) Vec2 {
    x: f32;
    y: f32;
}
```

규칙:

1. C ABI 경계로 노출되는 `struct`는 `layout(c)`를 명시해야 한다
2. `align(n)`은 ABI와 성능 정책(벡터화/캐시 정렬) 양쪽에 사용 가능하다
3. `layout(c)` struct 멤버에는 optional(`T?`)을 허용하지 않는다 (`c-v0` 안전 정책)
4. `struct` 멤버 선언에는 `mut` 키워드를 붙이지 않는다 (가변성은 바인딩에서 표현)
5. `@{repr:"C"}` 형태는 ABI 공식 문법으로 채택하지 않는다

---

## 6. 포인터 타입 표기 (고정)

Raw pointer 표기는 `*const/*mut`로 고정한다.

1. `*const T`
   - 읽기 전용 pointee
2. `*mut T`
   - 쓰기 가능 pointee

예:

```parus
extern "C" def read(buf: *const u8, len: usize) -> isize;
extern "C" def write(buf: *mut u8, len: usize) -> isize;
```

---

## 7. C ABI 안정 subset (`c-v0`)

### 7.1 허용 타입

1. 정수: `i8/i16/i32/i64`, `u8/u16/u32/u64`, `isize/usize`
2. 부동소수: `f32/f64`
3. 포인터: `*const T`, `*mut T` (T가 FFI-safe일 때)
4. `layout(c)`를 만족하는 `struct`
5. 문자열 뷰 경계 타입: `layout(c) struct Utf8Span { data: *const u8; len: usize; }`
6. 문자열 버퍼 경계 타입: `layout(c) struct Utf8Buf { data: *mut u8; len: usize; cap: usize; }`

### 7.2 금지 타입

1. borrow/escape 관련 타입 (`&`, `&mut`, `&&`)
2. optional, actor, class 직접 값 전달
3. 구현 의존 내부 타입
4. 표준 라이브러리 `String` 직접 값 전달 (`Utf8Span`/`Utf8Buf`로 변환해야 함)
5. `text` 직접 C ABI 전달 (`*const core::ext::c_char` + 명시 변환 경계 사용)

### 7.3 오버로딩/심볼 규칙

1. C ABI 경계 함수는 오버로딩을 허용하지 않는다
2. 동일 C 심볼의 중복 선언/정의는 컴파일 에러다
3. `export "C"` 심볼은 C ABI용 비맹글 심볼로 취급한다

### 7.4 `core::ext` C ABI 타입 규칙 (v0)

1. C ABI 경계 타입은 `core::ext::c_*` 및 `core::ext::vaList`를 표준 표면으로 사용한다.
2. `core::ext::c_*`는 대응 C ABI 원시 타입과 동치(transparent alias)로 취급한다.
3. `vaList`는 opaque 타입이며 C ABI 시그니처 경계에서만 허용한다.
4. plain string literal(`"..."`)은 C `char*` 기대 슬롯에서만 암묵 허용한다(주로 `*const core::ext::c_char` 계열).
5. `text` 변수/고수준 Parus 타입은 C ABI 경계에서 암묵 변환하지 않는다.
6. `c"..."`, `cr"..."`는 compile-time literal-only `core::ext::CStr` 생성 경로로 허용한다.
7. `text -> c_str` 자동 브리지는 v0 범위에서 제외한다.
8. C variadic(`...`)는 `extern "C"` 선언과 `cimport`된 함수 표면에서만 허용한다.
9. `manual[abi]`는 variadic 함수 자체가 아니라 실제 variadic tail 인자를 넘기는 call site에만 필요하다. zero-tail 호출과 ordinary fixed-arg C 호출은 `manual[abi]`를 요구하지 않는다.
10. variadic tail은 ABI-safe scalar/raw pointer/`core::ext::CStr`/plain string literal만 허용한다.
11. variadic tail에서는 `null` literal, borrow, escape, optional, aggregate, direct enum을 허용하지 않는다.
12. `null` literal은 typed pointer slot의 call-arg/return/assignment 경계에서만 허용한다. `T? -> *const/*mut T` 자동 변환은 계속 금지한다.

`core::ext` 매핑 표(요약):

| Parus type | C counterpart | ABI 폭 규칙 |
| --- | --- | --- |
| `core::ext::c_void` | `void` | 값 타입이 아니라 경계 표기용 |
| `core::ext::c_char` | `char` | target signedness 따름 |
| `core::ext::c_schar` | `signed char` | 8-bit |
| `core::ext::c_uchar` | `unsigned char` | 8-bit |
| `core::ext::c_short` | `short` | 16-bit |
| `core::ext::c_ushort` | `unsigned short` | 16-bit |
| `core::ext::c_int` | `int` | 32-bit |
| `core::ext::c_uint` | `unsigned int` | 32-bit |
| `core::ext::c_long` | `long` | target ABI(LP64/LLP64) |
| `core::ext::c_ulong` | `unsigned long` | target ABI(LP64/LLP64) |
| `core::ext::c_longlong` | `long long` | 64-bit |
| `core::ext::c_ulonglong` | `unsigned long long` | 64-bit |
| `core::ext::c_float` | `float` | 32-bit IEEE |
| `core::ext::c_double` | `double` | 64-bit IEEE |
| `core::ext::c_size` | `size_t` | target pointer-width |
| `core::ext::c_ssize` | `ssize_t` | target pointer-width signed |
| `core::ext::c_ptrdiff` | `ptrdiff_t` | target pointer-width signed |
| `core::ext::vaList` | `va_list` | opaque, C ABI parameter 경계 전용 |

`core::ext` Rust `core::ffi` parity 상태(요약):

| Rust `core::ffi` item | Parus status | Notes |
| --- | --- | --- |
| C scalar aliases (`c_int`, ...) | Implemented | `core::ext::c_*` |
| `VaList` | Implemented (restricted) | C ABI parameter boundary only |
| `CStr` | Implemented (subset) | Borrowed view helpers only |
| `CString` | Excluded | allocator 의존으로 v0 제외 |
| `c"..."`, `cr"..."` | Implemented (literal-only) | compiler builtin C string literals |
| `c_str!` | Excluded | 이름 기반 매크로 표면은 미도입 |

---

## 8. 예외/언와인드 경계

Parus v0는 예외 채널을 2개로 분리한다.

1. Recoverable 채널 (`throw` 기반): 값 전달 채널(비-언와인딩)
2. Unrecoverable 채널 (panic/foreign fault): 스택 언와인딩 채널

언와인딩 런타임은 코어 필수이며, 내부 경로에서는 Unrecoverable 채널을 처리할 수 있어야 한다.
단, FFI 경계 밖으로는 언와인드를 전파하지 않는다.

1. `extern "C"` 호출로 들어온 경계에서 언와인드가 외부로 전파되면 안 된다.
2. `export "C"` 함수도 C ABI 경계 밖으로 언와인드를 전파하지 않는다.
3. C/C++ 경계에서는 foreign unwind를 반드시 포획/변환해 ABI-safe 결과로 반환해야 한다.
4. Recoverable(`throw`)는 ABI 경계에서 값으로 변환하거나 명시 반환 채널로 브리지해야 한다.

### 8.1 C import TLS/Thread 경계 (고정)

1. `import "X.h" as c;`로 들어온 `_Thread_local`/`__thread` 전역은 imported TLS global로 취급한다.
2. TLS 초기화/소멸(ctor/dtor) 책임은 외부 C 런타임/로더에 있다.
3. Parus 컴파일러/런타임은 imported TLS에 대한 별도 ctor/dtor 실행 경로를 추가하지 않는다.
4. imported C global/TLS 접근은 thread-safe를 보장하지 않는다(호출/접근 effect는 보수적으로 유지).

---

## 9. DOD/SIMD 지향성 반영 원칙

Parus는 DOD 친화적 구조를 유지하되, 외부 ABI는 단순/안정하게 유지한다.

1. 외부(C ABI): 단순 `layout(c)`와 명시적 정렬 규칙
2. 내부(Parus Internal ABI): OIR/LLVM에서 벡터화/재배치/최적화 자유
3. ABI 안정성과 내부 고성능 경로를 분리해 양립한다

---

## 10. 구현 체크리스트

다음 항목을 통과해야 `c-v0` 준수로 본다.

1. 파서가 `extern "C"` / `export "C"`를 인식한다
2. 타입체커가 FFI-safe 타입 제한을 강제한다
3. `struct layout(c)`와 `align(n)` 규칙을 검증한다
4. C ABI 심볼 중복/충돌을 진단한다
5. 백엔드가 C ABI 심볼을 비맹글로 출력한다
6. 문자열 C 경계 타입(`Utf8Span`, `Utf8Buf`) 레이아웃이 문서와 일치한다

---

## 11. 문자열/저장소/OOP 정책 연결

본 문서는 문자열 및 저장소 정책을 "요약만" 포함한다.
상세 규칙은 아래 보조 명세를 따른다.

1. 문자열 모델/변환/표준 라이브러리 구현 지침:
   - `docs/reference/abi/v0.0.1/STRING_MODEL.md`
2. 스택/힙/정적 저장소 정책 및 `&&` 비힙 규칙:
   - `docs/reference/abi/v0.0.1/STORAGE_POLICY.md`
3. nullable(`T?`) 의미 규칙/승격 정책/하강 정합성:
   - `docs/reference/abi/v0.0.1/NULLABLE_MODEL.md`
4. OOP 역할 분리/`self`-`Self`/`acts`-`proto`-`class`-`actor` 규칙:
   - `docs/reference/abi/v0.0.1/OOP_MODEL.md`
5. 제네릭/제약 표기(`with [ ... ]`) 및 proto 결합 방향:
   - `docs/reference/abi/v0.0.1/GENERICS_MODEL.md`

요약 고정 규칙:

1. 빌트인 문자열 슬라이스 타입 이름은 `text`다.
2. `""`의 기본 타입은 표준 라이브러리 링크 여부와 무관하게 항상 `text`다.
3. `text -> String` 암시 변환은 허용하지 않는다.
4. `&&`는 힙에 materialize할 수 없다.
5. C ABI 문자열 경계는 `Utf8Span`/`Utf8Buf`를 사용한다.
6. nullable은 하이브리드 모델을 따른다.
   - `T?`는 1급 값 타입이다.
   - `T -> T?`는 대입 경계에서만 허용한다.
   - `T <: T?` 전역 승격은 허용하지 않는다.
   - `null` literal은 typed pointer slot 경계에서만 예외적으로 허용한다.
7. OOP 모델은 역할 분리를 고정한다.
   - `acts for` 부착 대상: `struct`, `class`
   - `actor`는 `commit/recast` 상태머신 보호를 위해 `acts for` 부착 금지
   - `proto`는 계약 전용이며 연산자 재정의를 담당하지 않는다
   - `acts for` 계열 함수는 `self`/`self mut` 리시버를 첫 파라미터로 강제한다 (`self move`는 v0 보류)
   - acts 선택은 `use T with acts(...)` 및 바인딩 sugar(`let/set ... = expr with acts(...)`)를 따른다
8. 제네릭 제약 표기는 `with [ ... ]` 단일 문법으로 고정한다.
   - `with T: P`(브래킷 없는 with)와 `[]` 단독 제약 절은 허용하지 않는다.
   - 제약이 1개여도 `with [T: P]`를 유지한다.

---

## 12. 변경 이력

### v0.0.1

1. ABI 문서 체계 최초 고정
2. FFI 선언을 `extern "C"` / `export "C"`로 통일
3. `struct` 레이아웃 표기를 `layout(c)`/`align(n)`로 고정
4. 포인터 표기를 `*const` / `*mut`로 고정

### v0.0.1 addendum (string/storage)

1. C 경계 문자열 표준 타입 `Utf8Span`/`Utf8Buf`를 명시
2. `String` 직접 C ABI 전달 금지 규칙을 명시
3. `text` 기본 리터럴 타입, `text -> String` 비암시 변환 규칙 요약 추가
4. `&&` 비힙 materialization 규칙 요약 추가
5. 상세 명세를 `STRING_MODEL.md`, `STORAGE_POLICY.md`로 분리
6. nullable 하이브리드 정본 문서 `NULLABLE_MODEL.md` 연결 규칙 추가
