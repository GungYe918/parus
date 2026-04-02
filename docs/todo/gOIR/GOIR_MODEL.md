# Parus gOIR Model

문서 버전: `draft-0.3`  
상태: `Design Freeze (TODO Track)`

이 문서는 Parus의 차세대 `gOIR`를 "multi-realization semantic IR + placed IR" 모델로 정의한다.
본 문서의 기준 파이프라인은 아래와 같다.

```text
frontend
-> SIR
-> gOIR-open
-> placement/specialization
-> gOIR-placed { core, cpu, gpu, hw.struct, hw.flow, bridge }
-> lower target
   -> MLIR / CIRCT / overlay backend / PMF backend
-> target-code
```

## 1. 배경과 canon 결정

1. 현재 `OIR`은 backend 친화적 실행/최적화 IR이며 CPU/LLVM 경로에 강하게 맞춰져 있다.
2. GPU/HW까지 장기 확장을 하려면 "여러 family region을 담는 IR"만으로는 부족하고, 하나의 계산과 여러 realization을 함께 다루는 계층이 필요하다.
3. 따라서 현재 OIR은 migration 기간 동안 `legacy cpuRefOIR` 역할로 남긴다.
4. 새 메인라인은 `gOIR-open -> gOIR-placed -> lower target`으로 간다.
5. `gOIR`는 MLIR을 그대로 복제하지 않고, Parus 전용 semantic contract, realization, placement, bridge, archive 중심 IR로 설계한다.
6. PMF는 `gOIR` 내부 family가 아니라 `hw.flow`에서 이어지는 lower target/backend project로 취급한다.

## 2. 목표

1. 하나의 계산 의미와 여러 realization을 같은 모듈 안에서 관리한다.
2. `gOIR-open`은 계산 의미와 candidate realization을 보존해야 한다.
3. `gOIR-placed`는 선택된 realization, concrete ABI, bridge, layout freeze를 명확히 가져야 한다.
4. `coreOIR`는 매우 빠르게 verify/canonicalize 가능해야 한다.
5. portable hardware lane과 target-specific hardware lane을 분리해야 한다.
6. Parus가 기존 FPGA/ASIC/CIRCT lane과 future spatial backend를 모두 받을 수 있어야 한다.
7. parlib archive와 JIT/AOT 공용 입력으로 쓸 수 있어야 한다.

## 3. 비목표

1. `gOIR`를 범용 외부 생태계 IR로 만드는 것
2. MLIR dialect 시스템 전체를 다시 구현하는 것
3. 서로 다른 target op를 같은 block 안에서 instruction 수준으로 뒤섞는 것
4. PMF-specific activation 모델을 `gOIR` family 자체에 넣는 것
5. arbitrary full hardware synthesis를 인터랙티브 JIT처럼 취급하는 것
6. hot-reload를 v0 설계 범위에 넣는 것

## 4. 전체 구성

`gOIR`는 단일 IR이 아니라, semantic entity 계층과 family region 계층을 함께 가지는 umbrella 구조다.

### 4.1 semantic entity 계층

1. `SemanticSig`
   - 계산 자체의 논리 입력/출력, effect, state, time, resource 계약
2. `GComputation`
   - 하나의 논리 계산 단위
   - 여러 `GRealization`을 소유
3. `GRealization`
   - 특정 computation의 `cpu/gpu/hw.struct/hw.flow` 구현체
   - entry region, concrete ABI, resource 요구, specialization 메타데이터를 보유
4. `GPlacementPolicy`
   - fixed, static-choice, runtime-auto, template-only 같은 placement 정책
5. `GService`
   - host service, runtime service, hw service endpoint 같은 상호 운용 경계 정의

### 4.2 stage 분리

1. `gOIR-open`
   - semantic invoke를 유지한다.
   - computation과 realization set를 보존한다.
   - bridge는 부분적으로만 materialize될 수 있다.
   - concrete call ABI, concrete launch/invoke 방식, externally visible layout은 아직 확정되지 않을 수 있다.
