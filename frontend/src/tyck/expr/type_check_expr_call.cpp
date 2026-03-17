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

    namespace {
        struct CImportCallMeta {
            bool is_c_import = false;
            bool is_c_abi = false;
            bool is_variadic = false;
            std::string callconv{}; // default|cdecl|stdcall|fastcall|vectorcall|win64|sysv
            std::string format_kind{}; // none | fmt_varargs | fmt_vlist
            int32_t fmt_param_index = -1;
            int32_t va_list_param_index = -1;
            std::string variadic_sibling_path{};
        };

        static CImportCallMeta parse_cimport_call_meta_(std::string_view payload) {
            CImportCallMeta out{};
            if (!payload.starts_with("parus_c_import|")) return out;
            out.is_c_import = true;

            size_t pos = 0;
            while (pos < payload.size()) {
                size_t next = payload.find('|', pos);
                if (next == std::string_view::npos) next = payload.size();
                const std::string_view part = payload.substr(pos, next - pos);
                const size_t eq = part.find('=');
                if (eq != std::string_view::npos && eq + 1 < part.size()) {
                    const std::string_view key = part.substr(0, eq);
                    const std::string_view val = part.substr(eq + 1);
                    if (key == "is_c_abi") {
                        out.is_c_abi = (val == "1" || val == "true");
                    } else if (key == "variadic") {
                        out.is_variadic = (val == "1" || val == "true");
                    } else if (key == "format") {
                        out.format_kind.assign(val);
                    } else if (key == "callconv") {
                        out.callconv.assign(val);
                    } else if (key == "fmt_idx") {
                        try {
                            out.fmt_param_index = std::stoi(std::string(val));
                        } catch (...) {
                            out.fmt_param_index = -1;
                        }
                    } else if (key == "va_idx") {
                        try {
                            out.va_list_param_index = std::stoi(std::string(val));
                        } catch (...) {
                            out.va_list_param_index = -1;
                        }
                    } else if (key == "sibling") {
                        out.variadic_sibling_path.assign(val);
                    }
                }
                if (next == payload.size()) break;
                pos = next + 1;
            }
            return out;
        }
    } // namespace

    ty::TypeId TypeChecker::check_expr_call_(ast::Expr e) {
        // e.a = callee, args slice in e.arg_begin/e.arg_count
        if (current_expr_id_ != ast::k_invalid_expr &&
            current_expr_id_ < expr_overload_target_cache_.size()) {
            expr_overload_target_cache_[current_expr_id_] = ast::k_invalid_stmt;
        }
        if (current_expr_id_ != ast::k_invalid_expr &&
            current_expr_id_ < expr_ctor_owner_type_cache_.size()) {
            expr_ctor_owner_type_cache_[current_expr_id_] = ty::kInvalidType;
        }
        if (current_expr_id_ != ast::k_invalid_expr &&
            current_expr_id_ < expr_enum_ctor_owner_type_cache_.size()) {
            expr_enum_ctor_owner_type_cache_[current_expr_id_] = ty::kInvalidType;
        }
        if (current_expr_id_ != ast::k_invalid_expr &&
            current_expr_id_ < expr_enum_ctor_variant_index_cache_.size()) {
            expr_enum_ctor_variant_index_cache_[current_expr_id_] = 0xFFFF'FFFFu;
        }
        if (current_expr_id_ != ast::k_invalid_expr &&
            current_expr_id_ < expr_enum_ctor_tag_value_cache_.size()) {
            expr_enum_ctor_tag_value_cache_[current_expr_id_] = 0;
        }
        if (current_expr_id_ != ast::k_invalid_expr &&
            current_expr_id_ < expr_external_callee_symbol_cache_.size()) {
            expr_external_callee_symbol_cache_[current_expr_id_] = sema::SymbolTable::kNoScope;
        }
        if (current_expr_id_ != ast::k_invalid_expr &&
            current_expr_id_ < expr_external_receiver_expr_cache_.size()) {
            expr_external_receiver_expr_cache_[current_expr_id_] = ast::k_invalid_expr;
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
            kMixedInvalid,
        };

        CallForm form = CallForm::kPositionalOnly;
        if (has_invalid_order) {
            form = CallForm::kMixedInvalid;
        } else if (!outside_positional.empty() && !outside_labeled.empty()) {
            // v1 simplification: positional + labeled tail call form is removed.
            form = CallForm::kMixedInvalid;
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
        bool is_cimport_call = false;
        CImportCallMeta cimport_meta{};
        uint32_t cimport_callee_symbol = sema::SymbolTable::kNoScope;
        ty::TypeId ctor_owner_type = ty::kInvalidType;
        ty::TypeId dot_owner_type = ty::kInvalidType;
        bool dot_needs_self_normalization = false;
        std::vector<ast::StmtId> overload_decl_ids;
        std::vector<ExternalActsMethodDecl> external_overload_methods;

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
                    if (is_self_named_type_(t)) return owner_t;
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

        auto resolve_member_owner_type = [&](ast::ExprId recv_eid, Span member_span) -> ty::TypeId {
            if (recv_eid == ast::k_invalid_expr || (size_t)recv_eid >= ast_.exprs().size()) {
                return ty::kInvalidType;
            }

            const ast::Expr& recv_expr = ast_.expr(recv_eid);
            if (recv_expr.kind == ast::ExprKind::kIdent) {
                std::string recv_lookup = std::string(recv_expr.text);
                if (auto rewritten = rewrite_imported_path_(recv_lookup)) {
                    recv_lookup = *rewritten;
                }
                if (auto recv_sid = lookup_symbol_(recv_lookup)) {
                    const auto& recv_sym = sym_.symbol(*recv_sid);
                    if (recv_sym.kind == sema::SymbolKind::kType) {
                        diag_(diag::Code::kDotReceiverMustBeValue, recv_expr.span, recv_expr.text);
                        err_(recv_expr.span, "member-call receiver must be a value, not a type name");
                        return types_.error();
                    }
                }
            }

            ty::TypeId owner_t = check_expr_(recv_eid);
            if (owner_t != ty::kInvalidType) {
                const auto& ot = types_.get(owner_t);
                if (ot.kind == ty::Kind::kBorrow) {
                    owner_t = ot.elem;
                }
            }

            {
                std::string owner_base;
                std::vector<ty::TypeId> owner_args;
                if (decompose_named_user_type_(owner_t, owner_base, owner_args) && !owner_args.empty()) {
                    std::string owner_key = owner_base;
                    if (auto rewritten = rewrite_imported_path_(owner_key)) {
                        owner_key = *rewritten;
                    }
                    auto cit = class_decl_by_name_.find(owner_key);
                    if (cit != class_decl_by_name_.end()) {
                        if (auto inst_sid = ensure_generic_class_instance_(cit->second, owner_args, member_span)) {
                            const auto& inst = ast_.stmt(*inst_sid);
                            if (inst.kind == ast::StmtKind::kClassDecl && inst.type != ty::kInvalidType) {
                                owner_t = inst.type;
                            }
                        }
                    }
                }
            }

            (void)ensure_generic_field_instance_from_type_(owner_t, member_span);
            ensure_generic_acts_for_owner_(owner_t, member_span);
            return owner_t;
        };

        auto resolve_owner_decl_sid_for_proto = [&](ty::TypeId owner_t) -> ast::StmtId {
            if (owner_t == ty::kInvalidType) return ast::k_invalid_stmt;
            if (auto it = class_decl_by_type_.find(owner_t); it != class_decl_by_type_.end()) {
                return it->second;
            }
            if (auto it = actor_decl_by_type_.find(owner_t); it != actor_decl_by_type_.end()) {
                return it->second;
            }
            if (auto it = enum_decl_by_type_.find(owner_t); it != enum_decl_by_type_.end()) {
                return it->second;
            }
            if (auto it = field_abi_meta_by_type_.find(owner_t); it != field_abi_meta_by_type_.end()) {
                return it->second.sid;
            }
            return ast::k_invalid_stmt;
        };

        auto collect_proto_closure = [&](auto&& self,
                                         ast::StmtId proto_sid,
                                         std::unordered_set<ast::StmtId>& out) -> void {
            if (proto_sid == ast::k_invalid_stmt || (size_t)proto_sid >= ast_.stmts().size()) return;
            if (!out.insert(proto_sid).second) return;
            const auto& ps = ast_.stmt(proto_sid);
            if (ps.kind != ast::StmtKind::kProtoDecl) return;

            const auto& refs = ast_.path_refs();
            const uint64_t ib = ps.decl_path_ref_begin;
            const uint64_t ie = ib + ps.decl_path_ref_count;
            if (ib <= refs.size() && ie <= refs.size()) {
                for (uint32_t i = ps.decl_path_ref_begin; i < ps.decl_path_ref_begin + ps.decl_path_ref_count; ++i) {
                    if (auto base_sid = resolve_proto_decl_from_path_ref_(refs[i], ps.span)) {
                        self(self, *base_sid, out);
                    }
                }
            }
        };

        auto proto_name_matches = [&](ast::StmtId proto_sid, std::string_view q) -> bool {
            if (proto_sid == ast::k_invalid_stmt || (size_t)proto_sid >= ast_.stmts().size()) return false;
            const auto& ps = ast_.stmt(proto_sid);
            if (ps.kind != ast::StmtKind::kProtoDecl) return false;
            if (ps.name == q) return true;
            if (auto it = proto_qualified_name_by_stmt_.find(proto_sid); it != proto_qualified_name_by_stmt_.end()) {
                if (it->second == q) return true;
            }
            return false;
        };

        auto collect_proto_arrow_call_overloads = [&](ty::TypeId owner_t,
                                                      std::string_view member_name,
                                                      std::optional<std::string_view> qualifier,
                                                      Span member_span,
                                                      std::vector<ast::StmtId>& out) -> bool {
            out.clear();
            if (owner_t == ty::kInvalidType) return false;

            const ast::StmtId owner_sid = resolve_owner_decl_sid_for_proto(owner_t);
            if (owner_sid == ast::k_invalid_stmt || (size_t)owner_sid >= ast_.stmts().size()) {
                diag_(diag::Code::kProtoArrowMemberNotFound, member_span, std::string(member_name));
                err_(member_span, "proto arrow member is unavailable for this receiver type");
                return false;
            }

            std::unordered_set<ast::StmtId> declared_proto_sids{};
            const auto& owner = ast_.stmt(owner_sid);
            const auto& refs = ast_.path_refs();
            const uint64_t rb = owner.decl_path_ref_begin;
            const uint64_t re = rb + owner.decl_path_ref_count;
            if (rb <= refs.size() && re <= refs.size()) {
                for (uint32_t i = owner.decl_path_ref_begin; i < owner.decl_path_ref_begin + owner.decl_path_ref_count; ++i) {
                    if (auto psid = resolve_proto_decl_from_path_ref_(refs[i], member_span)) {
                        collect_proto_closure(collect_proto_closure, *psid, declared_proto_sids);
                    }
                }
            }

            std::unordered_set<ast::StmtId> filtered_proto_sids{};
            filtered_proto_sids.reserve(declared_proto_sids.size());
            for (const ast::StmtId psid : declared_proto_sids) {
                if (psid == ast::k_invalid_stmt || (size_t)psid >= ast_.stmts().size()) continue;
                if (evaluate_proto_require_at_apply_(psid, owner_t, member_span,
                                                     /*emit_unsatisfied_diag=*/false,
                                                     /*emit_shape_diag=*/false)) {
                    filtered_proto_sids.insert(psid);
                }
            }

            if (qualifier.has_value()) {
                std::unordered_set<ast::StmtId> narrowed{};
                for (const ast::StmtId psid : filtered_proto_sids) {
                    if (proto_name_matches(psid, *qualifier)) {
                        collect_proto_closure(collect_proto_closure, psid, narrowed);
                    }
                }
                if (narrowed.empty()) {
                    diag_(diag::Code::kProtoArrowMemberNotFound, member_span, std::string(member_name));
                    err_(member_span, "unknown proto qualifier on arrow access");
                    return false;
                }
                filtered_proto_sids = std::move(narrowed);
            }

            std::unordered_set<ast::StmtId> provider_proto_sids{};
            const auto& kids = ast_.stmt_children();
            for (const ast::StmtId psid : filtered_proto_sids) {
                if (psid == ast::k_invalid_stmt || (size_t)psid >= ast_.stmts().size()) continue;
                const auto& ps = ast_.stmt(psid);
                const uint64_t mb = ps.stmt_begin;
                const uint64_t me = mb + ps.stmt_count;
                if (mb > kids.size() || me > kids.size()) continue;
                bool provided_here = false;
                for (uint32_t i = ps.stmt_begin; i < ps.stmt_begin + ps.stmt_count; ++i) {
                    const ast::StmtId msid = kids[i];
                    if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                    const auto& m = ast_.stmt(msid);
                    if (m.kind != ast::StmtKind::kFnDecl) continue;
                    if (m.proto_fn_role != ast::ProtoFnRole::kProvide) continue;
                    if (m.a == ast::k_invalid_stmt) continue;
                    if (m.name != member_name) continue;
                    out.push_back(msid);
                    provided_here = true;
                }
                if (provided_here) provider_proto_sids.insert(psid);
            }

            if (out.empty()) {
                diag_(diag::Code::kProtoArrowMemberNotFound, member_span, std::string(member_name));
                err_(member_span, "proto arrow call target is not found");
                return false;
            }

            if (!qualifier.has_value() && provider_proto_sids.size() > 1) {
                diag_(diag::Code::kProtoArrowQualifierRequired, member_span, std::string(member_name));
                err_(member_span, "arrow member is provided by multiple protos; use receiver->Proto.member");
                return false;
            }

            if (qualifier.has_value() && provider_proto_sids.size() > 1) {
                diag_(diag::Code::kProtoArrowMemberAmbiguous, member_span, std::string(member_name));
                err_(member_span, "arrow member remains ambiguous in qualified proto closure");
                return false;
            }

            return true;
        };

        // ------------------------------------------------------------
        // 1) method call fast-path: `value.ident(...)`
        // ------------------------------------------------------------
        {
            const ast::Expr& callee_expr = ast_.expr(e.a);
            if (callee_expr.kind == ast::ExprKind::kBinary &&
                callee_expr.op == K::kArrow &&
                callee_expr.a != ast::k_invalid_expr &&
                callee_expr.b != ast::k_invalid_expr) {
                const ast::Expr& rhs = ast_.expr(callee_expr.b);
                if (rhs.kind == ast::ExprKind::kIdent) {
                    const ty::TypeId owner_t = resolve_member_owner_type(callee_expr.a, rhs.span);
                    if (is_error_(owner_t)) {
                        check_all_arg_exprs_only();
                        return types_.error();
                    }

                    std::vector<ast::StmtId> proto_overloads;
                    if (!collect_proto_arrow_call_overloads(owner_t, rhs.text, std::nullopt, rhs.span, proto_overloads)) {
                        check_all_arg_exprs_only();
                        return types_.error();
                    }

                    overload_decl_ids.insert(overload_decl_ids.end(), proto_overloads.begin(), proto_overloads.end());
                    is_dot_method_call = true;
                    dot_owner_type = owner_t;
                    dot_needs_self_normalization = false;
                    callee_name = types_.to_string(owner_t) + "->" + std::string(rhs.text);
                }
            }

            if (!is_dot_method_call &&
                callee_expr.kind == ast::ExprKind::kBinary &&
                callee_expr.op == K::kDot &&
                callee_expr.a != ast::k_invalid_expr &&
                callee_expr.b != ast::k_invalid_expr) {
                const ast::Expr& rhs = ast_.expr(callee_expr.b);
                if (rhs.kind == ast::ExprKind::kIdent) {
                    ast::ExprId recv_eid = callee_expr.a;
                    std::optional<std::string_view> proto_qualifier{};
                    const ast::Expr& dot_lhs = ast_.expr(callee_expr.a);
                    if (dot_lhs.kind == ast::ExprKind::kBinary &&
                        dot_lhs.op == K::kArrow &&
                        dot_lhs.a != ast::k_invalid_expr &&
                        dot_lhs.b != ast::k_invalid_expr) {
                        const ast::Expr& qualifier_expr = ast_.expr(dot_lhs.b);
                        if (qualifier_expr.kind != ast::ExprKind::kIdent) {
                            diag_(diag::Code::kUnexpectedToken, qualifier_expr.span, "proto qualifier after '->'");
                            err_(qualifier_expr.span, "invalid proto qualifier in arrow member call");
                            check_all_arg_exprs_only();
                            return types_.error();
                        }
                        recv_eid = dot_lhs.a;
                        proto_qualifier = qualifier_expr.text;
                    }

                    ty::TypeId owner_t = resolve_member_owner_type(recv_eid, rhs.span);
                    if (is_error_(owner_t)) {
                        check_all_arg_exprs_only();
                        return types_.error();
                    }

                    if (proto_qualifier.has_value()) {
                        std::vector<ast::StmtId> proto_overloads;
                        if (!collect_proto_arrow_call_overloads(owner_t, rhs.text, proto_qualifier, rhs.span, proto_overloads)) {
                            check_all_arg_exprs_only();
                            return types_.error();
                        }

                        overload_decl_ids.insert(overload_decl_ids.end(), proto_overloads.begin(), proto_overloads.end());
                        is_dot_method_call = true;
                        dot_owner_type = owner_t;
                        dot_needs_self_normalization = false;
                        callee_name = types_.to_string(owner_t) + "->" + std::string(*proto_qualifier) + "." + std::string(rhs.text);
                    }

                    if (!is_dot_method_call) {

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

                    ty::TypeId class_owner_t = resolve_owner_type_in_map(class_effective_method_map_, owner_t);
                    const ty::TypeId actor_owner_t = resolve_owner_type_in_map(actor_method_map_, owner_t);
                    bool dot_member_named = false;

                    if (class_owner_t != ty::kInvalidType &&
                        class_effective_method_map_.find(class_owner_t) == class_effective_method_map_.end()) {
                        if (auto cit = class_decl_by_type_.find(class_owner_t); cit != class_decl_by_type_.end()) {
                            const ast::StmtId csid = cit->second;
                            if (generic_decl_checked_instances_.find(csid) == generic_decl_checked_instances_.end() &&
                                generic_decl_checking_instances_.find(csid) == generic_decl_checking_instances_.end()) {
                                check_stmt_class_decl_(csid);
                                generic_decl_checked_instances_.insert(csid);
                            }
                        }
                    }
                    class_owner_t = resolve_owner_type_in_map(class_effective_method_map_, owner_t);

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
                        bool any_external_method_named = false;
                        std::vector<ExternalActsMethodDecl> selected_external_methods;
                        if (owner_t != ty::kInvalidType) {
                            auto oit = external_acts_default_method_map_.find(owner_t);
                            if (oit != external_acts_default_method_map_.end()) {
                                auto mit = oit->second.find(std::string(rhs.text));
                                if (mit != oit->second.end() && !mit->second.empty()) {
                                    any_external_method_named = true;
                                    selected_external_methods = mit->second;
                                }
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
                        } else if (any_external_method_named) {
                            bool has_external_self_receiver = false;
                            for (const auto& md : selected_external_methods) {
                                if (!md.receiver_is_self) continue;
                                has_external_self_receiver = true;
                                external_overload_methods.push_back(md);
                            }
                            if (has_external_self_receiver) {
                                is_dot_method_call = true;
                                dot_owner_type = owner_t;
                                callee_name = types_.to_string(owner_t) + "." + std::string(rhs.text);
                            } else {
                                diag_(diag::Code::kDotMethodSelfRequired, rhs.span, rhs.text);
                                err_(rhs.span, "dot call requires self receiver on acts method");
                                check_all_arg_exprs_only();
                                return types_.error();
                            }
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
        }

        // ------------------------------------------------------------
        // 2) non-method path: regular callee typing + ident overload lookup
        // ------------------------------------------------------------
        if (!is_dot_method_call) {
            const ast::Expr& callee_expr = ast_.expr(e.a);

            if (callee_expr.kind == ast::ExprKind::kIdent) {
                const std::string callee_text(callee_expr.text);
                const size_t split = callee_text.rfind("::");
                if (split != std::string::npos && split > 0 && split + 2 < callee_text.size()) {
                    const std::string owner_text = callee_text.substr(0, split);
                    const std::string variant_text = callee_text.substr(split + 2);

                    ty::TypeId enum_owner_type = ty::kInvalidType;
                    ast::StmtId enum_owner_sid = ast::k_invalid_stmt;

                    auto resolve_plain_enum_owner = [&](const std::string& raw_name) -> ast::StmtId {
                        std::string key = raw_name;
                        if (auto rewritten = rewrite_imported_path_(key)) {
                            key = *rewritten;
                        }

                        // 1) exact enum-decl name key lookup
                        auto it = enum_decl_by_name_.find(key);
                        if (it != enum_decl_by_name_.end()) return it->second;

                        // 1.5) suffix lookup for qualified names (e.g. pkg::Token)
                        {
                            ast::StmtId found = ast::k_invalid_stmt;
                            const std::string suffix = "::" + key;
                            for (const auto& kv : enum_decl_by_name_) {
                                const std::string& q = kv.first;
                                if (q == key) {
                                    found = kv.second;
                                    break;
                                }
                                if (q.size() > suffix.size() &&
                                    q.compare(q.size() - suffix.size(), suffix.size(), suffix) == 0) {
                                    if (found != ast::k_invalid_stmt && found != kv.second) {
                                        found = ast::k_invalid_stmt;
                                        break;
                                    }
                                    found = kv.second;
                                }
                            }
                            if (found != ast::k_invalid_stmt) return found;
                        }

                        // 2) direct type-id lookup by the text key
                        const ty::TypeId key_ty = types_.intern_ident(key);
                        if (key_ty != ty::kInvalidType) {
                            auto eit = enum_decl_by_type_.find(key_ty);
                            if (eit != enum_decl_by_type_.end()) return eit->second;
                        }

                        // 3) symbol table fallback
                        if (auto sym_sid = lookup_symbol_(key)) {
                            const auto& ss = sym_.symbol(*sym_sid);
                            if (ss.kind == sema::SymbolKind::kType &&
                                ss.declared_type != ty::kInvalidType) {
                                auto eit2 = enum_decl_by_type_.find(ss.declared_type);
                                if (eit2 != enum_decl_by_type_.end()) return eit2->second;
                            }
                            auto eit = enum_decl_by_name_.find(ss.name);
                            if (eit != enum_decl_by_name_.end()) return eit->second;
                        }
                        return ast::k_invalid_stmt;
                    };

                    std::string owner_base;
                    std::vector<ty::TypeId> owner_args;
                    const ty::TypeId owner_maybe_generic = types_.intern_ident(owner_text);
                    if (decompose_named_user_type_(owner_maybe_generic, owner_base, owner_args) && !owner_args.empty()) {
                        std::string base_lookup = owner_base;
                        if (auto rewritten = rewrite_imported_path_(base_lookup)) {
                            base_lookup = *rewritten;
                        }
                        auto bit = enum_decl_by_name_.find(base_lookup);
                        if (bit != enum_decl_by_name_.end()) {
                            const auto& templ = ast_.stmt(bit->second);
                            if (templ.kind == ast::StmtKind::kEnumDecl &&
                                templ.decl_generic_param_count > 0) {
                                if (auto inst_sid = ensure_generic_enum_instance_(bit->second, owner_args, callee_expr.span)) {
                                    enum_owner_sid = *inst_sid;
                                    enum_owner_type = ast_.stmt(*inst_sid).type;
                                }
                            } else if (templ.kind == ast::StmtKind::kEnumDecl &&
                                       templ.decl_generic_param_count == 0) {
                                diag_(diag::Code::kGenericArityMismatch, callee_expr.span,
                                      "0", std::to_string(owner_args.size()));
                                err_(callee_expr.span, "non-generic enum constructor path does not accept type arguments");
                                check_all_arg_exprs_only();
                                return types_.error();
                            }
                        }
                    } else {
                        enum_owner_sid = resolve_plain_enum_owner(owner_text);
                        if (enum_owner_sid != ast::k_invalid_stmt &&
                            (size_t)enum_owner_sid < ast_.stmts().size()) {
                            const auto& decl = ast_.stmt(enum_owner_sid);
                            if (decl.kind == ast::StmtKind::kEnumDecl &&
                                decl.decl_generic_param_count > 0) {
                                diag_(diag::Code::kGenericTypeArgInferenceFailed, callee_expr.span, owner_text);
                                err_(callee_expr.span, "generic enum constructor requires explicit owner type arguments");
                                check_all_arg_exprs_only();
                                return types_.error();
                            }
                            enum_owner_type = decl.type;
                        }
                    }

                    if (enum_owner_sid != ast::k_invalid_stmt &&
                        (size_t)enum_owner_sid < ast_.stmts().size() &&
                        enum_owner_type != ty::kInvalidType) {
                        if (enum_abi_meta_by_type_.find(enum_owner_type) == enum_abi_meta_by_type_.end()) {
                            check_stmt_enum_decl_(enum_owner_sid);
                        }

                        auto mit = enum_abi_meta_by_type_.find(enum_owner_type);
                        if (mit == enum_abi_meta_by_type_.end()) {
                            diag_(diag::Code::kTypeErrorGeneric, callee_expr.span, "failed to build enum metadata");
                            err_(callee_expr.span, "failed to build enum metadata");
                            check_all_arg_exprs_only();
                            return types_.error();
                        }

                        const auto& meta = mit->second;
                        auto vit = meta.variant_index_by_name.find(variant_text);
                        if (vit == meta.variant_index_by_name.end()) {
                            diag_(diag::Code::kTypeErrorGeneric, callee_expr.span,
                                  std::string("unknown enum constructor variant '") + variant_text + "'");
                            err_(callee_expr.span, "unknown enum constructor variant");
                            check_all_arg_exprs_only();
                            return types_.error();
                        }
                        const auto& vm = meta.variants[vit->second];

                        if (!outside_positional.empty()) {
                            diag_(diag::Code::kEnumCtorArgMismatch, e.span, callee_text);
                            err_(e.span, "enum constructor only accepts labeled payload arguments");
                            check_all_arg_exprs_only();
                            return types_.error();
                        }

                        bool ctor_ok = true;
                        if (outside_labeled.size() != vm.fields.size()) {
                            diag_(diag::Code::kEnumCtorArgMismatch, e.span, callee_text);
                            err_(e.span, "enum constructor payload argument count mismatch");
                            ctor_ok = false;
                        }

                        std::unordered_set<std::string> used_labels;
                        used_labels.reserve(outside_labeled.size());
                        for (const auto* arg : outside_labeled) {
                            auto fit = vm.field_index_by_name.find(std::string(arg->label));
                            if (fit == vm.field_index_by_name.end()) {
                                diag_(diag::Code::kEnumCtorLabelMismatch, arg->span, arg->label);
                                err_(arg->span, "unknown enum constructor payload label");
                                ctor_ok = false;
                                continue;
                            }
                            if (!used_labels.insert(std::string(arg->label)).second) {
                                diag_(diag::Code::kEnumCtorLabelMismatch, arg->span, arg->label);
                                err_(arg->span, "duplicate enum constructor payload label");
                                ctor_ok = false;
                                continue;
                            }
                            const auto& field = vm.fields[fit->second];
                            if (arg->expr == ast::k_invalid_expr) {
                                diag_(diag::Code::kEnumCtorArgMismatch, arg->span, callee_text);
                                err_(arg->span, "missing enum constructor payload value");
                                ctor_ok = false;
                                continue;
                            }
                            const CoercionPlan plan = classify_assign_with_coercion_(
                                AssignSite::CallArg, field.type, arg->expr, arg->span);
                            if (!plan.ok) {
                                diag_(diag::Code::kEnumCtorTypeMismatch, arg->span,
                                      field.name, types_.to_string(field.type),
                                      type_for_user_diag_(plan.src_after, arg->expr));
                                err_(arg->span, "enum constructor payload type mismatch");
                                ctor_ok = false;
                            }
                        }

                        if (!ctor_ok) {
                            check_all_arg_exprs_only();
                            return types_.error();
                        }

                        if (current_expr_id_ != ast::k_invalid_expr &&
                            current_expr_id_ < expr_enum_ctor_owner_type_cache_.size()) {
                            expr_enum_ctor_owner_type_cache_[current_expr_id_] = enum_owner_type;
                        }
                        if (current_expr_id_ != ast::k_invalid_expr &&
                            current_expr_id_ < expr_enum_ctor_variant_index_cache_.size()) {
                            expr_enum_ctor_variant_index_cache_[current_expr_id_] = vm.index;
                        }
                        if (current_expr_id_ != ast::k_invalid_expr &&
                            current_expr_id_ < expr_enum_ctor_tag_value_cache_.size()) {
                            expr_enum_ctor_tag_value_cache_[current_expr_id_] = vm.tag;
                        }
                        return enum_owner_type;
                    }
                }
            }

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
                    if (owner_sym.kind != sema::SymbolKind::kField &&
                        owner_sym.kind != sema::SymbolKind::kType) {
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
                        oss << "acts path owner must resolve to a struct/class/enum type in v0, got '"
                            << owner_sym.name << "'";
                        diag_(diag::Code::kTypeErrorGeneric, callee_expr.span, oss.str());
                        err_(callee_expr.span, oss.str());
                        check_all_arg_exprs_only();
                        return types_.error();
                    }
                    ensure_generic_acts_for_owner_(owner_t, callee_expr.span);

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

                            if (is_actor_type) {
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
                                    diag_(diag::Code::kActorCtorMissingInit, callee_expr.span, sym.name);
                                    err_(callee_expr.span, "actor constructor call requires init overload");
                                    check_all_arg_exprs_only();
                                    return types_.error();
                                }
                            } else if (is_class_type) {
                                ast::StmtId class_sid = ast::k_invalid_stmt;
                                if (auto it = class_decl_by_name_.find(sym.name); it != class_decl_by_name_.end()) {
                                    class_sid = it->second;
                                }

                                is_ctor_call = true;
                                ctor_owner_type = sym.declared_type;
                                fallback_ret = (ctor_owner_type == ty::kInvalidType) ? types_.error() : ctor_owner_type;
                                callee_name = sym.name;

                                if (class_sid != ast::k_invalid_stmt &&
                                    (size_t)class_sid < ast_.stmts().size() &&
                                    ast_.stmt(class_sid).decl_generic_param_count > 0) {
                                    if (explicit_call_type_args.empty()) {
                                        diag_(diag::Code::kGenericTypeArgInferenceFailed, callee_expr.span, sym.name);
                                        err_(callee_expr.span, "generic class constructor requires explicit type arguments");
                                        check_all_arg_exprs_only();
                                        return types_.error();
                                    }

                                    auto inst_sid = ensure_generic_class_instance_(class_sid, explicit_call_type_args, callee_expr.span);
                                    if (!inst_sid.has_value() ||
                                        *inst_sid == ast::k_invalid_stmt ||
                                        (size_t)(*inst_sid) >= ast_.stmts().size()) {
                                        check_all_arg_exprs_only();
                                        return types_.error();
                                    }

                                    const auto& inst_decl = ast_.stmt(*inst_sid);
                                    ctor_owner_type = inst_decl.type;
                                    fallback_ret = (ctor_owner_type == ty::kInvalidType) ? types_.error() : ctor_owner_type;

                                    auto qit = class_qualified_name_by_stmt_.find(*inst_sid);
                                    if (qit != class_qualified_name_by_stmt_.end()) {
                                        callee_name = qit->second;
                                    }

                                    const auto& kids = ast_.stmt_children();
                                    const uint64_t mb = inst_decl.stmt_begin;
                                    const uint64_t me = mb + inst_decl.stmt_count;
                                    if (mb <= kids.size() && me <= kids.size()) {
                                        for (uint32_t mi = inst_decl.stmt_begin; mi < inst_decl.stmt_begin + inst_decl.stmt_count; ++mi) {
                                            const ast::StmtId msid = kids[mi];
                                            if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                                            const auto& member = ast_.stmt(msid);
                                            if (member.kind != ast::StmtKind::kFnDecl) continue;
                                            if (member.name != "init") continue;
                                            overload_decl_ids.push_back(msid);
                                        }
                                    }

                                    // class constructor type-args were consumed at class-level, not function-level.
                                    explicit_call_type_args.clear();
                                } else {
                                    if (!explicit_call_type_args.empty()) {
                                        diag_(diag::Code::kGenericArityMismatch, callee_expr.span, "0",
                                              std::to_string(explicit_call_type_args.size()));
                                        err_(callee_expr.span, "non-generic class constructor call does not accept type arguments");
                                        check_all_arg_exprs_only();
                                        return types_.error();
                                    }

                                    std::string init_qname = sym.name;
                                    init_qname += "::init";
                                    auto fit = fn_decl_by_name_.find(init_qname);
                                    if (fit != fn_decl_by_name_.end()) {
                                        overload_decl_ids = fit->second;
                                    }
                                }
                                if (overload_decl_ids.empty()) {
                                    diag_(diag::Code::kClassCtorMissingInit, callee_expr.span, sym.name);
                                    err_(callee_expr.span, "class constructor call requires init overload");
                                    check_all_arg_exprs_only();
                                    return types_.error();
                                }
                            }
                        } else if (sym.kind == sema::SymbolKind::kFn) {
                            callee_name = sym.name;
                            if (sym.is_external) {
                                cimport_meta = parse_cimport_call_meta_(sym.external_payload);
                                if (cimport_meta.is_c_import) {
                                    is_cimport_call = true;
                                    cimport_callee_symbol = *sid;
                                }
                            }
                            auto it = fn_decl_by_name_.find(callee_name);
                            if (it != fn_decl_by_name_.end()) {
                                overload_decl_ids = it->second;
                            }
                        }
                    }
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

        const auto read_decay_borrow_local = [&](ty::TypeId t) -> ty::TypeId {
            if (t == ty::kInvalidType || t >= types_.count()) return t;
            const auto& tt = types_.get(t);
            if (tt.kind != ty::Kind::kBorrow) return t;
            return tt.elem;
        };

        auto is_c_variadic_abi_arg_type = [&](ty::TypeId t) -> bool {
            if (t == ty::kInvalidType || is_error_(t)) return false;
            t = read_decay_borrow_local(t);
            const auto& tt = types_.get(t);
            if (tt.kind == ty::Kind::kPtr) return true;
            if (tt.kind != ty::Kind::kBuiltin) return false;
            switch (tt.builtin) {
                case ty::Builtin::kBool:
                case ty::Builtin::kChar:
                case ty::Builtin::kI8:
                case ty::Builtin::kI16:
                case ty::Builtin::kI32:
                case ty::Builtin::kI64:
                case ty::Builtin::kI128:
                case ty::Builtin::kU8:
                case ty::Builtin::kU16:
                case ty::Builtin::kU32:
                case ty::Builtin::kU64:
                case ty::Builtin::kU128:
                case ty::Builtin::kISize:
                case ty::Builtin::kUSize:
                case ty::Builtin::kF32:
                case ty::Builtin::kF64:
                case ty::Builtin::kF128:
                case ty::Builtin::kNull:
                    return true;
                default:
                    return false;
            }
        };

        auto is_plain_string_literal_expr = [&](ast::ExprId eid) -> bool {
            if (eid == ast::k_invalid_expr || (size_t)eid >= ast_.exprs().size()) return false;
            const auto& ex = ast_.expr(eid);
            return ex.kind == ast::ExprKind::kStringLit && !ex.string_is_format;
        };

        auto is_c_char_ptr_type = [&](ty::TypeId t) -> bool {
            if (t == ty::kInvalidType || is_error_(t)) return false;
            t = read_decay_borrow_local(t);
            const auto& tt = types_.get(t);
            if (tt.kind != ty::Kind::kPtr || tt.elem == ty::kInvalidType) return false;
            const auto& et = types_.get(tt.elem);
            if (et.kind != ty::Kind::kBuiltin) return false;
            switch (et.builtin) {
                case ty::Builtin::kChar:
                case ty::Builtin::kI8:
                case ty::Builtin::kU8:
                    return true;
                default:
                    return false;
            }
        };

        auto is_infer_integer_type = [&](ty::TypeId t) -> bool {
            if (t == ty::kInvalidType || is_error_(t)) return false;
            t = read_decay_borrow_local(t);
            t = canonicalize_transparent_external_typedef_(t);
            if (t == ty::kInvalidType || t >= types_.count()) return false;
            const auto& tt = types_.get(t);
            return tt.kind == ty::Kind::kBuiltin && tt.builtin == ty::Builtin::kInferInteger;
        };

        auto check_cimport_call_with_positions =
            [&](const sema::Symbol& callee_sym,
                const std::vector<ast::ExprId>& arg_exprs,
                Span diag_span) -> ty::TypeId {
                if (callee_sym.declared_type == ty::kInvalidType ||
                    types_.get(callee_sym.declared_type).kind != ty::Kind::kFn) {
                    diag_(diag::Code::kTypeNotCallable, diag_span, callee_name);
                    err_(diag_span, "invalid C import callee signature");
                    return types_.error();
                }

                const ty::TypeId fn_t = callee_sym.declared_type;
                const auto& fn_sig = types_.get(fn_t);
                const uint32_t fixed_param_count = fn_sig.param_count;
                if (!cimport_meta.is_variadic) {
                    if (arg_exprs.size() != fixed_param_count) {
                        diag_(diag::Code::kTypeArgCountMismatch, diag_span,
                              std::to_string(fixed_param_count), std::to_string(arg_exprs.size()));
                        err_(diag_span, "non-variadic C call argument count mismatch");
                        return types_.error();
                    }
                } else if (arg_exprs.size() < fixed_param_count) {
                    diag_(diag::Code::kTypeArgCountMismatch, diag_span,
                          std::to_string(fixed_param_count), std::to_string(arg_exprs.size()));
                    err_(diag_span, "C variadic call requires at least fixed parameter count");
                    return types_.error();
                }

                for (uint32_t i = 0; i < fixed_param_count && i < arg_exprs.size(); ++i) {
                    const ast::ExprId arg_eid = arg_exprs[i];
                    const ty::TypeId expected = types_.fn_param_at(fn_t, i);
                    if (arg_eid == ast::k_invalid_expr || (size_t)arg_eid >= ast_.exprs().size()) {
                        err_(diag_span, "invalid C ABI call argument");
                        return types_.error();
                    }

                    const auto& arg_expr = ast_.expr(arg_eid);
                    if (arg_expr.kind == ast::ExprKind::kStringLit &&
                        arg_expr.string_is_format) {
                        diag_(diag::Code::kCAbiFormatStringForbidden, arg_expr.span);
                        err_(arg_expr.span, "format-string literal is forbidden in C ABI call");
                        return types_.error();
                    }

                    if (is_c_char_ptr_type(expected) &&
                        is_plain_string_literal_expr(arg_eid)) {
                        // C ABI char* 슬롯에서도 문자열 리터럴은 반드시 먼저 타입체크해서
                        // expr_types를 채운다. (SIR/OIR lowering에서 invalid type 누락 방지)
                        const ty::TypeId lit_ty = check_expr_(arg_eid);
                        if (is_error_(lit_ty)) return types_.error();
                        continue;
                    }
                    const CoercionPlan plan = classify_assign_with_coercion_(
                        AssignSite::CallArg, expected, arg_eid, ast_.expr(arg_eid).span);
                    if (!plan.ok) {
                        diag_(diag::Code::kTypeArgTypeMismatch, ast_.expr(arg_eid).span,
                              std::to_string(i), types_.to_string(expected),
                              type_for_user_diag_(plan.src_after, arg_eid));
                        err_(ast_.expr(arg_eid).span, "C call fixed-parameter type mismatch");
                        return types_.error();
                    }
                }

                if (cimport_meta.is_variadic) {
                    for (size_t ai = fixed_param_count; ai < arg_exprs.size(); ++ai) {
                        const ast::ExprId arg_eid = arg_exprs[ai];
                        if (arg_eid == ast::k_invalid_expr || (size_t)arg_eid >= ast_.exprs().size()) {
                            err_(diag_span, "invalid C ABI variadic argument");
                            return types_.error();
                        }
                        const auto& arg_expr = ast_.expr(arg_eid);
                        if (arg_expr.kind == ast::ExprKind::kStringLit &&
                            arg_expr.string_is_format) {
                            diag_(diag::Code::kCAbiFormatStringForbidden, arg_expr.span);
                            err_(arg_expr.span, "format-string literal is forbidden in C ABI call");
                            return types_.error();
                        }

                        if (is_plain_string_literal_expr(arg_eid)) {
                            // variadic 슬롯에서도 문자열 리터럴 type cache를 유지한다.
                            const ty::TypeId lit_ty = check_expr_(arg_eid);
                            if (is_error_(lit_ty)) return types_.error();
                            continue;
                        }

                        const ty::TypeId at = check_expr_(arg_eid);
                        ty::TypeId checked_ty = at;
                        if (is_infer_integer_type(checked_ty)) {
                            // C variadic v1 policy: unsuffixed integer literal defaults to C int(i32).
                            // This keeps printf("%d", 5) usable while preserving explicit typing elsewhere.
                            (void)resolve_infer_int_in_context_(arg_eid, types_.builtin(ty::Builtin::kI32));
                            checked_ty = check_expr_(arg_eid);
                        }
                        if (!is_c_variadic_abi_arg_type(checked_ty)) {
                            diag_(diag::Code::kCImportVariadicArgTypeUnsupported,
                                  ast_.expr(arg_eid).span, types_.to_string(checked_ty));
                            err_(ast_.expr(arg_eid).span, "unsupported type in C variadic argument");
                            return types_.error();
                        }
                    }
                }

                if (current_expr_id_ != ast::k_invalid_expr &&
                    current_expr_id_ < expr_external_callee_symbol_cache_.size() &&
                    cimport_callee_symbol != sema::SymbolTable::kNoScope) {
                    expr_external_callee_symbol_cache_[current_expr_id_] = cimport_callee_symbol;
                }
                return fn_sig.ret;
            };

        if (is_cimport_call && overload_decl_ids.empty()) {
            if (form != CallForm::kPositionalOnly) {
                diag_(diag::Code::kCAbiCallPositionalOnly, e.span);
                err_(e.span, "C ABI call currently supports positional arguments only");
                check_all_arg_exprs_only();
                return types_.error();
            }
            if (cimport_callee_symbol == sema::SymbolTable::kNoScope ||
                cimport_callee_symbol >= sym_.symbols().size()) {
                err_(e.span, "invalid C import callee symbol");
                return types_.error();
            }
            std::vector<ast::ExprId> arg_exprs{};
            arg_exprs.reserve(outside_positional.size());
            for (const auto* a : outside_positional) {
                if (a != nullptr) arg_exprs.push_back(a->expr);
            }
            return check_cimport_call_with_positions(sym_.symbol(cimport_callee_symbol), arg_exprs, e.span);
        }

        // ------------------------------------------------------------
        // 2.5) external acts default-method candidates (from export-index metadata)
        // ------------------------------------------------------------
        if (overload_decl_ids.empty() && !external_overload_methods.empty()) {
            ast::ExprId receiver_eid = ast::k_invalid_expr;
            if (e.a != ast::k_invalid_expr) {
                const auto& callee_expr = ast_.expr(e.a);
                if (callee_expr.kind == ast::ExprKind::kBinary &&
                    callee_expr.op == K::kDot &&
                    callee_expr.a != ast::k_invalid_expr) {
                    receiver_eid = callee_expr.a;
                }
            }
            if (receiver_eid == ast::k_invalid_expr) {
                diag_(diag::Code::kTypeErrorGeneric, e.span, "invalid external acts method call receiver");
                err_(e.span, "invalid external acts method call receiver");
                check_all_arg_exprs_only();
                return fallback_ret;
            }

            if (form != CallForm::kPositionalOnly) {
                diag_(diag::Code::kOverloadNoMatchingCall, e.span, callee_name, make_callsite_summary());
                err_(e.span, "external acts method call currently supports positional arguments only");
                check_all_arg_exprs_only();
                return fallback_ret;
            }

            uint32_t selected_external_sym = sema::SymbolTable::kNoScope;
            ty::TypeId selected_external_fn_type = ty::kInvalidType;

            for (const auto& cand : external_overload_methods) {
                if (cand.fn_symbol == sema::SymbolTable::kNoScope ||
                    cand.fn_symbol >= sym_.symbols().size()) {
                    continue;
                }
                const auto& fn_sym = sym_.symbol(cand.fn_symbol);
                const ty::TypeId fn_t = fn_sym.declared_type;
                if (fn_t == ty::kInvalidType || types_.get(fn_t).kind != ty::Kind::kFn) continue;
                const uint32_t total_cnt = types_.get(fn_t).param_count;
                if (total_cnt == 0) continue;

                const uint32_t expected_outside_positional = total_cnt - 1u; // receiver consumed implicitly
                if (outside_positional.size() != expected_outside_positional) continue;

                const CoercionPlan recv_plan = classify_assign_with_coercion_(
                    AssignSite::CallArg,
                    types_.fn_param_at(fn_t, 0),
                    receiver_eid,
                    ast_.expr(receiver_eid).span
                );
                if (!recv_plan.ok) continue;

                bool all_ok = true;
                for (size_t i = 0; i < outside_positional.size(); ++i) {
                    const ty::TypeId expected = types_.fn_param_at(fn_t, static_cast<uint32_t>(i + 1));
                    const CoercionPlan plan = classify_assign_with_coercion_(
                        AssignSite::CallArg,
                        expected,
                        outside_positional[i]->expr,
                        outside_positional[i]->span
                    );
                    if (!plan.ok) {
                        all_ok = false;
                        break;
                    }
                }
                if (!all_ok) continue;

                selected_external_sym = cand.fn_symbol;
                selected_external_fn_type = fn_t;
                break;
            }

            if (selected_external_sym == sema::SymbolTable::kNoScope ||
                selected_external_fn_type == ty::kInvalidType) {
                diag_(diag::Code::kOverloadNoMatchingCall, e.span, callee_name, make_callsite_summary());
                err_(e.span, "no matching external acts method overload");
                check_all_arg_exprs_only();
                return fallback_ret;
            }

            if (current_expr_id_ != ast::k_invalid_expr &&
                current_expr_id_ < expr_external_callee_symbol_cache_.size()) {
                expr_external_callee_symbol_cache_[current_expr_id_] = selected_external_sym;
            }
            if (current_expr_id_ != ast::k_invalid_expr &&
                current_expr_id_ < expr_external_receiver_expr_cache_.size()) {
                expr_external_receiver_expr_cache_[current_expr_id_] = receiver_eid;
            }
            return types_.get(selected_external_fn_type).ret;
        }

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
                    if (!pos_params.empty() || named_params.empty()) return false;
                    for (const auto& pair : labeled_by_label) {
                        if (named_by_label.find(pair.first) == named_by_label.end()) {
                            return false;
                        }
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
                if (form == CallForm::kLabeledOnly && !pos_params.empty()) {
                    diag_(diag::Code::kCallLabeledNotAllowedForPositionalFn, e.span);
                    err_(e.span, "labeled-call form is not allowed for positional-only function");
                    check_all_arg_exprs_only();
                    return fallback_ret;
                }
                if (form == CallForm::kPositionalOnly &&
                    pos_params.empty() &&
                    !named_params.empty() &&
                    !outside_positional.empty()) {
                    diag_(diag::Code::kCallPositionalNotAllowedForNamedGroupFn, e.span);
                    err_(e.span, "positional-call form is not allowed for named-group-only function");
                    check_all_arg_exprs_only();
                    return fallback_ret;
                }
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

        auto fn_sig_same = [&](const ast::Stmt& a, const ast::Stmt& b) -> bool {
            if (a.kind != ast::StmtKind::kFnDecl || b.kind != ast::StmtKind::kFnDecl) return false;
            if (a.name != b.name) return false;
            if (a.param_count != b.param_count) return false;
            if (a.positional_param_count != b.positional_param_count) return false;
            if (a.fn_ret != b.fn_ret) return false;
            for (uint32_t i = 0; i < a.param_count; ++i) {
                const auto& ap = ast_.params()[a.param_begin + i];
                const auto& bp = ast_.params()[b.param_begin + i];
                if (ap.type != bp.type || ap.is_self != bp.is_self || ap.self_kind != bp.self_kind) return false;
            }
            return true;
        };

        auto proto_effective_required_empty = [&](ast::StmtId proto_sid) -> bool {
            if (proto_sid == ast::k_invalid_stmt || (size_t)proto_sid >= ast_.stmts().size()) return false;
            std::vector<ast::StmtId> reqs;
            std::vector<ast::StmtId> provs;
            std::unordered_set<ast::StmtId> visiting;
            auto collect = [&](auto&& self, ast::StmtId cur_sid) -> void {
                if (cur_sid == ast::k_invalid_stmt || (size_t)cur_sid >= ast_.stmts().size()) return;
                if (!visiting.insert(cur_sid).second) return;
                const auto& cur = ast_.stmt(cur_sid);
                if (cur.kind != ast::StmtKind::kProtoDecl) return;
                const auto& refs = ast_.path_refs();
                const uint64_t ib = cur.decl_path_ref_begin;
                const uint64_t ie = ib + cur.decl_path_ref_count;
                if (ib <= refs.size() && ie <= refs.size()) {
                    for (uint32_t i = cur.decl_path_ref_begin; i < cur.decl_path_ref_begin + cur.decl_path_ref_count; ++i) {
                        if (auto base_sid = resolve_proto_decl_from_path_ref_(refs[i], e.span)) {
                            self(self, *base_sid);
                        }
                    }
                }
                const auto& kids = ast_.stmt_children();
                const uint64_t mb = cur.stmt_begin;
                const uint64_t me = mb + cur.stmt_count;
                if (mb <= kids.size() && me <= kids.size()) {
                    for (uint32_t i = cur.stmt_begin; i < cur.stmt_begin + cur.stmt_count; ++i) {
                        const auto msid = kids[i];
                        if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                        const auto& m = ast_.stmt(msid);
                        if (m.kind != ast::StmtKind::kFnDecl) continue;
                        if (m.proto_fn_role == ast::ProtoFnRole::kRequire) reqs.push_back(msid);
                        if (m.proto_fn_role == ast::ProtoFnRole::kProvide && m.a != ast::k_invalid_stmt) provs.push_back(msid);
                    }
                }
            };
            collect(collect, proto_sid);
            for (const auto req_sid : reqs) {
                if (req_sid == ast::k_invalid_stmt || (size_t)req_sid >= ast_.stmts().size()) continue;
                const auto& req = ast_.stmt(req_sid);
                bool satisfied = false;
                for (const auto prov_sid : provs) {
                    if (prov_sid == ast::k_invalid_stmt || (size_t)prov_sid >= ast_.stmts().size()) continue;
                    if (fn_sig_same(req, ast_.stmt(prov_sid))) {
                        satisfied = true;
                        break;
                    }
                }
                if (!satisfied) return false;
            }
            return true;
        };

        auto type_satisfies_proto_constraint = [&](ty::TypeId concrete_t, ast::StmtId proto_sid) -> bool {
            if (proto_sid == ast::k_invalid_stmt) return false;
            if (!evaluate_proto_require_at_apply_(proto_sid, concrete_t, e.span,
                                                  /*emit_unsatisfied_diag=*/false,
                                                  /*emit_shape_diag=*/false)) {
                return false;
            }
            if (proto_effective_required_empty(proto_sid)) return true;

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
                    if (!c.positional.empty() || c.named.empty()) {
                        infer_ok = false;
                    } else {
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
                if (!c.positional.empty() || c.named.empty()) return false;
                for (const auto& pair : labeled_by_label) {
                    if (c.named_by_label.find(pair.first) == c.named_by_label.end()) {
                        return false;
                    }
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
            bool has_positional_shape = false;
            bool has_named_group_only_shape = false;
            for (const auto idx : filtered) {
                const auto& c = candidates[idx];
                if (c.positional.empty() && !c.named.empty()) {
                    has_named_group_only_shape = true;
                } else {
                    has_positional_shape = true;
                }
            }
            if (form == CallForm::kLabeledOnly &&
                !has_named_group_only_shape &&
                has_positional_shape) {
                diag_(diag::Code::kCallLabeledNotAllowedForPositionalFn, e.span);
                err_(e.span, "labeled-call form is not allowed for positional-only function");
                check_all_arg_exprs_only();
                return fallback_ret;
            }
            if (form == CallForm::kPositionalOnly &&
                !has_positional_shape &&
                has_named_group_only_shape &&
                !outside_positional.empty()) {
                diag_(diag::Code::kCallPositionalNotAllowedForNamedGroupFn, e.span);
                err_(e.span, "positional-call form is not allowed for named-group-only function");
                check_all_arg_exprs_only();
                return fallback_ret;
            }
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

        bool selected_is_c_abi = false;
        if (selected_decl_sid != ast::k_invalid_stmt &&
            (size_t)selected_decl_sid < ast_.stmts().size()) {
            const auto& selected_decl = ast_.stmt(selected_decl_sid);
            selected_is_c_abi =
                (selected_decl.kind == ast::StmtKind::kFnDecl &&
                 selected_decl.link_abi == ast::LinkAbi::kC);
        }

        if (selected_is_c_abi && form != CallForm::kPositionalOnly) {
            diag_(diag::Code::kCAbiCallPositionalOnly, e.span);
            err_(e.span, "C ABI call currently supports positional arguments only");
            check_all_arg_exprs_only();
            return types_.error();
        }

        if (selected_decl_sid != ast::k_invalid_stmt && (size_t)selected_decl_sid < ast_.stmts().size()) {
            const auto& selected_decl = ast_.stmt(selected_decl_sid);
            if (selected_decl.kind == ast::StmtKind::kFnDecl &&
                selected_decl.is_throwing &&
                !in_try_expr_context_ &&
                !fn_ctx_.is_throwing) {
                diag_(diag::Code::kTypeErrorGeneric, e.span,
                      "direct call to throwing function is not allowed here; wrap the call with 'try <call>'");
                err_(e.span, "non-throwing function must use try expression for throwing call");
                check_all_arg_exprs_only();
                return types_.error();
            }
        }

        const auto check_arg_against_param_final = [&](const ast::Arg& a, const ParamInfo& p) {
            if (a.expr == ast::k_invalid_expr) {
                diag_(diag::Code::kTypeArgTypeMismatch, a.span,
                    std::to_string(p.decl_index), types_.to_string(p.type), "<missing>");
                err_(a.span, "argument type mismatch for parameter '" + p.name + "'");
                return;
            }

            if (selected_is_c_abi &&
                (size_t)a.expr < ast_.exprs().size()) {
                const auto& ax = ast_.expr(a.expr);
                if (ax.kind == ast::ExprKind::kStringLit && ax.string_is_format) {
                    diag_(diag::Code::kCAbiFormatStringForbidden, a.span);
                    err_(a.span, "format-string literal is forbidden in C ABI call");
                    return;
                }
                if (is_c_char_ptr_type(p.type) && is_plain_string_literal_expr(a.expr)) {
                    return;
                }
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
            for (const auto& p : selected.named) {
                auto it = labeled_by_label.find(p.name);
                if (it != labeled_by_label.end()) {
                    check_arg_against_param_final(*it->second, p);
                }
            }
        }

        const auto consume_arg_if_moved = [&](const ast::Arg* arg, const ParamInfo& p) {
            if (arg == nullptr || arg->expr == ast::k_invalid_expr) return;
            mark_expr_move_consumed_(arg->expr, p.type, arg->span);
        };

        if (form == CallForm::kPositionalOnly) {
            for (size_t i = 0; i < outside_positional.size() && i < selected.positional.size(); ++i) {
                consume_arg_if_moved(outside_positional[i], selected.positional[i]);
            }
        } else if (form == CallForm::kLabeledOnly) {
            for (const auto& p : selected.named) {
                auto it = labeled_by_label.find(p.name);
                if (it != labeled_by_label.end()) {
                    consume_arg_if_moved(it->second, p);
                }
            }
        }

        if (selected.inject_receiver &&
            selected.receiver_decl_index != 0xFFFF'FFFFu &&
            selected_decl_sid != ast::k_invalid_stmt &&
            (size_t)selected_decl_sid < ast_.stmts().size()) {
            const auto& decl = ast_.stmt(selected_decl_sid);
            const auto& callee_expr = ast_.expr(e.a);
            if (decl.kind == ast::StmtKind::kFnDecl &&
                selected.receiver_decl_index < decl.param_count) {
                const auto& recv = ast_.params()[decl.param_begin + selected.receiver_decl_index];
                if (recv.is_self && recv.self_kind == ast::SelfReceiverKind::kMove &&
                    callee_expr.kind == ast::ExprKind::kBinary &&
                    callee_expr.a != ast::k_invalid_expr) {
                    mark_expr_move_consumed_(callee_expr.a, recv.type, callee_expr.span);
                }
            }
        }

        return selected.ret;
    }
