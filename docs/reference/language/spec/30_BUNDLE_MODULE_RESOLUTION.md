## 18. Bundle/Module Resolution Rules (Normative)

문서 버전: `v0.1`
상태: `Normative`

이 문서는 Parus의 bundle/module 이름 해석 규칙과 LEI 연동 규칙을 고정한다.

## 18.1 목적

1. `file -> module -> bundle -> project` 계층에서 이름 해석을 결정적으로 고정한다.
2. bundle prepass, `export` 가시성, `module.imports`/`bundle.deps` 검증 규칙을 단일 정본으로 제공한다.
3. 컴파일러, LSP, LEI build graph가 동일 규칙을 사용하게 한다.

## 18.2 고정 규칙 (Non-negotiable)

1. bundle 내 심볼 수집은 모든 `.pr` 파일 대상으로 수행한다.
2. 수집 대상 심볼은 `def/var/struct/acts` 전체다.
3. 같은 폴더 자동 공유는 `export` 심볼만 허용한다.
4. 다른 폴더 참조는 반드시 `import <head> as <alias>;`를 사용한다.
4-a. `import "Header.h" as <alias>;`는 C 헤더 import 전용 구문이며 module head/deps gate 검증 대상이 아니다.
5. `import foo::bar`와 `import ::foo::bar`를 모두 허용하며 내부 canonical은 `foo::bar`다.
6. `import <head>`의 `<head>`는 현재 module `imports`에 존재해야 한다.
7. cross-bundle import는 대상 bundle이 현재 bundle `deps`에도 존재해야 한다.
8. 이름 해석은 `오버로딩 해소 우선`, 이후 우선순위 적용으로 고정한다.
9. 동일 우선순위에 동일 시그니처 후보가 2개 이상 남으면 `ambiguous` 오류다.
10. `nest`는 네임스페이스 태깅 전용이며 module/import 해석에는 관여하지 않는다.
11. `import`는 alias 도입 문법이며 include semantics를 허용하지 않는다.
12. 재export는 v0에서 비지원이다.

## 18.3 모듈 head 정본

1. `module.head` 사용자 입력은 금지다.
2. module head는 `module.sources` 경로에서 자동 계산한다.
3. 계산 규칙은 source의 parent directory에서 첫 `src` 세그먼트 제거 후 `::` 결합이다.
4. 계산 결과가 비면 bundle 이름으로 fallback한다.
5. 같은 폴더 파일은 동일 module head를 공유한다.

예시:

1. `/foo/app/src/main.pr` -> `app`
2. `/foo/app/src/helper.pr` -> `app`
3. `/foo/app/src/net/http.pr` -> `app::net`
4. `/foo/math/src/add.pr` -> `math`

추가 규칙:

1. 하나의 module에 여러 source가 있을 때 자동 계산된 head가 다르면 오류다.
2. 서로 다른 bundle이 동일 top-head를 소유하면 오류다.

## 18.4 가시성 규칙

1. 같은 파일: `export` 여부와 무관하게 참조 가능.
2. 같은 폴더 다른 파일: `export` 선언만 참조 가능.
3. 다른 폴더/다른 bundle: `export` + `import` + `module.imports` 검증을 충족해야 참조 가능.
4. cross-bundle인 경우 대상 bundle `deps` 검증도 추가로 충족해야 참조 가능.
5. `export` 없는 선언은 파일 내부 전용이다.
6. 같은 폴더에서 동일 이름/동일 시그니처 `export` 충돌은 컴파일 오류다.

## 18.5 이름 해석 순서

### 18.5.1 1차: 오버로딩 후보 축소

1. 호출 시그니처 기준으로 오버로딩 후보를 먼저 축소한다.
2. 시그니처가 불일치하는 후보는 즉시 제거한다.

### 18.5.2 2차: 우선순위 적용

남은 후보에 대해 다음 우선순위를 적용한다.

1. 현재 파일
2. 같은 폴더 auto-share(`export`만)
3. 명시 `import` alias
4. `module.imports` + `bundle.deps`를 통과한 외부 export

### 18.5.3 3차: 모호성 처리

1. 동일 우선순위에서 동일 시그니처 후보가 2개 이상이면 `ambiguous` 오류.
2. 우선순위가 낮은 후보는 높은 후보가 존재하면 선택되지 않는다.

## 18.6 bundle prepass와 인덱스

