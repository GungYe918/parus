# Parus Monomorphization Model

문서 버전: `v0.0.1`  
상태: `Adopted`

이 문서는 Parus의 공통 정적 제네릭 실체화 모델을 고정한다.  
이 모델은 특정 문법 기능 하나의 구현 디테일이 아니라, Parus가 concrete code/layout을 생성하는 공통 계층이다.

충돌 우선순위:

1. ABI 경계 의미 충돌 시 `/Users/gungye/workspace/Lang/gaupel/docs/reference/abi/v0.0.1/ABI.md`
2. OOP/dyn 의미 충돌 시 `/Users/gungye/workspace/Lang/gaupel/docs/reference/abi/v0.0.1/OOP_MODEL.md`
3. 언어 표면 문법 충돌 시 language spec을 갱신해 정합화

---

## 1. 목표

1. 정적 제네릭은 끝까지 direct-code lane으로 유지한다.
2. 모노모피제이션은 generic free function 전용 패치가 아니라 공통 서비스로 다룬다.
3. local 선언과 external import를 같은 request/instance 모델로 처리한다.
4. 런타임 dictionary ABI, erased generic ABI, concrete matrix 선생성은 도입하지 않는다.

---

## 2. 공통 계층

Parus의 monomorphization은 아래 3단계 모델로 고정한다.

### 2.1 Template

`Template`은 generic declaration의 typed 원본이다.

장기 범위:

1. generic free function
2. generic class member / static member / lifecycle member
3. generic struct/class/enum concrete layout source
4. generic proto `provide def` / `provide const`
5. generic acts member / operator

이번 라운드 활성화 범위:

1. exported generic free function

### 2.2 Request

`Request`는 concrete instance가 필요한 시점의 실체화 요청이다.

Request는 아래 정보를 갖는다.

1. producer bundle
2. template identity(symbol/path/kind)
3. concrete type tuple
4. target
5. ABI lane

### 2.3 Instance

`Instance`는 이미 concrete화된 결과물이다.

이 단계부터는 SIR/OIR/LLVM이 generic을 직접 보지 않는다.

1. concrete 함수 시그니처
2. concrete layout
3. concrete symbol/link name
4. concrete lowered body

---

## 3. Instance Key

인스턴스 캐시 키는 아래 canonical tuple로 고정한다.

1. producer bundle
2. template symbol identity
3. concrete generic argument tuple
4. target triple / target lane
5. ABI line

즉 키는 개념적으로 아래와 같다.

```text
(producer_bundle, template_symbol, concrete_tuple, target, abi)
```

같은 키는 번들 내에서 한 번만 materialize한다.

---

## 4. Materialization Stage

공통 원칙:

1. parse/name-resolve/tyck는 typed template graph를 만든다.
2. monomorphization은 **tyck 이후, SIR 이전**에 concrete instance를 확정한다.
3. SIR/OIR/LLVM은 concrete instance만 본다.

즉 정적 generic lane은 “generic IR을 끝까지 보존”하지 않는다.

---

## 5. Local vs External Template Source

### 5.1 Local

같은 번들 안에서 선언된 generic은 template body를 직접 참조할 수 있다.

1. tyck는 local template를 직접 instance로 clone/materialize한다.
2. constraint 검사는 local 선언 메타를 사용한다.

### 5.2 External

export-index만으로는 external generic free function body를 복원할 수 없다.  
따라서 external exported generic free function은 **adjacent template sidecar**를 통해 template source를 제공한다.

v1 원칙:

1. producer bundle은 export-index와 별도의 optional sidecar를 낸다.
2. consumer bundle은 sidecar가 있으면 그 template source를 local template로 로드한다.
3. 이후 local monomorphization service가 같은 방식으로 concrete instance를 만든다.

이 과정은 generic ABI 호출이 아니라 **consumer-local monomorphic recompilation**이다.

---

## 6. External Template Sidecar

이번 라운드의 sidecar는 raw source closure 기반이다.

포함 범위:

1. exported generic free function
2. 그 body가 참조하는 same-file free-function dependency closure

이번 라운드 비범위:

1. IR-level template serialization
2. generic class/proto/acts body import
3. dyn/object-safe dispatch lane

sidecar가 있어도 export-index version은 유지한다.  
sidecar는 adjacent optional file이며, 없으면 기존 non-template import는 그대로 동작한다.

---

## 7. Performance Rules

Parus 정적 generic lane의 성능 원칙은 아래로 고정한다.

1. on-demand only
2. direct code only
3. no dictionary ABI
4. no erased generic ABI
5. no pre-expanded concrete matrix

즉 concrete instance가 필요할 때만 생성하고, 생성된 결과는 ordinary concrete symbol과 동일하게 취급한다.

그 결과:

1. direct call 유지
2. inlining 유지
3. dead code elimination 유지
4. 런타임 generic dispatch 비용 0

---

## 8. 이번 라운드 활성화 범위

이번 라운드에서 실제로 연 기능은 아래뿐이다.

1. 공통 monomorphization 문서/모델 고정
2. exported generic free function의 cross-bundle template import
3. consumer-local monomorphization
4. installed core generic helper 복원
5. concrete shim 제거

이번 라운드에서 아직 열지 않는 것:

1. exported generic class body import
2. exported generic proto body import
3. exported generic acts body import
4. dyn/object lane

---

## 9. Ownership Materialization과의 분리

`~` escape handle 문제는 monomorphization과 분리한다.

분리 원칙:

1. monomorphization = generic template를 concrete code/layout으로 만드는 문제
2. ownership materialization = `~`가 storage로 들어가는가, SSA alias로만 존재하는가의 문제

따라서 `~`는 이 문서의 직접 범위가 아니다.  
`~` 정책은 `/Users/gungye/workspace/Lang/gaupel/docs/reference/language/spec/19.md`에서 별도로 고정한다.

---

## 10. 비범위

이번 문서의 직접 비범위:

1. `dyn`
2. `Impl::*`
3. `macro`
4. `inst`
5. const generic
6. partial specialization
7. HKT

---

## 11. 변경 이력

### v0.0.1

1. 공통 monomorphization 계층을 `Template -> Request -> Instance`로 고정
2. canonical instance key를 `producer bundle + template symbol + concrete tuple + target + abi`로 고정
3. 정적 generic lane은 on-demand local monomorphization + direct code only로 고정
4. external activation v1은 exported generic free function first로 고정
5. sidecar 기반 consumer-local monomorphization 방향을 정본으로 확정
