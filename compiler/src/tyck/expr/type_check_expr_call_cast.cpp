// compiler/src/tyck/type_check_expr_call_cast.cpp
#include <parus/tyck/TypeCheck.hpp>
#include <parus/syntax/TokenKind.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/diag/DiagCode.hpp>
#include "../common/type_check_literals.hpp"

#include <sstream>
#include <unordered_map>


namespace parus::tyck {

    using K = parus::syntax::TokenKind;
    using detail::ParsedFloatLiteral;
    using detail::ParsedIntLiteral;
    using detail::parse_float_literal_;
    using detail::parse_int_literal_;

    ty::TypeId TypeChecker::check_expr_call_(const ast::Expr& e) {
        // e.a = callee, args slice in e.arg_begin/e.arg_count
        ty::TypeId callee_t = check_expr_(e.a);
        const auto& ct = types_.get(callee_t);

        auto check_all_arg_exprs_only = [&]() {
            for (uint32_t i = 0; i < e.arg_count; ++i) {
                const auto& a = ast_.args()[e.arg_begin + i];
                if (a.kind == ast::ArgKind::kNamedGroup) {
                    for (uint32_t k = 0; k < a.child_count; ++k) {
                        const auto& ca = ast_.named_group_args()[a.child_begin + k];
                        if (ca.expr != ast::k_invalid_expr) (void)check_expr_(ca.expr);
                    }
                } else {
                    if (a.expr != ast::k_invalid_expr) (void)check_expr_(a.expr);
                }
            }
        };

        if (ct.kind != ty::Kind::kFn) {
            diag_(diag::Code::kTypeNotCallable, e.span, types_.to_string(callee_t));
            err_(e.span, "call target is not a function");
            check_all_arg_exprs_only();
            return types_.error();
        }

        // ------------------------------------------------------------
        // 0) split args + call form classification
        // ------------------------------------------------------------
        std::vector<const ast::Arg*> outside_positional;
        std::vector<const ast::Arg*> outside_labeled;
        const ast::Arg* group_arg = nullptr;

        bool has_duplicate_group = false;
        Span duplicate_group_span{};

        for (uint32_t i = 0; i < e.arg_count; ++i) {
            const auto& a = ast_.args()[e.arg_begin + i];

            if (a.kind == ast::ArgKind::kNamedGroup) {
                if (group_arg != nullptr) {
                    has_duplicate_group = true;
                    if (duplicate_group_span.hi == 0) duplicate_group_span = a.span;
                } else {
                    group_arg = &a;
                }
                continue;
            }

            const bool is_labeled = a.has_label || (a.kind == ast::ArgKind::kLabeled);
            if (is_labeled) outside_labeled.push_back(&a);
            else outside_positional.push_back(&a);
        }

        enum class CallForm : uint8_t {
            kPositional,
            kLabeled,
            kPositionalPlusNamedGroup,
            kMixedInvalid,
        };

        CallForm form = CallForm::kPositional;
        if (group_arg != nullptr) {
            form = outside_labeled.empty() ? CallForm::kPositionalPlusNamedGroup
                                        : CallForm::kMixedInvalid;
        } else {
            if (!outside_labeled.empty() && !outside_positional.empty()) form = CallForm::kMixedInvalid;
            else if (!outside_labeled.empty()) form = CallForm::kLabeled;
            else form = CallForm::kPositional;
        }

        if (has_duplicate_group) {
            diag_(diag::Code::kCallOnlyOneNamedGroupAllowed, duplicate_group_span);
            err_(duplicate_group_span, "only one named-group '{ ... }' is allowed in a call");
            check_all_arg_exprs_only();
            return ct.ret;
        }

        if (form == CallForm::kMixedInvalid) {
            diag_(diag::Code::kCallArgMixNotAllowed, e.span);
            err_(e.span, "mixing labeled and positional arguments is not allowed");
            check_all_arg_exprs_only();
            return ct.ret;
        }

        // ------------------------------------------------------------
        // 1) find callee fn decl meta if possible (Ident-only in v0)
        // ------------------------------------------------------------
        const ast::Stmt* fn_decl = nullptr;
        ast::StmtId fn_decl_id = ast::k_invalid_stmt;

        {
            const ast::Expr& callee_expr = ast_.expr(e.a);
            if (callee_expr.kind == ast::ExprKind::kIdent) {
                auto it = fn_decl_by_name_.find(std::string(callee_expr.text));
                if (it != fn_decl_by_name_.end()) {
                    fn_decl_id = it->second;
                    fn_decl = &ast_.stmt(fn_decl_id);
                    if (fn_decl->kind != ast::StmtKind::kFnDecl) {
                        fn_decl = nullptr;
                        fn_decl_id = ast::k_invalid_stmt;
                    }
                }
            }
        }

        // ------------------------------------------------------------
        // 2) fallback: decl meta가 없으면 기존 signature 기반으로만 검사
        // ------------------------------------------------------------
        if (!fn_decl) {
            const uint32_t provided_non_group =
                static_cast<uint32_t>(outside_positional.size() + outside_labeled.size());

            if (provided_non_group != ct.param_count) {
                diag_(diag::Code::kTypeArgCountMismatch, e.span,
                    std::to_string(ct.param_count), std::to_string(provided_non_group));
                err_(e.span, "argument count mismatch");
            }

            uint32_t pi = 0;
            for (uint32_t i = 0; i < e.arg_count; ++i) {
                const auto& a = ast_.args()[e.arg_begin + i];

                if (a.kind == ast::ArgKind::kNamedGroup) {
                    for (uint32_t k = 0; k < a.child_count; ++k) {
                        const auto& ca = ast_.named_group_args()[a.child_begin + k];
                        if (ca.expr != ast::k_invalid_expr) (void)check_expr_(ca.expr);
                    }
                    continue;
                }

                ty::TypeId at = (a.expr != ast::k_invalid_expr) ? check_expr_(a.expr) : types_.error();

                if (pi < ct.param_count) {
                    ty::TypeId expected = types_.fn_param_at(callee_t, pi);

                    const auto& st = types_.get(at);
                    if (st.kind == ty::Kind::kBuiltin && st.builtin == ty::Builtin::kInferInteger) {
                        if (a.expr != ast::k_invalid_expr) {
                            (void)resolve_infer_int_in_context_(a.expr, expected);
                            at = check_expr_(a.expr);
                        }
                    }

                    if (!can_assign_(expected, at)) {
                        diag_(diag::Code::kTypeArgTypeMismatch, a.span,
                            std::to_string(pi), types_.to_string(expected), types_.to_string(at));
                        err_(a.span, "argument type mismatch");
                    }
                }

                ++pi;
            }

            if (group_arg) {
                std::string msg = "named-group arguments require a direct function declaration lookup in v0";
                diag_(diag::Code::kTypeErrorGeneric, group_arg->span, msg);
                err_(group_arg->span, msg);
            }

            return ct.ret;
        }

        // ------------------------------------------------------------
        // 3) build param metadata from declaration
        // ------------------------------------------------------------
        struct ParamInfo {
            uint32_t decl_index = 0;
            std::string_view name{};
            ty::TypeId type = ty::kInvalidType;
            bool has_default = false;
            bool is_named = false;
            Span span{};
        };

        std::vector<ParamInfo> positional_params;
        std::vector<ParamInfo> named_params;

        std::unordered_map<std::string, ParamInfo> all_params_by_name;
        std::unordered_map<std::string, ParamInfo> named_params_by_name;

        const uint32_t decl_total = fn_decl->param_count;
        uint32_t decl_positional = fn_decl->positional_param_count;
        if (decl_positional > decl_total) decl_positional = decl_total;

        positional_params.reserve(decl_positional);
        named_params.reserve(decl_total - decl_positional);
        all_params_by_name.reserve(decl_total);
        named_params_by_name.reserve(decl_total - decl_positional);

        for (uint32_t idx = 0; idx < decl_total; ++idx) {
            const auto& p = ast_.params()[fn_decl->param_begin + idx];

            const bool is_named = p.is_named_group || (idx >= decl_positional);

            ParamInfo info{};
            info.decl_index = idx;
            info.name = p.name;
            info.type = (p.type == ty::kInvalidType) ? types_.error() : p.type;
            info.has_default = is_named ? p.has_default : false; // positional default 정책 차단
            info.is_named = is_named;
            info.span = p.span;

            if (is_named) {
                named_params.push_back(info);
                named_params_by_name.emplace(std::string(info.name), info);
            } else {
                positional_params.push_back(info);
            }

            auto ins = all_params_by_name.emplace(std::string(info.name), info);
            if (!ins.second) {
                diag_(diag::Code::kDuplicateDecl, p.span, p.name);
                err_(p.span, "duplicate parameter label in function declaration: " + std::string(p.name));
            }
        }

        const bool decl_has_named_group = !named_params.empty();

        auto emit_count_too_many = [&](uint32_t expected_max, uint32_t got, Span sp, std::string_view ctx) {
            if (got <= expected_max) return;
            diag_(diag::Code::kTypeArgCountMismatch, sp,
                std::to_string(expected_max), std::to_string(got));
            err_(sp, std::string(ctx) + " argument count mismatch");
        };

        auto emit_missing_required = [&](const ParamInfo& p, bool named, Span report_span) {
            std::string msg = named
                ? ("missing required named argument '" + std::string(p.name) + "'")
                : ("missing required argument '" + std::string(p.name) + "'");
            diag_(diag::Code::kTypeErrorGeneric, report_span, msg);
            err_(report_span, msg);
        };

        auto check_arg_against_param = [&](const ast::Arg& a, const ParamInfo& p) {
            ty::TypeId at = (a.expr != ast::k_invalid_expr) ? check_expr_(a.expr) : types_.error();

            const auto& st = types_.get(at);
            if (st.kind == ty::Kind::kBuiltin && st.builtin == ty::Builtin::kInferInteger) {
                if (a.expr != ast::k_invalid_expr) {
                    (void)resolve_infer_int_in_context_(a.expr, p.type);
                    at = check_expr_(a.expr);
                }
            }

            if (!can_assign_(p.type, at)) {
                diag_(diag::Code::kTypeArgTypeMismatch, a.span,
                    std::to_string(p.decl_index), types_.to_string(p.type), types_.to_string(at));
                err_(a.span, "argument type mismatch for parameter '" + std::string(p.name) + "'");
            }
        };

        auto emit_named_group_requires_brace_diag = [&](Span sp, uint32_t pos_expected, uint32_t got_total) {
            uint32_t extras = (got_total > pos_expected) ? (got_total - pos_expected) : 0;
            std::string msg = "callee has named-group params; extra positional arguments (" +
                            std::to_string(extras) +
                            ") are not allowed. Pass them with '{ ... }' labels.";
            diag_(diag::Code::kTypeErrorGeneric, sp, msg);
            err_(sp, msg);
        };

        // ------------------------------------------------------------
        // 4) call-form specific matching
        // ------------------------------------------------------------
        switch (form) {
            case CallForm::kPositional: {
                // A) positional call: f(v1, v2, ...)
                // + compat: named-only 함수(fn({a,b}))에 대해 f(v1,v2) 허용
                if (!outside_labeled.empty()) {
                    diag_(diag::Code::kCallArgMixNotAllowed, e.span);
                    err_(e.span, "mixing labeled and positional arguments is not allowed");
                    check_all_arg_exprs_only();
                    return ct.ret;
                }

                const uint32_t got = static_cast<uint32_t>(outside_positional.size());

                if (!positional_params.empty()) {
                    const uint32_t pos_expected = static_cast<uint32_t>(positional_params.size());
                    const uint32_t bound = (got < pos_expected) ? got : pos_expected;

                    for (uint32_t i = 0; i < bound; ++i) {
                        check_arg_against_param(*outside_positional[i], positional_params[i]);
                    }

                    // 핵심 UX 수정:
                    // named-group이 있는 함수에서 positional이 초과되면
                    // 단순 "expected N, got M" 대신 정책 메시지를 낸다.
                    if (decl_has_named_group && got > pos_expected) {
                        for (uint32_t i = pos_expected; i < got; ++i) {
                            if (outside_positional[i]->expr != ast::k_invalid_expr) {
                                (void)check_expr_(outside_positional[i]->expr);
                            }
                        }
                        emit_named_group_requires_brace_diag(e.span, pos_expected, got);
                        return ct.ret;
                    }

                    // named-group 없는 일반 함수면 기존 count mismatch 유지
                    if (!decl_has_named_group) {
                        emit_count_too_many(pos_expected, got, e.span, "positional");
                    }

                    for (uint32_t i = bound; i < pos_expected; ++i) {
                        emit_missing_required(positional_params[i], /*named=*/false, e.span);
                    }

                    for (const auto& np : named_params) {
                        if (!np.has_default) emit_missing_required(np, /*named=*/true, e.span);
                    }
                } else if (!named_params.empty()) {
                    // named-only compat: declaration order로 positional 바인딩
                    emit_count_too_many(static_cast<uint32_t>(named_params.size()), got, e.span,
                                        "positional(named-only compat)");

                    const uint32_t bound =
                        (got < named_params.size()) ? got : static_cast<uint32_t>(named_params.size());

                    for (uint32_t i = 0; i < bound; ++i) {
                        check_arg_against_param(*outside_positional[i], named_params[i]);
                    }

                    for (uint32_t i = bound; i < named_params.size(); ++i) {
                        if (!named_params[i].has_default) emit_missing_required(named_params[i], /*named=*/true, e.span);
                    }
                } else {
                    emit_count_too_many(0, got, e.span, "positional");
                    for (const auto* pa : outside_positional) {
                        if (pa->expr != ast::k_invalid_expr) (void)check_expr_(pa->expr);
                    }
                }

                break;
            }

            case CallForm::kLabeled: {
                // B) labeled call: f(a:v1, b:v2, ...)
                std::unordered_map<std::string, const ast::Arg*> provided;
                provided.reserve(outside_labeled.size());

                for (const auto* la : outside_labeled) {
                    const std::string label(la->label);

                    auto ins = provided.emplace(label, la);
                    if (!ins.second) {
                        diag_(diag::Code::kDuplicateDecl, la->span, la->label);
                        err_(la->span, "duplicate argument label '" + label + "'");
                        if (la->expr != ast::k_invalid_expr) (void)check_expr_(la->expr);
                        continue;
                    }

                    auto it = all_params_by_name.find(label);
                    if (it == all_params_by_name.end()) {
                        std::string msg = "unknown argument label '" + label + "'";
                        diag_(diag::Code::kTypeErrorGeneric, la->span, msg);
                        err_(la->span, msg);
                        if (la->expr != ast::k_invalid_expr) (void)check_expr_(la->expr);
                        continue;
                    }

                    check_arg_against_param(*la, it->second);
                }

                for (const auto& pp : positional_params) {
                    if (provided.find(std::string(pp.name)) == provided.end()) {
                        emit_missing_required(pp, /*named=*/false, e.span);
                    }
                }

                for (const auto& np : named_params) {
                    if (!np.has_default && provided.find(std::string(np.name)) == provided.end()) {
                        emit_missing_required(np, /*named=*/true, e.span);
                    }
                }

                break;
            }

            case CallForm::kPositionalPlusNamedGroup: {
                // C) positional + named-group: f(pos..., {x:v, y:v})
                const uint32_t got_pos = static_cast<uint32_t>(outside_positional.size());
                emit_count_too_many(static_cast<uint32_t>(positional_params.size()), got_pos, e.span, "positional");

                const uint32_t bound =
                    (got_pos < positional_params.size()) ? got_pos : static_cast<uint32_t>(positional_params.size());

                for (uint32_t i = 0; i < bound; ++i) {
                    check_arg_against_param(*outside_positional[i], positional_params[i]);
                }

                for (uint32_t i = bound; i < positional_params.size(); ++i) {
                    emit_missing_required(positional_params[i], /*named=*/false, e.span);
                }

                if (!group_arg) break;

                const Span named_report_span = group_arg->span.hi ? group_arg->span : e.span;

                if (!decl_has_named_group) {
                    std::string msg = "callee does not declare a named-group parameter section";
                    diag_(diag::Code::kTypeErrorGeneric, named_report_span, msg);
                    err_(named_report_span, msg);

                    for (uint32_t k = 0; k < group_arg->child_count; ++k) {
                        const auto& ca = ast_.named_group_args()[group_arg->child_begin + k];
                        if (ca.expr != ast::k_invalid_expr) (void)check_expr_(ca.expr);
                    }
                    break;
                }

                std::unordered_map<std::string, const ast::Arg*> provided_named;
                provided_named.reserve(group_arg->child_count);

                for (uint32_t k = 0; k < group_arg->child_count; ++k) {
                    const auto& ca = ast_.named_group_args()[group_arg->child_begin + k];
                    const std::string label(ca.label);

                    auto ins = provided_named.emplace(label, &ca);
                    if (!ins.second) {
                        diag_(diag::Code::kDuplicateDecl, ca.span, ca.label);
                        err_(ca.span, "duplicate named argument label '" + label + "'");
                        if (ca.expr != ast::k_invalid_expr) (void)check_expr_(ca.expr);
                        continue;
                    }

                    auto it = named_params_by_name.find(label);
                    if (it == named_params_by_name.end()) {
                        std::string msg = "unknown named argument label '" + label + "'";
                        diag_(diag::Code::kTypeErrorGeneric, ca.span, msg);
                        err_(ca.span, msg);
                        if (ca.expr != ast::k_invalid_expr) (void)check_expr_(ca.expr);
                        continue;
                    }

                    check_arg_against_param(ca, it->second);
                }

                for (const auto& np : named_params) {
                    if (!np.has_default &&
                        provided_named.find(std::string(np.name)) == provided_named.end()) {
                        emit_missing_required(np, /*named=*/true, named_report_span);
                    }
                }

                break;
            }

            case CallForm::kMixedInvalid: {
                // 위에서 이미 return 처리됨. 방어용.
                check_all_arg_exprs_only();
                break;
            }
        }

        return ct.ret;
    }

