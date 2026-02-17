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
        if (current_expr_id_ != ast::k_invalid_expr &&
            current_expr_id_ < expr_overload_target_cache_.size()) {
            expr_overload_target_cache_[current_expr_id_] = ast::k_invalid_stmt;
        }

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

        ty::TypeId fallback_ret = types_.error();
        ty::TypeId callee_t = ty::kInvalidType;
        uint32_t callee_param_count = 0;

        std::string callee_name;
        bool is_dot_method_call = false;
        bool is_explicit_acts_path_call = false;
        std::vector<ast::StmtId> overload_decl_ids;

        struct ExplicitActsPath {
            std::string owner_path;
            std::string set_name;
            std::string member_name;
        };

        auto parse_explicit_acts_path = [](std::string_view text, ExplicitActsPath& out) -> bool {
            constexpr std::string_view marker = "::acts(";
            const size_t marker_pos = text.find(marker);
            if (marker_pos == std::string_view::npos) return false;

            const size_t close_pos = text.find(')', marker_pos + marker.size());
            if (close_pos == std::string_view::npos) return false;
            if (close_pos + 2 >= text.size()) return false;
            if (text[close_pos + 1] != ':' || text[close_pos + 2] != ':') return false;

            const std::string_view owner = text.substr(0, marker_pos);
            const std::string_view set = text.substr(marker_pos + marker.size(),
                                                     close_pos - (marker_pos + marker.size()));
            const std::string_view member = text.substr(close_pos + 3);

            if (owner.empty() || set.empty() || member.empty()) return false;
            if (member.find("::") != std::string_view::npos) return false;

            out.owner_path.assign(owner.data(), owner.size());
            out.set_name.assign(set.data(), set.size());
            out.member_name.assign(member.data(), member.size());
            return true;
        };

        // ------------------------------------------------------------
        // 1) method call fast-path: `value.ident(...)`
        // ------------------------------------------------------------
        {
            const ast::Expr& callee_expr = ast_.expr(e.a);
            if (callee_expr.kind == ast::ExprKind::kBinary &&
                callee_expr.op == K::kDot &&
                callee_expr.a != ast::k_invalid_expr &&
                callee_expr.b != ast::k_invalid_expr) {
                const ast::Expr& rhs = ast_.expr(callee_expr.b);
                if (rhs.kind == ast::ExprKind::kIdent) {
                    ty::TypeId owner_t = check_expr_(callee_expr.a);
                    if (owner_t != ty::kInvalidType) {
                        const auto& ot = types_.get(owner_t);
                        if (ot.kind == ty::Kind::kBorrow) {
                            owner_t = ot.elem;
                        }
                    }
                    const auto selected_methods = lookup_acts_methods_for_call_(owner_t, rhs.text);
                    bool any_method_named = false;
                    if (owner_t != ty::kInvalidType) {
                        auto oit = acts_default_method_map_.find(owner_t);
                        if (oit != acts_default_method_map_.end()) {
                            auto mit = oit->second.find(std::string(rhs.text));
                            any_method_named = (mit != oit->second.end() && !mit->second.empty());
                        }
                    }

                    bool has_self_receiver_candidate = false;
                    for (const auto& md : selected_methods) {
                        if (md.fn_sid == ast::k_invalid_stmt) continue;
                        if (!md.receiver_is_self) continue;
                        has_self_receiver_candidate = true;
                        overload_decl_ids.push_back(md.fn_sid);
                    }

                    if (has_self_receiver_candidate) {
                        is_dot_method_call = true;
                        callee_name = types_.to_string(owner_t) + "." + std::string(rhs.text);
                    } else if (any_method_named && !selected_methods.empty()) {
                        std::string msg =
                            "dot method call is only allowed for acts members with a self receiver (use path call for non-self acts functions)";
                        diag_(diag::Code::kTypeErrorGeneric, rhs.span, msg);
                        err_(rhs.span, msg);
                        check_all_arg_exprs_only();
                        return types_.error();
                    } else if (any_method_named && selected_methods.empty()) {
                        std::ostringstream oss;
                        oss << "no active acts method '" << rhs.text
                            << "' for type " << types_.to_string(owner_t)
                            << " (select with 'use " << types_.to_string(owner_t)
                            << " with acts(Name);' or use default)";
                        diag_(diag::Code::kTypeErrorGeneric, rhs.span, oss.str());
                        err_(rhs.span, oss.str());
                        check_all_arg_exprs_only();
                        return types_.error();
                    }
                }
            }
        }

        // ------------------------------------------------------------
        // 2) non-method path: regular callee typing + ident overload lookup
        // ------------------------------------------------------------
        if (!is_dot_method_call) {
            const ast::Expr& callee_expr = ast_.expr(e.a);
            if (callee_expr.kind == ast::ExprKind::kIdent) {
                ExplicitActsPath explicit_acts{};
                if (parse_explicit_acts_path(callee_expr.text, explicit_acts)) {
                    is_explicit_acts_path_call = true;
                    callee_name = std::string(callee_expr.text);

                    std::string owner_lookup = explicit_acts.owner_path;
                    if (auto rewritten = rewrite_imported_path_(owner_lookup)) {
                        owner_lookup = *rewritten;
                    }

                    const auto owner_sid = lookup_symbol_(owner_lookup);
                    if (!owner_sid.has_value()) {
                        std::ostringstream oss;
                        oss << "unknown acts owner type path '" << explicit_acts.owner_path << "'";
                        diag_(diag::Code::kTypeErrorGeneric, callee_expr.span, oss.str());
                        err_(callee_expr.span, oss.str());
                        check_all_arg_exprs_only();
                        return types_.error();
                    }

                    const auto& owner_sym = sym_.symbol(*owner_sid);
                    if (owner_sym.kind != sema::SymbolKind::kField) {
                        std::ostringstream oss;
                        oss << "acts path owner must be a field/tablet type in v0, got '"
                            << owner_sym.name << "'";
                        diag_(diag::Code::kTypeErrorGeneric, callee_expr.span, oss.str());
                        err_(callee_expr.span, oss.str());
                        check_all_arg_exprs_only();
                        return types_.error();
                    }

                    const ty::TypeId owner_t = owner_sym.declared_type;
                    auto owner_methods_it = acts_default_method_map_.find(owner_t);
                    if (owner_methods_it == acts_default_method_map_.end()) {
                        std::ostringstream oss;
                        oss << "no acts methods are declared for type " << types_.to_string(owner_t);
                        diag_(diag::Code::kTypeErrorGeneric, callee_expr.span, oss.str());
                        err_(callee_expr.span, oss.str());
                        check_all_arg_exprs_only();
                        return types_.error();
                    }

                    auto member_it = owner_methods_it->second.find(explicit_acts.member_name);
                    if (member_it == owner_methods_it->second.end()) {
                        std::ostringstream oss;
                        oss << "acts member '" << explicit_acts.member_name
                            << "' is not declared for type " << types_.to_string(owner_t);
                        diag_(diag::Code::kTypeErrorGeneric, callee_expr.span, oss.str());
                        err_(callee_expr.span, oss.str());
                        check_all_arg_exprs_only();
                        return types_.error();
                    }

                    if (explicit_acts.set_name == "default") {
                        for (const auto& md : member_it->second) {
                            if (!md.from_named_set) overload_decl_ids.push_back(md.fn_sid);
                        }
                    } else {
                        const auto named_sid = resolve_named_acts_decl_sid_(owner_t, explicit_acts.set_name);
                        if (!named_sid.has_value()) {
                            std::ostringstream oss;
                            oss << "unknown acts set '" << explicit_acts.set_name
                                << "' for type " << types_.to_string(owner_t);
                            diag_(diag::Code::kTypeErrorGeneric, callee_expr.span, oss.str());
                            err_(callee_expr.span, oss.str());
                            check_all_arg_exprs_only();
                            return types_.error();
                        }

                        for (const auto& md : member_it->second) {
                            if (md.from_named_set && md.acts_decl_sid == *named_sid) {
                                overload_decl_ids.push_back(md.fn_sid);
                            }
                        }
                    }

                    if (overload_decl_ids.empty()) {
                        std::ostringstream oss;
                        oss << "acts member '" << explicit_acts.member_name
                            << "' is not available in acts(" << explicit_acts.set_name
                            << ") for type " << types_.to_string(owner_t);
                        diag_(diag::Code::kTypeErrorGeneric, callee_expr.span, oss.str());
                        err_(callee_expr.span, oss.str());
                        check_all_arg_exprs_only();
                        return types_.error();
                    }
                }
            }

            if (!is_explicit_acts_path_call) {
                callee_t = check_expr_(e.a);
                const auto& ct = types_.get(callee_t);
                if (ct.kind != ty::Kind::kFn) {
                    diag_(diag::Code::kTypeNotCallable, e.span, types_.to_string(callee_t));
                    err_(e.span, "call target is not a function");
                    check_all_arg_exprs_only();
                    return types_.error();
                }

                fallback_ret = ct.ret;
                callee_param_count = ct.param_count;
            }

            if (!is_explicit_acts_path_call && callee_expr.kind == ast::ExprKind::kIdent) {
                std::string lookup_name = std::string(callee_expr.text);
                if (auto rewritten = rewrite_imported_path_(lookup_name)) {
                    lookup_name = *rewritten;
                }

                callee_name = lookup_name;
                if (auto sid = lookup_symbol_(lookup_name)) {
                    const auto& sym = sym_.symbol(*sid);
                    if (sym.kind == sema::SymbolKind::kFn) {
                        callee_name = sym.name;
                        auto it = fn_decl_by_name_.find(callee_name);
                        if (it != fn_decl_by_name_.end()) {
                            overload_decl_ids = it->second;
                        }
                    }
                }
            }
        }

        if (fallback_ret == ty::kInvalidType) fallback_ret = types_.error();
        if (callee_name.empty()) callee_name = "<callee>";

        if (has_duplicate_group) {
            diag_(diag::Code::kCallOnlyOneNamedGroupAllowed, duplicate_group_span);
            err_(duplicate_group_span, "only one named-group '{ ... }' is allowed in a call");
            check_all_arg_exprs_only();
            return fallback_ret;
        }

        if (form == CallForm::kMixedInvalid) {
            diag_(diag::Code::kCallArgMixNotAllowed, e.span);
            err_(e.span, "mixing labeled and positional arguments is not allowed");
            check_all_arg_exprs_only();
            return fallback_ret;
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
                return fallback_ret;
            }
            if (has_duplicate_labels(group_entries, dup_sp, dup_label)) {
                diag_(diag::Code::kDuplicateDecl, dup_sp, dup_label);
                err_(dup_sp, "duplicate named argument label '" + dup_label + "'");
                check_all_arg_exprs_only();
                return fallback_ret;
            }
        }

        // ------------------------------------------------------------
        // 3) fallback: overload 집합을 못 찾으면 signature 기반 검사
        // ------------------------------------------------------------
        if (overload_decl_ids.empty()) {
            const uint32_t provided_non_group =
                static_cast<uint32_t>(outside_positional.size() + outside_labeled.size());

            if (provided_non_group != callee_param_count) {
                diag_(diag::Code::kTypeArgCountMismatch, e.span,
                    std::to_string(callee_param_count), std::to_string(provided_non_group));
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

                if (pi < callee_param_count) {
                    ty::TypeId expected = types_.fn_param_at(callee_t, pi);
                    if (a.expr != ast::k_invalid_expr) {
                        const CoercionPlan plan = classify_assign_with_coercion_(
                            AssignSite::CallArg, expected, a.expr, a.span);
                        at = plan.src_after;
                        if (!plan.ok) {
                            diag_(diag::Code::kTypeArgTypeMismatch, a.span,
                                std::to_string(pi), types_.to_string(expected), type_for_user_diag_(at, a.expr));
                            err_(a.span, "argument type mismatch");
                        }
                    } else {
                        diag_(diag::Code::kTypeArgTypeMismatch, a.span,
                            std::to_string(pi), types_.to_string(expected), "<missing>");
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

            return fallback_ret;
        }

        // ------------------------------------------------------------
        // 4) overload 후보 구성
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
            bool inject_receiver = false;
            std::vector<ParamInfo> positional;
            std::vector<ParamInfo> named;
            std::unordered_map<std::string, size_t> positional_by_label;
            std::unordered_map<std::string, size_t> named_by_label;
        };

        std::vector<Candidate> candidates;
        candidates.reserve(overload_decl_ids.size());

        for (const ast::StmtId sid : overload_decl_ids) {
            const ast::Stmt& def = ast_.stmt(sid);
            if (def.kind != ast::StmtKind::kFnDecl) continue;

            Candidate c{};
            c.decl_id = sid;
            c.ret = (def.fn_ret != ty::kInvalidType)
                ? def.fn_ret
                : ((def.type != ty::kInvalidType && types_.get(def.type).kind == ty::Kind::kFn)
                    ? types_.get(def.type).ret
                    : fallback_ret);

            uint32_t pos_cnt = def.positional_param_count;
            if (pos_cnt > def.param_count) pos_cnt = def.param_count;

            uint32_t start_idx = 0;
            if (is_dot_method_call && def.param_count > 0) {
                const auto& p0 = ast_.params()[def.param_begin + 0];
                if (p0.is_self) {
                    c.inject_receiver = true;
                    start_idx = 1;
                }
            }
            if (start_idx > pos_cnt) pos_cnt = start_idx;

            const uint32_t eff_pos_cnt = (pos_cnt > start_idx) ? (pos_cnt - start_idx) : 0u;
            const uint32_t eff_named_cnt = (def.param_count > pos_cnt) ? (def.param_count - pos_cnt) : 0u;
            c.positional.reserve(eff_pos_cnt);
            c.named.reserve(eff_named_cnt);

            for (uint32_t i = start_idx; i < def.param_count; ++i) {
                const auto& p = ast_.params()[def.param_begin + i];
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
            diag_(diag::Code::kOverloadNoMatchingCall, e.span, callee_name, "no declaration candidates");
            err_(e.span, msg);
            check_all_arg_exprs_only();
            return fallback_ret;
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
            if (can_assign_(expected, at)) return true;
            if (is_optional_(expected)) {
                const ty::TypeId elem = optional_elem_(expected);
                if (elem != ty::kInvalidType && can_assign_(elem, at)) return true;
            }
            return false;
        };

        /// @brief 호출 지점의 형태/타입 요약 문자열을 생성한다.
        const auto make_callsite_summary = [&]() -> std::string {
            std::ostringstream oss;
            if (form == CallForm::kPositional) oss << "positional(";
            else if (form == CallForm::kLabeled) oss << "labeled(";
            else if (form == CallForm::kPositionalPlusNamedGroup) oss << "positional+named-group(";
            else oss << "mixed-invalid(";

            bool first = true;
            for (const auto* a : outside_positional) {
                if (!first) oss << ", ";
                first = false;
                oss << types_.to_string(arg_type_now(a));
            }
            for (const auto* a : outside_labeled) {
                if (!first) oss << ", ";
                first = false;
                oss << std::string(a->label) << ":" << types_.to_string(arg_type_now(a));
            }
            for (const auto* a : group_entries) {
                if (!first) oss << ", ";
                first = false;
                oss << "{" << std::string(a->label) << ":" << types_.to_string(arg_type_now(a)) << "}";
            }
            oss << ")";
            return oss.str();
        };

        /// @brief 후보 시그니처를 사람이 읽기 좋은 문자열로 만든다.
        const auto format_candidate = [&](const Candidate& c) -> std::string {
            std::ostringstream oss;
            oss << callee_name << "(";
            bool first = true;
            for (const auto& p : c.positional) {
                if (!first) oss << ", ";
                first = false;
                oss << p.name << ":" << types_.to_string(p.type);
            }
            if (!c.named.empty()) {
                if (!first) oss << ", ";
                oss << "{";
                for (size_t i = 0; i < c.named.size(); ++i) {
                    if (i) oss << ", ";
                    oss << c.named[i].name << ":" << types_.to_string(c.named[i].type);
                    if (c.named[i].has_default) oss << "=?";
                }
                oss << "}";
            }
            oss << ") -> " << types_.to_string(c.ret);
            return oss.str();
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
            diag_(diag::Code::kOverloadNoMatchingCall, e.span, callee_name, make_callsite_summary());
            err_(e.span, msg);
            check_all_arg_exprs_only();
            return fallback_ret;
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
            diag_(diag::Code::kOverloadNoMatchingCall, e.span, callee_name, make_callsite_summary());
            err_(e.span, msg);
            check_all_arg_exprs_only();
            return fallback_ret;
        }

        if (final_matches.size() > 1) {
            std::string msg = "ambiguous overloaded call '" + callee_name + "'";
            std::string candidate_list;
            for (size_t i = 0; i < final_matches.size(); ++i) {
                if (i) candidate_list += ", ";
                candidate_list += format_candidate(candidates[final_matches[i]]);
            }
            diag_(diag::Code::kOverloadAmbiguousCall, e.span, callee_name, candidate_list);
            err_(e.span, msg);
            check_all_arg_exprs_only();
            return fallback_ret;
        }

        const Candidate& selected = candidates[final_matches.front()];
        if (current_expr_id_ != ast::k_invalid_expr &&
            current_expr_id_ < expr_overload_target_cache_.size()) {
            expr_overload_target_cache_[current_expr_id_] = selected.decl_id;
        }

        /// @brief 선택된 후보에 대해 infer-int 해소를 포함한 최종 타입 검증을 수행한다.
        const auto check_arg_against_param_final = [&](const ast::Arg& a, const ParamInfo& p) {
            if (a.expr == ast::k_invalid_expr) {
                diag_(diag::Code::kTypeArgTypeMismatch, a.span,
                    std::to_string(p.decl_index), types_.to_string(p.type), "<missing>");
                err_(a.span, "argument type mismatch for parameter '" + p.name + "'");
                return;
            }

            const CoercionPlan plan = classify_assign_with_coercion_(
                AssignSite::CallArg, p.type, a.expr, a.span);
            const ty::TypeId at = plan.src_after;
            if (!plan.ok) {
                diag_(diag::Code::kTypeArgTypeMismatch, a.span,
                    std::to_string(p.decl_index), types_.to_string(p.type), type_for_user_diag_(at, a.expr));
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
