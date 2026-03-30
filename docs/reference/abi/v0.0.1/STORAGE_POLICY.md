# Parus Storage Policy Supplement (`v0.0.1`)

문서 상태: `Normative Supplement`

이 문서는 `docs/reference/abi/v0.0.1/ABI.md`의 저장소/수명 요약을 상세화한다.
특히 스택/힙/정적 저장소와 `~` 규칙을 고정한다.
이 문서의 `~` 규칙은 v0 core/freestanding semantics의 정본이며, alloc-backed owner container는 범위 밖으로 둔다.

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
   - non-`layout(c)` enum payload cell
3. ABI handle pack
   - return boundary
   - argument boundary

금지 경로:

1. heap box로의 자동 승격
2. 런타임 allocator 호출을 동반하는 `~` 생성

용어 고정:

1. `cell commit`
   - `~T`를 local/field/array-element/optional/static place에 저장
   - `core::mem::replace` / `core::mem::swap`로 existing `~T` cell을 교체/교환하는 것도 cell mutation으로 본다
2. `abi pack`
   - `~T`를 call/return ABI 경계에서 3word handle로 구성

`cell commit`은 곧바로 `abi pack`을 의미하지 않는다.

---

## 4. `~` 저장/ABI 규칙으로 인한 제약

다음 패턴은 v0에서 제한되거나 금지될 수 있다.

1. async/suspend 경계를 넘는 `~` 유지
2. unsized view / generalized container에 `~` 장기 저장
3. direct field/index projection으로 plain `~T` subplace를 임의 추출

권장 해결:

1. 장수명 owner cell이 필요하면 local/field/optional/static place를 사용한다.
2. sized array container가 필요하면 `(~T)[N]` 또는 `((~T)?)[N]`를 사용한다.
   - public surface로는 compiler-owned array family method를 사용한다.
   - `arr.swap(i, j)`, `arr.replace(i, next)`, `arr.take(i)`, `arr.put(i, next)`
3. storage-safe named aggregate가 필요하면 그 내부 field graph를 `~T`, `(~T)?`, sized owner-array, 다시 storage-safe named aggregate로만 구성한다.
4. owner payload를 상태기계에 실어야 하면 non-`layout(c)` enum payload에 `~T` / `(~T)?` / sized owner-array를 사용한다.
   - ordinary non-owner payload는 기존처럼 계속 허용된다.
5. one-shot extraction은 `(~T)?`, `((~T)?)[N]`, projected optional owner path + consume-binding, 또는 `core::mem::take(place)`를 사용한다.
6. owner payload를 `switch`로 꺼낼 때는 direct place consume를 하지 말고, 먼저 local move 또는 `core::mem::replace(..., Empty)`로 enum 값을 분리한 뒤 consuming switch on value를 사용한다.
7. initialized plain `~T` place를 교체해야 하면 `core::mem::replace` / `core::mem::swap`를 사용한다.
8. raw-pointer/borrow dereference `*p` / `*bp`는 ordinary pointer/manual path일 뿐 owner-cell family가 아니다.
   - `~(*p)`, `~(*bp)` 같은 owner extraction은 허용하지 않는다.
   - owner-typed pointee에 대한 raw read/write도 ordinary pointer path로 처리하지 않고 명시적으로 거부한다.
9. dynamic alloc-backed owner container(`Vec<(~T)?>`, `Box<~T>` 등)는 이 코어 저장 정책의 일부가 아니다.

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
2. local/field/array-element/optional 저장이 즉시 ABI pack으로 내려가지 않는지 검증한다.
3. enum payload owner storage와 `core::mem::take`가 eager ABI pack으로 내려가지 않는지 검증한다.
4. `String` drop 경로가 static backing에서 free를 호출하지 않는지 검증한다.
5. `text -> String` 암시 변환이 발생하지 않는지 타입체커에서 검증한다.
6. C ABI 함수에서 `String` 직접 전달을 거부하는지 검증한다.
