# 08. Roadmap

## 현재 상태 (v0.4 문서 정합 단계)

1. 구세대 문법 제거 완료
2. `proto`/`plan`/`export plan` 중심 명세 고정
3. `&` 엄격 합성 규칙 고정
4. LEI 제품 모델(Core/API/Engine Policy) 문서 체계 확정

## Core 트랙

1. 파서/AST/평가 semantics 정합 유지
2. `ObjectLit`/`PlanPatchLit` 판정 규칙 명확화
3. `&` 충돌 경로 진단 고도화
4. import namespace(`alias::symbol`) 경로 진단 강화

## Build API 트랙

1. `BuiltinRegistry`/`BuiltinPlanRegistry` 계약 안정화
2. 스키마 버전(`*_v1`) 관리 체계 확정
3. graph extraction 계약(엔트리 plan 루트) 고정
4. LSP/AOT/JIT 스냅샷 일치 검증 강화

## Engine Policy 트랙

1. 엔트리 운영 정책(`master`, `--plan`) 안정화
2. 정책 진단(`L_*`/`B_*`) 분리 강화
3. 정책 문서/테스트 템플릿 제공
4. 향후 host 연계 가이드 추가

## 장기 단계

1. LEI 별도 저장소 분리 검토
2. LSP/툴링 강화
3. lockfile/registry 정책 검토