1. bundle prepass는 선언을 전수 수집한 뒤 공개면/내부면 인덱스를 분리 보관한다.
2. LEI build/check 경로는 동일 인덱스 규칙을 사용해야 한다.
3. module import gate 정보는 LEI `module.imports`를 정본으로 사용한다.
4. `module.imports`는 `foo`, `foo::bar`, `::foo::bar`를 허용하되 내부 gate는 top-head(`foo`)로 canonicalize한다.
5. cross-bundle link/build order 정보는 LEI `bundle.deps`를 정본으로 사용한다.

## 18.7 `nest`와 `import` 경계

1. `nest`는 심볼 경로 태깅만 담당한다.
2. module head 계산, `deps` 검증, `import` 대상 판정에는 `nest`를 사용하지 않는다.
3. `import`는 파일 스코프에서만 허용한다.

## 18.7-a Typed Sidecar Closure와 Visibility

1. imported typed sidecar dependency closure는 same-bundle helper `free function/struct/enum/class`를 내부 materialization용으로 적재할 수 있다.
2. closure-private helper declaration은 consumer source에서 lexical import 없이 직접 참조될 수 없다.
3. closure-private helper declaration은 imported template table 내부 노드로만 존재하고, ordinary export/import visibility 규칙을 우회하지 않는다.
4. helper `actor` dependency closure와 global private state / class-static mutable state dependency closure는 v0 범위 밖이다.

## 18.7-b Proto-Target Qualified Path Exception

1. ordinary value/type path는 계속 lexical import 규칙을 따른다.
2. 예외는 `proto target` 위치뿐이다.
3. `proto target` 위치에는 아래가 포함된다.
3-a. generic constraint RHS
3-b. proto inheritance target
3-c. class/struct/enum의 proto attachment target
3-d. acts header의 generic constraint RHS
4. 위 위치에서는 qualified public proto path를 lexical import 없이 직접 해석할 수 있다.
5. 이 예외는 hidden/internal proto나 ordinary expression/type symbol까지 넓어지지 않는다.

## 18.8 진단 코드 계약

필수 진단 코드는 다음을 사용한다.

1. `E_IMPORT_DEP_NOT_DECLARED`
2. `E_SYMBOL_NOT_EXPORTED_FILE_SCOPE`
3. `E_SYMBOL_NOT_EXPORTED_BUNDLE_SCOPE`
4. `E_SYMBOL_AMBIGUOUS_OVERLOAD`
5. `E_EXPORT_COLLISION_SAME_FOLDER`
6. `W_NEST_NOT_USED_FOR_MODULE_RESOLUTION`

## 18.9 Canonical 예시

`/foo/app/src/helper.pr`

```parus
export def twice(x: i32) -> i32 {
  return x + x;
}
```

`/foo/app/src/main.pr`

```parus
import math as m;

def main() -> i32 {
  let v: i32 = m::add(a: 20i32, b: 1i32);
  return twice(v); // 같은 폴더 export auto-share
}
```

`/foo/math/src/add.pr`

```parus
export def add(a: i32, b: i32) -> i32 {
  return a + b;
}
```

## 18.10 구현 전 수용 검증 시나리오

1. 같은 파일 non-export 참조 성공.
2. 같은 폴더 다른 파일 non-export 참조 실패, export 참조 성공.
3. 다른 폴더 export 참조 성공, non-export 참조 실패.
4. `deps` 없는 import head 실패.
5. 오버로딩 해소 후 동순위 동일 시그니처 충돌 시 ambiguous 실패.
6. `nest` 유무가 module/import 판정 결과에 영향을 주지 않음.

## 18.11 결정적 초기화(ctor 배열 미사용)

1. Parus는 C++ 스타일 전역 ctor 배열(`@llvm.global_ctors`)을 사용하지 않는다.
2. 각 컴파일 모듈은 `__parus_module_init__<hash>` 형태의 module init 함수를 가진다.
3. bundle 모드에서는 `parus_bundle_init__<bundle>` API 심볼을 사용한다.
4. bundle 리더(정렬된 `bundle_sources_norm`의 첫 source)만 `parus_bundle_init__<bundle>`를 정의한다.
5. 비리더 모듈은 동일 bundle init 심볼을 `declare`만 한다.
6. bundle init 호출 순서는 `bundle_sources_norm` 정렬 순서로 고정한다.
7. 각 module init 내부 초기화 순서는 파일 내 선언 순서로 고정하고, 정리는 역순 규칙을 따른다.
8. 실행 엔트리 래퍼는 사용자 `main` 호출 전에 bundle init(또는 non-bundle module init)을 선호출한다.

