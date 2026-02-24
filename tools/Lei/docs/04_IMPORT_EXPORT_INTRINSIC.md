# 04. Import Export Intrinsic

## 모듈 경계

1. LEI는 파일 단위 모듈을 사용한다.
2. 외부 노출은 `export plan`으로 수행한다.
3. import는 별칭(alias) 기반 namespace로 사용한다.

## import

```lei
import app from "./app/app.lei";
import json from "./json/json.lei";
```

규칙:

1. import 경로는 상대 경로를 사용한다.
2. wildcard import는 지원하지 않는다.
3. 구형 intrinsic 전용 import 형식은 지원하지 않는다.

## namespace 접근

import된 심볼은 `alias::symbol`로 접근한다.

```lei
let bundles = [app::app_bundle, json::json_bundle];
```

## export

권장:

```lei
export plan json_bundle = bundle & {
  name = "json";
  kind = "lib";
  modules = [
    module & {
      head = "json";
      sources = ["json/src/json.pr"];
      imports = [];
    },
  ];
  deps = [];
};
```

허용:

```lei
plan json_bundle = bundle & {
  name = "json";
  kind = "lib";
  modules = [
    module & {
      head = "json";
      sources = ["json/src/json.pr"];
      imports = [];
    },
  ];
  deps = [];
};

export plan json_bundle;
```

## intrinsic 개념

1. intrinsic은 언어 공용 빌트인 함수/값 레지스트리 개념이다.
2. 특정 단일 고정 객체를 언어가 특수 취급하지 않는다.
3. 상세 정책은 `09_INTRINSICS.md`를 따른다.
4. intrinsic 상세 목록(상수/함수 카탈로그)은 `15_BUILTIN_CONSTANTS_AND_FUNCTIONS.md`를 따른다.

## cycle 정책

1. import 그래프는 DAG여야 한다.
2. 순환 발견 시 즉시 실패한다.
3. 진단에는 cycle 경로를 포함한다.
