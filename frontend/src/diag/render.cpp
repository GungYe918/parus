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
            case Code::kLegacyEscapeCaretAmpUseTilde: return "LegacyEscapeCaretAmpUseTilde";
            case Code::kSelfMutSyntaxRemoved: return "SelfMutSyntaxRemoved";
            case Code::kArraySizeExpectedIntLiteral: return "ArraySizeExpectedIntLiteral";
            case Code::kArraySizeInvalidLiteral: return "ArraySizeInvalidLiteral";
            case Code::kMacroNoMatch: return "MacroNoMatch";
            case Code::kMacroAmbiguous: return "MacroAmbiguous";
            case Code::kMacroRepeatEmpty: return "MacroRepeatEmpty";
            case Code::kMacroRecursionBudget: return "MacroRecursionBudget";
            case Code::kMacroReparseFail: return "MacroReparseFail";
            case Code::kMacroTokenPatternInvalid: return "MacroTokenPatternInvalid";
            case Code::kMacroTokenRepeatLengthMismatch: return "MacroTokenRepeatLengthMismatch";
            case Code::kMacroTokenVariadicOutsideRepeat: return "MacroTokenVariadicOutsideRepeat";
            case Code::kMacroPayloadExpected: return "MacroPayloadExpected";
            case Code::kMacroStringPayloadPlainOnly: return "MacroStringPayloadPlainOnly";
            case Code::kBareDollarStringRemoved: return "BareDollarStringRemoved";
            case Code::kCStringInteriorNulForbidden: return "CStringInteriorNulForbidden";
            case Code::kCStringLiteralRequiresCoreExt: return "CStringLiteralRequiresCoreExt";
            case Code::kPipeRhsMustBeCall: return "PipeRhsMustBeCall";
            case Code::kPipeFwdRhsMustBeCall: return "PipeFwdRhsMustBeCall";
            case Code::kPipeRevLhsMustBeCall: return "PipeRevLhsMustBeCall";
            case Code::kPipeRevNotSupportedYet: return "PipeRevNotSupportedYet";
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
            case Code::kVarConsumeElseOnlyOnLetSet: return "VarConsumeElseOnlyOnLetSet";
            case Code::kVarConsumeElseExpectedBlock: return "VarConsumeElseExpectedBlock";
            case Code::kVarConsumeElseWithActsUnsupported: return "VarConsumeElseWithActsUnsupported";
            case Code::kVarConsumeElseRequiresPlace: return "VarConsumeElseRequiresPlace";
            case Code::kVarConsumeElseRequiresOptionalPlace: return "VarConsumeElseRequiresOptionalPlace";
            case Code::kVarConsumeElseRequiresMutablePlace: return "VarConsumeElseRequiresMutablePlace";
            case Code::kVarConsumeElseMustDiverge: return "VarConsumeElseMustDiverge";

            case Code::kFnOnlyOneNamedGroupAllowed: return "FnOnlyOneNamedGroupAllowed";
            case Code::kFnNamedGroupMixedWithPositional: return "FnNamedGroupMixedWithPositional";
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
            case Code::kProtoRequireTrailingCommaNotAllowed: return "ProtoRequireTrailingCommaNotAllowed";
            case Code::kProtoImplTargetNotSupported: return "ProtoImplTargetNotSupported";
            case Code::kProtoImplMissingMember: return "ProtoImplMissingMember";
            case Code::kProtoConstraintUnsatisfied: return "ProtoConstraintUnsatisfied";
            case Code::kProtoSelfParamForbidden: return "ProtoSelfParamForbidden";
            case Code::kProtoArrowMemberNotFound: return "ProtoArrowMemberNotFound";
            case Code::kProtoArrowMemberAmbiguous: return "ProtoArrowMemberAmbiguous";
            case Code::kProtoArrowQualifierRequired: return "ProtoArrowQualifierRequired";
            case Code::kProtoDependencyCycle: return "ProtoDependencyCycle";
            case Code::kRequireExprTypeNotBool: return "RequireExprTypeNotBool";
            case Code::kRequireExprTooComplex: return "RequireExprTooComplex";
            case Code::kRequireUnsatisfied: return "RequireUnsatisfied";
            case Code::kDirectiveLegacyIfRemoved: return "DirectiveLegacyIfRemoved";
            case Code::kDirectiveCallExpected: return "DirectiveCallExpected";
            case Code::kDirectiveItemExpected: return "DirectiveItemExpected";
            case Code::kDirectiveIntrinsicSyntax: return "DirectiveIntrinsicSyntax";
            case Code::kDirectiveIntrinsicPolicyPending: return "DirectiveIntrinsicPolicyPending";
            case Code::kDirectiveInstBodyUnsupportedStmt: return "DirectiveInstBodyUnsupportedStmt";
            case Code::kDirectiveInstExprUnsupported: return "DirectiveInstExprUnsupported";
            case Code::kDirectiveInstMissingReturn: return "DirectiveInstMissingReturn";
            case Code::kDirectiveInstReturnMustBeBool: return "DirectiveInstReturnMustBeBool";
            case Code::kDirectiveInstRecursion: return "DirectiveInstRecursion";
            case Code::kDirectiveInstCallArgMismatch: return "DirectiveInstCallArgMismatch";
            case Code::kDirectiveInstUnknown: return "DirectiveInstUnknown";
            case Code::kDirectiveInstExternalPayloadInvalid: return "DirectiveInstExternalPayloadInvalid";
            case Code::kDirectiveInstForbiddenOperator: return "DirectiveInstForbiddenOperator";
            case Code::kDirectiveCoreMarkerMustBeHeader: return "DirectiveCoreMarkerMustBeHeader";
            case Code::kDirectiveCoreMarkerDuplicate: return "DirectiveCoreMarkerDuplicate";
            case Code::kCImportAliasRequired: return "CImportAliasRequired";
            case Code::kCImportHeaderLiteralExpected: return "CImportHeaderLiteralExpected";
            case Code::kCImportLibClangUnavailable: return "CImportLibClangUnavailable";
            case Code::kCImportFnMacroSkipped: return "CImportFnMacroSkipped";
            case Code::kCAbiCallPositionalOnly: return "CAbiCallPositionalOnly";
            case Code::kCAbiFormatStringForbidden: return "CAbiFormatStringForbidden";
            case Code::kManualAbiRequired: return "ManualAbiRequired";
            case Code::kCallLabeledNotAllowedForPositionalFn: return "CallLabeledNotAllowedForPositionalFn";
            case Code::kCallPositionalNotAllowedForNamedGroupFn: return "CallPositionalNotAllowedForNamedGroupFn";
            case Code::kCImportVariadicCallUnsupported: return "CImportVariadicCallUnsupported";
            case Code::kCImportVariadicArgTypeUnsupported: return "CImportVariadicArgTypeUnsupported";
            case Code::kCImportFormatBridgeShapeUnsupported: return "CImportFormatBridgeShapeUnsupported";
            case Code::kCImportFormatBridgeTypeUnsupported: return "CImportFormatBridgeTypeUnsupported";
            case Code::kCImportFormatBridgeNoVariadicSibling: return "CImportFormatBridgeNoVariadicSibling";
            case Code::kCImportFormatBridgeDynamicTextUnsupported: return "CImportFormatBridgeDynamicTextUnsupported";
            case Code::kFStringShortFormUnsupported: return "FStringShortFormUnsupported";
            case Code::kFStringRuntimeShapeUnsupported: return "FStringRuntimeShapeUnsupported";
            case Code::kFStringRuntimeExprMustBeText: return "FStringRuntimeExprMustBeText";
            case Code::kClassLifecycleDefaultParamNotAllowed: return "ClassLifecycleDefaultParamNotAllowed";
            case Code::kClassLifecycleSelfNotAllowed: return "ClassLifecycleSelfNotAllowed";
            case Code::kClassLifecycleDirectCallForbidden: return "ClassLifecycleDirectCallForbidden";
            case Code::kClassMemberLetSetRemoved: return "ClassMemberLetSetRemoved";
            case Code::kClassMemberFieldInitNotAllowed: return "ClassMemberFieldInitNotAllowed";
            case Code::kClassStaticMutNotAllowed: return "ClassStaticMutNotAllowed";
            case Code::kClassStaticVarRequiresInitializer: return "ClassStaticVarRequiresInitializer";
            case Code::kClassInheritanceNotAllowed: return "ClassInheritanceNotAllowed";
            case Code::kClassPrivateMemberAccessDenied: return "ClassPrivateMemberAccessDenied";
            case Code::kActorRequiresSingleDraft: return "ActorRequiresSingleDraft";
            case Code::kActorMemberNotAllowed: return "ActorMemberNotAllowed";
            case Code::kActorDeinitNotAllowed: return "ActorDeinitNotAllowed";
            case Code::kActorMethodModeRequired: return "ActorMethodModeRequired";
            case Code::kActorLifecycleDirectCallForbidden: return "ActorLifecycleDirectCallForbidden";
            case Code::kActorCtorMissingInit: return "ActorCtorMissingInit";
            case Code::kActorNotAvailableInNoStd: return "ActorNotAvailableInNoStd";
            case Code::kActorPathCallRemoved: return "ActorPathCallRemoved";
            case Code::kActorCommitOnlyInPub: return "ActorCommitOnlyInPub";
            case Code::kActorRecastOnlyInSub: return "ActorRecastOnlyInSub";
            case Code::kActorPubMissingTopLevelCommit: return "ActorPubMissingTopLevelCommit";
            case Code::kActorEscapeDraftMoveNotAllowed: return "ActorEscapeDraftMoveNotAllowed";
            case Code::kActorSelfReceiverNotAllowed: return "ActorSelfReceiverNotAllowed";
            case Code::kActorSelfFieldAccessUseDraft: return "ActorSelfFieldAccessUseDraft";
            case Code::kActorBraceInitNotAllowed: return "ActorBraceInitNotAllowed";
            case Code::kEnumVariantDuplicate: return "EnumVariantDuplicate";
            case Code::kEnumCtorArgMismatch: return "EnumCtorArgMismatch";
            case Code::kEnumCtorLabelMismatch: return "EnumCtorLabelMismatch";
            case Code::kEnumCtorTypeMismatch: return "EnumCtorTypeMismatch";
            case Code::kEnumDotFieldAccessForbidden: return "EnumDotFieldAccessForbidden";
            case Code::kEnumLayoutCPayloadNotAllowed: return "EnumLayoutCPayloadNotAllowed";
            case Code::kEnumDiscriminantNonCForbidden: return "EnumDiscriminantNonCForbidden";
            case Code::kEnumSwitchBindUnknownField: return "EnumSwitchBindUnknownField";
            case Code::kEnumProtoRequiresDefaultOnly: return "EnumProtoRequiresDefaultOnly";
            case Code::kEnumCAbiDirectSignatureForbidden: return "EnumCAbiDirectSignatureForbidden";
            case Code::kThrowOnlyInThrowingFn: return "ThrowOnlyInThrowingFn";
            case Code::kTryCatchOnlyInThrowingFn: return "TryCatchOnlyInThrowingFn";
            case Code::kTryExprOperandMustBeThrowingCall: return "TryExprOperandMustBeThrowingCall";
            case Code::kTryExprCAbiCallNotAllowed: return "TryExprCAbiCallNotAllowed";
            case Code::kThrowingCallRequiresTryExpr: return "ThrowingCallRequiresTryExpr";
            case Code::kThrowPayloadTypeNotAllowed: return "ThrowPayloadTypeNotAllowed";
            case Code::kThrowPayloadMustBeRecoverable: return "ThrowPayloadMustBeRecoverable";
            case Code::kTryCatchNeedsAtLeastOneCatch: return "TryCatchNeedsAtLeastOneCatch";
            case Code::kCatchBinderNameExpected: return "CatchBinderNameExpected";
            case Code::kUntypedCatchBinderRethrowOnly: return "UntypedCatchBinderRethrowOnly";
            case Code::kTryCatchExpectedCatchClause: return "TryCatchExpectedCatchClause";
            case Code::kExceptionLoweringDeferredV0: return "ExceptionLoweringDeferredV0";
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
            case Code::kLoopIterableUnsupported:     return "LoopIterableUnsupported";
            case Code::kLoopRangeBoundMustBeInteger: return "LoopRangeBoundMustBeInteger";
            case Code::kLoopRangeBoundTypeMismatch:  return "LoopRangeBoundTypeMismatch";
            case Code::kLoopRangeNeedsTypedBound:    return "LoopRangeNeedsTypedBound";
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
            case Code::kBorrowOperandMustBeOwnedPlace: return "BorrowOperandMustBeOwnedPlace";
            case Code::kEscapeOperandMustBePlace: return "EscapeOperandMustBePlace";
            case Code::kEscapeDerefSourceNotAllowed: return "EscapeDerefSourceNotAllowed";
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
            case Code::kUseAfterMove: return "UseAfterMove";
            case Code::kMaybeUninitializedMoveOnlyUse: return "MaybeUninitializedMoveOnlyUse";
            case Code::kMoveFromNonRootPlaceNotAllowed: return "MoveFromNonRootPlaceNotAllowed";
            case Code::kMoveFromGlobalOrStaticForbidden: return "MoveFromGlobalOrStaticForbidden";
            case Code::kRawPointerOwnerPointeeReadNotAllowed: return "RawPointerOwnerPointeeReadNotAllowed";
            case Code::kRawPointerOwnerPointeeWriteNotAllowed: return "RawPointerOwnerPointeeWriteNotAllowed";
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
            case Code::kAbiCThrowingFnNotAllowed:return "AbiCThrowingFnNotAllowed";
            case Code::kAbiCExternalThrowingMetadataInvalid:return "AbiCExternalThrowingMetadataInvalid";
            case Code::kAbiCGlobalMustBeStatic:return "AbiCGlobalMustBeStatic";
            case Code::kTypeReturnOutsideFn:  return "TypeReturnOutsideFn";
            case Code::kTypeReturnExprRequired:return "TypeReturnExprRequired";
            case Code::kTypeBreakValueOnlyInLoopExpr:return "TypeBreakValueOnlyInLoopExpr";
            case Code::kTypeUnaryBangMustBeBool:return "TypeUnaryBangMustBeBool";
            case Code::kTypeUnaryBitNotMustBeInteger:return "TypeUnaryBitNotMustBeInteger";
            case Code::kTypeBoolNegationUseNot:return "TypeBoolNegationUseNot";
            case Code::kTypeBinaryOperandsMustMatch:return "TypeBinaryOperandsMustMatch";
            case Code::kTypeCompareOperandsMustMatch:return "TypeCompareOperandsMustMatch";
            case Code::kTypeBorrowNotAllowedInPureComptime:return "TypeBorrowNotAllowedInPureComptime";
            case Code::kTypeEscapeNotAllowedInPureComptime:return "TypeEscapeNotAllowedInPureComptime";
            case Code::kTypeMismatch:         return "TypeMismatch";
            case Code::kTypeNotCallable:      return "TypeNotCallable";
            case Code::kTypeCondMustBeBool:   return "TypeCondMustBeBool";
            case Code::kTypeIndexMustBeUSize: return "TypeIndexMustBeUSize";
            case Code::kTypeIndexNonArray:    return "TypeIndexNonArray";
            case Code::kTypeSliceConstRangeInvalid: return "TypeSliceConstRangeInvalid";
            case Code::kTypeSliceConstOutOfBounds: return "TypeSliceConstOutOfBounds";
            case Code::kSetCannotInferFromNull: return "SetCannotInferFromNull";
            case Code::kMissingReturn: return "MissingReturn";

            case Code::kAssignLhsMustBePlace: return "AssignLhsMustBePlace";
            case Code::kPostfixOperandMustBePlace: return "PostfixOperandMustBePlace";
            case Code::kCopyCloneOperandMustBePlace: return "CopyCloneOperandMustBePlace";
            case Code::kCopyNotSupportedForType: return "CopyNotSupportedForType";
            case Code::kCloneNotSupportedForType: return "CloneNotSupportedForType";

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
            case Code::kGenericConstraintTypeMismatch: return "GenericConstraintTypeMismatch";
            case Code::kGenericUnknownTypeParamInConstraint: return "GenericUnknownTypeParamInConstraint";
            case Code::kGenericDeclConstraintUnsatisfied: return "GenericDeclConstraintUnsatisfied";
            case Code::kGenericTypePathArityMismatch: return "GenericTypePathArityMismatch";
            case Code::kGenericTypePathTemplateNotFound: return "GenericTypePathTemplateNotFound";
            case Code::kGenericTypePathTemplateKindMismatch: return "GenericTypePathTemplateKindMismatch";
            case Code::kGenericActsOverlap: return "GenericActsOverlap";
            case Code::kGenericActorDeclNotSupportedV1: return "GenericActorDeclNotSupportedV1";
            case Code::kTemplateSidecarUnavailable: return "TemplateSidecarUnavailable";
            case Code::kTemplateSidecarSchema: return "TemplateSidecarSchema";
            case Code::kTemplateSidecarUnsupportedClosure: return "TemplateSidecarUnsupportedClosure";
            case Code::kTemplateSidecarMissingNode: return "TemplateSidecarMissingNode";
            case Code::kTemplateSidecarConflictingNode: return "TemplateSidecarConflictingNode";
            case Code::kConstExprNotEvaluable: return "ConstExprNotEvaluable";
            case Code::kConstExprCallNotSupported: return "ConstExprCallNotSupported";
            case Code::kConstExprCycle: return "ConstExprCycle";
            case Code::kConstFnCallsNonConstFn: return "ConstFnCallsNonConstFn";
            case Code::kConstFnBodyUnsupportedStmt: return "ConstFnBodyUnsupportedStmt";
            case Code::kConstLoopExprNotSupported: return "ConstLoopExprNotSupported";
            case Code::kConstEvalCallDepthExceeded: return "ConstEvalCallDepthExceeded";
            case Code::kConstEvalStepLimitExceeded: return "ConstEvalStepLimitExceeded";
            case Code::kConstGlobalCompositeNotSupported: return "ConstGlobalCompositeNotSupported";

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
            case Code::kAmbiguousAmpPrefixChain: return "ambiguous '&' prefix chain (3+ consecutive '&'); use parentheses (e.g. ~( &x ) or &(~x))";
            case Code::kLegacyEscapeCaretAmpUseTilde: return "legacy escape syntax '^&' is removed; use '~' instead";
            case Code::kSelfMutSyntaxRemoved: return "receiver syntax 'self mut' is removed; use 'mut self' instead";
            case Code::kArraySizeExpectedIntLiteral: return "array size must be an integer literal (use T[N] or T[])";
            case Code::kArraySizeInvalidLiteral: return "invalid array size literal '{0}' (expected decimal u32 range)";
            case Code::kMacroNoMatch: return "no matching macro arm for call '{0}'";
            case Code::kMacroAmbiguous: return "ambiguous macro expansion for call '{0}'";
            case Code::kMacroRepeatEmpty: return "macro repetition body can match empty token stream";
            case Code::kMacroRecursionBudget: return "macro expansion budget exceeded for '{0}'";
            case Code::kMacroReparseFail: return "expanded macro output failed to parse as expected kind '{0}'";
            case Code::kMacroTokenPatternInvalid: return "invalid 'with token' pattern/template: {0}";
            case Code::kMacroTokenRepeatLengthMismatch: return "token repetition capture length mismatch: {0}";
            case Code::kMacroTokenVariadicOutsideRepeat: return "variadic capture '{0}' must be expanded with repetition context";
            case Code::kMacroPayloadExpected: return "macro call payload is required after '$path' (use (...), \"...\", or { ... })";
            case Code::kMacroStringPayloadPlainOnly: return "string payload form accepts plain string literal only (\"...\")";
            case Code::kBareDollarStringRemoved: return "bare $\"...\" is removed; use $foo\"...\"";
            case Code::kCStringInteriorNulForbidden: return "c\"...\"/cr\"...\" must not contain interior NUL byte";
            case Code::kCStringLiteralRequiresCoreExt: return "c\"...\"/cr\"...\" requires core::ext::CStr (core injection unavailable)";
            case Code::kPipeRhsMustBeCall: return "pipe operator requires a function call on the required side";
            case Code::kPipeFwdRhsMustBeCall: return "pipe operator '|>' requires a function call on the right-hand side";
            case Code::kPipeRevLhsMustBeCall: return "pipe operator '<|' requires a function call on the left-hand side";
            case Code::kPipeRevNotSupportedYet: return "pipe operator '<|' is reserved but not supported in v1";
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
            case Code::kVarConsumeElseOnlyOnLetSet: return "consume-binding else is only allowed on local let/set declarations";
            case Code::kVarConsumeElseExpectedBlock: return "consume-binding requires an else block '{ ... }'";
            case Code::kVarConsumeElseWithActsUnsupported: return "consume-binding does not support 'with acts(...)' on the same declaration in this round";
            case Code::kVarConsumeElseRequiresPlace: return "consume-binding rhs must be a mutable place expression";
            case Code::kVarConsumeElseRequiresOptionalPlace: return "consume-binding rhs must have optional type 'T?'";
            case Code::kVarConsumeElseRequiresMutablePlace: return "consume-binding rhs place must be writable";
            case Code::kVarConsumeElseMustDiverge: return "consume-binding else block must not fall through";

            case Code::kFnOnlyOneNamedGroupAllowed: return "function parameters allow at most one named-group '{ ... }'";
            case Code::kFnNamedGroupMixedWithPositional: return "function declaration must be either positional-only '(a: T, b: U)' or named-group-only '({a: T, ...})'";
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
            case Code::kProtoRequireExprTooComplex: return "require(...) supports only simple boolean folding (true/false/not/and/or/==/!=) in v1";
            case Code::kProtoRequireTrailingCommaNotAllowed: return "trailing comma is not allowed in proto require list";
            case Code::kProtoImplTargetNotSupported: return "implementation target is not a supported proto";
            case Code::kProtoImplMissingMember: return "proto implementation is missing a required member";
            case Code::kProtoConstraintUnsatisfied: return "proto constraint is not satisfied";
            case Code::kProtoSelfParamForbidden: return "proto require/provide function must not declare self parameter";
            case Code::kProtoArrowMemberNotFound: return "proto member is not found on arrow access";
            case Code::kProtoArrowMemberAmbiguous: return "proto arrow member is ambiguous";
            case Code::kProtoArrowQualifierRequired: return "proto arrow member requires an explicit proto qualifier";
            case Code::kProtoDependencyCycle: return "proto/type/acts dependency cycle detected";
            case Code::kRequireExprTypeNotBool: return "require(expr) must evaluate to bool";
            case Code::kRequireExprTooComplex: return "require(expr) supports only simple boolean folding (true/false/not/and/or/==/!=) in v0";
            case Code::kRequireUnsatisfied: return "require(expr) evaluated to false";
            case Code::kDirectiveLegacyIfRemoved: return "$[if(...)] is removed; use $[If(...)]";
            case Code::kDirectiveCallExpected: return "$[...] expects a call expression like $[If(...)]";
            case Code::kDirectiveItemExpected: return "$[...] must be followed by an item declaration";
            case Code::kDirectiveIntrinsicSyntax: return "$![...] syntax is invalid";
            case Code::kDirectiveIntrinsicPolicyPending: return "$![...] is parsed, but intrinsic binding policy is not fixed yet";
            case Code::kDirectiveInstBodyUnsupportedStmt: return "inst body supports only let/if/return statements";
            case Code::kDirectiveInstExprUnsupported: return "inst expression uses an unsupported construct";
            case Code::kDirectiveInstMissingReturn: return "inst evaluation reached end without a return";
            case Code::kDirectiveInstReturnMustBeBool: return "inst must return bool";
            case Code::kDirectiveInstRecursion: return "inst recursive or cyclic call is not allowed";
            case Code::kDirectiveInstCallArgMismatch: return "inst call argument count/type mismatch";
            case Code::kDirectiveInstUnknown: return "unknown inst used in $[...]";
            case Code::kDirectiveInstExternalPayloadInvalid: return "external inst payload is malformed";
            case Code::kDirectiveInstForbiddenOperator: return "'&', '~', and '^&' are forbidden in inst";
            case Code::kDirectiveCoreMarkerMustBeHeader: return "$![Impl::Core]; must be the first top-level item in the file header";
            case Code::kDirectiveCoreMarkerDuplicate: return "$![Impl::Core]; can only appear once per file";
            case Code::kCImportAliasRequired: return "c-header import requires alias form: import \"Header.h\" as alias;";
            case Code::kCImportHeaderLiteralExpected: return "c-header import expects plain string literal: import \"Header.h\" as alias;";
            case Code::kCImportLibClangUnavailable: return "c-header import is unavailable because libclang is not configured";
            case Code::kCImportFnMacroSkipped: return "function-like C macro was skipped under strict promotion rules";
            case Code::kCAbiCallPositionalOnly: return "C ABI calls allow positional arguments only";
            case Code::kCAbiFormatStringForbidden: return "C ABI call does not allow format-string literals ($\"...\"/F\"\"\"...\"\")";
            case Code::kManualAbiRequired: return "this ABI-risk operation requires manual[abi]";
            case Code::kCallLabeledNotAllowedForPositionalFn: return "labeled-call form is not allowed for positional-only function";
            case Code::kCallPositionalNotAllowedForNamedGroupFn: return "positional-call form is not allowed for named-group-only function";
            case Code::kCImportVariadicCallUnsupported: return "c variadic function calls are not supported in v1";
            case Code::kCImportVariadicArgTypeUnsupported: return "unsupported argument type in C variadic call";
            case Code::kCImportFormatBridgeShapeUnsupported: return "C format bridge expects a single argument in the form callee($\"...\")";
            case Code::kCImportFormatBridgeTypeUnsupported: return "unsupported interpolation type in C format bridge";
            case Code::kCImportFormatBridgeNoVariadicSibling: return "vlist format target has no variadic sibling for bridge rewrite";
            case Code::kCImportFormatBridgeDynamicTextUnsupported: return "dynamic text interpolation for %s is not supported in v1";
            case Code::kFStringShortFormUnsupported: return "F\"...\" is not supported; use F\"\"\"...\"\"\"";
            case Code::kFStringRuntimeShapeUnsupported: return "runtime f-string supports only a single interpolation in the form $\"{text_expr}\" in v1";
            case Code::kFStringRuntimeExprMustBeText: return "runtime f-string interpolation expression must have type text";
            case Code::kClassLifecycleDefaultParamNotAllowed: return "init()/deinit() = default only supports an empty parameter list";
            case Code::kClassLifecycleSelfNotAllowed: return "class lifecycle members must not declare a self receiver";
            case Code::kClassLifecycleDirectCallForbidden: return "init/deinit cannot be called directly; lifecycle is compiler-managed";
            case Code::kClassMemberLetSetRemoved: return "class member let/set syntax is removed; use 'name: Type;' for instance fields";
            case Code::kClassMemberFieldInitNotAllowed: return "class instance struct declaration must not include an initializer";
            case Code::kClassStaticMutNotAllowed: return "class static mut members are not supported in v0";
            case Code::kClassStaticVarRequiresInitializer: return "class static variable requires an initializer";
            case Code::kClassInheritanceNotAllowed: return "class-to-class inheritance is not allowed; class can only implement proto constraints";
            case Code::kClassPrivateMemberAccessDenied: return "private class member is only accessible inside its declaring class";
            case Code::kActorRequiresSingleDraft: return "actor must declare exactly one draft block";
            case Code::kActorMemberNotAllowed: return "actor body only allows draft/init/def sub|pub declarations";
            case Code::kActorDeinitNotAllowed: return "actor does not support deinit() in v0";
            case Code::kActorMethodModeRequired: return "actor method must be declared as 'def sub' or 'def pub'";
            case Code::kActorLifecycleDirectCallForbidden: return "actor init/deinit cannot be called directly; lifecycle is runtime-managed";
            case Code::kActorCtorMissingInit: return "actor constructor call '{0}(...)' requires at least one init(...) overload";
            case Code::kActorNotAvailableInNoStd: return "actor runtime is not available under '-fno-std'";
            case Code::kActorPathCallRemoved: return "actor member path call is removed; use dot call on actor value";
            case Code::kActorCommitOnlyInPub: return "commit is only allowed inside actor pub methods";
            case Code::kActorRecastOnlyInSub: return "recast is only allowed inside actor sub methods";
            case Code::kActorPubMissingTopLevelCommit: return "actor pub method must contain at least one top-level commit statement";
            case Code::kActorEscapeDraftMoveNotAllowed: return "actor draft cannot be moved with '~'";
            case Code::kActorSelfReceiverNotAllowed: return "actor methods and init() must not declare an explicit self receiver; use 'draft.x' inside the body";
            case Code::kActorSelfFieldAccessUseDraft: return "actor state access must use 'draft.{0}' instead of 'self.{0}'";
            case Code::kActorBraceInitNotAllowed: return "actor construction must use '{0}(...)', not '{0} { ... }'";
            case Code::kEnumVariantDuplicate: return "enum variant '{0}' is duplicated";
            case Code::kEnumCtorArgMismatch: return "enum constructor arguments do not match variant payload for '{0}'";
            case Code::kEnumCtorLabelMismatch: return "unknown or duplicated enum constructor label '{0}'";
            case Code::kEnumCtorTypeMismatch: return "enum constructor argument type mismatch for field '{0}': expected {1}, got {2}";
            case Code::kEnumDotFieldAccessForbidden: return "direct dot field access on enum value is not allowed; use switch pattern binding";
            case Code::kEnumLayoutCPayloadNotAllowed: return "layout(c) enum must be tag-only; payload variants are not allowed";
            case Code::kEnumDiscriminantNonCForbidden: return "explicit enum discriminant '= n' is only allowed on layout(c) tag-only enums";
            case Code::kEnumSwitchBindUnknownField: return "enum switch bind references unknown payload field '{0}'";
            case Code::kEnumProtoRequiresDefaultOnly: return "enum can only implement default-only proto in v0";
            case Code::kEnumCAbiDirectSignatureForbidden: return "enum direct type in C ABI signature is forbidden in v0; cast at boundary (as i32)";
            case Code::kThrowOnlyInThrowingFn: return "throw is only allowed inside throwing ('?') functions";
            case Code::kTryCatchOnlyInThrowingFn: return "try-catch is only allowed inside throwing ('?') functions";
            case Code::kTryExprOperandMustBeThrowingCall: return "try expression operand must be a throwing ('?') function call";
            case Code::kTryExprCAbiCallNotAllowed: return "try expression must not wrap a C ABI or cimport call; convert the boundary in an explicit wrapper";
            case Code::kThrowingCallRequiresTryExpr: return "direct call to throwing function is not allowed here; wrap the call with 'try <call>'";
            case Code::kThrowPayloadTypeNotAllowed: return "throw payload type is not allowed in v0 (expected enum/struct, got {0})";
            case Code::kThrowPayloadMustBeRecoverable: return "throw payload must satisfy Recoverable proto (got {0})";
            case Code::kTryCatchNeedsAtLeastOneCatch: return "try-catch requires at least one catch clause";
            case Code::kCatchBinderNameExpected: return "catch binder name is required (use: catch(name) or catch(name: Type))";
            case Code::kUntypedCatchBinderRethrowOnly: return "untyped catch binder is an opaque rethrow token and may only be used as 'throw e'";
            case Code::kTryCatchExpectedCatchClause: return "try block must be followed by at least one catch clause";
            case Code::kExceptionLoweringDeferredV0: return "exception lowering is deferred in v0 for function '{0}' (parser+tyck only)";
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
            case Code::kLoopIterableUnsupported:    return "loop header iterable does not satisfy the current iteration source requirements";
            case Code::kLoopRangeBoundMustBeInteger:return "loop range bounds must use builtin integer types";
            case Code::kLoopRangeBoundTypeMismatch: return "loop range bounds must have the same concrete integer type";
            case Code::kLoopRangeNeedsTypedBound:   return "loop range needs at least one typed integer bound; two infer-integer bounds are not allowed";
            
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
            case Code::kBorrowOperandMustBeOwnedPlace: return "&/&mut operand must be an owned place (cannot borrow from borrow/escape value)";
            case Code::kEscapeOperandMustBePlace: return "'~' operand must be a place expression";
            case Code::kEscapeDerefSourceNotAllowed: return "'~' cannot extract ownership through dereference";
            case Code::kEscapeSubplaceMoveNotAllowed: return "'~' only supports root identifier move in v0 (subplace move-out is not allowed)";
            case Code::kEscapeOperandMustNotBeBorrow: return "'~' cannot be applied to a borrow operand";
            case Code::kBorrowMutRequiresMutablePlace: return "&mut requires a mutable place";
            case Code::kBorrowMutConflict: return "cannot create borrow: an active mutable borrow already exists for this place";
            case Code::kBorrowSharedConflictWithMut: return "cannot create shared borrow '&': an active '&mut' borrow exists for this place";
            case Code::kBorrowMutConflictWithShared: return "cannot create '&mut': active shared borrow(s) exist for this place";
            case Code::kBorrowMutDirectAccessConflict: return "cannot access this place directly while an active '&mut' borrow exists";
            case Code::kBorrowSharedWriteConflict: return "cannot write to this place while active shared borrow(s) exist";
            case Code::kBorrowEscapeFromReturn: return "borrow value cannot be returned (non-escaping rule)";
            case Code::kBorrowEscapeToStorage: return "borrow value cannot be stored in an escaping/long-lived storage";
            case Code::kUseAfterEscapeMove: return "value was moved by '~' and cannot be used afterwards";
            case Code::kUseAfterMove: return "value '{0}' was moved and cannot be used until it is reinitialized";
            case Code::kMaybeUninitializedMoveOnlyUse: return "value '{0}' may be uninitialized after move on some control-flow path";
            case Code::kMoveFromNonRootPlaceNotAllowed: return "partial move from non-root place is not supported in v1";
            case Code::kMoveFromGlobalOrStaticForbidden: return "move from global/static storage '{0}' is not allowed in v1";
            case Code::kRawPointerOwnerPointeeReadNotAllowed: return "raw pointer dereference cannot read owner-typed pointee";
            case Code::kRawPointerOwnerPointeeWriteNotAllowed: return "raw pointer dereference cannot write owner-typed pointee";
            case Code::kEscapeWhileMutBorrowActive: return "cannot apply '~' while an active '&mut' borrow exists for this place";
            case Code::kEscapeWhileBorrowActive: return "cannot apply '~' while an active borrow exists for this place";
            case Code::kEscapeRequiresStaticOrBoundary: return "escaping '~' requires static storage or direct return/call-argument boundary";
            case Code::kSirUseAfterEscapeMove: return "use-after-move detected in SIR capability analysis ('~' moved value)";
            case Code::kSirEscapeBoundaryViolation: return "SIR capability analysis: escape rvalue must commit into an allowed owner cell or cross a call/return boundary";
            case Code::kSirEscapeMustNotMaterialize: return "SIR capability analysis: escape handle cannot be committed into this storage shape";

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
            case Code::kAbiCThrowingFnNotAllowed: return "C ABI function '{0}' must not be throwing ('?'); convert exception channel at boundary";
            case Code::kAbiCExternalThrowingMetadataInvalid: return "imported C ABI symbol '{0}' must not carry throwing metadata; C ABI functions are always non-throwing";
            case Code::kAbiCGlobalMustBeStatic: return "C ABI global '{0}' must be declared with 'static'";
            case Code::kTypeReturnOutsideFn:  return "return outside of function";
            case Code::kTypeReturnExprRequired: return "return expression is required (function does not return void)";
            case Code::kTypeBreakValueOnlyInLoopExpr: return "break with value is only allowed inside loop expressions";
            case Code::kTypeUnaryBangMustBeBool:return "logical negation requires bool (got {0})";
            case Code::kTypeUnaryBitNotMustBeInteger:return "prefix '!' is bitwise not and requires a builtin integer type (got {0})";
            case Code::kTypeBoolNegationUseNot:return "boolean negation must use 'not', not prefix '!'";
            case Code::kTypeBinaryOperandsMustMatch:return "binary arithmetic requires both operands to have the same type (lhs={0}, rhs={1})";
            case Code::kTypeCompareOperandsMustMatch:return "comparison requires both operands to have the same type (lhs={0}, rhs={1})";
            case Code::kTypeBorrowNotAllowedInPureComptime:return "borrow '&' is not allowed in pure/comptime functions";
            case Code::kTypeEscapeNotAllowedInPureComptime:return "escape '~' is not allowed in pure/comptime functions";
            case Code::kTypeMismatch: /* args[0]=expected, args[1]=got */ return "type mismatch: expected {0}, got {1}";
            case Code::kTypeNotCallable: /* args[0]=got_type */ return "cannot call non-function type {0}";
            case Code::kTypeCondMustBeBool: /* args[0]=got_type */ return "condition must be bool (got {0})";
            case Code::kTypeIndexMustBeUSize: /* args[0]=got_type */ return "index/slice bound must be an integer type (got {0})";
            case Code::kTypeIndexNonArray: /* args[0]=base_type */ return "cannot index non-array type {0}";
            case Code::kTypeSliceConstRangeInvalid: /* args[0]=lo, args[1]=hi_exclusive */ return "invalid constant slice range: lo ({0}) must be <= hi_exclusive ({1})";
            case Code::kTypeSliceConstOutOfBounds: /* args[0]=len, args[1]=hi_exclusive */ return "constant slice upper bound is out of range: hi_exclusive ({1}) exceeds array length ({0})";
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
            case Code::kCopyCloneOperandMustBePlace: return "copy/clone operand must be a place expression";
            case Code::kCopyNotSupportedForType: return "copy is not supported for type '{0}' (define operator(copy) in acts for this type)";
            case Code::kCloneNotSupportedForType: return "clone is not supported for type '{0}' (define operator(clone) in acts for this type)";

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
            case Code::kTypeFieldMemberMustBePodBuiltin: return "struct member '{0}' must use a POD builtin value type, `~T`/`(~T)?`, a recursively-sized owner array, or a storage-safe named aggregate in this round (got {1})";
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
            case Code::kGenericConstraintTypeMismatch: return "generic equality constraint '{0} == {1}' is not satisfied ('{2}' != '{3}')";
            case Code::kGenericUnknownTypeParamInConstraint: return "constraint references unknown generic type parameter '{0}'";
            case Code::kGenericDeclConstraintUnsatisfied: return "declaration generic constraint '{0}: {1}' is not satisfied by '{2}'";
            case Code::kGenericTypePathArityMismatch: return "generic type path arity mismatch on '{0}': expected {1}, got {2}";
            case Code::kGenericTypePathTemplateNotFound: return "generic type path target template not found: '{0}'";
            case Code::kGenericTypePathTemplateKindMismatch: return "generic type path '{0}' points to a template of different kind (expected {1}, got {2})";
            case Code::kGenericActsOverlap: return "generic acts overlap detected for owner '{0}' and member '{1}'";
            case Code::kGenericActorDeclNotSupportedV1: return "generic actor declaration is not supported in v1: '{0}'";
            case Code::kTemplateSidecarUnavailable: return "template-sidecar materialization is unavailable: '{0}'";
            case Code::kTemplateSidecarSchema: return "template-sidecar payload is invalid: '{0}'";
            case Code::kTemplateSidecarUnsupportedClosure: return "template-sidecar dependency closure is unsupported: '{0}'";
            case Code::kTemplateSidecarMissingNode: return "template-sidecar dependency node is missing: '{0}'";
            case Code::kTemplateSidecarConflictingNode: return "template-sidecar has conflicting duplicate node: '{0}'";
            case Code::kConstExprNotEvaluable: return "const expression is not evaluable: {0}";
            case Code::kConstExprCallNotSupported: return "function calls are not supported in const expressions (v1)";
            case Code::kConstExprCycle: return "const declaration cycle detected";
            case Code::kConstFnCallsNonConstFn: return "const evaluation can only call const def (callee: {0})";
            case Code::kConstFnBodyUnsupportedStmt: return "const def body contains unsupported statement: {0}";
            case Code::kConstLoopExprNotSupported: return "loop expression is not allowed in const evaluation (use while statement)";
            case Code::kConstEvalCallDepthExceeded: return "const evaluation call depth limit exceeded while calling '{0}'";
            case Code::kConstEvalStepLimitExceeded: return "const evaluation step limit exceeded (budget={0})";
            case Code::kConstGlobalCompositeNotSupported: return "global/static const composite initializer is not supported in v1: '{0}'";

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
            case Code::kAmbiguousAmpPrefixChain: return "'&' 접두사 체인이 모호합니다(연속 '&' 3개 이상). 괄호로 명시하세요(예: ~(&x), &(~x))";
            case Code::kLegacyEscapeCaretAmpUseTilde: return "legacy escape 문법 '^&'는 제거되었습니다. 대신 '~'를 사용하세요";
            case Code::kSelfMutSyntaxRemoved: return "리시버 문법 'self mut'는 제거되었습니다. 대신 'mut self'를 사용하세요";
            case Code::kArraySizeExpectedIntLiteral: return "배열 크기는 정수 리터럴이어야 합니다(T[N] 또는 T[] 사용)";
            case Code::kArraySizeInvalidLiteral: return "배열 크기 리터럴 '{0}'이(가) 유효하지 않습니다(10진 u32 범위 필요)";
            case Code::kMacroNoMatch: return "매크로 호출 '{0}'과(와) 일치하는 arm을 찾지 못했습니다";
            case Code::kMacroAmbiguous: return "매크로 호출 '{0}'의 확장이 모호합니다";
            case Code::kMacroRepeatEmpty: return "매크로 반복 본문이 빈 토큰 스트림과 매칭될 수 없습니다";
            case Code::kMacroRecursionBudget: return "매크로 '{0}' 확장 예산(깊이/단계)을 초과했습니다";
            case Code::kMacroReparseFail: return "매크로 확장 결과를 기대 kind '{0}'로 다시 파싱할 수 없습니다";
            case Code::kMacroTokenPatternInvalid: return "'with token' 패턴/템플릿이 올바르지 않습니다: {0}";
            case Code::kMacroTokenRepeatLengthMismatch: return "token 반복 캡처 길이가 일치하지 않습니다: {0}";
            case Code::kMacroTokenVariadicOutsideRepeat: return "variadic 캡처 '{0}'는 반복 문맥에서만 단일 항목으로 사용할 수 있습니다";
            case Code::kMacroPayloadExpected: return "매크로 호출 '$path' 뒤에는 payload가 필요합니다 ((...), \"...\", 또는 { ... })";
            case Code::kMacroStringPayloadPlainOnly: return "문자열 payload 형태는 일반 문자열 리터럴(\"...\")만 허용됩니다";
            case Code::kBareDollarStringRemoved: return "bare $\"...\" 형식은 제거되었습니다. $foo\"...\"를 사용하세요";
            case Code::kCStringInteriorNulForbidden: return "c\"...\"/cr\"...\"에는 중간 NUL 바이트를 포함할 수 없습니다";
            case Code::kCStringLiteralRequiresCoreExt: return "c\"...\"/cr\"...\"는 core::ext::CStr가 필요합니다 (core 주입 불가)";
            case Code::kPipeRhsMustBeCall: return "파이프 연산자는 필요한 쪽에 함수 호출이 있어야 합니다";
            case Code::kPipeFwdRhsMustBeCall: return "파이프 연산자 '|>'의 오른쪽은 함수 호출이어야 합니다";
            case Code::kPipeRevLhsMustBeCall: return "파이프 연산자 '<|'의 왼쪽은 함수 호출이어야 합니다";
            case Code::kPipeRevNotSupportedYet: return "파이프 연산자 '<|'는 예약되어 있으나 v1에서 지원되지 않습니다";
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
            case Code::kVarConsumeElseOnlyOnLetSet: return "consume-binding else는 지역 let/set 선언에서만 사용할 수 있습니다";
            case Code::kVarConsumeElseExpectedBlock: return "consume-binding에는 else 블록 '{ ... }'이 필요합니다";
            case Code::kVarConsumeElseWithActsUnsupported: return "이번 라운드에서는 consume-binding과 'with acts(...)'를 같은 선언에 함께 쓸 수 없습니다";
            case Code::kVarConsumeElseRequiresPlace: return "consume-binding의 오른쪽은 mutable place expression이어야 합니다";
            case Code::kVarConsumeElseRequiresOptionalPlace: return "consume-binding의 오른쪽은 optional 타입 'T?'이어야 합니다";
            case Code::kVarConsumeElseRequiresMutablePlace: return "consume-binding의 source place는 쓰기 가능해야 합니다";
            case Code::kVarConsumeElseMustDiverge: return "consume-binding의 else 블록은 fallthrough 없이 종료되어야 합니다";

            case Code::kFnOnlyOneNamedGroupAllowed: return "함수 파라미터에서는 named-group '{ ... }'를 최대 1개만 사용할 수 있습니다";
            case Code::kFnNamedGroupMixedWithPositional: return "함수 선언은 positional-only '(a: T, b: U)' 또는 named-group-only '({a: T, ...})' 중 하나여야 합니다";
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
            case Code::kProtoRequireExprTooComplex: return "v1에서 require(...)는 단순 bool 폴딩(true/false/not/and/or/==/!=)만 허용됩니다";
            case Code::kProtoRequireTrailingCommaNotAllowed: return "proto require 목록에서는 trailing comma를 사용할 수 없습니다";
            case Code::kProtoImplTargetNotSupported: return "구현 대상으로 지정한 항목이 지원되는 proto가 아닙니다";
            case Code::kProtoImplMissingMember: return "proto 구현에 필요한 멤버가 누락되었습니다";
            case Code::kProtoConstraintUnsatisfied: return "proto 제약을 만족하지 못했습니다";
            case Code::kProtoSelfParamForbidden: return "proto require/provide 함수는 self 파라미터를 선언할 수 없습니다";
            case Code::kProtoArrowMemberNotFound: return "-> 접근에서 proto 멤버를 찾을 수 없습니다";
            case Code::kProtoArrowMemberAmbiguous: return "-> 접근의 proto 멤버가 모호합니다";
            case Code::kProtoArrowQualifierRequired: return "-> 접근의 proto 멤버는 명시적 proto 한정자가 필요합니다";
            case Code::kProtoDependencyCycle: return "proto/type/acts 의존 순환이 감지되었습니다";
            case Code::kRequireExprTypeNotBool: return "require(expr)는 bool로 평가되어야 합니다";
            case Code::kRequireExprTooComplex: return "v0에서 require(expr)는 단순 bool 폴딩(true/false/not/and/or/==/!=)만 허용됩니다";
            case Code::kRequireUnsatisfied: return "require(expr)가 false로 평가되었습니다";
            case Code::kDirectiveLegacyIfRemoved: return "$[if(...)]는 제거되었습니다. $[If(...)]를 사용하세요";
            case Code::kDirectiveCallExpected: return "$[...] 안에는 $[If(...)] 형태의 호출식이 필요합니다";
            case Code::kDirectiveItemExpected: return "$[...] 뒤에는 item declaration이 와야 합니다";
            case Code::kDirectiveIntrinsicSyntax: return "$![...] 문법이 올바르지 않습니다";
            case Code::kDirectiveIntrinsicPolicyPending: return "$![...]는 파싱되지만 intrinsic 바인딩 정책은 아직 확정되지 않았습니다";
            case Code::kDirectiveInstBodyUnsupportedStmt: return "inst 본문은 let/if/return 문만 허용됩니다";
            case Code::kDirectiveInstExprUnsupported: return "inst 식에 지원되지 않는 구문이 포함되어 있습니다";
            case Code::kDirectiveInstMissingReturn: return "inst 평가가 return 없이 끝났습니다";
            case Code::kDirectiveInstReturnMustBeBool: return "inst는 bool 값을 반환해야 합니다";
            case Code::kDirectiveInstRecursion: return "inst 재귀/순환 호출은 허용되지 않습니다";
            case Code::kDirectiveInstCallArgMismatch: return "inst 호출 인자 개수/타입이 맞지 않습니다";
            case Code::kDirectiveInstUnknown: return "$[...]에서 사용한 inst를 찾을 수 없습니다";
            case Code::kDirectiveInstExternalPayloadInvalid: return "외부 inst payload 형식이 올바르지 않습니다";
            case Code::kDirectiveInstForbiddenOperator: return "inst에서는 '&', '~', '^&' 연산을 사용할 수 없습니다";
            case Code::kDirectiveCoreMarkerMustBeHeader: return "$![Impl::Core];는 파일 헤더의 첫 번째 top-level item이어야 합니다";
            case Code::kDirectiveCoreMarkerDuplicate: return "$![Impl::Core];는 파일당 1회만 선언할 수 있습니다";
            case Code::kCImportAliasRequired: return "C 헤더 import는 별칭이 필요합니다: import \"Header.h\" as alias;";
            case Code::kCImportHeaderLiteralExpected: return "C 헤더 import는 일반 문자열 리터럴만 허용합니다: import \"Header.h\" as alias;";
            case Code::kCImportLibClangUnavailable: return "libclang이 구성되지 않아 C 헤더 import를 사용할 수 없습니다";
            case Code::kCImportFnMacroSkipped: return "엄격 승격 규칙에 맞지 않아 함수형 C 매크로를 건너뛰었습니다";
            case Code::kCAbiCallPositionalOnly: return "C ABI 호출은 positional 인자만 허용합니다";
            case Code::kCAbiFormatStringForbidden: return "C ABI 호출 인자에서는 $\"...\"/F\"\"\"...\"\"\" 형식을 사용할 수 없습니다";
            case Code::kManualAbiRequired: return "이 ABI-risk 동작에는 manual[abi]가 필요합니다";
            case Code::kCallLabeledNotAllowedForPositionalFn: return "positional-only 함수에는 labeled-call 형태를 사용할 수 없습니다";
            case Code::kCallPositionalNotAllowedForNamedGroupFn: return "named-group-only 함수에는 positional-call 형태를 사용할 수 없습니다";
            case Code::kCImportVariadicCallUnsupported: return "v1에서는 C 가변 인자 함수 호출을 지원하지 않습니다";
            case Code::kCImportVariadicArgTypeUnsupported: return "C 가변 인자 구간에서 지원되지 않는 인자 타입입니다";
            case Code::kCImportFormatBridgeShapeUnsupported: return "C format 브리지는 callee($\"...\") 단일 인자 형태만 지원합니다";
            case Code::kCImportFormatBridgeTypeUnsupported: return "C format 브리지에서 지원되지 않는 보간 타입입니다";
            case Code::kCImportFormatBridgeNoVariadicSibling: return "vlist format 대상에 대응되는 variadic sibling을 찾을 수 없습니다";
            case Code::kCImportFormatBridgeDynamicTextUnsupported: return "v1에서는 %s용 동적 text 보간을 지원하지 않습니다";
            case Code::kFStringShortFormUnsupported: return "F\"...\"는 지원되지 않습니다. F\"\"\"...\"\"\"를 사용하세요";
            case Code::kFStringRuntimeShapeUnsupported: return "v1 런타임 f-string은 $\"{text_expr}\" 형태의 단일 보간만 지원합니다";
            case Code::kFStringRuntimeExprMustBeText: return "런타임 f-string 보간 식의 타입은 text여야 합니다";
            case Code::kClassLifecycleDefaultParamNotAllowed: return "init()/deinit() = default 는 빈 파라미터 목록만 허용합니다";
            case Code::kClassLifecycleSelfNotAllowed: return "class lifecycle 멤버에는 self 리시버를 선언할 수 없습니다";
            case Code::kClassLifecycleDirectCallForbidden: return "init/deinit 는 직접 호출할 수 없습니다. lifecycle 호출은 컴파일러가 관리합니다";
            case Code::kClassMemberLetSetRemoved: return "class 멤버 let/set 문법은 제거되었습니다. 인스턴스 필드는 'name: Type;'을 사용하세요";
            case Code::kClassMemberFieldInitNotAllowed: return "class 인스턴스 필드 선언에는 초기화식을 둘 수 없습니다";
            case Code::kClassStaticMutNotAllowed: return "class static mut 멤버는 v0에서 지원하지 않습니다";
            case Code::kClassStaticVarRequiresInitializer: return "class static 변수는 초기화식이 반드시 필요합니다";
            case Code::kClassInheritanceNotAllowed: return "class 간 상속은 허용되지 않습니다. class는 proto 제약만 구현할 수 있습니다";
            case Code::kClassPrivateMemberAccessDenied: return "private class 멤버는 선언한 class 내부에서만 접근할 수 있습니다";
            case Code::kActorRequiresSingleDraft: return "actor는 draft 블록을 정확히 1개 선언해야 합니다";
            case Code::kActorMemberNotAllowed: return "actor 본문에는 draft/init/def sub|pub 선언만 허용됩니다";
            case Code::kActorDeinitNotAllowed: return "actor는 v0에서 deinit()을 지원하지 않습니다";
            case Code::kActorMethodModeRequired: return "actor 메서드는 'def sub' 또는 'def pub'로 선언해야 합니다";
            case Code::kActorLifecycleDirectCallForbidden: return "actor init/deinit은 직접 호출할 수 없습니다";
            case Code::kActorCtorMissingInit: return "actor 생성 호출 '{0}(...)'에는 최소 1개의 init(...) 오버로드가 필요합니다";
            case Code::kActorNotAvailableInNoStd: return "actor 런타임은 '-fno-std' 프로파일에서 사용할 수 없습니다";
            case Code::kActorPathCallRemoved: return "actor 멤버 경로 호출은 제거되었습니다. actor 값에 대해 dot 호출을 사용하세요";
            case Code::kActorCommitOnlyInPub: return "commit은 actor pub 메서드 내부에서만 사용할 수 있습니다";
            case Code::kActorRecastOnlyInSub: return "recast는 actor sub 메서드 내부에서만 사용할 수 있습니다";
            case Code::kActorPubMissingTopLevelCommit: return "actor pub 메서드에는 최상위 commit 문이 최소 1개 필요합니다";
            case Code::kActorEscapeDraftMoveNotAllowed: return "actor draft는 '~'로 move할 수 없습니다";
            case Code::kActorSelfReceiverNotAllowed: return "actor 메서드와 init()은 explicit self receiver를 선언할 수 없습니다. 본문에서는 'draft.x'를 사용하세요";
            case Code::kActorSelfFieldAccessUseDraft: return "actor 상태 접근은 'self.{0}' 대신 'draft.{0}'를 사용해야 합니다";
            case Code::kActorBraceInitNotAllowed: return "actor 생성은 '{0}(...)' 형식을 사용해야 하며 '{0} { ... }'는 허용되지 않습니다";
            case Code::kEnumVariantDuplicate: return "enum variant '{0}'이(가) 중복 선언되었습니다";
            case Code::kEnumCtorArgMismatch: return "enum 생성자 인자가 variant payload와 일치하지 않습니다: '{0}'";
            case Code::kEnumCtorLabelMismatch: return "enum 생성자 라벨이 잘못되었거나 중복되었습니다: '{0}'";
            case Code::kEnumCtorTypeMismatch: return "enum 생성자 인자 타입이 다릅니다: 필드 '{0}'는 {1}이어야 하지만 {2}가 전달되었습니다";
            case Code::kEnumDotFieldAccessForbidden: return "enum 값에 대한 직접 dot 필드 접근은 허용되지 않습니다. switch 패턴 바인딩을 사용하세요";
            case Code::kEnumLayoutCPayloadNotAllowed: return "layout(c) enum은 tag-only여야 하며 payload variant를 포함할 수 없습니다";
            case Code::kEnumDiscriminantNonCForbidden: return "enum의 명시적 discriminant(= n)는 layout(c) tag-only enum에서만 허용됩니다";
            case Code::kEnumSwitchBindUnknownField: return "enum switch 바인딩이 존재하지 않는 payload 필드 '{0}'를 참조합니다";
            case Code::kEnumProtoRequiresDefaultOnly: return "v0에서 enum은 default-only proto만 구현할 수 있습니다";
            case Code::kEnumCAbiDirectSignatureForbidden: return "v0에서 C ABI 시그니처에 enum 직접 타입은 금지됩니다. 경계에서 as i32 캐스팅을 사용하세요";
            case Code::kThrowOnlyInThrowingFn: return "throw는 throwing('?') 함수 내부에서만 사용할 수 있습니다";
            case Code::kTryCatchOnlyInThrowingFn: return "try-catch는 throwing('?') 함수 내부에서만 사용할 수 있습니다";
            case Code::kTryExprOperandMustBeThrowingCall: return "try 식의 피연산자는 throwing('?') 함수 호출이어야 합니다";
            case Code::kTryExprCAbiCallNotAllowed: return "try 식은 C ABI 또는 cimport 호출을 감쌀 수 없습니다. 명시적 wrapper에서 경계를 변환하세요";
            case Code::kThrowingCallRequiresTryExpr: return "여기서는 throwing 함수 직접 호출이 허용되지 않습니다. 'try <call>'로 감싸야 합니다";
            case Code::kThrowPayloadTypeNotAllowed: return "v0에서 throw payload 타입이 허용되지 않습니다(enum/struct 필요, 현재 {0})";
            case Code::kThrowPayloadMustBeRecoverable: return "throw payload는 Recoverable proto를 만족해야 합니다(현재 {0})";
            case Code::kTryCatchNeedsAtLeastOneCatch: return "try-catch에는 최소 1개의 catch 절이 필요합니다";
            case Code::kCatchBinderNameExpected: return "catch 바인더 이름이 필요합니다 (예: catch(name), catch(name: Type))";
            case Code::kUntypedCatchBinderRethrowOnly: return "untyped catch 바인더는 opaque rethrow token이며 'throw e' 형태로만 사용할 수 있습니다";
            case Code::kTryCatchExpectedCatchClause: return "try 블록 뒤에는 최소 1개의 catch 절이 와야 합니다";
            case Code::kExceptionLoweringDeferredV0: return "함수 '{0}'의 예외 lowering은 v0에서 아직 미구현입니다(parser+tyck 단계까지만 지원)";
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
            case Code::kLoopIterableUnsupported:    return "loop 헤더 iterable은 현재 v0에서 지원되지 않습니다; 지금은 T[N], T[], 범위(a..b, a..:b)만 허용됩니다";
            case Code::kLoopRangeBoundMustBeInteger:return "loop 범위 경계는 builtin 정수 타입이어야 합니다";
            case Code::kLoopRangeBoundTypeMismatch: return "loop 범위 경계는 같은 구체 정수 타입이어야 합니다";
            case Code::kLoopRangeNeedsTypedBound:   return "loop 범위에는 최소 하나의 typed integer 경계가 필요합니다; 양쪽이 infer-int인 범위는 허용되지 않습니다";

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
            case Code::kBorrowOperandMustBeOwnedPlace: return "'&'/'&mut'의 피연산자는 owned place여야 합니다(borrow/escape 값 재참조 금지)";
            case Code::kEscapeOperandMustBePlace: return "'~'의 피연산자는 place expression이어야 합니다";
            case Code::kEscapeDerefSourceNotAllowed: return "'~'는 역참조 결과에서 소유권을 추출할 수 없습니다";
            case Code::kEscapeSubplaceMoveNotAllowed: return "v0에서 '~'는 루트 식별자만 이동할 수 있습니다(subplace move-out 금지)";
            case Code::kEscapeOperandMustNotBeBorrow: return "'~'는 borrow('& ...')에 적용할 수 없습니다";
            case Code::kBorrowMutRequiresMutablePlace: return "'&mut'는 mutable place에만 적용할 수 있습니다";
            case Code::kBorrowMutConflict: return "이미 활성화된 '&mut' borrow가 있어 추가 borrow를 만들 수 없습니다";
            case Code::kBorrowSharedConflictWithMut: return "활성 '&mut' borrow가 있는 동안에는 shared borrow('&')를 만들 수 없습니다";
            case Code::kBorrowMutConflictWithShared: return "활성 shared borrow가 있는 동안에는 '&mut' borrow를 만들 수 없습니다";
            case Code::kBorrowMutDirectAccessConflict: return "활성 '&mut' borrow가 있는 동안에는 해당 place를 직접 접근할 수 없습니다";
            case Code::kBorrowSharedWriteConflict: return "활성 shared borrow가 있는 동안에는 해당 place에 쓰기할 수 없습니다";
            case Code::kBorrowEscapeFromReturn: return "borrow 값은 반환할 수 없습니다(비탈출 규칙)";
            case Code::kBorrowEscapeToStorage: return "borrow 값은 탈출/장수명 저장소에 저장할 수 없습니다";
            case Code::kUseAfterEscapeMove: return "'~'로 move된 값은 이후 사용할 수 없습니다";
            case Code::kUseAfterMove: return "값 '{0}'은(는) move되어 재초기화 전까지 사용할 수 없습니다";
            case Code::kMaybeUninitializedMoveOnlyUse: return "값 '{0}'은(는) 일부 제어 흐름에서 move되어 미초기화 상태일 수 있습니다";
            case Code::kMoveFromNonRootPlaceNotAllowed: return "v1에서는 non-root place에서의 partial move를 지원하지 않습니다";
            case Code::kMoveFromGlobalOrStaticForbidden: return "global/static 저장소 '{0}'에서는 move를 수행할 수 없습니다";
            case Code::kRawPointerOwnerPointeeReadNotAllowed: return "raw pointer 역참조로 owner 타입 pointee를 읽을 수 없습니다";
            case Code::kRawPointerOwnerPointeeWriteNotAllowed: return "raw pointer 역참조로 owner 타입 pointee에 쓸 수 없습니다";
            case Code::kEscapeWhileMutBorrowActive: return "활성 '&mut' borrow가 있는 동안에는 해당 place에 '~'를 적용할 수 없습니다";
            case Code::kEscapeWhileBorrowActive: return "활성 borrow가 있는 동안에는 해당 place에 '~'를 적용할 수 없습니다";
            case Code::kEscapeRequiresStaticOrBoundary: return "'~' 탈출은 static 저장소이거나 return/호출 인자 경계에서 직접 사용되어야 합니다";
            case Code::kSirUseAfterEscapeMove: return "SIR capability 분석에서 use-after-move가 감지되었습니다('~'로 move된 값 사용)";
            case Code::kSirEscapeBoundaryViolation: return "SIR capability 분석: escape rvalue는 허용된 owner cell에 commit되거나 return/호출 인자 경계를 넘어야 합니다";
            case Code::kSirEscapeMustNotMaterialize: return "SIR capability 분석: escape handle은 이 storage shape으로 commit될 수 없습니다";

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
            case Code::kAbiCThrowingFnNotAllowed: return "C ABI 함수 '{0}'는 throwing('?')일 수 없습니다. 경계에서 예외 채널을 변환하세요";
            case Code::kAbiCExternalThrowingMetadataInvalid: return "imported C ABI 심볼 '{0}'에는 throwing 메타데이터를 붙일 수 없습니다. C ABI 함수는 항상 non-throwing입니다";
            case Code::kAbiCGlobalMustBeStatic: return "C ABI 전역 '{0}'는 반드시 'static'으로 선언해야 합니다";
            case Code::kTypeReturnOutsideFn:  return "함수 밖에서 return을 사용할 수 없습니다";
            case Code::kTypeReturnExprRequired:return "return에는 식이 필요합니다(현재 반환 타입이 void가 아닙니다)";
            case Code::kTypeBreakValueOnlyInLoopExpr: return "값을 가진 break는 loop 표현식 안에서만 허용됩니다";
            case Code::kTypeUnaryBangMustBeBool:return "논리 부정은 bool에만 사용할 수 있습니다(현재 {0})";
            case Code::kTypeUnaryBitNotMustBeInteger:return "prefix '!'는 bitwise not이며 builtin 정수 타입에만 사용할 수 있습니다(현재 {0})";
            case Code::kTypeBoolNegationUseNot:return "bool 부정은 prefix '!'가 아니라 'not'을 사용해야 합니다";
            case Code::kTypeBinaryOperandsMustMatch:return "산술 연산의 양쪽 피연산자 타입이 같아야 합니다(lhs={0}, rhs={1})";
            case Code::kTypeCompareOperandsMustMatch:return "비교 연산의 양쪽 피연산자 타입이 같아야 합니다(lhs={0}, rhs={1})";
            case Code::kTypeBorrowNotAllowedInPureComptime:return "pure/comptime 함수에서는 '&'를 사용할 수 없습니다";
            case Code::kTypeEscapeNotAllowedInPureComptime:return "pure/comptime 함수에서는 '~'를 사용할 수 없습니다";
            case Code::kTypeMismatch: /* args[0]=expected, args[1]=got */ return "타입이 일치하지 않습니다: 기대 {0}, 실제 {1}";
            case Code::kTypeNotCallable: /* args[0]=got_type */ return "함수가 아닌 타입 {0}은(는) 호출할 수 없습니다";
            case Code::kTypeCondMustBeBool: /* args[0]=got_type */ return "조건식은 bool이어야 합니다(현재 {0})";
            case Code::kTypeIndexMustBeUSize: /* args[0]=got_type */ return "인덱스/슬라이스 경계는 정수 타입이어야 합니다(현재 {0})";
            case Code::kTypeIndexNonArray: /* args[0]=base_type */ return "배열이 아닌 타입 {0}에는 인덱싱을 사용할 수 없습니다";
            case Code::kTypeSliceConstRangeInvalid: /* args[0]=lo, args[1]=hi_exclusive */ return "상수 슬라이스 범위가 잘못되었습니다: lo({0})는 hi_exclusive({1}) 이하여야 합니다";
            case Code::kTypeSliceConstOutOfBounds: /* args[0]=len, args[1]=hi_exclusive */ return "상수 슬라이스 상한이 배열 범위를 벗어났습니다: hi_exclusive({1}) > len({0})";
            
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
            case Code::kCopyCloneOperandMustBePlace: return "copy/clone 피연산자는 place expression이어야 합니다";
            case Code::kCopyNotSupportedForType: return "타입 '{0}'에는 copy를 사용할 수 없습니다(acts for 타입에 operator(copy)를 정의하세요)";
            case Code::kCloneNotSupportedForType: return "타입 '{0}'에는 clone을 사용할 수 없습니다(acts for 타입에 operator(clone)를 정의하세요)";

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
            case Code::kTypeFieldMemberMustBePodBuiltin: return "struct 멤버 '{0}'는 이번 라운드에서 POD 내장 값 타입, `~T`/`(~T)?`, 재귀적인 sized owner array, 또는 storage-safe named aggregate 여야 합니다(현재 {1})";
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
            case Code::kGenericConstraintTypeMismatch: return "제네릭 동치 제약 '{0} == {1}'을(를) 만족하지 않습니다 ('{2}' != '{3}')";
            case Code::kGenericUnknownTypeParamInConstraint: return "제약이 알 수 없는 제네릭 타입 파라미터 '{0}'를 참조합니다";
            case Code::kGenericDeclConstraintUnsatisfied: return "선언 제네릭 제약 '{0}: {1}'을(를) '{2}' 타입이 만족하지 않습니다";
            case Code::kGenericTypePathArityMismatch: return "제네릭 타입 경로 '{0}'의 인자 개수가 맞지 않습니다: 기대 {1}개, 실제 {2}개";
            case Code::kGenericTypePathTemplateNotFound: return "제네릭 타입 경로 대상 템플릿을 찾을 수 없습니다: '{0}'";
            case Code::kGenericTypePathTemplateKindMismatch: return "제네릭 타입 경로 '{0}'가 다른 종류의 템플릿을 가리킵니다: 기대 {1}, 실제 {2}";
            case Code::kGenericActsOverlap: return "owner '{0}'와 멤버 '{1}'에서 제네릭 acts 중복(coherence overlap)이 발생했습니다";
            case Code::kGenericActorDeclNotSupportedV1: return "v1에서는 제네릭 actor 선언을 지원하지 않습니다: '{0}'";
            case Code::kTemplateSidecarUnavailable: return "template-sidecar 물질화 경로를 사용할 수 없습니다: '{0}'";
            case Code::kTemplateSidecarSchema: return "template-sidecar payload가 올바르지 않습니다: '{0}'";
            case Code::kTemplateSidecarUnsupportedClosure: return "template-sidecar dependency closure가 지원되지 않습니다: '{0}'";
            case Code::kTemplateSidecarMissingNode: return "template-sidecar dependency node가 누락되었습니다: '{0}'";
            case Code::kTemplateSidecarConflictingNode: return "template-sidecar에 충돌하는 중복 node가 있습니다: '{0}'";
            case Code::kConstExprNotEvaluable: return "const 식을 평가할 수 없습니다: {0}";
            case Code::kConstExprCallNotSupported: return "v1 const 식에서는 함수 호출을 지원하지 않습니다";
            case Code::kConstExprCycle: return "const 선언 간 순환 참조가 감지되었습니다";
            case Code::kConstFnCallsNonConstFn: return "const 평가에서는 const def만 호출할 수 있습니다(호출 대상: {0})";
            case Code::kConstFnBodyUnsupportedStmt: return "const def 본문에 지원되지 않는 문이 있습니다: {0}";
            case Code::kConstLoopExprNotSupported: return "const 평가에서는 loop 식을 사용할 수 없습니다(while 문을 사용하세요)";
            case Code::kConstEvalCallDepthExceeded: return "const 평가 호출 깊이 제한을 초과했습니다(호출 대상: '{0}')";
            case Code::kConstEvalStepLimitExceeded: return "const 평가 단계 제한을 초과했습니다(예산={0})";
            case Code::kConstGlobalCompositeNotSupported: return "v1에서는 전역/static const의 복합 초기화식을 지원하지 않습니다: '{0}'";

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

    static void append_span_block_(
        std::ostringstream& out,
        const SourceManager& sm,
        Span sp,
        uint32_t context_lines,
        std::string_view header_prefix,
        std::string_view underline_message
    ) {
        auto lc = sm.line_col(sp.file_id, sp.lo);
        auto blk = sm.snippet_block_for_span(sp, context_lines);
        const uint32_t last_line_no = blk.first_line_no +
            (blk.lines.empty() ? 0u : static_cast<uint32_t>(blk.lines.size()) - 1);
        const uint32_t w = digits10(last_line_no == 0 ? 1u : last_line_no);

        out << header_prefix << sm.name(sp.file_id) << ":" << lc.line << ":" << lc.col << "\n";
        out << "  |\n";
        for (uint32_t i = 0; i < blk.lines.size(); ++i) {
            const uint32_t line_no = blk.first_line_no + i;
            out << "  ";
            {
                const std::string num = std::to_string(line_no);
                out << std::string(w - static_cast<uint32_t>(num.size()), ' ') << num;
            }
            out << " | " << blk.lines[i] << "\n";

            if (i == blk.caret_line_offset) {
                out << "  " << std::string(w, ' ') << " | ";
                out << std::string(blk.caret_cols_before, ' ');
                out << std::string(blk.caret_cols_len, '^');
                if (!underline_message.empty()) {
                    out << " " << underline_message;
                }
                out << "\n";
            }
        }
    }

    std::string render_one(const Diagnostic& d, Language lang, const SourceManager& sm) {
        std::string msg = render_message(d, lang);

        std::ostringstream oss;
        auto sev = d.severity();
        const char* sev_name =
            (sev == Severity::kWarning) ? "warning" :
            (sev == Severity::kFatal)   ? "fatal"   : "error";

        oss << sev_name << "[" << code_name_sv_(d.code()) << "]: " << msg << "\n";
        append_span_block_(oss, sm, d.span(), /*context_lines=*/0, " --> ", "");
        for (const auto& label : d.labels()) {
            append_span_block_(oss, sm, label.span, /*context_lines=*/0, " ::: ", label.message);
        }
        for (const auto& note : d.notes()) {
            oss << "  = note: " << note << "\n";
        }
        for (const auto& help : d.help()) {
            oss << "  = help: " << help << "\n";
        }
        return oss.str();
    }

    std::string render_one_context(const Diagnostic& d, Language lang, const SourceManager& sm, uint32_t context_lines) {
        std::string msg = (lang == Language::kKo) ? template_ko(d.code()) : template_en(d.code());
        msg = format_template(std::move(msg), d.args());

        std::ostringstream out;

        auto sev = d.severity();
        const char* sev_name =
            (sev == Severity::kWarning) ? "warning" :
            (sev == Severity::kFatal)   ? "fatal"   : "error";

        out << sev_name << "[" << code_name_sv_(d.code()) << "]: " << msg << "\n";
        append_span_block_(out, sm, d.span(), context_lines, " --> ", "");
        for (const auto& label : d.labels()) {
            append_span_block_(out, sm, label.span, context_lines, " ::: ", label.message);
        }
        for (const auto& note : d.notes()) {
            out << "  = note: " << note << "\n";
        }
        for (const auto& help : d.help()) {
            out << "  = help: " << help << "\n";
        }
        return out.str();
    }

} // namespace parus::diag
