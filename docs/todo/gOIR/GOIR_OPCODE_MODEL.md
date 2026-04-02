# Parus gOIR Opcode Model

문서 버전: `draft-0.3`  
상태: `Design Freeze (TODO Track)`

이 문서는 `gOIR`의 실제 표현 계층과 opcode 초안을 고정한다.
이 문서의 opcode 표는 기본적으로 `gOIR-placed` 기준의 canonical 표다.
`gOIR-open`은 semantic invoke와 realization/service 참조를 유지하며,
placed 단계에서 family-specific opcode와 `bridgeOIR`로 materialize된다.

## 1. 기본 원칙

1. `gOIR`의 canonical 단위는 `semantic entity + region + block + op + value + bridge`다.
2. `gOIR-open`은 computation, realization set, semantic invoke를 보존한다.
3. `gOIR-placed`는 selected realization, family-specific call, explicit bridge를 가진다.
4. `coreOIR`, `cpuOIR`, `gpuOIR`, `bridgeOIR`는 기본적으로 SSA + block parameter 모델을 사용한다.
5. `hw.struct`와 `hw.flow`는 hardware lane의 두 개의 정식 family다.
6. `hw.struct`는 structural 중심, `hw.flow`는 spatial/dataflow 중심의 portable lane이다.
7. `phi`는 도입하지 않고 block parameter로 통일한다.
8. 함수 인자는 별도 opcode로 두지 않고 entry block parameter로 모델링한다.
9. open 단계에서 cross-family 의미 연결은 direct family call로 표현하지 않는다.
10. placed 단계에서 cross-family 이동은 반드시 explicit bridge를 통해 일어난다.
11. 일반 side-effect는 block 순서와 effect metadata로 모델링한다.
12. async/synchronization 경계는 explicit `token/event` 값으로 모델링한다.
13. structured control op는 child region을 가진다.

## 2. 공통 타입 계열

### 2.1 공통 scalar

1. `i8/i16/i32/i64`
2. `u8/u16/u32/u64`
3. `f16/f32/f64`
4. `bool`
5. `index`

### 2.2 공통 aggregate

1. `tuple<T...>`
2. `array<N, T>`
3. `vec<N, T>`
4. `mask<N>`
5. `shape<R>`

### 2.3 family-specific type

1. `cpu.ptr<T>`
2. `cpu.memref<T, rank>`
3. `gpu.ptr<addrspace, T>`
4. `gpu.buffer<T, rank>`
5. `gpu.image<dim, T>`
6. `gpu.sampler`
7. `gpu.token`
8. `gpu.event`
9. `hw.bits<N>`
10. `hw.uint<N>`
11. `hw.sint<N>`
12. `hw.clock`
13. `hw.reset`
14. `hw.channel<T, protocol>`
15. `hw.window<T, rank>`
16. `bridge.event`
17. `bridge.template`

### 2.4 semantic/placement support type

1. `service.ref`
2. `placement.policy`
3. `resource.set`

## 3. 공통 표현 규칙

### 3.1 value와 effect

1. 모든 SSA 결과는 정확히 하나의 `type_id`를 가진다.
2. effectful op는 `flags`와 effect metadata를 통해 메모리, 동기화, residency 성격을 드러낸다.
3. async op는 반드시 `token/event` 결과를 반환하거나 소비해야 한다.
4. verifier는 family별로 허용 타입과 effect 조합을 검사한다.

### 3.2 stage-specific invoke 규칙

1. `gOIR-open`은 semantic invoke를 유지한다.
2. semantic invoke는 computation symbol, optional placement policy override, service mediation intent를 참조할 수 있다.
3. `gOIR-open`의 semantic invoke는 아직 `cpu/gpu/hw.struct/hw.flow` direct call로 고정되지 않는다.
4. `gOIR-placed`에서 semantic invoke는 `CallPure`, `CallHost`, `CallDevice`, hardware family-local invoke form, `bridge.*` 중 하나 또는 그 조합으로 materialize된다.
5. service-style interop는 open 단계에서 semantic request로 표현하고, placed 단계에서 `bridge.service_req`/`bridge.service_reply`로 concretize한다.

### 3.3 structured op

다음 부류는 child region을 가질 수 있다.

1. `cpu.for`, `cpu.if`, `cpu.reduce`
2. `gpu.kernel`
3. `hw.struct.module`, `hw.struct.comb`, `hw.struct.seq`
4. `hw.flow.pipeline`, `hw.flow.stage`, `hw.flow.task`
5. `bridge.launch_gpu`, `bridge.invoke_hw`, `bridge.service_req`의 callback-style completion region

