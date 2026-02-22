# LEI Useage (Parus Integration Example)

이 문서는 Parus 빌드 시스템과 연계해 LEI를 사용하는 실전 예시를 보여준다.

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

## 4) 루트 config.lei에서 프로젝트 설정 + 마스터 플랜 생성

### `/config.lei`

```lei
import app from "./app/app.lei";
import core from "./core/core.lei";
import json from "./json/json.lei";
import tools from "./tools/tools.lei";

plan defaults {
  profile = "debug";
  opt = 0;
};

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

plan merged_master = master & {
  build = defaults;
  graph = workspace;
};

plan master = merged_master;
```

설명:

1. `project`는 `ProjectMeta` proto를 합성해 타입/필수 필드를 고정한다.
2. bundle은 하위 폴더의 export plan(`json_bundle` 등)을 import해 배열로 합성한다.
3. task/codegen plan은 별도 노드로 import해 master 그래프에 포함한다.
4. 루트는 Parus가 주입한 빌트인 `master` plan과 patch를 `&`로 합성해 최종 `plan master`를 생성한다.

## 5) Parus 빌드 시스템 해석 규칙

1. 엔트리 파일은 `config.lei`다.
2. 기본 엔트리 plan은 `master`다.
3. CLI `--plan <name>`으로 엔트리 plan override가 가능하다.
4. `master`는 export하지 않는다.

### 금지 예시

```lei
export plan master = master & {
  graph = workspace;
};
```

위 예시는 LEI 문법상 가능할 수 있지만, Parus 통합 프로파일에서는 정책 위반으로 처리한다.

## 6) 배열/네임스페이스 접근 예시

```lei
let first_bundle_name = workspace.bundles[0].name;
let app_name = app::app_bundle.name;
let first_task = workspace.tasks[0].name;
```

## 7) 단일 필드 변경 패턴

```lei
plan workspace2 = workspace & {
  project = workspace.project & {
    name = "sample-project-renamed";
  };
};
```

`foo.name & bar.name`은 값 변경이 아니라 동일성 제약이므로, 업데이트 용도로는 위처럼 객체 patch를 사용한다.

## 8) 합성 실패 예시 (`&` 충돌)

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
