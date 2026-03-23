# Parus Core Text Plan

문서 상태: `active`

이 문서는 `core::text`의 현재 구현 상태와, 앞으로 어떤 언어 표면 위에서 확장할지를 고정한다.

## 현재 결론

1. `core::text`의 최소 표면은 이제 실제로 구현되었다.
2. 구현은 builtin 함수가 아니라 언어 코어 확장 위에 올라간다.
3. 현재 열린 기반 표면은 `*` deref, builtin view field(`.len`, `.data`), `text{ data, len }` constructor다.
4. `text`는 계속 freestanding non-owning UTF-8 view다.
5. `String`은 이후 std에서 도입하는 소유 타입으로 남기며, 이번 설계와 권한이 겹치지 않는다.
6. `text` 길이는 sentinel scan이 아니라 metadata다.

## 이번 라운드에서 실제로 열린 표면

### 1. Deref

`*expr`가 expression-level deref로 열린다.

1. `&T`, `&mut T`에 대해서는 ordinary deref
2. raw `*const/*mut`에 대해서는 기존 `manual[get]` / `manual[set]` 정책 아래 deref
3. `text` acts의 `self: &text`는 `*self`로 읽을 수 있다

### 2. Builtin view field

다음 transparent field가 열린다.

1. `text.len -> usize`
2. `text.data -> *const u8`
3. `T[].len -> usize`
4. `T[].data -> *const T`
5. `T[N].len -> usize`

이 표면은 함수 호출이 아니라 view metadata projection이다.

### 3. Text view constructor

`text{ data: ..., len: ... }`를 통해 zero-copy `text` view를 직접 구성할 수 있다.

제약:

1. `manual[abi]` 안에서만 허용
2. `data`는 byte-compatible pointer여야 함
3. `len`은 `usize` assignable이어야 함
4. hidden allocation, UTF-8 validation, implicit ownership promotion 없음

## 현재 구현된 `core::text`

현재 `sysroot/core/text/text.pr`의 public surface:

1. `text.is_empty(self) -> bool`
2. `text.len_bytes(self) -> usize`
3. `text.as_ptr(self) -> *const u8`

구현은 다음 형태다.

1. `(*self).len == 0usize`
2. `(*self).len`
3. `(*self).data`

즉 길이 metadata는 언어 표면에서 직접 읽고, library acts는 그 위를 얇게 감싼다.

## ABI / lowering invariants

이번 변경으로도 `text` ABI는 바뀌지 않는다.

1. lowering은 계속 `{ptr, len}` header를 사용한다
2. `*borrow` on `text`는 header copy로 내려간다
3. `text{ data, len }`는 zero-copy header build로 내려간다
4. layout/type이 불명확하면 blob fallback이 아니라 lowering error로 fail-closed 한다

## `core::ext::CStr`와의 관계

이번 변경은 `CStr`의 기존 ABI를 바꾸지 않는다.

1. `CStr -> *const c_char` C ABI coercion은 그대로 유지
2. `CStr`는 이제 `to_text()` / `core::ext::to_text(c)` 브리지를 제공한다
3. `to_text()`는 `text{ data: core::ext::as_ptr(c), len: core::ext::len(c) }` 형태를 기준으로 잡는다
4. `len_with_nul()`이 아니라 `len()`을 사용해야 한다

## 비채택한 방향

이번 라운드에서는 `text` 길이를 raw pointer scan으로 다시 계산하는 표면은 도입하지 않는다.

이유:

1. 현재 `text`는 `{ptr,len}` view이며, pointer-scan은 이 의미와 충돌한다
2. `text{ data, len }`로 만든 일반 view는 NUL-terminated 보장을 갖지 않는다
3. O(1) metadata access를 O(n) sentinel scan으로 바꾸는 것은 후퇴다
4. sentinel scan은 `text`가 아니라 `CStr` 계층의 책임으로 남긴다

## 이번 라운드에서 여전히 하지 않는 것

다음은 아직 열지 않는다.

1. text iteration
2. text indexing / slicing
3. `bytes()` adapter
4. `starts_with`, `ends_with`
5. prefix/suffix helper
6. `String`
7. `core::slice`와 직접 연결되는 generic `u8[]` view bridge

`core::slice` 자체는 이미 별도 모듈로 들어왔지만, `text`는 아직 generic `u8[]` view constructor/bridge가 없다. 그래서 `text.starts_with`, `text.ends_with`, `text.bytes()` 같은 2차 helper는 계속 보류한다.

즉 현재 단계는 iterator와 slice foundation은 열린 상태이고, 이후 `core::text::bytes()`를 올릴 수 있는 byte-view bridge가 다음 선행 조건이다.

## Recommended Next Order

1. `core::text` smoke/runtime/LSP 하드닝 유지
2. `core::mem`
3. `core::hint`
4. `core::range`
5. `core::iter`
6. 필요 시 `core::slice`
