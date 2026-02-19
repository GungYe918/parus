# parus-lld Parlib Link Plan

## 목적

`.parlib` 입력을 실제 링크 인자로 변환하는 정책을 정리한다.

## 입력 계획 구조

1. `LinkPlan.object_inputs`
2. `LinkPlan.native_args`
3. 임시 추출 파일 목록(`temp_files`)

## 처리 순서

1. `ParlibReader::open()`
2. header hash/triple 검증
3. object lane 선택 (`pcore` -> `prt` -> `pstd` 우선)
4. `ObjectArchive` chunk 추출 후 임시 파일에 저장
5. `NativeDeps` 처리
   - `embed`: payload chunk 추출 후 파일 추가
   - `reference`: framework/lib 인자 변환

## hash 정책

1. `--toolchain-hash` 또는 env/manifest 해시와 header `compiler_hash` 비교
2. `--target-hash` 또는 env/manifest 해시와 header `feature_bits` 비교
3. 불일치 시 hard error

## 제약

1. expected hash가 0이면 해당 검사는 생략
2. `.parlib` 내부 object chunk가 없으면 실패

## 코드 근거

1. `backend/tools/parus-lld/main.cpp` (`plan_inputs_`)
2. `backend/include/parus/backend/parlib/Parlib.hpp`
