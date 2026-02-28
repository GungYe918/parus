# Parus Generics System Model (v1~v4 Roadmap, Design Freeze)

문서 버전: `draft-0.2`  
상태: `Design Freeze (TODO Track)`  
목적: 본 문서 하나만으로 Parus 제네릭 도입을 단계별로 구현할 수 있도록 기준/작업/수용조건을 고정한다.

---

## 1. 목적과 범위

이 문서는 Parus 제네릭 시스템의 도입 순서를 `v1 -> v2 -> v3 -> v4`로 고정한다.

1. 표면 문법과 의미론을 페이즈별로 분리 고정한다.
1. `proto`, `acts`, `class/field`, `dyn`의 결합 규칙을 충돌 없이 정의한다.
1. 정적 모노모피제이션과 동적 모노모피제이션(JIT)을 분리한다.
1. 각 단계의 구현 체크리스트/테스트/수용 기준을 포함한다.

비범위:

1. HKT(고차 타입 매개변수)
1. const generic
1. 부분 특수화(partial specialization)
1. trait/impl 스타일 신규 키워드(Parus 정책상 미도입)

---

## 2. 비가역 결정 사항 (Non-negotiable)

1. 제약 문법은 `with [T: Proto, ...]` 단일형만 허용한다.
1. 기본 경로는 정적 모노모피제이션이다.
1. `dyn`은 명시 경계 전용이며 정적 제네릭과 분리한다.
1. `acts`는 정책/연산자 계층이며 런타임 다형성 수단이 아니다.
1. `proto`는 계약 계층이며 연산자 선언은 금지한다.
1. class 상속 금지는 유지한다.

참조 정본:

1. `/Users/gungye/workspace/Lang/gaupel/docs/reference/abi/v0.0.1/GENERICS_MODEL.md`
1. `/Users/gungye/workspace/Lang/gaupel/docs/reference/abi/v0.0.1/OOP_MODEL.md`
1. `/Users/gungye/workspace/Lang/gaupel/docs/reference/language/spec/22.md`

---

## 3. 현재 코드 기준 (출발선)

현재 저장소는 다음이 이미 존재한다.

1. 함수 generic 파라미터 파싱(`<T, U>`)  
1. 함수 제약절 파싱(`with [T: Proto]`)  
1. 선언 시점의 최소 제약 검증(unknown type param/proto path)

현재 부족한 부분:

1. 제네릭 인스턴스 실체화 파이프라인 부재
1. generic 함수 호출 시 타입 인자 추론/결정 부재
1. generic class/field/proto의 전면 타입체크 및 lowering 부재
1. generic acts 결합 부재

---

## 4. 표면 문법 고정 (완성 상태 기준)

```ebnf
GenericParamClauseOpt := "<" TypeParam ("," TypeParam)* ">" | ε ;
TypeParam := Ident ;

ConstraintClauseOpt := "with" "[" Constraint ("," Constraint)* "]" | ε ;
Constraint := TypeParam ":" ProtoPath ;

TypePath := Path GenericArgsOpt ;
GenericArgsOpt := "<" Type ("," Type)* ">" | ε ;

NormalFuncDecl := ... Ident GenericParamClauseOpt FuncParams ConstraintClauseOpt "->" Type Block ;
ProtoDecl      := "proto" Ident GenericParamClauseOpt ProtoBaseListOpt ProtoBody ProtoTailOpt ;
ClassDecl      := "class" Ident GenericParamClauseOpt ClassProtoListOpt ConstraintClauseOpt ClassBody ;
FieldDecl      := "field" Ident GenericParamClauseOpt FieldProtoListOpt ConstraintClauseOpt FieldBody ;
ActsForDecl    := "acts" NameOpt "for" TypePath GenericParamClauseOpt ConstraintClauseOpt ActsBody ;
```

고정 규칙:

1. 제약절은 항상 `with [ ... ]`.
1. 브래킷 없는 `with T: P` 금지.
1. generic 인자 생략 호출은 추론 성공 시만 허용.
1. 실패 시 명시적 `<...>`를 요구한다.
1. `class/field/acts`의 `with [...]`는 해당 선언의 generic param만 참조 가능하다.
1. `proto`의 `with require(...)`와 제네릭 `with [ ... ]`는 문법적으로 다른 레이어이며 혼용하지 않는다.

---

## 5. 의미론 고정

## 5.1 정적 제네릭

1. 제네릭 함수/타입은 concrete 타입 튜플마다 인스턴스를 만든다.
1. 한 인스턴스는 단일 concrete 시그니처를 가진다.
1. 동일 인스턴스 키는 번들 전체에서 중복 생성하지 않는다.

인스턴스 키:

1. owner symbol(qualified)
1. generic param 순서
1. concrete type tuple
1. target triple / abi line / bundle hash

## 5.2 제약 검사

1. `with [T: P]`에서 `T`는 해당 선언의 generic param이어야 한다.
1. `P`는 proto로 해석되어야 한다.
1. 인스턴스화 시점에 concrete 타입이 proto 요구를 충족해야 한다.
1. proto의 `Self`는 concrete 타입으로 치환 후 시그니처를 비교한다.
1. class/field 선언의 제약은 인스턴스화 경계(`TypePath` concrete화 시점)에서 검사한다.
1. 함수 선언의 제약은 호출 인스턴스화 시점에서 검사한다.

## 5.3 오버로드 해소 우선순위

1. 비제네릭 exact match
1. 제네릭 인스턴스 exact match
1. 실패 또는 다중 후보면 ambiguous 진단

## 5.4 타입 인자 추론 알고리즘 (v1 고정)

1. 호출 인자 타입과 파라미터 타입을 좌우 대응해 제약식을 수집한다.
1. 제약식으로부터 type var 대입 후보를 만든다.
1. 동일 type var의 다중 후보는 단일 concrete로 합치되, 실패하면 ambiguous.
1. 미결정 type var가 남으면:
1. 반환 타입 문맥(expected type)으로 보조 추론한다.
1. 그래도 미결정이면 명시 `<...>` 요구 진단을 낸다.
1. 최종 concrete tuple 생성 후 `with [...]` 제약을 검사한다.

## 5.5 진단 규약 (v1 최소 세트)

1. `GenericUnknownTypeParamInConstraint`
1. `GenericConstraintProtoNotFound`
1. `GenericConstraintUnsatisfied`
1. `GenericTypeArgInferenceFailed`
1. `GenericAmbiguousOverload`
1. `GenericArityMismatch`
1. 진단은 “선언 지점 + 인스턴스화/호출 지점” 쌍 위치를 함께 제공한다.

---

## 6. 페이즈 로드맵

## 6.1 v1: Static Generics Core

목표:

1. generic 함수 호출/추론/인스턴스화 완성
1. generic class/field/proto 선언 및 concrete 사용 가능
1. SIR/OIR/LLVM까지 인스턴스 lowering 연결

구현 항목:

1. AST에 generic owner 메타 정리 (`generic param begin/count`, constraint refs)
1. Tyck에 인스턴스 요청 수집기 추가
1. 인스턴스화 엔진(타입 치환기) 추가
1. mangle 규칙 추가: generic 인스턴스 심볼 이름 결정화
1. SIR 빌드에서 concrete 함수만 생성
1. OIR/LLVM 하향은 concrete만 처리
1. 제네릭 인스턴스 캐시/중복제거(번들 단위) 도입

세부 구현 순서:

1. NameResolve: generic owner symbol id 안정화
1. Tyck: `InstantiationRequest` 수집
1. Tyck: type substitution + constraint checker
1. SIR: concrete decl clone(materialize)
1. OIR/LLVM: materialized symbol만 하향
1. Verify: 미실체화 generic symbol 하향 금지 검증

수용 기준:

1. `def id<T>(x: T) -> T` 호출이 concrete로 생성
1. `with [T: Proto]` 위반 시 인스턴스화 지점 진단
1. 같은 인스턴스 재사용(중복 코드 생성 없음)

## 6.2 v2: Generic Acts + Coherence

목표:

1. generic acts 선언 허용
1. concrete 타입에 대한 acts 인스턴스 선택 규칙 고정
1. 중복/겹침(overlap) 금지 규칙 도입

핵심 규칙:

1. `acts for Vec<T> with [T: Equatable]` 허용
1. 동일 concrete owner에 겹치는 acts 인스턴스 금지
1. 해소 순서: named acts > default acts > builtin
1. coherence key는 `(owner concrete type, acts name-or-default, method/op signature)`로 고정

수용 기준:

1. generic acts 메서드/연산자가 concrete 타입에서 해소됨
1. overlap 선언은 컴파일 에러
1. 기본 타입(i32 등) builtin acts와 충돌 없이 공존

## 6.3 v3: dyn Introduction (Proto Object Boundary)

목표:

1. `dyn Proto` 타입 도입
1. object safety 규칙 고정
1. 정적 제네릭과 dyn 경계 분리

가능해지는 표현:

1. 이종 컬렉션(`dyn Proto` 저장)
1. 플러그인 경계 호출
1. 명시적 업캐스팅/동적 디스패치

금지/제약:

1. `Self` 반환/제네릭 메서드 등 object-unsafe 시그니처는 dyn 금지
1. `acts`에는 dyn 경로 도입하지 않음

수용 기준:

1. `&dyn Proto` 호출 lowering(vtable/witness 기반) 동작
1. object-unsafe proto는 진단
1. 기존 정적 제네릭 코드 성능/의미 회귀 없음

## 6.4 v4: Dynamic Monomorphization (JIT Only)

목표:

1. hot concrete tuple에 대해 런타임 특수화 허용
1. 정적 모노모피제이션과 실행 경로 분리

분리 원칙:

1. AOT 산출물: 정적 인스턴스만
1. JIT 런타임: `jit-instance-cache` 별도 유지
1. 특수화 실패 시 정적/일반 경로 fallback

수용 기준:

1. 동일 hot path 재실행 시 JIT 인스턴스 재사용
1. fallback 경로의 의미론 100% 동일
1. 번들 결정성/AOT 산출물 비결정성 없음

---

## 7. 정적 vs dyn vs 동적 모노모피제이션 분리

## 7.1 호출 경로

1. 정적 generic 호출: compile-time resolved, direct call
1. dyn 호출: runtime table lookup + indirect call
1. dynamic mono(JIT): runtime specialization 후 direct-like fast path

## 7.2 코드생성 위치

1. 정적 mono: frontend tyck/sir 단계에서 인스턴스 생성
1. dyn: ABI 고정 후 backend dispatch path 생성
1. dynamic mono: JIT runtime subsystem에서만 생성

## 7.3 심볼 네이밍(권장)

1. 정적: `__g$<owner>$<type_tuple_hash>`
1. dyn helper: `__dyn$<proto>$<method>`
1. JIT: `__jit$<owner>$<type_tuple_hash>$<runtime_id>`

---

## 8. proto + acts 결합 정책

## 8.1 역할 분리 유지

1. proto: 계약
1. acts: 행동/연산 정책
1. class/field: 데이터/구현체

## 8.2 generic 도입 이후 acts 사용

1. v1: concrete acts 중심(현재 모델 유지)
1. v2+: generic acts 활성
1. proto 충족 검사는 우선 구현체 멤버를 본다.
1. 선택적으로 acts 기반 충족 후보를 허용하되, 규칙은 단일화(모호성 금지)한다.

권장:

1. 알고리즘 제약은 proto로 표현
1. 연산자 구현은 acts로 표현

## 8.3 generic + acts 예측 가능한 해소 규칙

1. 호출 후보 생성 시 concrete owner type 기준으로만 acts 후보를 모은다.
1. 후보에 generic acts가 있으면 concrete tuple을 먼저 확정하고 나서 시그니처 비교한다.
1. 동일 우선순위 후보 2개 이상이면 모호성 에러(암묵 우선순위 없음).
1. 타이브레이커로 “더 구체적” 규칙을 도입하지 않는다(v2 범위 밖).

---

## 9. 구현 체크리스트 (파일 단위)

## 9.1 Parser/AST

1. `/Users/gungye/workspace/Lang/gaupel/frontend/include/parus/ast/Nodes.hpp`
1. `/Users/gungye/workspace/Lang/gaupel/frontend/include/parus/parse/Parser.hpp`
1. `/Users/gungye/workspace/Lang/gaupel/frontend/src/parse/decl/parse_decl_fn.cpp`
1. `/Users/gungye/workspace/Lang/gaupel/frontend/src/parse/decl/parse_decl_data.cpp`
1. `/Users/gungye/workspace/Lang/gaupel/frontend/src/parse/type/parse_type.cpp`

## 9.2 Tyck/NameResolve

1. `/Users/gungye/workspace/Lang/gaupel/frontend/include/parus/tyck/TypeCheck.hpp`
1. `/Users/gungye/workspace/Lang/gaupel/frontend/src/tyck/core/type_check_entry.cpp`
1. `/Users/gungye/workspace/Lang/gaupel/frontend/src/tyck/stmt/type_check_stmt.cpp`
1. `/Users/gungye/workspace/Lang/gaupel/frontend/src/tyck/expr/type_check_expr_call_cast.cpp`
1. `/Users/gungye/workspace/Lang/gaupel/frontend/src/passes/name_resolve.cpp`

## 9.3 IR/Lowering

1. `/Users/gungye/workspace/Lang/gaupel/frontend/include/parus/sir/SIR.hpp`
1. `/Users/gungye/workspace/Lang/gaupel/frontend/src/sir/lower/sir_builder_decl.cpp`
1. `/Users/gungye/workspace/Lang/gaupel/frontend/src/sir/lower/sir_builder_expr_stmt.cpp`
1. `/Users/gungye/workspace/Lang/gaupel/frontend/src/oir/oir_builder.cpp`
1. `/Users/gungye/workspace/Lang/gaupel/backend/src/aot/LLVMIRLowering.cpp`

## 9.4 문서

