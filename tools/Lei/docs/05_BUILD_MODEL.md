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

## Parus interop 계약 (bundle import/export)

1. `bundle.deps`는 Parus의 `import <head>` 검증에 직접 사용된다.
2. Parus는 bundle 단위 prepass로 export index를 생성하고 의존 bundle index를 로드한다.
3. 같은 bundle 내부라도 **다른 파일 선언 참조**는 `export`된 선언만 허용된다.
4. 다른 bundle 선언 참조도 `export`된 선언만 허용된다.
5. `import <head>`의 `<head>`가 현재 bundle `deps`에 없으면 컴파일 오류다.
6. `nest` 경로가 최종 심볼 경로 정본으로 사용된다.

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

plan workspace {
  project = ProjectMeta & {
    name = "demo";
    version = "0.1.0";
  };
  bundles = [json::json_bundle, app::app_bundle];
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

## 배열 접근 예제

```lei
let first_name = workspace.bundles[0].name;
```

## 단일 필드 변경 예제

```lei
plan workspace2 = workspace & {
  project = workspace.project & {
    name = "demo-renamed";
  };
};
```

## LEI Core vs LEI Engine Policy

LEI Core 규칙:

1. Core는 `proto`/`plan`/`export plan`/`import alias`/`&`를 정의한다.
2. Core는 `master` 같은 엔트리 이름을 예약어로 갖지 않는다.

LEI Engine Policy 규칙:

1. `config.lei`를 엔트리 파일로 특별취급한다.
2. `config.lei`의 `plan master` 또는 CLI 지정 plan을 엔트리로 사용한다.
3. `master` export는 정책상 금지한다.
4. 하위 bundle의 `master` import/재export는 정책상 금지한다.
5. `bundle`, `master`, `task`, `codegen`은 엔진이 주입한 빌트인 plan 값으로 해석한다.
6. `project`는 특수 plan이 아니다.
7. 엔트리 plan 루트(`project/bundles/tasks/codegens`)를 엔진이 canonical graph로 해석한다.
8. `build = { graph: ... }` 직접 명시는 금지된다.
9. 그래프 조회는 `lei --view_graph [--format json|text|dot]`로 수행한다.

위 규칙은 LEI Core 문법이 아니라 엔진 정책이다.
