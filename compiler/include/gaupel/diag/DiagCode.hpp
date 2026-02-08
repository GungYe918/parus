// compiler/include/gaupel/diag/DiagCode.hpp
#pragma once
#include <cstdint>


namespace gaupel::diag {

    enum class Severity : uint8_t {
        kError,
        kWarning,
        kFatal,
    };

    enum class Language : uint8_t {
        kEn,
        kKo,
    };

    enum class Code : uint16_t {
        kInvalidUtf8,   // 올바른 UTF8 문자열이 아님
        
        // generic parse
        kExpectedToken,
        kUnexpectedToken,
        kUnexpectedEof,
        kTooManyErrors,      
        kNestedTernaryNotAllowed,

        // pipe + hole rules
        kPipeRhsMustBeCall,
        // dedicated pipe direction diagnostics
        kPipeFwdRhsMustBeCall, // |> requires RHS call
        kPipeRevLhsMustBeCall, // <| requires LHS call

        kPipeHoleMustBeLabeled,
        kPipeHoleCountMismatch,
        kPipeHolePositionalNotAllowed,

        // call rules
        kCallArgMixNotAllowed,
        kNamedGroupEntryExpectedColon,     // entry must be "label: expr|_"
        kCallOnlyOneNamedGroupAllowed,     // only one "{ ... }" in a call
        kAttrNameExpectedAfterAt,          // '@' must be followed by attr name

        kNamedGroupLabelMustBeIdent,     // label must be identifier (e.g., x: 1)
        kNamedGroupLabelUnderscoreReserved, // '_' cannot be a label; only allowed as value

        // ---- var parsing ----
        kVarDeclTypeAnnotationRequired,   // let requires ': Type'
        kVarDeclTypeAnnotationNotAllowed, // set must NOT have ': Type'

        // fn param default rules
        kFnParamDefaultNotAllowedOutsideNamedGroup, // positional param can't have "= expr"
        kFnParamDefaultExprExpected,                // named-group param has "=", but expr missing

        // fn param named-group count
        kFnOnlyOneNamedGroupAllowed,
        
        // fn body parsing rule
        kFnReturnTypeRequired, // missing '-> ReturnType' in function declaration

        // pub/sub misuse
        kPubSubOnlyAllowedInClass,

        // ---- type parsing ----
        kTypeNameExpected,          // type name (ident) expected
        kTypeArrayMissingRBracket,  // missing ']' in T[]
        kTypeOptionalDuplicate,     // T?? 같은 중복
        kTypeRecovery,              // 타입 파싱 실패 후 동기화

        // ---- while parsing ----
        kWhileHeaderExpectedLParen,   // while ( ... ) 에서 '(' 없음
        kWhileHeaderExpectedRParen,   // while ( ... ) 에서 ')' 없음
        kWhileBodyExpectedBlock,      // while (...) { ... } 에서 block 없음

        // ---- loop parsing ----
        kLoopHeaderExpectedLParen,  // loop ( ... ) 형태인데 '(' 없음
        kLoopHeaderVarExpectedIdent,  // loop (<var> in <iter>)에서 <var>가 ident 아님
        kLoopHeaderExpectedIn,      // loop (v in xs)에서 'in' 필요
        kLoopHeaderExpectedRParen,  // header ')' 필요
        kLoopBodyExpectedBlock,     // loop body '{...}' 필요

        // ---- if-expr parsing ----
        kIfExprThenExpectedBlock,   // if expr then must be "{ ... }"
        kIfExprElseExpectedBlock,   // else branch must be "{ ... }" or "else if ..."
        kIfExprMissingElse,         // if-expr requires else
        kIfExprBranchValueExpected, // then/else block must yield a value (tail expr)

        // ---- expr-block tail rules ----
        kBlockTailSemicolonNotAllowed, // tail value has ';' right before '}'

        // ---- switch parsing ----
        kSwitchHeaderExpectedLParen,      // switch ( ... ) '(' 없음
        kSwitchHeaderExpectedRParen,      // switch ( ... ) ')' 없음
        kSwitchBodyExpectedLBrace,        // switch (...) '{' 없음
        kSwitchBodyExpectedRBrace,        // switch (...) '}' 없음 (복구 실패 시)
        kSwitchCaseExpectedPattern,       // case <pattern> 에서 pattern 토큰 아님
        kSwitchCaseExpectedColon,         // case/default 뒤 ':' 없음
        kSwitchCaseBodyExpectedBlock,     // case/default 본문 block 없음
        kSwitchDefaultDuplicate,          // default 중복
        kSwitchNeedsAtLeastOneCase,       // switch { } 비어있음 (CaseClause+ 위반)
        kSwitchOnlyCaseOrDefaultAllowed,  // switch 내부에 case/default 외 토큰

        // ---- var parsing ----
        kVarMutMustFollowKw,        // "set mut x"만 허용, "set x mut" 금지

        // ---- &, &&관련 ----
        kBorrowOperandMustBePlace,
        kEscapeOperandMustBePlace,
        kEscapeOperandMustNotBeBorrow,

        // =========================
        // passes / sema
        // =========================

        // top-level 규칙 (Rust처럼 top-level은 decl-only)
        kTopLevelMustBeBlock,   // parse_program 결과가 block이 아닐 때
        kTopLevelDeclOnly,      // 최상위에서 stmt 금지

        // name resolve
        kUndefinedName,         // 선언되지 않은 이름 사용
        kDuplicateDecl,         // 같은 스코프 중복 선언
        kShadowing,             // shadowing 발생(경고용)
        kShadowingNotAllowed,   // shadowing을 에러로 승격
        kSetOnUndeclared,       // set이 선언되지 않은 변수에 적용됨

        // ---- use parsing ----
        kUseTextSubstExprExpected,     // use NAME ;  (값 누락)
        kUseTextSubstTrailingTokens,   // use NAME <expr> ... ; (expr 이후 ; 전 잔여 토큰)
    };

} // namespace gaupel::diag