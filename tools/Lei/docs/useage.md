# LEI Useage (Build System Example)

이 문서는 LEI Build System 사용 예시를 보여준다. 아래 예시는 LEI 엔진 기본 정책을 적용한 시나리오다.

## 1) 예시 프로젝트 트리

```text
sample-project/
  config.lei
  app/
    app.lei
    src/main.pr
    src/cli.pr
  json/
    json.lei
    src/json.pr
  core/
    core.lei
    src/lib.pr
  tools/
    tools.lei
    proto/user.proto
```

## 2) bundle별 소스 등록 (`proto` + built-in `bundle`)

### `/json/json.lei`

```lei
proto myBundleProto {
  name: string;
  kind: string = "lib";
  sources: [string];
  deps: [string] = [];
};

export plan json_bundle = bundle & myBundleProto & {
  name = "json";
  sources = ["src/json.pr"];
};
```

### `/core/core.lei`

```lei
export plan core_bundle = bundle & {
  name = "core";
  kind = "lib";
  sources = ["src/lib.pr"];
  deps = ["json"];
};
```

### `/app/app.lei`

```lei
export plan app_bundle = bundle & {
  name = "app";
  kind = "bin";
  sources = ["src/main.pr", "src/cli.pr"];
  deps = ["core", "json"];
};
```

## 3) task/codegen plan 등록

### `/tools/tools.lei`

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

## 4) LEI 시스템 사용 시나리오

### `/config.lei`

```lei
import app from "./app/app.lei";
import core from "./core/core.lei";
import json from "./json/json.lei";
import tools from "./tools/tools.lei";

proto ProjectMeta {
  name: string;
  version: string;
};

plan workspace {
  project = ProjectMeta & {
    name = "sample-project";
    version = "0.1.0";
  };
  bundles = [json::json_bundle, core::core_bundle, app::app_bundle];
  tasks = [tools::lint];
  codegens = [tools::gen_user];
};

plan master = master & {
  project = workspace.project;
  bundles = workspace.bundles;
  tasks = workspace.tasks;
  codegens = workspace.codegens;
};
```

엔진 정책 해석:

1. 엔트리 파일은 `config.lei`다.
2. 기본 엔트리 plan은 `master`다.
3. CLI `--plan <name>`으로 엔트리 plan override가 가능하다.
4. canonical graph source는 엔트리 plan 루트(`project/bundles/tasks/codegens`)다.
5. `master`는 export하지 않는다.
6. 그래프 조회는 `lei-build config.lei --view_graph [--format json|text|dot]`로 수행한다.

### 금지 예시

```lei
export plan master = master & {
  project = workspace.project;
  bundles = workspace.bundles;
  tasks = workspace.tasks;
  codegens = workspace.codegens;
};
```

위 예시는 문법적으로 파싱되더라도, LEI 엔진 정책 위반으로 처리한다.

## 5) 배열/네임스페이스 접근 예시

```lei
let first_bundle_name = workspace.bundles[0].name;
let app_name = app::app_bundle.name;
let first_task = workspace.tasks[0].name;
```

## 6) 단일 필드 변경 패턴

```lei
plan workspace2 = workspace & {
  project = workspace.project & {
    name = "sample-project-renamed";
  };
};
```

`foo.name & bar.name`은 값 변경이 아니라 동일성 제약이므로, 업데이트 용도로는 위처럼 객체 patch를 사용한다.

## 7) 합성 실패 예시 (`&` 충돌)

```lei
plan a {
  profile = "debug";
};

plan b {
  profile = "release";
};

plan bad_merge = a & b; // 충돌: profile
```

충돌은 경로 포함 진단으로 즉시 실패해야 한다.
