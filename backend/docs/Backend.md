<!-- backend/docs/Backend.md -->
# Parus Backend Structure

## 1) 목표

`frontend`는 언어 프론트엔드(AST -> SIR -> OIR)만 담당하고,  
`backend`는 OIR 이후의 타깃별 코드 생성 계층만 담당한다.

이 분리는 다음을 보장한다.

- 프론트엔드와 백엔드 변경의 결합도 감소
- LLVM/JIT/WASM 의존성의 국소화
- 향후 LSP/정적분석 도구와 백엔드 개발의 독립 진행

## 2) 디렉토리 구성

```text
backend/
  CMakeLists.txt
  include/parus/backend/
    Backend.hpp
    aot/
      AOTBackend.hpp
      AOTLLVMDispatcher.hpp
    jit/
      JITBackend.hpp
    wasm/
      WasmBackend.hpp
  src/
    aot/
      CMakeLists.txt
      AOTBackend.cpp
      AOTBackendLLVMDispatch.cpp
    jit/
      CMakeLists.txt
      JITBackend.cpp
    wasm/
      CMakeLists.txt
      WasmBackend.cpp
  llvmconfig/
    CMakeLists.txt
    lanes/
      v20/
        AOTLlvmV20.cpp
      v21/
        AOTLlvmV21.cpp
```

## 3) 핵심 설계 원칙

### 3.1 BackendKind는 3개만 유지

`BackendKind`:

- `kAot`
- `kJit`
- `kWasm`

LLVM은 독립 backend kind가 아니라 **AOT 내부 엔진**으로 취급한다.

### 3.2 AOT 엔진 분리

`AOTEngine`:

- `kLlvm` (현재 기본)
- `kNativeCodegen` (추후 자체 코드젠)

즉, 사용자 관점의 경로는 AOT 1개지만 내부 엔진은 교체 가능하다.

### 3.3 LLVM 의존성 격리

LLVM lane 선택/탐색/버전 가드는 `backend/llvmconfig`에만 둔다.

- `backend/src/aot`는 "Parus AOT 규약"을 관리
- `backend/llvmconfig`는 "LLVM 버전/툴체인 규약"을 관리

이렇게 분리해두면 LLVM 업그레이드 영향이 AOT 핵심 로직으로 과도하게 전파되지 않는다.

## 4) LLVM 버전 지원 정책

현재 lane:

- LLVM 20
- LLVM 21

상위 CMake에서 `PARUS_LLVM_VERSION`은 **20/21만 허용**하며, 그 외 값은 configure 단계에서 즉시 실패한다.

관련 옵션:

- `PARUS_AOT_ENABLE_LLVM` : AOT 내부 LLVM 엔진 활성화
- `PARUS_LLVM_VERSION` : `20` 또는 `21`
- `PARUS_LLVM_REQUIRE_TOOLCHAIN` : 선택 lane의 LLVM toolchain 필수 여부

권장 운영:

- 기본 lane은 안정성이 높은 버전(예: 20)으로 고정
- 차기 lane(예: 21)은 병행 검증 후 기본 lane 교체

## 5) 빌드 흐름 요약

1. Top-level CMake에서 backend module 옵션을 해석
2. `PARUS_ENABLE_AOT_BACKEND && PARUS_AOT_ENABLE_LLVM`일 때 `backend/llvmconfig` 포함
3. `backend/src/aot`는 선택된 lane source를 링크
4. AOT compile 경로에서 `PARUS_LLVM_SELECTED_MAJOR`에 따라 lane dispatch

## 6) 향후 확장 계획

### 6.1 AOT

- OIR -> LLVM IR lowering 본 구현
- target triple/cpu/features 반영
- object/asm/bitcode 출력 경로 정리
- `kNativeCodegen` 엔진 프로토타입 추가

### 6.2 JIT

- ORC JIT 또는 대체 JIT lane 연결
- OIR lowering 재사용 계층 분리
- 런타임 심볼 해석 정책 추가

### 6.3 WASM

- OIR -> WASM IR 또는 중간 추상 계층 설계
- ABI/메모리 모델 정책 정의

## 7) 유지보수 체크리스트

- 새 LLVM lane 추가 시:
  - `backend/llvmconfig/lanes/vXX` 구현
  - `backend/llvmconfig/CMakeLists.txt` lane 허용값 갱신
  - 상위 `CMakeLists.txt`의 `PARUS_LLVM_VERSION` 허용 범위 갱신
- backend core(`backend/include`, `backend/src`)에 LLVM 헤더 직접 확산 금지
- `frontend`와 `backend`의 책임 경계(AST/SIR/OIR vs codegen)를 유지
