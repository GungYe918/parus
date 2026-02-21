# 06. Security And Budget

## 보안 기본 정책

1. LEI는 결정적 평가를 기본으로 한다.
2. 동적 코드 실행은 금지한다.
3. 부작용 IO intrinsic은 기본 금지한다.
4. 평가 실패는 즉시 중단한다.

## 제어문 정책

1. `for`는 허용한다.
2. 재귀 호출은 금지한다.
3. 무제한 반복은 금지한다.
4. `while`은 현재 지원하지 않는다.

## 예산 정책 (수치 비고정)

평가기는 아래 예산을 정책적으로 강제해야 한다.

1. 전체 평가 단계 예산(`max_steps`)
2. 호출 깊이 예산(`max_call_depth`)
3. 루프 반복 예산(`max_loop_iters`, `max_total_loop_steps`)
4. 노드/출력 크기 예산(`max_nodes`, `max_output_nodes`)
5. task 계획 예산(`max_tasks`)
6. codegen 계획 예산(`max_codegens`)

정책 원칙:

1. 예산 초과 시 결정적 진단과 함께 실패한다.
2. 예산은 CLI/호스트 설정으로 조정 가능해야 한다.
3. 상한 clamp 정책을 제공해야 한다.

## 결정성 정책

1. 같은 입력 + 같은 옵션이면 같은 결과를 생성한다.
2. 시간/랜덤/환경 의존 intrinsic은 기본 제공하지 않는다.
3. import 해석 순서는 DAG 기반으로 결정적이어야 한다.
4. `task`/`codegen` plan 해석은 입력 plan과 옵션만으로 결정적이어야 한다.

## intrinsic 안전 계약

1. intrinsic 함수는 순수/결정적이어야 한다.
2. 입력이 같으면 출력이 같아야 한다.
3. 내부 상태를 숨겨 변경하면 안 된다.

## built-in plan 주입 안전 계약

1. built-in plan 템플릿/스키마는 평가 시작 전에 1회 주입한다.
2. 평가 중 built-in plan 레지스트리 변경은 금지한다.
3. LSP/AOT/JIT는 동일 스키마 스냅샷을 공유해야 한다.
4. `task.v1`, `codegen.v1` 스키마 위반은 빌드 실행 이전 단계에서 차단한다.
