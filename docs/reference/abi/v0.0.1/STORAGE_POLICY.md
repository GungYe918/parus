# Parus Storage Policy Supplement (`v0.0.1`)

문서 상태: `Normative Supplement`

이 문서는 `docs/reference/abi/v0.0.1/ABI.md`의 저장소/수명 요약을 상세화한다.
특히 스택/힙/정적 저장소와 `~` 규칙을 고정한다.

---

## 1. 저장소 클래스

Parus v0 저장소 클래스는 다음으로 구분한다.

1. `register/stack`
   - 함수 프레임/SSA 값
2. `static/rodata`
   - 프로그램 수명 저장소
3. `heap`
   - 명시적 할당 API를 통한 동적 저장소

---

## 2. 타입별 기본 저장 정책

### 2.1 `text`

1. 값 자체는 2워드 헤더(`*const u8`, `usize`)로 취급한다.
2. 헤더는 register/stack에서 이동한다.
3. 실제 바이트 backing은 static/stack/heap 어디든 가능하지만 `text`는 소유권을 갖지 않는다.

### 2.2 `String`

1. 값 헤더는 3워드(`data`, `len`, `cap`)로 register/stack 이동한다.
2. backing 저장소는 static 또는 heap을 사용한다.
3. heap 사용은 명시적 `String` API 호출 경로에서만 허용한다.

### 2.3 C 경계 타입

1. `Utf8Span`/`Utf8Buf`는 `layout(c)` 값 전달을 기본으로 한다.
2. ABI 경계에서 내부 `String` 헤더를 직접 노출하지 않는다.

---

## 3. `~` 저장/ABI 규칙 (강제)

`~`는 절대 implicit heap box로 materialize할 수 없다.

허용 경로:

1. escape rvalue (cell commit/ABI pack 없음)
2. handle cell
   - local cell
   - field cell
   - optional cell
   - static cell
3. ABI handle pack
   - return boundary
   - argument boundary

금지 경로:

1. heap box로의 자동 승격
2. 런타임 allocator 호출을 동반하는 `~` 생성

용어 고정:

1. `cell commit`
   - `~T`를 local/field/optional/static place에 저장
   - `core::mem::replace` / `core::mem::swap`로 existing `~T` cell을 교체/교환하는 것도 cell mutation으로 본다
2. `abi pack`
   - `~T`를 call/return ABI 경계에서 3word handle로 구성

`cell commit`은 곧바로 `abi pack`을 의미하지 않는다.

---

## 4. `~` 저장/ABI 규칙으로 인한 제약

다음 패턴은 v0에서 제한되거나 금지될 수 있다.

1. async/suspend 경계를 넘는 `~` 유지
2. 컨테이너/배열에 `~` 장기 저장
3. direct field/index projection으로 `~` subplace를 임의 추출

권장 해결:

1. 장수명 owner cell이 필요하면 local/field/optional/static place를 사용한다.
2. one-shot extraction은 `(~T)?` + consume-binding을 사용한다.
3. initialized plain `~T` place를 교체해야 하면 `core::mem::replace` / `core::mem::swap`를 사용한다.

---

## 5. 컴파일타임/런타임 규약

1. 문자열 리터럴 backing 배치는 컴파일타임에 완료해야 한다.
2. 오버로드 해소/맹글링 단계는 런타임 분기를 도입하면 안 된다.
3. `text` 경로는 무할당을 기본으로 한다.
4. heap 경로는 명시 API 호출에서만 발생해야 한다.

---

## 6. JIT/AOT 일관성 요구사항

1. JIT/AOT 모두 `String` 3워드 헤더 ABI를 동일하게 해석해야 한다.
2. `Utf8Span`/`Utf8Buf`의 필드 오프셋/정렬은 lane과 무관하게 동일해야 한다.
3. `~`의 `cell commit` / `abi pack` 규칙은 JIT/AOT에서 동일하게 검증되어야 한다.

---

## 7. 구현 체크리스트

1. `~` lowering 경로에 heap materialization이 없는지 검증한다.
2. local/field/optional 저장이 즉시 ABI pack으로 내려가지 않는지 검증한다.
3. `String` drop 경로가 static backing에서 free를 호출하지 않는지 검증한다.
4. `text -> String` 암시 변환이 발생하지 않는지 타입체커에서 검증한다.
5. C ABI 함수에서 `String` 직접 전달을 거부하는지 검증한다.
