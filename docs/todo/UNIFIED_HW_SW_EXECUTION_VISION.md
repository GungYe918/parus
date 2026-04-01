# Parus Unified HW/SW Execution Vision

문서 버전: `draft-0.1`  
상태: `Vision Freeze (TODO Track)`

이 문서는 Parus의 초장기 비전인 "하드웨어와 소프트웨어를 하나의 계산 모델 아래에서 다루는 실행환경"을 고정한다.
본 문서는 구체 opcode나 구현 세부보다 방향, 범위, 성공 기준을 정의한다.

## 1. 핵심 비전

Parus의 장기 목표는 소프트웨어가 고정된 하드웨어 위에서만 실행되는 구조를 넘어서,
계산을 공통 의미로 기술하고 그 일부를 CPU, GPU, 재구성 가능 하드웨어, future programmable hardware에 걸쳐 배치할 수 있는 통합 실행환경을 만드는 것이다.

핵심 문장:

> 계산은 하나의 프로그램 의미를 가지되, 그 실현 형태(realization)는 CPU, GPU, HW를 오가며 선택될 수 있어야 한다.

## 2. 문제 인식

현대 컴퓨팅 스택은 대체로 아래 제약을 가진다.

1. CPU, GPU, FPGA, ASIC가 서로 다른 언어와 툴체인으로 분리돼 있다.
2. 소프트웨어와 하드웨어 사이의 경계가 너무 두껍다.
3. 하드웨어 특화는 강력하지만 개발, 디버깅, 배포, 검증 비용이 크다.
4. GPU 중심 모델은 강력하지만 모든 workload에 최적은 아니다.
5. 특정 계산을 더 적합한 실행 자원에 자연스럽게 옮기기 어렵다.

Parus는 이 문제를 "새 HDL 하나"로 푸는 것이 아니라, 공통 언어/IR/ABI/런타임 계약을 통해 완화하는 방향을 택한다.

## 3. 장기 그림

장기적으로 Parus는 아래 그림을 지향한다.

```text
source program
-> semantic IR
-> multi-realization IR
-> runtime / compiler placement
-> CPU | GPU | reconfigurable HW | future programmable silicon
```

여기서 중요한 점은 다음과 같다.

1. 계산의 의미와 실행 배치는 분리된다.
2. 동일한 계산이 여러 realization을 가질 수 있다.
3. host code와 device/HW code는 같은 파일과 같은 프로그램 안에 공존할 수 있다.
4. 경계는 숨기지 않고 explicit ABI/bridge로 드러낸다.
5. 미래 하드웨어가 더 유연해질수록 Parus는 그 위에서 더 깊은 혼합 실행을 허용할 수 있어야 한다.

## 4. Parus의 역할

Parus는 아래 셋을 동시에 담당하는 substrate를 지향한다.

1. 계산 구조를 드러내는 언어
2. CPU/GPU/HW 경계를 잇는 IR/ABI 계층
3. placement, specialization, bridge legality를 관리하는 실행 계약

Parus는 초장기적으로 "모든 하드웨어를 자동 생성하는 마법 언어"를 목표로 하지 않는다.
대신 아래 성질을 갖는 언어/컴파일러가 되는 것을 목표로 한다.

1. 같은 계산을 여러 실행 모델로 내릴 수 있다.
2. 특정 계산 영역을 제한된 조건 아래 HW realization으로 전환할 수 있다.
3. 실행환경이 비용 모델과 자원 상태를 보고 적절한 realization을 선택할 수 있다.
4. high-level 코드와 low-level 실행 구조를 추적 가능하게 연결한다.

## 5. realization 모델

Parus의 미래 모델에서 중요한 것은 "함수 호출의 통일"보다 "계산 의미의 통일"이다.

Parus는 장기적으로 아래 형태를 허용할 수 있어야 한다.

1. 하나의 계산이 `cpu`, `gpu`, `hw` realization을 동시에 가진다.
2. host는 적절한 realization을 선택하거나 런타임에 선택을 위임한다.
3. 계산의 일부 region만 HW로 분리되고 나머지는 CPU에 남을 수 있다.
4. HW 쪽은 단순 템플릿 선언을 넘어서 host service, shared memory, stream/channel, explicit bridge를 통해 host와 협력할 수 있다.

