// frontend/src/tyck/type_check_expr_call_cast.cpp
#include <parus/tyck/TypeCheck.hpp>
#include <parus/syntax/TokenKind.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/diag/DiagCode.hpp>
#include "../common/type_check_literals.hpp"

#include <sstream>
#include <unordered_map>
#include <unordered_set>
#include <algorithm>


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
        std::string callee_name;
        bool callee_is_fn_symbol = false;
        std::vector<ast::StmtId> overload_decl_ids;

        {
            const ast::Expr& callee_expr = ast_.expr(e.a);
            if (callee_expr.kind == ast::ExprKind::kIdent) {
                callee_name = std::string(callee_expr.text);
                if (auto sid = sym_.lookup(callee_expr.text)) {
                    const auto& sym = sym_.symbol(*sid);
                    callee_is_fn_symbol = (sym.kind == sema::SymbolKind::kFn);
                }

                if (callee_is_fn_symbol) {
                    auto it = fn_decl_by_name_.find(callee_name);
                    if (it != fn_decl_by_name_.end()) {
                        overload_decl_ids = it->second;
                    }
                }
            }
        }

        // 라벨 중복은 오버로드 해소 이전에 무조건 진단한다.
        auto has_duplicate_labels = [&](const std::vector<const ast::Arg*>& args, Span& out_span, std::string& out_label) -> bool {
            std::unordered_set<std::string> seen;
            seen.reserve(args.size());
            for (const auto* a : args) {
                const std::string label(a->label);
                if (!seen.insert(label).second) {
                    out_span = a->span;
                    out_label = label;
                    return true;
                }
            }
            return false;
        };

        std::vector<const ast::Arg*> group_entries;
        if (group_arg != nullptr) {
            group_entries.reserve(group_arg->child_count);
            for (uint32_t k = 0; k < group_arg->child_count; ++k) {
                const auto& ca = ast_.named_group_args()[group_arg->child_begin + k];
                group_entries.push_back(&ca);
            }
        }

        {
            Span dup_sp{};
            std::string dup_label;
            if (has_duplicate_labels(outside_labeled, dup_sp, dup_label)) {
                diag_(diag::Code::kDuplicateDecl, dup_sp, dup_label);
                err_(dup_sp, "duplicate argument label '" + dup_label + "'");
                check_all_arg_exprs_only();
                return ct.ret;
            }
            if (has_duplicate_labels(group_entries, dup_sp, dup_label)) {
                diag_(diag::Code::kDuplicateDecl, dup_sp, dup_label);
                err_(dup_sp, "duplicate named argument label '" + dup_label + "'");
                check_all_arg_exprs_only();
                return ct.ret;
            }
        }

        // ------------------------------------------------------------
        // 2) fallback: overload 집합을 못 찾으면 기존 signature 기반으로만 검사
        // ------------------------------------------------------------
        if (!callee_is_fn_symbol || overload_decl_ids.empty()) {
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
        // 3) overload 후보 구성
        // ------------------------------------------------------------
        struct ParamInfo {
            uint32_t decl_index = 0;
            std::string name{};
            ty::TypeId type = ty::kInvalidType;
            bool has_default = false;
            Span span{};
        };
        struct Candidate {
            ast::StmtId decl_id = ast::k_invalid_stmt;
            ty::TypeId ret = ty::kInvalidType;
            std::vector<ParamInfo> positional;
            std::vector<ParamInfo> named;
            std::unordered_map<std::string, size_t> positional_by_label;
            std::unordered_map<std::string, size_t> named_by_label;
        };

        std::vector<Candidate> candidates;
        candidates.reserve(overload_decl_ids.size());

        for (const ast::StmtId sid : overload_decl_ids) {
            const ast::Stmt& fn = ast_.stmt(sid);
            if (fn.kind != ast::StmtKind::kFnDecl) continue;

            Candidate c{};
            c.decl_id = sid;
            c.ret = (fn.fn_ret != ty::kInvalidType)
                ? fn.fn_ret
                : ((fn.type != ty::kInvalidType && types_.get(fn.type).kind == ty::Kind::kFn)
                    ? types_.get(fn.type).ret
                    : ct.ret);

            uint32_t pos_cnt = fn.positional_param_count;
            if (pos_cnt > fn.param_count) pos_cnt = fn.param_count;

            c.positional.reserve(pos_cnt);
            c.named.reserve(fn.param_count - pos_cnt);

            for (uint32_t i = 0; i < fn.param_count; ++i) {
                const auto& p = ast_.params()[fn.param_begin + i];
                ParamInfo info{};
                info.decl_index = i;
                info.name = std::string(p.name);
                info.type = (p.type == ty::kInvalidType) ? types_.error() : p.type;
                info.has_default = p.has_default;
                info.span = p.span;

                const bool is_named = (i >= pos_cnt) || p.is_named_group;
                if (is_named) {
                    c.named_by_label.emplace(info.name, c.named.size());
                    c.named.push_back(std::move(info));
                } else {
                    c.positional_by_label.emplace(info.name, c.positional.size());
                    c.positional.push_back(std::move(info));
                }
            }

            candidates.push_back(std::move(c));
        }

        if (candidates.empty()) {
            std::string msg = "no callable declaration candidate for '" + callee_name + "'";
            diag_(diag::Code::kTypeErrorGeneric, e.span, msg);
            err_(e.span, msg);
            check_all_arg_exprs_only();
            return ct.ret;
        }

        std::unordered_map<std::string, const ast::Arg*> labeled_by_label;
        labeled_by_label.reserve(outside_labeled.size());
        for (const auto* a : outside_labeled) {
            labeled_by_label.emplace(std::string(a->label), a);
        }

        std::unordered_map<std::string, const ast::Arg*> group_by_label;
        group_by_label.reserve(group_entries.size());
        for (const auto* a : group_entries) {
            group_by_label.emplace(std::string(a->label), a);
        }

        const auto arg_type_now = [&](const ast::Arg* a) -> ty::TypeId {
            if (a == nullptr || a->expr == ast::k_invalid_expr) return types_.error();
            return check_expr_(a->expr);
        };

        const auto arg_assignable_now = [&](const ast::Arg* a, ty::TypeId expected) -> bool {
            const ty::TypeId at = arg_type_now(a);
            return can_assign_(expected, at);
        };

        std::vector<size_t> filtered;
        filtered.reserve(candidates.size());

        for (size_t i = 0; i < candidates.size(); ++i) {
            const auto& c = candidates[i];

            if (form == CallForm::kPositional) {
                if (!c.named.empty()) continue; // spec 6.1.7(D)-1
                filtered.push_back(i);
                continue;
            }

            if (form == CallForm::kLabeled) {
                if (!c.named.empty()) continue; // spec 6.1.7(D)-2
                if (outside_labeled.size() != c.positional.size()) continue;

                bool label_set_ok = true;
                for (const auto& pp : c.positional) {
                    if (labeled_by_label.find(pp.name) == labeled_by_label.end()) {
                        label_set_ok = false;
                        break;
                    }
                }
                if (!label_set_ok) continue;
                filtered.push_back(i);
                continue;
            }

            if (form == CallForm::kPositionalPlusNamedGroup) {
                if (c.named.empty()) continue; // spec 6.1.7(D)-3
                filtered.push_back(i);
                continue;
            }
        }

        if (filtered.empty()) {
            std::string msg = "no overload candidate matches call form for '" + callee_name + "'";
            diag_(diag::Code::kTypeErrorGeneric, e.span, msg);
            err_(e.span, msg);
            check_all_arg_exprs_only();
            return ct.ret;
        }

        /// @brief spec 6.1.7(E)의 단계 A/B를 구현하는 후보 매칭 함수.
        const auto match_candidate = [&](const Candidate& c, bool allow_defaults) -> bool {
            if (form == CallForm::kPositional) {
                if (outside_positional.size() != c.positional.size()) return false;
                for (size_t i = 0; i < outside_positional.size(); ++i) {
                    if (!arg_assignable_now(outside_positional[i], c.positional[i].type)) return false;
                }
                return true;
            }

            if (form == CallForm::kLabeled) {
                for (const auto& p : c.positional) {
                    auto it = labeled_by_label.find(p.name);
                    if (it == labeled_by_label.end()) {
                        if (!allow_defaults || !p.has_default) return false;
                        continue;
                    }
                    if (!arg_assignable_now(it->second, p.type)) return false;
                }
                return true;
            }

            if (form == CallForm::kPositionalPlusNamedGroup) {
                if (outside_positional.size() != c.positional.size()) return false;
                for (size_t i = 0; i < outside_positional.size(); ++i) {
                    if (!arg_assignable_now(outside_positional[i], c.positional[i].type)) return false;
                }

                for (const auto& pair : group_by_label) {
                    if (c.named_by_label.find(pair.first) == c.named_by_label.end()) return false;
                }

                for (const auto& p : c.named) {
                    auto it = group_by_label.find(p.name);
                    if (it == group_by_label.end()) {
                        if (!allow_defaults || !p.has_default) return false;
                        continue;
                    }
                    if (!arg_assignable_now(it->second, p.type)) return false;
                }
                return true;
            }

            return false;
        };

        std::vector<size_t> stage_a_matches;
        for (size_t idx : filtered) {
            if (match_candidate(candidates[idx], /*allow_defaults=*/false)) {
                stage_a_matches.push_back(idx);
            }
        }

        std::vector<size_t> final_matches;
        if (!stage_a_matches.empty()) {
            final_matches = std::move(stage_a_matches);
        } else {
            for (size_t idx : filtered) {
                if (match_candidate(candidates[idx], /*allow_defaults=*/true)) {
                    final_matches.push_back(idx);
                }
            }
        }

        if (final_matches.empty()) {
            std::string msg = "no matching overload found for call '" + callee_name + "'";
            diag_(diag::Code::kTypeErrorGeneric, e.span, msg);
            err_(e.span, msg);
            check_all_arg_exprs_only();
            return ct.ret;
        }

        if (final_matches.size() > 1) {
            std::string msg = "ambiguous overloaded call '" + callee_name + "'";
            diag_(diag::Code::kTypeErrorGeneric, e.span, msg);
            err_(e.span, msg);
            check_all_arg_exprs_only();
            return ct.ret;
        }

        const Candidate& selected = candidates[final_matches.front()];

        /// @brief 선택된 후보에 대해 infer-int 해소를 포함한 최종 타입 검증을 수행한다.
        const auto check_arg_against_param_final = [&](const ast::Arg& a, const ParamInfo& p) {
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
                err_(a.span, "argument type mismatch for parameter '" + p.name + "'");
            }
        };

        if (form == CallForm::kPositional) {
            for (size_t i = 0; i < outside_positional.size() && i < selected.positional.size(); ++i) {
                check_arg_against_param_final(*outside_positional[i], selected.positional[i]);
            }
        } else if (form == CallForm::kLabeled) {
            for (const auto& p : selected.positional) {
                auto it = labeled_by_label.find(p.name);
                if (it != labeled_by_label.end()) {
                    check_arg_against_param_final(*it->second, p);
                }
            }
        } else if (form == CallForm::kPositionalPlusNamedGroup) {
            for (size_t i = 0; i < outside_positional.size() && i < selected.positional.size(); ++i) {
                check_arg_against_param_final(*outside_positional[i], selected.positional[i]);
            }
            for (const auto& p : selected.named) {
                auto it = group_by_label.find(p.name);
                if (it != group_by_label.end()) {
                    check_arg_against_param_final(*it->second, p);
                }
            }
        }

        return selected.ret;
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