## 18.12 C Header Import v2 (Normative)

1. C 헤더 import 문법은 `import "Header.h" as alias;`만 허용한다(`as` 필수).
2. include 탐색 순서는 고정한다.
2-a. 현재 소스 파일 디렉터리
2-b. `-I`로 전달된 경로(입력 순서 유지)
2-c. `-isystem`로 전달된 경로(입력 순서 유지)
2-d. libclang 기본 시스템 include 경로
2-e. C import 전처리 옵션은 다음을 지원한다: `-D`, `-U`, `-include`, `-imacros`.
2-f. 전처리 옵션은 Parus 소스 파싱에는 영향을 주지 않고 C header import 단계에만 적용한다.
3. C variadic 함수 호출은 제한적으로 허용한다(`cimport` + handwritten `extern "C"` 선언 공통).
3-a. call site는 `manual[abi]`가 필요하다.
3-b. fixed parameter 구간은 일반 호출과 동일 타입검사를 적용한다.
3-c. variadic 구간은 ABI-safe scalar/raw pointer/`core::ext::CStr`/plain string literal만 허용한다.
3-d. variadic 구간에서는 `text`, borrow, escape, optional, aggregate(`layout(c)` struct 포함), direct enum, `null` literal을 허용하지 않는다.
3-e. default promotion(`f32 -> f64`, small int/bool/char -> int 계열)은 lowering에서 적용한다.
3-f. variadic 구간의 unsuffixed integer literal은 v1에서 `C int(i32)` 컨텍스트로 확정한다.
4. C ABI 호출은 positional-only다(import C / extern "C" 공통).
4-a. labeled/named-group 인자는 허용하지 않는다.
4-b. format string literal(`F"""..."""`)은 C ABI 인자에서 금지한다.
4-c. `bare $"..."` 형식은 문법에서 제거되었으므로 C ABI 경계에서도 허용되지 않는다.
4-d. C `char*`(및 `const char*`) 기대 슬롯에서는 plain string literal(`"..."`)을 자동 허용한다. 이 경로는 `core::ext::c_char` 계열 포인터 슬롯을 기준으로 적용한다.
4-e. `null` literal은 typed pointer slot의 call-arg/return/assignment 경계에서만 허용한다. `T? -> *const/*mut T` 자동 변환은 허용하지 않는다.
5. C union import는 2차 규칙을 따른다.
5-a. union field dot 접근은 `manual[...]` 내부에서만 허용한다.
5-b. read는 `manual[get]` 또는 `manual[set]`가 필요하다.
5-c. write는 `manual[set]`가 필요하다.
6. C macro import v2.2 규칙:
6-a. object-like macro만 상수 심볼(`alias::MACRO`)로 승격한다.
6-b. 상수 승격 대상은 literal + 엄격 상수식 subset으로 제한한다. 허용 연산은 괄호, unary(`+/-/~`), binary(산술/비트/비교/논리)와 object-like macro 상수 참조다.
6-c. function-like macro는 strict subset에 한해 호출 가능한 함수 심볼(`alias::MACRO`)로 승격할 수 있다.
6-d. strict subset은 `CALLEE(arg...)` 단일 호출식이며 각 인자는 macro parameter의 직접 전달 또는 단순 cast 전달(`(T)param`)이어야 한다.
6-e. direct identity forwarding은 `DirectAlias`로 승격하고 shim 없이 기존 C 함수 심볼을 재사용한다.
6-f. 인자 재배열/단순 cast forwarding은 `IRWrapperCall` recipe로 승격하며 wrapper body는 Parus IR/OIR에서 직접 합성한다.
6-g. `##`, `#`, statement macro(`({ ... })`), compiler extension 의존 form, variadic function-like macro는 승격 대상에서 제외하며 경고 후 건너뛴다.
6-h. function-like macro chain 승격은 고정점 해석으로 수행하며 순환 체인은 skip+경고로 처리한다.
7. 익명 선언/bitfield v2.3 규칙:
7-a. 이름 없는 `struct/union/enum`은 synthetic internal 이름(`__anon_*`)으로 수집한다.
7-b. typedef가 익명 선언을 가리키는 경우 동일 선언 identity를 공유하도록 연결한다.
7-c. bitfield는 importer가 `offset/width/signedness/storage-unit`를 손실 없이 계산할 수 있는 경우만 지원한다.
7-d. packed/exotic/ambiguous bitfield layout은 partial로 남기지 않고 import hard error로 중단한다.
7-e. bitfield 접근 lowering은 field metadata를 바탕으로 load/store + mask/shift/sign-extend/merge로 직접 수행한다.
7-f. anonymous flatten으로 유입된 union-origin field는 기존 union 정책과 동일하게 `manual[...]` gate를 적용한다.
7-g. anonymous flatten 결과 필드명이 충돌하면 c-import를 hard error로 중단한다.
8. ABI 속성 반영(v2.3):
8-a. 함수 calling convention 메타를 importer payload에 보존하고 extern decl/call lowering에 실제 반영한다.
8-b. record layout은 effective `size/align` 기준으로 유지하며 packed/aligned 속성은 payload 메타로 보존한다.
9. C global/TLS import(v1.0 freeze):
9-a. external linkage `VarDecl`은 `alias::NAME` external var 심볼로 import한다.
9-b. importer payload는 `const/volatile/restrict` qualifier와 `tls_kind(none/dynamic/static)`를 보존한다.
9-c. `_Thread_local`/`__thread` import 심볼은 LLVM lowering에서 `thread_local` external global로 반영한다.
9-d. TLS 초기화/소멸 책임은 외부 C 런타임/로더에 있으며 Parus는 TLS ctor/dtor 실행 경로를 추가하지 않는다.
9-e. imported C global/TLS 접근은 thread-safe를 보장하지 않으며 effect는 보수적으로 유지한다.
10. 미지원/부분지원 처리 정책:
10-a. `##`, `#`, statement macro, compiler-extension macro, variadic function-like macro는 skip+warning으로 처리한다.
10-b. silent skip은 금지하며 skip 항목은 reason code와 함께 진단에 노출한다.
11. C ABI 호출 인자/타입은 `core::ext::c_*` 및 `core::ext::vaList` 표면을 사용한다.
11-a. non-core 번들에서는 core auto injection이 활성화된 경우 `core::ext` 경로를 import 없이 사용할 수 있다.
11-b. `core::ext::vaList`는 opaque 타입이며 C ABI 함수 시그니처 파라미터 위치에서만 허용한다.
11-c. `-fno-core`(또는 `PARUS_NO_CORE=1`)에서 c-import를 사용하면 하드 에러다.
11-d. `text`는 C ABI 타입으로 허용하지 않으며, 문자열 경계는 `*const core::ext::c_char`와 명시 변환으로 처리한다.
11-e. `text -> *const core::ext::c_char` hidden lowering bridge는 허용하지 않는다. 타입체커가 거부한 C ABI 경계 변환을 lowering이 복구해서는 안 된다.

