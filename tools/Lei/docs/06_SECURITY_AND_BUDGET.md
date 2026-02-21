# 06. Security And Budget

## 금지 기능

1. `for`, `while`
2. 재귀
3. effect/IO
4. 동적 코드 로딩/실행

## 평가 예산

`EvaluatorBudget`를 강제한다.

1. `max_steps`
2. `max_call_depth`
3. `max_nodes`

기본값(v0):

1. `max_steps = 200000`
2. `max_call_depth = 64`
3. `max_nodes = 200000`

하드 상한(v0):

1. `max_steps <= 1000000`
2. `max_call_depth <= 256`
3. `max_nodes <= 1000000`

## 결정성

1. 같은 입력/옵션은 같은 결과를 낸다.
2. 실패는 항상 동일한 코드/위치로 보고된다.
3. 시간/랜덤/환경 의존 내장값을 제공하지 않는다.

## 공개 API (v0)

1. `lei::parse::ParserControl`
   1. 파서 동작 가드(예: 제거 문법 차단)를 외부에서 제어한다.
2. `lei::eval::BuiltinRegistry`
   1. C++ 엔진 코드에서 빌트인 값/네이티브 함수를 등록한다.
   2. 기본 레지스트리는 `make_default_builtin_registry()`로 생성한다.
3. `lei::graph::BuildConventions`
   1. `module_map`, `bundles` 같은 빌드 객체 필드명을 외부에서 구성한다.