### 3.4 textual dump 형태

권장 dump 표기는 아래 스타일을 따른다.

```text
%out = exec.invoke @matmul(%a, %b, %c) policy(@auto_tile) : tuple<gpu, hw.flow, cpu>
%3 = core.addi %1, %2 : i32
%7 = cpu.for %lb, %ub, %step iter(%acc = %init) -> i32 { ... }
%tid = gpu.global_id.x : index
%m = hw.struct.module @accel { ... }
%p = hw.flow.pipeline iter(%tok = %in) -> hw.channel<i32, ready_valid> { ... }
%tok1 = bridge.launch_gpu @matmul.gpu(%bufA, %bufB, %bufC) grid(%gx, %gy, %gz) block(%bx, %by, %bz)
```

## 4. coreOIR

`coreOIR`는 가장 작은 공통 계산 IR이다.
메모리, runtime, address space, clock, barrier는 포함하지 않는다.
아래 표는 `gOIR-placed`에서 쓰이는 canonical core opcode를 정의한다.

### 4.1 core 표현 범위

1. scalar/aggregate 계산
2. placed-stage pure local call
3. branch/switch/return
4. shape/index helper

### 4.2 core opcode

```cpp
enum class CoreOpcode : uint16_t {
    ConstI, ConstF, ConstB, ConstIndex, Undef,
    AddI, SubI, MulI, DivS, DivU, RemS, RemU,
    AddF, SubF, MulF, DivF, NegF,
    And, Or, Xor, Shl, LShr, AShr,
    ICmpEq, ICmpNe, ICmpSLt, ICmpSLe, ICmpSGt, ICmpSGe,
    ICmpULt, ICmpULe, ICmpUGt, ICmpUGe,
    FCmpOEq, FCmpONe, FCmpOLt, FCmpOLe, FCmpOGt, FCmpOGe,
    Trunc, ZExt, SExt, FPTrunc, FPExt, SIToFP, UIToFP, FPToSI, FPToUI, Bitcast,
    Select,
    TupleMake, TupleGet,
    ArrayMake, ArrayGet,
    ShapeOf, Dim,
    CallPure,
    Br, CondBr, Switch, Ret, Unreachable
};
```

### 4.3 core verifier 규칙

1. `CallPure`는 `gOIR-placed`에서만 나타난다.
2. `CallPure`는 이미 선택된 computation realization 또는 placed-stage pure helper만 호출할 수 있다.
3. memory effect가 있는 op는 금지한다.
4. `vec`는 값 타입으로는 허용하되, vector memory op는 허용하지 않는다.

### 4.4 core opcode 상세 표

표기:

1. `T`: 동일 타입
2. `I`: 정수 또는 `index`
3. `SInt/UInt`: signed/unsigned 정수
4. `F`: 부동소수
5. `V<N,T>`: 길이 `N`의 벡터
6. `mask<N>`: 길이 `N`의 마스크
7. `Agg`: `tuple`/`array`
8. `Shaped`: `array`/`vec`/shape-aware 값