    ty::TypeId TypeChecker::check_expr_cast_(const ast::Expr& e) {
        // AST contract:
        // - e.a: operand
        // - e.cast_type: target type
        // - e.cast_kind: as / as? / as!
        const ast::ExprId operand_eid = e.a;

        if (operand_eid == ast::k_invalid_expr) {
            diag_(diag::Code::kTyckCastMissingOperand, e.span);
            err_(e.span, "cast missing operand");
            return types_.error();
        }

        ty::TypeId operand_t = check_expr_(operand_eid, Slot::kValue);

        ty::TypeId target_t = e.cast_type;
        if (target_t == ty::kInvalidType) {
            diag_(diag::Code::kTyckCastMissingTargetType, e.span);
            err_(e.span, "cast missing target type");
            return types_.error();
        }

        // ------------------------------------------------------------
        // 7.6.3 semantics (Swift/C#-like):
        //
        //   expr as  T   -> T     (no optional auto-unwrapping)
        //   expr as? T   -> T?    (always optional-normalized, no T??)
        //   expr as! T   -> T     (runtime trap on failure)
        //
        // Optional rules:
        // - `as`  : optional 값을 자동 해소하지 않는다.
        // - `as?` : 입력이 null이면 결과 null, 성공하면 T 값을 T?로 반환
        // - `as!` : 입력이 null이거나 변환 실패 시 trap(런타임)
        //
        // v0 scope:
        // - numeric scalar casts only (int<->int, int<->float, float<->float)
        // - future: runtime-checked downcast / ref casts in v1+
        // ------------------------------------------------------------

        auto make_optional_if_needed = [&](ty::TypeId t) -> ty::TypeId {
            if (t == ty::kInvalidType) return types_.error();
            if (is_optional_(t)) return t;
            return types_.make_optional(t);
        };

        // (A) 결과 타입 계산: as?만 항상 optional-normalize
        ty::TypeId result_t = target_t;
        if (e.cast_kind == ast::CastKind::kAsOptional) {
            result_t = make_optional_if_needed(target_t);
        }

        // (B) operand가 null인 경우
        const bool operand_is_null = is_null_(operand_t);
        if (operand_is_null) {
            // null as? T  -> null (T?)
            if (e.cast_kind == ast::CastKind::kAsOptional) {
                return result_t; // T?
            }

            // null as! T  -> runtime trap, but type is T
            if (e.cast_kind == ast::CastKind::kAsForce) {
                return result_t; // T
            }

            // null as T:
            // - only allowed when T is optional (null -> T?)
            // - otherwise error (no implicit unwrap / no null-to-nonopt)
            if (is_optional_(result_t)) {
                return result_t;
            }

            diag_(diag::Code::kTyckCastNullToNonOptional, e.span, types_.to_string(result_t));
            err_(e.span, "cannot cast null to non-optional type");
            return types_.error();
        }

        // (C) operand가 optional(U?)인 경우:
        // - as   : optional 자동 해소 없음 -> U? 를 그대로 검사해야 함 (대부분 비허용이 정상)
        // - as?  : null-safe/실패가능 -> 검사 단계에서는 U로 unwrap해서 "값이 있을 때 변환 가능?"만 본다
        // - as!  : 강제 -> 검사 단계에서는 U로 unwrap해서 변환 가능성만 보고, null은 런타임 trap로 처리
        const bool operand_is_opt = is_optional_(operand_t);

        ty::TypeId check_operand_t = operand_t;
        if ((e.cast_kind == ast::CastKind::kAsOptional || e.cast_kind == ast::CastKind::kAsForce) && operand_is_opt) {
            ty::TypeId elem = optional_elem_(operand_t);
            check_operand_t = (elem == ty::kInvalidType) ? types_.error() : elem;
        }

        // (D) 변환 가능성 체크는 "target_t" 기준으로 한다.
        //     - as?의 결과가 T?인 것과 별개로, 변환 자체는 T로 되는지 확인해야 한다.
        ty::TypeId check_target_t = target_t;

        // ------------------------------------------------------------
        // helper: builtin predicates
        // ------------------------------------------------------------
        auto is_builtin = [&](ty::TypeId t) -> bool {
            return t != ty::kInvalidType && types_.get(t).kind == ty::Kind::kBuiltin;
        };

        auto is_numeric_builtin = [&](ty::TypeId t) -> bool {
            if (!is_builtin(t)) return false;
            auto b = types_.get(t).builtin;
            switch (b) {
                case ty::Builtin::kI8: case ty::Builtin::kI16: case ty::Builtin::kI32:
                case ty::Builtin::kI64: case ty::Builtin::kI128:
                case ty::Builtin::kU8: case ty::Builtin::kU16: case ty::Builtin::kU32:
                case ty::Builtin::kU64: case ty::Builtin::kU128:
                case ty::Builtin::kISize: case ty::Builtin::kUSize:
                case ty::Builtin::kF32: case ty::Builtin::kF64: case ty::Builtin::kF128:
                    return true;
                default:
                    return false;
            }
        };

        // ------------------------------------------------------------
        // (E) "{integer}" placeholder 처리:
        // - 명시적 cast는 강력한 "컨텍스트"이므로,
        //   정수 타겟으로는 여기서 resolve 시도 가능.
        // - float 타겟으로 cast하는 경우는 resolve_infer_int_in_context_를 호출하면
        //   (네 정책상) IntToFloatNotAllowed 진단이 날 수 있으므로 호출하지 않는다.
        //   (float로 가고 싶다면 literal에 f32/f64 suffix를 붙이거나,
        //    향후 별도 정책을 도입하면 된다.)
        // ------------------------------------------------------------
        auto try_resolve_infer_int_by_cast_target = [&]() {
            const auto& st = types_.get(check_operand_t);
            if (!(st.kind == ty::Kind::kBuiltin && st.builtin == ty::Builtin::kInferInteger)) return;

            const auto& dt = types_.get(check_target_t);
            if (dt.kind != ty::Kind::kBuiltin) return;

            auto is_int_builtin = [&](ty::Builtin b) -> bool {
                return b == ty::Builtin::kI8 || b == ty::Builtin::kI16 || b == ty::Builtin::kI32 ||
                    b == ty::Builtin::kI64 || b == ty::Builtin::kI128 ||
                    b == ty::Builtin::kU8 || b == ty::Builtin::kU16 || b == ty::Builtin::kU32 ||
                    b == ty::Builtin::kU64 || b == ty::Builtin::kU128 ||
                    b == ty::Builtin::kISize || b == ty::Builtin::kUSize;
            };

            auto is_float_builtin = [&](ty::Builtin b) -> bool {
                return b == ty::Builtin::kF32 || b == ty::Builtin::kF64 || b == ty::Builtin::kF128;
            };

            if (is_int_builtin(dt.builtin)) {
                (void)resolve_infer_int_in_context_(operand_eid, check_target_t);
                // NOTE: expr cache가 이미 {integer}로 박혀있을 수 있으니
                // check_operand_t는 논리적으로만 업데이트한다고 가정(필요하면 재-read).
            } else if (is_float_builtin(dt.builtin)) {
                // explicit cast to float is allowed conceptually,
                // but we do NOT resolve infer-int here to avoid "implicit int->float" diag.
                // (policy v0; can be revisited later)
            }
        };

        // ------------------------------------------------------------
        // (F) 허용 규칙(v0):
        // 1) 같은 타입 (T -> T)
        // 2) builtin numeric <-> builtin numeric (explicit cast only)
        // 3) (향후) 다운캐스트/런타임 타입 검사: v1+
        //
        // IMPORTANT:
        // - `as`는 optional 자동 해소가 없으므로,
        //   U? as T 같은 것은 일반적으로 허용되지 않는다.
        // - `as?`/`as!`는 검사 시 unwrap(U? -> U) 후 변환 가능성만 체크한다.
        // ------------------------------------------------------------

        // 1) identical
        if (check_operand_t == check_target_t) {
            return result_t;
        }

        // 2) resolve "{integer}" with cast target when possible
        try_resolve_infer_int_by_cast_target();

        // 3) numeric explicit casts
        if (is_numeric_builtin(check_target_t) && is_numeric_builtin(check_operand_t)) {
            return result_t;
        }

        // Otherwise: not allowed (future: runtime checked downcast)
        diag_(diag::Code::kTyckCastNotAllowed, e.span,
            types_.to_string(operand_t), types_.to_string(result_t));
        err_(e.span, "cast not allowed");
        return types_.error();
    }

} // namespace parus::tyck
