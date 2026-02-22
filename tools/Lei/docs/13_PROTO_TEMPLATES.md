# 13. Proto Templates

이 문서는 LEI의 `proto` 문법을 사용해 plan 템플릿/스키마를 정의하는 규칙을 고정한다.

## 목적

1. 사용자 정의 스키마를 반복 사용 가능한 템플릿으로 선언한다.
2. `export plan x = MyProto & { ... };` 패턴을 표준화한다.
3. 빌트인 plan(`bundle/master/task/codegen`)과 `proto`의 역할을 분리한다.

## 문법

```lei
proto MyProto {
  fieldA: string;
  fieldB: int = 10;
  fieldC: [string] = [];
};
```

규칙:

1. 각 필드는 `name: type` 형식이다.
2. 기본값은 `name: type = expr`로 선언한다.
3. 기본값 없는 필드는 필수 필드다.
4. `proto` 자체는 실행 plan이 아니며 `export plan` 대상이 아니다.

## 합성 규칙

```lei
export plan foo = MyProto & {
  fieldA = "value";
};
```

1. `MyProto` 타입 제약을 먼저 적용한다.
2. patch에서 지정한 값은 타입 제약을 만족해야 한다.
3. patch에 없는 필드는 기본값으로 채워진다.
4. 필수 필드가 최종 결과에 없으면 오류다.
5. scalar-scalar 합성(예: `foo.name & bar.name`)은 동일성 제약이며 값 덮어쓰기가 아니다.
6. 단일 필드 업데이트는 객체 patch를 사용한다.

## 빌트인 plan과 연쇄 합성

```lei
proto myBundleProto {
  name: string;
  kind: string = "lib";
  sources: [string];
  deps: [string] = [];
};

export plan core_bundle = bundle & myBundleProto & {
  name = "core";
  sources = ["src/lib.pr"];
};
```

1. `bundle`은 Parus가 주입한 빌트인 plan이다.
2. `myBundleProto`는 사용자 제약/기본값 템플릿이다.
3. 마지막 patch는 프로젝트별 실제 값을 제공한다.

## 요구 예시

```lei
proto myProto {
  a: string;
  b: int = 1;
};

export plan foo = myProto & {
  a = "aaa";
};
```

단일 필드 변경 예시:

```lei
plan foo2 = foo & {
  a = "bbb";
};
```

## 진단 포인트

1. 필수 필드 누락
2. 타입 불일치
3. `&` 충돌 경로
4. 미정의 `proto` 참조

## 경계

1. `proto`는 LEI 언어 코어 기능이다.
2. `bundle/master/task/codegen` 의미는 Parus 통합 프로파일이 부여한다.
3. 이 문서는 언어 규칙이며, 엔트리/마스터 선택 규칙은 `11_PARUS_BUILD_PROFILE.md`를 따른다.
