# LEI Usage (Module-First)

## 권장 트리

```text
project/
  config.lei
  math/
    math.lei
    src/
      add.pr
  app/
    app.lei
    src/
      main.pr
      helper.pr
```

## bundle 파일 예제

### `/math/math.lei`

```lei
export plan math_module = module & {
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

### `/app/app.lei`

```lei
export plan app_module = module & {
  sources = ["app/src/main.pr", "app/src/helper.pr"];
  imports = ["::math"];
};

export plan app_bundle = bundle & {
  name = "app";
  kind = "bin";
  modules = [app_module];
  deps = ["math"];
};
```

## 루트 예제

### `/config.lei`

```lei
import math from "./math/math.lei";
import app from "./app/app.lei";

plan master = master & {
  project = {
    name: "sample-project",
    version: "0.1.0",
  };
  bundles = [math::math_bundle, app::app_bundle];
  tasks = [];
  codegens = [];
};
```

## 단일 inline bundle 예외

bundle이 최종 1개인 경우에만 `config.lei` inline 선언을 허용한다.

```lei
plan app_module = module & {
  sources = ["src/main.pr"];
  imports = [];
};

plan app_bundle = bundle & {
  name = "app";
  kind = "bin";
  modules = [app_module];
  deps = [];
};

plan master = master & {
  project = { name: "single", version: "0.1.0" };
  bundles = [app_bundle];
  tasks = [];
  codegens = [];
};
```

## 실행

```bash
parus check
parus build
parus graph --format dot
```

## 규칙 요약

1. `bundle.sources`는 제거되었고 `bundle.modules`를 사용한다.
2. module import gate는 `module.imports`다(`foo`, `foo::bar`, `::foo::bar` 입력을 내부에서 top-head로 canonicalize).
3. build/link 순서는 `bundle.deps`다.
4. 동일 module auto-share는 `export`만 허용한다.
5. 다른 module 참조는 `import <head> as <alias>;`가 필요하고, `import ::foo::bar as x;`도 허용한다.
