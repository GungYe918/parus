# Parus Macro System Model (Design Freeze)

문서 버전: `draft-0.2`  
상태: `Design Freeze (TODO Track)`  
목적: 다음 대화에서 구현을 진행하기 위한 결정 완결 기준 문서

## 1. 목적과 범위

이 문서는 Parus 매크로 시스템의 전체 설계를 고정한다.

1. 표면 문법, 의미 규칙, 위생성, 진단 규칙, 파이프라인 통합 기준을 고정한다.
1. 구현자가 추가 결정을 하지 않아도 되도록 EBNF와 수용 기준을 함께 정의한다.
1. 본 문서는 TODO 트랙 설계 문서이며, 구현 이후 normative 문서로 승격 가능하다.

## 1.1 현재 구현 기준 (Phase1.5 Delta)

이 문서는 장기 목표(Phase2+ 포함)와 현재 구현 기준(Phase1.5)을 함께 기록한다.

1. 현재 라운드는 `typed macro(expr/stmt/item/type)`를 실사용 기준으로 고정한다.
1. `with token`은 기본 비활성 상태이며, 실험 플래그를 켜도 확장기는 미구현 진단으로 종료한다.
1. 위생성은 binder 중심으로 제한한다(`let/set/def param/loop binder`).
1. `$sizeof` 같은 표준 매크로는 빌트인 계약점/표준 라이브러리 체계 확정 이후로 이월한다.
1. 매크로 예산은 기본값 + 사용자 조정 + 하드 상한 clamp 정책으로 동작한다.

## 2. 결정 사항 (Non-negotiable)

1. 문서 범위는 전체 설계 + EBNF로 고정한다.
1. 매크로 호출 문법은 `$foo(...)`만 허용한다.
1. `with token`의 장기 목표는 `macro_rules!` 코어급 표현력(반복/재귀 포함)이다.
1. Phase1.5 구현에서는 `with token`을 실험 플래그로만 노출하고 확장은 미지원으로 고정한다.
1. 안전성 우선 정책을 적용한다. 증명 불가능한 경우 보수적으로 실패/진단한다.

## 3. 비목표

1. 본 문서에서 실제 파서/AST 코드 구현을 수행하지 않는다.
1. 본 문서에서 컴파일러 extension 기반 매크로 런타임을 도입하지 않는다.
1. 기본 문법/기본 타입/핵심 런타임 API를 매크로로 대체하는 정책을 허용하지 않는다.
1. `$sizeof`를 이번 라운드 표준 매크로로 포함하지 않는다.

## 4. 표면 문법 고정

```ebnf
MacroDecl      := "macro" Ident "->" "{" MacroGroup+ "}" ;

MacroGroup     := "with" MatchKind "{" MacroArm+ "}" ;
MatchKind      := "expr" | "stmt" | "item" | "type" | "token" ;

MacroArm       := "(" Pattern ")" "=>" OutKind Block ";" ;
OutKind        := "expr" | "stmt" | "item" | "type" ;

Pattern        := TypedPattern | TokenPattern ;

TypedPattern   := TypedElem ("," TypedElem)* ;
TypedElem      := Ident ":" FragKind
               | Ident ":" FragKind "..."
               ;
FragKind       := "expr" | "stmt" | "item" | "type" | "path" | "ident" | "block" ;

TokenPattern   := TokenElem* ;
TokenElem      := TokenLit
               | Capture
               | Repeat
               | Group
               ;
Capture        := Ident ":" FragKind
               | Ident ":" "tt"
               | Ident ":" FragKind "..."
               | Ident ":" "tt" "..."
               ;
Repeat         := "$(" TokenElem* ")" RepOp
               | "$(" TokenElem* ")" Sep RepOp ;
RepOp          := "*" | "+" | "?" ;
Sep            := "," | ";" | "|" ;

MacroCall      := "$" Path "(" ArgTokenStream? ")" ;
```

보조 정의:

1. `ArgTokenStream`은 파서가 일반 토큰 스트림으로 받아 매칭 엔진에 전달한다.
1. `Group`은 `(...)`, `{...}`, `[...]`로 중첩 가능한 토큰 그룹을 의미한다.
1. `TokenLit`은 키워드/연산자/구두점 리터럴 토큰을 의미한다.

## 5. `with token` 범위 (장기 목표 + 현재 상태)

장기 목표(Phase2+)에서 `with token`은 다음 기능을 지원한다.

