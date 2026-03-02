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