| Opcode | Operands | Result | Attrs | Verifier |
| --- | --- | --- | --- | --- |
| `ConstI` | `-` | `SInt/UInt/index` | `value` | 결과 타입이 정수 또는 `index`여야 하고 literal이 폭에 맞아야 한다 |
| `ConstF` | `-` | `F` | `value` | 결과 타입이 float여야 한다 |
| `ConstB` | `-` | `bool` | `value` | 결과 타입이 `bool`이어야 한다 |
| `ConstIndex` | `-` | `index` | `value` | 결과 타입이 `index`여야 한다 |
| `Undef` | `-` | `T` | `-` | `T`는 first-class pure value여야 한다 |
| `AddI` | `%a:T, %b:T` | `T` | `-` | `T`는 정수/`index` 또는 그 lane-wise vector여야 한다 |
| `SubI` | `%a:T, %b:T` | `T` | `-` | `T` 동일, 정수 계열이어야 한다 |
| `MulI` | `%a:T, %b:T` | `T` | `-` | `T` 동일, 정수 계열이어야 한다 |
| `DivS` | `%a:T, %b:T` | `T` | `-` | `T`는 signed int lane 타입이어야 하며 `index`는 금지한다 |
| `DivU` | `%a:T, %b:T` | `T` | `-` | `T`는 unsigned int lane 타입이어야 하며 `index`는 금지한다 |
| `RemS` | `%a:T, %b:T` | `T` | `-` | `T`는 signed int lane 타입이어야 한다 |
| `RemU` | `%a:T, %b:T` | `T` | `-` | `T`는 unsigned int lane 타입이어야 한다 |
| `AddF` | `%a:T, %b:T` | `T` | `fastmath?` | `T`는 float 또는 float vector여야 한다 |
| `SubF` | `%a:T, %b:T` | `T` | `fastmath?` | `T` 동일, float 계열이어야 한다 |
| `MulF` | `%a:T, %b:T` | `T` | `fastmath?` | `T` 동일, float 계열이어야 한다 |
| `DivF` | `%a:T, %b:T` | `T` | `fastmath?` | `T` 동일, float 계열이어야 한다 |
| `NegF` | `%x:T` | `T` | `fastmath?` | `T`는 float 또는 float vector여야 한다 |
| `And` | `%a:T, %b:T` | `T` | `-` | `T`는 int/bool/mask 또는 그 vector-compatible 형이어야 한다 |
| `Or` | `%a:T, %b:T` | `T` | `-` | `T`가 동일해야 한다 |
| `Xor` | `%a:T, %b:T` | `T` | `-` | `T`가 동일해야 한다 |
| `Shl` | `%a:T, %b:T` | `T` | `-` | `T`는 정수 lane 타입이어야 하며 shift 수 타입도 lane 호환이어야 한다 |
| `LShr` | `%a:T, %b:T` | `T` | `-` | `T`는 unsigned/int lane 타입이어야 한다 |
| `AShr` | `%a:T, %b:T` | `T` | `-` | `T`는 signed int lane 타입이어야 한다 |
| `ICmpEq` | `%a:T, %b:T` | `bool \| mask<N>` | `-` | `T`는 int/index lane 타입이어야 하며 결과 shape는 operand shape와 일치해야 한다 |
| `ICmpNe` | `%a:T, %b:T` | `bool \| mask<N>` | `-` | `T`는 int/index lane 타입이어야 하며 결과 shape는 operand shape와 일치해야 한다 |
| `ICmpSLt` | `%a:T, %b:T` | `bool \| mask<N>` | `-` | `T`는 signed int lane 타입이어야 한다 |
| `ICmpSLe` | `%a:T, %b:T` | `bool \| mask<N>` | `-` | `T`는 signed int lane 타입이어야 한다 |
| `ICmpSGt` | `%a:T, %b:T` | `bool \| mask<N>` | `-` | `T`는 signed int lane 타입이어야 한다 |
| `ICmpSGe` | `%a:T, %b:T` | `bool \| mask<N>` | `-` | `T`는 signed int lane 타입이어야 한다 |
| `ICmpULt` | `%a:T, %b:T` | `bool \| mask<N>` | `-` | `T`는 unsigned/int/index lane 타입이어야 한다 |
| `ICmpULe` | `%a:T, %b:T` | `bool \| mask<N>` | `-` | `T`는 unsigned/int/index lane 타입이어야 한다 |
| `ICmpUGt` | `%a:T, %b:T` | `bool \| mask<N>` | `-` | `T`는 unsigned/int/index lane 타입이어야 한다 |
| `ICmpUGe` | `%a:T, %b:T` | `bool \| mask<N>` | `-` | `T`는 unsigned/int/index lane 타입이어야 한다 |
| `FCmpOEq` | `%a:T, %b:T` | `bool \| mask<N>` | `fastmath?` | `T`는 float lane 타입이어야 한다 |
| `FCmpONe` | `%a:T, %b:T` | `bool \| mask<N>` | `fastmath?` | `T`는 float lane 타입이어야 한다 |
| `FCmpOLt` | `%a:T, %b:T` | `bool \| mask<N>` | `fastmath?` | `T`는 float lane 타입이어야 한다 |
| `FCmpOLe` | `%a:T, %b:T` | `bool \| mask<N>` | `fastmath?` | `T`는 float lane 타입이어야 한다 |
| `FCmpOGt` | `%a:T, %b:T` | `bool \| mask<N>` | `fastmath?` | `T`는 float lane 타입이어야 한다 |
| `FCmpOGe` | `%a:T, %b:T` | `bool \| mask<N>` | `fastmath?` | `T`는 float lane 타입이어야 한다 |
| `Trunc` | `%x:Tsrc` | `Tdst` | `-` | int lane 폭 축소만 허용하며 lane shape는 동일해야 한다 |
| `ZExt` | `%x:Tsrc` | `Tdst` | `-` | unsigned/int lane 확장만 허용하며 lane shape는 동일해야 한다 |
| `SExt` | `%x:Tsrc` | `Tdst` | `-` | signed int lane 확장만 허용하며 lane shape는 동일해야 한다 |
| `FPTrunc` | `%x:Tsrc` | `Tdst` | `-` | float lane 폭 축소만 허용한다 |
| `FPExt` | `%x:Tsrc` | `Tdst` | `-` | float lane 폭 확장만 허용한다 |
| `SIToFP` | `%x:Tsrc` | `Tdst` | `-` | signed int lane -> float lane 변환만 허용하며 lane shape는 동일해야 한다 |
| `UIToFP` | `%x:Tsrc` | `Tdst` | `-` | unsigned int lane -> float lane 변환만 허용한다 |
| `FPToSI` | `%x:Tsrc` | `Tdst` | `-` | float lane -> signed int lane 변환만 허용한다 |
| `FPToUI` | `%x:Tsrc` | `Tdst` | `-` | float lane -> unsigned int lane 변환만 허용한다 |
| `Bitcast` | `%x:Tsrc` | `Tdst` | `-` | bit-width가 동일해야 하며 pure representation cast만 허용한다 |
| `Select` | `%cond, %a:T, %b:T` | `T` | `-` | `%cond`는 `bool` 또는 `mask<N>`이어야 하며 `%a/%b` 타입은 동일해야 한다 |
| `TupleMake` | `%x0..%xn` | `tuple<...>` | `-` | 결과 tuple 항목 타입이 operand와 정확히 일치해야 한다 |
| `TupleGet` | `%agg` | `T` | `index` | `%agg`가 tuple이어야 하고 `index` 범위가 유효해야 한다 |
| `ArrayMake` | `%x0..%xn` | `array<N,T>` | `-` | 모든 operand 타입이 동일해야 하며 `N`과 개수가 일치해야 한다 |
| `ArrayGet` | `%arr, %idx` | `T` | `-` | `%arr`가 `array<N,T>`여야 하고 `%idx`는 `index` 또는 정수여야 한다 |
| `ShapeOf` | `%x` | `shape<R>` | `-` | `%x`가 shaped type이어야 한다 |
| `Dim` | `%x` | `index` | `axis` | `%x`가 `shape<R>` 또는 shaped type이어야 하며 `axis < R`이어야 한다 |
| `CallPure` | `%arg0..%argn` | `0..1` | `callee_sym`, `abi_sig` | placed 단계에서만 허용되며 callee가 pure realization/helper여야 하고 arg/result 타입이 시그니처와 일치해야 한다 |
| `Br` | `%arg0..%argn` | `-` | `target_bb` | target block param 개수/타입과 인자가 일치해야 한다 |
| `CondBr` | `%cond, %then_args..., %else_args...` | `-` | `then_bb`, `else_bb` | `%cond`는 `bool`이어야 하며 각 분기 인자가 대상 block param과 일치해야 한다 |
| `Switch` | `%cond` | `-` | `cases`, `targets`, `default_bb` | `%cond`는 scalar int/index/tag여야 하며 case 값 중복은 금지한다 |
| `Ret` | `0..1 value` | `-` | `-` | 함수 결과 타입과 정확히 일치해야 한다 |
| `Unreachable` | `-` | `-` | `-` | terminator 위치에서만 허용한다 |

