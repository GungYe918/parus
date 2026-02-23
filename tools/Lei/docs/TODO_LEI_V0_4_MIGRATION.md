# TODO LEI v0.4 Migration

상태: `completed`  
목표: `tools/Lei/docs` 신문법(draft-0.4) 기준으로 구현 전면 전환

## Phase Checklist

- [x] Phase 1: 토큰/렉서 전환
- [x] Phase 2: AST 전면 교체
- [x] Phase 3: 파서 전면 교체
- [x] Phase 4: 진단 코드 체계 확장
- [x] Phase 5: 평가기 재구성 (proto/plan/export/builtin-plan)
- [x] Phase 6: 그래프 추출 + Ninja lower 전환 (entry plan 루트 그래프)
- [x] Phase 7: CLI 전환 (`--plan`, 기본 `master`)
- [x] Phase 8: 테스트 전면 교체 (신문법 케이스)
- [x] Phase 9: 문서-구현 정합 검증

## Done Criteria

1. `lei-build`가 기본 엔트리 `master`를 사용해 entry plan 루트 그래프 기반 Ninja를 생성한다.
2. `--plan <name>`로 엔트리 plan override가 동작한다.
3. 구세대 문법(`export build`, `=>`, `?=`, named import)은 호환 없이 실패한다.
4. 빌트인 plan `bundle/master/task/codegen` 스키마 검증이 동작한다.
5. `ctest --test-dir build -R lei_tests --output-on-failure` 통과.

## Verification Command

1. `cmake --build build -j8 --target lei-build lei_tests`
2. `ctest --test-dir build -R lei_tests --output-on-failure`

## Progress Log

- [2026-02-22] 체크리스트 문서 생성.
- [2026-02-22] 토큰/AST/파서를 신문법 기준으로 교체.
- [2026-02-22] 평가기 모델을 `proto/plan/export plan` + `BuiltinPlanRegistry` 기반으로 전환.
- [2026-02-22] 그래프 입력을 entry plan 루트 모델로 전환, Ninja lower 갱신.
- [2026-02-22] CLI에 `--plan` 추가, 기본 엔트리 `master` 반영.
- [2026-02-22] 테스트 케이스를 신문법 세트로 교체 후 전체 통과.

## Open Issues

1. (none)
2. (none)
3. (none)
