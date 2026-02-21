# Parus Macro System Model (Design Freeze)

문서 버전: `draft-0.1`  
상태: `Design Freeze (TODO Track)`  
목적: 다음 대화에서 구현을 진행하기 위한 결정 완결 기준 문서

## 1. 목적과 범위

이 문서는 Parus 매크로 시스템의 전체 설계를 고정한다.

1. 표면 문법, 의미 규칙, 위생성, 진단 규칙, 파이프라인 통합 기준을 고정한다.
1. 구현자가 추가 결정을 하지 않아도 되도록 EBNF와 수용 기준을 함께 정의한다.
1. 본 문서는 TODO 트랙 설계 문서이며, 구현 이후 normative 문서로 승격 가능하다.

## 2. 결정 사항 (Non-negotiable)

1. 문서 범위는 전체 설계 + EBNF로 고정한다.
1. 매크로 호출 문법은 `$foo(...)`만 허용한다.
1. `with token`은 `macro_rules!` 코어급 표현력(반복/재귀 포함)을 제공한다.
1. 안전성 우선 정책을 적용한다. 증명 불가능한 경우 보수적으로 실패/진단한다.

## 3. 비목표

1. 본 문서에서 실제 파서/AST 코드 구현을 수행하지 않는다.
1. 본 문서에서 컴파일러 extension 기반 매크로 런타임을 도입하지 않는다.
1. 기본 문법/기본 타입/핵심 런타임 API를 매크로로 대체하는 정책을 허용하지 않는다.

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

## 5. `with token` 허용 범위 (코어급)

`with token`은 다음 기능을 반드시 지원한다.

1. `tt` 캡처
1. typed fragment 캡처(`expr`, `type`, `path`, `ident`, `stmt`, `item`, `block`)
1. 반복 `*`, `+`, `?`
1. 구분자 반복(`$(...),*` 스타일)
1. 중첩 반복
1. 재귀 매크로 호출
1. variadic 포워딩(`tt...`, `$n...`)
1. 리터럴 토큰 매칭(키워드/연산자/구두점)

## 6. 위생성(Hygiene) 모델

기본 위생성은 hygienic이다.

1. 매크로가 생성한 식별자는 gensym 기반으로 call-site와 충돌하지 않아야 한다.
1. 패턴 캡처된 식별자는 call-site 문맥에서 해석한다.
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

## 13. 예제 (`sizeof`) 2종

### 13.1 Typed 그룹 버전

```parus
macro __sizeof_type -> {
    with expr {
        (t: type) => expr {
            __contract::type_size($0)
        };
    }
}

macro sizeof -> {
    with expr {
        (t: type) => expr {
            $__sizeof_type($0)
        };

        (v: expr) => expr {
            $__sizeof_type(type_of($0))
        };
    }
}
```

### 13.2 `with token` 버전

```parus
macro __sizeof_type -> {
    with expr {
        (t: type) => expr {
            __contract::type_size($0)
        };
    }
}

macro sizeof -> {
    with token {
        ($t:type) => expr {
            $__sizeof_type($0)
        };

        ($t:type) => expr {
            $__sizeof_type($0)
        };

        ($e:expr) => expr {
            $__sizeof_type(type_of($0))
        };
    }
}
```

사용 예시:

```parus
field Vec2 {
    x: i32;
    y: i32;
};

def main() -> i32 {
    let s0: usize = $sizeof(i32);
    let v: Vec2 = Vec2 { x: 1, y: 2 };
    let s1: usize = $sizeof(v);
    return (s0 + s1) as i32;
}
```

설계 의도:

1. `sizeof(type)`는 `__contract::type_size(type)`로 내린다.
1. `sizeof(expr)`는 `type_of(expr)` 후 동일 계약으로 내린다.

## 14. 테스트/수용 기준

파서:

1. `macro ... -> {...}` 정상 파싱
1. 잘못된 `macro` 선언 문법 에러 복구
1. `$foo(...)` 호출 파싱

매칭:

1. first-match 동작 검증
1. typed/token 그룹 분기 검증

`with token`:

1. 반복/중첩 반복/구분자 반복
1. 재귀 성공 케이스
1. 예산 초과 실패 케이스

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

