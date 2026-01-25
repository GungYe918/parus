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

        // fn param default rules
        kFnParamDefaultNotAllowedOutsideNamedGroup, // positional param can't have "= expr"
        kFnParamDefaultExprExpected,                // named-group param has "=", but expr missing

        // fn param named-group count
        kFnOnlyOneNamedGroupAllowed,

        // pub/sub misuse
        kPubSubOnlyAllowedInClass,

        // ---- type parsing ----
        kTypeNameExpected,          // type name (ident) expected
        kTypeArrayMissingRBracket,  // missing ']' in T[]
        kTypeOptionalDuplicate,     // T?? 같은 중복
        kTypeRecovery,              // 타입 파싱 실패 후 동기화(선택)

        // ---- loop parsing ----
        kLoopHeaderExpectedLParen,  // loop ( ... ) 형태인데 '(' 없음
        kLoopHeaderExpectedIn,      // loop (v in xs)에서 'in' 필요
        kLoopHeaderExpectedRParen,  // header ')' 필요
        kLoopBodyExpectedBlock,     // loop body '{...}' 필요

        // ---- var parsing ----
        kVarMutMustFollowKw,        // "set mut x"만 허용, "set x mut" 금지
    };

} // namespace gaupel::diag