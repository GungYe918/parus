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

