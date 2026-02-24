# 05. Build Model

## 단위 모델

```text
file -> module -> bundle -> project
```

1. `file`: 하나의 `.pr` 또는 `.lei` 파일
2. `module`: 하나의 폴더가 가지는 논리 심볼 경계
3. `bundle`: 여러 module을 묶는 컴파일/링크 단위
4. `project`: 여러 bundle을 묶는 최상위 단위

## 모델 고정 규칙 (v0.4)

1. module은 `export plan module = module & { ... };` 형태로 선언한다.
2. bundle은 `export plan <name> = bundle & { modules = [...], deps = [...] };` 형태로 선언한다.
3. `bundle.sources`는 제거되었다. 반드시 `bundle.modules`를 사용한다.
4. module별 소스는 `module.sources`에 선언한다.
5. module별 import gate는 `module.imports`로 선언한다.
6. bundle 간 빌드/링크 순서는 `bundle.deps`로 선언한다.
7. `config.lei`에서 inline bundle 선언은 최종 bundle이 1개일 때만 허용한다.
8. 최종 bundle이 2개 이상이면 각 bundle 폴더의 `<bundle>.lei`에서 선언해야 한다.

## Parus interop 계약

1. 같은 module(같은 폴더) 내부 auto-share는 `export` 선언만 허용한다.
2. 다른 module 참조는 `import <head> as <alias>;`가 필요하다.
3. `import <head>`의 `<head>`는 현재 module의 `module.imports`에 있어야 한다.
4. cross-bundle import가 발생하면 대상 bundle은 `bundle.deps`에도 있어야 한다.
5. `nest`는 네임스페이스 태깅 전용이며 module/import head 계산에 관여하지 않는다.

## 예제 1: bundle 선언 (`/app/app.lei`)

```lei
export plan app_main_module = module & {
  head = "app";
  sources = ["app/src/main.pr", "app/src/helper.pr"];
  imports = ["math"];
};

export plan app_bundle = bundle & {
  name = "app";
  kind = "bin";
  modules = [app_main_module];
  deps = ["math"];
};
```

## 예제 2: 의존 bundle (`/math/math.lei`)

```lei
export plan math_module = module & {
  head = "math";
  sources = ["math/src/add.pr"];
  imports = [];
};

export plan math_bundle = bundle & {
  name = "math";
  kind = "lib";
  modules = [math_module];
  deps = [];
};
```

## 예제 3: 프로젝트 루트 (`/config.lei`)

```lei
import app from "./app/app.lei";
import math from "./math/math.lei";

plan master = master & {
  project = {
    name: "demo",
    version: "0.1.0",
  };
  bundles = [math::math_bundle, app::app_bundle];
  tasks = [];
  codegens = [];
};
```

## 정책 경계

1. LEI Core는 문법/평가(`proto`, `plan`, `import`, `&`)를 정의한다.
2. LEI Engine Policy는 엔트리/그래프/reserved plan(`bundle`, `module`, `master`, `task`, `codegen`)을 정의한다.
3. `master` export 금지는 엔진 상시 정책이다.