## 5. cpuOIR

`cpuOIR`는 CPU 최적화 친화 IR이다.
현재 legacy OIR처럼 너무 빨리 `alloca/load/store`만 남는 구조를 피하고, loop/access/vector 정보를 오래 유지한다.
아래 표는 `gOIR-placed`에서 쓰이는 canonical CPU family opcode를 정의한다.

### 5.1 cpu 표현 범위

1. structured loop / if / reduction
2. host memory access
3. vector primitive
4. placed-stage direct host call / intrinsic / runtime bridge
5. alignment/noalias/access metadata

추가 canonical 결정:

1. 함수 인자는 별도 `FuncArg` opcode로 두지 않고 entry block parameter로 모델링한다.
2. `While`은 canonical opcode로 두지 않고 `If + Loop/For`로 정규화한다.
3. child region이 값을 내보낼 때는 `Yield`를 사용한다.
4. `AllocaStack`은 허용하되 late form으로 제한한다.

### 5.2 cpu opcode

```cpp
enum class CpuOpcode : uint16_t {
    CallHost, CallIntrinsic,
    If, For, Reduce, Scan, Yield,
    AllocaStack, AddrOf, FieldAddr, IndexAddr,
    Load, Store, MemCpy, MemSet, Prefetch,
    SubView, ReinterpretView,
    VectorSplat, VectorBroadcast, VectorExtract, VectorInsert,
    VectorLoad, VectorStore, MaskedLoad, MaskedStore,
    Shuffle, Blend, FMA,
    AssumeAligned, AssumeNoAlias,
    Return
};
```