1. `/Users/gungye/workspace/Lang/gaupel/docs/reference/abi/v0.0.1/GENERICS_MODEL.md`
1. `/Users/gungye/workspace/Lang/gaupel/docs/reference/language/spec/22.md`
1. `/Users/gungye/workspace/Lang/gaupel/docs/reference/language/spec/28.md`
1. `/Users/gungye/workspace/Lang/gaupel/docs/reference/language/spec/31_PROTO_CONSTRAINTS.md`

---

## 10. 테스트 계획

## 10.1 v1 parser/tyck

1. `ok_generic_fn_infer_basic.pr`
1. `ok_generic_fn_explicit_args.pr`
1. `ok_generic_proto_constraint_instance.pr`
1. `err_generic_unknown_type_param_constraint.pr`
1. `err_generic_constraint_unsatisfied_instance.pr`
1. `err_generic_ambiguous_overload.pr`
1. `err_generic_type_arg_inference_failed.pr`
1. `ok_generic_class_constraint_satisfied.pr`
1. `err_generic_class_constraint_unsatisfied.pr`

## 10.2 v1 OIR/LLVM

1. 인스턴스별 함수 심볼 생성 검증
1. 중복 인스턴스 제거 검증
1. concrete call direct lowering 검증
1. 같은 concrete tuple 재호출 시 재실체화 방지 검증
1. 번들 cross-file 동일 인스턴스 dedup 검증

## 10.3 v2 acts

1. `ok_generic_acts_for_type.pr`
1. `err_generic_acts_overlap.pr`
1. `ok_generic_acts_operator_resolution.pr`

## 10.4 v3 dyn

1. `ok_dyn_proto_call_basic.pr`
1. `err_dyn_object_unsafe_self_return.pr`
1. `ok_static_and_dyn_mixed_path.pr`

---

## 11. 구현 완료 시 가능한 예제 (목표 인벤토리)

## 11.1 정적 제네릭 알고리즘

```parus
proto Equatable {
    def eq(self, rhs: &Self) -> bool;
} with require(true);

def contains<T>(xs: &[T], x: &T) with [T: Equatable] -> bool {
    set mut i = 0;
    while (i < xs.len) {
        if (xs[i].eq(rhs: x)) { return true; }
        i = i + 1;
    }
    return false;
}
```

## 11.2 generic class + proto

```parus
proto Hashable {
    def hash(self) -> u64;
}

class Box<T>: Hashable with [T: Hashable] {
    value: T;
    init(v: T) { self.value = v; }
    def hash(self) -> u64 { return self.value.hash(); }
}
```

## 11.3 generic acts (v2)

```parus
acts for Vec<T> with [T: Equatable] {
    def has(self, x: &T) -> bool {
        return contains<T>(xs: self.items(), x: x);
    }
}
```

## 11.3-b generic field + constraint

```parus
field Pair<K, V> with [K: Hashable, V: Equatable] {
    key: K;
    value: V;
}
```

## 11.4 dyn 경계 (v3)

```parus
proto Drawable {
    def draw(self, fb: &mut Frame) -> void;
}

def render_static<T>(x: &T, fb: &mut Frame) with [T: Drawable] -> void {
    x.draw(fb: fb);
}

def render_dyn(x: &dyn Drawable, fb: &mut Frame) -> void {
    x.draw(fb: fb);
}
```

## 11.5 static + dynamic mono 분리 (v4, 개념 예시)

```parus
// AOT: 기존 정적 인스턴스 경로 유지
// JIT: hot concrete tuple만 추가 특수화 캐시에 적재
def map<T, U>(xs: &[T], f: fn(&T)->U) -> Vec<U> { ... }
```

---

## 12. 수용 기준 (최종)

1. v1: generic 함수/타입/proto 제약이 정적 인스턴스화로 완결된다.
1. v2: generic acts와 coherence 규칙이 동작한다.
1. v3: dyn proto 경계가 object safety 포함해 안정화된다.
1. v4: JIT 동적 모노모피제이션이 AOT 결정성을 깨지 않는다.
1. 문서와 구현이 동일한 규칙을 공유한다.

추가 완료 조건:

1. 신규 진단 코드가 파서/tyck/문서에 동일 명칭으로 정합된다.
1. generic 관련 하향 경로에서 미실체화 심볼이 남지 않는다.
1. 번들 병렬 빌드 시 인스턴스 심볼 순서가 결정적이다.

---

## 13. 주의사항

1. `dyn` 도입 전에는 제네릭 경로에서 런타임 다형성을 암시하지 않는다.
1. `acts`는 계속 정적 해소 계층으로 유지한다.
1. 구현 편의를 위해 문법을 뒤섞지 않는다(`impl`/`derive` 신규 도입 금지).