1. `tt` 캡처
1. typed fragment 캡처(`expr`, `type`, `path`, `ident`, `stmt`, `item`, `block`)
1. 반복 `*`, `+`, `?`
1. 구분자 반복(`$(...),*` 스타일)
1. 중첩 반복
1. 재귀 매크로 호출
1. variadic 포워딩(`tt...`, `$n...`)
1. 리터럴 토큰 매칭(키워드/연산자/구두점)

Phase1.5 현재 상태:

1. 기본값은 비활성이다.
1. `-Xparus -macro-token-experimental` 또는 LSP 초기화 옵션으로만 선언 파싱을 허용한다.
1. 확장기는 token-group arm을 만나면 결정적 미구현 진단(`kMacroTokenUnimplemented`)으로 실패한다.

## 6. 위생성(Hygiene) 모델

기본 위생성은 hygienic이며, Phase1.5에서는 binder 중심으로 제한 적용한다.

1. 매크로가 생성한 식별자는 gensym 기반으로 call-site와 충돌하지 않아야 한다.
1. 패턴 캡처된 식별자는 call-site 문맥에서 해석한다.
1. Phase1.5 자동 rename 대상 binder:
1. `let [mut] name`
1. `set [mut] name`
1. `def (...)` 파라미터 이름
1. `loop (name in ...)`
1. 캡처 토큰은 rename 금지(call-site 의미 보존).
1. 기본 정책에서 의도적 비위생 escape는 금지한다.
1. 비위생 escape는 향후 reserved opt-in 설계로만 허용한다.

필수 내부 타입:

1. `HygieneId`
1. `GensymTable`

## 7. 확장 알고리즘 (매칭/선택/확장/재파싱)

확장 알고리즘은 아래 순서를 고정한다.

1. 호출 식별자(`$Path(...)`)로 매크로 선언을 찾는다.
1. `MatchKind` 그룹을 문맥(`expr/stmt/item/type/token`)에 따라 선택한다.
1. 그룹 내부 arm을 선언 순서로 first-match 평가한다.
1. 매칭 성공 arm의 템플릿을 캡처 바인딩으로 확장한다.
1. 확장 결과를 `OutKind` 문맥으로 재파싱한다.
1. 재파싱 성공 시 기존 AST/파이프라인에 삽입한다.
1. 재파싱 실패 시 확장 스택을 포함한 결정적 진단을 발생시킨다.

positional 규칙:

1. 이름 캡처와 positional 참조를 동시에 허용한다.
1. positional 인덱스는 캡처 선언 순서 기준으로 자동 부여한다: `$0`, `$1`, ...
1. variadic 캡처는 `$n...` 확장을 허용한다.

## 8. 안전성/자원 제한(예산)

무한 확장/폭발을 방지하기 위해 예산을 고정한다.

필수 타입:

1. `MacroExpansionContext`
1. `ExpansionBudget { max_depth, max_steps, max_output_tokens }`

필수 정책:

1. 재귀는 "토큰 소비 없는 재귀"를 금지한다.
1. empty-match 가능한 `*` 반복 본문은 금지한다.
1. 예산 초과 시 즉시 확장을 중단하고 오류를 보고한다.
1. 확장 실패는 침묵 fallback 없이 컴파일 오류로 처리한다.
1. 기본값:
1. AOT 기본값 `64 / 20000 / 200000`
1. JIT 기본값 `32 / 8000 / 80000`
1. 하드 상한:
1. `max_depth <= 256`
1. `max_steps <= 200000`
1. `max_output_tokens <= 1000000`
1. 사용자 입력은 clamp하며, clamp 사실은 경고로 노출한다.

## 9. 진단 모델 (오류 품질 개선 규칙)

필수 진단 코드:

1. `E_MACRO_NO_MATCH`
1. `E_MACRO_AMBIGUOUS`
1. `E_MACRO_REPEAT_EMPTY`
1. `E_MACRO_RECURSION_BUDGET`
1. `E_MACRO_REPARSE_FAIL`

필수 진단 정보:

1. 실패한 매크로 이름 및 호출 위치
1. 실패한 arm 인덱스 또는 매칭 단계
1. 기대 fragment/token 정보
1. 확장 스택(backtrace)
1. 재파싱 실패 시 출력 kind와 파싱 오류 위치

## 10. 남발 방지 정책

매크로 표현력은 높게 제공하되 남발을 제한한다.

