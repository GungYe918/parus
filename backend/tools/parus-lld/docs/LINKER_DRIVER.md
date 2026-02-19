# Parus Linker Driver Design (Current + Draft)

문서 버전: `v0-draft`
상태: `Implementation-aligned Draft`

## 목적

`parus-lld`의 현재 구현과 유지해야 할 설계 제약을 함께 기록한다.

## 확정된 구현 축

1. shell-string 실행 금지, argv 실행만 사용
2. `.parlib` 직접 입력 지원
3. toolchain/target hash mismatch 하드 에러
4. backend linker는 플랫폼별 lld 계열 기본 사용

## 계층 모델

1. Driver: CLI 파싱 + 환경 해석
2. Planner: 입력 정규화(`LinkPlan`)
3. Backend argv builder: 최종 linker 호출 인자 생성

## SDK 정책

1. Apple SDK payload는 번들하지 않음
2. SDK 경로는 `--apple-sdk-root`, `SDKROOT`, `xcrun` 순으로 해석

## parusc 연계

1. `parusc`가 기본적으로 `parus-lld`를 호출
2. `parus-lld`는 frontend/oir 의미를 다루지 않고 링크에만 집중

## 향후 과제

1. response file(`@rsp`) 지원
2. 링크 단계 진단 코드 체계 세분화
3. Parlib lane/target 선택 정책 고도화

## 코드 근거

1. `backend/tools/parus-lld/main.cpp`
