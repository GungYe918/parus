# Parus Linker Driver Design (Draft)

문서 버전: `v0-draft`  
상태: `Design Draft (next implementation target)`

이 문서는 `parus-lld` 및 링크 파이프라인 강화 방향을 정의한다.

---

## 1. 범위

1. `parus-lld`를 shell-string 래퍼에서 argv 기반 드라이버로 전환
2. Parlib 입력(`.parlib`)을 링크 본선에서 직접 처리
3. hash mismatch를 링크 단계 하드 에러로 고정
4. `parusc` 오케스트레이션 규칙 고정

---

## 2. 구성 계층

`parus-lld`는 다음 3계층으로 분리한다.

1. `Driver`: CLI 파싱, 진단 메시지, 모드 선택
2. `Planner`: 입력 오브젝트/Parlib/NativeDeps를 `LinkPlan`으로 정규화
3. `Backend`: 플랫폼별 LLVM linker argv 생성/실행

---

## 3. 실행 정책

1. 기본 링커는 `parus-lld`
2. `parus-lld` 내부 백엔드:
   - Darwin: `ld64.lld`
   - ELF: `ld.lld`
   - COFF: `lld-link`
3. `clang` 경유는 디버그/강제 옵션이 없는 한 기본 비활성화
4. `std::system` 호출 금지, `exec` 계열 argv 호출만 허용
5. 긴 인자는 response file(`@rsp`) 사용

---

## 4. Parlib 통합 규칙

Planner는 `.parlib`를 직접 입력으로 받는다.

1. `ObjectArchive` 선택: lane/target 기준
2. `ExportCIndex` 로드: C export 심볼 조회/충돌 진단
3. `NativeDeps` 로드:
   - `embed`: `NativeArchivePayload` 추출 후 링크 입력에 추가
   - `reference`: 경로/식별자 + hash 검증 후 링크 인자로 변환

---

## 5. hash 불일치 하드 에러

링크 preflight에서 다음을 강제한다.

1. `toolchain_hash` 불일치 -> 즉시 실패
2. `target_hash` 불일치 -> 즉시 실패
3. foreign 입력(C 시스템 라이브러리, 외부 `.o/.a`)은 별도 분류

정책:

1. Parus 산출물에만 hash strict mode를 적용
2. foreign 입력은 strict hash 대상이 아니며 별도 진단 레벨 사용

---

## 6. target/sdks preflight

Darwin 기준 최소 검사:

1. 입력 오브젝트 `LC_BUILD_VERSION` minos 정합성
2. target triple 정합성
3. SDK 경로 해석 가능 여부

SDK 정책:

1. Apple SDK payload를 Parus에 포함하지 않는다.
2. `--apple-sdk-root`, `SDKROOT`, `xcrun` 순으로 참조 경로를 찾는다.

---

## 7. parusc 오케스트레이션

1. 사용자 진입점은 `parusc` 하나로 고정
2. `parusc`는 active toolchain을 통해 같은 버전의 `parus-lld`를 호출
3. 링크 정책/target profile은 `parusc -> parus-lld`로 명시 전달
4. `parus-lld`는 최종 linker backend만 담당하고, frontend/oir 규칙은 관여하지 않는다