2. `gOIR-placed`
   - realization 선택이 끝난 상태다.
   - family-specific call, `bridgeOIR`, concrete layout, resource residency, sync 경계가 확정된다.
   - MLIR/CIRCT/overlay/PMF backend의 직접 입력은 기본적으로 이 단계다.

### 4.3 family region 계층

1. `coreOIR`
   - 매우 작은 공통 코어
   - SSA, block, branch, const, arithmetic, compare, select, shape/index, tuple/aggregate pack-unpack
2. `cpuOIR`
   - placed-stage CPU realization family
   - loop/access/reduction/vector/alignment/noalias/call/stack-memory/exception-free host compute
3. `gpuOIR`
   - placed-stage GPU realization family
   - device kernel, address space, barrier, subgroup, image/buffer/framebuffer resource
4. `hw.struct`
   - placed-stage structural hardware family
   - module/port/instance/reg/wire/comb/seq/mem 중심의 portable lane
   - CIRCT/Verilog/legacy FPGA/ASIC flow로 내리기 위한 기준점
5. `hw.flow`
   - placed-stage portable spatial/dataflow hardware family
   - stream/channel/pipeline/window/service-friendly dataflow 중심의 portable lane
   - FPGA overlay, CGRA-like backend, future PMF backend의 공통 입력
6. `bridgeOIR`
   - launch/copy 계층에 그치지 않고 interop, placement, runtime boundary를 표현

### 4.4 region 규칙

1. 한 region은 정확히 하나의 primary family에 속한다.
2. region 내부에서는 같은 family op만 허용한다.
3. 다른 family 값 참조는 placed 단계에서 bridge 출력만 허용한다.
4. open 단계의 cross-family 의미 연결은 computation/realization/service 참조로 표현하고 direct family call로 고정하지 않는다.
5. `hw.struct`와 `hw.flow`는 서로 다른 family이며 direct op mixing을 허용하지 않는다.
6. bridge는 legality, ownership, sync, residency, layout 재검증 지점이다.

## 5. 메모리 내 포맷

메모리 내 표현은 현재 OIR처럼 `u32 id + arena` 스타일을 유지한다.
이 선택은 캐시 효율, 직렬화 단순성, verify 속도 때문에 고정한다.

### 5.1 최상위 모듈

```cpp
struct GModule {
    GHeader header;
    StringTable strings;
    TypeTable types;
    LayoutTable layouts;
    SymbolTable symbols;
    AttrPool attrs;
    SemanticSigTable semantic_sigs;
    AbiSigTable abi_sigs;
    ComputationTable computations;
    RealizationTable realizations;
    PlacementPolicyTable placement_policies;
    ServiceTable services;
    ResourceTable resources;
    DialectRegistry dialects;
    std::vector<GRegion> regions;
    std::vector<GBlock> blocks;
    std::vector<GOp> ops;
    std::vector<GValue> values;
    std::vector<GBridge> bridges;
    DebugMap debug;           // optional
    AnalysisCache analysis;   // optional, non-canonical
};
```

### 5.2 헤더

```cpp
struct GHeader {
    uint32_t goir_version_major;
    uint32_t goir_version_minor;
    uint32_t internal_abi_rev;
    uint16_t stage_kind;      // Open or Placed
    uint16_t reserved0;
    uint32_t feature_mask;
    uint32_t target_profile;
    uint64_t source_hash;
    uint64_t compiler_hash;
};
```

고정 규칙:

1. `goir_version_*`는 포맷 호환성 기준이다.
2. `internal_abi_rev`는 `SemanticSig`, `AbiSig`, `bridgeOIR` 의미 호환성 기준이다.
3. `stage_kind`는 `gOIR-open`과 `gOIR-placed`를 구분한다.
4. `feature_mask`는 optional semantic/placement/encoding feature 사용 여부를 기록한다.
5. `target_profile`은 generic, host-cpu, gpu-runtime, hw-runtime 같은 상위 환경 프로필을 식별한다.

