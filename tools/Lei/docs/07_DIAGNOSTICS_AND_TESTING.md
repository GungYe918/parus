# 07. Diagnostics And Testing

## 진단 코드 계층

1. `C_*`: 파싱/문법
2. `L_*`: Core 의미/평가
3. `B_*`: 빌드 그래프/플래너

## 필수 진단 시나리오

1. 파서
   1. `proto`/`export plan ...` 파싱 성공/실패
   2. 제거된 구세대 문법 사용 시 실패
   3. 예약 식별자 선언(`bundle/master/task/codegen`) 실패
2. Core 의미론
   1. `&` 합성 충돌 경로 진단
   2. namespace 접근 실패(`alias::missing`)
   3. 배열 인덱스 타입/범위 오류
   4. `proto` 필수 필드 누락/타입 위반 진단
3. 엔진 정책
   1. `config.lei` 엔트리 해석 실패
   2. `master` 정책 위반(예: export 금지 위반)
   3. legacy explicit graph(`build.graph`) 사용 금지 위반
   4. built-in plan 템플릿 누락(`bundle`/`master`/`task`/`codegen` 미주입)
   5. built-in plan 스키마 충돌 경로 진단
   6. `task.v1` 필수 필드(`run`) 누락 진단
   7. `codegen.v1` 필수 필드(`outputs`) 누락 진단
   8. `--view_graph` CLI 모드 충돌/포맷 오류 진단
4. Parus interop 정책
   1. `import` head가 `bundle.deps`에 없으면 `ImportDepNotDeclared`
   2. 같은 bundle 다른 파일 non-export 참조 시 `SymbolNotExportedFileScope`
   3. 다른 bundle non-export 참조 시 `SymbolNotExportedBundleScope`
   4. 의존 export index 누락 시 `ExportIndexMissing`
   5. 의존 export index 스키마 오류 시 `ExportIndexSchema`

## 테스트 축

1. parser 테스트
2. evaluator 테스트
3. graph lower 테스트
4. import cycle 테스트
5. budget 초과 테스트
6. 엔진 정책 테스트(`master export 금지`, 예약 식별자 규칙)
7. built-in plan registry 스냅샷 일치 테스트(LSP/AOT/JIT)
8. proto-merge 테스트(`MyProto & { ... }`)
9. Parus interop 테스트(`deps/import/export` 가시성 규칙)

## 문서-구현 정합 검증

1. docs의 `lei` 코드블록 파싱 체크를 자동화한다.
2. 핵심 예제는 `lei --check` 실행 가능해야 한다.
3. 엔진 정책 예제는 엔트리 plan 루트 그래프 계약(`project/bundles/tasks/codegens`)을 만족해야 한다.
4. 구세대 표면 문법 토큰이 문서에 재도입되지 않아야 한다.

## 현재 저장소 원칙

1. LEI 테스트는 `tools/Lei/tests/`에서 독립 실행한다.
2. Parus 테스트는 `tests/`에서 기존대로 유지한다.
