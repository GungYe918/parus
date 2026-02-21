# 09. Intrinsics

## 개요

LEI v0의 intrinsic은 별도 import 없이 자동 제공된다.

1. `import intrinsic ...` 문법은 사용하지 않는다.
2. intrinsic은 빌드 DSL 결정성을 유지하는 최소 집합만 제공한다.

## 현재 제공 목록 (v0)

### `base`

`base`는 LEI 평가 시작 시 자동으로 주입되는 read-only 객체다.

```lei
{
  version: "0.1",
  backend: "ninja",
  defaults: {
    profile: "debug",
    opt: 0,
  },
}
```

의도:

1. 빌드 기본값을 합성하는 공통 시작점
2. 프로젝트별 오버레이(`?=`) 적용 기반

예시:

```lei
let cfg = base ?= {
  defaults: { profile: "release" },
};
```

## 안정성 규칙

1. intrinsic 값은 순수 데이터여야 한다.
2. 시간/랜덤/환경 종속 intrinsic은 금지한다.
3. 사용자 코드가 같은 이름을 선언하면 해당 스코프에서 shadowing될 수 있다.

## 향후 확장 규칙

1. intrinsic 추가는 문서 업데이트와 함께만 허용한다.
2. 새 intrinsic 이름은 Starlark 관례와의 정합성을 우선 검토한다.
3. 추가 intrinsic은 보안/결정성 검증을 통과해야 한다.

## 구현 API

1. `lei::eval::BuiltinRegistry`
   1. `register_value(name, factory)`로 빌트인 값 등록
   2. `register_native_function(name, callback)`로 C++ 엔진 빌트인 함수 등록
2. 기본 intrinsic 세트는 `make_default_builtin_registry()`로 생성한다.
