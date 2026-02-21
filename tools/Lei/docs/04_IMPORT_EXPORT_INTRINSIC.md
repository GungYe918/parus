# 04. Import Export Intrinsic

## 모듈 경계

1. 파일 단위 모듈을 사용한다.
2. 외부 공개는 `export`로만 허용한다.
3. 기본 정책은 named import/export only다.

## export

1. `export const NAME = ...;`
2. `export let NAME = ...;`
3. `export def NAME(...) => ...;`
4. `export build EXPR;` (최종 산출값)

## import

1. 파일 import: `import { a, b } from "./x.lei";`
2. wildcard import, namespace import는 v0에서 금지한다.
3. `import intrinsic ...` 문법은 v0에서 제거한다.

## intrinsic (자동 제공)

v0 intrinsic은 `base`만 제공하며, import 없이 자동으로 스코프에 주입된다.

`base` 예시:

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

상세 intrinsic 설명은 `09_INTRINSICS.md`를 따른다.

## cycle 정책

1. import 그래프는 DAG여야 한다.
2. 순환 발견 시 즉시 실패한다.
3. 진단에는 cycle 경로를 포함한다.
