# 08. Roadmap

## v0 (현재)

1. 최소 문법 + 비튜링 정책
2. named import/export
3. intrinsic `base` 자동 제공
4. build 객체 -> ninja lowering

## v1

1. 제한적 표준 라이브러리 도입 검토
2. schema/validate 재도입
3. richer build profile/target matrix

## v2

1. 제한된 effect capability(명시 opt-in)
2. lockfile/registry 지원
3. incremental evaluation cache

## 확장 원칙

1. 결정성/보안 규칙을 먼저 만족해야 한다.
2. 표현력 확장은 opt-in으로 추가한다.
3. 기존 v0 문법과 호환 가능한 방향을 우선한다.