### 5.3 semantic 계약 개체

```cpp
struct SemanticSig {
    uint32_t logical_arg_desc_begin;
    uint16_t logical_arg_desc_count;
    uint16_t logical_result_desc_count;
    uint16_t effect_contract;
    uint16_t state_contract;
    uint16_t time_contract;
    uint16_t resource_contract;
    uint32_t attr_begin;
    uint16_t attr_count;
    uint16_t flags;
};

struct GComputation {
    uint32_t symbol;
    uint32_t semantic_sig;
    uint32_t placement_policy;
    uint32_t realization_begin;
    uint16_t realization_count;
    uint16_t trait_mask;
    uint32_t attr_begin;
    uint16_t attr_count;
    uint16_t flags;
};

struct GRealization {
    uint32_t computation;
    uint16_t family_kind;     // Core/Cpu/Gpu/HwStruct/HwFlow
    uint16_t invoke_kind;     // InlineLocal/DirectCpu/LaunchGpu/InvokeHw/ServiceMediated
    uint32_t entry_region;
    uint32_t abi_sig;
    uint32_t resource_req_begin;
    uint16_t resource_req_count;
    uint16_t specialization_count;
    uint32_t specialization_begin;
    uint32_t cost_model_key;
    uint32_t attr_begin;
    uint16_t attr_count;
    uint16_t flags;
};

struct GPlacementPolicy {
    uint16_t selection_kind;  // Fixed/StaticChoice/RuntimeAuto/TemplateOnly
    uint16_t fallback_kind;
    uint16_t cost_model_kind;
    uint16_t preferred_family_mask;
    uint32_t attr_begin;
    uint16_t attr_count;
    uint16_t flags;
};

struct GService {
    uint32_t symbol;
    uint16_t endpoint_kind;   // HostService/RuntimeService/HwService
    uint16_t owner_family_mask;
    uint32_t semantic_sig;
    uint32_t abi_sig;
    uint32_t attr_begin;
    uint16_t attr_count;
    uint16_t flags;
};
```

고정 규칙:

1. `SemanticSig`는 계산 자체를 기술하고, realization 선택과 무관한 계약이어야 한다.
2. `GComputation`은 반드시 하나 이상의 candidate realization을 가질 수 있어야 하지만, open 단계에서는 실제 선택이 확정되지 않을 수 있다.
3. `GRealization`은 concrete ABI와 concrete family를 가진다.
4. `GPlacementPolicy`는 compile-time 또는 runtime placement 결정을 안내하지만, 계산 의미를 바꾸지 않는다.
5. `GService`는 direct cross-family call 대신 service-mediated interop를 모델링하는 기본 단위다.

### 5.4 region / block / op / value

```cpp
enum class GRegionKind : uint8_t {
    Core,
    Cpu,
    Gpu,
    HwStruct,
    HwFlow,
    Bridge,
};

struct GRegion {
    GRegionKind kind;
    uint32_t owner_realization;
    uint32_t symbol;
    uint32_t abi_sig;
    uint32_t attr_begin;
    uint32_t attr_count;
    uint32_t block_begin;
    uint32_t block_count;
};

struct GBlock {
    uint32_t region;
    uint32_t param_begin;
    uint32_t param_count;
    uint32_t op_begin;
    uint32_t op_count;
};

struct GOp {
    uint16_t dialect_id;
    uint16_t opcode;
    uint32_t result_begin;
    uint16_t result_count;
    uint32_t operand_begin;
    uint16_t operand_count;
    uint32_t attr_begin;
    uint16_t attr_count;
    uint32_t child_region_begin;
    uint16_t child_region_count;
    uint16_t flags;
};

struct GValue {
    uint32_t type_id;
    uint32_t def_region;
    uint32_t def_block;
    uint32_t def_op;
    uint16_t def_result_index;
    uint16_t value_flags;
};
```

고정 규칙:

1. op 의미는 `(dialect_id, opcode)` 쌍으로 식별한다.
2. dialect별 payload는 attr/side-table 기반으로 인코딩한다.
3. structured op는 선택적으로 child region을 소유할 수 있다.
4. canonical 의미는 `semantic_sigs`, `computations`, `realizations`, `placement_policies`, `services`, `ops`, `values`, `bridges`, `types`, `layouts`, `abi_sigs`를 함께 본다.
5. analysis cache는 언제든 폐기 가능해야 한다.

## 6. family별 최소 범위

### 6.1 coreOIR

`coreOIR`는 매우 작아야 하며, 고수준 target 의미를 넣지 않는다.

허용 범위:

1. integer/float/bool/index const
2. unary/binary arithmetic
3. compare
4. select
5. tuple/aggregate pack-unpack
6. block param
7. `br`, `condbr`, `ret`
8. shape/index helper

비허용:

1. `alloca/load/store`
2. actor/exception/runtime object
3. address space
4. barrier
5. clock/reset
6. raw pointer/FFI contract

### 6.2 cpuOIR

1. structured loop
2. affine-ish access summary
3. reduction op
4. vector pack/unpack
5. alignment/noalias metadata
6. explicit memory ops
7. placed-stage direct host call과 host runtime bridge

### 6.3 gpuOIR

1. kernel entry
2. builtin ids: global/local/group/subgroup
3. address spaces: global/shared/const/private
4. barrier/memory fence
5. async copy and token
6. buffer/image/sampler/framebuffer resource ops
7. placed-stage device call과 device-local sync/resource semantics

### 6.4 hw.struct

1. hw module
2. port bundle
3. wire/reg
4. comb region
5. seq region with clock/reset
6. memory macro / instance
7. legacy-friendly portable structural lane

의도:

1. 기존 CIRCT/Verilog/FPGA/ASIC flow와 자연스럽게 이어진다.
2. 구조를 드러내는 HW subset의 기준점이 된다.
3. PMF-specific activation, capsule, residency 의미는 여기 넣지 않는다.

### 6.5 hw.flow

1. stream/channel handshake
2. pipeline/stage
3. task/dataflow control
4. buffer/window/residency-friendly flow
5. service endpoint cooperation
6. spatial/dataflow portable lane

의도:

1. PMF 전용 IR가 아니라 overlay, CGRA-like backend, future spatial backend의 공통 입력이다.
2. direct structural netlist보다 계산 흐름과 지역성 제약을 오래 보존한다.
3. lower target에서 필요하면 `hw.struct`, overlay IR, PMF backend IR로 각각 갈라질 수 있다.

### 6.6 bridgeOIR

`bridgeOIR`는 target-specific region을 이어주는 transport 계층이 아니라,
interop/placement/runtime boundary 계약층이다.

범위:

1. launch / invoke / copy / bind / map / unmap
2. event / sync / wait
3. service request / reply
4. realization selection / fallback
5. residency migration / ownership transfer
6. template select / configure

비범위:

1. PMF capsule activation 모델
2. PMF context bank 운영 세부
3. PMF warm/cold activation 세부

## 7. archive 정책

### 7.1 open/placed 분리 직렬화

1. 메모리 내 `gOIR`와 디스크 직렬화 포맷은 분리한다.
2. archive는 `gOIR-open`과 `gOIR-placed`를 모두 담을 수 있어야 한다.
3. verifier는 payload 일부만 읽고도 stage, semantic contract, ABI, section integrity를 검사할 수 있어야 한다.

### 7.2 parlib 통합

1. 초기 단계에서는 기존 `OIRArchive` chunk kind를 유지한다.
2. `OIRArchive` payload 헤더에 `format_kind = legacy-oir | goir-open-v1 | goir-placed-v1`를 둔다.
3. 추후 필요 시 chunk kind 분리는 가능하지만 v0 canon은 payload-level multiplexer다.

### 7.3 goir-v1 archive 섹션