### 5.3 cpu 구조 op 의미

1. `If`
   - child region 2개: then, else
   - 결과는 region yield 타입으로 정의
2. `For`
   - operands: `lb, ub, step`
   - iter arg를 block parameter로 받는다
   - child region 1개: loop body
3. `Reduce`
   - reduction kind attr 또는 combiner child region을 가진다
4. `Scan`
   - prefix-scan 전용 구조 op다
5. `Yield`
   - structured child region의 terminator다
   - parent op의 result 타입과 일치해야 한다

### 5.4 cpu 메모리 규칙

1. 주소 계산과 load/store를 분리한다.
2. address op는 pure다.
3. `Load/Store`는 memory effect를 가진다.
4. `cpu.memref`와 `cpu.ptr`는 verifier가 alias class와 alignment를 함께 본다.

### 5.5 cpu opcode 상세 표

추천 freeze v0에서는 `FuncArg`와 `While`를 canonical opcode로 두지 않는다.

| Opcode | Operands | Result | Attrs | Verifier |
| --- | --- | --- | --- | --- |
| `CallHost` | `%arg0..%argn` | `0..1` | `callee_sym`, `abi_sig`, `tail?`, `noreturn?`, `readonly?` | placed 단계에서만 허용되며 CPU-legal 타입만 허용하고 시그니처와 arg/result가 일치해야 한다 |
| `CallIntrinsic` | `%arg0..%argn` | `0..1` | `intrinsic_id`, `fastmath?` | `intrinsic_id`가 유효해야 하고 타입 조합이 intrinsic 규약과 일치해야 한다 |
| `If` | `%cond` | `0..N` | `-` | `%cond`는 `bool`이어야 하며 `then/else` child region 모두 `Yield`로 끝나고 yield 타입이 동일해야 한다 |
| `For` | `%lb, %ub, %step, %iter0..%itern` | `0..N` | `parallel_hint?`, `unroll?`, `vectorize?` | `lb/ub/step`은 `index`여야 하며 body 첫 param은 induction `index`, 나머지 iter-carried 타입은 result와 일치해야 한다 |
| `Reduce` | `%input, %init` | `T` 또는 reduced-shaped | `axes`, `kind?`, `parallelizable?` | `%input`은 shaped/vector여야 하며 `axes`가 유효해야 하고 builtin `kind`가 없으면 combiner region 시그니처가 `(T,T)->T`여야 한다 |
| `Scan` | `%input, %init` | shaped result | `axis`, `inclusive|exclusive`, `kind` | `%input`은 shaped type이어야 하며 `axis`가 유효하고 결과 shape는 입력과 동일해야 한다 |
| `Yield` | `%v0..%vn` | `-` | `-` | structured child region terminator에서만 허용되며 parent op 기대 타입과 일치해야 한다 |
| `Return` | `0..1 value` | `-` | `-` | 함수 결과 타입과 일치해야 한다 |
| `AllocaStack` | `-` | `cpu.memref<T,0>` | `alignment?`, `lifetime` | late form으로만 허용하며 resource/gpu/hw 타입은 금지하고 escaping은 허용하지 않는다 |
| `AddrOf` | `-` | `cpu.ptr<T>` 또는 `cpu.memref<T,R>` | `symbol_ref` | symbol이 존재해야 하며 CPU 주소공간에서 접근 가능해야 한다 |
| `FieldAddr` | `%base` | same addr-kind | `field_index` | `%base`가 aggregate address/view여야 하고 field index가 유효해야 한다 |
| `IndexAddr` | `%base, %i0..%ik` | same addr-kind | `inbounds?` | index 개수와 rank/aggregate depth가 일치해야 한다 |
| `Load` | `%base, %i0..%ik` | `T` | `alignment?`, `volatile?`, `nontemporal?` | `%base`가 load 가능한 ptr/memref여야 하며 index 개수가 유효해야 한다 |
| `Store` | `%base, %i0..%ik, %value` | `-` | `alignment?`, `volatile?`, `nontemporal?` | `%value` 타입이 pointee/element 타입과 일치해야 한다 |
| `MemCpy` | `%dst, %src, %len` | `-` | `dst_align?`, `src_align?`, `volatile?` | `%dst/%src`는 byte-addressable이어야 하고 `%len`은 `index` 또는 정수여야 하며 overlap 정책이 명시돼야 한다 |
| `MemSet` | `%dst, %byte, %len` | `-` | `dst_align?`, `volatile?` | `%byte`는 byte-int여야 하며 `%dst`는 byte-addressable이어야 한다 |
| `Prefetch` | `%base, %i0..%ik` | `-` | `rw`, `locality`, `cache_type` | 힌트 op이며 결과가 없고 주소 타입만 검사한다 |
| `SubView` | `%base, %offsets..., %sizes..., %strides...` | `cpu.memref<T,R2>` | `-` | base가 memref여야 하며 rank/offset-size-stride 개수가 일치하고 결과 layout이 일관돼야 한다 |
| `ReinterpretView` | `%base` | `cpu.memref<T2,R2>` | `offset`, `sizes`, `strides` | byte size/stride 일관성을 만족해야 한다 |
| `VectorSplat` | `%x:T` | `V<N,T>` | `lanes` | scalar 입력만 허용하며 `lanes > 0`이어야 한다 |
| `VectorBroadcast` | `%x` | `V<N,T>` | `broadcast_dims` | scalar 또는 더 낮은 rank/vector에서만 허용하며 차원 매핑이 유효해야 한다 |
| `VectorExtract` | `%vec, %idx...` | `T` | `static_index?` | `%vec`가 vector여야 하고 index 수/범위가 유효해야 한다 |
| `VectorInsert` | `%val, %vec, %idx...` | `V<N,T>` | `static_index?` | `%val` lane 타입이 vector element와 일치해야 한다 |
| `VectorLoad` | `%base, %i0..%ik` | `V<N,T>` | `alignment?`, `masking_policy?` | base가 contiguous/compatible element layout이어야 한다 |
| `VectorStore` | `%base, %i0..%ik, %vec` | `-` | `alignment?`, `masking_policy?` | `%vec` element 타입과 memory element 타입이 일치해야 한다 |
| `MaskedLoad` | `%base, %i0..%ik, %mask, %passthru` | `V<N,T>` | `alignment?` | `%mask`는 `mask<N>`이어야 하고 `%passthru`는 `V<N,T>`여야 하며 lane 수가 일치해야 한다 |
| `MaskedStore` | `%base, %i0..%ik, %mask, %vec` | `-` | `alignment?` | `%mask`와 `%vec`의 lane 수가 일치해야 한다 |
| `Shuffle` | `%a, %b` | `V<N,T>` | `lane_map` | `%a/%b` 타입이 동일해야 하고 `lane_map` 길이와 범위가 유효해야 한다 |
| `Blend` | `%a, %b, %mask` | `V<N,T>` | `-` | `%a/%b` 타입이 동일해야 하고 `%mask`는 `mask<N>`이어야 한다 |
| `FMA` | `%a, %b, %c` | `T` | `fastmath?` | `T`는 float 또는 float vector여야 하며 세 operand 타입이 동일해야 한다 |
| `AssumeAligned` | `%base` | same as input | `alignment`, `offset?` | `alignment`가 power-of-two여야 하고 입력은 addressable CPU 값이어야 한다 |
| `AssumeNoAlias` | `%v0..%vn` | `-` | `scope_id`, `domain_id?` | 두 개 이상의 addressable 값이 필요하며 scope/domain 일관성이 유지돼야 한다 |

