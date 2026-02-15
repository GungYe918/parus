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
        kAmbiguousAmpPrefixChain,      // ambiguous '&' prefix chain (e.g. &&&x)
        kArraySizeExpectedIntLiteral,  // array suffix requires integer literal (T[N])
        kArraySizeInvalidLiteral,      // array size literal is malformed/out of range

        // pipe + hole rules
        kPipeRhsMustBeCall,
        // dedicated pipe direction diagnostics
        kPipeFwdRhsMustBeCall, // |> requires RHS call
        kPipeRevLhsMustBeCall, // <| requires LHS call

        kPipeHoleMustBeLabeled,
        kPipeHoleCountMismatch,
        kPipeHolePositionalNotAllowed,

        // call rules
        kDeclExpected,               // declaration expected in current context
        kFnNameExpected,             // function name identifier is required
        kFnParamNameExpected,        // function parameter name identifier is required
        kFieldNameExpected,          // field declaration name identifier is required
        kFieldMemberNameExpected,    // field member name identifier is required
        kActsNameExpected,           // acts declaration name identifier is required
        kCallArgMixNotAllowed,
        kCallNoArgsAfterNamedGroup,      // no extra args after named-group '{...}'
        kNamedGroupEntryExpectedColon,     // entry must be "label: expr|_"
        kCallOnlyOneNamedGroupAllowed,     // only one "{ ... }" in a call
        kAttrNameExpectedAfterAt,          // '@' must be followed by attr name

        kNamedGroupLabelMustBeIdent,     // label must be identifier (e.g., x: 1)
        kNamedGroupLabelUnderscoreReserved, // '_' cannot be a label; only allowed as value

        // ---- var parsing ----
        kVarDeclTypeAnnotationRequired,   // let requires ': Type'
        kVarDeclTypeAnnotationNotAllowed, // set must NOT have ': Type'
        kVarDeclNameExpected,             // variable name identifier is required
        kVarDeclInitializerExpected,      // '=' present but initializer expr missing
        kSetInitializerRequired,          // set must always have '=' initializer
        kStaticVarExpectedLetOrSet,       // 'static' must be followed by [mut] let/set
        kStaticVarRequiresInitializer,    // static var must have initializer

        // fn param default rules
        kFnParamDefaultNotAllowedOutsideNamedGroup, // positional param can't have "= expr"
        kFnParamDefaultExprExpected,                // named-group param has "=", but expr missing

        // fn param named-group count
        kFnOnlyOneNamedGroupAllowed,
        kActsForNotSupported,             // acts for T is not supported yet in parser
        kActsMemberExportNotAllowed,      // member-level export inside acts is not allowed
        
        // fn body parsing rule
        kFnReturnTypeRequired, // missing '-> ReturnType' in function declaration

        // pub/sub misuse
        kPubSubOnlyAllowedInClass,

        // ---- type parsing ----
        kTypeFnSignatureExpected, // type-context 'fn' must be followed by '('
        kTypeNameExpected,          // type name (ident) expected
        kTypeArrayMissingRBracket,  // missing ']' in T[]
        kTypeOptionalDuplicate,     // T?? 같은 중복
        kTypeRecovery,              // 타입 파싱 실패 후 동기화
        kCastTargetTypeExpected,    // "as/as?/as!" 뒤에 타입 필요
        kTypeInternalNameReserved,  // internal-only type name used in source

        // ---- while parsing ----
        kWhileHeaderExpectedLParen,   // while ( ... ) 에서 '(' 없음
        kWhileHeaderExpectedRParen,   // while ( ... ) 에서 ')' 없음
        kWhileBodyExpectedBlock,      // while (...) { ... } 에서 block 없음
        kDoBodyExpectedBlock,         // do { ... } 에서 block 없음
        kDoWhileExpectedLParen,       // do { ... } while (...) 에서 '(' 없음
        kDoWhileExpectedRParen,       // do { ... } while (...) 에서 ')' 없음
        kDoWhileExpectedSemicolon,    // do { ... } while (...); 에서 ';' 없음
        kBareBlockScopePreferDo,      // 단독 '{...}' 블록은 do { ... } 사용 권장 (warning)

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
        kBlockTailExprRequired,        // value-required block is missing tail expr

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
        kBorrowMutRequiresMutablePlace,
        kBorrowMutConflict,
        kBorrowSharedConflictWithMut,
        kBorrowMutConflictWithShared,
        kBorrowMutDirectAccessConflict,
        kBorrowSharedWriteConflict,
        kBorrowEscapeFromReturn,
        kBorrowEscapeToStorage,
        kUseAfterEscapeMove,
        kEscapeWhileMutBorrowActive,
        kEscapeWhileBorrowActive,
        kEscapeRequiresStaticOrBoundary,
        kSirUseAfterEscapeMove,          // SIR pass: use-after-move via escape
        kSirEscapeBoundaryViolation,     // SIR pass: escape handle must be boundary-consumed or static-origin
        kSirEscapeMustNotMaterialize,    // SIR pass: escape handle must not be materialized into non-static locals

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
        
        // ---- use parsing ----
        kUseTextSubstExprExpected,     // use NAME ;  (값 누락)
        kUseTextSubstTrailingTokens,   // use NAME <expr> ... ; (expr 이후 ; 전 잔여 토큰)

        // =========================
        // tyck (TYPE CHECK)
        // =========================
        kTypeErrorGeneric,      // args[0] = message
        kTypeLetInitMismatch,   // args[0]=var, args[1]=expected, args[2]=got
        kTypeSetAssignMismatch, // args[0]=var, args[1]=expected, args[2]=got
        kTypeArgCountMismatch,  // args[0]=expected, args[1]=got
        kTypeArgTypeMismatch,   // args[0]=index, args[1]=expected, args[2]=got
        kTypeReturnOutsideFn,   // (no args)
        kTypeReturnExprRequired,// (no args)
        kTypeBreakValueOnlyInLoopExpr, // break value is only allowed in loop expression
        kTypeUnaryBangMustBeBool,// args[0]=got
        kTypeBinaryOperandsMustMatch,// args[0]=lhs, args[1]=rhs
        kTypeCompareOperandsMustMatch,// args[0]=lhs, args[1]=rhs
        kTypeBorrowNotAllowedInPureComptime, // (no args)
        kTypeEscapeNotAllowedInPureComptime, // (no args)
        kTypeMismatch,          // args[0]=expected, args[1]=got
        kTypeNotCallable,       // args[0]=got_type
        kTypeCondMustBeBool,    // args[0]=got_type
        kTypeIndexMustBeUSize,  // args[0]=got_type
        kTypeIndexNonArray,     // args[0]=base_type
        kSetCannotInferFromNull, // set <name> = null; is not allowed
        kMissingReturn,         // return is missing

        // ---- place requirement (tyck) ----
        kAssignLhsMustBePlace,      // (no args)
        kPostfixOperandMustBePlace, // (no args)

        // ---- integer literal / inference ----
        kIntLiteralInvalid,          // args[0]=text
        kIntLiteralOverflow,         // args[0]=text, args[1]=target (e.g., "i128" or "u128")
        kIntLiteralNeedsTypeContext, // (no args) "{integer}" requires context
        kIntLiteralDoesNotFit,       // args[0]=target, args[1]=value (shortened)
        kIntToFloatNotAllowed,       // args[0]=float_type

        kBreakOutsideLoop,
        kContinueOutsideLoop,
        kBlockExprValueExpected,

        kTypeParamTypeRequired,        // args[0]=param_name
        kTypeDuplicateParam,           // args[0]=param_name
        kTypeParamDefaultMismatch,     // args[0]=param_name, args[1]=expected, args[2]=got
        kTypeAssignMismatch,           // args[0]=expected, args[1]=got
        kTypeTernaryCondMustBeBool,    // args[0]=got_type
        kTypeUnresolvedHole,           // (no args)

        // type casting
        kTyckCastMissingOperand,
        kTyckCastMissingTargetType,
        kTyckCastNullToNonOptional,   // arg0: target type string
        kTyckCastNotAllowed,          // arg0: from, arg1: to

        // ---- ??, ??= ----
        kTypeNullCoalesceLhsMustBeOptional,
        kTypeNullCoalesceRhsMismatch,

        kTypeNullCoalesceAssignLhsMustBeOptional,
        kTypeNullCoalesceAssignRhsMismatch,

        // array / field diagnostics
        kTypeArrayLiteralEmptyNeedsContext,
        kTypeFieldMemberRangeInvalid,
        kTypeFieldMemberMustBePodBuiltin, // args[0]=member, args[1]=got_type

        // ---- mut check ----
        kWriteToImmutable
        
    };

} // namespace gaupel::diag
