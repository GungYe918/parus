# Parus Inline Assembly Model (Compiler Contract, No Surface Builtin)

문서 버전: `draft-0.1`  
상태: `Design Freeze (TODO Track)`

이 문서는 현재 합의된 인라인 어셈블리 기능 모델을 고정한다.  
매크로 시스템 표면 문법은 아직 확정되지 않았으므로, 본 문서에서는 문법 예시를 다루지 않는다.

## 1. 결정 사항

1. 인라인 어셈블리의 내부 계약점(contract point)은 확장(extension)이 아니라 컴파일러가 직접 제공한다.
1. 기존의 "확장에서 어셈블리어 수준 조작 API를 직접 제공"하는 계획은 폐기한다.
1. 표면 사용성은 라이브러리 매크로가 담당하되, 실제 의미론 검증과 lowering 계약은 컴파일러가 담당한다.
1. 알려진 패턴은 어셈블리로 유지하지 않고 일반 OIR로 재표현하여 최적화 이점을 확보한다.
1. 해석이 불완전하거나 변화 중인 어셈블리 입력은 최적화 대상에서 제외하고 보수적으로 LLVM 경로로 하강한다.

## 2. 목표

1. 통합성: 프론트엔드/타입체커/OIR/백엔드 전 단계에서 일관된 asm 의미론 유지.
1. 안전성: 잘못된 pure/read/write/clobber 선언으로 인한 오최적화를 방지.
1. 확장성: 아키텍처별 lowering은 분리하되 상위 계약 모델은 공통 유지.
1. 성능: 특정 도메인에서 기존 언어의 builtin asm보다 강한 최적화 경로 확보.

## 3. 비목표

1. 임의의 raw asm 텍스트를 아키텍처 무관하게 완전 해석하는 것.
1. 모든 asm을 강제 최적화하는 것.
1. 매크로 문법 자체를 본 문서에서 확정하는 것.

## 4. 컴파일러 제공 계약점 모델

계약점은 "asm plan" 기반으로 제공한다. 매크로는 사용자 입력을 plan으로 구성하고, 컴파일러는 plan의 의미론을 검증/하강한다.

### 4.1 Plan 생명주기 계약

1. plan 생성
1. instruction/operand/clobber/effect 메타 추가
1. inline 또는 global finalize

### 4.2 Operand/Constraint 계약

1. 입력, 출력, 입출력(inout) 바인딩
1. 레지스터/메모리/플래그 clobber 선언
1. ABI 및 target 제약 선언

### 4.3 Effect 계약

1. pure
1. may-read
1. may-write
1. may-trap
1. barrier/ordering

## 5. 최적화 정책 (Tiered)

### Tier A: 완전 해석 가능

1. 명령 의미론이 충분히 알려진 경우 컴파일타임에 패턴 해석을 수행한다.
1. 해당 패턴은 asm으로 내리지 않고 일반 OIR 연산으로 재표현한다.
1. 이 경로에서 기존 OIR 최적화(mem2reg, GVN, LICM 등) 이점을 얻는다.

### Tier B: 부분 해석 가능

1. 안전하게 증명 가능한 범위의 제한적 최적화만 수행한다.
1. 증명 불가능한 부분은 보수적으로 유지한다.

### Tier C: 해석 불가/변동 큼

1. asm 최적화를 수행하지 않는다.
1. 보수적 효과 모델(메모리/배리어/클로버)로 처리하여 LLVM 경로로 하강한다.

## 6. "Builtin asm 대비 우위"를 얻는 조건

1. 도메인 카탈로그 기반 known-pattern 인식이 안정적으로 동작할 것.
1. 인식된 패턴을 일반 OIR로 재표현할 것.
1. 재표현 후 기존 OIR 최적화 파이프라인을 그대로 적용할 것.
1. 패턴 미인식 시 즉시 보수 경로로 전환할 것.

이 조건을 만족하면 syscall wrapper, bit-manipulation, 일부 메모리 접근 패턴 등 특정 도메인에서 기존 builtin asm 경로보다 높은 최적화 이점을 기대할 수 있다.

## 7. oASM IR 도입 원칙

1. asm IR은 `oASM IR`로 별도 정의한다.
1. `oASM IR`은 OIR 체계의 하위집합(하위 dialect)으로 포함한다.
1. 단, 일반 OIR와 instruction 수준에서 직접 혼합하지 않는다.
1. 일반 OIR 영역과 oASM 영역은 명시적 경계 노드(bridge)로만 연결한다.
1. 경계에서 타입/효과/클로버 계약을 재검증한다.

## 8. IR 격리 규칙

1. 일반 OIR instruction block 내부에 oASM instruction을 삽입하지 않는다.
1. oASM block 내부에 일반 OIR instruction을 삽입하지 않는다.
1. 값 전달은 bridge에서 정의된 값 슬롯을 통해서만 허용한다.
1. bridge 바깥에서 oASM 내부 임시값을 참조하는 행위를 금지한다.

## 9. 파이프라인 통합 위치

1. 매크로 확장 단계에서 asm plan 수집
1. asm 의미론 분석 패스에서 Tier 분류
1. Tier A는 일반 OIR 재표현
1. Tier B/C는 oASM IR 생성 후 backend lowering
1. 검증 실패 시 컴파일 오류 또는 보수 fallback 수행

## 10. 보안/신뢰 경계

1. asm 계약점이 컴파일러 내장으로 이동했으므로, extension 권한 남용면적을 줄인다.
1. 위험한 해석 추정은 금지하고, 증명 가능한 규칙만 최적화에 사용한다.
1. 불확실하면 항상 보수 경로(Tier C)로 강등한다.

## 11. 구현 단계 제안

1. 단계 1: asm plan 계약점 + Tier C fallback 우선 구현
1. 단계 1: oASM IR 자료구조와 verifier 추가
1. 단계 2: known-pattern 카탈로그와 Tier A 재표현 패스 추가
1. 단계 3: 도메인별 최적화 카탈로그 확장 및 성능 회귀 검증

## 12. 추적 항목

1. oASM IR verifier 규칙 상세
1. bridge 노드 타입 시스템 정의
1. architecture별 Tier A 지원 매트릭스
1. 진단 코드 체계(패턴 미인식, 계약 위반, fallback 사유)