1. 기본 문법/기본 타입/핵심 런타임 API 대체 수단으로 매크로를 사용하지 않는다.
1. 표준 라이브러리 핵심 API는 함수/일반 선언 우선 정책을 유지한다.
1. 매크로는 선택적 확장 수단으로 유지하며 호출 시 `$` 접두를 강제한다.
1. 확장 실패는 조용히 우회하지 않고 명시적으로 실패시킨다.

## 11. 컴파일러 파이프라인 통합 지점

기존 파이프라인에 아래 단계가 추가된다.

1. Parse 이후, Pass 이전에 매크로 확장 단계를 수행한다.
1. 확장 결과를 재파싱한 AST를 Pass/NameResolve/Tyck에 그대로 투입한다.
1. Tyck/Pass는 "확장 완료 AST"를 기준으로 동작한다.
1. `parse_macro_decl`, `parse_macro_call`, `parse_token_pattern`를 파서 엔트리로 추가한다.

중요 인터페이스/타입 추가:

1. AST: `MacroDecl`, `MacroGroup`, `MacroArm`, `PatternNode`, `TemplateNode`
1. Expander: `MacroExpansionContext`, `ExpansionBudget`
1. Parser: `parse_macro_decl`, `parse_macro_call`, `parse_token_pattern`
1. Hygiene: `HygieneId`, `GensymTable`

## 12. JIT 안정성 조건

현재 저장소 기준 JIT backend는 scaffold 상태다. 따라서 구현 전제 조건을 고정한다.

1. 매크로 확장은 backend(AOT/JIT) 이전에 완전히 종료되어야 한다.
1. 같은 입력/옵션에서 확장 결과는 결정적(deterministic)이어야 한다.
1. 예산/재귀/반복 안전 규칙은 backend 종류와 무관하게 동일해야 한다.
1. JIT 구현 시에도 확장 실패 정책은 동일하게 "결정적 컴파일 오류"를 유지한다.

## 13. 예제 상태 (`sizeof` 보류)

이번 라운드에서는 `$sizeof`를 구현하지 않는다.

1. 이유:
1. 빌트인 계약점 및 표준 라이브러리 계층이 아직 고정되지 않았다.
1. `sizeof`는 해당 계층 확정 이후 별도 라운드로 추가한다.
1. 현재 회귀 테스트 예시는 일반 typed 매크로(`expr/stmt/item/type`) 조합으로 유지한다.

## 14. 테스트/수용 기준

파서:

1. `macro ... -> {...}` 정상 파싱
1. 잘못된 `macro` 선언 문법 에러 복구
1. `$foo(...)` 호출 파싱

매칭:

1. first-match 동작 검증
1. typed/token 그룹 분기 검증

`with token`:

1. 플래그 OFF에서 선언 차단 진단
1. 플래그 ON에서 선언 파싱 허용
1. 확장 시 미구현 진단으로 결정적 실패

위생성:

1. 캡처 이름과 생성 이름 충돌 방지

positional:

1. `$0`, `$1...` 정상 확장
1. out-of-range 진단

재파싱:

1. `=> expr` 결과가 expr가 아니면 정확한 위치/스택 진단

안정성:

1. empty-repeat 금지
1. no-progress 재귀 금지

## 15. 구현 단계 (Phase 1~3)

### Phase 1

1. 문법/AST/파서 추가
1. typed 그룹 확장기 추가
1. hygiene 기본(gensym) 적용

### Phase 1.5

1. binder 중심 hygiene(`let/set/def param/loop binder`) 고정
1. 예산 기본값(AOT/JIT) + 하드 상한 clamp + 경고 정책 고정
1. `with token` 실험 플래그 게이팅 및 미구현 확장 진단 고정

### Phase 2

1. `with token` 코어급 기능 추가
1. 반복/중첩 반복/재귀 추가
1. 확장 예산 시스템 추가

### Phase 3

1. 진단 품질 고도화
1. 성능 최적화
1. 표준 매크로(`sizeof`) 회귀 검증

## 16. 추적 항목 (후속 TODO)

1. `with token` 모호성 사전 검출 알고리즘 상세
1. 비위생 escape opt-in 설계 상세
1. 확장 결과 캐시 전략(incremental parsing 연계)
1. 매크로 확장 디버그 출력 포맷(`-Xparus` 옵션 연계)
1. JIT backend 구현 이후 동일성 검증 계획
