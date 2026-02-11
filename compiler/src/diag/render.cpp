// compiler/src/diag/render.cpp
#include <gaupel/diag/Render.hpp>

#include <array>
#include <algorithm>
#include <sstream>


namespace gaupel::diag {

    static constexpr uint32_t digits10(uint32_t v) {
        uint32_t d = 1;
        while (v >= 10) { v /= 10; ++d; }
        return d;
    }

    static std::string replace_all(std::string s, std::string_view from, std::string_view to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }

        return s;
    }

    static std::string format_template(std::string templ, const std::vector<std::string>& args) {
        for (size_t i = 0; i < args.size(); ++i) {
            std::string key = "{" + std::to_string(i) + "}";
            templ = replace_all(std::move(templ), key, args[i]);
        }
        return templ;
    }

    static std::string_view code_name(Code c) {
        switch (c) {
            case Code::kInvalidUtf8: return "InvalidUtf8";
            case Code::kExpectedToken: return "ExpectedToken";
            case Code::kUnexpectedToken: return "UnexpectedToken";
            case Code::kUnexpectedEof: return "UnexpectedEof";
            case Code::kTooManyErrors: return "TooManyErrors";
            case Code::kNestedTernaryNotAllowed: return "NestedTernaryNotAllowed";
            case Code::kPipeRhsMustBeCall: return "PipeRhsMustBeCall";
            case Code::kPipeFwdRhsMustBeCall: return "PipeFwdRhsMustBeCall";
            case Code::kPipeRevLhsMustBeCall: return "PipeRevLhsMustBeCall";
            case Code::kPipeHoleMustBeLabeled: return "PipeHoleMustBeLabeled";
            case Code::kPipeHoleCountMismatch: return "PipeHoleCountMismatch";
            case Code::kPipeHolePositionalNotAllowed: return "PipeHolePositionalNotAllowed";
            case Code::kCallArgMixNotAllowed: return "CallArgMixNotAllowed";
            case Code::kNamedGroupEntryExpectedColon: return "NamedGroupEntryExpectedColon";
            case Code::kCallOnlyOneNamedGroupAllowed: return "CallOnlyOneNamedGroupAllowed";
            case Code::kFnReturnTypeRequired: return "FnReturnTypeRequired";
            case Code::kAttrNameExpectedAfterAt:      return "AttrNameExpectedAfterAt";
            case Code::kFnParamDefaultNotAllowedOutsideNamedGroup: return "FnParamDefaultNotAllowedOutsideNamedGroup";
            case Code::kFnParamDefaultExprExpected:                return "FnParamDefaultExprExpected";
            case Code::kNamedGroupLabelMustBeIdent: return "NamedGroupLabelMustBeIdent";
            case Code::kNamedGroupLabelUnderscoreReserved: return "NamedGroupLabelUnderscoreReserved";

            case Code::kVarDeclTypeAnnotationRequired: return "VarDeclTypeAnnotationRequired";
            case Code::kVarDeclTypeAnnotationNotAllowed: return "VarDeclTypeAnnotationNotAllowed";

            case Code::kFnOnlyOneNamedGroupAllowed: return "FnOnlyOneNamedGroupAllowed";
            case Code::kPubSubOnlyAllowedInClass: return "PubSubOnlyAllowedInClass";
            case Code::kTypeNameExpected: return "TypeNameExpected";
            case Code::kTypeArrayMissingRBracket: return "TypeArrayMissingRBracket";
            case Code::kTypeOptionalDuplicate: return "TypeOptionalDuplicate";
            case Code::kTypeRecovery: return "TypeRecovery";
            case Code::kWhileHeaderExpectedLParen: return "WhileHeaderExpectedLParen";
            case Code::kWhileHeaderExpectedRParen: return "WhileHeaderExpectedRParen";
            case Code::kWhileBodyExpectedBlock:    return "WhileBodyExpectedBlock";
            case Code::kLoopHeaderExpectedLParen:    return "LoopHeaderExpectedLParen";
            case Code::kLoopHeaderVarExpectedIdent:  return "LoopHeaderVarExpectedIdent";
            case Code::kLoopHeaderExpectedIn:        return "LoopHeaderExpectedIn";
            case Code::kLoopHeaderExpectedRParen:    return "LoopHeaderExpectedRParen";
            case Code::kLoopBodyExpectedBlock:       return "LoopBodyExpectedBlock";
            case Code::kIfExprThenExpectedBlock: return "IfExprThenExpectedBlock";
            case Code::kIfExprElseExpectedBlock: return "IfExprElseExpectedBlock";
            case Code::kIfExprMissingElse: return "IfExprMissingElse";
            case Code::kIfExprBranchValueExpected: return "IfExprBranchValueExpected";
            case Code::kBlockTailSemicolonNotAllowed: return "BlockTailSemicolonNotAllowed";
            case Code::kSwitchHeaderExpectedLParen:  return "SwitchHeaderExpectedLParen";
            case Code::kSwitchHeaderExpectedRParen:  return "SwitchHeaderExpectedRParen";
            case Code::kSwitchBodyExpectedLBrace:    return "SwitchBodyExpectedLBrace";
            case Code::kSwitchBodyExpectedRBrace:    return "SwitchBodyExpectedRBrace";
            case Code::kSwitchCaseExpectedPattern:   return "SwitchCaseExpectedPattern";
            case Code::kSwitchCaseExpectedColon:     return "SwitchCaseExpectedColon";
            case Code::kSwitchCaseBodyExpectedBlock: return "SwitchCaseBodyExpectedBlock";
            case Code::kSwitchDefaultDuplicate:      return "SwitchDefaultDuplicate";
            case Code::kSwitchNeedsAtLeastOneCase:   return "SwitchNeedsAtLeastOneCase";
            case Code::kSwitchOnlyCaseOrDefaultAllowed: return "SwitchOnlyCaseOrDefaultAllowed";
            case Code::kVarMutMustFollowKw: return "VarMutMustFollowKw";
            case Code::kBorrowOperandMustBePlace: return "BorrowOperandMustBePlace";
            case Code::kEscapeOperandMustBePlace: return "EscapeOperandMustBePlace";
            case Code::kEscapeOperandMustNotBeBorrow: return "EscapeOperandMustNotBeBorrow";

            case Code::kTopLevelMustBeBlock: return "TopLevelMustBeBlock";
            case Code::kTopLevelDeclOnly: return "TopLevelDeclOnly";

            case Code::kUndefinedName: return "UndefinedName";
            case Code::kDuplicateDecl: return "DuplicateDecl";
            case Code::kShadowing: return "Shadowing";
            case Code::kShadowingNotAllowed: return "ShadowingNotAllowed";

            case Code::kUseTextSubstExprExpected:   return "UseTextSubstExprExpected";
            case Code::kUseTextSubstTrailingTokens: return "UseTextSubstTrailingTokens";

            // =========================
            // tyck (TYPE CHECK)
            // =========================
            case Code::kTypeErrorGeneric:     return "TypeErrorGeneric";
            case Code::kTypeLetInitMismatch:  return "TypeLetInitMismatch";
            case Code::kTypeSetAssignMismatch:return "TypeSetAssignMismatch";
            case Code::kTypeArgCountMismatch: return "TypeArgCountMismatch";
            case Code::kTypeArgTypeMismatch:  return "TypeArgTypeMismatch";
            case Code::kTypeReturnOutsideFn:  return "TypeReturnOutsideFn";
            case Code::kTypeReturnExprRequired:return "TypeReturnExprRequired";
            case Code::kTypeUnaryBangMustBeBool:return "TypeUnaryBangMustBeBool";
            case Code::kTypeBinaryOperandsMustMatch:return "TypeBinaryOperandsMustMatch";
            case Code::kTypeCompareOperandsMustMatch:return "TypeCompareOperandsMustMatch";
            case Code::kTypeBorrowNotAllowedInPureComptime:return "TypeBorrowNotAllowedInPureComptime";
            case Code::kTypeEscapeNotAllowedInPureComptime:return "TypeEscapeNotAllowedInPureComptime";
            case Code::kTypeMismatch:         return "TypeMismatch";
            case Code::kTypeNotCallable:      return "TypeNotCallable";
            case Code::kTypeCondMustBeBool:   return "TypeCondMustBeBool";
            case Code::kTypeIndexMustBeUSize: return "TypeIndexMustBeUSize";
            case Code::kTypeIndexNonArray:    return "TypeIndexNonArray";
            case Code::kSetCannotInferFromNull: return "SetCannotInferFromNull";
            case Code::kMissingReturn: return "MissingReturn";

            case Code::kAssignLhsMustBePlace: return "AssignLhsMustBePlace";
            case Code::kPostfixOperandMustBePlace: return "PostfixOperandMustBePlace";

            case Code::kIntLiteralInvalid: return "IntLiteralInvalid";
            case Code::kIntLiteralOverflow: return "IntLiteralOverflow";
            case Code::kIntLiteralNeedsTypeContext: return "IntLiteralNeedsTypeContext";
            case Code::kIntLiteralDoesNotFit: return "IntLiteralDoesNotFit";
            case Code::kIntToFloatNotAllowed: return "IntToFloatNotAllowed";

            case Code::kBreakOutsideLoop: return "BreakOutsideLoop";
            case Code::kContinueOutsideLoop: return "ContinueOutsideLoop";
            case Code::kBlockExprValueExpected: return "BlockExprValueExpected";

            case Code::kTypeParamTypeRequired:     return "TypeParamTypeRequired";
            case Code::kTypeDuplicateParam:        return "TypeDuplicateParam";
            case Code::kTypeParamDefaultMismatch:  return "TypeParamDefaultMismatch";
            case Code::kTypeAssignMismatch:        return "TypeAssignMismatch";
            case Code::kTypeTernaryCondMustBeBool: return "TypeTernaryCondMustBeBool";
            case Code::kTypeUnresolvedHole:        return "TypeUnresolvedHole";

            case Code::kTyckCastMissingOperand: return "TyckCastMissingOperand";
            case Code::kTyckCastMissingTargetType: return "TyckCastMissingTargetType";
            case Code::kTyckCastNullToNonOptional: return "TyckCastNullToNonOptional";
            case Code::kTyckCastNotAllowed: return "TyckCastNotAllowed";

            case Code::kTypeNullCoalesceLhsMustBeOptional: return "TypeNullCoalesceLhsMustBeOptional";
            case Code::kTypeNullCoalesceRhsMismatch:       return "TypeNullCoalesceRhsMismatch";
            case Code::kTypeNullCoalesceAssignLhsMustBeOptional: return "TypeNullCoalesceAssignLhsMustBeOptional";
            case Code::kTypeNullCoalesceAssignRhsMismatch:       return "TypeNullCoalesceAssignRhsMismatch";
        }

        return "Unknown";
    }


    static std::string template_en(Code c) {
        switch (c) {
            // args: {0}=byte offset, {1}=byte hex
            case Code::kInvalidUtf8: return "invalid UTF-8 sequence starting at byte offset {0} (byte=0x{1})";
            case Code::kExpectedToken: return "expected '{0}'";
            case Code::kUnexpectedToken: return "unexpected token '{0}'";
            case Code::kUnexpectedEof:  return "unexpected end of file; expected {0}";
            case Code::kTooManyErrors:  return "too many errors emitted; parsing stopped";
            case Code::kNestedTernaryNotAllowed: return "nested ternary operator is not allowed";
            case Code::kPipeRhsMustBeCall: return "pipe operator requires a function call on the required side";
            case Code::kPipeFwdRhsMustBeCall: return "pipe operator '|>' requires a function call on the right-hand side";
            case Code::kPipeRevLhsMustBeCall: return "pipe operator '<|' requires a function call on the left-hand side";
            case Code::kPipeHoleMustBeLabeled: return "hole '_' must appear as a labeled argument value (e.g., a: _)";
            case Code::kPipeHoleCountMismatch: return "pipe call must contain exactly one labeled hole '_' (found {0})";
            case Code::kPipeHolePositionalNotAllowed: return "hole '_' is not allowed as a positional argument in pipe calls";
            case Code::kCallArgMixNotAllowed: return "mixing labeled and positional arguments is not allowed";
            case Code::kNamedGroupEntryExpectedColon: return "named-group entry must be 'label: expr' or 'label: _'";
            case Code::kCallOnlyOneNamedGroupAllowed: return "only one named-group '{ ... }' is allowed in a call";
            case Code::kFnReturnTypeRequired: return "function return type is required (use: fn name(...) -> T { ... })";
            case Code::kAttrNameExpectedAfterAt: return "attribute name expected after '@'";
            case Code::kFnParamDefaultNotAllowedOutsideNamedGroup: return "default value is only allowed inside named-group '{ ... }'";
            case Code::kFnParamDefaultExprExpected: return "default expression expected after '='";
            case Code::kNamedGroupLabelMustBeIdent: return "named-group label must be an identifier";
            case Code::kNamedGroupLabelUnderscoreReserved: return "'_' cannot be used as a named-group label; use it only as a value (e.g., x: _)";

            case Code::kVarDeclTypeAnnotationRequired: return "type annotation is required for 'let' (use: let x: T = ...;)";
            case Code::kVarDeclTypeAnnotationNotAllowed: return "type annotation is not allowed for 'set' (use: set x = ...;)";

            case Code::kFnOnlyOneNamedGroupAllowed: return "function parameters allow at most one named-group '{ ... }'";
            case Code::kPubSubOnlyAllowedInClass: return "'pub'/'sub' is only allowed inside a class;";
            case Code::kTypeNameExpected: return "type name expected";
            case Code::kTypeArrayMissingRBracket: return "array type suffix requires closing ']'";
            case Code::kTypeOptionalDuplicate: return "duplicate optional suffix '?'";
            case Code::kTypeRecovery: return "failed to parse type; recovered";
            case Code::kWhileHeaderExpectedLParen: return "expected '(' after 'while'";
            case Code::kWhileHeaderExpectedRParen: return "expected ')' to close while header";
            case Code::kWhileBodyExpectedBlock:    return "expected while body block '{ ... }'";
            case Code::kLoopHeaderExpectedLParen:   return "expected '(' after 'loop' header";
            case Code::kLoopHeaderVarExpectedIdent: return "loop header variable must be an identifier";
            case Code::kLoopHeaderExpectedIn:       return "expected 'in' in loop header (e.g., loop (v in xs))";
            case Code::kLoopHeaderExpectedRParen:   return "expected ')' to close loop header";
            case Code::kLoopBodyExpectedBlock:      return "expected loop body block '{ ... }'";
            
            case Code::kIfExprThenExpectedBlock: return "if-expression requires a then-branch block '{ ... }'";
            case Code::kIfExprElseExpectedBlock: return "if-expression requires an else-branch block '{ ... }' or 'else if ...'";
            case Code::kIfExprMissingElse: return "if-expression requires an 'else' branch";
            case Code::kIfExprBranchValueExpected: return "if-expression branch must yield a value (remove trailing ';' or add a tail expression)";
            case Code::kBlockTailSemicolonNotAllowed: return "tail value in a block must not end with ';'";

            case Code::kSwitchHeaderExpectedLParen: return "expected '(' after 'switch'";
            case Code::kSwitchHeaderExpectedRParen: return "expected ')' to close switch header";
            case Code::kSwitchBodyExpectedLBrace:   return "expected '{' to start switch body";
            case Code::kSwitchBodyExpectedRBrace:   return "expected '}' to close switch body";
            case Code::kSwitchCaseExpectedPattern:  return "case pattern expected (literal/ident)";
            case Code::kSwitchCaseExpectedColon:    return "expected ':' after case/default label";
            case Code::kSwitchCaseBodyExpectedBlock:return "expected case/default body block '{ ... }'";
            case Code::kSwitchDefaultDuplicate:     return "duplicate 'default' clause in switch";
            case Code::kSwitchNeedsAtLeastOneCase:  return "switch must contain at least one 'case' clause";
            case Code::kSwitchOnlyCaseOrDefaultAllowed: return "only 'case'/'default' clauses are allowed inside switch body";
            case Code::kVarMutMustFollowKw: return "'mut' must appear immediately after 'let'/'set' (e.g., 'set mut x = ...')";
            case Code::kBorrowOperandMustBePlace: return "& operand must be a place expression";
            case Code::kEscapeOperandMustBePlace: return "&& operand must be a place expression";
            case Code::kEscapeOperandMustNotBeBorrow: return "&& cannot be applied to a borrow operand";

            case Code::kTopLevelMustBeBlock: return "internal: program root must be a block";
            case Code::kTopLevelDeclOnly: return "top-level allows declarations only";

            case Code::kUndefinedName: return "use of undeclared name '{0}'";
            case Code::kDuplicateDecl: return "duplicate declaration '{0}' in the same scope";
            case Code::kShadowing: return "declaration '{0}' shadows an outer declaration";
            case Code::kShadowingNotAllowed: return "shadowing is not allowed: '{0}'";

            case Code::kUseTextSubstExprExpected: return "use substitution requires an expression (e.g., use NAME 123;)";
            case Code::kUseTextSubstTrailingTokens: return "unexpected tokens after use substitution expression; expected ';'";

            // =========================
            // tyck (TYPE CHECK)
            // =========================
            case Code::kTypeErrorGeneric: /* args[0] = message */ return "{0}";
            case Code::kTypeLetInitMismatch: return "cannot initialize let '{0}': expected {1}, got {2}";
            case Code::kTypeSetAssignMismatch:return "cannot assign to '{0}': expected {1}, got {2}";
            case Code::kTypeArgCountMismatch: return "argument count mismatch: expected {0}, got {1}";
            case Code::kTypeArgTypeMismatch:  return "argument type mismatch at #{0}: expected {1}, got {2}";
            case Code::kTypeReturnOutsideFn:  return "return outside of function";
            case Code::kTypeReturnExprRequired: return "return expression is required (function does not return unit)";
            case Code::kTypeUnaryBangMustBeBool:return "operator '!' requires bool (got {0})";
            case Code::kTypeBinaryOperandsMustMatch:return "binary arithmetic requires both operands to have the same type (lhs={0}, rhs={1})";
            case Code::kTypeCompareOperandsMustMatch:return "comparison requires both operands to have the same type (lhs={0}, rhs={1})";
            case Code::kTypeBorrowNotAllowedInPureComptime:return "borrow '&' is not allowed in pure/comptime functions";
            case Code::kTypeEscapeNotAllowedInPureComptime:return "escape '&&' is not allowed in pure/comptime functions";
            case Code::kTypeMismatch: /* args[0]=expected, args[1]=got */ return "type mismatch: expected {0}, got {1}";
            case Code::kTypeNotCallable: /* args[0]=got_type */ return "cannot call non-function type {0}";
            case Code::kTypeCondMustBeBool: /* args[0]=got_type */ return "condition must be bool (got {0})";
            case Code::kTypeIndexMustBeUSize: /* args[0]=got_type */ return "index expression must be usize (got {0})";
            case Code::kTypeIndexNonArray: /* args[0]=base_type */ return "cannot index non-array type {0}";
            case Code::kSetCannotInferFromNull: /* args[0]=name (optional) */ return "cannot infer type from null in 'set' (use: let {0}: T? = null; with an explicit optional type)";
            case Code::kMissingReturn: return "missing return";

            case Code::kIntLiteralInvalid: return "invalid integer literal '{0}'";
            case Code::kIntLiteralOverflow: return "integer literal '{0}' overflows target type {1}";

            case Code::kIntLiteralNeedsTypeContext: return "integer literal needs a type context; add an explicit type (e.g., i32) or provide a typed destination";
            case Code::kIntLiteralDoesNotFit: return "integer literal does not fit into '{0}'";
            case Code::kIntToFloatNotAllowed: return "cannot use a deferred integer in {0} context (no implicit int->float)";

            case Code::kBreakOutsideLoop: return "break is only allowed inside a loop";
            case Code::kContinueOutsideLoop: return "continue is only allowed inside a loop";
            case Code::kBlockExprValueExpected: return "value expected: block expression in value context must have a tail expression";

            case Code::kAssignLhsMustBePlace: return "assignment left-hand side must be a place expression (ident/index)";
            case Code::kPostfixOperandMustBePlace: return "postfix operator requires a place expression (ident/index)";

            case Code::kTypeParamTypeRequired: return "parameter '{0}' requires an explicit type";
            case Code::kTypeDuplicateParam: return "duplicate parameter name '{0}'";
            case Code::kTypeParamDefaultMismatch: return "default value type mismatch for parameter '{0}': expected {1}, got {2}";
            case Code::kTypeAssignMismatch: return "cannot assign: expected {0}, got {1}";
            case Code::kTypeTernaryCondMustBeBool: return "ternary condition must be bool (got {0})";
            case Code::kTypeUnresolvedHole: return "unresolved hole '_' in expression";

            case Code::kTyckCastMissingOperand: return "cast expression is missing its operand";
            case Code::kTyckCastMissingTargetType: return "cast expression is missing its target type";
            case Code::kTyckCastNullToNonOptional: return "cannot cast 'null' to non-optional type '{0}'";
            case Code::kTyckCastNotAllowed: return "cast not allowed: '{0}' -> '{1}'";

            // args: {0}=lhs_type
            case Code::kTypeNullCoalesceLhsMustBeOptional: return "operator '??' requires an optional lhs (got {0})";
            // args: {0}=elem_type, {1}=rhs_type
            case Code::kTypeNullCoalesceRhsMismatch: return "operator '??' requires rhs assignable to {0} (got {1})";
            // args: {0}=lhs_type
            case Code::kTypeNullCoalesceAssignLhsMustBeOptional: return "operator '??=' requires an optional lhs (got {0})";
            // args: {0}=elem_type, {1}=rhs_type
            case Code::kTypeNullCoalesceAssignRhsMismatch: return "operator '??=' requires rhs assignable to {0} (got {1})";
        }

        return "unknown diagnostic";
    }

    static std::string template_ko(Code c) {
        switch (c) {
            // args: {0}=byte offset, {1}=byte hex
            case Code::kInvalidUtf8: return "UTF-8 시퀀스가 바이트 오프셋 {0}에서 깨졌습니다 (바이트=0x{1})";
            case Code::kExpectedToken: return "'{0}'이(가) 필요합니다";
            case Code::kUnexpectedToken: return "예상치 못한 토큰 '{0}'";
            case Code::kUnexpectedEof:  return "예상치 못한 파일 끝(EOF)입니다; {0}이(가) 필요합니다";
            case Code::kTooManyErrors:  return "오류가 너무 많아 파싱을 중단합니다";
            case Code::kNestedTernaryNotAllowed: return "삼항 연산자 중첩은 허용되지 않습니다";
            case Code::kPipeRhsMustBeCall: return "파이프 연산자는 필요한 쪽에 함수 호출이 있어야 합니다";
            case Code::kPipeFwdRhsMustBeCall: return "파이프 연산자 '|>'의 오른쪽은 함수 호출이어야 합니다";
            case Code::kPipeRevLhsMustBeCall: return "파이프 연산자 '<|'의 왼쪽은 함수 호출이어야 합니다";
            case Code::kPipeHoleMustBeLabeled: return "'_'는 라벨 인자 값 위치에만 올 수 있습니다(예: a: _)";
            case Code::kPipeHoleCountMismatch: return "파이프 호출에는 라벨 인자 값으로 '_'가 정확히 1개 있어야 합니다(현재 {0}개)";
            case Code::kPipeHolePositionalNotAllowed: return "'_'는 파이프 호출에서 위치 인자로 사용할 수 없습니다";
            case Code::kCallArgMixNotAllowed: return "라벨 인자와 위치 인자를 섞어 호출할 수 없습니다";
            case Code::kNamedGroupEntryExpectedColon: return "named-group entry는 'label: expr' 또는 'label: _' 형태여야 합니다";
            case Code::kFnReturnTypeRequired: return "함수 반환 타입이 필요합니다 (예: fn name(...) -> T { ... })";
            case Code::kCallOnlyOneNamedGroupAllowed: return "호출 인자에서 named-group '{ ... }'는 1개만 허용됩니다";
            case Code::kAttrNameExpectedAfterAt: return "'@' 뒤에는 attribute 이름이 와야 합니다";
            case Code::kFnParamDefaultNotAllowedOutsideNamedGroup: return "기본값은 named-group '{ ... }' 안에서만 사용할 수 있습니다";
            case Code::kFnParamDefaultExprExpected: return "'=' 뒤에는 기본값 식이 와야 합니다";
            case Code::kNamedGroupLabelMustBeIdent: return "named-group의 라벨은 식별자(ident)여야 합니다";
            case Code::kNamedGroupLabelUnderscoreReserved: return "'_'는 named-group의 라벨로 사용할 수 없습니다. 값 위치에서만 사용하세요(예: x: _)";

            case Code::kVarDeclTypeAnnotationRequired: return "let 선언은 타입을 명시해야 합니다 (예: let x: T = ...;)";
            case Code::kVarDeclTypeAnnotationNotAllowed: return "set 선언에 타입을 명시하는 것은 허용되지 않습니다 (예: set x = ...;)";

            case Code::kFnOnlyOneNamedGroupAllowed: return "함수 파라미터에서는 named-group '{ ... }'를 최대 1개만 사용할 수 있습니다";
            case Code::kPubSubOnlyAllowedInClass: return "pub/sub는 class 내부에서만 사용할 수 있습니다.";
            case Code::kTypeNameExpected: return "타입 이름(ident)이 필요합니다";
            case Code::kTypeArrayMissingRBracket: return "배열 타입 접미사 '[]'를 닫는 ']'이(가) 필요합니다";
            case Code::kTypeOptionalDuplicate: return "nullable 접미사 '?'가 중복되었습니다";
            case Code::kTypeRecovery: return "타입 파싱에 실패하여 복구했습니다";
            case Code::kWhileHeaderExpectedLParen: return "'while' 뒤에는 '('이(가) 필요합니다";
            case Code::kWhileHeaderExpectedRParen: return "while 헤더를 닫는 ')'이(가) 필요합니다";
            case Code::kWhileBodyExpectedBlock:    return "while 본문 블록 '{ ... }'이(가) 필요합니다";

            case Code::kLoopHeaderExpectedLParen:   return "'loop' 헤더 뒤에는 '('이(가) 필요합니다";
            case Code::kLoopHeaderVarExpectedIdent: return "loop 헤더의 변수는 식별자(ident)여야 합니다";
            case Code::kLoopHeaderExpectedIn:       return "loop 헤더에는 'in'이(가) 필요합니다 (예: loop (v in xs))";
            case Code::kLoopHeaderExpectedRParen:   return "loop 헤더를 닫는 ')'이(가) 필요합니다";
            case Code::kLoopBodyExpectedBlock:      return "loop 본문 블록 '{ ... }'이(가) 필요합니다";

            case Code::kIfExprThenExpectedBlock: return "if 표현식의 then 분기는 블록 '{ ... }' 이어야 합니다";
            case Code::kIfExprElseExpectedBlock: return "if 표현식의 else 분기는 블록 '{ ... }' 또는 'else if ...' 이어야 합니다";
            case Code::kIfExprMissingElse: return "if 표현식에는 'else' 분기가 필요합니다";
            case Code::kIfExprBranchValueExpected: return "if 표현식의 분기는 값을 반환해야 합니다(끝의 ';'를 제거하거나 tail 값을 추가하세요)";
            case Code::kBlockTailSemicolonNotAllowed: return "블록의 마지막 값에는 ';'를 붙일 수 없습니다";

            case Code::kSwitchHeaderExpectedLParen: return "'switch' 뒤에는 '('이(가) 필요합니다";
            case Code::kSwitchHeaderExpectedRParen: return "switch 헤더를 닫는 ')'이(가) 필요합니다";
            case Code::kSwitchBodyExpectedLBrace:   return "switch 본문을 시작하는 '{'이(가) 필요합니다";
            case Code::kSwitchBodyExpectedRBrace:   return "switch 본문을 닫는 '}'이(가) 필요합니다";
            case Code::kSwitchCaseExpectedPattern:  return "case 패턴(literal/ident)이 필요합니다";
            case Code::kSwitchCaseExpectedColon:    return "case/default 라벨 뒤에는 ':'이(가) 필요합니다";
            case Code::kSwitchCaseBodyExpectedBlock:return "case/default 본문 블록 '{ ... }'이(가) 필요합니다";
            case Code::kSwitchDefaultDuplicate:     return "switch에서 default 절은 1개만 허용됩니다";
            case Code::kSwitchNeedsAtLeastOneCase:  return "switch에는 최소 1개의 case 절이 필요합니다";
            case Code::kSwitchOnlyCaseOrDefaultAllowed: return "switch 본문에는 case/default 절만 올 수 있습니다";

            case Code::kVarMutMustFollowKw: return "'mut'는 'let/set' 바로 뒤에만 올 수 있습니다 (예: set mut x = ...)";

            case Code::kBorrowOperandMustBePlace: return "'&'의 피연산자는 place expression이어야 합니다";
            case Code::kEscapeOperandMustBePlace: return "'&&'의 피연산자는 place expression이어야 합니다";
            case Code::kEscapeOperandMustNotBeBorrow: return "'&&'는 borrow('& ...')에 적용할 수 없습니다";

            case Code::kTopLevelMustBeBlock: return "내부 오류: 프로그램 루트는 블록이어야 합니다";
            case Code::kTopLevelDeclOnly: return "최상위에서는 decl만 허용됩니다";

            case Code::kUndefinedName: return "선언되지 않은 이름 '{0}'을(를) 사용했습니다";
            case Code::kDuplicateDecl: return "같은 스코프에서 '{0}'이(가) 중복 선언되었습니다";
            case Code::kShadowing: return "'{0}'이(가) 바깥 선언을 가립니다(shadowing)";
            case Code::kShadowingNotAllowed: return "shadowing이 금지되었습니다: '{0}'";

            case Code::kUseTextSubstExprExpected: return "use 치환에는 식이 필요합니다 (예: use NAME 123;)";
            case Code::kUseTextSubstTrailingTokens: return "use 치환 식 뒤에 예상치 못한 토큰이 있습니다. ';'가 필요합니다";

            // =========================
            // tyck (TYPE CHECK)
            // =========================
            case Code::kTypeErrorGeneric: /* args[0] = message */ return "{0}";
            case Code::kTypeLetInitMismatch: return "let '{0}' 초기화 실패: 기대 {1}, 실제 {2}";
            case Code::kTypeSetAssignMismatch:return "'{0}'에 대입할 수 없습니다: 기대 {1}, 실제 {2}";
            case Code::kTypeArgCountMismatch: return "인자 개수가 맞지 않습니다: 기대 {0}개, 실제 {1}개";
            case Code::kTypeArgTypeMismatch:  return "{0}번째 인자 타입이 맞지 않습니다: 기대 {1}, 실제 {2}";
            case Code::kTypeReturnOutsideFn:  return "함수 밖에서 return을 사용할 수 없습니다";
            case Code::kTypeReturnExprRequired:return "return에는 식이 필요합니다(현재 unit 타입이 없습니다)";
            case Code::kTypeUnaryBangMustBeBool:return "'!' 연산자는 bool에만 사용할 수 있습니다(현재 {0})";
            case Code::kTypeBinaryOperandsMustMatch:return "산술 연산의 양쪽 피연산자 타입이 같아야 합니다(lhs={0}, rhs={1})";
            case Code::kTypeCompareOperandsMustMatch:return "비교 연산의 양쪽 피연산자 타입이 같아야 합니다(lhs={0}, rhs={1})";
            case Code::kTypeBorrowNotAllowedInPureComptime:return "pure/comptime 함수에서는 '&'를 사용할 수 없습니다";
            case Code::kTypeEscapeNotAllowedInPureComptime:return "pure/comptime 함수에서는 '&&'를 사용할 수 없습니다";
            case Code::kTypeMismatch: /* args[0]=expected, args[1]=got */ return "타입이 일치하지 않습니다: 기대 {0}, 실제 {1}";
            case Code::kTypeNotCallable: /* args[0]=got_type */ return "함수가 아닌 타입 {0}은(는) 호출할 수 없습니다";
            case Code::kTypeCondMustBeBool: /* args[0]=got_type */ return "조건식은 bool이어야 합니다(현재 {0})";
            case Code::kTypeIndexMustBeUSize: /* args[0]=got_type */ return "인덱스 식은 usize여야 합니다(현재 {0})";
            case Code::kTypeIndexNonArray: /* args[0]=base_type */ return "배열이 아닌 타입 {0}에는 인덱싱을 사용할 수 없습니다";
            
            case Code::kSetCannotInferFromNull: /* args[0]=name (optional) */ return "set에서 null로는 타입을 추론할 수 없습니다. (예: let {0}: T? = null; 처럼 옵셔널 타입을 명시하세요)";
            case Code::kMissingReturn: return "missing return";
            
            case Code::kIntLiteralInvalid: return "정수 리터럴이 올바르지 않습니다: '{0}'";
            case Code::kIntLiteralOverflow: /* args[0]=text, args[1]=target */ return "정수 리터럴 '{0}'이(가) 대상 타입 {1}에서 오버플로우됩니다";

            case Code::kIntLiteralNeedsTypeContext: return "정수 리터럴은 타입 컨텍스트가 필요합니다. (예: i32)처럼 명시하거나, 타입이 정해진 대상에 대입하세요.";
            case Code::kIntLiteralDoesNotFit: return "정수 리터럴이 '{0}' 범위를 벗어납니다";
            
            
            case Code::kIntToFloatNotAllowed: /* args[0]=float_type */ return "지연된 정수 리터럴은 {0} 컨텍스트에서 사용할 수 없습니다(암시적 int->float 변환 없음)";

            case Code::kBreakOutsideLoop: return "break는 loop 안에서만 사용할 수 있습니다";
            case Code::kContinueOutsideLoop: return "continue는 loop 안에서만 사용할 수 있습니다";
            case Code::kBlockExprValueExpected: return "값이 필요합니다: value 컨텍스트의 block 표현식은 tail 식이 있어야 합니다";

            case Code::kAssignLhsMustBePlace: return "대입문의 왼쪽은 place expression(ident/index)이어야 합니다";
            case Code::kPostfixOperandMustBePlace: return "후위 연산자는 place expression(ident/index)에만 적용할 수 있습니다";

            case Code::kTypeParamTypeRequired: return "파라미터 '{0}'에는 타입이 필요합니다";
            case Code::kTypeDuplicateParam: return "파라미터 이름 '{0}'이(가) 중복되었습니다";
            case Code::kTypeParamDefaultMismatch: return "파라미터 '{0}'의 기본값 타입이 맞지 않습니다: 기대 {1}, 실제 {2}";
            case Code::kTypeAssignMismatch: return "대입할 수 없습니다: 기대 {0}, 실제 {1}";
            case Code::kTypeTernaryCondMustBeBool: return "삼항 조건식은 bool이어야 합니다(현재 {0})";
            case Code::kTypeUnresolvedHole: return "식에서 '_'(hole)이 해소되지 않았습니다";

            case Code::kTyckCastMissingOperand: return "cast expression is missing its operand";
            case Code::kTyckCastMissingTargetType: return "cast expression is missing its target type";
            case Code::kTyckCastNullToNonOptional: return "cannot cast 'null' to non-optional type '{0}'";
            case Code::kTyckCastNotAllowed: return "cast not allowed: '{0}' -> '{1}'";

            // args: {0}=lhs_type
            case Code::kTypeNullCoalesceLhsMustBeOptional: return "'??' 연산자의 왼쪽은 옵셔널(T?)이어야 합니다(현재 {0})";
            // args: {0}=elem_type, {1}=rhs_type
            case Code::kTypeNullCoalesceRhsMismatch: return "'??' 연산자의 오른쪽은 {0}에 대입 가능해야 합니다(현재 {1})";
            // args: {0}=lhs_type
            case Code::kTypeNullCoalesceAssignLhsMustBeOptional: return "'??=' 연산자의 왼쪽은 옵셔널(T?)이어야 합니다(현재 {0})";
            // args: {0}=elem_type, {1}=rhs_type
            case Code::kTypeNullCoalesceAssignRhsMismatch: return "'??=' 연산자의 오른쪽은 {0}에 대입 가능해야 합니다(현재 {1})";
        }

        return "알 수 없는 진단";
    }

    std::string render_one(const Diagnostic& d, Language lang, const SourceManager& sm) {
        std::string msg = (lang == Language::kKo) ? template_ko(d.code()) : template_en(d.code());
        msg = format_template(std::move(msg), d.args());

        const auto sp = d.span();
        auto lc = sm.line_col(sp.file_id, sp.lo);
        auto sn = sm.snippet_for_span(sp);

        std::ostringstream oss;
        auto sev = d.severity();
        const char* sev_name =
            (sev == Severity::kWarning) ? "warning" :
            (sev == Severity::kFatal)   ? "fatal"   : "error";

        oss << sev_name << "[" << code_name(d.code()) << "]: " << msg << "\n";
        oss << " --> " << sm.name(sp.file_id) << ":" << lc.line << ":" << lc.col << "\n";
        oss << "  |\n";
        oss << sn.line_no << " | " << sn.line_text << "\n";
        oss << "  | ";

        // spaces to caret
        for (uint32_t i = 0; i < sn.caret_cols_before; ++i) oss << ' ';

        // underline
        for (uint32_t i = 0; i < sn.caret_cols_len; ++i) oss << '^';
        
        return oss.str();
    }

    std::string render_one_context(const Diagnostic& d, Language lang, const SourceManager& sm, uint32_t context_lines) {
        std::string msg = (lang == Language::kKo) ? template_ko(d.code()) : template_en(d.code());
        msg = format_template(std::move(msg), d.args());

        const auto sp = d.span();
        auto lc = sm.line_col(sp.file_id, sp.lo);

        // 컨텍스트 스니펫
        auto blk = sm.snippet_block_for_span(sp, context_lines);

        uint32_t last_line_no = blk.first_line_no + static_cast<uint32_t>(blk.lines.size()) - 1;
        uint32_t w = digits10(last_line_no);

        std::ostringstream out;

        // error / fatal 출력
        auto sev = d.severity();
        const char* sev_name =
            (sev == Severity::kWarning) ? "warning" :
            (sev == Severity::kFatal)   ? "fatal"   : "error";

        out << sev_name << "[" << code_name(d.code()) << "]: " << msg << "\n";
        out << " --> " << sm.name(sp.file_id) << ":" << lc.line << ":" << lc.col << "\n";
        out << "  |\n";

        for (uint32_t i = 0; i < blk.lines.size(); ++i) {
            uint32_t line_no = blk.first_line_no + i;

            // "  12 | code..."
            out << std::string(2, ' ');
            {
                std::string num = std::to_string(line_no);
                out << std::string(w - static_cast<uint32_t>(num.size()), ' ') << num;
            }
            out << " | " << blk.lines[i] << "\n";

            if (i == blk.caret_line_offset) {
                out << std::string(2, ' ');
                out << std::string(w, ' ') << " | ";
                out << std::string(blk.caret_cols_before, ' ');
                out << std::string(blk.caret_cols_len, '^') << "\n";
            }
        }

        return out.str();
    }

} // namespace gaupel::diag