### 5.6 cpu 성능 강화용 future 후보

다음은 v0 canonical에 즉시 넣지 않지만 강한 후보로 남긴다.

1. `VectorReduce`
2. `Gather`
3. `Scatter`
4. `AssumeContiguous`
5. `RangeHint`
6. `AffineApply`
7. `LayoutCast`
8. `Transpose`

### 5.7 v0 범위 밖

1. actor
2. recoverable exception
3. class RAII
4. borrow/escape 특수 semantics

이 부류는 초기 `gOIR` 주경로에 강제로 넣지 않으며, 필요하면 future `rtOIR` family를 도입한다.

## 6. gpuOIR

`gpuOIR`는 placed-stage device realization family다.
open 단계에서는 GPU를 direct launch 대상으로 보지 않고 candidate realization으로만 유지한다.
placed 단계에서 선택된 GPU realization이 `gpuOIR` region과 `bridge.launch_gpu` 계열로 materialize된다.

### 6.1 gpu 표현 범위

1. kernel entry
2. builtin ids / launch geometry
3. address-space-qualified memory
4. barrier/fence
5. async copy
6. subgroup primitive
7. image/buffer/framebuffer resource op
8. placed-stage device-local call

### 6.2 gpu placement/service 영향

1. launch 자체는 `gpuOIR`가 아니라 `bridgeOIR`가 담당한다.
2. service-mediated GPU cooperation은 `bridge.service_req`/`bridge.service_reply`를 통해 표현한다.
3. residency, queue, present, host callback 같은 orchestration은 `bridgeOIR`에 남긴다.
4. 아래 opcode 집합은 placed-stage device body 표현의 representative set이며, open 단계의 semantic invoke를 직접 대체하지 않는다.