## 18.13 Imported Generic Metadata Resolution (Normative)

1. source-level path lookup과 imported generic metadata lookup은 구분한다.
2. 사용자가 소스에 직접 쓴 path는 본 문서의 ordinary import/head/deps 규칙을 그대로 따른다.
3. imported generic free function/proto/acts/class metadata 안의 proto constraint target은 lexical import로 다시 찾지 않는다.
4. imported metadata는 canonical proto identity를 사용한다.
4-a. producer bundle
4-b. proto module head
4-c. proto public path
5. consumer는 이 canonical identity를 기준으로 proto stub 또는 imported proto symbol을 직접 resolve한다.
6. 따라서 소비자는 라이브러리 내부 constraint vocabulary를 다시 import하지 않아도 된다.
7. class/proto/acts whole-body import도 같은 canonical identity 규칙을 사용한다.
8. 이 완화는 imported metadata 경로에만 적용되며, source-level explicit import requirement를 제거하지 않는다.
9. imported generic root는 exported root declaration만이 아니라 typed dependency closure를 함께 반입할 수 있다.
10. 이번 라운드의 closure 허용 범위는 아래로 제한한다.
10-a. same-bundle free-function dependency
10-b. same-bundle helper `struct/enum` type-body dependency
11. closure-private helper type/function은 materialization 전용 내부 의존성으로만 사용된다.
12. consumer source는 closure-private helper를 lexical import 없이 직접 참조할 수 없다.
