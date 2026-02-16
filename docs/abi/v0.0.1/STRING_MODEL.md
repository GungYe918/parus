# Parus String Model Supplement (`v0.0.1`)

문서 상태: `Normative Supplement`

이 문서는 `docs/abi/v0.0.1/ABI.md`의 문자열 관련 요약을 상세화한다.
충돌 처리 우선순위는 다음과 같다.

1. `docs/abi/v0.0.1/ABI.md`
2. 본 문서
3. `docs/spec_v0.md`

---

## 1. 타입 계층

Parus v0 문자열 계층은 다음 2단계로 고정한다.

1. 빌트인 슬라이스: `text`
2. 표준 라이브러리 소유 문자열: `String`

규칙:

1. `text`는 비소유/불변 UTF-8 뷰다.
2. `String`은 소유 UTF-8 버퍼 타입이며 빌트인이 아니다.
3. 사용자는 `text` 변수를 직접 선언/반환/인자로 사용할 수 있다.

---

## 2. 리터럴 규칙

`""` 리터럴 규칙:

1. `""`의 기본 타입은 항상 `text`다.
2. 표준 라이브러리 링크 여부는 `""` 기본 타입을 바꾸지 않는다.
3. `R"""..."""`, `F"""..."""`는 lexer/parser에서 문자열 리터럴로 활성화한다.
4. `F"""..."""`의 표현식 보간(lowering)은 v0에서 아직 미구현이며, 현재는 본문 텍스트를 그대로 `text`로 취급한다.

---

## 3. 변환 규칙

`text`와 `String` 변환은 비용이 큰 경로를 명시적으로 드러내야 한다.

1. `text -> String` 암시 변환은 금지한다.
2. `text -> String`은 표준 라이브러리 API 호출로만 허용한다.
3. `String -> text`는 무할당 뷰 획득 API로 제공한다.

권장 API 이름:

1. `String::from_text(src: text) -> String`
2. `String::as_text(self: &String) -> text`
3. `String::into_utf8_buf(self: String) -> Utf8Buf`
4. `String::from_utf8_buf(buf: Utf8Buf) -> String`

---

## 4. C ABI 경계 타입 (고정)

C ABI 경계 문자열 전달은 다음 2개 타입으로 고정한다.

```parus
field layout(c) Utf8Span {
  data: ptr u8;
  len: usize;
}

field layout(c) Utf8Buf {
  data: ptr mut u8;
  len: usize;
  cap: usize;
}
```

규칙:

1. C 경계에서 `String` 직접 값 전달은 금지한다.
2. 읽기 전용 인자는 `Utf8Span`을 사용한다.
3. 소유권 이동/반납 계약이 필요한 경우 `Utf8Buf`를 사용한다.
4. `Utf8Buf`는 free 주체를 API 계약에 반드시 명시해야 한다.

---

## 5. `String` 내부 ABI (Parus Internal ABI)

`String` 내부 레이아웃은 3워드로 고정한다.

```text
data: ptr mut u8
len : usize
cap : usize
```

의미:

1. `data`: UTF-8 바이트 시작 주소
2. `len`: 현재 길이(바이트)
3. `cap`: 수용량(바이트) + 상태 플래그 비트

권장 플래그:

1. `cap` 최상위 비트(`STATIC_BIT`)는 static backing 여부를 나타낸다.
2. `cap & ~STATIC_BIT`는 실제 수용량으로 해석한다.

불변 조건:

1. `len <= capacity(cap)` 이어야 한다.
2. `STATIC_BIT`가 켜진 경우 drop/free는 no-op 이어야 한다.
3. `STATIC_BIT`가 켜진 상태에서 쓰기 연산이 필요하면 먼저 heap 버퍼로 분리(detach)해야 한다.

---

## 6. 표준 라이브러리 구현 지침 (`field + acts`)

표준 라이브러리의 최소 구현 형태:

```parus
field String {
  data: ptr mut u8;
  len: usize;
  cap: usize;
}

acts for String {
  fn from_text(src: text) -> String;
  fn as_text(self s: &String) -> text;
  fn into_utf8_buf(self s: String) -> Utf8Buf;
  fn from_utf8_buf(buf: Utf8Buf) -> String;
  fn drop(self s: String) -> void;
}
```

구현 강제사항:

1. `from_text`는 리터럴/static 입력에서 무할당 경로를 제공해야 한다.
2. `drop`은 static backing에서 free를 호출하면 안 된다.
3. 할당/복제는 오버로드 해소나 암시 변환이 아니라 API 호출에서만 발생해야 한다.
4. JIT/AOT 모두 동일한 3워드 레이아웃을 관찰해야 한다.

---

## 7. 성능/컴파일 규약

1. 리터럴 경로는 컴파일타임에 static 배치를 완료해야 한다.
2. `text` 연산은 무할당, 무가상호출 경로를 기본으로 한다.
3. `text -> String` 비용 경로는 소스 코드에 명시되어야 한다.
4. 오버로드 해소는 런타임 디스패치를 도입하지 않는다.