### 6.3 gpu opcode

```cpp
enum class GpuOpcode : uint16_t {
    Kernel,
    GlobalIdX, GlobalIdY, GlobalIdZ,
    LocalIdX, LocalIdY, LocalIdZ,
    GroupIdX, GroupIdY, GroupIdZ,
    GlobalSizeX, GlobalSizeY, GlobalSizeZ,
    LocalSizeX, LocalSizeY, LocalSizeZ,
    NumGroupsX, NumGroupsY, NumGroupsZ,
    LoadGlobal, StoreGlobal,
    LoadShared, StoreShared,
    LoadConst,
    AtomicAdd, AtomicSub, AtomicMin, AtomicMax, AtomicCAS,
    BarrierWorkgroup, BarrierDevice, MemFence,
    AsyncCopyG2S, AsyncCopyS2G, WaitAsync,
    BufferSubView,
    ImageLoad, ImageStore, ImageSample,
    FramebufferLoad, FramebufferStore,
    SubgroupShuffle, SubgroupBroadcastFirst, SubgroupReduceAdd,
    Ballot, VoteAny, VoteAll,
    CallDevice,
    ReturnKernel
};
```

### 6.4 gpu verifier 규칙

1. `CallDevice`는 `gOIR-placed`에서만 허용한다.
2. `gpuOIR` 안에서는 `actor/class/throw/manual`류 op를 금지한다.
3. 모든 pointer-like 타입은 address space가 명시돼야 한다.
4. `Barrier*`는 workgroup legality를 검사한다.
5. `Framebuffer*` op는 graphics-capable target profile에서만 허용한다.

## 7. hw.struct

`hw.struct`는 placed-stage structural hardware family다.
이 family는 기존 structural HW IR과 가장 자연스럽게 연결되는 portable lane이다.

### 7.1 hw.struct 표현 범위

1. hardware module hierarchy
2. combinational dataflow
3. sequential state
4. memory macro
5. instance-based composition

### 7.2 hw.struct placement 영향

1. open 단계에서는 structural hardware 자체로 고정하지 않고 candidate realization으로만 유지한다.
2. placed 단계에서 structural path가 선택되면 `bridge.invoke_hw` 또는 structural export lane과 연결된다.
3. `hw.struct`는 CIRCT/Verilog/legacy FPGA/ASIC flow의 기준점이다.
4. capsule, warm/cold activation, context bank 같은 PMF-specific 개념은 이 family에 올리지 않는다.

### 7.3 hw.struct opcode

```cpp
enum class HwStructOpcode : uint16_t {
    Module, InputPort, OutputPort, InoutPort, Output,
    Instance,
    ConstBits,
    CombAnd, CombOr, CombXor, CombNot,
    CombAdd, CombSub, CombMul,
    CombShl, CombLShr, CombAShr,
    CombEq, CombLtU, CombLtS,
    CombMux, CombConcat, CombSlice,
    Wire, Assign,
    Reg, RegNext,
    CombRegion, SeqRegion, When, Case,
    Mem, MemRead, MemWrite
};
```

### 7.4 hw.struct 구조 규칙

1. `Module`은 child region으로 본문을 가진다.
2. `CombRegion`은 side-effect 없는 조합 논리만 허용한다.
3. `SeqRegion`은 `clock/reset`이 명시된 상태 op만 허용한다.
4. `Reg`와 `RegNext`는 분리해, current-state와 next-state 연결을 명확히 한다.
5. structural lane은 portable structural semantics를 유지해야 하며 backend-specific activation semantics를 담지 않는다.

## 8. hw.flow

`hw.flow`는 placed-stage portable spatial/dataflow hardware family다.
이 family는 PMF 전용 IR가 아니며, FPGA overlay나 future spatial backend도 받을 수 있는 공통 lane이다.

### 8.1 hw.flow 표현 범위

1. stream/channel handshake
2. pipeline/stage
3. task/dataflow control
4. window/buffer residency-friendly flow
5. service endpoint cooperation
6. spatial locality와 steady-state throughput을 오래 보존하는 flow

