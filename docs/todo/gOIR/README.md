# Parus gOIR Track

문서 버전: `draft-0.3`  
상태: `Design Freeze (TODO Track)`

이 디렉터리는 Parus의 차세대 IR 축인 `gOIR` 설계 문서를 모은다.
이 트랙의 결정 사항은 현재 구현보다 미래 파이프라인 기준의 canon으로 취급한다.

## 1. 목적

1. 현재 `OIR -> LLVM` CPU 경로를 reference/bootstrap lane으로 유지한다.
2. 차세대 메인라인을 `frontend -> SIR -> gOIR-open -> placement/specialization -> gOIR-placed -> lower target -> target-code`로 고정한다.
3. `gOIR`를 Parus 전용 `semantic + realization + placement + family-region` IR로 정의한다.
4. `gOIR` 안에서 `SemanticSig`, `GComputation`, `GRealization`, `GPlacementPolicy`, `GService`와 `coreOIR`, `cpuOIR`, `gpuOIR`, `hw.struct`, `hw.flow`, `bridgeOIR`를 함께 다룬다.
5. GPU/HW/backend 확장을 문법보다 먼저 IR 계약과 boundary 모델 중심으로 진행한다.

## 2. canon 결정

1. 현재 구현의 `OIR`은 미래 설계 문맥에서 개념적으로 `legacy cpuRefOIR` 또는 `current OIR`로 부른다.
2. 새 `gOIR`는 기존 `OIR`의 단순 확장이 아니라 재설계된 umbrella IR이다.
3. `gOIR`는 MLIR을 그대로 복제하지 않는다.
4. `gOIR`의 핵심 추상화는 family region만이 아니라 `SemanticSig`, `GComputation`, `GRealization`, `GPlacementPolicy`, `GService`다.
5. `gOIR`는 `gOIR-open`과 `gOIR-placed`의 2단계 모델을 정식 형태로 사용한다.
6. `gOIR-open`에서는 semantic invoke와 realization set를 유지하고, `gOIR-placed`에서 family-specific call과 explicit bridge를 확정한다.
7. `bridgeOIR`는 transport-only 계층이 아니라 interop/placement/runtime boundary 계층이다.
8. hardware lane은 `hw.struct`와 `hw.flow`로 정식 분리한다.
9. `hw.struct`는 structural, portable, legacy-friendly hardware lane이다.
10. `hw.flow`는 portable spatial/dataflow hardware lane이며 FPGA overlay, CGRA-like backend, future spatial backend의 공통 진입점이다.
11. PMF는 `gOIR` family가 아니라 lower target/backend project로 다룬다.
12. 무거운 최적화와 target-specific legalization은 MLIR 이후 단계로 넘긴다.
13. cross-region 값 이동과 실행 경계는 placed 단계에서 명시적 bridge node를 통해서만 허용한다.
14. hot-reload는 이 트랙의 정식 범위에 포함하지 않는다.

## 3. 문서 인덱스

1. `GOIR_MODEL.md`
   - semantic entity
   - open/placed 단계
   - 메모리 포맷
   - 계약, ABI, placement, lowering, compile-time, JIT 정책
2. `GOIR_OPCODE_MODEL.md`
   - 공통 표현 규칙
   - placed-stage opcode 체계
   - `hw.struct` / `hw.flow` 분리
   - semantic invoke가 family opcode에 주는 영향

## 4. 현재 코드베이스와의 관계

1. 현재 코드베이스의 실제 구현 메인라인은 여전히 `SIR -> OIR -> LLVM`이다.
2. 이 디렉터리의 문서는 그 구현을 즉시 대체하지 않으며, 차세대 파이프라인의 설계 기준을 고정한다.
3. migration 중에는 legacy OIR, `gOIR-open`, `gOIR-placed`가 병행 존재할 수 있다.
