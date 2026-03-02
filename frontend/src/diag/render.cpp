// frontend/src/diag/render.cpp
#include <parus/diag/Render.hpp>

#include <array>
#include <algorithm>
#include <sstream>


namespace parus::diag {

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

    static std::string_view code_name_sv_(Code c) {
        switch (c) {
            case Code::kInvalidUtf8: return "InvalidUtf8";
            case Code::kExpectedToken: return "ExpectedToken";
            case Code::kUnexpectedToken: return "UnexpectedToken";
            case Code::kUnexpectedEof: return "UnexpectedEof";
            case Code::kTooManyErrors: return "TooManyErrors";
            case Code::kNestedTernaryNotAllowed: return "NestedTernaryNotAllowed";
            case Code::kAmbiguousAmpPrefixChain: return "AmbiguousAmpPrefixChain";
            case Code::kArraySizeExpectedIntLiteral: return "ArraySizeExpectedIntLiteral";
            case Code::kArraySizeInvalidLiteral: return "ArraySizeInvalidLiteral";
            case Code::kMacroNoMatch: return "MacroNoMatch";
            case Code::kMacroAmbiguous: return "MacroAmbiguous";
            case Code::kMacroRepeatEmpty: return "MacroRepeatEmpty";
            case Code::kMacroRecursionBudget: return "MacroRecursionBudget";
            case Code::kMacroReparseFail: return "MacroReparseFail";
            case Code::kMacroTokenExperimentalRequired: return "MacroTokenExperimentalRequired";
            case Code::kMacroTokenUnimplemented: return "MacroTokenUnimplemented";
            case Code::kPipeRhsMustBeCall: return "PipeRhsMustBeCall";
            case Code::kPipeFwdRhsMustBeCall: return "PipeFwdRhsMustBeCall";
            case Code::kPipeRevLhsMustBeCall: return "PipeRevLhsMustBeCall";
            case Code::kPipeHoleMustBeLabeled: return "PipeHoleMustBeLabeled";
            case Code::kPipeHoleCountMismatch: return "PipeHoleCountMismatch";
            case Code::kPipeHolePositionalNotAllowed: return "PipeHolePositionalNotAllowed";
            case Code::kGenericCallTypeArgParseAmbiguous: return "GenericCallTypeArgParseAmbiguous";
            case Code::kDeclExpected: return "DeclExpected";
            case Code::kFnNameExpected: return "FnNameExpected";
            case Code::kFnParamNameExpected: return "FnParamNameExpected";
            case Code::kFieldNameExpected: return "FieldNameExpected";
            case Code::kFieldMemberNameExpected: return "FieldMemberNameExpected";
            case Code::kFieldMemberMutNotAllowed: return "FieldMemberMutNotAllowed";
            case Code::kActsNameExpected: return "ActsNameExpected";
            case Code::kCallArgMixNotAllowed: return "CallArgMixNotAllowed";
            case Code::kFnReturnTypeRequired: return "FnReturnTypeRequired";
            case Code::kAttrNameExpectedAfterAt:      return "AttrNameExpectedAfterAt";
            case Code::kFnParamDefaultNotAllowedOutsideNamedGroup: return "FnParamDefaultNotAllowedOutsideNamedGroup";
            case Code::kFnParamDefaultExprExpected:                return "FnParamDefaultExprExpected";

            case Code::kVarDeclTypeAnnotationRequired: return "VarDeclTypeAnnotationRequired";
            case Code::kVarDeclTypeAnnotationNotAllowed: return "VarDeclTypeAnnotationNotAllowed";
            case Code::kVarDeclNameExpected: return "VarDeclNameExpected";
            case Code::kVarDeclInitializerExpected: return "VarDeclInitializerExpected";
            case Code::kSetInitializerRequired: return "SetInitializerRequired";
            case Code::kStaticVarExpectedLetOrSet: return "StaticVarExpectedLetOrSet";
            case Code::kStaticVarRequiresInitializer: return "StaticVarRequiresInitializer";

            case Code::kFnOnlyOneNamedGroupAllowed: return "FnOnlyOneNamedGroupAllowed";
            case Code::kActsForNotSupported: return "ActsForNotSupported";
            case Code::kActsMemberExportNotAllowed: return "ActsMemberExportNotAllowed";
            case Code::kActsForTypeExpected: return "ActsForTypeExpected";
            case Code::kActsGenericClauseRemoved: return "ActsGenericClauseRemoved";
            case Code::kOperatorDeclOnlyInActsFor: return "OperatorDeclOnlyInActsFor";
            case Code::kOperatorKeyExpected: return "OperatorKeyExpected";
            case Code::kOperatorSelfFirstParamRequired: return "OperatorSelfFirstParamRequired";
            case Code::kProtoMemberBodyNotAllowed: return "ProtoMemberBodyNotAllowed";
            case Code::kProtoMemberBodyMixNotAllowed: return "ProtoMemberBodyMixNotAllowed";
            case Code::kProtoOperatorNotAllowed: return "ProtoOperatorNotAllowed";
            case Code::kProtoRequireTypeNotBool: return "ProtoRequireTypeNotBool";
            case Code::kProtoRequireExprTooComplex: return "ProtoRequireExprTooComplex";
            case Code::kProtoImplTargetNotSupported: return "ProtoImplTargetNotSupported";
            case Code::kProtoImplMissingMember: return "ProtoImplMissingMember";
            case Code::kProtoConstraintUnsatisfied: return "ProtoConstraintUnsatisfied";
            case Code::kClassLifecycleDefaultParamNotAllowed: return "ClassLifecycleDefaultParamNotAllowed";
            case Code::kClassLifecycleSelfNotAllowed: return "ClassLifecycleSelfNotAllowed";
            case Code::kClassLifecycleDirectCallForbidden: return "ClassLifecycleDirectCallForbidden";
            case Code::kClassMemberLetSetRemoved: return "ClassMemberLetSetRemoved";
            case Code::kClassMemberFieldInitNotAllowed: return "ClassMemberFieldInitNotAllowed";
            case Code::kClassStaticMutNotAllowed: return "ClassStaticMutNotAllowed";
            case Code::kClassStaticVarRequiresInitializer: return "ClassStaticVarRequiresInitializer";
            case Code::kClassInheritanceNotAllowed: return "ClassInheritanceNotAllowed";
            case Code::kActorRequiresSingleDraft: return "ActorRequiresSingleDraft";
            case Code::kActorMemberNotAllowed: return "ActorMemberNotAllowed";
            case Code::kActorDeinitNotAllowed: return "ActorDeinitNotAllowed";
            case Code::kActorMethodModeRequired: return "ActorMethodModeRequired";
            case Code::kActorLifecycleDirectCallForbidden: return "ActorLifecycleDirectCallForbidden";
            case Code::kActorSpawnTargetMustBeActor: return "ActorSpawnTargetMustBeActor";
            case Code::kActorSpawnMissingInit: return "ActorSpawnMissingInit";
            case Code::kActorCtorStyleCallNotAllowed: return "ActorCtorStyleCallNotAllowed";
            case Code::kActorPathCallRemoved: return "ActorPathCallRemoved";
            case Code::kActorCommitOnlyInPub: return "ActorCommitOnlyInPub";
            case Code::kActorRecastOnlyInSub: return "ActorRecastOnlyInSub";
            case Code::kActorPubMissingTopLevelCommit: return "ActorPubMissingTopLevelCommit";
            case Code::kActorEscapeDraftMoveNotAllowed: return "ActorEscapeDraftMoveNotAllowed";
            case Code::kTypeFnSignatureExpected: return "TypeFnSignatureExpected";
            case Code::kTypeNameExpected: return "TypeNameExpected";
            case Code::kTypeArrayMissingRBracket: return "TypeArrayMissingRBracket";
            case Code::kTypeOptionalDuplicate: return "TypeOptionalDuplicate";
            case Code::kTypeRecovery: return "TypeRecovery";
            case Code::kCastTargetTypeExpected: return "CastTargetTypeExpected";
            case Code::kTypeInternalNameReserved: return "TypeInternalNameReserved";
            case Code::kWhileHeaderExpectedLParen: return "WhileHeaderExpectedLParen";
            case Code::kWhileHeaderExpectedRParen: return "WhileHeaderExpectedRParen";
            case Code::kWhileBodyExpectedBlock:    return "WhileBodyExpectedBlock";
            case Code::kDoBodyExpectedBlock:       return "DoBodyExpectedBlock";
            case Code::kDoWhileExpectedLParen:     return "DoWhileExpectedLParen";
            case Code::kDoWhileExpectedRParen:     return "DoWhileExpectedRParen";
            case Code::kDoWhileExpectedSemicolon:  return "DoWhileExpectedSemicolon";
            case Code::kBareBlockScopePreferDo:    return "BareBlockScopePreferDo";
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
            case Code::kBlockTailExprRequired: return "BlockTailExprRequired";
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
            case Code::kEscapeSubplaceMoveNotAllowed: return "EscapeSubplaceMoveNotAllowed";
            case Code::kEscapeOperandMustNotBeBorrow: return "EscapeOperandMustNotBeBorrow";
            case Code::kBorrowMutRequiresMutablePlace: return "BorrowMutRequiresMutablePlace";
            case Code::kBorrowMutConflict: return "BorrowMutConflict";
            case Code::kBorrowSharedConflictWithMut: return "BorrowSharedConflictWithMut";
            case Code::kBorrowMutConflictWithShared: return "BorrowMutConflictWithShared";
            case Code::kBorrowMutDirectAccessConflict: return "BorrowMutDirectAccessConflict";
            case Code::kBorrowSharedWriteConflict: return "BorrowSharedWriteConflict";
            case Code::kBorrowEscapeFromReturn: return "BorrowEscapeFromReturn";
            case Code::kBorrowEscapeToStorage: return "BorrowEscapeToStorage";
            case Code::kUseAfterEscapeMove: return "UseAfterEscapeMove";
            case Code::kEscapeWhileMutBorrowActive: return "EscapeWhileMutBorrowActive";
            case Code::kEscapeWhileBorrowActive: return "EscapeWhileBorrowActive";
            case Code::kEscapeRequiresStaticOrBoundary: return "EscapeRequiresStaticOrBoundary";
            case Code::kSirUseAfterEscapeMove: return "SirUseAfterEscapeMove";
            case Code::kSirEscapeBoundaryViolation: return "SirEscapeBoundaryViolation";
            case Code::kSirEscapeMustNotMaterialize: return "SirEscapeMustNotMaterialize";

            case Code::kTopLevelMustBeBlock: return "TopLevelMustBeBlock";
            case Code::kTopLevelDeclOnly: return "TopLevelDeclOnly";

            case Code::kUndefinedName: return "UndefinedName";
            case Code::kDuplicateDecl: return "DuplicateDecl";
            case Code::kShadowing: return "Shadowing";
            case Code::kShadowingNotAllowed: return "ShadowingNotAllowed";
            case Code::kImportDepNotDeclared: return "ImportDepNotDeclared";
            case Code::kSymbolNotExportedFileScope: return "SymbolNotExportedFileScope";
            case Code::kSymbolNotExportedBundleScope: return "SymbolNotExportedBundleScope";
            case Code::kSymbolAmbiguousOverload: return "SymbolAmbiguousOverload";
            case Code::kExportCollisionSameFolder: return "ExportCollisionSameFolder";
            case Code::kNestNotUsedForModuleResolution: return "NestNotUsedForModuleResolution";
            case Code::kExportIndexMissing: return "ExportIndexMissing";
            case Code::kExportIndexSchema: return "ExportIndexSchema";

            case Code::kUseTextSubstExprExpected:   return "UseTextSubstExprExpected";
            case Code::kUseTextSubstTrailingTokens: return "UseTextSubstTrailingTokens";
            case Code::kUseNestPathExpectedNamespace: return "UseNestPathExpectedNamespace";
            case Code::kUseNestAliasAsOnly: return "UseNestAliasAsOnly";
            case Code::kUseNestAliasPreferred: return "UseNestAliasPreferred";

            // =========================
            // tyck (TYPE CHECK)
            // =========================
            case Code::kTypeErrorGeneric:     return "TypeErrorGeneric";
            case Code::kTypeLetInitMismatch:  return "TypeLetInitMismatch";
            case Code::kTypeSetAssignMismatch:return "TypeSetAssignMismatch";
            case Code::kTypeArgCountMismatch: return "TypeArgCountMismatch";
            case Code::kTypeArgTypeMismatch:  return "TypeArgTypeMismatch";
            case Code::kOverloadDeclConflict:return "OverloadDeclConflict";
            case Code::kOverloadNoMatchingCall:return "OverloadNoMatchingCall";
            case Code::kOverloadAmbiguousCall:return "OverloadAmbiguousCall";
            case Code::kMangleSymbolCollision:return "MangleSymbolCollision";
            case Code::kAbiCOverloadNotAllowed:return "AbiCOverloadNotAllowed";
            case Code::kAbiCNamedGroupNotAllowed:return "AbiCNamedGroupNotAllowed";
            case Code::kAbiCTypeNotFfiSafe:return "AbiCTypeNotFfiSafe";
            case Code::kAbiCGlobalMustBeStatic:return "AbiCGlobalMustBeStatic";
            case Code::kTypeReturnOutsideFn:  return "TypeReturnOutsideFn";
            case Code::kTypeReturnExprRequired:return "TypeReturnExprRequired";
            case Code::kTypeBreakValueOnlyInLoopExpr:return "TypeBreakValueOnlyInLoopExpr";
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
            case Code::kTypeArrayLiteralEmptyNeedsContext: return "TypeArrayLiteralEmptyNeedsContext";
            case Code::kTypeFieldMemberRangeInvalid: return "TypeFieldMemberRangeInvalid";
            case Code::kTypeFieldMemberMustBePodBuiltin: return "TypeFieldMemberMustBePodBuiltin";
            case Code::kFieldInitTypeExpected: return "FieldInitTypeExpected";
            case Code::kFieldInitUnknownMember: return "FieldInitUnknownMember";
            case Code::kFieldInitDuplicateMember: return "FieldInitDuplicateMember";
            case Code::kFieldInitMissingMember: return "FieldInitMissingMember";
            case Code::kFieldInitNonOptionalNull: return "FieldInitNonOptionalNull";
            case Code::kFieldInitEmptyNotAllowed: return "FieldInitEmptyNotAllowed";
            case Code::kDotMethodSelfRequired: return "DotMethodSelfRequired";
            case Code::kDotReceiverMustBeValue: return "DotReceiverMustBeValue";
            case Code::kClassCtorMissingInit: return "ClassCtorMissingInit";
            case Code::kClassProtoPathCallRemoved: return "ClassProtoPathCallRemoved";
            case Code::kGenericArityMismatch: return "GenericArityMismatch";
            case Code::kGenericTypeArgInferenceFailed: return "GenericTypeArgInferenceFailed";
            case Code::kGenericAmbiguousOverload: return "GenericAmbiguousOverload";
            case Code::kGenericConstraintProtoNotFound: return "GenericConstraintProtoNotFound";
            case Code::kGenericConstraintUnsatisfied: return "GenericConstraintUnsatisfied";
            case Code::kGenericUnknownTypeParamInConstraint: return "GenericUnknownTypeParamInConstraint";
            case Code::kGenericDeclConstraintUnsatisfied: return "GenericDeclConstraintUnsatisfied";
            case Code::kGenericTypePathArityMismatch: return "GenericTypePathArityMismatch";
            case Code::kGenericTypePathTemplateNotFound: return "GenericTypePathTemplateNotFound";
            case Code::kGenericTypePathTemplateKindMismatch: return "GenericTypePathTemplateKindMismatch";
            case Code::kGenericActsOverlap: return "GenericActsOverlap";
            case Code::kGenericActorDeclNotSupportedV1: return "GenericActorDeclNotSupportedV1";

            case Code::kWriteToImmutable: return "WriteToImmutable";
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
            case Code::kAmbiguousAmpPrefixChain: return "ambiguous '&' prefix chain (3+ consecutive '&'); use parentheses (e.g. ^&(&x) or &(^&x))";
            case Code::kArraySizeExpectedIntLiteral: return "array size must be an integer literal (use T[N] or T[])";
            case Code::kArraySizeInvalidLiteral: return "invalid array size literal '{0}' (expected decimal u32 range)";
            case Code::kMacroNoMatch: return "no matching macro arm for call '{0}'";
            case Code::kMacroAmbiguous: return "ambiguous macro expansion for call '{0}'";
            case Code::kMacroRepeatEmpty: return "macro repetition body can match empty token stream";
            case Code::kMacroRecursionBudget: return "macro expansion budget exceeded for '{0}'";
            case Code::kMacroReparseFail: return "expanded macro output failed to parse as expected kind '{0}'";
            case Code::kMacroTokenExperimentalRequired: return "'with token' requires '-Xparus -macro-token-experimental'";
            case Code::kMacroTokenUnimplemented: return "'with token' group expansion is not implemented yet";
            case Code::kPipeRhsMustBeCall: return "pipe operator requires a function call on the required side";
            case Code::kPipeFwdRhsMustBeCall: return "pipe operator '|>' requires a function call on the right-hand side";
            case Code::kPipeRevLhsMustBeCall: return "pipe operator '<|' requires a function call on the left-hand side";
            case Code::kPipeHoleMustBeLabeled: return "hole '_' must appear as a labeled argument value (e.g., a: _)";
            case Code::kPipeHoleCountMismatch: return "pipe call must contain exactly one labeled hole '_' (found {0})";
            case Code::kPipeHolePositionalNotAllowed: return "hole '_' is not allowed as a positional argument in pipe calls";
            case Code::kGenericCallTypeArgParseAmbiguous: return "generic call type-argument list '<...>' is malformed or ambiguous";
            case Code::kDeclExpected: return "declaration expected";
            case Code::kFnNameExpected: return "function name identifier is required";
            case Code::kFnParamNameExpected: return "function parameter name identifier is required";
            case Code::kFieldNameExpected: return "type declaration name identifier is required";
            case Code::kFieldMemberNameExpected: return "struct member name identifier is required";
            case Code::kFieldMemberMutNotAllowed: return "struct members must not use 'mut' (declare mutability on bindings instead)";
            case Code::kActsNameExpected: return "acts name identifier is required";
            case Code::kCallArgMixNotAllowed: return "mixing labeled and positional arguments is not allowed";
            case Code::kFnReturnTypeRequired: return "function return type is required (use: def name(...) -> T { ... })";
            case Code::kAttrNameExpectedAfterAt: return "attribute name expected after '@'";
            case Code::kFnParamDefaultNotAllowedOutsideNamedGroup: return "default value is only allowed inside named-group '{ ... }'";
            case Code::kFnParamDefaultExprExpected: return "default expression expected after '='";

            case Code::kVarDeclTypeAnnotationRequired: return "type annotation is required for 'let' (use: let x: T = ...;)";
            case Code::kVarDeclTypeAnnotationNotAllowed: return "type annotation is not allowed for 'set' (use: set x = ...;)";
            case Code::kVarDeclNameExpected: return "variable declaration requires an identifier name";
            case Code::kVarDeclInitializerExpected: return "initializer expression is required after '='";
            case Code::kSetInitializerRequired: return "'set' declaration requires '=' initializer";
            case Code::kStaticVarExpectedLetOrSet: return "'static' declaration must be followed by [mut] let/set";
            case Code::kStaticVarRequiresInitializer: return "static variable requires an initializer";

            case Code::kFnOnlyOneNamedGroupAllowed: return "function parameters allow at most one named-group '{ ... }'";
            case Code::kActsForNotSupported: return "'acts for T' is not supported yet; use 'acts A { ... }'";
            case Code::kActsMemberExportNotAllowed: return "member-level 'export' is not allowed inside acts";
            case Code::kActsForTypeExpected: return "'acts for' requires a target type";
            case Code::kActsGenericClauseRemoved: return "acts trailing generic clause is removed; put generic parameters only on owner type (e.g. acts for Vec<T>)";
            case Code::kOperatorDeclOnlyInActsFor: return "operator(...) declarations are only allowed in 'acts for T' or 'acts Name for T'";
            case Code::kOperatorKeyExpected: return "operator key is missing or invalid (e.g., +, ==, ++pre)";
            case Code::kOperatorSelfFirstParamRequired: return "operator(...) first parameter must be a receiver marked with 'self'";
            case Code::kProtoMemberBodyNotAllowed: return "proto member body is not allowed in this context";
            case Code::kProtoMemberBodyMixNotAllowed: return "proto members must be all signature-only or all default-body";
            case Code::kProtoOperatorNotAllowed: return "operator declarations are not allowed in proto";
            case Code::kProtoRequireTypeNotBool: return "require(...) expression must have type bool";
            case Code::kProtoRequireExprTooComplex: return "require(...) supports only simple boolean expressions (true/false/not/and/or/parentheses) in v1";
            case Code::kProtoImplTargetNotSupported: return "implementation target is not a supported proto";
            case Code::kProtoImplMissingMember: return "proto implementation is missing a required member";
            case Code::kProtoConstraintUnsatisfied: return "proto constraint is not satisfied";
            case Code::kClassLifecycleDefaultParamNotAllowed: return "init()/deinit() = default only supports an empty parameter list";
            case Code::kClassLifecycleSelfNotAllowed: return "class lifecycle members must not declare a self receiver";
            case Code::kClassLifecycleDirectCallForbidden: return "init/deinit cannot be called directly; lifecycle is compiler-managed";
            case Code::kClassMemberLetSetRemoved: return "class member let/set syntax is removed; use 'name: Type;' for instance fields";
            case Code::kClassMemberFieldInitNotAllowed: return "class instance struct declaration must not include an initializer";
            case Code::kClassStaticMutNotAllowed: return "class static mut members are not supported in v0";
            case Code::kClassStaticVarRequiresInitializer: return "class static variable requires an initializer";
            case Code::kClassInheritanceNotAllowed: return "class-to-class inheritance is not allowed; class can only implement proto constraints";
            case Code::kActorRequiresSingleDraft: return "actor must declare exactly one draft block";
            case Code::kActorMemberNotAllowed: return "actor body only allows draft/init/def sub|pub declarations";
            case Code::kActorDeinitNotAllowed: return "actor does not support deinit() in v0";
            case Code::kActorMethodModeRequired: return "actor method must be declared as 'def sub' or 'def pub'";
            case Code::kActorLifecycleDirectCallForbidden: return "actor init/deinit cannot be called directly; lifecycle is runtime-managed";
            case Code::kActorSpawnTargetMustBeActor: return "spawn target must be an actor type";
            case Code::kActorSpawnMissingInit: return "spawn target requires at least one init overload";
            case Code::kActorCtorStyleCallNotAllowed: return "actor construction must use 'spawn A(...)' (ctor-style 'A(...)' is class-only)";
            case Code::kActorPathCallRemoved: return "actor member path call is removed; use dot call on actor value";
            case Code::kActorCommitOnlyInPub: return "commit is only allowed inside actor pub methods";
            case Code::kActorRecastOnlyInSub: return "recast is only allowed inside actor sub methods";
            case Code::kActorPubMissingTopLevelCommit: return "actor pub method must contain at least one top-level commit statement";
            case Code::kActorEscapeDraftMoveNotAllowed: return "actor draft cannot be moved with ^&";
            case Code::kTypeFnSignatureExpected: return "type-context 'def' must be followed by '('";
            case Code::kTypeNameExpected: return "type name expected";
            case Code::kTypeArrayMissingRBracket: return "array type suffix requires closing ']'";
            case Code::kTypeOptionalDuplicate: return "duplicate optional suffix '?'";
            case Code::kTypeRecovery: return "failed to parse type; recovered";
            case Code::kCastTargetTypeExpected: return "cast target type is required after 'as'/'as?'/'as!'";
            case Code::kTypeInternalNameReserved: return "type name '{0}' is reserved for compiler internals (use 'void' in source)";
            case Code::kWhileHeaderExpectedLParen: return "expected '(' after 'while'";
            case Code::kWhileHeaderExpectedRParen: return "expected ')' to close while header";
            case Code::kWhileBodyExpectedBlock:    return "expected while body block '{ ... }'";
            case Code::kDoBodyExpectedBlock:       return "expected do body block '{ ... }'";
            case Code::kDoWhileExpectedLParen:     return "expected '(' after 'while' in do-while";
            case Code::kDoWhileExpectedRParen:     return "expected ')' to close do-while condition";
            case Code::kDoWhileExpectedSemicolon:  return "expected ';' after do-while condition";
            case Code::kBareBlockScopePreferDo:    return "bare block scope is allowed but 'do { ... }' is preferred";
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
            case Code::kBlockTailExprRequired: return "block expression in value-required context must have a tail expression";

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
            case Code::kVarMutMustFollowKw: return "'mut' must appear immediately after declaration keyword (e.g., 'let mut x: T', 'set mut x = ...', 'static mut x: T = ...')";
            case Code::kBorrowOperandMustBePlace: return "& operand must be a place expression";
            case Code::kEscapeOperandMustBePlace: return "^& operand must be a place expression";
            case Code::kEscapeSubplaceMoveNotAllowed: return "^& only supports root identifier move in v0 (subplace move-out is not allowed)";
            case Code::kEscapeOperandMustNotBeBorrow: return "^& cannot be applied to a borrow operand";
            case Code::kBorrowMutRequiresMutablePlace: return "&mut requires a mutable place";
            case Code::kBorrowMutConflict: return "cannot create borrow: an active mutable borrow already exists for this place";
            case Code::kBorrowSharedConflictWithMut: return "cannot create shared borrow '&': an active '&mut' borrow exists for this place";
            case Code::kBorrowMutConflictWithShared: return "cannot create '&mut': active shared borrow(s) exist for this place";
            case Code::kBorrowMutDirectAccessConflict: return "cannot access this place directly while an active '&mut' borrow exists";
            case Code::kBorrowSharedWriteConflict: return "cannot write to this place while active shared borrow(s) exist";
            case Code::kBorrowEscapeFromReturn: return "borrow value cannot be returned (non-escaping rule)";
            case Code::kBorrowEscapeToStorage: return "borrow value cannot be stored in an escaping/long-lived storage";
            case Code::kUseAfterEscapeMove: return "value was moved by '^&' and cannot be used afterwards";
            case Code::kEscapeWhileMutBorrowActive: return "cannot apply '^&' while an active '&mut' borrow exists for this place";
            case Code::kEscapeWhileBorrowActive: return "cannot apply '^&' while an active borrow exists for this place";
            case Code::kEscapeRequiresStaticOrBoundary: return "escaping '^&' requires static storage or direct return/call-argument boundary";
            case Code::kSirUseAfterEscapeMove: return "use-after-move detected in SIR capability analysis ('^&' moved value)";
            case Code::kSirEscapeBoundaryViolation: return "SIR capability analysis: escape value must be consumed at return/call boundary or originate from static storage";
            case Code::kSirEscapeMustNotMaterialize: return "SIR capability analysis: escape handle cannot be materialized into non-static bindings";

            case Code::kTopLevelMustBeBlock: return "internal: program root must be a block";
            case Code::kTopLevelDeclOnly: return "top-level allows declarations only";

            case Code::kUndefinedName: return "use of undeclared name '{0}'";
            case Code::kDuplicateDecl: return "duplicate declaration '{0}' in the same scope";
            case Code::kShadowing: return "declaration '{0}' shadows an outer declaration";
            case Code::kShadowingNotAllowed: return "shadowing is not allowed: '{0}'";
            case Code::kImportDepNotDeclared: return "import head '{0}' is not declared in current bundle deps";
            case Code::kSymbolNotExportedFileScope: return "symbol '{0}' is declared in another file and must be exported";
            case Code::kSymbolNotExportedBundleScope: return "symbol '{0}' from another bundle must be exported";
            case Code::kSymbolAmbiguousOverload: return "symbol '{0}' is ambiguous after overload/visibility resolution";
            case Code::kExportCollisionSameFolder: return "export collision in same module folder for '{0}'";
            case Code::kNestNotUsedForModuleResolution: return "'nest' affects namespace only; module resolution ignores it";
            case Code::kExportIndexMissing: return "export index file is missing: '{0}'";
            case Code::kExportIndexSchema: return "invalid export index schema: '{0}'";

            case Code::kUseTextSubstExprExpected: return "use substitution requires an expression (e.g., use NAME 123;)";
            case Code::kUseTextSubstTrailingTokens: return "unexpected tokens after use substitution expression; expected ';'";
            case Code::kUseNestPathExpectedNamespace: return "use nest target must be a namespace path";
            case Code::kUseNestAliasAsOnly: return "use nest alias requires 'as' (use: use nest a::b as x;)";
            case Code::kUseNestAliasPreferred: return "namespace alias should prefer 'use nest ... as ...' (fix-it: use nest {0} as {1};)";

            // =========================
            // tyck (TYPE CHECK)
            // =========================
            case Code::kTypeErrorGeneric: /* args[0] = message */ return "{0}";
            case Code::kTypeLetInitMismatch: return "cannot initialize let '{0}': expected {1}, got {2}";
            case Code::kTypeSetAssignMismatch:return "cannot assign to '{0}': expected {1}, got {2}";
            case Code::kTypeArgCountMismatch: return "argument count mismatch: expected {0}, got {1}";
            case Code::kTypeArgTypeMismatch:  return "argument type mismatch at #{0}: expected {1}, got {2}";
            case Code::kOverloadDeclConflict: return "overload declaration conflict in '{0}': {1}";
            case Code::kOverloadNoMatchingCall: return "no matching overload for '{0}' with call form/types: {1}";
            case Code::kOverloadAmbiguousCall: return "ambiguous overloaded call '{0}'; candidates: {1}";
            case Code::kMangleSymbolCollision: return "mangle collision on symbol '{0}': {1} vs {2}";
            case Code::kAbiCOverloadNotAllowed: return "C ABI function '{0}' must not be overloaded";
            case Code::kAbiCNamedGroupNotAllowed: return "C ABI function '{0}' must not use named-group parameters";
            case Code::kAbiCTypeNotFfiSafe: return "C ABI requires FFI-safe type for {0}; got '{1}'";
            case Code::kAbiCGlobalMustBeStatic: return "C ABI global '{0}' must be declared with 'static'";
            case Code::kTypeReturnOutsideFn:  return "return outside of function";
            case Code::kTypeReturnExprRequired: return "return expression is required (function does not return void)";
            case Code::kTypeBreakValueOnlyInLoopExpr: return "break with value is only allowed inside loop expressions";
            case Code::kTypeUnaryBangMustBeBool:return "operator '!' requires bool (got {0})";
            case Code::kTypeBinaryOperandsMustMatch:return "binary arithmetic requires both operands to have the same type (lhs={0}, rhs={1})";
            case Code::kTypeCompareOperandsMustMatch:return "comparison requires both operands to have the same type (lhs={0}, rhs={1})";
            case Code::kTypeBorrowNotAllowedInPureComptime:return "borrow '&' is not allowed in pure/comptime functions";
            case Code::kTypeEscapeNotAllowedInPureComptime:return "escape '^&' is not allowed in pure/comptime functions";
            case Code::kTypeMismatch: /* args[0]=expected, args[1]=got */ return "type mismatch: expected {0}, got {1}";
            case Code::kTypeNotCallable: /* args[0]=got_type */ return "cannot call non-function type {0}";
            case Code::kTypeCondMustBeBool: /* args[0]=got_type */ return "condition must be bool (got {0})";
            case Code::kTypeIndexMustBeUSize: /* args[0]=got_type */ return "index/slice bound must be an integer type (got {0})";
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
            case Code::kTypeNullCoalesceLhsMustBeOptional: return "operator '?" "?' requires an optional lhs (got {0})";
            // args: {0}=elem_type, {1}=rhs_type
            case Code::kTypeNullCoalesceRhsMismatch: return "operator '?" "?' requires rhs assignable to {0} (got {1})";
            // args: {0}=lhs_type
            case Code::kTypeNullCoalesceAssignLhsMustBeOptional: return "operator '?" "?=' requires an optional lhs (got {0})";
            // args: {0}=elem_type, {1}=rhs_type
            case Code::kTypeNullCoalesceAssignRhsMismatch: return "operator '?" "?=' requires rhs assignable to {0} (got {1})";
            case Code::kTypeArrayLiteralEmptyNeedsContext: return "empty array literal requires an explicit contextual type";
            case Code::kTypeFieldMemberRangeInvalid: return "internal: struct member range is out of AST bounds";
            case Code::kTypeFieldMemberMustBePodBuiltin: return "struct member '{0}' must use a POD builtin value type (got {1})";
            case Code::kFieldInitTypeExpected: return "struct initializer head must be a struct type (got '{0}')";
            case Code::kFieldInitUnknownMember: return "struct initializer for '{0}' has unknown member '{1}'";
            case Code::kFieldInitDuplicateMember: return "struct initializer has duplicate member '{0}'";
            case Code::kFieldInitMissingMember: return "struct initializer for '{0}' is missing member '{1}'";
            case Code::kFieldInitNonOptionalNull: return "struct member '{0}' of type '{1}' is non-optional and cannot be initialized with null";
            case Code::kFieldInitEmptyNotAllowed: return "empty struct initializer is not allowed for non-empty struct type '{0}'";
            case Code::kDotMethodSelfRequired: return "dot method call requires the first parameter to be a 'self' receiver";
            case Code::kDotReceiverMustBeValue: return "dot method call receiver must be a value; type names are not allowed";
            case Code::kClassCtorMissingInit: return "class constructor call '{0}(...)' requires at least one init(...) overload";
            case Code::kClassProtoPathCallRemoved: return "path call '{0}' is removed for class/proto members; use value dot call instead";
            case Code::kGenericArityMismatch: return "generic arity mismatch: expected {0} type arguments, got {1}";
            case Code::kGenericTypeArgInferenceFailed: return "failed to infer generic type arguments for call '{0}'";
            case Code::kGenericAmbiguousOverload: return "generic overload resolution is ambiguous for call '{0}'";
            case Code::kGenericConstraintProtoNotFound: return "generic constraint references unknown proto '{0}'";
            case Code::kGenericConstraintUnsatisfied: return "generic constraint '{0}: {1}' is not satisfied by '{2}'";
            case Code::kGenericUnknownTypeParamInConstraint: return "constraint references unknown generic type parameter '{0}'";
            case Code::kGenericDeclConstraintUnsatisfied: return "declaration generic constraint '{0}: {1}' is not satisfied by '{2}'";
            case Code::kGenericTypePathArityMismatch: return "generic type path arity mismatch on '{0}': expected {1}, got {2}";
            case Code::kGenericTypePathTemplateNotFound: return "generic type path target template not found: '{0}'";
            case Code::kGenericTypePathTemplateKindMismatch: return "generic type path '{0}' points to a template of different kind (expected {1}, got {2})";
            case Code::kGenericActsOverlap: return "generic acts overlap detected for owner '{0}' and member '{1}'";
            case Code::kGenericActorDeclNotSupportedV1: return "generic actor declaration is not supported in v1: '{0}'";

            case Code::kWriteToImmutable: return "cannot write to immutable binding (declare it with 'mut')";
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
            case Code::kAmbiguousAmpPrefixChain: return "'&' 접두사 체인이 모호합니다(연속 '&' 3개 이상). 괄호로 명시하세요(예: ^&(&x), &(^&x))";
            case Code::kArraySizeExpectedIntLiteral: return "배열 크기는 정수 리터럴이어야 합니다(T[N] 또는 T[] 사용)";
            case Code::kArraySizeInvalidLiteral: return "배열 크기 리터럴 '{0}'이(가) 유효하지 않습니다(10진 u32 범위 필요)";
            case Code::kMacroNoMatch: return "매크로 호출 '{0}'과(와) 일치하는 arm을 찾지 못했습니다";
            case Code::kMacroAmbiguous: return "매크로 호출 '{0}'의 확장이 모호합니다";
            case Code::kMacroRepeatEmpty: return "매크로 반복 본문이 빈 토큰 스트림과 매칭될 수 없습니다";
            case Code::kMacroRecursionBudget: return "매크로 '{0}' 확장 예산(깊이/단계)을 초과했습니다";
            case Code::kMacroReparseFail: return "매크로 확장 결과를 기대 kind '{0}'로 다시 파싱할 수 없습니다";
            case Code::kMacroTokenExperimentalRequired: return "'with token'은 '-Xparus -macro-token-experimental' 옵션이 필요합니다";
            case Code::kMacroTokenUnimplemented: return "'with token' 그룹 확장은 아직 구현되지 않았습니다";
            case Code::kPipeRhsMustBeCall: return "파이프 연산자는 필요한 쪽에 함수 호출이 있어야 합니다";
            case Code::kPipeFwdRhsMustBeCall: return "파이프 연산자 '|>'의 오른쪽은 함수 호출이어야 합니다";
            case Code::kPipeRevLhsMustBeCall: return "파이프 연산자 '<|'의 왼쪽은 함수 호출이어야 합니다";
            case Code::kPipeHoleMustBeLabeled: return "'_'는 라벨 인자 값 위치에만 올 수 있습니다(예: a: _)";
            case Code::kPipeHoleCountMismatch: return "파이프 호출에는 라벨 인자 값으로 '_'가 정확히 1개 있어야 합니다(현재 {0}개)";
            case Code::kPipeHolePositionalNotAllowed: return "'_'는 파이프 호출에서 위치 인자로 사용할 수 없습니다";
            case Code::kGenericCallTypeArgParseAmbiguous: return "제네릭 호출 타입 인자 '<...>' 구문이 잘못되었거나 모호합니다";
            case Code::kDeclExpected: return "이 위치에는 선언(declaration)이 필요합니다";
            case Code::kFnNameExpected: return "함수 이름 식별자가 필요합니다";
            case Code::kFnParamNameExpected: return "함수 파라미터 이름 식별자가 필요합니다";
            case Code::kFieldNameExpected: return "타입 선언 이름 식별자가 필요합니다";
            case Code::kFieldMemberNameExpected: return "struct 멤버 이름 식별자가 필요합니다";
            case Code::kFieldMemberMutNotAllowed: return "struct 멤버에는 'mut'를 사용할 수 없습니다(가변성은 바인딩에서 표현하세요)";
            case Code::kActsNameExpected: return "acts 이름 식별자가 필요합니다";
            case Code::kCallArgMixNotAllowed: return "라벨 인자와 위치 인자를 섞어 호출할 수 없습니다";
            case Code::kFnReturnTypeRequired: return "함수 반환 타입이 필요합니다 (예: def name(...) -> T { ... })";
            case Code::kAttrNameExpectedAfterAt: return "'@' 뒤에는 attribute 이름이 와야 합니다";
            case Code::kFnParamDefaultNotAllowedOutsideNamedGroup: return "기본값은 named-group '{ ... }' 안에서만 사용할 수 있습니다";
            case Code::kFnParamDefaultExprExpected: return "'=' 뒤에는 기본값 식이 와야 합니다";

            case Code::kVarDeclTypeAnnotationRequired: return "let 선언은 타입을 명시해야 합니다 (예: let x: T = ...;)";
            case Code::kVarDeclTypeAnnotationNotAllowed: return "set 선언에 타입을 명시하는 것은 허용되지 않습니다 (예: set x = ...;)";
            case Code::kVarDeclNameExpected: return "변수 선언에는 식별자 이름이 필요합니다";
            case Code::kVarDeclInitializerExpected: return "'=' 뒤에는 초기화 식이 필요합니다";
            case Code::kSetInitializerRequired: return "set 선언에는 '=' 초기화식이 반드시 필요합니다";
            case Code::kStaticVarExpectedLetOrSet: return "'static' 선언 뒤에는 [mut] let/set이 와야 합니다";
            case Code::kStaticVarRequiresInitializer: return "static 변수는 초기화식이 반드시 필요합니다";

            case Code::kFnOnlyOneNamedGroupAllowed: return "함수 파라미터에서는 named-group '{ ... }'를 최대 1개만 사용할 수 있습니다";
            case Code::kActsForNotSupported: return "'acts for T'는 아직 지원되지 않습니다. 'acts A { ... }' 형태를 사용하세요";
            case Code::kActsMemberExportNotAllowed: return "acts 내부 멤버 함수에는 'export'를 붙일 수 없습니다";
            case Code::kActsForTypeExpected: return "'acts for' 뒤에는 대상 타입이 필요합니다";
            case Code::kActsGenericClauseRemoved: return "acts의 trailing generic 절은 제거되었습니다. owner 타입에만 제네릭을 표기하세요 (예: acts for Vec<T>)";
            case Code::kOperatorDeclOnlyInActsFor: return "operator(...) 선언은 'acts for T' 또는 'acts Name for T' 내부에서만 허용됩니다";
            case Code::kOperatorKeyExpected: return "operator 키가 없거나 올바르지 않습니다 (예: +, ==, ++pre)";
            case Code::kOperatorSelfFirstParamRequired: return "operator(...)의 첫 번째 파라미터는 'self' 리시버여야 합니다";
            case Code::kProtoMemberBodyNotAllowed: return "이 문맥에서는 proto 멤버 본문을 사용할 수 없습니다";
            case Code::kProtoMemberBodyMixNotAllowed: return "proto 멤버는 전부 시그니처 전용이거나 전부 기본 구현이어야 합니다";
            case Code::kProtoOperatorNotAllowed: return "proto 내부에서는 operator 선언을 사용할 수 없습니다";
            case Code::kProtoRequireTypeNotBool: return "require(...) 식의 타입은 bool이어야 합니다";
            case Code::kProtoRequireExprTooComplex: return "v1에서 require(...)는 단순 bool 식(true/false/not/and/or/괄호)만 허용됩니다";
            case Code::kProtoImplTargetNotSupported: return "구현 대상으로 지정한 항목이 지원되는 proto가 아닙니다";
            case Code::kProtoImplMissingMember: return "proto 구현에 필요한 멤버가 누락되었습니다";
            case Code::kProtoConstraintUnsatisfied: return "proto 제약을 만족하지 못했습니다";
            case Code::kClassLifecycleDefaultParamNotAllowed: return "init()/deinit() = default 는 빈 파라미터 목록만 허용합니다";
            case Code::kClassLifecycleSelfNotAllowed: return "class lifecycle 멤버에는 self 리시버를 선언할 수 없습니다";
            case Code::kClassLifecycleDirectCallForbidden: return "init/deinit 는 직접 호출할 수 없습니다. lifecycle 호출은 컴파일러가 관리합니다";
            case Code::kClassMemberLetSetRemoved: return "class 멤버 let/set 문법은 제거되었습니다. 인스턴스 필드는 'name: Type;'을 사용하세요";
            case Code::kClassMemberFieldInitNotAllowed: return "class 인스턴스 필드 선언에는 초기화식을 둘 수 없습니다";
            case Code::kClassStaticMutNotAllowed: return "class static mut 멤버는 v0에서 지원하지 않습니다";
            case Code::kClassStaticVarRequiresInitializer: return "class static 변수는 초기화식이 반드시 필요합니다";
            case Code::kClassInheritanceNotAllowed: return "class 간 상속은 허용되지 않습니다. class는 proto 제약만 구현할 수 있습니다";
            case Code::kActorRequiresSingleDraft: return "actor는 draft 블록을 정확히 1개 선언해야 합니다";
            case Code::kActorMemberNotAllowed: return "actor 본문에는 draft/init/def sub|pub 선언만 허용됩니다";
            case Code::kActorDeinitNotAllowed: return "actor는 v0에서 deinit()을 지원하지 않습니다";
            case Code::kActorMethodModeRequired: return "actor 메서드는 'def sub' 또는 'def pub'로 선언해야 합니다";
            case Code::kActorLifecycleDirectCallForbidden: return "actor init/deinit은 직접 호출할 수 없습니다";
            case Code::kActorSpawnTargetMustBeActor: return "spawn 대상은 actor 타입이어야 합니다";
            case Code::kActorSpawnMissingInit: return "spawn 대상 actor에는 최소 1개 이상의 init 오버로드가 필요합니다";
            case Code::kActorCtorStyleCallNotAllowed: return "actor 생성은 'spawn A(...)'만 허용됩니다";
            case Code::kActorPathCallRemoved: return "actor 멤버 경로 호출은 제거되었습니다. actor 값에 대해 dot 호출을 사용하세요";
            case Code::kActorCommitOnlyInPub: return "commit은 actor pub 메서드 내부에서만 사용할 수 있습니다";
            case Code::kActorRecastOnlyInSub: return "recast는 actor sub 메서드 내부에서만 사용할 수 있습니다";
            case Code::kActorPubMissingTopLevelCommit: return "actor pub 메서드에는 최상위 commit 문이 최소 1개 필요합니다";
            case Code::kActorEscapeDraftMoveNotAllowed: return "actor draft는 ^&로 move할 수 없습니다";
            case Code::kTypeFnSignatureExpected: return "타입 문맥의 'def' 뒤에는 '('이(가) 필요합니다";
            case Code::kTypeNameExpected: return "타입 이름(ident)이 필요합니다";
            case Code::kTypeArrayMissingRBracket: return "배열 타입 접미사 '[]'를 닫는 ']'이(가) 필요합니다";
            case Code::kTypeOptionalDuplicate: return "nullable 접미사 '?'가 중복되었습니다";
            case Code::kTypeRecovery: return "타입 파싱에 실패하여 복구했습니다";
            case Code::kCastTargetTypeExpected: return "'as'/'as?'/'as!' 뒤에는 대상 타입이 필요합니다";
            case Code::kTypeInternalNameReserved: return "타입 이름 '{0}'은(는) 컴파일러 내부 전용입니다(소스에서는 'void'를 사용하세요)";
            case Code::kWhileHeaderExpectedLParen: return "'while' 뒤에는 '('이(가) 필요합니다";
            case Code::kWhileHeaderExpectedRParen: return "while 헤더를 닫는 ')'이(가) 필요합니다";
            case Code::kWhileBodyExpectedBlock:    return "while 본문 블록 '{ ... }'이(가) 필요합니다";
            case Code::kDoBodyExpectedBlock:       return "do 본문 블록 '{ ... }'이(가) 필요합니다";
            case Code::kDoWhileExpectedLParen:     return "do-while의 'while' 뒤에는 '('이(가) 필요합니다";
            case Code::kDoWhileExpectedRParen:     return "do-while 조건을 닫는 ')'이(가) 필요합니다";
            case Code::kDoWhileExpectedSemicolon:  return "do-while 조건 뒤에는 ';'이(가) 필요합니다";
            case Code::kBareBlockScopePreferDo:    return "단독 블록 스코프도 허용되지만 'do { ... }' 사용을 권장합니다";

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
            case Code::kBlockTailExprRequired: return "값이 필요한 블록 표현식에는 tail 식이 필요합니다";

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

            case Code::kVarMutMustFollowKw: return "'mut'는 선언 키워드 바로 뒤에만 올 수 있습니다 (예: let mut x: T, set mut x = ..., static mut x: T = ...)";

            case Code::kBorrowOperandMustBePlace: return "'&'의 피연산자는 place expression이어야 합니다";
            case Code::kEscapeOperandMustBePlace: return "'^&'의 피연산자는 place expression이어야 합니다";
            case Code::kEscapeSubplaceMoveNotAllowed: return "v0에서 '^&'는 루트 식별자만 이동할 수 있습니다(subplace move-out 금지)";
            case Code::kEscapeOperandMustNotBeBorrow: return "'^&'는 borrow('& ...')에 적용할 수 없습니다";
            case Code::kBorrowMutRequiresMutablePlace: return "'&mut'는 mutable place에만 적용할 수 있습니다";
            case Code::kBorrowMutConflict: return "이미 활성화된 '&mut' borrow가 있어 추가 borrow를 만들 수 없습니다";
            case Code::kBorrowSharedConflictWithMut: return "활성 '&mut' borrow가 있는 동안에는 shared borrow('&')를 만들 수 없습니다";
            case Code::kBorrowMutConflictWithShared: return "활성 shared borrow가 있는 동안에는 '&mut' borrow를 만들 수 없습니다";
            case Code::kBorrowMutDirectAccessConflict: return "활성 '&mut' borrow가 있는 동안에는 해당 place를 직접 접근할 수 없습니다";
            case Code::kBorrowSharedWriteConflict: return "활성 shared borrow가 있는 동안에는 해당 place에 쓰기할 수 없습니다";
            case Code::kBorrowEscapeFromReturn: return "borrow 값은 반환할 수 없습니다(비탈출 규칙)";
            case Code::kBorrowEscapeToStorage: return "borrow 값은 탈출/장수명 저장소에 저장할 수 없습니다";
            case Code::kUseAfterEscapeMove: return "'^&'로 move된 값은 이후 사용할 수 없습니다";
            case Code::kEscapeWhileMutBorrowActive: return "활성 '&mut' borrow가 있는 동안에는 해당 place에 '^&'를 적용할 수 없습니다";
            case Code::kEscapeWhileBorrowActive: return "활성 borrow가 있는 동안에는 해당 place에 '^&'를 적용할 수 없습니다";
            case Code::kEscapeRequiresStaticOrBoundary: return "'^&' 탈출은 static 저장소이거나 return/호출 인자 경계에서 직접 사용되어야 합니다";
            case Code::kSirUseAfterEscapeMove: return "SIR capability 분석에서 use-after-move가 감지되었습니다('^&'로 move된 값 사용)";
            case Code::kSirEscapeBoundaryViolation: return "SIR capability 분석: escape 값은 return/호출 인자 경계에서 소비되거나 static 저장소 기원이어야 합니다";
            case Code::kSirEscapeMustNotMaterialize: return "SIR capability 분석: escape handle은 non-static 바인딩으로 물질화할 수 없습니다";

            case Code::kTopLevelMustBeBlock: return "내부 오류: 프로그램 루트는 블록이어야 합니다";
            case Code::kTopLevelDeclOnly: return "최상위에서는 decl만 허용됩니다";

            case Code::kUndefinedName: return "선언되지 않은 이름 '{0}'을(를) 사용했습니다";
            case Code::kDuplicateDecl: return "같은 스코프에서 '{0}'이(가) 중복 선언되었습니다";
            case Code::kShadowing: return "'{0}'이(가) 바깥 선언을 가립니다(shadowing)";
            case Code::kShadowingNotAllowed: return "shadowing이 금지되었습니다: '{0}'";
            case Code::kImportDepNotDeclared: return "import head '{0}'가 현재 bundle deps에 선언되어 있지 않습니다";
            case Code::kSymbolNotExportedFileScope: return "다른 파일의 심볼 '{0}'을(를) 참조하려면 export가 필요합니다";
            case Code::kSymbolNotExportedBundleScope: return "다른 bundle의 심볼 '{0}'은(는) export되어야 합니다";
            case Code::kSymbolAmbiguousOverload: return "오버로드/가시성 해소 후에도 심볼 '{0}'이(가) 모호합니다";
            case Code::kExportCollisionSameFolder: return "같은 모듈 폴더에서 export 충돌이 발생했습니다: '{0}'";
            case Code::kNestNotUsedForModuleResolution: return "'nest'는 네임스페이스 태깅 전용이며 모듈 해석에는 사용되지 않습니다";
            case Code::kExportIndexMissing: return "export index 파일을 찾을 수 없습니다: '{0}'";
            case Code::kExportIndexSchema: return "export index 형식이 올바르지 않습니다: '{0}'";

            case Code::kUseTextSubstExprExpected: return "use 치환에는 식이 필요합니다 (예: use NAME 123;)";
            case Code::kUseTextSubstTrailingTokens: return "use 치환 식 뒤에 예상치 못한 토큰이 있습니다. ';'가 필요합니다";
            case Code::kUseNestPathExpectedNamespace: return "use nest 대상은 네임스페이스 경로여야 합니다";
            case Code::kUseNestAliasAsOnly: return "use nest alias는 'as'만 허용합니다 (예: use nest a::b as x;)";
            case Code::kUseNestAliasPreferred: return "네임스페이스 alias는 'use nest ... as ...' 사용을 권장합니다 (자동수정: use nest {0} as {1};)";

            // =========================
            // tyck (TYPE CHECK)
            // =========================
            case Code::kTypeErrorGeneric: /* args[0] = message */ return "{0}";
            case Code::kTypeLetInitMismatch: return "let '{0}' 초기화 실패: 기대 {1}, 실제 {2}";
            case Code::kTypeSetAssignMismatch:return "'{0}'에 대입할 수 없습니다: 기대 {1}, 실제 {2}";
            case Code::kTypeArgCountMismatch: return "인자 개수가 맞지 않습니다: 기대 {0}개, 실제 {1}개";
            case Code::kTypeArgTypeMismatch:  return "{0}번째 인자 타입이 맞지 않습니다: 기대 {1}, 실제 {2}";
            case Code::kOverloadDeclConflict: return "'{0}' 오버로드 선언이 충돌합니다: {1}";
            case Code::kOverloadNoMatchingCall: return "'{0}' 호출과 일치하는 오버로드를 찾지 못했습니다: {1}";
            case Code::kOverloadAmbiguousCall: return "'{0}' 호출이 모호합니다. 후보: {1}";
            case Code::kMangleSymbolCollision: return "맹글링 심볼 '{0}'이 충돌합니다: {1} vs {2}";
            case Code::kAbiCOverloadNotAllowed: return "C ABI 함수 '{0}'는 오버로딩할 수 없습니다";
            case Code::kAbiCNamedGroupNotAllowed: return "C ABI 함수 '{0}'는 named-group 파라미터를 사용할 수 없습니다";
            case Code::kAbiCTypeNotFfiSafe: return "C ABI의 {0}에는 FFI-safe 타입만 허용됩니다. 현재 타입: '{1}'";
            case Code::kAbiCGlobalMustBeStatic: return "C ABI 전역 '{0}'는 반드시 'static'으로 선언해야 합니다";
            case Code::kTypeReturnOutsideFn:  return "함수 밖에서 return을 사용할 수 없습니다";
            case Code::kTypeReturnExprRequired:return "return에는 식이 필요합니다(현재 반환 타입이 void가 아닙니다)";
            case Code::kTypeBreakValueOnlyInLoopExpr: return "값을 가진 break는 loop 표현식 안에서만 허용됩니다";
            case Code::kTypeUnaryBangMustBeBool:return "'!' 연산자는 bool에만 사용할 수 있습니다(현재 {0})";
            case Code::kTypeBinaryOperandsMustMatch:return "산술 연산의 양쪽 피연산자 타입이 같아야 합니다(lhs={0}, rhs={1})";
            case Code::kTypeCompareOperandsMustMatch:return "비교 연산의 양쪽 피연산자 타입이 같아야 합니다(lhs={0}, rhs={1})";
            case Code::kTypeBorrowNotAllowedInPureComptime:return "pure/comptime 함수에서는 '&'를 사용할 수 없습니다";
            case Code::kTypeEscapeNotAllowedInPureComptime:return "pure/comptime 함수에서는 '^&'를 사용할 수 없습니다";
            case Code::kTypeMismatch: /* args[0]=expected, args[1]=got */ return "타입이 일치하지 않습니다: 기대 {0}, 실제 {1}";
            case Code::kTypeNotCallable: /* args[0]=got_type */ return "함수가 아닌 타입 {0}은(는) 호출할 수 없습니다";
            case Code::kTypeCondMustBeBool: /* args[0]=got_type */ return "조건식은 bool이어야 합니다(현재 {0})";
            case Code::kTypeIndexMustBeUSize: /* args[0]=got_type */ return "인덱스/슬라이스 경계는 정수 타입이어야 합니다(현재 {0})";
            case Code::kTypeIndexNonArray: /* args[0]=base_type */ return "배열이 아닌 타입 {0}에는 인덱싱을 사용할 수 없습니다";
            
            case Code::kSetCannotInferFromNull: /* args[0]=name (optional) */ return "set에서 null로는 타입을 추론할 수 없습니다. (예: let {0}: T? = null; 처럼 옵셔널 타입을 명시하세요)";
            case Code::kMissingReturn: return "반환문이 누락되었습니다";
            
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

            case Code::kTyckCastMissingOperand: return "cast 식에 피연산자가 없습니다";
            case Code::kTyckCastMissingTargetType: return "cast 식에 대상 타입이 없습니다";
            case Code::kTyckCastNullToNonOptional: return "'null'을 non-optional 타입 '{0}'로 cast할 수 없습니다";
            case Code::kTyckCastNotAllowed: return "허용되지 않는 cast입니다: '{0}' -> '{1}'";

            // args: {0}=lhs_type
            case Code::kTypeNullCoalesceLhsMustBeOptional: return "'?" "?' 연산자의 왼쪽은 옵셔널(T?)이어야 합니다(현재 {0})";
            // args: {0}=elem_type, {1}=rhs_type
            case Code::kTypeNullCoalesceRhsMismatch: return "'?" "?' 연산자의 오른쪽은 {0}에 대입 가능해야 합니다(현재 {1})";
            // args: {0}=lhs_type
            case Code::kTypeNullCoalesceAssignLhsMustBeOptional: return "'?" "?=' 연산자의 왼쪽은 옵셔널(T?)이어야 합니다(현재 {0})";
            // args: {0}=elem_type, {1}=rhs_type
            case Code::kTypeNullCoalesceAssignRhsMismatch: return "'?" "?=' 연산자의 오른쪽은 {0}에 대입 가능해야 합니다(현재 {1})";
            case Code::kTypeArrayLiteralEmptyNeedsContext: return "빈 배열 리터럴은 명시적 문맥 타입이 필요합니다";
            case Code::kTypeFieldMemberRangeInvalid: return "내부 오류: struct 멤버 범위가 AST 범위를 벗어났습니다";
            case Code::kTypeFieldMemberMustBePodBuiltin: return "struct 멤버 '{0}'는 POD 내장 값 타입이어야 합니다(현재 {1})";
            case Code::kFieldInitTypeExpected: return "struct 초기화 헤드는 struct 타입이어야 합니다(현재 '{0}')";
            case Code::kFieldInitUnknownMember: return "struct '{0}' 초기화에 존재하지 않는 멤버 '{1}'가 있습니다";
            case Code::kFieldInitDuplicateMember: return "struct 초기화에서 멤버 '{0}'가 중복되었습니다";
            case Code::kFieldInitMissingMember: return "struct '{0}' 초기화에서 멤버 '{1}'가 누락되었습니다";
            case Code::kFieldInitNonOptionalNull: return "멤버 '{0}' 타입 '{1}'는 non-optional이므로 null로 초기화할 수 없습니다";
            case Code::kFieldInitEmptyNotAllowed: return "멤버가 있는 struct 타입 '{0}'에는 빈 초기화 '{}'를 사용할 수 없습니다";
            case Code::kDotMethodSelfRequired: return "dot 메서드 호출은 첫 번째 파라미터가 'self' 리시버여야 합니다";
            case Code::kDotReceiverMustBeValue: return "dot 메서드 호출의 receiver는 값이어야 하며 타입 이름은 허용되지 않습니다";
            case Code::kClassCtorMissingInit: return "class 생성 호출 '{0}(...)'에는 최소 1개의 init(...) 오버로드가 필요합니다";
            case Code::kClassProtoPathCallRemoved: return "class/proto 멤버 경로 호출 '{0}'은 제거되었습니다. 값 dot 호출을 사용하세요";
            case Code::kGenericArityMismatch: return "제네릭 타입 인자 개수가 맞지 않습니다: 기대 {0}개, 실제 {1}개";
            case Code::kGenericTypeArgInferenceFailed: return "호출 '{0}'의 제네릭 타입 인자 추론에 실패했습니다";
            case Code::kGenericAmbiguousOverload: return "호출 '{0}'의 제네릭 오버로드 해석이 모호합니다";
            case Code::kGenericConstraintProtoNotFound: return "제네릭 제약이 알 수 없는 proto '{0}'를 참조합니다";
            case Code::kGenericConstraintUnsatisfied: return "제네릭 제약 '{0}: {1}'을(를) '{2}' 타입이 만족하지 않습니다";
            case Code::kGenericUnknownTypeParamInConstraint: return "제약이 알 수 없는 제네릭 타입 파라미터 '{0}'를 참조합니다";
            case Code::kGenericDeclConstraintUnsatisfied: return "선언 제네릭 제약 '{0}: {1}'을(를) '{2}' 타입이 만족하지 않습니다";
            case Code::kGenericTypePathArityMismatch: return "제네릭 타입 경로 '{0}'의 인자 개수가 맞지 않습니다: 기대 {1}개, 실제 {2}개";
            case Code::kGenericTypePathTemplateNotFound: return "제네릭 타입 경로 대상 템플릿을 찾을 수 없습니다: '{0}'";
            case Code::kGenericTypePathTemplateKindMismatch: return "제네릭 타입 경로 '{0}'가 다른 종류의 템플릿을 가리킵니다: 기대 {1}, 실제 {2}";
            case Code::kGenericActsOverlap: return "owner '{0}'와 멤버 '{1}'에서 제네릭 acts 중복(coherence overlap)이 발생했습니다";
            case Code::kGenericActorDeclNotSupportedV1: return "v1에서는 제네릭 actor 선언을 지원하지 않습니다: '{0}'";

            case Code::kWriteToImmutable: return "불변 변수에 대해 값을 쓸 수 없습니다";
        }

        return "알 수 없는 진단";
    }

    std::string code_name(Code c) {
        return std::string(code_name_sv_(c));
    }

    std::string render_message(const Diagnostic& d, Language lang) {
        std::string msg = (lang == Language::kKo) ? template_ko(d.code()) : template_en(d.code());
        return format_template(std::move(msg), d.args());
    }

    std::string render_one(const Diagnostic& d, Language lang, const SourceManager& sm) {
        std::string msg = render_message(d, lang);

        const auto sp = d.span();
        auto lc = sm.line_col(sp.file_id, sp.lo);
        auto sn = sm.snippet_for_span(sp);

        std::ostringstream oss;
        auto sev = d.severity();
        const char* sev_name =
            (sev == Severity::kWarning) ? "warning" :
            (sev == Severity::kFatal)   ? "fatal"   : "error";

        oss << sev_name << "[" << code_name_sv_(d.code()) << "]: " << msg << "\n";
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

        out << sev_name << "[" << code_name_sv_(d.code()) << "]: " << msg << "\n";
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

} // namespace parus::diag
