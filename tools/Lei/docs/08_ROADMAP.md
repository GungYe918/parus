# 08. Roadmap

## v0.2 (문서 고정 단계)

1. 구세대 문법 제거 완료
2. `plan`/`export plan` 중심 명세 고정
3. `&` 엄격 합성 규칙 고정
4. LEI 언어 규칙과 Parus 통합 프로파일 규칙 분리

## 다음 구현 단계

1. 파서/AST를 신문법(`plan`, `var`, 블록 `def`, `for`, `return`)에 맞게 갱신
2. 평가기에서 `for` 예산 정책 구현
3. `&` 충돌 경로 진단 고도화
4. import namespace(`alias::symbol`) 경로 진단 강화
5. built-in plan 스키마 주입 API(`BuiltinPlanRegistry`) 구현

## Parus 통합 단계

1. `config.lei` 엔트리 규약 반영
2. `master` 기본 엔트리 + `--plan` override 반영
3. `master` 정책 위반 진단(`P_*`) 추가
4. `bundle`/`master` built-in plan 주입 + 스키마 검증 경로 반영

## 장기 단계

1. LEI 별도 저장소 분리
2. LSP/툴링 강화
3. lockfile/registry 정책 검토
