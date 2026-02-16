# Parus v0 Manual Keyword Specification

문서 버전: `v0.0.1`  
상태: `Normative Draft`

이 문서는 Parus의 `manual` 키워드 규칙을 독립적으로 정의한다.  
`docs/spec_v0.md`가 큰 단일 문서이므로, `manual` 관련 규약은 본 문서를 기준으로 관리한다.

---

## 1. 목적

1. FFI/시스템 경계에서 필요한 저수준 포인터 연산 허용
2. 컴파일러의 기본 안전 규칙을 전부 끄지 않고, 필요한 권한만 좁게 부여
3. 코드 리뷰/감사에서 위험 지점을 명확하게 드러내기

---

## 2. 핵심 원칙

1. `manual`은 “무제한 권한 모드”가 아니다.
2. `manual`은 함수/모듈 전체가 아니라 **블록 단위**로만 적용된다.
3. `manual` 내부에서도 `&`, `&mut`, `&&` 규칙(CapCheck/escape 규칙)은 그대로 유지된다.
4. ABI 경계에서 borrow/escape 타입 전달 금지 규칙은 `manual`로 해제되지 않는다.
5. `because ...` 같은 사유 문자열/식별자는 언어 문법으로 도입하지 않는다.

---

## 3. 문법 (v0)

```ebnf
ManualStmt := "manual" "[" ManualPermList "]" Block ;

ManualPermList := ManualPerm ("," ManualPerm)* ;

ManualPerm := "get" | "set" | "abi" ;
```

설명:

1. `manual`은 문(statement)이며, 항상 블록 본문을 가진다.
2. `manual` 권한은 해당 블록 내부에서만 유효하다.
3. `set` 권한은 `get` 권한을 포함한다(읽기+쓰기).

---

## 4. 권한 모델

### 4.1 `get`

읽기 중심 저수준 동작 허용:

1. raw pointer 역참조 읽기
2. pointer offset/index 계산
3. pointer <-> integer 캐스트(읽기 경로)

### 4.2 `set`

쓰기 중심 저수준 동작 허용:

1. raw pointer 역참조 쓰기
2. 메모리 갱신(store)
3. `get`에 포함된 동작 전체

### 4.3 `abi`

ABI 경계 호출 허용:

1. `extern "C"` 함수 호출
2. 향후 syscall 진입점(도입 시) 호출

---

## 5. 허용/금지 동작

## 5.1 허용

1. FFI 함수가 받는 `ptr`/`ptr mut` 버퍼를 직접 순회
2. ABI 호출 전후 포인터 기반 복사/검증/길이 처리
3. 저수준 입출력 루틴 내부의 제한적 raw 메모리 접근

## 5.2 금지

1. `manual fn` 선언 (함수 단위 전파 금지)
2. `manual`로 borrow/escape 규칙 무효화
3. ABI 경계에 `&`, `&mut`, `&&` 타입 직접 전달
4. 언어 안전성 진단을 끄는 목적의 전역 스위치 사용

---

## 6. 격리성 규칙

1. `manual`은 블록 스코프에서만 유효하다.
2. 블록 밖으로 권한이 전파되지 않는다.
3. 함수 시그니처 자체를 `manual`로 표시하지 않는다.
4. 호출자에게 “manual 상태”를 전파하지 않는다.

---

## 7. `&`, `&&`와의 관계

`manual`의 도입 목적은 raw pointer/ABI 처리이지, borrow 의미론 해제가 아니다.

1. `&`, `&mut`는 기존 비탈출/배타 규칙 그대로 검사한다.
2. `&&`는 기존 escape 규칙 그대로 검사한다.
3. `manual` 내부에서도 borrow를 ABI 경계로 넘길 수 없다.
4. 즉 `manual`은 “포인터 표현력 확장”, “수명/권한 규칙 해제”가 아니다.

---

## 8. FFI 중심 예시

```parus
extern "C" fn c_write(fd: i32, buf: ptr u8, len: usize) -> isize;

fn write_chunk(fd: i32, buf: ptr u8, len: usize) -> isize {
    manual[get, abi] {
        return c_write(fd: fd, buf: buf, len: len);
    }
}
```

```parus
extern "C" fn c_fill(dst: ptr mut u8, len: usize, v: u8) -> void;

fn fill_zero(dst: ptr mut u8, len: usize) -> void {
    manual[set, abi] {
        c_fill(dst: dst, len: len, v: 0u8);
    }
}
```

---

## 9. 구현 지침 (Frontend/SIR/OIR)

1. 파서:
   - `manual[perm,...] { ... }`를 독립 stmt로 파싱
2. 타입체커/CapCheck:
   - 블록별 manual permission stack 유지
   - 권한 없는 raw pointer 연산/ABI 호출을 진단
3. SIR/OIR:
   - manual 권한을 블록 메타로 보존(최적화 힌트/진단용)
   - 의미론은 기존 안전 규칙을 우회하지 않음

---

## 10. 변경 정책

본 문서의 규칙 변경은 `docs/abi` 버전 정책과 동일하게 `MAJOR.MINOR.PATCH`를 따른다.