1. `ArchiveHeader`
2. `StringTable`
3. `TypeTable`
4. `LayoutTable`
5. `SymbolTable`
6. `SemanticSigTable`
7. `AbiSigTable`
8. `ComputationTable`
9. `RealizationTable`
10. `PlacementPolicyTable`
11. `ServiceTable`
12. `DialectRegistry`
13. `ResourceTable`
14. `RegionDirectory`
15. `RegionPayloads`
16. `BridgeTable`
17. `DebugMap` optional
18. `AnalysisCache` optional

### 7.4 직렬화 규칙

1. 모든 테이블은 little-endian binary로 기록한다.
2. 각 섹션은 offset/size/hash를 가진다.
3. region payload는 독립 seek/read 가능해야 한다.
4. analysis cache는 불일치 시 즉시 폐기 가능해야 한다.

## 8. 계약과 ABI

Parus의 인터페이스 계약은 semantic layer와 concrete ABI layer를 분리한다.

### 8.1 SemanticSig

`SemanticSig`는 계산 자체의 계약이다.

범위:

1. logical args/results
2. effect contract
3. state contract
4. time contract
5. resource contract

의도:

1. computation 의미를 concrete realization과 분리한다.
2. 같은 computation이 여러 `AbiSig`를 가질 수 있게 한다.
3. placement와 service mediation이 semantic 레벨에서 reasoning 가능하도록 한다.

### 8.2 AbiSig

`AbiSig`는 realization별 concrete ABI다.

```cpp
struct AbiSig {
    uint16_t cc_kind;
    uint16_t layout_profile;
    uint16_t implicit_arg_policy;
    uint16_t effect_contract;
    uint16_t sync_contract;
    uint16_t ownership_contract;
    uint32_t arg_desc_begin;
    uint16_t arg_desc_count;
    uint16_t result_desc_count;
};
```

`cc_kind` 예시:

1. `ParusCpu`
2. `C`
3. `KernelEntry`
4. `KernelLaunch`
5. `HwTemplate`
6. `RuntimeIntrinsic`
7. `ServiceEndpoint`

`layout_profile` 예시:

1. `NativeCpu`
2. `C`
3. `GpuBuffer`
4. `GpuImage`
5. `FramebufferPixel`
6. `HwPortBundle`
7. `PackedInternal`

### 8.3 Bridge ABI

Bridge ABI는 서로 다른 실행 자원 사이의 경계 ABI다.

범위:

1. host <-> gpu
2. host <-> hw.struct
3. host <-> hw.flow
4. gpu <-> runtime service
5. hardware realization <-> runtime service
6. service-mediated host/hw cooperation

원칙:

1. bridge는 explicit descriptor 중심으로 표현한다.
2. bridge는 implicit runtime object를 숨기지 않는다.
3. bridge는 ownership, sync, visibility, residency legality를 재검증한다.
4. PMF-specific activation 계약은 backend lower-target 문서로 넘긴다.

### 8.4 외부 ABI

1. 외부 안정 ABI는 기존 `c-v0`를 유지한다.
2. `layout(c)`와 `extern/export "C"` 규칙은 그대로 둔다.
3. GPU/HW 가속 경로도 외부 노출 시 최종 경계는 가능하면 `c-v0` wrapper로 감싼다.

### 8.5 ABI legality rules

1. GPU kernel ABI에는 actor/class/borrow/escape/throw를 넣지 않는다.
2. hardware template ABI에는 runtime object와 heap object를 넣지 않는다.
3. `SemanticSig`는 family에 종속되지 않아야 한다.
4. 하나의 computation은 여러 `AbiSig`를 가진 realization을 가질 수 있다.

## 9. lowering 전략

### 9.1 정식 메인라인

```text
AST
-> Tyck
-> CAP
-> SIR build + verify
-> SIR canonical + partition analysis
-> gOIR-open build
-> gOIR-open verify + cheap canonicalize
-> placement/specialization
-> gOIR-placed verify + cheap canonicalize
-> lower target
-> target-specific pipeline
-> target-code
```

