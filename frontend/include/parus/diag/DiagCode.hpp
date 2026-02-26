// frontend/include/parus/diag/DiagCode.hpp
#pragma once
#include <cstdint>


namespace parus::diag {

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
        kMacroNoMatch,                 // macro arm/group matching failed
        kMacroAmbiguous,               // multiple macro arms matched ambiguously
        kMacroRepeatEmpty,             // repetition body can match empty
        kMacroRecursionBudget,         // expansion recursion/steps budget exceeded
        kMacroReparseFail,             // expanded output failed to reparse for OutKind
        kMacroTokenExperimentalRequired, // with token requires explicit experimental flag
        kMacroTokenUnimplemented,      // with token expansion path is not implemented yet

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
        kFieldMemberMutNotAllowed,   // mut is not allowed on field members
        kActsNameExpected,           // acts declaration name identifier is required
        kCallArgMixNotAllowed,
        kAttrNameExpectedAfterAt,          // '@' must be followed by attr name

        // ---- var parsing ----
        kVarDeclTypeAnnotationRequired,   // let requires ': Type'
        kVarDeclTypeAnnotationNotAllowed, // set must NOT have ': Type'
        kVarDeclNameExpected,             // variable name identifier is required
        kVarDeclInitializerExpected,      // '=' present but initializer expr missing
        kSetInitializerRequired,          // set must always have '=' initializer
        kStaticVarExpectedLetOrSet,       // 'static' must be followed by [mut] let/set
        kStaticVarRequiresInitializer,    // static var must have initializer

        // def param default rules
        kFnParamDefaultNotAllowedOutsideNamedGroup, // positional param can't have "= expr"
        kFnParamDefaultExprExpected,                // named-group param has "=", but expr missing

        // def param named-group count
        kFnOnlyOneNamedGroupAllowed,
        kActsForNotSupported,             // acts for T is not supported yet in parser
        kActsMemberExportNotAllowed,      // member-level export inside acts is not allowed
        kActsForTypeExpected,             // `acts for` requires a target type
        kOperatorDeclOnlyInActsFor,       // operator(...) is only allowed in acts-for forms
        kOperatorKeyExpected,             // operator(<key>) key is missing/invalid
        kOperatorSelfFirstParamRequired,  // operator(...) first parameter must be `self`
        kProtoMemberBodyNotAllowed,       // reserved (legacy): proto member body not allowed
        kProtoMemberBodyMixNotAllowed,    // proto members must be all declaration-only or all default-body
        kProtoOperatorNotAllowed,         // operator declaration is forbidden inside proto
        kProtoRequireTypeNotBool,         // require(expr) must evaluate to bool
        kProtoRequireExprTooComplex,      // require(expr) supports simple boolean expression only (v1)
        kProtoImplTargetNotSupported,     // implements target is not a known proto
        kProtoImplMissingMember,          // implementation type misses required proto member
        kProtoConstraintUnsatisfied,      // generic/proto constraint not satisfied
        kClassLifecycleDefaultParamNotAllowed, // init()/deinit() = default only supports empty parameter list
        kClassLifecycleSelfNotAllowed,    // class lifecycle members must not declare self receiver

        // def body parsing rule
        kFnReturnTypeRequired, // missing '-> ReturnType' in function declaration

        // pub/sub misuse
        kPubSubOnlyAllowedInClass,

        // ---- type parsing ----
        kTypeFnSignatureExpected, // type-context 'def' must be followed by '('
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
        kVarMutMustFollowKw,        // mut must appear right after declaration keyword (let/set/static)

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
        kImportDepNotDeclared,  // import head is not listed in bundle deps
        kSymbolNotExportedFileScope, // cross-file reference requires export
        kSymbolNotExportedBundleScope, // cross-bundle reference requires export
        kSymbolAmbiguousOverload, // overload candidates remain ambiguous after visibility ranking
        kExportCollisionSameFolder, // same-folder export symbol collision with identical signature
        kNestNotUsedForModuleResolution, // nest is namespace-only and not used for module-head resolution
        kExportIndexMissing,    // export index file missing
        kExportIndexSchema,     // export index parse/schema mismatch
        
        // ---- use parsing ----
        kUseTextSubstExprExpected,     // use NAME ;  (값 누락)
        kUseTextSubstTrailingTokens,   // use NAME <expr> ... ; (expr 이후 ; 전 잔여 토큰)
        kUseNestPathExpectedNamespace, // use nest path가 namespace 경로가 아님
        kUseNestAliasAsOnly,           // use nest path = alias 금지(as만 허용)
        kUseNestAliasPreferred,        // namespace path alias는 use nest ... 권장 (warning)

        // =========================
        // tyck (TYPE CHECK)
        // =========================
        kTypeErrorGeneric,      // args[0] = message
        kTypeLetInitMismatch,   // args[0]=var, args[1]=expected, args[2]=got
        kTypeSetAssignMismatch, // args[0]=var, args[1]=expected, args[2]=got
        kTypeArgCountMismatch,  // args[0]=expected, args[1]=got
        kTypeArgTypeMismatch,   // args[0]=index, args[1]=expected, args[2]=got
        kOverloadDeclConflict,  // args[0]=def, args[1]=reason
        kOverloadNoMatchingCall,// args[0]=def, args[1]=call_signature
        kOverloadAmbiguousCall, // args[0]=def, args[1]=candidate_list
        kMangleSymbolCollision, // args[0]=mangled_symbol, args[1]=lhs, args[2]=rhs
        kAbiCOverloadNotAllowed,// args[0]=def
        kAbiCNamedGroupNotAllowed, // args[0]=def
        kAbiCTypeNotFfiSafe,       // args[0]=entity, args[1]=type
        kAbiCGlobalMustBeStatic,   // args[0]=name
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
        kDotMethodSelfRequired, // dot method calls require self receiver in the first parameter
        kClassCtorMissingInit,  // class constructor call requires init overload
        kClassProtoPathCallRemoved, // class/proto member path call removed (use dot call)

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
        kFieldInitTypeExpected,          // args[0]=type path
        kFieldInitUnknownMember,         // args[0]=type, args[1]=member
        kFieldInitDuplicateMember,       // args[0]=member
        kFieldInitMissingMember,         // args[0]=type, args[1]=member
        kFieldInitNonOptionalNull,       // args[0]=member, args[1]=type
        kFieldInitEmptyNotAllowed,       // args[0]=type

        // ---- mut check ----
        kWriteToImmutable
        
    };

} // namespace parus::diag
