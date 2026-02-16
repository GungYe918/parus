# Parus ABI Specification v0.0.1

문서 버전: `v0.0.1`  
상태: `Normative (Single Source of Truth)`

이 문서는 Parus ABI 구현의 단일 신뢰 기준이다.  
`docs/spec_v0.md`와 충돌 시, ABI 관련 사항은 본 문서를 우선한다.

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
extern "C" fn puts(s: ptr u8) -> i32;
extern "C" static mut errno: i32;
```

### 4.2 심볼 내보내기

```parus
export "C" fn parus_add(a: i32, b: i32) -> i32 {
    return a + b;
}
```

규칙:

1. `export "C"` / `extern "C"`는 함수/전역 심볼 선언에만 적용
2. `field` 선언에 `export`/`extern`을 붙이지 않는다

---

## 5. 레이아웃 규칙 (field)

FFI와 메모리 모델은 레이아웃 키워드로 명시한다.

```parus
field layout(c) align(16) Vec2 {
    x: f32;
    y: f32;
}
```

규칙:

1. C ABI 경계로 노출되는 `field`는 `layout(c)`를 명시해야 한다
2. `align(n)`은 ABI와 성능 정책(벡터화/캐시 정렬) 양쪽에 사용 가능하다
3. `@{repr:"C"}` 형태는 ABI 공식 문법으로 채택하지 않는다

---

## 6. 포인터 타입 표기 (고정)

Rust식 `*const/*mut` 대신 Parus 고유 표기를 사용한다.

1. `ptr T`
   - 읽기 전용 pointee
2. `ptr mut T`
   - 쓰기 가능 pointee

예:

```parus
extern "C" fn read(buf: ptr u8, len: usize) -> isize;
extern "C" fn write(buf: ptr mut u8, len: usize) -> isize;
```

---

## 7. C ABI 안정 subset (`c-v0`)

### 7.1 허용 타입

1. 정수: `i8/i16/i32/i64`, `u8/u16/u32/u64`, `isize/usize`
2. 부동소수: `f32/f64`
3. 포인터: `ptr T`, `ptr mut T` (T가 FFI-safe일 때)
4. `layout(c)`를 만족하는 `field`

### 7.2 금지 타입

1. borrow/escape 관련 타입 (`&`, `&mut`, `&&`)
2. optional, class, tablet 직접 값 전달
3. 구현 의존 내부 타입

### 7.3 오버로딩/심볼 규칙

1. C ABI 경계 함수는 오버로딩을 허용하지 않는다
2. 동일 C 심볼의 중복 선언/정의는 컴파일 에러다
3. `export "C"` 심볼은 C ABI용 비맹글 심볼로 취급한다

---

## 8. 예외/언와인드 경계

FFI 경계에서 예외/언와인드는 넘기지 않는다.

1. `extern "C"` 호출로 들어온 경계에서 언와인드가 외부로 전파되면 안 된다
2. `export "C"` 함수도 C ABI 경계 밖으로 언와인드를 전파하지 않는다

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
3. `field layout(c)`와 `align(n)` 규칙을 검증한다
4. C ABI 심볼 중복/충돌을 진단한다
5. 백엔드가 C ABI 심볼을 비맹글로 출력한다

---

## 11. 변경 이력

### v0.0.1

1. ABI 문서 체계 최초 고정
2. FFI 선언을 `extern "C"` / `export "C"`로 통일
3. `field` 레이아웃 표기를 `layout(c)`/`align(n)`로 고정
4. 포인터 표기를 `ptr` / `ptr mut`로 고정
