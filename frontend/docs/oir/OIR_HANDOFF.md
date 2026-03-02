# OIR Handoff

## 목적

SIR에서 OIR로 넘어가는 경계(semantic handoff)를 정의한다.

## 현재 구현 (코드 근거)

1. OIR builder: `frontend/src/oir/oir_builder.cpp`
2. OIR verify: `frontend/src/oir/oir_verify.cpp`
3. OIR passes: `frontend/src/oir/oir_passes.cpp`
4. API: `frontend/include/parus/oir/Builder.hpp`, `frontend/include/parus/oir/Passes.hpp`

## handoff 원칙

1. SIR verify 통과 모듈만 OIR build 대상
2. SIR의 type/place/effect 정보를 OIR value/inst 메타로 반영
3. 제어 흐름은 block/terminator 중심으로 재구성

## 주요 매핑

1. SIR local/var -> OIR alloca/load/store 또는 direct SSA value
2. SIR call -> OIR `InstCall` (direct callee 가능 시 direct_callee 채움)
3. SIR struct/index -> OIR `InstField`/`InstIndex`
4. 상수 -> `InstConst*`

## verify 및 pass 경계

1. build 후 즉시 OIR verify
2. pass는 verify 가능한 모듈만 입력으로 가정
3. pass 이후 backend 전달 전 재검증 권장

## 제약/비범위 (v0)

1. full SSA canonical form 강제는 아직 아님
2. nullable/optional 고급 최적화 제한적

## 미래 설계 (v1+)

1. frontend OIR와 backend lowering 계약 인터페이스 분리
2. pass pipeline profile/trace 산출
