# 07. Diagnostics And Testing

## 진단 코드 계층

1. `C_*`: 파싱/문법
2. `L_*`: 언어 의미/평가
3. `B_*`: 빌드 그래프/플래너
4. `P_*`: Parus 통합 프로파일 정책 위반

## 필수 진단 시나리오

1. 파서
   1. `proto`/`export plan ...` 파싱 성공/실패
   2. 제거된 구세대 문법 사용 시 실패
2. 의미론
   1. `&` 합성 충돌 경로 진단
   2. namespace 접근 실패(`alias::missing`)
   3. 배열 인덱스 타입/범위 오류
   4. `proto` 필수 필드 누락/타입 위반 진단
3. 통합 프로파일
   1. `config.lei` 엔트리 해석 실패
   2. `master` 정책 위반(예: export 금지 위반)
   3. built-in plan 템플릿 누락(`bundle`/`master`/`task`/`codegen` 미주입)
   4. built-in plan 스키마 충돌 경로 진단
   5. `task.v1` 필수 필드(`run`) 누락 진단
   6. `codegen.v1` 필수 필드(`outputs`) 누락 진단

## 테스트 축

1. parser 테스트
2. evaluator 테스트
3. graph lower 테스트
4. import cycle 테스트
5. budget 초과 테스트
6. profile 정책 테스트(Parus 통합 계층)
7. built-in plan registry 스냅샷 일치 테스트(LSP/AOT/JIT)
8. proto-merge 테스트(`MyProto & { ... }`)

## 문서 검증 체크

문서에서 구세대 표면 문법 토큰이 재도입되지 않아야 한다.

## 현재 저장소 원칙

1. LEI 테스트는 `tools/Lei/tests/`에서 독립 실행한다.
2. Parus 테스트는 `tests/`에서 기존대로 유지한다.
