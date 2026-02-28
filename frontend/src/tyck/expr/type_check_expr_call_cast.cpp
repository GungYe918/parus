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

    ty::TypeId TypeChecker::check_expr_call_(ast::Expr e) {
        // e.a = callee, args slice in e.arg_begin/e.arg_count
        const bool is_spawn_expr = (e.kind == ast::ExprKind::kSpawn);
        if (current_expr_id_ != ast::k_invalid_expr &&
            current_expr_id_ < expr_overload_target_cache_.size()) {
            expr_overload_target_cache_[current_expr_id_] = ast::k_invalid_stmt;
        }
        if (current_expr_id_ != ast::k_invalid_expr &&
            current_expr_id_ < expr_ctor_owner_type_cache_.size()) {
            expr_ctor_owner_type_cache_[current_expr_id_] = ty::kInvalidType;
        }

        // Snapshot call-site args before any generic instantiation can mutate AST storage.
        std::vector<ast::Arg> call_args;
        {
            const auto& all_args = ast_.args();
            const uint64_t begin = e.arg_begin;
            const uint64_t end = begin + e.arg_count;
            if (begin <= all_args.size() && end <= all_args.size()) {
                call_args.reserve(e.arg_count);
                for (uint32_t i = 0; i < e.arg_count; ++i) {
                    call_args.push_back(all_args[e.arg_begin + i]);
                }
            } else {
                diag_(diag::Code::kTypeErrorGeneric, e.span, "invalid call-argument slice on call expression");
                err_(e.span, "invalid call-argument slice");
                return types_.error();
            }
        }

        auto check_all_arg_exprs_only = [&]() {
            for (const auto& a : call_args) {
                if (a.expr != ast::k_invalid_expr) (void)check_expr_(a.expr);
            }
        };

        std::vector<ty::TypeId> explicit_call_type_args;
        if (e.call_type_arg_count > 0) {
            const auto& type_args = ast_.type_args();
            const uint64_t begin = e.call_type_arg_begin;
            const uint64_t end = begin + e.call_type_arg_count;
            if (begin <= type_args.size() && end <= type_args.size()) {
                explicit_call_type_args.reserve(e.call_type_arg_count);
                for (uint32_t i = 0; i < e.call_type_arg_count; ++i) {
                    explicit_call_type_args.push_back(type_args[e.call_type_arg_begin + i]);
                }
            } else {
                diag_(diag::Code::kGenericCallTypeArgParseAmbiguous, e.span);
                err_(e.span, "invalid generic type-argument slice on call expression");
                check_all_arg_exprs_only();
                return types_.error();
            }
        }

        // ------------------------------------------------------------
        // 0) split args + call form classification
        // ------------------------------------------------------------
        std::vector<const ast::Arg*> outside_positional;
        std::vector<const ast::Arg*> outside_labeled;
        outside_positional.reserve(e.arg_count);
        outside_labeled.reserve(e.arg_count);

        bool seen_labeled = false;
        bool has_invalid_order = false;

        for (uint32_t i = 0; i < call_args.size(); ++i) {
            const auto& a = call_args[i];
            const bool is_labeled = a.has_label || (a.kind == ast::ArgKind::kLabeled);
            if (is_labeled) {
                seen_labeled = true;
                outside_labeled.push_back(&a);
            } else {
                if (seen_labeled) has_invalid_order = true;
                outside_positional.push_back(&a);
            }
        }

        enum class CallForm : uint8_t {
            kPositionalOnly,
            kLabeledOnly,
            kPositionalThenLabeled,
            kMixedInvalid,
        };

        CallForm form = CallForm::kPositionalOnly;
        if (has_invalid_order) {
            form = CallForm::kMixedInvalid;
        } else if (!outside_positional.empty() && !outside_labeled.empty()) {
            form = CallForm::kPositionalThenLabeled;
        } else if (!outside_labeled.empty()) {
            form = CallForm::kLabeledOnly;
        } else {
            form = CallForm::kPositionalOnly;
        }

        ty::TypeId fallback_ret = types_.error();
        ty::TypeId callee_t = ty::kInvalidType;
        uint32_t callee_param_count = 0;

        std::string callee_name;
        bool is_dot_method_call = false;
        bool is_explicit_acts_path_call = false;
        bool is_ctor_call = false;
        ty::TypeId ctor_owner_type = ty::kInvalidType;
        ty::TypeId dot_owner_type = ty::kInvalidType;
        bool dot_needs_self_normalization = false;
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

        auto normalize_self_for_owner = [&](auto&& self, ty::TypeId t, ty::TypeId owner_t) -> ty::TypeId {
            if (t == ty::kInvalidType || owner_t == ty::kInvalidType) return t;
            const auto& tt = types_.get(t);
            switch (tt.kind) {
                case ty::Kind::kNamedUser:
                    if (types_.to_string(t) == "Self") return owner_t;
                    return t;
                case ty::Kind::kBorrow: {
                    const ty::TypeId elem = self(self, tt.elem, owner_t);
                    return types_.make_borrow(elem, tt.borrow_is_mut);
                }
                case ty::Kind::kPtr: {
                    const ty::TypeId elem = self(self, tt.elem, owner_t);
                    return types_.make_ptr(elem, tt.ptr_is_mut);
                }
                case ty::Kind::kEscape: {
                    const ty::TypeId elem = self(self, tt.elem, owner_t);
                    return types_.make_escape(elem);
                }
                case ty::Kind::kOptional: {
                    const ty::TypeId elem = self(self, tt.elem, owner_t);
                    return types_.make_optional(elem);
                }
                case ty::Kind::kArray: {
                    const ty::TypeId elem = self(self, tt.elem, owner_t);
                    return types_.make_array(elem, tt.array_has_size, tt.array_size);
                }
                case ty::Kind::kFn: {
                    const uint32_t param_count = tt.param_count;
                    std::vector<ty::TypeId> params;
                    std::vector<std::string_view> labels;
                    std::vector<uint8_t> has_default_flags;
                    params.reserve(param_count);
                    labels.reserve(param_count);
                    has_default_flags.reserve(param_count);

                    for (uint32_t pi = 0; pi < param_count; ++pi) {
                        params.push_back(self(self, types_.fn_param_at(t, pi), owner_t));
                        labels.push_back(types_.fn_param_label_at(t, pi));
                        has_default_flags.push_back(types_.fn_param_has_default_at(t, pi) ? 1u : 0u);
                    }
                    const ty::TypeId ret = self(self, tt.ret, owner_t);
                    return types_.make_fn(
                        ret,
                        params.empty() ? nullptr : params.data(),
                        param_count,
                        types_.fn_positional_count(t),
                        labels.empty() ? nullptr : labels.data(),
                        has_default_flags.empty() ? nullptr : has_default_flags.data()
                    );
                }
                default:
                    return t;
            }
        };

        auto is_class_lifecycle_decl = [&](ast::StmtId sid) -> bool {
            if (sid == ast::k_invalid_stmt || (size_t)sid >= ast_.stmts().size()) return false;
            if (class_member_fn_sid_set_.find(sid) == class_member_fn_sid_set_.end()) return false;
            const auto& m = ast_.stmt(sid);
            if (m.kind != ast::StmtKind::kFnDecl) return false;
            return m.name == "init" || m.name == "deinit";
        };

        auto is_actor_lifecycle_decl = [&](ast::StmtId sid) -> bool {
            if (sid == ast::k_invalid_stmt || (size_t)sid >= ast_.stmts().size()) return false;
            if (actor_member_fn_sid_set_.find(sid) == actor_member_fn_sid_set_.end()) return false;
            const auto& m = ast_.stmt(sid);
            if (m.kind != ast::StmtKind::kFnDecl) return false;
            return m.name == "init" || m.name == "deinit";
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
                    const ast::Expr& recv_expr = ast_.expr(callee_expr.a);
                    if (recv_expr.kind == ast::ExprKind::kIdent) {
                        std::string recv_lookup = std::string(recv_expr.text);
                        if (auto rewritten = rewrite_imported_path_(recv_lookup)) {
                            recv_lookup = *rewritten;
                        }
                        if (auto recv_sid = lookup_symbol_(recv_lookup)) {
                            const auto& recv_sym = sym_.symbol(*recv_sid);
                            if (recv_sym.kind == sema::SymbolKind::kType) {
                                diag_(diag::Code::kDotReceiverMustBeValue, recv_expr.span, recv_expr.text);
                                err_(recv_expr.span, "dot call receiver must be a value, not a type name");
                                check_all_arg_exprs_only();
                                return types_.error();
                            }
                        }
                    }

                    ty::TypeId owner_t = check_expr_(callee_expr.a);
                    if (owner_t != ty::kInvalidType) {
                        const auto& ot = types_.get(owner_t);
                        if (ot.kind == ty::Kind::kBorrow) {
                            owner_t = ot.elem;
                        }
                    }

                    auto resolve_owner_type_in_map = [&](auto& method_map, ty::TypeId t) -> ty::TypeId {
                        if (t == ty::kInvalidType) return t;
                        if (method_map.find(t) != method_map.end()) {
                            return t;
                        }
                        const auto& tt = types_.get(t);
                        if (tt.kind != ty::Kind::kNamedUser) return t;
                        if (auto sid = lookup_symbol_(types_.to_string(t))) {
                            const auto& ss = sym_.symbol(*sid);
                            if (ss.kind == sema::SymbolKind::kType &&
                                method_map.find(ss.declared_type) != method_map.end()) {
                                return ss.declared_type;
                            }
                        }
                        return t;
                    };

                    const ty::TypeId class_owner_t = resolve_owner_type_in_map(class_effective_method_map_, owner_t);
                    const ty::TypeId actor_owner_t = resolve_owner_type_in_map(actor_method_map_, owner_t);
                    bool dot_member_named = false;

                    if (class_owner_t != ty::kInvalidType) {
                        auto cit = class_effective_method_map_.find(class_owner_t);
                        if (cit != class_effective_method_map_.end()) {
                            auto mit = cit->second.find(std::string(rhs.text));
                            if (mit != cit->second.end() && !mit->second.empty()) {
                                dot_member_named = true;

                                bool has_self_receiver_candidate = false;
                                for (const auto sid : mit->second) {
                                    if (sid == ast::k_invalid_stmt || (size_t)sid >= ast_.stmts().size()) continue;
                                    const auto& m = ast_.stmt(sid);
                                    if (m.kind != ast::StmtKind::kFnDecl) continue;
                                    bool has_self = false;
                                    for (uint32_t pi = 0; pi < m.param_count; ++pi) {
                                        const auto& p = ast_.params()[m.param_begin + pi];
                                        if (p.is_self) {
                                            has_self = true;
                                            break;
                                        }
                                    }
                                    if (!has_self) continue;
                                    has_self_receiver_candidate = true;
                                    overload_decl_ids.push_back(sid);
                                }

                                if (has_self_receiver_candidate) {
                                    is_dot_method_call = true;
                                    dot_owner_type = class_owner_t;
                                    dot_needs_self_normalization = true;
                                    callee_name = types_.to_string(class_owner_t) + "." + std::string(rhs.text);
                                } else {
                                    diag_(diag::Code::kDotMethodSelfRequired, rhs.span, rhs.text);
                                    err_(rhs.span, "dot call requires self receiver on class/proto method");
                                    check_all_arg_exprs_only();
                                    return types_.error();
                                }
                            }
                        }
                    }

                    if (!is_dot_method_call && actor_owner_t != ty::kInvalidType) {
                        auto ait = actor_method_map_.find(actor_owner_t);
                        if (ait != actor_method_map_.end()) {
                            auto mit = ait->second.find(std::string(rhs.text));
                            if (mit != ait->second.end() && !mit->second.empty()) {
                                dot_member_named = true;
                                bool has_self_receiver_candidate = false;
                                for (const auto sid : mit->second) {
                                    if (sid == ast::k_invalid_stmt || (size_t)sid >= ast_.stmts().size()) continue;
                                    const auto& m = ast_.stmt(sid);
                                    if (m.kind != ast::StmtKind::kFnDecl) continue;
                                    bool has_self = false;
                                    for (uint32_t pi = 0; pi < m.param_count; ++pi) {
                                        const auto& p = ast_.params()[m.param_begin + pi];
                                        if (p.is_self) {
                                            has_self = true;
                                            break;
                                        }
                                    }
                                    if (!has_self) continue;
                                    has_self_receiver_candidate = true;
                                    overload_decl_ids.push_back(sid);
                                }

                                if (has_self_receiver_candidate) {
                                    is_dot_method_call = true;
                                    dot_owner_type = actor_owner_t;
                                    callee_name = types_.to_string(actor_owner_t) + "." + std::string(rhs.text);
                                } else {
                                    diag_(diag::Code::kDotMethodSelfRequired, rhs.span, rhs.text);
                                    err_(rhs.span, "dot call requires self receiver on actor method");
                                    check_all_arg_exprs_only();
                                    return types_.error();
                                }
                            }
                        }
                    }

                    if (!is_dot_method_call && !dot_member_named) {
                        const ActiveActsSelection* bound_selection = nullptr;
                        const ast::Expr& recv_expr = ast_.expr(callee_expr.a);
                        if (recv_expr.kind == ast::ExprKind::kIdent) {
                            std::string recv_lookup = std::string(recv_expr.text);
                            if (auto rewritten = rewrite_imported_path_(recv_lookup)) {
                                recv_lookup = *rewritten;
                            }
                            if (auto recv_sid = lookup_symbol_(recv_lookup)) {
                                bound_selection = lookup_symbol_acts_selection_(*recv_sid);
                            }
                        }

                        const auto selected_methods = lookup_acts_methods_for_call_(owner_t, rhs.text, bound_selection);
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
                            dot_owner_type = owner_t;
                            callee_name = types_.to_string(owner_t) + "." + std::string(rhs.text);
                        } else if (any_method_named && !selected_methods.empty()) {
                            diag_(diag::Code::kDotMethodSelfRequired, rhs.span, rhs.text);
                            err_(rhs.span, "dot call requires self receiver on acts method");
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
                        oss << "acts path owner must be a type path (use T::acts(...), not value::acts(...))";
                        diag_(diag::Code::kTypeErrorGeneric, callee_expr.span, oss.str());
                        err_(callee_expr.span, oss.str());
                        check_all_arg_exprs_only();
                        return types_.error();
                    }
                    ty::TypeId owner_t = canonicalize_acts_owner_type_(owner_sym.declared_type);
                    if (owner_t == ty::kInvalidType) {
                        std::ostringstream oss;
                        oss << "acts path owner must resolve to a field/class type in v0, got '"
                            << owner_sym.name << "'";
                        diag_(diag::Code::kTypeErrorGeneric, callee_expr.span, oss.str());
                        err_(callee_expr.span, oss.str());
                        check_all_arg_exprs_only();
                        return types_.error();
                    }

                    auto collect_member_from_decl = [&](ast::StmtId acts_sid) {
                        if (acts_sid == ast::k_invalid_stmt || (size_t)acts_sid >= ast_.stmts().size()) return;
                        const auto& acts_decl = ast_.stmt(acts_sid);
                        if (acts_decl.kind != ast::StmtKind::kActsDecl) return;

                        const auto& kids = ast_.stmt_children();
                        const uint32_t begin = acts_decl.stmt_begin;
                        const uint32_t end = acts_decl.stmt_begin + acts_decl.stmt_count;
                        if (begin >= kids.size() || end > kids.size()) return;

                        for (uint32_t i = begin; i < end; ++i) {
                            const ast::StmtId msid = kids[i];
                            if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                            const auto& member = ast_.stmt(msid);
                            if (member.kind != ast::StmtKind::kFnDecl) continue;
                            if (member.fn_is_operator) continue;
                            if (member.name != explicit_acts.member_name) continue;
                            overload_decl_ids.push_back(msid);
                        }
                    };

                    if (explicit_acts.set_name == "default") {
                        bool has_default_decl = false;
                        for (uint32_t sid = 0; sid < (uint32_t)ast_.stmts().size(); ++sid) {
                            const auto& s = ast_.stmt(sid);
                            if (s.kind != ast::StmtKind::kActsDecl) continue;
                            if (!s.acts_is_for || s.acts_has_set_name) continue;
                            const ty::TypeId decl_owner = canonicalize_acts_owner_type_(s.acts_target_type);
                            if (decl_owner != owner_t) continue;
                            has_default_decl = true;
                            collect_member_from_decl(sid);
                        }

                        if (!has_default_decl) {
                            std::ostringstream oss;
                            oss << "no default acts are declared for type " << types_.to_string(owner_t);
                            diag_(diag::Code::kTypeErrorGeneric, callee_expr.span, oss.str());
                            err_(callee_expr.span, oss.str());
                            check_all_arg_exprs_only();
                            return types_.error();
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
                        collect_member_from_decl(*named_sid);
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
                if (callee_expr.kind == ast::ExprKind::kIdent) {
                    std::string lookup_name = std::string(callee_expr.text);
                    if (auto rewritten = rewrite_imported_path_(lookup_name)) {
                        lookup_name = *rewritten;
                    }

                    callee_name = lookup_name;
                    if (auto sid = lookup_symbol_(lookup_name)) {
                        const auto& sym = sym_.symbol(*sid);
                        if (sym.kind == sema::SymbolKind::kType) {
                            const bool is_class_type =
                                (class_decl_by_name_.find(sym.name) != class_decl_by_name_.end());
                            const bool is_actor_type =
                                (actor_decl_by_name_.find(sym.name) != actor_decl_by_name_.end());

                            if (is_spawn_expr) {
                                if (!is_actor_type) {
                                    diag_(diag::Code::kActorSpawnTargetMustBeActor, callee_expr.span, sym.name);
                                    err_(callee_expr.span, "spawn target must be actor type");
                                    check_all_arg_exprs_only();
                                    return types_.error();
                                }

                                is_ctor_call = true;
                                ctor_owner_type = sym.declared_type;
                                fallback_ret = (ctor_owner_type == ty::kInvalidType) ? types_.error() : ctor_owner_type;
                                callee_name = sym.name;

                                std::string init_qname = sym.name;
                                init_qname += "::init";
                                auto fit = fn_decl_by_name_.find(init_qname);
                                if (fit != fn_decl_by_name_.end()) {
                                    overload_decl_ids = fit->second;
                                }
                                if (overload_decl_ids.empty()) {
                                    diag_(diag::Code::kActorSpawnMissingInit, callee_expr.span, sym.name);
                                    err_(callee_expr.span, "actor spawn requires init overload");
                                    check_all_arg_exprs_only();
                                    return types_.error();
                                }
                            } else if (is_actor_type) {
                                diag_(diag::Code::kActorCtorStyleCallNotAllowed, callee_expr.span, sym.name);
                                err_(callee_expr.span, "actor construction requires spawn");
                                check_all_arg_exprs_only();
                                return types_.error();
                            } else if (is_class_type) {
                                is_ctor_call = true;
                                ctor_owner_type = sym.declared_type;
                                fallback_ret = (ctor_owner_type == ty::kInvalidType) ? types_.error() : ctor_owner_type;
                                callee_name = sym.name;

                                std::string init_qname = sym.name;
                                init_qname += "::init";
                                auto fit = fn_decl_by_name_.find(init_qname);
                                if (fit != fn_decl_by_name_.end()) {
                                    overload_decl_ids = fit->second;
                                }
                                if (overload_decl_ids.empty()) {
                                    diag_(diag::Code::kClassCtorMissingInit, callee_expr.span, sym.name);
                                    err_(callee_expr.span, "class constructor call requires init overload");
                                    check_all_arg_exprs_only();
                                    return types_.error();
                                }
                            }
                        } else if (!is_spawn_expr && sym.kind == sema::SymbolKind::kFn) {
                            callee_name = sym.name;
                            auto it = fn_decl_by_name_.find(callee_name);
                            if (it != fn_decl_by_name_.end()) {
                                overload_decl_ids = it->second;
                            }
                        }
                    }
                }

                if (is_spawn_expr && !is_ctor_call) {
                    diag_(diag::Code::kActorSpawnTargetMustBeActor, callee_expr.span);
                    err_(callee_expr.span, "spawn target must be actor type");
                    check_all_arg_exprs_only();
                    return types_.error();
                }

                if (!is_ctor_call) {
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
            }

            if (!is_explicit_acts_path_call &&
                !is_ctor_call &&
                !is_spawn_expr &&
                callee_expr.kind == ast::ExprKind::kIdent &&
                std::string_view(callee_expr.text).find("::") != std::string_view::npos &&
                !overload_decl_ids.empty()) {
                bool dropped_removed_target = false;
                bool dropped_lifecycle_target = false;
                bool dropped_actor_target = false;
                std::vector<ast::StmtId> filtered;
                filtered.reserve(overload_decl_ids.size());
                for (const auto sid : overload_decl_ids) {
                    if (proto_member_fn_sid_set_.find(sid) != proto_member_fn_sid_set_.end()) {
                        dropped_removed_target = true;
                        continue;
                    }
                    if (actor_member_fn_sid_set_.find(sid) != actor_member_fn_sid_set_.end()) {
                        dropped_removed_target = true;
                        dropped_actor_target = true;
                        continue;
                    }
                    if (class_member_fn_sid_set_.find(sid) != class_member_fn_sid_set_.end()) {
                        if (sid == ast::k_invalid_stmt || (size_t)sid >= ast_.stmts().size()) {
                            dropped_removed_target = true;
                            continue;
                        }
                        const auto& ms = ast_.stmt(sid);
                        if (ms.name == "init" || ms.name == "deinit") {
                            dropped_removed_target = true;
                            dropped_lifecycle_target = true;
                            continue;
                        }
                        if (!ms.is_static) {
                            dropped_removed_target = true;
                            continue;
                        }
                    }
                    filtered.push_back(sid);
                }
                if (dropped_removed_target && filtered.empty()) {
                    if (dropped_actor_target) {
                        diag_(diag::Code::kActorPathCallRemoved, callee_expr.span, callee_expr.text);
                        err_(callee_expr.span, "actor member path call is removed");
                    } else if (dropped_lifecycle_target) {
                        diag_(diag::Code::kClassLifecycleDirectCallForbidden, callee_expr.span, callee_expr.text);
                        err_(callee_expr.span, "class lifecycle direct call is forbidden");
                    } else {
                        diag_(diag::Code::kClassProtoPathCallRemoved, callee_expr.span, callee_expr.text);
                        err_(callee_expr.span, "class/proto path member call is removed");
                    }
                    check_all_arg_exprs_only();
                    return types_.error();
                }
                if (dropped_removed_target) {
                    overload_decl_ids = std::move(filtered);
                }
            }
        }

        if (fallback_ret == ty::kInvalidType) fallback_ret = types_.error();
        if (callee_name.empty()) callee_name = "<callee>";

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

        {
            Span dup_sp{};
            std::string dup_label;
            if (has_duplicate_labels(outside_labeled, dup_sp, dup_label)) {
                diag_(diag::Code::kDuplicateDecl, dup_sp, dup_label);
                err_(dup_sp, "duplicate argument label '" + dup_label + "'");
                check_all_arg_exprs_only();
                return fallback_ret;
            }
        }

        std::unordered_map<std::string, const ast::Arg*> labeled_by_label;
        labeled_by_label.reserve(outside_labeled.size());
        for (const auto* a : outside_labeled) {
            labeled_by_label.emplace(std::string(a->label), a);
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

        const auto make_callsite_summary = [&]() -> std::string {
            std::ostringstream oss;
            if (form == CallForm::kPositionalOnly) oss << "positional(";
            else if (form == CallForm::kLabeledOnly) oss << "labeled(";
            else if (form == CallForm::kPositionalThenLabeled) oss << "positional+labeled(";
            else oss << "mixed-invalid(";

            bool first = true;
            for (const auto* a : outside_positional) {
                if (!first) oss << ", ";
                first = false;
                oss << type_for_user_diag_(arg_type_now(a), a->expr);
            }
            for (const auto* a : outside_labeled) {
                if (!first) oss << ", ";
                first = false;
                oss << std::string(a->label) << ":" << type_for_user_diag_(arg_type_now(a), a->expr);
            }
            oss << ")";
            return oss.str();
        };

        // ------------------------------------------------------------
        // 3) fallback: overload 집합을 못 찾으면 function type call-shape로 검사
        // ------------------------------------------------------------
        if (overload_decl_ids.empty()) {
            if (callee_t == ty::kInvalidType || types_.get(callee_t).kind != ty::Kind::kFn) {
                check_all_arg_exprs_only();
                return fallback_ret;
            }

            const uint32_t total_cnt = callee_param_count;
            const uint32_t pos_cnt = types_.fn_positional_count(callee_t);
            const uint32_t named_cnt = (total_cnt > pos_cnt) ? (total_cnt - pos_cnt) : 0u;

            struct ShapeParam {
                uint32_t idx = 0;
                std::string name{};
                ty::TypeId type = ty::kInvalidType;
                bool has_default = false;
            };

            std::vector<ShapeParam> pos_params;
            std::vector<ShapeParam> named_params;
            std::unordered_map<std::string, size_t> pos_by_label;
            std::unordered_map<std::string, size_t> named_by_label;
            pos_params.reserve(pos_cnt);
            named_params.reserve(named_cnt);

            for (uint32_t i = 0; i < total_cnt; ++i) {
                ShapeParam p{};
                p.idx = i;
                p.type = types_.fn_param_at(callee_t, i);
                p.name = std::string(types_.fn_param_label_at(callee_t, i));
                p.has_default = types_.fn_param_has_default_at(callee_t, i);

                if (i < pos_cnt) {
                    if (!p.name.empty()) pos_by_label.emplace(p.name, pos_params.size());
                    pos_params.push_back(std::move(p));
                } else {
                    if (!p.name.empty()) named_by_label.emplace(p.name, named_params.size());
                    named_params.push_back(std::move(p));
                }
            }

            auto match_fallback_shape = [&](bool allow_defaults) -> bool {
                if (form == CallForm::kPositionalOnly) {
                    if (outside_positional.size() != pos_params.size()) return false;
                    for (size_t i = 0; i < outside_positional.size(); ++i) {
                        if (!arg_assignable_now(outside_positional[i], pos_params[i].type)) return false;
                    }
                    if (!named_params.empty()) {
                        if (!allow_defaults) return false;
                        for (const auto& p : named_params) {
                            if (!p.has_default) return false;
                        }
                    }
                    return true;
                }

                if (form == CallForm::kLabeledOnly) {
                    for (const auto& pair : labeled_by_label) {
                        if (pos_by_label.find(pair.first) == pos_by_label.end() &&
                            named_by_label.find(pair.first) == named_by_label.end()) {
                            return false;
                        }
                    }

                    for (const auto& p : pos_params) {
                        if (p.name.empty()) return false;
                        auto it = labeled_by_label.find(p.name);
                        if (it == labeled_by_label.end()) {
                            if (!allow_defaults || !p.has_default) return false;
                            continue;
                        }
                        if (!arg_assignable_now(it->second, p.type)) return false;
                    }

                    for (const auto& p : named_params) {
                        if (p.name.empty()) return false;
                        auto it = labeled_by_label.find(p.name);
                        if (it == labeled_by_label.end()) {
                            if (!allow_defaults || !p.has_default) return false;
                            continue;
                        }
                        if (!arg_assignable_now(it->second, p.type)) return false;
                    }
                    return true;
                }

                if (form == CallForm::kPositionalThenLabeled) {
                    if (named_params.empty()) return false;
                    if (outside_positional.size() != pos_params.size()) return false;

                    for (size_t i = 0; i < outside_positional.size(); ++i) {
                        if (!arg_assignable_now(outside_positional[i], pos_params[i].type)) return false;
                    }

                    for (const auto& pair : labeled_by_label) {
                        if (named_by_label.find(pair.first) == named_by_label.end()) return false;
                    }

                    for (const auto& p : named_params) {
                        if (p.name.empty()) return false;
                        auto it = labeled_by_label.find(p.name);
                        if (it == labeled_by_label.end()) {
                            if (!allow_defaults || !p.has_default) return false;
                            continue;
                        }
                        if (!arg_assignable_now(it->second, p.type)) return false;
                    }
                    return true;
                }

                return false;
            };

            const bool stage_a_ok = match_fallback_shape(/*allow_defaults=*/false);
            const bool final_ok = stage_a_ok || match_fallback_shape(/*allow_defaults=*/true);
            if (!final_ok) {
                diag_(diag::Code::kOverloadNoMatchingCall, e.span, callee_name, make_callsite_summary());
                err_(e.span, "no matching callable shape for indirect function call");
                check_all_arg_exprs_only();
                return fallback_ret;
            }

            const auto check_arg_against_type = [&](const ast::Arg& a, ty::TypeId expected, uint32_t idx) {
                if (a.expr == ast::k_invalid_expr) {
                    diag_(diag::Code::kTypeArgTypeMismatch, a.span,
                        std::to_string(idx), types_.to_string(expected), "<missing>");
                    err_(a.span, "argument type mismatch");
                    return;
                }

                const CoercionPlan plan = classify_assign_with_coercion_(
                    AssignSite::CallArg, expected, a.expr, a.span);
                if (!plan.ok) {
                    diag_(diag::Code::kTypeArgTypeMismatch, a.span,
                        std::to_string(idx), types_.to_string(expected),
                        type_for_user_diag_(plan.src_after, a.expr));
                    err_(a.span, "argument type mismatch");
                }
            };

            if (form == CallForm::kPositionalOnly) {
                for (size_t i = 0; i < outside_positional.size() && i < pos_params.size(); ++i) {
                    check_arg_against_type(*outside_positional[i], pos_params[i].type, pos_params[i].idx);
                }
            } else if (form == CallForm::kLabeledOnly) {
                for (const auto& p : pos_params) {
                    if (p.name.empty()) continue;
                    auto it = labeled_by_label.find(p.name);
                    if (it != labeled_by_label.end()) {
                        check_arg_against_type(*it->second, p.type, p.idx);
                    }
                }
                for (const auto& p : named_params) {
                    if (p.name.empty()) continue;
                    auto it = labeled_by_label.find(p.name);
                    if (it != labeled_by_label.end()) {
                        check_arg_against_type(*it->second, p.type, p.idx);
                    }
                }
            } else if (form == CallForm::kPositionalThenLabeled) {
                for (size_t i = 0; i < outside_positional.size() && i < pos_params.size(); ++i) {
                    check_arg_against_type(*outside_positional[i], pos_params[i].type, pos_params[i].idx);
                }
                for (const auto& p : named_params) {
                    if (p.name.empty()) continue;
                    auto it = labeled_by_label.find(p.name);
                    if (it != labeled_by_label.end()) {
                        check_arg_against_type(*it->second, p.type, p.idx);
                    }
                }
            }

            return fallback_ret;
        }

        struct GenericFailureInfo {
            bool has_arity = false;
            uint32_t expected_arity = 0;
            uint32_t got_arity = 0;
            bool has_infer_fail = false;
            bool has_proto_not_found = false;
            std::string proto_not_found{};
            bool has_unsatisfied = false;
            std::string unsat_type_param{};
            std::string unsat_proto{};
            std::string unsat_concrete{};
        };
        GenericFailureInfo generic_failure{};

        auto infer_generic_bindings = [&](auto&& self,
                                          ty::TypeId param_t,
                                          ty::TypeId arg_t,
                                          const std::unordered_set<std::string>& generic_set,
                                          std::unordered_map<std::string, ty::TypeId>& out_bindings) -> bool {
            if (param_t == ty::kInvalidType || arg_t == ty::kInvalidType) return true;
            const auto& pt = types_.get(param_t);
            if (pt.kind == ty::Kind::kNamedUser) {
                const std::string pname = types_.to_string(param_t);
                if (generic_set.find(pname) != generic_set.end()) {
                    auto it = out_bindings.find(pname);
                    if (it == out_bindings.end()) {
                        out_bindings.emplace(pname, arg_t);
                        return true;
                    }
                    return it->second == arg_t;
                }
                return true;
            }

            const auto& at = types_.get(arg_t);
            if (pt.kind != at.kind) return true;
            switch (pt.kind) {
                case ty::Kind::kBorrow:
                case ty::Kind::kPtr:
                case ty::Kind::kEscape:
                case ty::Kind::kOptional:
                    return self(self, pt.elem, at.elem, generic_set, out_bindings);
                case ty::Kind::kArray:
                    if (pt.array_has_size != at.array_has_size) return false;
                    if (pt.array_has_size && pt.array_size != at.array_size) return false;
                    return self(self, pt.elem, at.elem, generic_set, out_bindings);
                case ty::Kind::kFn: {
                    if (pt.param_count != at.param_count) return false;
                    for (uint32_t i = 0; i < pt.param_count; ++i) {
                        if (!self(self,
                                  types_.fn_param_at(param_t, i),
                                  types_.fn_param_at(arg_t, i),
                                  generic_set,
                                  out_bindings)) {
                            return false;
                        }
                    }
                    return self(self, pt.ret, at.ret, generic_set, out_bindings);
                }
                default:
                    return true;
            }
        };

        auto resolve_proto_sid_for_constraint = [&](std::string_view raw) -> std::optional<ast::StmtId> {
            if (raw.empty()) return std::nullopt;
            std::string key(raw);
            if (auto rewritten = rewrite_imported_path_(key)) {
                key = *rewritten;
            }
            auto it = proto_decl_by_name_.find(key);
            if (it != proto_decl_by_name_.end()) return it->second;
            if (auto sym_sid = lookup_symbol_(key)) {
                const auto& ss = sym_.symbol(*sym_sid);
                auto pit = proto_decl_by_name_.find(ss.name);
                if (pit != proto_decl_by_name_.end()) return pit->second;
            }
            return std::nullopt;
        };

        auto proto_all_default_impl = [&](ast::StmtId proto_sid) -> bool {
            if (proto_sid == ast::k_invalid_stmt || (size_t)proto_sid >= ast_.stmts().size()) return false;
            const auto& ps = ast_.stmt(proto_sid);
            if (ps.kind != ast::StmtKind::kProtoDecl) return false;
            const auto& kids = ast_.stmt_children();
            const uint64_t begin = ps.stmt_begin;
            const uint64_t end = begin + ps.stmt_count;
            if (begin > kids.size() || end > kids.size()) return false;
            if (ps.stmt_count == 0) return true;
            for (uint32_t i = 0; i < ps.stmt_count; ++i) {
                const ast::StmtId msid = kids[ps.stmt_begin + i];
                if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) return false;
                const auto& m = ast_.stmt(msid);
                if (m.kind != ast::StmtKind::kFnDecl) return false;
                if (m.a == ast::k_invalid_stmt) return false;
            }
            return true;
        };

        auto type_satisfies_proto_constraint = [&](ty::TypeId concrete_t, ast::StmtId proto_sid) -> bool {
            if (proto_sid == ast::k_invalid_stmt) return false;
            if (proto_all_default_impl(proto_sid)) return true;

            ast::StmtId owner_sid = ast::k_invalid_stmt;
            if (auto cit = class_decl_by_type_.find(concrete_t); cit != class_decl_by_type_.end()) {
                owner_sid = cit->second;
            } else if (auto fit = field_abi_meta_by_type_.find(concrete_t); fit != field_abi_meta_by_type_.end()) {
                owner_sid = fit->second.sid;
            }

            if (owner_sid == ast::k_invalid_stmt || (size_t)owner_sid >= ast_.stmts().size()) return false;
            const auto& owner = ast_.stmt(owner_sid);
            if (owner.kind != ast::StmtKind::kClassDecl && owner.kind != ast::StmtKind::kFieldDecl) {
                return false;
            }

            const auto& refs = ast_.path_refs();
            const uint64_t begin = owner.decl_path_ref_begin;
            const uint64_t end = begin + owner.decl_path_ref_count;
            if (begin > refs.size() || end > refs.size()) return false;
            for (uint32_t i = owner.decl_path_ref_begin; i < owner.decl_path_ref_begin + owner.decl_path_ref_count; ++i) {
                const auto& pr = refs[i];
                const std::string path = path_join_(pr.path_begin, pr.path_count);
                auto psid = resolve_proto_sid_for_constraint(path);
                if (psid.has_value() && *psid == proto_sid) return true;
            }
            return false;
        };

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
            bool is_generic = false;
            std::vector<std::string> generic_param_names;
            std::unordered_map<std::string, ty::TypeId> generic_bindings;
            std::vector<ty::TypeId> generic_concrete_args;
            bool inject_receiver = false;
            uint32_t receiver_decl_index = 0xFFFF'FFFFu;
            std::vector<ParamInfo> positional;
            std::vector<ParamInfo> named;
            std::unordered_map<std::string, size_t> positional_by_label;
            std::unordered_map<std::string, size_t> named_by_label;
            bool generic_viable = true;
        };

        std::vector<Candidate> candidates;
        candidates.reserve(overload_decl_ids.size());

        for (const ast::StmtId sid : overload_decl_ids) {
            const ast::Stmt& def = ast_.stmt(sid);
            if (def.kind != ast::StmtKind::kFnDecl) continue;

            Candidate c{};
            c.decl_id = sid;
            c.is_generic = (def.fn_generic_param_count > 0);
            if (c.is_generic) {
                c.generic_param_names = collect_generic_param_names_(def);
                if (!explicit_call_type_args.empty()) {
                    if (c.generic_param_names.size() != explicit_call_type_args.size()) {
                        generic_failure.has_arity = true;
                        generic_failure.expected_arity = static_cast<uint32_t>(c.generic_param_names.size());
                        generic_failure.got_arity = static_cast<uint32_t>(explicit_call_type_args.size());
                        c.generic_viable = false;
                    } else {
                        for (size_t gi = 0; gi < c.generic_param_names.size(); ++gi) {
                            c.generic_bindings.emplace(c.generic_param_names[gi], explicit_call_type_args[gi]);
                        }
                    }
                }
            } else if (!explicit_call_type_args.empty()) {
                generic_failure.has_arity = true;
                generic_failure.expected_arity = 0;
                generic_failure.got_arity = static_cast<uint32_t>(explicit_call_type_args.size());
                c.generic_viable = false;
            }

            if (!c.generic_viable) {
                continue;
            }

            ty::TypeId resolved_ret = (def.fn_ret != ty::kInvalidType)
                ? def.fn_ret
                : ((def.type != ty::kInvalidType && types_.get(def.type).kind == ty::Kind::kFn)
                    ? types_.get(def.type).ret
                    : fallback_ret);
            if (is_dot_method_call && dot_needs_self_normalization && dot_owner_type != ty::kInvalidType) {
                resolved_ret = normalize_self_for_owner(normalize_self_for_owner, resolved_ret, dot_owner_type);
            }
            if (c.is_generic && !c.generic_bindings.empty()) {
                resolved_ret = substitute_generic_type_(resolved_ret, c.generic_bindings);
            }
            if (is_ctor_call && ctor_owner_type != ty::kInvalidType) {
                c.ret = ctor_owner_type;
            } else {
                c.ret = resolved_ret;
            }

            uint32_t pos_cnt = def.positional_param_count;
            if (pos_cnt > def.param_count) pos_cnt = def.param_count;

            if (is_ctor_call && def.param_count > 0) {
                for (uint32_t pi = 0; pi < def.param_count; ++pi) {
                    const auto& p = ast_.params()[def.param_begin + pi];
                    if (p.is_self) {
                        c.receiver_decl_index = pi;
                        break;
                    }
                }
            }

            if (is_dot_method_call && def.param_count > 0) {
                for (uint32_t pi = 0; pi < def.param_count; ++pi) {
                    const auto& p = ast_.params()[def.param_begin + pi];
                    if (p.is_self) {
                        c.inject_receiver = true;
                        c.receiver_decl_index = pi;
                        break;
                    }
                }
            }

            uint32_t eff_pos_cnt = pos_cnt;
            if ((is_ctor_call || c.inject_receiver) &&
                c.receiver_decl_index != 0xFFFF'FFFFu &&
                c.receiver_decl_index < pos_cnt &&
                eff_pos_cnt > 0) {
                --eff_pos_cnt;
            }
            uint32_t eff_named_cnt = (def.param_count > pos_cnt) ? (def.param_count - pos_cnt) : 0u;
            if ((is_ctor_call || c.inject_receiver) &&
                c.receiver_decl_index != 0xFFFF'FFFFu &&
                c.receiver_decl_index >= pos_cnt &&
                eff_named_cnt > 0) {
                --eff_named_cnt;
            }
            c.positional.reserve(eff_pos_cnt);
            c.named.reserve(eff_named_cnt);

            for (uint32_t i = 0; i < def.param_count; ++i) {
                const auto& p = ast_.params()[def.param_begin + i];
                if ((is_ctor_call || c.inject_receiver) &&
                    c.receiver_decl_index != 0xFFFF'FFFFu &&
                    i == c.receiver_decl_index) {
                    // ctor user-call shape excludes hidden lifecycle receiver
                    continue;
                }
                ParamInfo info{};
                info.decl_index = i;
                info.name = std::string(p.name);
                info.type = (p.type == ty::kInvalidType) ? types_.error() : p.type;
                if (is_dot_method_call && dot_needs_self_normalization && dot_owner_type != ty::kInvalidType) {
                    info.type = normalize_self_for_owner(normalize_self_for_owner, info.type, dot_owner_type);
                }
                if (c.is_generic && !c.generic_bindings.empty()) {
                    info.type = substitute_generic_type_(info.type, c.generic_bindings);
                }
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

        for (auto& c : candidates) {
            if (!c.is_generic || !c.generic_viable) continue;

            if (c.generic_bindings.empty()) {
                std::unordered_set<std::string> generic_set;
                generic_set.reserve(c.generic_param_names.size());
                for (const auto& n : c.generic_param_names) {
                    generic_set.insert(n);
                }

                auto infer_arg = [&](const ast::Arg* a, const ParamInfo& p) -> bool {
                    if (a == nullptr || a->expr == ast::k_invalid_expr) return true;
                    const ty::TypeId at = check_expr_(a->expr);
                    return infer_generic_bindings(
                        infer_generic_bindings,
                        p.type,
                        at,
                        generic_set,
                        c.generic_bindings
                    );
                };

                bool infer_ok = true;
                if (form == CallForm::kPositionalOnly) {
                    const size_t n = std::min(outside_positional.size(), c.positional.size());
                    for (size_t i = 0; i < n; ++i) {
                        if (!infer_arg(outside_positional[i], c.positional[i])) {
                            infer_ok = false;
                            break;
                        }
                    }
                } else if (form == CallForm::kLabeledOnly) {
                    for (const auto& p : c.positional) {
                        auto it = labeled_by_label.find(p.name);
                        if (it != labeled_by_label.end() && !infer_arg(it->second, p)) {
                            infer_ok = false;
                            break;
                        }
                    }
                    if (infer_ok) {
                        for (const auto& p : c.named) {
                            auto it = labeled_by_label.find(p.name);
                            if (it != labeled_by_label.end() && !infer_arg(it->second, p)) {
                                infer_ok = false;
                                break;
                            }
                        }
                    }
                } else if (form == CallForm::kPositionalThenLabeled) {
                    const size_t n = std::min(outside_positional.size(), c.positional.size());
                    for (size_t i = 0; i < n; ++i) {
                        if (!infer_arg(outside_positional[i], c.positional[i])) {
                            infer_ok = false;
                            break;
                        }
                    }
                    if (infer_ok) {
                        for (const auto& p : c.named) {
                            auto it = labeled_by_label.find(p.name);
                            if (it != labeled_by_label.end() && !infer_arg(it->second, p)) {
                                infer_ok = false;
                                break;
                            }
                        }
                    }
                }

                if (!infer_ok) {
                    generic_failure.has_infer_fail = true;
                    c.generic_viable = false;
                    continue;
                }

                for (const auto& n : c.generic_param_names) {
                    if (c.generic_bindings.find(n) == c.generic_bindings.end()) {
                        generic_failure.has_infer_fail = true;
                        c.generic_viable = false;
                        break;
                    }
                }
                if (!c.generic_viable) continue;

                c.generic_concrete_args.reserve(c.generic_param_names.size());
                for (const auto& n : c.generic_param_names) {
                    c.generic_concrete_args.push_back(c.generic_bindings[n]);
                }

                c.ret = substitute_generic_type_(c.ret, c.generic_bindings);
                for (auto& p : c.positional) p.type = substitute_generic_type_(p.type, c.generic_bindings);
                for (auto& p : c.named) p.type = substitute_generic_type_(p.type, c.generic_bindings);
            } else {
                c.generic_concrete_args.reserve(c.generic_param_names.size());
                for (const auto& n : c.generic_param_names) {
                    auto it = c.generic_bindings.find(n);
                    if (it == c.generic_bindings.end()) {
                        generic_failure.has_infer_fail = true;
                        c.generic_viable = false;
                        break;
                    }
                    c.generic_concrete_args.push_back(it->second);
                }
                if (!c.generic_viable) continue;
            }

            const auto& def = ast_.stmt(c.decl_id);
            for (uint32_t ci = 0; ci < def.fn_constraint_count; ++ci) {
                const uint32_t cidx = def.fn_constraint_begin + ci;
                if (cidx >= ast_.fn_constraint_decls().size()) break;
                const auto& cc = ast_.fn_constraint_decls()[cidx];
                auto bit = c.generic_bindings.find(std::string(cc.type_param));
                if (bit == c.generic_bindings.end()) {
                    generic_failure.has_infer_fail = true;
                    c.generic_viable = false;
                    break;
                }

                const std::string proto_path = path_join_(cc.proto_path_begin, cc.proto_path_count);
                auto proto_sid = resolve_proto_sid_for_constraint(proto_path);
                if (!proto_sid.has_value()) {
                    generic_failure.has_proto_not_found = true;
                    generic_failure.proto_not_found = proto_path;
                    c.generic_viable = false;
                    break;
                }

                if (!type_satisfies_proto_constraint(bit->second, *proto_sid)) {
                    generic_failure.has_unsatisfied = true;
                    generic_failure.unsat_type_param = std::string(cc.type_param);
                    generic_failure.unsat_proto = proto_path;
                    generic_failure.unsat_concrete = types_.to_string(bit->second);
                    c.generic_viable = false;
                    break;
                }
            }
        }

        if (candidates.empty()) {
            std::string msg = "no callable declaration candidate for '" + callee_name + "'";
            diag_(diag::Code::kOverloadNoMatchingCall, e.span, callee_name, "no declaration candidates");
            err_(e.span, msg);
            check_all_arg_exprs_only();
            return fallback_ret;
        }

        std::vector<size_t> filtered;
        filtered.reserve(candidates.size());
        for (size_t i = 0; i < candidates.size(); ++i) {
            const auto& c = candidates[i];
            if (!c.generic_viable) {
                continue;
            }
            if (form == CallForm::kPositionalThenLabeled && c.named.empty()) {
                continue;
            }
            filtered.push_back(i);
        }

        if (filtered.empty()) {
            if (generic_failure.has_arity) {
                diag_(diag::Code::kGenericArityMismatch, e.span,
                      std::to_string(generic_failure.expected_arity),
                      std::to_string(generic_failure.got_arity));
                err_(e.span, "generic arity mismatch");
                check_all_arg_exprs_only();
                return fallback_ret;
            }
            if (generic_failure.has_proto_not_found) {
                diag_(diag::Code::kGenericConstraintProtoNotFound, e.span, generic_failure.proto_not_found);
                err_(e.span, "generic constraint references unknown proto");
                check_all_arg_exprs_only();
                return fallback_ret;
            }
            if (generic_failure.has_unsatisfied) {
                diag_(diag::Code::kGenericConstraintUnsatisfied, e.span,
                      generic_failure.unsat_type_param,
                      generic_failure.unsat_proto,
                      generic_failure.unsat_concrete);
                err_(e.span, "generic constraint is not satisfied");
                check_all_arg_exprs_only();
                return fallback_ret;
            }
            if (generic_failure.has_infer_fail) {
                diag_(diag::Code::kGenericTypeArgInferenceFailed, e.span, callee_name);
                err_(e.span, "failed to infer generic call type arguments");
                check_all_arg_exprs_only();
                return fallback_ret;
            }
            std::string msg = "no overload candidate matches call form for '" + callee_name + "'";
            diag_(diag::Code::kOverloadNoMatchingCall, e.span, callee_name, make_callsite_summary());
            err_(e.span, msg);
            check_all_arg_exprs_only();
            return fallback_ret;
        }

        auto match_candidate = [&](const Candidate& c, bool allow_defaults) -> bool {
            if (form == CallForm::kPositionalOnly) {
                if (outside_positional.size() != c.positional.size()) return false;
                for (size_t i = 0; i < outside_positional.size(); ++i) {
                    if (!arg_assignable_now(outside_positional[i], c.positional[i].type)) return false;
                }
                if (!c.named.empty()) {
                    if (!allow_defaults) return false;
                    for (const auto& p : c.named) {
                        if (!p.has_default) return false;
                    }
                }
                return true;
            }

            if (form == CallForm::kLabeledOnly) {
                for (const auto& pair : labeled_by_label) {
                    if (c.positional_by_label.find(pair.first) == c.positional_by_label.end() &&
                        c.named_by_label.find(pair.first) == c.named_by_label.end()) {
                        return false;
                    }
                }

                for (const auto& p : c.positional) {
                    auto it = labeled_by_label.find(p.name);
                    if (it == labeled_by_label.end()) {
                        if (!allow_defaults || !p.has_default) return false;
                        continue;
                    }
                    if (!arg_assignable_now(it->second, p.type)) return false;
                }

                for (const auto& p : c.named) {
                    auto it = labeled_by_label.find(p.name);
                    if (it == labeled_by_label.end()) {
                        if (!allow_defaults || !p.has_default) return false;
                        continue;
                    }
                    if (!arg_assignable_now(it->second, p.type)) return false;
                }
                return true;
            }

            if (form == CallForm::kPositionalThenLabeled) {
                if (outside_positional.size() != c.positional.size()) return false;
                for (size_t i = 0; i < outside_positional.size(); ++i) {
                    if (!arg_assignable_now(outside_positional[i], c.positional[i].type)) return false;
                }

                for (const auto& pair : labeled_by_label) {
                    if (c.named_by_label.find(pair.first) == c.named_by_label.end()) return false;
                }

                for (const auto& p : c.named) {
                    auto it = labeled_by_label.find(p.name);
                    if (it == labeled_by_label.end()) {
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
            if (generic_failure.has_arity) {
                diag_(diag::Code::kGenericArityMismatch, e.span,
                      std::to_string(generic_failure.expected_arity),
                      std::to_string(generic_failure.got_arity));
                err_(e.span, "generic arity mismatch");
                check_all_arg_exprs_only();
                return fallback_ret;
            }
            if (generic_failure.has_proto_not_found) {
                diag_(diag::Code::kGenericConstraintProtoNotFound, e.span, generic_failure.proto_not_found);
                err_(e.span, "generic constraint references unknown proto");
                check_all_arg_exprs_only();
                return fallback_ret;
            }
            if (generic_failure.has_unsatisfied) {
                diag_(diag::Code::kGenericConstraintUnsatisfied, e.span,
                      generic_failure.unsat_type_param,
                      generic_failure.unsat_proto,
                      generic_failure.unsat_concrete);
                err_(e.span, "generic constraint is not satisfied");
                check_all_arg_exprs_only();
                return fallback_ret;
            }
            if (generic_failure.has_infer_fail) {
                diag_(diag::Code::kGenericTypeArgInferenceFailed, e.span, callee_name);
                err_(e.span, "failed to infer generic call type arguments");
                check_all_arg_exprs_only();
                return fallback_ret;
            }
            std::string msg = "no matching overload found for call '" + callee_name + "'";
            diag_(diag::Code::kOverloadNoMatchingCall, e.span, callee_name, make_callsite_summary());
            err_(e.span, msg);
            check_all_arg_exprs_only();
            return fallback_ret;
        }

        if (final_matches.size() > 1) {
            std::vector<size_t> non_generic;
            std::vector<size_t> only_generic;
            non_generic.reserve(final_matches.size());
            only_generic.reserve(final_matches.size());
            for (const auto idx : final_matches) {
                if (candidates[idx].is_generic) only_generic.push_back(idx);
                else non_generic.push_back(idx);
            }

            if (non_generic.size() == 1) {
                final_matches = {non_generic.front()};
            } else if (non_generic.size() > 1) {
                std::string msg = "ambiguous overloaded call '" + callee_name + "'";
                diag_(diag::Code::kSymbolAmbiguousOverload, e.span, callee_name);
                err_(e.span, msg);
                check_all_arg_exprs_only();
                return fallback_ret;
            } else if (only_generic.size() == 1) {
                final_matches = {only_generic.front()};
            } else {
                diag_(diag::Code::kGenericAmbiguousOverload, e.span, callee_name);
                err_(e.span, "ambiguous generic overload call");
                check_all_arg_exprs_only();
                return fallback_ret;
            }
        }

        const Candidate& selected = candidates[final_matches.front()];
        ast::StmtId selected_decl_sid = selected.decl_id;
        if (selected.is_generic) {
            auto inst_sid = ensure_generic_function_instance_(
                selected.decl_id,
                selected.generic_concrete_args,
                e.span
            );
            if (!inst_sid.has_value()) {
                check_all_arg_exprs_only();
                return types_.error();
            }
            selected_decl_sid = *inst_sid;
        }
        if (current_expr_id_ != ast::k_invalid_expr &&
            current_expr_id_ < expr_overload_target_cache_.size()) {
            expr_overload_target_cache_[current_expr_id_] = selected_decl_sid;
        }
        if (current_expr_id_ != ast::k_invalid_expr &&
            current_expr_id_ < expr_ctor_owner_type_cache_.size()) {
            expr_ctor_owner_type_cache_[current_expr_id_] = is_ctor_call ? ctor_owner_type : ty::kInvalidType;
        }

        if (!is_ctor_call && is_actor_lifecycle_decl(selected_decl_sid)) {
            diag_(diag::Code::kActorLifecycleDirectCallForbidden, e.span, callee_name);
            err_(e.span, "actor lifecycle direct call is forbidden");
            check_all_arg_exprs_only();
            return types_.error();
        }

        if (!is_ctor_call && is_class_lifecycle_decl(selected_decl_sid)) {
            diag_(diag::Code::kClassLifecycleDirectCallForbidden, e.span, callee_name);
            err_(e.span, "class lifecycle direct call is forbidden");
            check_all_arg_exprs_only();
            return types_.error();
        }

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

        if (form == CallForm::kPositionalOnly) {
            for (size_t i = 0; i < outside_positional.size() && i < selected.positional.size(); ++i) {
                check_arg_against_param_final(*outside_positional[i], selected.positional[i]);
            }
        } else if (form == CallForm::kLabeledOnly) {
            for (const auto& p : selected.positional) {
                auto it = labeled_by_label.find(p.name);
                if (it != labeled_by_label.end()) {
                    check_arg_against_param_final(*it->second, p);
                }
            }
            for (const auto& p : selected.named) {
                auto it = labeled_by_label.find(p.name);
                if (it != labeled_by_label.end()) {
                    check_arg_against_param_final(*it->second, p);
                }
            }
        } else if (form == CallForm::kPositionalThenLabeled) {
            for (size_t i = 0; i < outside_positional.size() && i < selected.positional.size(); ++i) {
                check_arg_against_param_final(*outside_positional[i], selected.positional[i]);
            }
            for (const auto& p : selected.named) {
                auto it = labeled_by_label.find(p.name);
                if (it != labeled_by_label.end()) {
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
