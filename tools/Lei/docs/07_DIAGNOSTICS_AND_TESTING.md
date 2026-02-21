# 07. Diagnostics And Testing

## 진단 코드 분리

1. 공통 코드: `C_*`
2. LEI 언어 코드: `L_*`
3. 빌드 그래프/플래너 코드: `B_*`

예시:

1. `C_UNEXPECTED_TOKEN`
2. `L_IMPORT_CYCLE`
3. `L_RECURSION_FORBIDDEN`
4. `B_INVALID_BUILD_SHAPE`

## 리포팅 계층

1. 공통 진단 컨테이너 + 위치(`file/line/col`)
2. 언어별 메시지 카탈로그
3. CLI 렌더러(간단) + 향후 LSP 렌더러

## 테스트 축

1. 파서 테스트
   1. `{}`/`[]`/trailing comma
2. import/export 테스트
   1. named import 성공
   2. 없는 export 실패
   3. cycle 실패
3. evaluator 테스트
   1. `if`/`match`
   2. `&`/`?=`
   3. 예산 초과
4. graph 테스트
   1. deterministic ninja output

## 현재 저장소 반영

1. Parus 테스트는 기존 `tests/` 유지
2. LEI 테스트는 `tools/Lei/tests/`에 분리