### 9.2 SIR -> gOIR-open

1. SIR canonical pass에서 shape, effect, target intent를 정리한다.
2. computation 후보를 식별하고 `SemanticSig`를 만든다.
3. candidate realization set를 `cpu/gpu/hw.struct/hw.flow` family 기준으로 붙인다.
4. open 단계 호출은 concrete family call이 아니라 semantic invoke로 유지한다.
5. layout과 ABI는 logical 수준까지만 기록하고 externally visible concrete form은 아직 freeze하지 않는다.

### 9.3 gOIR-open 내부 pass

허용:

1. semantic/realization consistency verify
2. cheap constant fold
3. dead semantic result prune
4. debug/source map normalization
5. open-stage legality check

비권장:

1. deep LICM/GVN
2. aggressive vectorization
3. target-specific scheduling
4. concrete launch/invoke materialization 이전의 과도한 ABI freeze

### 9.4 placement / specialization

역할:

1. computation별 realization 선택
2. 필요 시 runtime-auto 또는 template-only policy 구체화
3. semantic invoke를 direct family call 또는 `bridgeOIR`로 materialize
4. service request / reply 경계 삽입
5. concrete layout, residency, sync contract freeze
6. `hw.flow` realization이 선택된 뒤 lower target 후보를 overlay/CGRA-like/PMF backend로 남길 수 있다

### 9.5 gOIR-placed 내부 pass

허용:

1. verify
2. bridge legality check
3. dead bridge/result prune
4. layout freeze validation
5. cheap constant fold
6. debug/source map normalization

비권장:

1. deep LICM/GVN
2. aggressive vectorization
3. target-specific scheduling
4. full hardware resource binding optimization

### 9.6 gOIR-placed -> lower target

1. `coreOIR` -> `parus.core` 또는 `arith/cf/scf`
2. `cpuOIR` -> `memref/scf/affine/vector/llvm`
3. `gpuOIR` -> `gpu/spirv` 또는 backend-specific GPU lowering lane
4. `hw.struct` -> CIRCT `hw/comb/seq/sv` 또는 동등 structural lane -> Verilog -> FPGA/ASIC
5. `hw.flow` -> overlay IR / CGRA-like backend / future spatial backend
6. `hw.flow` -> PMF backend -> `pmfIR/capsule`
7. `bridgeOIR` -> runtime/async/token/resource/service dialect

### 9.7 legacy lane

1. migration 동안 `SIR -> current OIR -> LLVM`를 유지한다.
2. 이 경로는 CPU reference/backend regression lane이다.
3. overlapping subset에 대해 `legacy OIR`와 `gOIR-placed` 결과를 differential test 한다.

## 10. 컴파일 속도 정책

### 10.1 기대값

1. frontend에서 SIR까지의 시간은 지금과 큰 차이가 없다.
2. `SIR -> gOIR-open`은 현재 `SIR -> OIR`보다 약간 느려질 수 있다.
3. `gOIR-open -> placement -> gOIR-placed`가 새 비용으로 추가된다.
4. CPU cold compile은 `gOIR-placed -> lower target -> LLVM` 경로가 현재 `OIR -> LLVM`보다 대체로 느릴 가능성이 높다.
5. 대신 loop/access/vector 정보를 늦게까지 보존하면 최적화 품질은 좋아질 수 있다.
6. GPU/HW AOT compile은 CPU보다 훨씬 느릴 수 있다.

### 10.2 설계 원칙

1. `coreOIR` verify/canonicalize는 매우 빨라야 한다.
2. `gOIR-open`은 semantic consistency와 placement 준비에 집중한다.
3. `gOIR-placed`는 cheap legality, bridge normalization, archive friendliness에 집중한다.
4. heavy pass는 lower-target 이후 단계로 미룬다.
5. hardware path는 template/cache 중심으로 재사용을 강하게 유도한다.

### 10.3 권장 컴파일 프로파일

