# 05. Build Model

## Parus 모델 연결

LEI는 Parus 빌드 단위를 그대로 기술한다.

```text
file -> module -> bundle -> project
```

## 핵심 규칙

1. module은 파일 시스템 관례가 아니라 명시 매핑으로 정의한다.
2. `module_map`에서 모듈 경로와 파일 목록을 명시한다.
3. 루트 기준 상대경로를 사용한다.
4. `mod.rs`/폴더명 암묵 규칙을 사용하지 않는다.
5. 빌드 객체 필드명(`module_map`, `defaults`)은 Starlark/CMake 관용어를 참고한다.
6. Parus 단위명(`project`, `bundle`, `module`)은 그대로 유지한다.

## 권장 build 객체 형태

```lei
export build {
  project: "workspace-name",
  profile: "debug",
  bundles: [
    {
      name: "core",
      root: "core",
      module_map: {
        core::math: ["core/math/add.pr", "core/math/mul.pr"],
      },
      deps: [],
    },
  ],
}
```

## bundle prepass 연동

1. bundle 단위 선언 수집 prepass를 위해 module_map은 정적 평가 가능해야 한다.
2. module_map은 런타임 입력/IO 없이 계산되어야 한다.