단, 이 모델은 "아무 일반 함수나 회로화한다"는 뜻이 아니다.
초기와 중기에는 아래 조건을 우선한다.

1. 효과가 제한적이거나 명시적이다.
2. 병렬성이나 데이터플로우 구조가 드러난다.
3. 메모리 접근과 상태 보유 방식이 분석 가능하다.
4. ABI와 실행 비용을 예측할 수 있다.

## 6. 단계별 현실화 전략

### 6.1 초기 단계

1. CPU와 GPU를 공통 의미 아래 다루는 language/IR pipeline 확보
2. reduction, matmul, stencil, stream kernel 같은 좁은 도메인 증명
3. simulator, trace, debug mapping의 최소 툴체인 확보
4. HW는 범용 합성보다 template-oriented 실험으로 시작

### 6.2 중기 단계

1. hardware template selection/configuration을 실행 모델에 포함
2. GPU/HW/CPU 사이의 bridge ABI와 resource model 정교화
3. cost model 기반 placement 실험
4. 공통 언어 표현에서 실제 specialization 이득 입증

### 6.3 장기 단계

1. reconfigurable hardware를 production-grade execution resource로 통합
2. 동일 계산의 multi-realization 선택을 일상화
3. future programmable silicon이 등장할 경우 더 미세한 CPU/HW 혼합 실행으로 확장
4. software-defined hardware에 가까운 실행 플랫폼으로 발전

## 7. 비목표

이 비전은 아래를 즉시 약속하지 않는다.

1. 임의의 일반 프로그램을 자동으로 고성능 회로로 변환하는 것
2. 기존 GPU/CPU/HDL 생태계를 단기간에 완전히 대체하는 것
3. 모든 배치 결정을 완전 자동으로 해결하는 것
4. 디버깅, 검증, 컴파일 시간 문제를 무시한 채 "통합"만 추구하는 것
5. 표면 문법만으로 모든 미래 기능을 미리 확정하는 것

## 8. 연구 가치와 성공 기준

이 비전은 언어, 컴파일러, 런타임, 시스템, 하드웨어 자동화가 만나는 연구 주제다.
성공 기준은 단순한 문법 우아함이 아니라 아래와 같다.

1. 공통 표현에서 CPU/GPU/HW realization으로 의미를 보존할 수 있다.
2. 특정 커널군에서 실제 성능 또는 효율 개선을 보인다.
3. bridge ABI와 debug mapping이 사용 가능한 수준으로 정리된다.
4. specialization이 단순 데모를 넘어 반복 가능한 흐름이 된다.
5. Parus가 host language + heterogeneous execution substrate로 설득력을 가진다.

## 9. 현재 설계에 주는 원칙

이 비전은 현재 Parus 설계에 아래 원칙을 요구한다.

1. 표면 문법보다 IR, ABI, resource contract를 먼저 단단히 만든다.
2. CPU/GPU/HW 의미를 하나의 거대한 공통 IR로 뭉개지 않는다.
3. 공통 코어는 작고 빠르게 유지한다.
4. 실행 모델 사이 이동은 explicit bridge로 남긴다.
5. 미래 확장 가능성을 위해 effect, state, time, resource 의미를 IR에 담을 준비를 한다.

## 10. canon 결정

1. Parus의 초장기 비전은 "하드웨어와 소프트웨어를 하나의 계산 모델 아래에서 다루는 통합 실행환경"으로 고정한다.
2. 이 비전의 핵심 추상화는 "통일된 호출 문법"이 아니라 "통일된 계산 의미와 다중 realization"이다.
3. Parus는 범용 시스템 언어이면서 동시에 heterogeneous execution substrate로 발전할 수 있는 방향을 유지한다.
4. 현재와 중기 설계는 좁은 도메인에서 실증 가능해야 하며, 비전만 앞서 나가서는 안 된다.