### 8.2 hw.flow placement 영향

1. open 단계에서는 `hw.flow`를 direct backend target으로 고정하지 않는다.
2. placed 단계에서 `hw.flow` realization은 overlay backend, CGRA-like backend, PMF backend 중 하나로 내려갈 수 있다.
3. `hw.flow`는 PMF-specific activation semantics를 직접 표현하지 않는다.
4. PMF-specific lowering은 `hw.flow -> PMF backend -> pmfIR/capsule`에서만 등장한다.

### 8.3 hw.flow representative op class

`hw.flow`는 이번 단계에서 full opcode freeze까지는 가지 않는다.
대신 canonical 표현 범위와 representative op class를 아래처럼 고정한다.

1. `Task`
   - flow task 또는 dataflow kernel 경계
2. `Pipeline`
   - steady-state pipeline region
3. `Stage`
   - pipeline stage boundary
4. `ChannelSend`
   - channel/stream write
5. `ChannelRecv`
   - channel/stream read
6. `WindowSubView`
   - buffer/window residency-friendly local view
7. `WindowBind`
   - flow-local window binding
8. `ServiceEndpoint`
   - service cooperation 경계
9. `ServiceCall`
   - service endpoint 요청
10. `FlowYield`
   - structured flow region terminator

### 8.4 hw.flow 규칙

1. channel/stream은 handshake protocol 또는 equivalent flow contract를 가져야 한다.
2. pipeline/stage는 ordering과 steady-state intent를 드러내야 한다.
3. window/buffer 표현은 direct structural memory macro보다 locality/residency 친화 의미를 우선한다.
4. service cooperation은 direct host call보다 endpoint/bridge 계약을 우선한다.
5. `hw.flow`는 PMF 전용이 아니므로 capsule/context/pin/evict 같은 개념을 직접 갖지 않는다.

## 9. bridgeOIR

`bridgeOIR`는 host/device/hardware 경계를 표현하는 placed-stage boundary family다.
이 family는 transport-only 계층이 아니라 interop, placement, runtime boundary를 담당한다.

### 9.1 bridge opcode

```cpp
enum class BridgeOpcode : uint16_t {
    SelectRealization, FallbackRealization,
    LaunchGpu,
    WaitGpu,
    CopyH2D, CopyD2H, CopyD2D,
    MapBuffer, UnmapBuffer,
    BindImage,
    AcquireFramebuffer, PresentFramebuffer,
    MigrateResidency,
    ServiceReq, ServiceReply,
    MakeEvent, SignalEvent, WaitEvent, JoinToken,
    SelectHwTemplate, ConfigureHwTemplate, InvokeHwTemplate, WaitHw
};
```

### 9.2 bridge verifier 규칙

1. `SelectRealization`과 `FallbackRealization`은 computation/realization 관계를 따라야 한다.
2. `LaunchGpu`는 `gpuOIR::Kernel` realization만 대상으로 한다.
3. `InvokeHwTemplate`는 검증된 `hw.struct::Module`, `hw.flow` realization, 또는 cached hardware realization만 대상으로 한다.
4. `ServiceReq`와 `ServiceReply`는 등록된 `GService`를 참조해야 한다.
5. copy op는 source/destination layout profile과 residency를 함께 검사한다.
6. async bridge op는 반드시 `token/event`를 생성하거나 소비해야 한다.
7. PMF capsule activation 세부는 이 family의 canonical 범위 밖이다.

## 10. future family reserve

`gOIR`는 확장 가능성을 열어둔다.
다만 v0 canon은 아래 family를 reserved slot으로만 남긴다.

1. `rtOIR`
   - actor/exception/class/runtime-heavy semantics
2. `simOIR`
   - simulator/trace/functional timing model
3. `graphOIR`
   - larger compute graph / scheduling IR

## 11. 추천 구현 순서

1. `SemanticSig`, `GComputation`, `GRealization`, `GPlacementPolicy`, `GService`
2. `gOIR-open`의 semantic invoke와 `gOIR-placed` materialization 규칙
3. `coreOIR` opcode와 verifier
4. `cpuOIR`의 `If/For/Reduce/Load/Store/Vector*`
5. `bridgeOIR`의 `SelectRealization/LaunchGpu/ServiceReq/Copy*/Event`
6. `gpuOIR`의 kernel + memory + barrier + builtin ids
7. `hw.struct`의 `Module/Port/Reg/Comb/Seq`
8. `hw.flow`의 task/pipeline/channel/window/service class
9. 이후 overlay backend, `hw.flow -> PMF backend` lower-target refinement
