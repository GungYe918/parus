# SIR and OIR Design Notes

## 목적

SIR과 OIR의 역할 경계를 설계 관점에서 기록한다.

## 역할 분리

1. AST: 문법 보존
2. SIR: 타입/심볼/구조 제어흐름이 반영된 semantic IR
3. OIR: backend 친화적 실행/최적화 IR

## 현재 구현 연결점

1. SIR build 후 verify 수행
2. OIR builder가 SIR 모듈을 입력으로 받아 block/value/inst 생성
3. OIR verify + pass 실행 후 backend 전달

코드 근거:

1. `frontend/src/sir/lower/*`
2. `frontend/src/oir/oir_builder.cpp`
3. `frontend/src/oir/oir_verify.cpp`
4. `frontend/src/oir/oir_passes.cpp`

## mut/nullable/acts 영향

1. mut/acts 해소는 tyck 단계에서 확정
2. SIR/OIR는 확정된 호출 대상 및 대입 의미를 반영
3. nullable coercion 결과는 타입/값 형태로 OIR까지 유지

## 미래 방향

1. SIR canonical pass 도입
2. OIR pre-pass와 backend-lowering 계약 분리
3. incremental compilation 친화 데이터 캐시 도입
