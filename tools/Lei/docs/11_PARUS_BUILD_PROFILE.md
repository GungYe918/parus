# 11. LEI Engine Policy (Master/Entry Rules)

이 문서는 LEI 엔진에 상시 적용되는 엔트리/예약 이름 정책을 고정한다.

## 목적

1. LEI는 단일 엔진 정책으로 동작한다.
2. 빌드 엔트리 선택과 `master` 관련 규칙을 구현과 동일하게 고정한다.
3. 기존 parus 연계 워크플로는 이 엔진 정책 위에서 그대로 유지한다.

## 엔트리 파일 규칙

1. 프로젝트 루트의 `config.lei`를 엔트리 파일로 사용한다.
2. `config.lei`는 프로젝트 메타데이터와 최종 마스터 플랜을 정의한다.

## 빌트인 plan 집합 (engine default set)

1. `bundle`
2. `master`
3. `task`
4. `codegen`

## 예약 식별자 규칙

1. `bundle/master/task/codegen`은 예약 식별자다.
2. 선언 위치(`import alias`, `proto`, `let/var`, `def`, 함수 파라미터, `for` iterator)에서 예약 식별자는 금지된다.
3. 예외: `plan master ...` 선언은 허용된다.
4. `export plan master ...`/`export plan master;`는 파싱은 허용되지만 엔진 정책 위반으로 실패한다.

## master 선택 규칙

1. 기본 엔트리 plan은 `master`다.
2. CLI `--plan <name>`이 제공되면 해당 plan을 엔트리로 사용한다.
3. CLI 지정 plan이 없고 `master`가 없으면 에러로 종료한다.
4. 엔트리 plan 루트(`project/bundles/tasks/codegens`)를 canonical graph로 해석한다.
5. `build = { graph: ... }` 직접 명시는 금지다.

## master export 정책

1. `master`는 export 금지다.
2. `export plan master`가 발견되면 `L_MASTER_EXPORT_FORBIDDEN` 오류로 즉시 실패한다.
3. 이 규칙은 옵션 분기 없이 상시 적용된다.

## import 경로 정책

1. 예시에서는 상대 경로(`./`, `../`)만 사용한다.
2. 엔트리 파일/하위 파일 모두 동일하게 상대 경로를 권장한다.

## 결정성/재현성 조건

1. 동일한 `config.lei` + 동일한 CLI 옵션은 동일한 엔트리 plan을 선택해야 한다.
2. `&` 충돌은 빌드 전에 경로 포함 진단으로 즉시 실패한다.
3. 암묵 fallback(다른 plan 자동 선택)은 금지한다.

## 예시 스니펫

```lei
// json/json.lei
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

// tools/tools.lei
export plan lint = task & {
  name = "lint";
  run = ["parusc", "--check", "src/main.pr"];
};

// config.lei
import json from "./json/json.lei";
import tools from "./tools/tools.lei";

plan workspace {
  project = {
    name: "sample",
    version: "0.1.0",
  };
  bundles = [json::json_bundle];
  tasks = [tools::lint];
  codegens = [];
};

plan master = master & {
  project = workspace.project;
  bundles = workspace.bundles;
  tasks = workspace.tasks;
  codegens = workspace.codegens;
};
```
