# Parus Terminology Guide v0.0.1

문서 버전: `v0.0.1`
상태: `Normative (Language Vocabulary Source)`

이 문서는 Parus 언어 문서에서 사용하는 핵심 용어를 고정한다.
`docs/reference/language/SPEC.md` 및 개별 설계 문서에서 용어가 충돌하면, 의미 해석은 본 문서를 우선한다.

---

## 1. 용어 사용 원칙

1. 용어는 가능하면 PL/컴파일러 분야의 통용어를 사용한다.
2. 같은 개념은 문서 전체에서 같은 이름으로만 부른다.
3. 문법 레벨과 의미(semantics) 레벨 용어를 섞지 않는다.
4. 예외가 아니면 축약어보다 원어를 우선한다.

권장 표기:

* `item`, `statement`, `declaration`, `expression`, `block`
* `file scope`, `lexical scope`, `path`, `symbol`
* `lowering`, `type checking`, `name resolution`, `diagnostic`

---

## 2. 구문 계층 용어

### 2.1 expression

값을 계산해 타입이 있는 결과를 만드는 구문 단위.

예:

* `a + b`
* `v.add(1)`
* `Vec2{ x: 1, y: 2 }`

### 2.2 statement

실행 흐름을 구성하는 문장 단위. 값 생성이 목적이 아닌 제어/효과 중심 단위.

예:

* `return x;`
* `if (...) { ... }`
* `set x = 1;`

### 2.3 block

`{ ... }`로 감싼 statement 집합.

### 2.4 item

파일/네임스페이스 레벨에서 배치되는 선언성 단위.
`item`은 `declaration item`과 `directive item`으로 나뉜다.

### 2.5 declaration item

심볼(타입/함수/전역 등)을 실제로 도입하는 item.

### 2.6 directive item

심볼 자체 정의가 아니라 해석 환경(별칭/경로/선택)을 바꾸는 item.

---

## 3. 스코프/이름해소 용어

### 3.1 file scope

하나의 `.pr` 파일 최상위 범위.

### 3.2 lexical scope

소스의 중괄호/블록 경계에 의해 정해지는 정적 스코프.

### 3.3 path

`::`로 연결된 심볼 경로.

예:

* `foo::math::add`
* `Vec2::acts(A)::add`

### 3.4 alias

`import`/`use`로 도입한 대체 이름.

### 3.5 namespace alias (`use nest`)

`use nest <Path> [as Alias];` 형태로 도입한 네임스페이스 경로 별칭.

규칙:

* namespace 경로에만 적용한다.
* alias 생략 시 마지막 path segment를 별칭으로 사용한다.

---

## 4. 타입/메모리 용어

### 4.1 value type

값 의미(value semantics) 중심 타입.

### 4.2 optional type (`T?`)

`T` 또는 `null` 값을 갖는 1급 값 타입.

### 4.3 borrow / escape

* borrow: 소유권을 이전하지 않는 참조 관점
* escape(`&&`): 소유권/수명 경계를 넘기는 핸들 관점

### 4.4 layout(c), align(n)

C ABI 경계 레이아웃과 정렬 정책 지정자.

---

## 5. OOP/행동 모델 용어

### 5.1 field

데이터 저장/레이아웃 중심 타입.

### 5.2 acts

타입에 부착되는 정적 행동/연산자 집합.

### 5.3 proto

인터페이스 계약(시그니처 집합).

### 5.4 tablet

구현체 타입(수명주기 포함).

### 5.5 class

공유 상태/동시성 상태머신 모델.

### 5.6 self / Self

* `self`: 리시버 토큰
* `Self`: 문맥 타입 이름(contextual type name)

---

## 6. 빌드/배포 용어

### 6.1 file / module / bundle / project

* file: 파싱 단위
* module: bundle 내부 논리 그룹
* bundle: 컴파일/배포 단위
* project: 여러 bundle의 집합

### 6.2 toolchain / sysroot / target

* toolchain: 컴파일러+링커+런타임 구성 집합
* sysroot: 표준 라이브러리/타겟 산출물 기준 루트
* target: triple/abi/feature 조합 단위

### 6.3 parlib

Parus 내부 재사용/링크 최적화를 위한 번들 포맷.

---

## 7. 문서 상태 용어

### 7.1 Normative

구현 판정의 기준이 되는 정본 문서.

### 7.2 Draft

설계 제안 단계 문서. 구현 근거로 직접 사용하지 않는다.

### 7.3 Single Source of Truth

해당 주제에서 최종 충돌 해소 기준이 되는 문서.

---

## 8. 문서 작성 패턴(권장)

1. 먼저 목적과 범위를 쓴다.
2. 용어를 먼저 고정하고 문법/규칙을 제시한다.
3. 규칙은 "허용/금지/예외" 3단으로 쓴다.
4. 각 규칙은 최소 1개 성공/실패 예제를 붙인다.
5. 구현 체크리스트와 비범위를 분리해 기록한다.
