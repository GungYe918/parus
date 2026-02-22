# 05. Build Model

## 단위 모델

```text
file -> module -> bundle -> project
```

1. file: 하나의 `.lei` 또는 `.pr` 파일
2. module: 파일들이 구성하는 논리 심볼 경계
3. bundle: 배포/컴파일 단위
4. project: 여러 bundle 합성 단위

## 파일 관행

1. 프로젝트 루트는 `config.lei`를 권장한다.
2. 폴더 단위 파일은 `<folder>.lei`를 권장한다.
3. `lei.lei` 같은 대체 파일명은 허용하되 권장하지 않는다.

## canonical plan 합성 흐름

1. 각 bundle은 자신의 `config.lei` 또는 `<folder>.lei`에서 canonical bundle plan을 export한다.
2. 상위 프로젝트는 하위 bundle plan을 import해 `&`로 명시 합성한다.
3. task/codegen plan은 빌드 실행 단계 노드로 import/합성한다.
4. 합성 결과가 상위 프로젝트의 canonical plan이 된다.

## 예제 1: app bundle (`/app/app.lei`)

```lei
proto myBundleProto {
  name: string;
  kind: string = "lib";
  sources: [string];
  deps: [string] = [];
};

export plan app_bundle = bundle & myBundleProto & {
  name = "app";
  kind = "bin";
  sources = ["src/main.pr", "src/cli.pr"];
  deps = ["json"];
};
```

## 예제 2: json bundle (`/json/json.lei`)

```lei
export plan json_bundle = bundle & {
  name = "json";
  kind = "lib";
  sources = ["src/json.pr"];
  deps = [];
};
```

## 예제 3: task/codegen (`/tools/tools.lei`)

```lei
export plan lint = task & {
  name = "lint";
  run = ["parusc", "--check", "src/main.pr"];
  inputs = ["src/main.pr"];
  outputs = [];
  always_run = true;
};

export plan gen_user = codegen & {
  name = "gen_user";
  tool = ["protoc"];
  inputs = ["proto/user.proto"];
  outputs = ["gen/user.pb.pr"];
  args = ["--parus_out=gen", "proto/user.proto"];
};
```

## 예제 4: 프로젝트 루트 (`/config.lei`)

```lei
import app from "./app/app.lei";
import json from "./json/json.lei";
import tools from "./tools/tools.lei";

proto ProjectMeta {
  name: string;
  version: string;
};

plan project_graph {
  project = ProjectMeta & {
    name = "demo";
    version = "0.1.0";
  };
  bundles = [json::json_bundle, app::app_bundle];
  tasks = [tools::lint];
  codegens = [tools::gen_user];
};

plan merged_master = master & {
  build = project_graph;
};

plan master = merged_master;
```

## 배열 접근 예제

```lei
let first_name = project_graph.bundles[0].name;
```

## 단일 필드 변경 예제

```lei
plan project_graph2 = project_graph & {
  project = project_graph.project & {
    name = "demo-renamed";
  };
};
```

## LEI 언어 vs Parus 통합 프로파일

언어 규칙:

1. LEI 자체에는 `master` 개념이 없다.
2. LEI는 `proto`/`plan`/`export plan`/`import alias`/`&`를 정의한다.

Parus 통합 프로파일 규칙:

1. `config.lei`를 엔트리 파일로 특별취급한다.
2. `config.lei`의 `plan master` 또는 CLI 지정 plan을 엔트리로 사용한다.
3. `master` export는 정책상 금지한다.
4. 하위 bundle의 `master` import/재export는 정책상 금지한다.
5. `bundle`, `master`, `task`, `codegen`은 Parus가 주입한 빌트인 plan 값으로 해석한다.
6. `project`는 특수 plan이 아니다.
7. 빌트인 plan 주입 계약은 `12_BUILTIN_PLAN_SCHEMA_INJECTION.md`를 따른다.

위 규칙은 LEI 언어 문법이 아니라 Parus 빌드 시스템 정책이다.