1. `dev-fast`
   - minimal open/placed checks
   - shallow placement
   - shallow lower-target pipeline
   - CPU reference lane fallback 허용
2. `balanced`
   - 기본 개발 프로파일
   - 표준 placement + CPU/GPU 일반 최적화
3. `max-opt`
   - 장시간 placement/cost-model
   - 장시간 target-specific optimization
   - hw template search 강화

### 10.4 CPU 경로에 대한 판단

1. CPU만 볼 때 현재 `legacy OIR -> LLVM`이 속도 우위일 가능성이 높다.
2. CPU auto-vectorization, affine loop, structured reduction 품질은 `cpuOIR -> MLIR` 쪽이 더 좋아질 여지가 있다.
3. 따라서 초기에는 두 CPU lane을 병행 유지하는 것이 맞다.

## 11. JIT 확장 정책

hot-reload는 보류한다. 이 문서에서 말하는 JIT는 실행 중 specialization, placement, code emission, cache 재사용을 뜻한다.

### 11.1 CPU JIT

1. 가장 straightforward한 JIT lane이다.
2. runtime specialization된 `gOIR-open` fragment를 만든 뒤 `placement -> gOIR-placed -> MLIR -> LLVM ORC` 또는 동등 lane으로 연결 가능하다.
3. 현재 scaffolded JIT backend의 장기 확장 방향은 이 축이다.

### 11.2 GPU JIT

가능하다. 다만 CPU JIT와 같은 의미의 "즉시 네이티브 코드 생성"이 아니라 driver/toolchain 연계형 JIT다.

형태:

1. runtime specialization된 `gOIR-open` 생성
2. placement로 GPU realization 선택
3. `gOIR-placed -> MLIR -> SPIR-V/PTX/MSL IR`
4. driver/runtime compile 또는 cached binary reuse
5. kernel cache key 기반 재사용

정책:

1. launch shape, dtype, tile size 같은 specialization 인자를 cache key에 넣는다.
2. source-level hot-reload는 다루지 않는다.
3. runtime compile latency가 큰 경우 background build + cache warming을 사용한다.

### 11.3 HW JIT

가능하다. 다만 "arbitrary synthesis JIT"가 아니라 "template-based hardware specialization/runtime configuration" 의미로 제한한다.

권장 범위:

1. 미리 검증된 hardware template 집합 유지
2. runtime은 template 선택, 파라미터 구성, service binding, partial reconfiguration만 수행
3. bitstream/cache reuse를 우선한다

비권장:

1. 범용 RTL 생성 후 완전 합성을 인터랙티브 JIT처럼 매번 수행하는 것
2. 대규모 place-and-route를 요청 경로에 넣는 것

### 11.4 JIT canon 결론

1. CPU JIT는 정식 지원 대상으로 본다.
2. GPU JIT도 정식 지원 대상으로 본다.
3. HW JIT는 "template specialization runtime" 의미로 정식 지원 대상으로 본다.
4. hot-reload는 별도 트랙에서 다룬다.

## 12. canon 결론

1. `gOIR`의 정식 형태는 `gOIR-open`과 `gOIR-placed`의 2단계 모델이다.
2. `gOIR`의 핵심 추상화는 family region 자체가 아니라 `SemanticSig`, `GComputation`, `GRealization`, `GPlacementPolicy`, `GService`다.
3. `bridgeOIR`는 transport-only IR가 아니라 interop/placement/runtime boundary IR다.
4. hardware lane은 `hw.struct`와 `hw.flow`로 정식 분리한다.
5. `hw.struct`는 CIRCT/Verilog/legacy FPGA/ASIC 친화 portable structural lane이다.
6. `hw.flow`는 overlay, CGRA-like backend, PMF backend로 이어질 수 있는 portable spatial/dataflow lane이다.
7. PMF는 `gOIR` family가 아니라 lower target/backend project다.
8. current OIR은 계속 legacy CPU reference lane으로 유지한다.
