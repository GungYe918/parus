# 10. Architecture Independence

## 목적

LEI는 Parus 빌드 시스템과 연동될 수 있지만, 언어 자체는 독립적으로 진화한다.

## 독립성 경계

1. LEI 언어 코어
   1. 문법/의미론/진단/예산
   2. `plan`, `export plan`, `import alias`, `&` 규칙
2. Parus 통합 프로파일
   1. `config.lei` 엔트리
   2. `master` 기본 plan 선택
   3. `--plan <name>` override
   4. `master` export 금지 같은 운영 정책

## 코드 경계

1. LEI 구현 코드는 `tools/Lei/include/lei`, `tools/Lei/src`에 둔다.
2. LEI 언어 문서는 Parus 정책을 언어 키워드로 승격하지 않는다.
3. 통합 정책은 별도 프로파일 섹션에서만 다룬다.
4. 빌트인 plan 주입 계약은 `12_BUILTIN_PLAN_SCHEMA_INJECTION.md`에서 다룬다.

## 1회 복제 정책

1. Parus frontend 파서 코어에서 필요한 로직은 1회만 복제했다.
2. 스냅샷 기준 커밋: `287a83b`.
3. 복제 후 Parus frontend 변경을 자동 동기화하지 않는다.

## 빌드 정책

1. 루트 빌드에서 `PARUS_BUILD_LEI`로 LEI 포함 여부를 제어한다.
2. LEI는 standalone configure/build를 유지한다.

## 장기 계획

1. LEI를 별도 저장소로 분리한다.
2. 분리 이후에도 빌드 그래프 계약은 유지한다.
3. Parus 통합 규칙은 프로파일 문서로만 관리한다.
