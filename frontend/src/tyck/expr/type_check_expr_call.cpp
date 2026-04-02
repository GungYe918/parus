// frontend/src/tyck/type_check_expr_call_cast.cpp
#include <parus/tyck/TypeCheck.hpp>
#include <parus/common/ModulePath.hpp>
#include <parus/cimport/TypeReprNormalize.hpp>
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
        std::string_view parse_impl_binding_mode_(std::string_view payload) {
            if (!payload.starts_with("parus_impl_binding|key=")) return {};
            const size_t mode_pos = payload.find("|mode=");
            if (mode_pos == std::string_view::npos) return "compiler";
            std::string_view mode = payload.substr(mode_pos + std::string_view("|mode=").size());
            if (const size_t next = mode.find('|'); next != std::string_view::npos) {
                mode = mode.substr(0, next);
            }
            return mode;
        }

        std::string payload_unescape_value_(std::string_view raw) {
            auto hex_value = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
                if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
                return -1;
            };

            std::string out{};
            out.reserve(raw.size());
            for (size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '%' && i + 2 < raw.size()) {
                    const int hi = hex_value(raw[i + 1]);
                    const int lo = hex_value(raw[i + 2]);
                    if (hi >= 0 && lo >= 0) {
                        out.push_back(static_cast<char>((hi << 4) | lo));
                        i += 2;
                        continue;
                    }
                }
                out.push_back(raw[i]);
            }
            return out;
        }

        struct ExternalGenericConstraintMeta {
            enum class Kind : uint8_t {
                kProto = 0,
                kTypeEq,
            };
            Kind kind = Kind::kProto;
            std::string lhs{};
            std::string rhs{};
        };

        struct ExternalGenericDeclMeta {
            std::vector<std::string> params{};
            std::vector<ExternalGenericConstraintMeta> constraints{};
            std::vector<std::pair<std::string, std::string>> impl_protos{};
        };

        ExternalGenericDeclMeta parse_external_generic_decl_meta_(std::string_view payload) {
            ExternalGenericDeclMeta out{};
            size_t pos = 0;
            while (pos < payload.size()) {
                size_t next = payload.find('|', pos);
                if (next == std::string_view::npos) next = payload.size();
                const std::string_view part = payload.substr(pos, next - pos);
                if (part.starts_with("gparam=")) {
                    out.params.push_back(payload_unescape_value_(part.substr(std::string_view("gparam=").size())));
                } else if (part.starts_with("gconstraint=")) {
                    const std::string_view body = part.substr(std::string_view("gconstraint=").size());
                    const size_t comma1 = body.find(',');
                    const size_t comma2 = (comma1 == std::string_view::npos) ? std::string_view::npos : body.find(',', comma1 + 1);
                    if (comma1 != std::string_view::npos && comma2 != std::string_view::npos) {
                        ExternalGenericConstraintMeta cc{};
                        const std::string_view kind = body.substr(0, comma1);
                        cc.kind = (kind == "type_eq")
                            ? ExternalGenericConstraintMeta::Kind::kTypeEq
                            : ExternalGenericConstraintMeta::Kind::kProto;
                        cc.lhs = payload_unescape_value_(body.substr(comma1 + 1, comma2 - comma1 - 1));
                        cc.rhs = payload_unescape_value_(body.substr(comma2 + 1));
                        out.constraints.push_back(std::move(cc));
                    }
                } else if (part.starts_with("impl_proto=")) {
                    const std::string body = payload_unescape_value_(
                        part.substr(std::string_view("impl_proto=").size())
                    );
                    const size_t split = body.find('@');
                    if (split == std::string::npos) {
                        out.impl_protos.emplace_back(body, std::string{});
                    } else {
                        out.impl_protos.emplace_back(body.substr(0, split), body.substr(split + 1));
                    }
                }
                if (next == payload.size()) break;
                pos = next + 1;
            }
            return out;
        }

        struct CImportCallMeta {
            bool is_c_import = false;
            bool is_c_decl = false;
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
            if (payload.starts_with("parus_c_import|")) {
                out.is_c_import = true;
                out.is_c_abi = true;
            } else if (payload.starts_with("parus_c_abi_decl|")) {
                out.is_c_decl = true;
                out.is_c_abi = true;
            } else {
                return out;
            }

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

        bool parse_external_throwing_payload_(std::string_view payload) {
            if (payload.starts_with("parus_c_import|") ||
                payload.starts_with("parus_c_abi_decl|")) {
                return false;
            }
            size_t pos = 0;
            while (pos < payload.size()) {
                size_t next = payload.find('|', pos);
                if (next == std::string_view::npos) next = payload.size();
                const std::string_view part = payload.substr(pos, next - pos);
                if (part == "throwing=1") return true;
                if (next == payload.size()) break;
                pos = next + 1;
            }
            return false;
        }
    } // namespace

    ty::TypeId TypeChecker::check_expr_call_(ast::Expr e) {
        // e.a = callee, args slice in e.arg_begin/e.arg_count
        const ast::ExprId call_expr_id = current_expr_id_;
        if (call_expr_id != ast::k_invalid_expr &&
            call_expr_id < expr_overload_target_cache_.size()) {
            expr_overload_target_cache_[call_expr_id] = ast::k_invalid_stmt;
        }
        if (call_expr_id != ast::k_invalid_expr &&
            call_expr_id < expr_ctor_owner_type_cache_.size()) {
            expr_ctor_owner_type_cache_[call_expr_id] = ty::kInvalidType;
        }
        if (call_expr_id != ast::k_invalid_expr &&
            call_expr_id < expr_enum_ctor_owner_type_cache_.size()) {
            expr_enum_ctor_owner_type_cache_[call_expr_id] = ty::kInvalidType;
        }
        if (call_expr_id != ast::k_invalid_expr &&
            call_expr_id < expr_enum_ctor_variant_index_cache_.size()) {
            expr_enum_ctor_variant_index_cache_[call_expr_id] = 0xFFFF'FFFFu;
        }
        if (call_expr_id != ast::k_invalid_expr &&
            call_expr_id < expr_enum_ctor_tag_value_cache_.size()) {
            expr_enum_ctor_tag_value_cache_[call_expr_id] = 0;
        }
        if (call_expr_id != ast::k_invalid_expr &&
            call_expr_id < expr_external_callee_symbol_cache_.size()) {
            expr_external_callee_symbol_cache_[call_expr_id] = sema::SymbolTable::kNoScope;
        }
        if (call_expr_id != ast::k_invalid_expr &&
            call_expr_id < expr_external_callee_type_cache_.size()) {
            expr_external_callee_type_cache_[call_expr_id] = ty::kInvalidType;
        }
        if (call_expr_id != ast::k_invalid_expr &&
            call_expr_id < expr_call_fn_type_cache_.size()) {
            expr_call_fn_type_cache_[call_expr_id] = ty::kInvalidType;
        }
        if (call_expr_id != ast::k_invalid_expr &&
            call_expr_id < expr_external_receiver_expr_cache_.size()) {
            expr_external_receiver_expr_cache_[call_expr_id] = ast::k_invalid_expr;
        }
        if (call_expr_id != ast::k_invalid_expr &&
            call_expr_id < expr_array_family_call_kind_cache_.size()) {
            expr_array_family_call_kind_cache_[call_expr_id] =
                static_cast<uint8_t>(ArrayFamilyCallKind::kNone);
        }
        if (call_expr_id != ast::k_invalid_expr &&
            call_expr_id < expr_call_is_throwing_cache_.size()) {
            expr_call_is_throwing_cache_[call_expr_id] = 0u;
        }
        if (call_expr_id != ast::k_invalid_expr &&
            call_expr_id < expr_call_is_c_abi_cache_.size()) {
            expr_call_is_c_abi_cache_[call_expr_id] = 0u;
        }
        if (call_expr_id != ast::k_invalid_expr &&
            call_expr_id < expr_call_is_c_variadic_cache_.size()) {
            expr_call_is_c_variadic_cache_[call_expr_id] = 0u;
        }
        if (call_expr_id != ast::k_invalid_expr &&
            call_expr_id < expr_call_c_callconv_cache_.size()) {
            expr_call_c_callconv_cache_[call_expr_id] = ty::CCallConv::kDefault;
        }
        if (call_expr_id != ast::k_invalid_expr &&
            call_expr_id < expr_call_c_fixed_param_count_cache_.size()) {
            expr_call_c_fixed_param_count_cache_[call_expr_id] = 0u;
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
        uint32_t direct_ident_symbol = sema::SymbolTable::kNoScope;
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
                        has_default_flags.empty() ? nullptr : has_default_flags.data(),
                        types_.fn_is_c_abi(t),
                        types_.fn_is_c_variadic(t),
                        types_.fn_callconv(t),
                        types_.fn_is_throwing(t)
                    );
                }
                default:
                    return t;
            }
        };

        auto trailing_name_segment_ = [](std::string_view name) -> std::string_view {
            const size_t pos = name.rfind("::");
            return (pos == std::string_view::npos) ? name : name.substr(pos + 2);
        };

        auto visible_external_fn_name_ = [](std::string_view name) -> std::string {
            constexpr std::string_view marker = "@@extovl$";
            const size_t pos = name.find(marker);
            if (pos == std::string_view::npos) return std::string(name);
            return std::string(name.substr(0, pos));
        };

        auto collect_imported_template_overloads_for_external_symbol_ = [&](const sema::Symbol& sym) {
            std::vector<ast::StmtId> out{};
            std::unordered_set<ast::StmtId> seen{};
            auto add_sid = [&](ast::StmtId sid) {
                if (sid == ast::k_invalid_stmt) return;
                if (!seen.insert(sid).second) return;
                out.push_back(sid);
            };
            auto add_name = [&](std::string_view candidate) {
                if (candidate.empty()) return;
                for (const auto sid : lookup_imported_fn_templates_by_name_(candidate)) {
                    add_sid(sid);
                }
            };

            if (!sym.link_name.empty()) {
                if (auto sid = lookup_imported_fn_template_by_link_name_(sym.link_name);
                    sid.has_value()) {
                    add_sid(*sid);
                }
            }

            add_name(sym.name);
            const std::string visible = visible_external_fn_name_(sym.name);
            add_name(visible);

            const std::string current_bundle = current_bundle_name_();
            const std::string current_head =
                parus::normalize_core_public_module_head(current_bundle, current_module_head_());
            const std::string symbol_head =
                parus::normalize_core_public_module_head(sym.decl_bundle_name, sym.decl_module_head);
            const auto candidates = parus::candidate_names_for_external_export(
                sym.name,
                symbol_head,
                sym.decl_bundle_name,
                current_head
            );
            for (const auto& candidate : candidates) {
                add_name(candidate);
            }

            if (visible.starts_with("core::")) {
                add_name(visible.substr(std::string("core::").size()));
            } else if (sym.decl_bundle_name == "core") {
                add_name(std::string("core::") + visible);
            }

            std::sort(out.begin(), out.end());
            return out;
        };

        auto is_external_free_fn_candidate_ = [&](const sema::Symbol& ss) -> bool {
            if (ss.kind != sema::SymbolKind::kFn) return false;
            if (parse_impl_binding_mode_(ss.external_payload) == "compiler") return false;
            const bool has_external_generic_meta =
                ss.external_payload.find("parus_generic_decl") != std::string::npos;
            const bool is_hidden_external_overload =
                ss.name.find("@@extovl$") != std::string::npos;
            const std::string current_bundle = current_bundle_name_();
            const bool bundle_mismatch_external =
                !ss.decl_bundle_name.empty() &&
                (current_bundle.empty() || ss.decl_bundle_name != current_bundle);
            return ss.is_external ||
                   has_external_generic_meta ||
                   is_hidden_external_overload ||
                   bundle_mismatch_external;
        };

        auto collect_external_fn_candidates_for_name_ = [&](std::string_view raw_name) {
            std::vector<uint32_t> out{};
            std::string lookup(raw_name);
            if (auto rewritten = rewrite_imported_path_(lookup)) {
                lookup = *rewritten;
            }
            const std::string visible_lookup = visible_external_fn_name_(lookup);
            for (uint32_t sid = 0; sid < sym_.symbols().size(); ++sid) {
                const auto& ss = sym_.symbol(sid);
                if (!is_external_free_fn_candidate_(ss)) continue;
                if (visible_external_fn_name_(ss.name) != visible_lookup) continue;
                out.push_back(sid);
            }
            return out;
        };

        auto cache_external_callee_ = [&](uint32_t sid, ty::TypeId fn_t = ty::kInvalidType) {
            if (call_expr_id != ast::k_invalid_expr &&
                call_expr_id < expr_external_callee_symbol_cache_.size()) {
                expr_external_callee_symbol_cache_[call_expr_id] = sid;
            }
            if (call_expr_id != ast::k_invalid_expr &&
                call_expr_id < expr_external_callee_type_cache_.size()) {
                expr_external_callee_type_cache_[call_expr_id] = fn_t;
            }
            if (call_expr_id != ast::k_invalid_expr &&
                call_expr_id < expr_call_fn_type_cache_.size()) {
                expr_call_fn_type_cache_[call_expr_id] = fn_t;
            }
            if (call_expr_id != ast::k_invalid_expr &&
                call_expr_id < expr_call_is_throwing_cache_.size()) {
                expr_call_is_throwing_cache_[call_expr_id] =
                    (fn_t != ty::kInvalidType && types_.fn_is_throwing(fn_t)) ? 1u : 0u;
            }
        };

        auto cache_call_fn_type_ = [&](ty::TypeId fn_t) {
            if (call_expr_id != ast::k_invalid_expr &&
                call_expr_id < expr_call_fn_type_cache_.size()) {
                expr_call_fn_type_cache_[call_expr_id] = fn_t;
            }
            if (call_expr_id != ast::k_invalid_expr &&
                call_expr_id < expr_call_is_throwing_cache_.size()) {
                expr_call_is_throwing_cache_[call_expr_id] =
                    (fn_t != ty::kInvalidType && types_.fn_is_throwing(fn_t)) ? 1u : 0u;
            }
        };

        auto is_core_module_fn_ = [&](uint32_t sid, std::string_view module_head, std::string_view leaf) -> bool {
            if (sid == sema::SymbolTable::kNoScope || sid >= sym_.symbols().size()) return false;
            const auto& ss = sym_.symbol(sid);
            const bool module_match =
                ss.decl_module_head == module_head ||
                ss.decl_module_head == ("core::" + std::string(module_head));
            return ss.kind == sema::SymbolKind::kFn &&
                   ss.decl_bundle_name == "core" &&
                   module_match &&
                   trailing_name_segment_(ss.name) == leaf;
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
                const bool recv_rewritten = apply_imported_path_rewrite_(recv_lookup);
                if (auto recv_sid = recv_rewritten ? sym_.lookup(recv_lookup) : lookup_symbol_(recv_lookup)) {
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
                while (owner_t != ty::kInvalidType) {
                    const auto& ot = types_.get(owner_t);
                    if ((ot.kind == ty::Kind::kBorrow || ot.kind == ty::Kind::kEscape) &&
                        ot.elem != ty::kInvalidType) {
                        owner_t = ot.elem;
                        continue;
                    }
                    break;
                }
            }

            if (auto inst_sid = ensure_generic_class_instance_from_type_(owner_t, member_span)) {
                const auto& inst = ast_.stmt(*inst_sid);
                if (inst.kind == ast::StmtKind::kClassDecl && inst.type != ty::kInvalidType) {
                    owner_t = inst.type;
                }
            }

            (void)ensure_generic_field_instance_from_type_(owner_t, member_span);
            ensure_generic_acts_for_owner_(owner_t, member_span);
            return owner_t;
        };

        auto owner_base_name_matches = [&](std::string_view lhs, std::string_view rhs) -> bool {
            if (lhs == rhs) return true;
            auto suffix_match = [](std::string_view full, std::string_view suffix) -> bool {
                if (full.size() <= suffix.size() + 2u) return false;
                if (!full.ends_with(suffix)) return false;
                const size_t split = full.size() - suffix.size();
                return full[split - 1] == ':' && full[split - 2] == ':';
            };
            return suffix_match(lhs, rhs) || suffix_match(rhs, lhs);
        };

        auto cache_array_family_call_ = [&](ArrayFamilyCallKind kind) {
            if (call_expr_id != ast::k_invalid_expr &&
                call_expr_id < expr_array_family_call_kind_cache_.size()) {
                expr_array_family_call_kind_cache_[call_expr_id] =
                    static_cast<uint8_t>(kind);
            }
        };

        struct ArrayFamilyMethodResult {
            bool matched = false;
            bool ok = false;
            ty::TypeId result_type = ty::kInvalidType;
        };

        auto try_check_array_family_method_ = [&](ast::ExprId recv_eid,
                                                  ty::TypeId recv_t,
                                                  std::string_view member_name,
                                                  Span member_span) -> ArrayFamilyMethodResult {
            ArrayFamilyMethodResult out{};
            if (member_name != "swap" &&
                member_name != "replace" &&
                member_name != "take" &&
                member_name != "put") {
                return out;
            }
            if (recv_t == ty::kInvalidType || is_error_(recv_t) || recv_t >= types_.count()) {
                return out;
            }

            auto add_array_method_diag = [&](Span primary_span,
                                             Span label_span,
                                             std::string primary,
                                             std::string label,
                                             std::string note,
                                             std::string help) {
                diag::Diagnostic d(diag::Severity::kError, diag::Code::kTypeErrorGeneric, primary_span);
                d.add_arg(std::move(primary));
                d.add_label(label_span, std::move(label));
                if (!note.empty()) d.add_note(std::move(note));
                if (!help.empty()) d.add_help(std::move(help));
                if (diag_bag_) diag_bag_->add(std::move(d));
            };
            auto require_positional_arity = [&](size_t expected, std::string_view sig) -> bool {
                if (form == CallForm::kPositionalOnly &&
                    outside_positional.size() == expected &&
                    outside_labeled.empty()) {
                    return true;
                }
                add_array_method_diag(
                    e.span,
                    member_span,
                    "compiler-owned sized array method `" + std::string(member_name) +
                        "` expects " + std::string(sig),
                    "this method uses fixed positional arguments on sized arrays",
                    "this round exposes sized owner-array container operations as compiler-owned array family methods",
                    "call `" + std::string(member_name) + std::string(sig) + "` on a writable sized array receiver");
                err_(e.span, "invalid compiler-owned sized array method shape");
                return false;
            };
            auto check_index_arg_ = [&](const ast::Arg* arg, std::string_view which) -> bool {
                if (arg == nullptr || arg->expr == ast::k_invalid_expr) {
                    add_array_method_diag(
                        e.span,
                        member_span,
                        "compiler-owned sized array method `" + std::string(member_name) +
                            "` requires an integer index argument",
                        std::string(which) + " index is missing here",
                        "sized array container methods operate on concrete element indices",
                        "pass an integer index such as `0usize` or `i`");
                    err_(member_span, "array method index argument is missing");
                    return false;
                }
                ty::TypeId idx_t = check_expr_(arg->expr);
                if (!is_error_(idx_t)) {
                    const auto& itt = types_.get(idx_t);
                    if (itt.kind == ty::Kind::kBuiltin &&
                        itt.builtin == ty::Builtin::kInferInteger) {
                        (void)resolve_infer_int_in_context_(arg->expr, types_.builtin(ty::Builtin::kUSize));
                        idx_t = check_expr_(arg->expr);
                    }
                }
                if (!is_error_(idx_t) && !is_index_int_type_(idx_t)) {
                    add_array_method_diag(
                        e.span,
                        arg->span,
                        "compiler-owned sized array method `" + std::string(member_name) +
                            "` requires integer indices",
                        "this argument has type `" + types_.to_string(idx_t) + "`",
                        "sized array container methods follow the same indexing rules as `arr[i]` in v0",
                        "use an integer/`usize` index expression here");
                    err_(arg->span, "array method index must be integer type in v0");
                    return false;
                }
                return !is_error_(idx_t);
            };
            auto require_writable_array_place_ = [&](bool forbid_global_like) -> std::optional<uint32_t> {
                if (!is_place_expr_(recv_eid)) {
                    add_array_method_diag(
                        e.span,
                        member_span,
                        "compiler-owned sized array method `" + std::string(member_name) +
                            "` requires a writable sized array place receiver",
                        "this receiver is not a writable place",
                        "array family container methods mutate the receiver in place",
                        "store the sized array in a mutable local/field and call the method on that place");
                    err_(member_span, "array method receiver must be a writable place");
                    return std::nullopt;
                }
                const auto root = root_place_symbol_(recv_eid);
                if (!root.has_value() || !is_mutable_symbol_(*root)) {
                    add_array_method_diag(
                        e.span,
                        member_span,
                        "compiler-owned sized array method `" + std::string(member_name) +
                            "` requires a mutable receiver",
                        "this sized array receiver is not writable",
                        "container mutation rewrites the receiver's element cells in place",
                        "make the root binding mutable, for example `set mut arr = ...` or use `mut self`");
                    err_(member_span, "array method receiver must be mutable");
                    return std::nullopt;
                }
                if (!ensure_symbol_readable_(*root, member_span)) {
                    return std::nullopt;
                }
                if (forbid_global_like && is_global_like_symbol_(*root)) {
                    diag_(diag::Code::kMoveFromGlobalOrStaticForbidden, member_span, sym_.symbol(*root).name);
                    err_(member_span, "move from global/static owner array is not allowed");
                    return std::nullopt;
                }
                return root;
            };

            const auto& recv_tt = types_.get(recv_t);
            if (recv_tt.kind != ty::Kind::kArray) {
                return out;
            }
            out.matched = true;

            if (explicit_call_type_args.size() > 0) {
                diag_(diag::Code::kGenericArityMismatch, e.span, "0",
                      std::to_string(explicit_call_type_args.size()));
                err_(e.span, "compiler-owned sized array methods do not accept explicit type arguments");
                out.ok = false;
                out.result_type = types_.error();
                return out;
            }

            if (!recv_tt.array_has_size) {
                add_array_method_diag(
                    e.span,
                    member_span,
                    "compiler-owned array family methods are only available on sized arrays `T[N]` in this round",
                    "this receiver has unsized array/view type `" + types_.to_string(recv_t) + "`",
                    "unsized views remain non-owning and do not get the compiler-owned owner-container method surface",
                    "use a sized array such as `T[N]`, `(~T)[N]`, or `((~T)?)[N]`, or move this operation into a storage-safe named aggregate");
                err_(member_span, "array family methods require sized array receivers");
                out.ok = false;
                out.result_type = types_.error();
                return out;
            }

            const ty::TypeId elem_t = recv_tt.elem;
            const bool elem_is_escape =
                elem_t != ty::kInvalidType &&
                elem_t < types_.count() &&
                types_.get(elem_t).kind == ty::Kind::kEscape;
            const bool elem_is_optional_escape =
                elem_t != ty::kInvalidType &&
                elem_t < types_.count() &&
                types_.get(elem_t).kind == ty::Kind::kOptional &&
                types_.get(elem_t).elem != ty::kInvalidType &&
                types_.get(elem_t).elem < types_.count() &&
                types_.get(types_.get(elem_t).elem).kind == ty::Kind::kEscape;
            const bool array_contains_escape = type_contains_escape_(recv_t);

            if (member_name == "swap") {
                if (!require_positional_arity(2u, "(i, j)")) {
                    out.ok = false;
                    out.result_type = types_.error();
                    return out;
                }
                if (!check_index_arg_(outside_positional[0], "first") ||
                    !check_index_arg_(outside_positional[1], "second")) {
                    out.ok = false;
                    out.result_type = types_.error();
                    return out;
                }
                const auto root = require_writable_array_place_(/*forbid_global_like=*/array_contains_escape);
                if (!root.has_value()) {
                    out.ok = false;
                    out.result_type = types_.error();
                    return out;
                }
                mark_symbol_initialized_(*root);
                cache_array_family_call_(ArrayFamilyCallKind::kSwap);
                out.ok = true;
                out.result_type = types_.builtin(ty::Builtin::kUnit);
                return out;
            }

            if (member_name == "replace") {
                if (!elem_is_escape) {
                    const std::string help =
                        elem_is_optional_escape
                            ? "use `.put(i, value)` on `((~T)?)[N]`, or keep using `mem::take`/consume-binding when extraction is needed"
                            : "use ordinary indexed assignment/swap for non-owner arrays";
                    add_array_method_diag(
                        e.span,
                        member_span,
                        "compiler-owned array method `replace` is only available on plain owner arrays `(~T)[N]`",
                        "this receiver has type `" + types_.to_string(recv_t) + "`",
                        "`replace` models explicit owner-cell replacement on a plain sized owner array",
                        help);
                    err_(member_span, "array replace requires plain owner array receiver");
                    out.ok = false;
                    out.result_type = types_.error();
                    return out;
                }
                if (!require_positional_arity(2u, "(i, value)")) {
                    out.ok = false;
                    out.result_type = types_.error();
                    return out;
                }
                if (!check_index_arg_(outside_positional[0], "first")) {
                    out.ok = false;
                    out.result_type = types_.error();
                    return out;
                }
                const auto root = require_writable_array_place_(/*forbid_global_like=*/true);
                if (!root.has_value()) {
                    out.ok = false;
                    out.result_type = types_.error();
                    return out;
                }
                const CoercionPlan value_plan = classify_assign_with_coercion_(
                    AssignSite::CallArg, elem_t,
                    outside_positional[1]->expr, outside_positional[1]->span);
                if (!value_plan.ok) {
                    add_array_method_diag(
                        e.span,
                        outside_positional[1]->span,
                        "array owner `replace` expects a replacement value assignable to `" + types_.to_string(elem_t) + "`",
                        "this argument does not match the owner element type",
                        "`replace` moves a new owner into the indexed cell and returns the old one",
                        "pass a value of type `" + types_.to_string(elem_t) + "` here");
                    err_(outside_positional[1]->span, "array replace value type mismatch");
                    out.ok = false;
                    out.result_type = types_.error();
                    return out;
                }
                mark_expr_move_consumed_(outside_positional[1]->expr, elem_t, outside_positional[1]->span);
                mark_symbol_initialized_(*root);
                cache_array_family_call_(ArrayFamilyCallKind::kOwnerReplace);
                out.ok = true;
                out.result_type = elem_t;
                return out;
            }

            if (member_name == "take") {
                if (!elem_is_optional_escape) {
                    const std::string help =
                        elem_is_escape
                            ? "use `.replace(i, value)` on `(~T)[N]`, or keep using `mem::replace(arr[i], value)` when you already have a replacement"
                            : "use ordinary indexed read/assignment for non-owner arrays";
                    add_array_method_diag(
                        e.span,
                        member_span,
                        "compiler-owned array method `take` is only available on optional owner arrays `((~T)?)[N]`",
                        "this receiver has type `" + types_.to_string(recv_t) + "`",
                        "`take` is shorthand for indexed optional owner extraction plus null writeback",
                        help);
                    err_(member_span, "array take requires optional owner array receiver");
                    out.ok = false;
                    out.result_type = types_.error();
                    return out;
                }
                if (!require_positional_arity(1u, "(i)")) {
                    out.ok = false;
                    out.result_type = types_.error();
                    return out;
                }
                if (!check_index_arg_(outside_positional[0], "first")) {
                    out.ok = false;
                    out.result_type = types_.error();
                    return out;
                }
                const auto root = require_writable_array_place_(/*forbid_global_like=*/true);
                if (!root.has_value()) {
                    out.ok = false;
                    out.result_type = types_.error();
                    return out;
                }
                mark_symbol_initialized_(*root);
                cache_array_family_call_(ArrayFamilyCallKind::kOwnerTake);
                out.ok = true;
                out.result_type = elem_t;
                return out;
            }

            if (!elem_is_optional_escape) {
                const std::string help =
                    elem_is_escape
                        ? "use `.replace(i, value)` on `(~T)[N]` when the indexed cell is always occupied"
                        : "use ordinary indexed assignment for non-owner arrays";
                add_array_method_diag(
                    e.span,
                    member_span,
                    "compiler-owned array method `put` is only available on optional owner arrays `((~T)?)[N]`",
                    "this receiver has type `" + types_.to_string(recv_t) + "`",
                    "`put` writes a new owner into an optional owner slot and returns the previous optional value",
                    help);
                err_(member_span, "array put requires optional owner array receiver");
                out.ok = false;
                out.result_type = types_.error();
                return out;
            }
            if (!require_positional_arity(2u, "(i, value)")) {
                out.ok = false;
                out.result_type = types_.error();
                return out;
            }
            if (!check_index_arg_(outside_positional[0], "first")) {
                out.ok = false;
                out.result_type = types_.error();
                return out;
            }
            const auto root = require_writable_array_place_(/*forbid_global_like=*/true);
            if (!root.has_value()) {
                out.ok = false;
                out.result_type = types_.error();
                return out;
            }
            const ty::TypeId value_t = types_.get(elem_t).elem;
            const CoercionPlan value_plan = classify_assign_with_coercion_(
                AssignSite::CallArg, value_t,
                outside_positional[1]->expr, outside_positional[1]->span);
            if (!value_plan.ok) {
                add_array_method_diag(
                    e.span,
                    outside_positional[1]->span,
                    "array owner `put` expects a replacement owner assignable to `" + types_.to_string(value_t) + "`",
                    "this argument does not match the owner element type",
                    "`put` stores a new owner into the optional slot and returns the previous optional owner",
                    "pass a value of type `" + types_.to_string(value_t) + "` here");
                err_(outside_positional[1]->span, "array put value type mismatch");
                out.ok = false;
                out.result_type = types_.error();
                return out;
            }
            mark_expr_move_consumed_(outside_positional[1]->expr, value_t, outside_positional[1]->span);
            mark_symbol_initialized_(*root);
            cache_array_family_call_(ArrayFamilyCallKind::kOwnerPut);
            out.ok = true;
            out.result_type = elem_t;
            return out;
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
                                                      std::vector<ast::StmtId>& out,
                                                      bool emit_diag = true) -> bool {
            out.clear();
            if (owner_t == ty::kInvalidType) return false;

            const ast::StmtId owner_sid = resolve_owner_decl_sid_for_proto(owner_t);
            if (owner_sid == ast::k_invalid_stmt || (size_t)owner_sid >= ast_.stmts().size()) {
                if (emit_diag) {
                    diag_(diag::Code::kProtoArrowMemberNotFound, member_span, std::string(member_name));
                    err_(member_span, "proto arrow member is unavailable for this receiver type");
                }
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
            if (auto it = explicit_impl_proto_sids_by_type_.find(canonicalize_acts_owner_type_(owner_t));
                it != explicit_impl_proto_sids_by_type_.end()) {
                for (const auto psid : it->second) {
                    collect_proto_closure(collect_proto_closure, psid, declared_proto_sids);
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
                    if (emit_diag) {
                        diag_(diag::Code::kProtoArrowMemberNotFound, member_span, std::string(member_name));
                        err_(member_span, "unknown proto qualifier on arrow access");
                    }
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
                if (emit_diag) {
                    diag_(diag::Code::kProtoArrowMemberNotFound, member_span, std::string(member_name));
                    err_(member_span, "proto arrow call target is not found");
                }
                return false;
            }

            if (!qualifier.has_value() && provider_proto_sids.size() > 1) {
                if (emit_diag) {
                    diag_(diag::Code::kProtoArrowQualifierRequired, member_span, std::string(member_name));
                    err_(member_span, "arrow member is provided by multiple protos; use receiver->Proto.member");
                }
                return false;
            }

            if (qualifier.has_value() && provider_proto_sids.size() > 1) {
                if (emit_diag) {
                    diag_(diag::Code::kProtoArrowMemberAmbiguous, member_span, std::string(member_name));
                    err_(member_span, "arrow member remains ambiguous in qualified proto closure");
                }
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
                    if (!proto_qualifier.has_value()) {
                        const auto array_method = try_check_array_family_method_(recv_eid, owner_t, rhs.text, rhs.span);
                        if (array_method.matched) {
                            if (!array_method.ok) {
                                check_all_arg_exprs_only();
                                return types_.error();
                            }
                            return array_method.result_type;
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
                                bool had_private_candidate = false;
                                ast::StmtId class_owner_sid = ast::k_invalid_stmt;
                                if (auto it = class_decl_by_type_.find(class_owner_t);
                                    it != class_decl_by_type_.end()) {
                                    class_owner_sid = it->second;
                                }
                                for (const auto sid : mit->second) {
                                    if (sid == ast::k_invalid_stmt || (size_t)sid >= ast_.stmts().size()) continue;
                                    const auto& m = ast_.stmt(sid);
                                    if (m.kind != ast::StmtKind::kFnDecl) continue;
                                    if (class_owner_sid != ast::k_invalid_stmt &&
                                        is_private_class_stmt_member_(sid) &&
                                        !can_access_class_member_(class_owner_sid, ast::FieldMember::Visibility::kPrivate)) {
                                        had_private_candidate = true;
                                        continue;
                                    }
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
                                } else if (had_private_candidate) {
                                    diag_(diag::Code::kClassPrivateMemberAccessDenied, rhs.span, rhs.text);
                                    err_(rhs.span, "private class member is not accessible here");
                                    check_all_arg_exprs_only();
                                    return types_.error();
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
                            const bool recv_rewritten = apply_imported_path_rewrite_(recv_lookup);
                            if (auto recv_sid = recv_rewritten ? sym_.lookup(recv_lookup) : lookup_symbol_(recv_lookup)) {
                                bound_selection = lookup_symbol_acts_selection_(*recv_sid);
                            }
                        }

                        const auto selected_methods = lookup_acts_methods_for_call_(owner_t, rhs.text, bound_selection);
                        auto effective_selected_methods = selected_methods;
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
                            std::string owner_base{};
                            std::vector<ty::TypeId> owner_args{};
                            if (decompose_named_user_type_(owner_t, owner_base, owner_args) && !owner_base.empty()) {
                                for (const auto& [template_owner_base, member_map] : external_acts_template_method_map_) {
                                    if (!owner_base_name_matches(owner_base, template_owner_base)) continue;
                                    auto mit = member_map.find(std::string(rhs.text));
                                    if (mit != member_map.end() && !mit->second.empty()) {
                                        any_external_method_named = true;
                                        selected_external_methods.insert(
                                            selected_external_methods.end(),
                                            mit->second.begin(),
                                            mit->second.end()
                                        );
                                    }
                                }
                            }
                        }
                        if (!any_external_method_named) {
                            selected_external_methods =
                                lookup_external_acts_methods_for_call_(owner_t, rhs.text);
                            if (!selected_external_methods.empty()) {
                                any_external_method_named = true;
                            }
                        }

                        if (effective_selected_methods.empty() &&
                            any_external_method_named &&
                            owner_t != ty::kInvalidType &&
                            materialize_imported_acts_templates_for_member_(owner_t, rhs.text, rhs.span)) {
                            effective_selected_methods =
                                lookup_acts_methods_for_call_(owner_t, rhs.text, bound_selection);
                            if (!effective_selected_methods.empty()) {
                                any_method_named = true;
                            }
                        }

                        bool has_self_receiver_candidate = false;
                        for (const auto& md : effective_selected_methods) {
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
                        } else if (any_method_named && !effective_selected_methods.empty()) {
                            diag_(diag::Code::kDotMethodSelfRequired, rhs.span, rhs.text);
                            err_(rhs.span, "dot call requires self receiver on acts method");
                            check_all_arg_exprs_only();
                            return types_.error();
                        } else if (any_method_named && effective_selected_methods.empty()) {
                            std::ostringstream oss;
                            oss << "no active acts method '" << rhs.text
                                << "' for type " << types_.to_string(owner_t)
                                << " (select with 'use " << types_.to_string(owner_t)
                                << " with acts(Name);' or use default)";
                            diag_(diag::Code::kTypeErrorGeneric, rhs.span, oss.str());
                            err_(rhs.span, oss.str());
                            check_all_arg_exprs_only();
                            return types_.error();
                        } else {
                            std::vector<ast::StmtId> proto_overloads;
                            if (collect_proto_arrow_call_overloads(
                                    owner_t, rhs.text, std::nullopt, rhs.span, proto_overloads, /*emit_diag=*/false)) {
                                overload_decl_ids.insert(overload_decl_ids.end(), proto_overloads.begin(), proto_overloads.end());
                                is_dot_method_call = true;
                                dot_owner_type = owner_t;
                                dot_needs_self_normalization = false;
                                callee_name = types_.to_string(owner_t) + "." + std::string(rhs.text);
                            }
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
                    const bool allow_hidden_imported_enum =
                        callee_expr.span.file_id != 0 &&
                        explicit_file_bundle_overrides_.find(callee_expr.span.file_id) !=
                            explicit_file_bundle_overrides_.end();
                    auto is_hidden_imported_enum_sid = [&](ast::StmtId sid) -> bool {
                        return imported_hidden_enum_template_sid_set_.find(sid) != imported_hidden_enum_template_sid_set_.end() ||
                               imported_hidden_enum_instance_sid_set_.find(sid) != imported_hidden_enum_instance_sid_set_.end();
                    };

                    auto resolve_plain_enum_owner = [&](const std::string& raw_name) -> ast::StmtId {
                        std::string key = raw_name;
                        const bool key_rewritten = apply_imported_path_rewrite_(key);

                        if (key.find("::") == std::string::npos) {
                            ast::StmtId local_found = ast::k_invalid_stmt;
                            bool ambiguous_local = false;
                            for (ast::StmtId sid = 0; sid < static_cast<ast::StmtId>(ast_.stmts().size()); ++sid) {
                                const auto& cand = ast_.stmt(sid);
                                if (cand.kind != ast::StmtKind::kEnumDecl) continue;
                                if (cand.name != key) continue;
                                if (local_found != ast::k_invalid_stmt && local_found != sid) {
                                    ambiguous_local = true;
                                    break;
                                }
                                local_found = sid;
                            }
                            if (!ambiguous_local && local_found != ast::k_invalid_stmt) {
                                return local_found;
                            }
                        }

                        if (key.find("::") == std::string::npos) {
                            const std::string current_qualified = qualify_decl_name_(key);
                            auto hidden_it = imported_enum_template_sid_by_qname_.find(current_qualified);
                            if (hidden_it != imported_enum_template_sid_by_qname_.end()) {
                                if (allow_hidden_imported_enum || !is_hidden_imported_enum_sid(hidden_it->second)) {
                                    return hidden_it->second;
                                }
                            }
                            auto current_it = enum_decl_by_name_.find(current_qualified);
                            if (current_it != enum_decl_by_name_.end() &&
                                (allow_hidden_imported_enum || !is_hidden_imported_enum_sid(current_it->second))) {
                                return current_it->second;
                            }
                        }

                        // 1) exact enum-decl name key lookup
                        auto hidden_exact = imported_enum_template_sid_by_qname_.find(key);
                        if (hidden_exact != imported_enum_template_sid_by_qname_.end()) {
                            if (allow_hidden_imported_enum || !is_hidden_imported_enum_sid(hidden_exact->second)) {
                                return hidden_exact->second;
                            }
                        }
                        auto it = enum_decl_by_name_.find(key);
                        if (it != enum_decl_by_name_.end() &&
                            (allow_hidden_imported_enum || !is_hidden_imported_enum_sid(it->second))) {
                            return it->second;
                        }

                        // 1.5) suffix lookup for qualified names (e.g. pkg::Token)
                        {
                            ast::StmtId found = ast::k_invalid_stmt;
                            const std::string suffix = "::" + key;
                            for (const auto& kv : enum_decl_by_name_) {
                                const std::string& q = kv.first;
                                if (q == key) {
                                    if (!allow_hidden_imported_enum && is_hidden_imported_enum_sid(kv.second)) {
                                        continue;
                                    }
                                    found = kv.second;
                                    break;
                                }
                                if (q.size() > suffix.size() &&
                                    q.compare(q.size() - suffix.size(), suffix.size(), suffix) == 0) {
                                    if (!allow_hidden_imported_enum && is_hidden_imported_enum_sid(kv.second)) {
                                        continue;
                                    }
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
                        if (auto sym_sid = key_rewritten ? sym_.lookup(key) : lookup_symbol_(key)) {
                            const auto& ss = sym_.symbol(*sym_sid);
                            if (ss.kind == sema::SymbolKind::kType &&
                                ss.declared_type != ty::kInvalidType) {
                                auto eit2 = enum_decl_by_type_.find(ss.declared_type);
                                if (eit2 != enum_decl_by_type_.end() &&
                                    (allow_hidden_imported_enum || !is_hidden_imported_enum_sid(eit2->second))) {
                                    return eit2->second;
                                }
                            }
                            auto eit = enum_decl_by_name_.find(ss.name);
                            if (eit != enum_decl_by_name_.end() &&
                                (allow_hidden_imported_enum || !is_hidden_imported_enum_sid(eit->second))) {
                                return eit->second;
                            }
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
                        ast::StmtId template_enum_sid = ast::k_invalid_stmt;
                        auto hidden_exact_it = imported_enum_template_sid_by_qname_.find(base_lookup);
                        if (hidden_exact_it != imported_enum_template_sid_by_qname_.end() &&
                            (allow_hidden_imported_enum || !is_hidden_imported_enum_sid(hidden_exact_it->second))) {
                            template_enum_sid = hidden_exact_it->second;
                        }
                        auto bit = enum_decl_by_name_.find(base_lookup);
                        if (bit == enum_decl_by_name_.end() &&
                            base_lookup.find("::") == std::string::npos) {
                            ast::StmtId local_template = ast::k_invalid_stmt;
                            bool ambiguous_local = false;
                            for (ast::StmtId sid = 0; sid < static_cast<ast::StmtId>(ast_.stmts().size()); ++sid) {
                                const auto& cand = ast_.stmt(sid);
                                if (cand.kind != ast::StmtKind::kEnumDecl) continue;
                                if (cand.name != base_lookup) continue;
                                if (cand.decl_generic_param_count == 0) continue;
                                if (local_template != ast::k_invalid_stmt && local_template != sid) {
                                    ambiguous_local = true;
                                    break;
                                }
                                local_template = sid;
                            }
                            if (!ambiguous_local && local_template != ast::k_invalid_stmt) {
                                template_enum_sid = local_template;
                            }
                        }
                        if (bit == enum_decl_by_name_.end() &&
                            base_lookup.find("::") == std::string::npos) {
                            const std::string current_qualified = qualify_decl_name_(base_lookup);
                            auto hidden_it = imported_enum_template_sid_by_qname_.find(current_qualified);
                            if (hidden_it != imported_enum_template_sid_by_qname_.end() &&
                                (allow_hidden_imported_enum || !is_hidden_imported_enum_sid(hidden_it->second))) {
                                template_enum_sid = hidden_it->second;
                            }
                            bit = enum_decl_by_name_.find(current_qualified);
                        }
                        if (bit != enum_decl_by_name_.end() &&
                            (allow_hidden_imported_enum || !is_hidden_imported_enum_sid(bit->second))) {
                            template_enum_sid = bit->second;
                        }
                        if (template_enum_sid != ast::k_invalid_stmt) {
                            const auto& templ = ast_.stmt(template_enum_sid);
                            if (templ.kind == ast::StmtKind::kEnumDecl &&
                                templ.decl_generic_param_count > 0) {
                                MonoRequest req{};
                                req.templ.template_sid = template_enum_sid;
                                req.templ.producer_bundle = current_bundle_name_();
                                req.templ.template_symbol = base_lookup;
                                req.concrete_args = owner_args;
                                if (imported_enum_template_sid_set_.find(template_enum_sid) != imported_enum_template_sid_set_.end()) {
                                    req.templ.source = MonoTemplateRef::SourceKind::kImportedEnum;
                                    if (auto idx_it = imported_enum_template_index_by_sid_.find(template_enum_sid);
                                        idx_it != imported_enum_template_index_by_sid_.end() &&
                                        idx_it->second < explicit_imported_enum_templates_.size()) {
                                        const auto& templ_meta = explicit_imported_enum_templates_[idx_it->second];
                                        req.templ.producer_bundle = templ_meta.producer_bundle;
                                        req.templ.template_symbol =
                                            !templ_meta.public_path.empty() ? templ_meta.public_path : templ_meta.lookup_name;
                                    }
                                } else {
                                    req.templ.source = MonoTemplateRef::SourceKind::kLocalEnum;
                                }
                                if (auto inst_sid = ensure_monomorphized_enum_(req, callee_expr.span)) {
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
                    const bool owner_rewritten = apply_imported_path_rewrite_(owner_lookup);

                    const auto owner_sid = owner_rewritten ? sym_.lookup(owner_lookup) : lookup_symbol_(owner_lookup);
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
                    const bool lookup_rewritten = apply_imported_path_rewrite_(lookup_name);
                    const bool allow_hidden_imported_class =
                        callee_expr.span.file_id != 0 &&
                        explicit_file_bundle_overrides_.find(callee_expr.span.file_id) !=
                            explicit_file_bundle_overrides_.end();
                    auto is_hidden_imported_class_sid = [&](ast::StmtId sid) -> bool {
                        return imported_hidden_class_template_sid_set_.find(sid) != imported_hidden_class_template_sid_set_.end() ||
                               imported_hidden_class_instance_sid_set_.find(sid) != imported_hidden_class_instance_sid_set_.end();
                    };
                    auto resolve_class_template_by_text = [&](const std::string& raw_name) -> ast::StmtId {
                        ast::StmtId found = ast::k_invalid_stmt;
                        if (raw_name.find("::") == std::string::npos) {
                            const std::string current_qualified = qualify_decl_name_(raw_name);
                            if (auto hit = imported_class_template_sid_by_qname_.find(current_qualified);
                                hit != imported_class_template_sid_by_qname_.end()) {
                                if (allow_hidden_imported_class || !is_hidden_imported_class_sid(hit->second)) {
                                    found = hit->second;
                                }
                            }
                            if (found == ast::k_invalid_stmt) {
                                if (auto it = class_decl_by_name_.find(current_qualified);
                                    it != class_decl_by_name_.end() &&
                                    (allow_hidden_imported_class || !is_hidden_imported_class_sid(it->second))) {
                                    found = it->second;
                                }
                            }
                            if (found == ast::k_invalid_stmt) {
                                const std::string cur_head = current_module_head_();
                                if (!cur_head.empty()) {
                                    const std::string module_qualified = cur_head + "::" + raw_name;
                                    if (auto hit = imported_class_template_sid_by_qname_.find(module_qualified);
                                        hit != imported_class_template_sid_by_qname_.end()) {
                                        if (allow_hidden_imported_class || !is_hidden_imported_class_sid(hit->second)) {
                                            found = hit->second;
                                        }
                                    }
                                    if (found == ast::k_invalid_stmt) {
                                        if (auto it = class_decl_by_name_.find(module_qualified);
                                            it != class_decl_by_name_.end() &&
                                            (allow_hidden_imported_class || !is_hidden_imported_class_sid(it->second))) {
                                            found = it->second;
                                        }
                                    }
                                }
                            }
                        }
                        if (found == ast::k_invalid_stmt) {
                            if (auto hit = imported_class_template_sid_by_qname_.find(raw_name);
                                hit != imported_class_template_sid_by_qname_.end()) {
                                if (allow_hidden_imported_class || !is_hidden_imported_class_sid(hit->second)) {
                                    found = hit->second;
                                }
                            }
                        }
                        if (found == ast::k_invalid_stmt) {
                            if (auto it = class_decl_by_name_.find(raw_name);
                                it != class_decl_by_name_.end() &&
                                (allow_hidden_imported_class || !is_hidden_imported_class_sid(it->second))) {
                                found = it->second;
                            }
                        }
                        return found;
                    };
                    auto adopt_class_ctor_from_sid = [&](ast::StmtId class_sid, const std::string& class_name_seed) -> bool {
                        if (class_sid == ast::k_invalid_stmt || (size_t)class_sid >= ast_.stmts().size()) return false;
                        const auto& class_decl = ast_.stmt(class_sid);
                        if (class_decl.kind != ast::StmtKind::kClassDecl) return false;

                        std::string class_name = class_name_seed;
                        if (auto qit = class_qualified_name_by_stmt_.find(class_sid);
                            qit != class_qualified_name_by_stmt_.end() && !qit->second.empty()) {
                            class_name = qit->second;
                        }

                        is_ctor_call = true;
                        ctor_owner_type = class_decl.type;
                        fallback_ret = (ctor_owner_type == ty::kInvalidType) ? types_.error() : ctor_owner_type;
                        callee_name = class_name;

                        if (class_decl.decl_generic_param_count > 0) {
                            if (explicit_call_type_args.empty()) {
                                diag_(diag::Code::kGenericTypeArgInferenceFailed, callee_expr.span, class_name);
                                err_(callee_expr.span, "generic class constructor requires explicit type arguments");
                                check_all_arg_exprs_only();
                                return false;
                            }

                            MonoRequest req{};
                            req.templ.template_sid = class_sid;
                            req.templ.producer_bundle = current_bundle_name_();
                            req.templ.template_symbol = class_name;
                            req.concrete_args = explicit_call_type_args;
                            if (imported_class_template_sid_set_.find(class_sid) != imported_class_template_sid_set_.end()) {
                                req.templ.source = MonoTemplateRef::SourceKind::kImportedClass;
                                if (auto it = imported_class_template_index_by_sid_.find(class_sid);
                                    it != imported_class_template_index_by_sid_.end() &&
                                    it->second < explicit_imported_class_templates_.size()) {
                                    const auto& templ_meta = explicit_imported_class_templates_[it->second];
                                    req.templ.producer_bundle = templ_meta.producer_bundle;
                                    req.templ.template_symbol =
                                        !templ_meta.lookup_name.empty() ? templ_meta.lookup_name : templ_meta.public_path;
                                }
                            } else {
                                req.templ.source = MonoTemplateRef::SourceKind::kLocalClass;
                            }

                            auto inst_sid = ensure_monomorphized_class_(req, callee_expr.span);
                            if (!inst_sid.has_value() ||
                                *inst_sid == ast::k_invalid_stmt ||
                                (size_t)(*inst_sid) >= ast_.stmts().size()) {
                                check_all_arg_exprs_only();
                                return false;
                            }

                            const auto& inst_decl = ast_.stmt(*inst_sid);
                            ctor_owner_type = inst_decl.type;
                            fallback_ret = (ctor_owner_type == ty::kInvalidType) ? types_.error() : ctor_owner_type;

                            if (auto qit = class_qualified_name_by_stmt_.find(*inst_sid);
                                qit != class_qualified_name_by_stmt_.end() && !qit->second.empty()) {
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
                                    if (member.kind != ast::StmtKind::kFnDecl || member.name != "init") continue;
                                    overload_decl_ids.push_back(msid);
                                }
                            }
                            explicit_call_type_args.clear();
                        } else {
                            if (!explicit_call_type_args.empty()) {
                                diag_(diag::Code::kGenericArityMismatch, callee_expr.span, "0",
                                      std::to_string(explicit_call_type_args.size()));
                                err_(callee_expr.span, "non-generic class constructor call does not accept type arguments");
                                check_all_arg_exprs_only();
                                return false;
                            }

                            std::string init_qname = class_name;
                            init_qname += "::init";
                            if (auto fit = fn_decl_by_name_.find(init_qname); fit != fn_decl_by_name_.end()) {
                                overload_decl_ids = fit->second;
                            }
                        }

                        if (overload_decl_ids.empty()) {
                            diag_(diag::Code::kClassCtorMissingInit, callee_expr.span, class_name);
                            err_(callee_expr.span, "class constructor call requires init overload");
                            check_all_arg_exprs_only();
                            return false;
                        }
                        return true;
                    };

                    callee_name = lookup_name;
                    if (auto sid = lookup_rewritten ? sym_.lookup(lookup_name) : lookup_symbol_(lookup_name)) {
                        direct_ident_symbol = *sid;
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

                                    MonoRequest req{};
                                    req.templ.template_sid = class_sid;
                                    req.templ.producer_bundle = current_bundle_name_();
                                    req.templ.template_symbol = sym.name;
                                    req.concrete_args = explicit_call_type_args;
                                    if (imported_class_template_sid_set_.find(class_sid) != imported_class_template_sid_set_.end()) {
                                        req.templ.source = MonoTemplateRef::SourceKind::kImportedClass;
                                        if (auto it = imported_class_template_index_by_sid_.find(class_sid);
                                            it != imported_class_template_index_by_sid_.end() &&
                                            it->second < explicit_imported_class_templates_.size()) {
                                            const auto& templ_meta = explicit_imported_class_templates_[it->second];
                                            req.templ.producer_bundle = templ_meta.producer_bundle;
                                            req.templ.template_symbol =
                                                !templ_meta.lookup_name.empty() ? templ_meta.lookup_name : templ_meta.public_path;
                                        }
                                    } else {
                                        req.templ.source = MonoTemplateRef::SourceKind::kLocalClass;
                                        if (auto qit = class_qualified_name_by_stmt_.find(class_sid);
                                            qit != class_qualified_name_by_stmt_.end()) {
                                            req.templ.template_symbol = qit->second;
                                        }
                                    }

                                    auto inst_sid = ensure_monomorphized_class_(req, callee_expr.span);
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
                            if (!sym.external_payload.empty()) {
                                cimport_meta = parse_cimport_call_meta_(sym.external_payload);
                            }
                            if (cimport_meta.is_c_abi ||
                                (sym.declared_type != ty::kInvalidType &&
                                 types_.get(sym.declared_type).kind == ty::Kind::kFn &&
                                 types_.fn_is_c_abi(sym.declared_type))) {
                                is_cimport_call = true;
                                cimport_callee_symbol = *sid;
                            }
                            if (sym.is_external) {
                                overload_decl_ids = collect_imported_template_overloads_for_external_symbol_(sym);
                            }
                            if (overload_decl_ids.empty()) {
                                auto it = fn_decl_by_name_.find(callee_name);
                                if (it != fn_decl_by_name_.end()) {
                                    overload_decl_ids = it->second;
                                }
                            }
                        }
                    }
                    if (direct_ident_symbol == sema::SymbolTable::kNoScope && overload_decl_ids.empty()) {
                        const ast::StmtId hidden_class_sid = resolve_class_template_by_text(lookup_name);
                        if (hidden_class_sid != ast::k_invalid_stmt) {
                            if (!adopt_class_ctor_from_sid(hidden_class_sid, lookup_name)) {
                                return types_.error();
                            }
                        }
                    }
                    if (direct_ident_symbol == sema::SymbolTable::kNoScope &&
                        overload_decl_ids.empty() &&
                        !is_ctor_call) {
                        auto external_sids = collect_external_fn_candidates_for_name_(lookup_name);
                        if (!external_sids.empty()) {
                            direct_ident_symbol = external_sids.front();
                            const auto& sym = sym_.symbol(direct_ident_symbol);
                            callee_name = visible_external_fn_name_(sym.name);
                            if (!sym.external_payload.empty()) {
                                cimport_meta = parse_cimport_call_meta_(sym.external_payload);
                            }
                            if (cimport_meta.is_c_abi ||
                                (sym.declared_type != ty::kInvalidType &&
                                 types_.get(sym.declared_type).kind == ty::Kind::kFn &&
                                 types_.fn_is_c_abi(sym.declared_type))) {
                                is_cimport_call = true;
                                cimport_callee_symbol = direct_ident_symbol;
                            }
                        }
                    }
                }

                if (!is_ctor_call) {
                    callee_t = check_expr_(e.a);
                    if (direct_ident_symbol == sema::SymbolTable::kNoScope &&
                        e.a != ast::k_invalid_expr &&
                        static_cast<size_t>(e.a) < expr_resolved_symbol_cache_.size()) {
                        const uint32_t resolved_sid = expr_resolved_symbol_cache_[e.a];
                        if (resolved_sid != sema::SymbolTable::kNoScope &&
                            resolved_sid < sym_.symbols().size()) {
                            direct_ident_symbol = resolved_sid;
                            const auto& resolved_sym = sym_.symbol(resolved_sid);
                            callee_name = resolved_sym.name;
                            if (resolved_sym.kind == sema::SymbolKind::kFn) {
                                if (!resolved_sym.external_payload.empty()) {
                                    cimport_meta = parse_cimport_call_meta_(resolved_sym.external_payload);
                                }
                                if (cimport_meta.is_c_abi ||
                                    (resolved_sym.declared_type != ty::kInvalidType &&
                                     types_.get(resolved_sym.declared_type).kind == ty::Kind::kFn &&
                                     types_.fn_is_c_abi(resolved_sym.declared_type))) {
                                    is_cimport_call = true;
                                    cimport_callee_symbol = resolved_sid;
                                }
                                if (overload_decl_ids.empty()) {
                                    if (resolved_sym.is_external) {
                                        overload_decl_ids =
                                            collect_imported_template_overloads_for_external_symbol_(resolved_sym);
                                    }
                                    if (overload_decl_ids.empty()) {
                                        auto it = fn_decl_by_name_.find(callee_name);
                                        if (it != fn_decl_by_name_.end()) {
                                            overload_decl_ids = it->second;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    ty::TypeId callable_sig = callee_t;
                    const auto& ct = types_.get(callee_t);
                    if (ct.kind == ty::Kind::kPtr &&
                        ct.elem != ty::kInvalidType &&
                        ct.elem < types_.count() &&
                        types_.get(ct.elem).kind == ty::Kind::kFn) {
                        callable_sig = ct.elem;
                    }
                    bool adopted_class_ctor_from_type = false;
                    const auto& callable_tt = types_.get(callable_sig);
                    if (callable_tt.kind != ty::Kind::kFn) {
                        if (auto class_sid = ensure_generic_class_instance_from_type_(callee_t, callee_expr.span);
                            class_sid.has_value()) {
                            ast::StmtId concrete_class_sid = *class_sid;
                            if ((size_t)concrete_class_sid >= ast_.stmts().size() ||
                                ast_.stmt(concrete_class_sid).kind != ast::StmtKind::kClassDecl) {
                                return types_.error();
                            }
                            const auto* class_decl = &ast_.stmt(concrete_class_sid);
                            if (class_decl->decl_generic_param_count > 0) {
                                if (explicit_call_type_args.empty()) {
                                    diag_(diag::Code::kGenericTypeArgInferenceFailed, callee_expr.span, types_.to_string(callee_t));
                                    err_(callee_expr.span, "generic class constructor requires explicit type arguments");
                                    check_all_arg_exprs_only();
                                    return types_.error();
                                }

                                MonoRequest req{};
                                req.templ.template_sid = concrete_class_sid;
                                req.templ.producer_bundle = current_bundle_name_();
                                req.templ.template_symbol = types_.to_string(callee_t);
                                req.concrete_args = explicit_call_type_args;
                                if (imported_class_template_sid_set_.find(concrete_class_sid) != imported_class_template_sid_set_.end()) {
                                    req.templ.source = MonoTemplateRef::SourceKind::kImportedClass;
                                    if (auto it = imported_class_template_index_by_sid_.find(concrete_class_sid);
                                        it != imported_class_template_index_by_sid_.end() &&
                                        it->second < explicit_imported_class_templates_.size()) {
                                        const auto& templ_meta = explicit_imported_class_templates_[it->second];
                                        req.templ.producer_bundle = templ_meta.producer_bundle;
                                        req.templ.template_symbol =
                                            !templ_meta.lookup_name.empty() ? templ_meta.lookup_name : templ_meta.public_path;
                                    }
                                } else {
                                    req.templ.source = MonoTemplateRef::SourceKind::kLocalClass;
                                    if (auto qit = class_qualified_name_by_stmt_.find(concrete_class_sid);
                                        qit != class_qualified_name_by_stmt_.end() && !qit->second.empty()) {
                                        req.templ.template_symbol = qit->second;
                                    }
                                }

                                auto concrete_inst_sid = ensure_monomorphized_class_(req, callee_expr.span);
                                if (!concrete_inst_sid.has_value() ||
                                    *concrete_inst_sid == ast::k_invalid_stmt ||
                                    (size_t)(*concrete_inst_sid) >= ast_.stmts().size()) {
                                    return types_.error();
                                }
                                concrete_class_sid = *concrete_inst_sid;
                                class_decl = &ast_.stmt(concrete_class_sid);
                                explicit_call_type_args.clear();
                            }

                            is_ctor_call = true;
                            ctor_owner_type = class_decl->type;
                            fallback_ret = (ctor_owner_type == ty::kInvalidType) ? types_.error() : ctor_owner_type;
                            callee_name = types_.to_string(callee_t);
                            if (auto qit = class_qualified_name_by_stmt_.find(concrete_class_sid);
                                qit != class_qualified_name_by_stmt_.end() && !qit->second.empty()) {
                                callee_name = qit->second;
                            }

                            overload_decl_ids.clear();
                            const auto& kids = ast_.stmt_children();
                            const uint64_t mb = class_decl->stmt_begin;
                            const uint64_t me = mb + class_decl->stmt_count;
                            if (mb <= kids.size() && me <= kids.size()) {
                                for (uint32_t mi = class_decl->stmt_begin; mi < class_decl->stmt_begin + class_decl->stmt_count; ++mi) {
                                    const ast::StmtId msid = kids[mi];
                                    if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                                    const auto& member = ast_.stmt(msid);
                                    if (member.kind != ast::StmtKind::kFnDecl || member.name != "init") continue;
                                    overload_decl_ids.push_back(msid);
                                }
                            }
                            if (overload_decl_ids.empty()) {
                                diag_(diag::Code::kClassCtorMissingInit, callee_expr.span, callee_name);
                                err_(callee_expr.span, "class constructor call requires init overload");
                                check_all_arg_exprs_only();
                                return types_.error();
                            }
                            adopted_class_ctor_from_type = true;
                        } else {
                            diag_(diag::Code::kTypeNotCallable, e.span, types_.to_string(callee_t));
                            err_(e.span, "call target is not a function");
                            check_all_arg_exprs_only();
                            return types_.error();
                        }
                    }

                    if (!adopted_class_ctor_from_type) {
                        callee_t = callable_sig;
                        fallback_ret = callable_tt.ret;
                        callee_param_count = callable_tt.param_count;
                    }
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

        if (!overload_decl_ids.empty()) {
            std::unordered_set<ast::StmtId> seen_overload_sids{};
            seen_overload_sids.reserve(overload_decl_ids.size());
            std::vector<ast::StmtId> deduped_overload_sids{};
            deduped_overload_sids.reserve(overload_decl_ids.size());
            for (const auto sid : overload_decl_ids) {
                if (seen_overload_sids.insert(sid).second) {
                    deduped_overload_sids.push_back(sid);
                }
            }
            overload_decl_ids = std::move(deduped_overload_sids);
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

        auto is_c_char_like_type = [&](ty::TypeId t) -> bool {
            if (t == ty::kInvalidType || is_error_(t)) return false;
            const ty::TypeId elem = canonicalize_transparent_external_typedef_(t);
            if (elem == ty::kInvalidType || elem >= types_.count()) return false;
            const auto& et = types_.get(elem);
            if (et.kind == ty::Kind::kBuiltin) {
                switch (et.builtin) {
                    case ty::Builtin::kChar:
                    case ty::Builtin::kI8:
                    case ty::Builtin::kU8:
                    case ty::Builtin::kCChar:
                    case ty::Builtin::kCSChar:
                    case ty::Builtin::kCUChar:
                        return true;
                    default:
                        break;
                }
            }
            if (et.kind == ty::Kind::kNamedUser) {
                std::vector<std::string_view> path{};
                std::vector<ty::TypeId> args{};
                if (!types_.decompose_named_user(elem, path, args)) return false;
                if (!args.empty() || path.empty()) return false;
                const std::string_view leaf = path.back();
                if (!(leaf == "c_char" || leaf == "c_schar" || leaf == "c_uchar")) {
                    return false;
                }
                if (path.size() == 1) return true;
                return path[path.size() - 2] == "ext";
            }
            return false;
        };

        auto is_c_char_ptr_type = [&](ty::TypeId t) -> bool {
            if (t == ty::kInvalidType || is_error_(t)) return false;
            if (t < types_.count()) {
                const auto& t0 = types_.get(t);
                if (t0.kind == ty::Kind::kBorrow) t = t0.elem;
            }
            if (t == ty::kInvalidType || t >= types_.count()) return false;
            const auto& tt = types_.get(t);
            if (tt.kind != ty::Kind::kPtr || tt.elem == ty::kInvalidType) return false;
            return is_c_char_like_type(tt.elem);
        };

        auto is_core_ext_cstr_type = [&](ty::TypeId t) -> bool {
            if (t != ty::kInvalidType && t < types_.count()) {
                const auto& t0 = types_.get(t);
                if (t0.kind == ty::Kind::kBorrow) t = t0.elem;
            }
            t = canonicalize_transparent_external_typedef_(t);
            if (t == ty::kInvalidType || t >= types_.count()) return false;
            const auto& tt = types_.get(t);
            if (tt.kind != ty::Kind::kNamedUser) return false;
            std::vector<std::string_view> path{};
            std::vector<ty::TypeId> args{};
            if (!types_.decompose_named_user(t, path, args)) return false;
            if (!args.empty() || path.empty()) return false;
            if (path.back() != "CStr") return false;
            if (path.size() == 1) return true;
            const std::string_view parent = path[path.size() - 2];
            return parent == "ext" || parent == "core";
        };

        const auto arg_assignable_now = [&](const ast::Arg* a, ty::TypeId expected) -> bool {
            const ty::TypeId at = arg_type_now(a);
            if (can_assign_(expected, at)) return true;
            if (is_c_char_ptr_type(expected) && is_core_ext_cstr_type(at)) return true;

            // C ABI convenience: allow plain string literal for C char pointer slots.
            if (a != nullptr && a->expr != ast::k_invalid_expr &&
                (size_t)a->expr < ast_.exprs().size()) {
                const auto& ex = ast_.expr(a->expr);
                if (ex.kind == ast::ExprKind::kStringLit && !ex.string_is_format) {
                    if (is_c_char_ptr_type(expected)) return true;
                }
            }

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
            if (t < types_.count()) {
                const auto& raw_tt = types_.get(t);
                if (raw_tt.kind == ty::Kind::kBorrow ||
                    raw_tt.kind == ty::Kind::kEscape ||
                    raw_tt.kind == ty::Kind::kOptional) {
                    return false;
                }
            }
            t = canonicalize_transparent_external_typedef_(t);
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
                case ty::Builtin::kCChar:
                case ty::Builtin::kCSChar:
                case ty::Builtin::kCUChar:
                case ty::Builtin::kCShort:
                case ty::Builtin::kCUShort:
                case ty::Builtin::kCInt:
                case ty::Builtin::kCUInt:
                case ty::Builtin::kCLong:
                case ty::Builtin::kCULong:
                case ty::Builtin::kCLongLong:
                case ty::Builtin::kCULongLong:
                case ty::Builtin::kCFloat:
                case ty::Builtin::kCDouble:
                case ty::Builtin::kCSize:
                case ty::Builtin::kCSSize:
                case ty::Builtin::kCPtrDiff:
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

        auto external_overload_match_score = [&](ty::TypeId expected, ast::ExprId arg_eid, const CoercionPlan& plan) -> uint32_t {
            if (arg_eid == ast::k_invalid_expr) return 1000u;
            if (is_c_char_ptr_type(expected) && is_plain_string_literal_expr(arg_eid)) return 0u;

            ty::TypeId actual = check_expr_(arg_eid);
            actual = canonicalize_transparent_external_typedef_(read_decay_borrow_local(actual));
            expected = canonicalize_transparent_external_typedef_(read_decay_borrow_local(expected));

            uint32_t score = 0u;
            if (actual != expected) score += 10u;

            switch (plan.kind) {
                case CoercionKind::Exact:
                    break;
                case CoercionKind::InferThenExact:
                    score += 1u;
                    break;
                case CoercionKind::LiftToOptionalSome:
                case CoercionKind::InferThenLiftToOptionalSome:
                    score += 20u;
                    break;
                case CoercionKind::NullToOptionalNone:
                case CoercionKind::NullToPtrBoundary:
                    score += 30u;
                    break;
                case CoercionKind::Reject:
                    score += 1000u;
                    break;
            }
            return score;
        };

        struct ExternalFreeFnAttempt {
            bool handled = false;
            ty::TypeId ret = ty::kInvalidType;
        };

        auto try_external_free_fn_call_ = [&](std::string_view raw_name,
                                              uint32_t preferred_sid) -> ExternalFreeFnAttempt {
            std::vector<uint32_t> external_candidate_sids{};
            if (!raw_name.empty()) {
                external_candidate_sids = collect_external_fn_candidates_for_name_(raw_name);
            }
            if (external_candidate_sids.empty() &&
                preferred_sid != sema::SymbolTable::kNoScope &&
                preferred_sid < sym_.symbols().size() &&
                is_external_free_fn_candidate_(sym_.symbol(preferred_sid))) {
                external_candidate_sids.push_back(preferred_sid);
            }
            if (external_candidate_sids.empty()) return {};

            if (form != CallForm::kPositionalOnly) {
                diag_(diag::Code::kOverloadNoMatchingCall, e.span, callee_name, make_callsite_summary());
                err_(e.span, "external function call currently supports positional arguments only");
                check_all_arg_exprs_only();
                return {true, fallback_ret};
            }

            uint32_t selected_external_sym = sema::SymbolTable::kNoScope;
            ty::TypeId selected_external_fn_type = ty::kInvalidType;
            bool has_selected_external = false;
            uint32_t selected_external_score = 0xFFFF'FFFFu;
            bool selected_external_is_template = true;
            std::vector<ty::TypeId> selected_external_generic_args{};
            bool ext_has_arity = false;
            uint32_t ext_expected_arity = 0;
            uint32_t ext_got_arity = 0;
            bool ext_has_infer_fail = false;
            bool ext_has_proto_not_found = false;
            std::string ext_proto_not_found{};
            bool ext_has_unsatisfied = false;
            std::string ext_unsat_lhs{};
            std::string ext_unsat_rhs{};
            std::string ext_unsat_concrete{};
            bool ext_has_type_mismatch = false;
            std::string ext_eq_lhs{};
            std::string ext_eq_rhs{};
            std::string ext_eq_concrete_lhs{};
            std::string ext_eq_concrete_rhs{};

            auto infer_generic_bindings_external = [&](auto&& self,
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
                        return self(self, pt.elem, at.elem, generic_set, out_bindings);
                    case ty::Kind::kNamedUser: {
                        std::vector<std::string_view> ppath{};
                        std::vector<ty::TypeId> pargs{};
                        std::vector<std::string_view> apath{};
                        std::vector<ty::TypeId> aargs{};
                        if (!types_.decompose_named_user(param_t, ppath, pargs) ||
                            !types_.decompose_named_user(arg_t, apath, aargs)) {
                            return true;
                        }
                        if (ppath != apath || pargs.size() != aargs.size()) return true;
                        for (size_t i = 0; i < pargs.size(); ++i) {
                            if (!self(self, pargs[i], aargs[i], generic_set, out_bindings)) return false;
                        }
                        return true;
                    }
                    case ty::Kind::kFn:
                        if (pt.param_count != at.param_count) return true;
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
                    default:
                        return true;
                }
            };

            for (const auto sid : external_candidate_sids) {
                if (sid == sema::SymbolTable::kNoScope || sid >= sym_.symbols().size()) continue;
                const auto& fn_sym = sym_.symbol(sid);
                const ty::TypeId fn_t = fn_sym.declared_type;
                if (fn_t == ty::kInvalidType || fn_t >= types_.count()) continue;
                const auto& fn_tt = types_.get(fn_t);
                if (fn_tt.kind != ty::Kind::kFn) continue;

                const auto meta = parse_external_generic_decl_meta_(fn_sym.external_payload);
                std::unordered_map<std::string, ty::TypeId> bindings{};
                std::vector<ty::TypeId> expected_params{};
                expected_params.reserve(fn_tt.param_count);

                if (!meta.params.empty()) {
                    if (!explicit_call_type_args.empty()) {
                        if (meta.params.size() != explicit_call_type_args.size()) {
                            ext_has_arity = true;
                            ext_expected_arity = static_cast<uint32_t>(meta.params.size());
                            ext_got_arity = static_cast<uint32_t>(explicit_call_type_args.size());
                            continue;
                        }
                        for (size_t gi = 0; gi < meta.params.size(); ++gi) {
                            bindings.emplace(meta.params[gi], explicit_call_type_args[gi]);
                        }
                    } else {
                        if (outside_positional.size() != fn_tt.param_count) continue;
                        std::unordered_set<std::string> generic_set(meta.params.begin(), meta.params.end());
                        bool infer_ok = true;
                        for (uint32_t i = 0; i < fn_tt.param_count; ++i) {
                            const ty::TypeId expected = types_.fn_param_at(fn_t, i);
                            const ty::TypeId arg_t = check_expr_(outside_positional[i]->expr);
                            if (!infer_generic_bindings_external(
                                    infer_generic_bindings_external,
                                    expected,
                                    arg_t,
                                    generic_set,
                                    bindings)) {
                                infer_ok = false;
                                break;
                            }
                        }
                        if (!infer_ok) {
                            ext_has_infer_fail = true;
                            continue;
                        }
                        bool complete = true;
                        for (const auto& gp : meta.params) {
                            if (bindings.find(gp) == bindings.end()) {
                                complete = false;
                                break;
                            }
                        }
                        if (!complete) {
                            ext_has_infer_fail = true;
                            continue;
                        }
                    }
                } else if (!explicit_call_type_args.empty()) {
                    ext_has_arity = true;
                    ext_expected_arity = 0;
                    ext_got_arity = static_cast<uint32_t>(explicit_call_type_args.size());
                    continue;
                }

                bool constraint_ok = true;
                for (const auto& cc : meta.constraints) {
                    auto lhs_it = bindings.find(cc.lhs);
                    if (lhs_it == bindings.end()) {
                        ext_has_infer_fail = true;
                        constraint_ok = false;
                        break;
                    }

                    if (cc.kind == ExternalGenericConstraintMeta::Kind::kProto) {
                        std::optional<ast::StmtId> proto_sid{};
                        {
                            ImportedProtoIdentity identity{};
                            identity.path = cc.rhs;
                            proto_sid = resolve_imported_proto_sid_by_identity_(identity, e.span);
                        }
                        ty::TypeId rhs_t = ty::kInvalidType;
                        if (proto_sid.has_value()) {
                            const auto& proto_decl = ast_.stmt(*proto_sid);
                            rhs_t = proto_decl.type;
                        } else {
                            rhs_t = parus::cimport::parse_external_type_repr(cc.rhs, {}, {}, types_);
                            if (rhs_t == ty::kInvalidType) {
                                ext_has_proto_not_found = true;
                                ext_proto_not_found = cc.rhs;
                                constraint_ok = false;
                                break;
                            }
                            rhs_t = substitute_generic_type_(rhs_t, bindings);
                        }
                        const bool proto_is_leaf = cc.rhs.find("::") == std::string::npos;
                        bool typed_path_failure = false;
                        if (!proto_sid.has_value()) {
                            proto_sid = resolve_proto_decl_from_type_(rhs_t, e.span, &typed_path_failure, /*emit_diag=*/false);
                        }
                        if (!proto_sid.has_value()) {
                            if (builtin_family_proto_satisfied_by_primitive_name_(lhs_it->second, cc.rhs)) {
                                continue;
                            }
                            const size_t pos = cc.rhs.rfind("::");
                            const std::string_view leaf =
                                (pos == std::string::npos) ? std::string_view(cc.rhs)
                                                           : std::string_view(cc.rhs).substr(pos + 2);
                            if (proto_is_leaf &&
                                (leaf == "Comparable" || leaf == "BinaryInteger" || leaf == "SignedInteger" ||
                                 leaf == "UnsignedInteger" || leaf == "BinaryFloatingPoint")) {
                                ext_has_unsatisfied = true;
                                ext_unsat_lhs = cc.lhs;
                                ext_unsat_rhs = cc.rhs;
                                ext_unsat_concrete = types_.to_string(lhs_it->second);
                                constraint_ok = false;
                                break;
                            }
                            if (!typed_path_failure) {
                                ext_has_proto_not_found = true;
                                ext_proto_not_found = cc.rhs;
                            }
                            constraint_ok = false;
                            break;
                        }
                        if (!type_satisfies_proto_constraint_(lhs_it->second, *proto_sid, e.span)) {
                            ext_has_unsatisfied = true;
                            ext_unsat_lhs = cc.lhs;
                            ext_unsat_rhs = cc.rhs;
                            ext_unsat_concrete = types_.to_string(lhs_it->second);
                            constraint_ok = false;
                            break;
                        }
                    } else {
                        ty::TypeId rhs_t = parus::cimport::parse_external_type_repr(cc.rhs, {}, {}, types_);
                        if (rhs_t == ty::kInvalidType) {
                            ext_has_infer_fail = true;
                            constraint_ok = false;
                            break;
                        }
                        rhs_t = substitute_generic_type_(rhs_t, bindings);
                        const ty::TypeId lhs_t = canonicalize_transparent_external_typedef_(lhs_it->second);
                        rhs_t = canonicalize_transparent_external_typedef_(rhs_t);
                        if (lhs_t != rhs_t) {
                            ext_has_type_mismatch = true;
                            ext_eq_lhs = cc.lhs;
                            ext_eq_rhs = cc.rhs;
                            ext_eq_concrete_lhs = types_.to_string(lhs_it->second);
                            ext_eq_concrete_rhs = types_.to_string(rhs_t);
                            constraint_ok = false;
                            break;
                        }
                    }
                }
                if (!constraint_ok) continue;

                ty::TypeId resolved_fn_t = fn_t;
                if (!bindings.empty()) resolved_fn_t = substitute_generic_type_(fn_t, bindings);
                expected_params.clear();
                if (resolved_fn_t == ty::kInvalidType || resolved_fn_t >= types_.count()) continue;
                const auto& resolved_tt = types_.get(resolved_fn_t);
                if (resolved_tt.kind != ty::Kind::kFn) continue;
                for (uint32_t i = 0; i < resolved_tt.param_count; ++i) {
                    expected_params.push_back(types_.fn_param_at(resolved_fn_t, i));
                }

                if (outside_positional.size() != expected_params.size()) continue;

                bool all_ok = true;
                uint32_t candidate_score = 0u;
                const bool candidate_is_template = !meta.params.empty();
                for (size_t i = 0; i < outside_positional.size(); ++i) {
                    const ty::TypeId expected = expected_params[i];
                    if (is_c_char_ptr_type(expected) &&
                        is_plain_string_literal_expr(outside_positional[i]->expr)) {
                        const ty::TypeId lit_ty = check_expr_(outside_positional[i]->expr);
                        if (is_error_(lit_ty)) {
                            all_ok = false;
                        }
                        if (!all_ok) break;
                        continue;
                    }
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
                    candidate_score += external_overload_match_score(
                        expected, outside_positional[i]->expr, plan);
                }
                if (!all_ok) continue;

                if (!has_selected_external ||
                    candidate_score < selected_external_score ||
                    (candidate_score == selected_external_score &&
                     selected_external_is_template && !candidate_is_template)) {
                    has_selected_external = true;
                    selected_external_score = candidate_score;
                    selected_external_is_template = candidate_is_template;
                    selected_external_sym = sid;
                    selected_external_fn_type = resolved_fn_t;
                    selected_external_generic_args.clear();
                    if (!meta.params.empty()) {
                        selected_external_generic_args.reserve(meta.params.size());
                        for (const auto& name : meta.params) {
                            auto bit = bindings.find(name);
                            selected_external_generic_args.push_back(
                                bit != bindings.end() ? bit->second : ty::kInvalidType
                            );
                        }
                    }
                }
            }

            if (selected_external_sym != sema::SymbolTable::kNoScope &&
                selected_external_fn_type != ty::kInvalidType) {
                ast::StmtId imported_template_sid = ast::k_invalid_stmt;
                if (selected_external_sym < sym_.symbols().size()) {
                    const auto& selected_sym = sym_.symbol(selected_external_sym);
                    const auto imported_sids =
                        collect_imported_template_overloads_for_external_symbol_(selected_sym);
                    if (!imported_sids.empty()) {
                        imported_template_sid = imported_sids.front();
                    }
                }

                if (imported_template_sid != ast::k_invalid_stmt) {
                    MonoRequest req{};
                    req.templ.template_sid = imported_template_sid;
                    req.templ.source = MonoTemplateRef::SourceKind::kImportedFn;
                    req.templ.producer_bundle = current_bundle_name_();
                    req.templ.template_symbol = sym_.symbol(selected_external_sym).name;
                    req.templ.link_name = sym_.symbol(selected_external_sym).link_name;
                    if (auto it = imported_fn_template_index_by_sid_.find(imported_template_sid);
                        it != imported_fn_template_index_by_sid_.end() &&
                        it->second < explicit_imported_fn_templates_.size()) {
                        const auto& imported = explicit_imported_fn_templates_[it->second];
                        req.templ.producer_bundle = imported.producer_bundle;
                        req.templ.template_symbol = imported.lookup_name.empty()
                            ? req.templ.template_symbol
                            : imported.lookup_name;
                        req.templ.link_name = imported.link_name;
                    }
                    req.concrete_args = selected_external_generic_args;
                    req.target_lane = "default";
                    req.abi_lane = "static";
                    if (auto inst = ensure_monomorphized_free_function_(req, e.span); inst.has_value()) {
                        if (call_expr_id != ast::k_invalid_expr &&
                            call_expr_id < expr_overload_target_cache_.size()) {
                            expr_overload_target_cache_[call_expr_id] = inst->decl_sid;
                        }
                        if (call_expr_id != ast::k_invalid_expr &&
                            call_expr_id < expr_external_callee_symbol_cache_.size()) {
                            expr_external_callee_symbol_cache_[call_expr_id] = sema::SymbolTable::kNoScope;
                        }
                        if (call_expr_id != ast::k_invalid_expr &&
                            call_expr_id < expr_external_callee_type_cache_.size()) {
                            expr_external_callee_type_cache_[call_expr_id] = ty::kInvalidType;
                        }
                        if (call_expr_id != ast::k_invalid_expr &&
                            call_expr_id < expr_external_receiver_expr_cache_.size()) {
                            expr_external_receiver_expr_cache_[call_expr_id] = ast::k_invalid_expr;
                        }
                        return {true, types_.get(inst->fn_type).ret};
                    }
                }

                const bool external_is_throwing =
                    types_.fn_is_throwing(selected_external_fn_type) ||
                    ((selected_external_sym < sym_.symbols().size()) &&
                     parse_external_throwing_payload_(sym_.symbol(selected_external_sym).external_payload));
                if (external_is_throwing &&
                    !in_try_expr_context_ &&
                    !fn_ctx_.is_throwing) {
                    diag_(diag::Code::kThrowingCallRequiresTryExpr, e.span);
                    err_(e.span, "non-throwing function must use try expression for external throwing call");
                    check_all_arg_exprs_only();
                    return {true, types_.error()};
                }
                if (selected_external_is_template) {
                    if (diag_bag_) {
                        diag::Diagnostic d(
                            diag::Severity::kError,
                            diag::Code::kTemplateSidecarUnavailable,
                            e.span
                        );
                        d.add_arg(visible_external_fn_name_(sym_.symbol(selected_external_sym).name));
                        d.add_note("external generic calls require typed template-sidecar materialization");
                        d.add_help("load the producer bundle's export-index together with its adjacent .templates.json sidecar");
                        diag_bag_->add(std::move(d));
                    }
                    err_(e.span, "external generic free function requires template-sidecar materialization");
                    check_all_arg_exprs_only();
                    return {true, fallback_ret};
                }
                cache_external_callee_(selected_external_sym, selected_external_fn_type);
                return {true, types_.get(selected_external_fn_type).ret};
            }
            auto emit_external_generic_failure_diag_ = [&](diag::Code code,
                                                           std::string_view a0 = {},
                                                           std::string_view a1 = {},
                                                           std::string_view a2 = {},
                                                           std::string_view a3 = {}) {
                if (!diag_bag_) return;
                diag::Diagnostic d(diag::Severity::kError, code, e.span);
                if (!a0.empty()) d.add_arg(a0);
                if (!a1.empty()) d.add_arg(a1);
                if (!a2.empty()) d.add_arg(a2);
                if (!a3.empty()) d.add_arg(a3);
                switch (code) {
                    case diag::Code::kGenericConstraintProtoNotFound:
                        d.add_note("proto-target import ergonomics only applies to public exported proto targets");
                        d.add_note("the imported generic declaration depends on a proto target that could not be resolved");
                        d.add_help("add an explicit import for the proto, or export the proto through a public path");
                        break;
                    case diag::Code::kGenericConstraintUnsatisfied:
                        d.add_note("constraint checking happens after substituting the concrete type tuple for this imported generic call");
                        d.add_help("choose type arguments that satisfy the required proto, or relax the producer-side constraint");
                        break;
                    case diag::Code::kGenericConstraintTypeMismatch:
                        d.add_note("generic equality constraints are checked after the imported call is concretized");
                        d.add_help("make both sides reduce to the same concrete type");
                        break;
                    case diag::Code::kGenericTypeArgInferenceFailed:
                        d.add_note("the compiler could not infer all generic arguments for this imported call shape");
                        d.add_help("add explicit type arguments to the call");
                        break;
                    default:
                        break;
                }
                diag_bag_->add(std::move(d));
            };
            if (ext_has_arity) {
                diag_(diag::Code::kGenericArityMismatch, e.span,
                      std::to_string(ext_expected_arity),
                      std::to_string(ext_got_arity));
                err_(e.span, "generic arity mismatch");
                check_all_arg_exprs_only();
                return {true, fallback_ret};
            }
            if (ext_has_proto_not_found) {
                emit_external_generic_failure_diag_(
                    diag::Code::kGenericConstraintProtoNotFound,
                    ext_proto_not_found
                );
                err_(e.span, "generic constraint references unknown proto");
                check_all_arg_exprs_only();
                return {true, fallback_ret};
            }
            if (ext_has_unsatisfied) {
                emit_external_generic_failure_diag_(
                    diag::Code::kGenericConstraintUnsatisfied,
                    ext_unsat_lhs, ext_unsat_rhs, ext_unsat_concrete
                );
                err_(e.span, "generic constraint is not satisfied");
                check_all_arg_exprs_only();
                return {true, fallback_ret};
            }
            if (ext_has_type_mismatch) {
                emit_external_generic_failure_diag_(
                    diag::Code::kGenericConstraintTypeMismatch,
                    ext_eq_lhs, ext_eq_rhs, ext_eq_concrete_lhs, ext_eq_concrete_rhs
                );
                err_(e.span, "generic equality constraint is not satisfied");
                check_all_arg_exprs_only();
                return {true, fallback_ret};
            }
            if (ext_has_infer_fail) {
                emit_external_generic_failure_diag_(
                    diag::Code::kGenericTypeArgInferenceFailed,
                    callee_name
                );
                err_(e.span, "failed to infer generic call type arguments");
                check_all_arg_exprs_only();
                return {true, fallback_ret};
            }

            diag_(diag::Code::kOverloadNoMatchingCall, e.span, callee_name, make_callsite_summary());
            err_(e.span, "no matching external function overload");
            check_all_arg_exprs_only();
            return {true, fallback_ret};
        };

        auto is_infer_integer_type = [&](ty::TypeId t) -> bool {
            if (t == ty::kInvalidType || is_error_(t)) return false;
            t = read_decay_borrow_local(t);
            t = canonicalize_transparent_external_typedef_(t);
            if (t == ty::kInvalidType || t >= types_.count()) return false;
            const auto& tt = types_.get(t);
            return tt.kind == ty::Kind::kBuiltin && tt.builtin == ty::Builtin::kInferInteger;
        };

        auto resolve_c_abi_fn_type = [&](ty::TypeId fn_t) -> ty::TypeId {
            ty::TypeId cur = fn_t;
            for (uint32_t depth = 0; depth < 8 && cur != ty::kInvalidType; ++depth) {
                cur = read_decay_borrow_local(cur);
                cur = canonicalize_transparent_external_typedef_(cur);
                if (cur == ty::kInvalidType || cur >= types_.count()) return ty::kInvalidType;
                const auto& tt = types_.get(cur);
                if (tt.kind == ty::Kind::kFn) return cur;
                if (tt.kind == ty::Kind::kPtr ||
                    tt.kind == ty::Kind::kBorrow ||
                    tt.kind == ty::Kind::kEscape) {
                    cur = tt.elem;
                    continue;
                }
                return ty::kInvalidType;
            }
            return ty::kInvalidType;
        };

        auto resolve_any_fn_type = [&](ty::TypeId fn_t) -> ty::TypeId {
            ty::TypeId cur = fn_t;
            for (uint32_t depth = 0; depth < 8 && cur != ty::kInvalidType; ++depth) {
                cur = read_decay_borrow_local(cur);
                cur = canonicalize_transparent_external_typedef_(cur);
                if (cur == ty::kInvalidType || cur >= types_.count()) return ty::kInvalidType;
                const auto& tt = types_.get(cur);
                if (tt.kind == ty::Kind::kFn) return cur;
                if (tt.kind == ty::Kind::kPtr ||
                    tt.kind == ty::Kind::kBorrow ||
                    tt.kind == ty::Kind::kEscape) {
                    cur = tt.elem;
                    continue;
                }
                return ty::kInvalidType;
            }
            return ty::kInvalidType;
        };

        auto classify_c_abi_fn_type = [&](ty::TypeId fn_t,
                                          bool& out_is_c_abi,
                                          bool& out_is_variadic,
                                          ty::CCallConv& out_callconv) {
            out_is_c_abi = false;
            out_is_variadic = false;
            out_callconv = ty::CCallConv::kDefault;
            const ty::TypeId resolved = resolve_c_abi_fn_type(fn_t);
            if (resolved == ty::kInvalidType) return;
            out_is_c_abi = types_.fn_is_c_abi(resolved);
            out_is_variadic = types_.fn_is_c_variadic(resolved);
            out_callconv = types_.fn_callconv(resolved);
        };

        auto check_c_abi_call_with_positions =
            [&](ty::TypeId fn_t,
                uint32_t direct_callee_symbol,
                const std::vector<ast::ExprId>& arg_exprs,
                Span diag_span) -> ty::TypeId {
                if (fn_t == ty::kInvalidType || types_.get(fn_t).kind != ty::Kind::kFn) {
                    diag_(diag::Code::kTypeNotCallable, diag_span, callee_name);
                    err_(diag_span, "invalid C import callee signature");
                    return types_.error();
                }

                const auto& fn_sig = types_.get(fn_t);
                const uint32_t fixed_param_count = fn_sig.param_count;
                const bool is_c_variadic = types_.fn_is_c_variadic(fn_t);
                if (!is_c_variadic) {
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
                    if (is_c_char_ptr_type(expected)) {
                        const ty::TypeId src_ty = check_expr_(arg_eid);
                        if (is_error_(src_ty)) return types_.error();
                        if (is_core_ext_cstr_type(src_ty)) continue;
                    }
                    const CoercionPlan plan = classify_assign_with_coercion_(
                        AssignSite::CallArg, expected, arg_eid, ast_.expr(arg_eid).span);
                    if (!plan.ok) {
                        const ty::TypeId src_canon =
                            canonicalize_transparent_external_typedef_(read_decay_borrow_local(plan.src_after));
                        const bool src_is_text =
                            src_canon != ty::kInvalidType &&
                            src_canon < types_.count() &&
                            types_.get(src_canon).kind == ty::Kind::kBuiltin &&
                            types_.get(src_canon).builtin == ty::Builtin::kText;
                        if (src_is_text && is_c_char_ptr_type(expected)) {
                            diag_(diag::Code::kTypeErrorGeneric, ast_.expr(arg_eid).span,
                                  "text value is not C ABI-safe; use *const core::ext::c_char and explicit boundary conversion");
                        }
                        diag_(diag::Code::kTypeArgTypeMismatch, ast_.expr(arg_eid).span,
                              std::to_string(i), types_.to_string(expected),
                              type_for_user_diag_(plan.src_after, arg_eid));
                        err_(ast_.expr(arg_eid).span, "C call fixed-parameter type mismatch");
                        return types_.error();
                    }
                }

                if (is_c_variadic) {
                    const bool has_variadic_tail = arg_exprs.size() > fixed_param_count;
                    if (has_variadic_tail && !has_manual_permission_(ast::kManualPermAbi)) {
                        diag_(diag::Code::kManualAbiRequired, diag_span);
                        err_(diag_span, "C variadic call requires manual[abi]");
                        return types_.error();
                    }
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
                        if (is_core_ext_cstr_type(checked_ty)) {
                            continue;
                        }
                        if (!is_c_variadic_abi_arg_type(checked_ty)) {
                            diag_(diag::Code::kCImportVariadicArgTypeUnsupported,
                                  ast_.expr(arg_eid).span, types_.to_string(checked_ty));
                            err_(ast_.expr(arg_eid).span, "unsupported type in C variadic argument");
                            return types_.error();
                        }
                    }
                }

                if (call_expr_id != ast::k_invalid_expr &&
                    call_expr_id < expr_external_callee_symbol_cache_.size() &&
                    direct_callee_symbol != sema::SymbolTable::kNoScope) {
                    expr_external_callee_symbol_cache_[call_expr_id] = direct_callee_symbol;
                }
                if (call_expr_id != ast::k_invalid_expr &&
                    call_expr_id < expr_external_callee_type_cache_.size()) {
                    expr_external_callee_type_cache_[call_expr_id] = fn_t;
                }
                if (call_expr_id != ast::k_invalid_expr &&
                    call_expr_id < expr_call_fn_type_cache_.size()) {
                    expr_call_fn_type_cache_[call_expr_id] = fn_t;
                }
                if (call_expr_id != ast::k_invalid_expr &&
                    call_expr_id < expr_call_is_c_abi_cache_.size()) {
                    expr_call_is_throwing_cache_[call_expr_id] = 0u;
                    expr_call_is_c_abi_cache_[call_expr_id] = 1u;
                    expr_call_is_c_variadic_cache_[call_expr_id] = is_c_variadic ? 1u : 0u;
                    expr_call_c_callconv_cache_[call_expr_id] = types_.fn_callconv(fn_t);
                    expr_call_c_fixed_param_count_cache_[call_expr_id] = fixed_param_count;
                }
                return fn_sig.ret;
            };

        if (is_cimport_call) {
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
            ty::TypeId c_abi_fn_t = callee_t;
            if (cimport_callee_symbol != sema::SymbolTable::kNoScope &&
                cimport_callee_symbol < sym_.symbols().size()) {
                const auto& callee_sym = sym_.symbol(cimport_callee_symbol);
                if (callee_sym.declared_type != ty::kInvalidType &&
                    callee_sym.declared_type < types_.count() &&
                    types_.get(callee_sym.declared_type).kind == ty::Kind::kFn &&
                    types_.fn_is_c_abi(callee_sym.declared_type)) {
                    c_abi_fn_t = callee_sym.declared_type;
                }
            }
            return check_c_abi_call_with_positions(c_abi_fn_t, cimport_callee_symbol, arg_exprs, e.span);
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
            bool has_selected_external = false;
            uint32_t selected_external_score = 0xFFFF'FFFFu;
            bool selected_external_is_template = true;
            std::vector<ty::TypeId> selected_external_generic_args{};
            bool ext_has_arity = false;
            uint32_t ext_expected_arity = 0;
            uint32_t ext_got_arity = 0;
            bool ext_has_proto_not_found = false;
            std::string ext_proto_not_found{};
            bool ext_has_unsatisfied = false;
            std::string ext_unsat_lhs{};
            std::string ext_unsat_rhs{};
            std::string ext_unsat_concrete{};
            bool ext_has_type_mismatch = false;
            std::string ext_eq_lhs{};
            std::string ext_eq_rhs{};
            std::string ext_eq_concrete_lhs{};
            std::string ext_eq_concrete_rhs{};
            bool ext_has_infer_fail = false;

            auto infer_generic_bindings_external = [&](auto&& self,
                                                       ty::TypeId pattern,
                                                       ty::TypeId actual,
                                                       const std::unordered_set<std::string>& generic_set,
                                                       std::unordered_map<std::string, ty::TypeId>& out_bindings) -> bool {
                pattern = canonicalize_transparent_external_typedef_(pattern);
                actual = canonicalize_transparent_external_typedef_(actual);
                if (pattern == ty::kInvalidType || actual == ty::kInvalidType) return false;
                const auto& pt = types_.get(pattern);
                const auto& at = types_.get(actual);
                switch (pt.kind) {
                    case ty::Kind::kNamedUser: {
                        std::vector<std::string_view> ppath{};
                        std::vector<ty::TypeId> pargs{};
                        if (!types_.decompose_named_user(pattern, ppath, pargs) || ppath.empty()) {
                            return pattern == actual;
                        }
                        if (pargs.empty() && ppath.size() == 1) {
                            const std::string name(ppath.front());
                            if (generic_set.find(name) != generic_set.end()) {
                                auto it = out_bindings.find(name);
                                if (it == out_bindings.end()) {
                                    out_bindings.emplace(name, actual);
                                    return true;
                                }
                                return it->second == actual;
                            }
                        }
                        std::vector<std::string_view> apath{};
                        std::vector<ty::TypeId> aargs{};
                        if (!types_.decompose_named_user(actual, apath, aargs)) return false;
                        if (ppath.size() != apath.size() || pargs.size() != aargs.size()) return false;
                        for (size_t i = 0; i < ppath.size(); ++i) {
                            if (ppath[i] != apath[i]) return false;
                        }
                        for (size_t i = 0; i < pargs.size(); ++i) {
                            if (!self(self, pargs[i], aargs[i], generic_set, out_bindings)) {
                                return false;
                            }
                        }
                        return true;
                    }
                    case ty::Kind::kBorrow:
                    case ty::Kind::kEscape:
                    case ty::Kind::kPtr:
                    case ty::Kind::kOptional:
                    case ty::Kind::kArray:
                        if (pt.kind != at.kind) return false;
                        return self(self, pt.elem, at.elem, generic_set, out_bindings);
                    case ty::Kind::kFn:
                        if (at.kind != ty::Kind::kFn ||
                            pt.param_count != at.param_count ||
                            pt.positional_param_count != at.positional_param_count) {
                            return false;
                        }
                        for (uint32_t i = 0; i < pt.param_count; ++i) {
                            if (!self(self, types_.fn_param_at(pattern, i), types_.fn_param_at(actual, i), generic_set, out_bindings)) {
                                return false;
                            }
                        }
                        return self(self, pt.ret, at.ret, generic_set, out_bindings);
                    default:
                        return pattern == actual;
                }
            };

            for (const auto& cand : external_overload_methods) {
                if (cand.fn_symbol == sema::SymbolTable::kNoScope ||
                    cand.fn_symbol >= sym_.symbols().size()) {
                    continue;
                }
                const auto& fn_sym = sym_.symbol(cand.fn_symbol);
                const ty::TypeId fn_t = fn_sym.declared_type;
                if (fn_t == ty::kInvalidType || types_.get(fn_t).kind != ty::Kind::kFn) continue;
                const auto meta = parse_external_generic_decl_meta_(cand.external_payload);
                const uint32_t total_cnt = types_.get(fn_t).param_count;
                if (total_cnt == 0) continue;

                std::unordered_map<std::string, ty::TypeId> bindings{};
                std::vector<ty::TypeId> expected_params{};
                expected_params.reserve(total_cnt > 0 ? total_cnt - 1u : 0u);

                if (cand.owner_is_generic_template) {
                    std::string concrete_owner_base{};
                    std::vector<ty::TypeId> concrete_owner_args{};
                    if (!decompose_named_user_type_(dot_owner_type, concrete_owner_base, concrete_owner_args) ||
                        !owner_base_name_matches(concrete_owner_base, cand.owner_base) ||
                        concrete_owner_args.size() != cand.owner_generic_arity ||
                        meta.params.size() < cand.owner_generic_arity) {
                        continue;
                    }

                    for (size_t i = 0; i < concrete_owner_args.size(); ++i) {
                        bindings.emplace(meta.params[i], concrete_owner_args[i]);
                    }

                    const size_t remaining_generic_count =
                        (meta.params.size() >= cand.owner_generic_arity)
                            ? (meta.params.size() - cand.owner_generic_arity)
                            : 0u;

                    if (remaining_generic_count > 0) {
                        if (!explicit_call_type_args.empty()) {
                            if (explicit_call_type_args.size() != remaining_generic_count) {
                                ext_has_arity = true;
                                ext_expected_arity = static_cast<uint32_t>(remaining_generic_count);
                                ext_got_arity = static_cast<uint32_t>(explicit_call_type_args.size());
                                continue;
                            }
                            for (size_t gi = 0; gi < remaining_generic_count; ++gi) {
                                bindings.emplace(meta.params[cand.owner_generic_arity + gi], explicit_call_type_args[gi]);
                            }
                        } else {
                            std::unordered_set<std::string> generic_set{};
                            for (size_t gi = 0; gi < remaining_generic_count; ++gi) {
                                generic_set.insert(meta.params[cand.owner_generic_arity + gi]);
                            }
                            bool infer_ok = true;
                            for (size_t i = 0; i < outside_positional.size() && i + 1u < total_cnt; ++i) {
                                ty::TypeId expected = types_.fn_param_at(fn_t, static_cast<uint32_t>(i + 1u));
                                expected = substitute_generic_type_(expected, bindings);
                                const ty::TypeId arg_t = check_expr_(outside_positional[i]->expr);
                                if (!infer_generic_bindings_external(
                                        infer_generic_bindings_external,
                                        expected,
                                        arg_t,
                                        generic_set,
                                        bindings)) {
                                    infer_ok = false;
                                    break;
                                }
                            }
                            if (!infer_ok) {
                                ext_has_infer_fail = true;
                                continue;
                            }
                            bool complete = true;
                            for (size_t gi = 0; gi < remaining_generic_count; ++gi) {
                                if (bindings.find(meta.params[cand.owner_generic_arity + gi]) == bindings.end()) {
                                    complete = false;
                                    break;
                                }
                            }
                            if (!complete) {
                                ext_has_infer_fail = true;
                                continue;
                            }
                        }
                    } else if (!explicit_call_type_args.empty()) {
                        ext_has_arity = true;
                        ext_expected_arity = 0;
                        ext_got_arity = static_cast<uint32_t>(explicit_call_type_args.size());
                        continue;
                    }

                    bool constraint_ok = true;
                    for (const auto& cc : meta.constraints) {
                        auto lhs_it = bindings.find(cc.lhs);
                        if (lhs_it == bindings.end()) {
                            ext_has_infer_fail = true;
                            constraint_ok = false;
                            break;
                        }

                        if (cc.kind == ExternalGenericConstraintMeta::Kind::kProto) {
                            std::optional<ast::StmtId> proto_sid{};
                            {
                                ImportedProtoIdentity identity{};
                                identity.path = cc.rhs;
                                proto_sid = resolve_imported_proto_sid_by_identity_(identity, e.span);
                            }
                            ty::TypeId rhs_t = ty::kInvalidType;
                            if (proto_sid.has_value()) {
                                const auto& proto_decl = ast_.stmt(*proto_sid);
                                rhs_t = proto_decl.type;
                            } else {
                                rhs_t = parus::cimport::parse_external_type_repr(cc.rhs, {}, {}, types_);
                                if (rhs_t == ty::kInvalidType) {
                                    ext_has_proto_not_found = true;
                                    ext_proto_not_found = cc.rhs;
                                    constraint_ok = false;
                                    break;
                                }
                                rhs_t = substitute_generic_type_(rhs_t, bindings);
                            }
                            const bool proto_is_leaf = cc.rhs.find("::") == std::string::npos;
                            bool typed_path_failure = false;
                            if (!proto_sid.has_value()) {
                                proto_sid = resolve_proto_decl_from_type_(rhs_t, e.span, &typed_path_failure, /*emit_diag=*/false);
                            }
                            if (!proto_sid.has_value()) {
                                if (builtin_family_proto_satisfied_by_primitive_name_(lhs_it->second, cc.rhs)) {
                                    continue;
                                }
                                const size_t pos = cc.rhs.rfind("::");
                                const std::string_view leaf =
                                    (pos == std::string::npos) ? std::string_view(cc.rhs)
                                                               : std::string_view(cc.rhs).substr(pos + 2);
                                if (proto_is_leaf &&
                                    (leaf == "Comparable" || leaf == "BinaryInteger" || leaf == "SignedInteger" ||
                                     leaf == "UnsignedInteger" || leaf == "BinaryFloatingPoint")) {
                                    ext_has_unsatisfied = true;
                                    ext_unsat_lhs = cc.lhs;
                                    ext_unsat_rhs = cc.rhs;
                                    ext_unsat_concrete = types_.to_string(lhs_it->second);
                                } else {
                                    if (!typed_path_failure) {
                                        ext_has_proto_not_found = true;
                                        ext_proto_not_found = cc.rhs;
                                    }
                                }
                                constraint_ok = false;
                                break;
                            }
                            if (!type_satisfies_proto_constraint_(lhs_it->second, *proto_sid, e.span)) {
                                ext_has_unsatisfied = true;
                                ext_unsat_lhs = cc.lhs;
                                ext_unsat_rhs = cc.rhs;
                                ext_unsat_concrete = types_.to_string(lhs_it->second);
                                constraint_ok = false;
                                break;
                            }
                            continue;
                        }

                        ty::TypeId rhs_t = parus::cimport::parse_external_type_repr(cc.rhs, {}, {}, types_);
                        if (rhs_t == ty::kInvalidType) {
                            ext_has_infer_fail = true;
                            constraint_ok = false;
                            break;
                        }
                        rhs_t = substitute_generic_type_(rhs_t, bindings);
                        const ty::TypeId lhs_t = canonicalize_transparent_external_typedef_(lhs_it->second);
                        rhs_t = canonicalize_transparent_external_typedef_(rhs_t);
                        if (lhs_t != rhs_t) {
                            ext_has_type_mismatch = true;
                            ext_eq_lhs = cc.lhs;
                            ext_eq_rhs = cc.rhs;
                            ext_eq_concrete_lhs = types_.to_string(lhs_it->second);
                            ext_eq_concrete_rhs = types_.to_string(rhs_t);
                            constraint_ok = false;
                            break;
                        }
                    }
                    if (!constraint_ok) continue;

                } else {
                    if (!explicit_call_type_args.empty()) {
                        ext_has_arity = true;
                        ext_expected_arity = 0;
                        ext_got_arity = static_cast<uint32_t>(explicit_call_type_args.size());
                        continue;
                    }
                    for (uint32_t i = 1; i < total_cnt; ++i) {
                        expected_params.push_back(types_.fn_param_at(fn_t, i));
                    }
                }

                ty::TypeId resolved_fn_t = fn_t;
                if (!bindings.empty()) {
                    resolved_fn_t = substitute_generic_type_(fn_t, bindings);
                }
                if (resolved_fn_t == ty::kInvalidType || resolved_fn_t >= types_.count()) continue;
                const auto& resolved_tt = types_.get(resolved_fn_t);
                if (resolved_tt.kind != ty::Kind::kFn || resolved_tt.param_count == 0) continue;
                expected_params.clear();
                for (uint32_t i = 1; i < resolved_tt.param_count; ++i) {
                    expected_params.push_back(types_.fn_param_at(resolved_fn_t, i));
                }

                const uint32_t expected_outside_positional = static_cast<uint32_t>(expected_params.size());
                if (outside_positional.size() != expected_outside_positional) continue;

                bool all_ok = true;
                uint32_t candidate_score = 0u;
                const bool candidate_is_template = cand.owner_is_generic_template || !meta.params.empty();
                for (size_t i = 0; i < outside_positional.size(); ++i) {
                    const ty::TypeId expected = expected_params[i];
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
                    candidate_score += external_overload_match_score(
                        expected, outside_positional[i]->expr, plan);
                }
                if (!all_ok) continue;

                if (!has_selected_external ||
                    candidate_score < selected_external_score ||
                    (candidate_score == selected_external_score &&
                     selected_external_is_template && !candidate_is_template)) {
                    has_selected_external = true;
                    selected_external_score = candidate_score;
                    selected_external_is_template = candidate_is_template;
                    selected_external_sym = cand.fn_symbol;
                    selected_external_fn_type = resolved_fn_t;
                    selected_external_generic_args.clear();
                    if (!meta.params.empty()) {
                        selected_external_generic_args.reserve(meta.params.size());
                        for (const auto& name : meta.params) {
                            auto bit = bindings.find(name);
                            selected_external_generic_args.push_back(
                                bit != bindings.end() ? bit->second : ty::kInvalidType
                            );
                        }
                    }
                }
            }

            if (selected_external_sym == sema::SymbolTable::kNoScope ||
                selected_external_fn_type == ty::kInvalidType) {
                auto emit_external_generic_failure_diag_ = [&](diag::Code code,
                                                               std::string_view a0 = {},
                                                               std::string_view a1 = {},
                                                               std::string_view a2 = {},
                                                               std::string_view a3 = {}) {
                    if (!diag_bag_) return;
                    diag::Diagnostic d(diag::Severity::kError, code, e.span);
                    if (!a0.empty()) d.add_arg(a0);
                    if (!a1.empty()) d.add_arg(a1);
                    if (!a2.empty()) d.add_arg(a2);
                    if (!a3.empty()) d.add_arg(a3);
                    switch (code) {
                        case diag::Code::kGenericConstraintProtoNotFound:
                            d.add_note("proto-target import ergonomics only applies to public exported proto targets");
                            d.add_note("the imported acts method depends on a proto target that could not be resolved");
                            d.add_help("add an explicit import for the proto, or export the proto through a public path");
                            break;
                        case diag::Code::kGenericConstraintUnsatisfied:
                            d.add_note("constraint checking happens after substituting the concrete type tuple for this imported acts member");
                            d.add_help("choose type arguments that satisfy the required proto, or relax the producer-side constraint");
                            break;
                        case diag::Code::kGenericConstraintTypeMismatch:
                            d.add_note("generic equality constraints are checked after the imported acts member is concretized");
                            d.add_help("make both sides reduce to the same concrete type");
                            break;
                        case diag::Code::kGenericTypeArgInferenceFailed:
                            d.add_note("the compiler could not infer all generic arguments for this imported acts member call");
                            d.add_help("add explicit type arguments to the call");
                            break;
                        default:
                            break;
                    }
                    diag_bag_->add(std::move(d));
                };
                if (ext_has_arity) {
                    diag_(diag::Code::kGenericArityMismatch, e.span,
                          std::to_string(ext_expected_arity),
                          std::to_string(ext_got_arity));
                    err_(e.span, "generic arity mismatch");
                    check_all_arg_exprs_only();
                    return fallback_ret;
                }
                if (ext_has_proto_not_found) {
                    emit_external_generic_failure_diag_(
                        diag::Code::kGenericConstraintProtoNotFound,
                        ext_proto_not_found
                    );
                    err_(e.span, "generic constraint references unknown proto");
                    check_all_arg_exprs_only();
                    return fallback_ret;
                }
                if (ext_has_unsatisfied) {
                    emit_external_generic_failure_diag_(
                        diag::Code::kGenericConstraintUnsatisfied,
                        ext_unsat_lhs, ext_unsat_rhs, ext_unsat_concrete
                    );
                    err_(e.span, "generic constraint is not satisfied");
                    check_all_arg_exprs_only();
                    return fallback_ret;
                }
                if (ext_has_type_mismatch) {
                    emit_external_generic_failure_diag_(
                        diag::Code::kGenericConstraintTypeMismatch,
                        ext_eq_lhs, ext_eq_rhs, ext_eq_concrete_lhs, ext_eq_concrete_rhs
                    );
                    err_(e.span, "generic equality constraint is not satisfied");
                    check_all_arg_exprs_only();
                    return fallback_ret;
                }
                if (ext_has_infer_fail) {
                    emit_external_generic_failure_diag_(
                        diag::Code::kGenericTypeArgInferenceFailed,
                        callee_name
                    );
                    err_(e.span, "failed to infer generic call type arguments");
                    check_all_arg_exprs_only();
                    return fallback_ret;
                }
                diag_(diag::Code::kOverloadNoMatchingCall, e.span, callee_name, make_callsite_summary());
                err_(e.span, "no matching external acts method overload");
                check_all_arg_exprs_only();
                return fallback_ret;
            }

            if (selected_external_sym < sym_.symbols().size()) {
                const auto& selected_sym = sym_.symbol(selected_external_sym);
                const size_t selected_split = selected_sym.name.rfind("::");
                const std::string_view selected_member_name =
                    (selected_split == std::string::npos)
                        ? std::string_view(selected_sym.name)
                        : std::string_view(selected_sym.name).substr(selected_split + 2);

                if (dot_owner_type != ty::kInvalidType &&
                    !selected_member_name.empty() &&
                    materialize_imported_acts_templates_for_member_(dot_owner_type, selected_member_name, e.span)) {
                    const auto rebound_local_methods =
                        lookup_acts_methods_for_call_(dot_owner_type, selected_member_name, nullptr);
                    for (const auto& md : rebound_local_methods) {
                        if (md.fn_sid == ast::k_invalid_stmt) continue;
                        overload_decl_ids.push_back(md.fn_sid);
                    }
                    if (!overload_decl_ids.empty()) {
                        external_overload_methods.clear();
                    }
                }

                ast::StmtId imported_template_sid = ast::k_invalid_stmt;
                if (overload_decl_ids.empty() && !selected_sym.link_name.empty()) {
                    if (auto sid = lookup_imported_fn_template_by_link_name_(selected_sym.link_name);
                        sid.has_value()) {
                        imported_template_sid = *sid;
                    }
                }
                if (overload_decl_ids.empty() &&
                    imported_template_sid == ast::k_invalid_stmt &&
                    !selected_sym.name.empty()) {
                    const auto sids = lookup_imported_fn_templates_by_name_(selected_sym.name);
                    if (!sids.empty()) {
                        imported_template_sid = sids.front();
                    }
                }

                if (imported_template_sid != ast::k_invalid_stmt) {
                    MonoRequest req{};
                    req.templ.template_sid = imported_template_sid;
                    req.templ.source = MonoTemplateRef::SourceKind::kImportedFn;
                    req.templ.producer_bundle = current_bundle_name_();
                    req.templ.template_symbol = selected_sym.name;
                    req.templ.link_name = selected_sym.link_name;
                    if (auto it = imported_fn_template_index_by_sid_.find(imported_template_sid);
                        it != imported_fn_template_index_by_sid_.end() &&
                        it->second < explicit_imported_fn_templates_.size()) {
                        const auto& imported = explicit_imported_fn_templates_[it->second];
                        req.templ.producer_bundle = imported.producer_bundle;
                        req.templ.template_symbol = imported.lookup_name.empty()
                            ? req.templ.template_symbol
                            : imported.lookup_name;
                        req.templ.link_name = imported.link_name;
                    }
                    req.concrete_args = selected_external_generic_args;
                    req.target_lane = "default";
                    req.abi_lane = "static";
                    if (auto inst = ensure_monomorphized_free_function_(req, e.span); inst.has_value()) {
                        overload_decl_ids = {inst->decl_sid};
                        external_overload_methods.clear();
                    }
                }
            }

            if (overload_decl_ids.empty()) {
                if (call_expr_id != ast::k_invalid_expr &&
                    call_expr_id < expr_external_callee_symbol_cache_.size()) {
                    expr_external_callee_symbol_cache_[call_expr_id] = selected_external_sym;
                }
                if (call_expr_id != ast::k_invalid_expr &&
                    call_expr_id < expr_external_callee_type_cache_.size()) {
                    expr_external_callee_type_cache_[call_expr_id] = selected_external_fn_type;
                }
                if (call_expr_id != ast::k_invalid_expr &&
                    call_expr_id < expr_external_receiver_expr_cache_.size()) {
                    expr_external_receiver_expr_cache_[call_expr_id] = receiver_eid;
                }
                if (selected_external_sym < sym_.symbols().size() &&
                    parse_external_throwing_payload_(sym_.symbol(selected_external_sym).external_payload) &&
                    !in_try_expr_context_ &&
                    !fn_ctx_.is_throwing) {
                    diag_(diag::Code::kThrowingCallRequiresTryExpr, e.span);
                    err_(e.span, "non-throwing function must use try expression for external throwing acts call");
                    check_all_arg_exprs_only();
                    return types_.error();
                }
                return types_.get(selected_external_fn_type).ret;
            }
        }

        // ------------------------------------------------------------
        // 2.55) external free-function overload candidates (from export-index metadata)
        // ------------------------------------------------------------
        if (!is_ctor_call && overload_decl_ids.empty()) {
            std::string external_lookup_name{};
            if (direct_ident_symbol != sema::SymbolTable::kNoScope &&
                direct_ident_symbol < sym_.symbols().size()) {
                external_lookup_name = sym_.symbol(direct_ident_symbol).name;
            } else {
                external_lookup_name = callee_name;
            }
            if (auto external_attempt = try_external_free_fn_call_(external_lookup_name, direct_ident_symbol);
                external_attempt.handled) {
                return external_attempt.ret;
            }
        }

        // ------------------------------------------------------------
        // 2.6) core impl-bound helpers and external core::mem generics
        // ------------------------------------------------------------
        if (!is_ctor_call &&
            direct_ident_symbol != sema::SymbolTable::kNoScope &&
            direct_ident_symbol < sym_.symbols().size()) {
            auto fail_core_shape_ = [&](std::string_view message) -> ty::TypeId {
                diag_(diag::Code::kOverloadNoMatchingCall, e.span, callee_name, make_callsite_summary());
                err_(e.span, std::string(message));
                check_all_arg_exprs_only();
                return types_.error();
            };

            const auto& direct_sym = sym_.symbol(direct_ident_symbol);
            const ImplBindingKind impl_binding =
                (parse_impl_binding_mode_(direct_sym.external_payload) == "compiler")
                    ? parse_impl_binding_payload_(direct_sym.external_payload)
                    : ImplBindingKind::kNone;

            if (impl_binding == ImplBindingKind::kSpinLoop) {
                if (form != CallForm::kPositionalOnly || !outside_positional.empty() || !outside_labeled.empty()) {
                    return fail_core_shape_("core::hint::spin_loop requires zero runtime arguments");
                }
                if (!explicit_call_type_args.empty()) {
                    diag_(diag::Code::kGenericArityMismatch, e.span, "0",
                          std::to_string(explicit_call_type_args.size()));
                    err_(e.span, "core::hint::spin_loop does not accept explicit type arguments");
                    check_all_arg_exprs_only();
                    return types_.error();
                }
                cache_external_callee_(direct_ident_symbol, direct_sym.declared_type);
                return types_.builtin(ty::Builtin::kUnit);
            }

            if (impl_binding == ImplBindingKind::kSizeOf ||
                impl_binding == ImplBindingKind::kAlignOf) {
                if (form != CallForm::kPositionalOnly || !outside_positional.empty() || !outside_labeled.empty()) {
                    return fail_core_shape_("core::mem metadata helpers require zero runtime arguments");
                }
                if (explicit_call_type_args.size() != 1) {
                    diag_(diag::Code::kGenericArityMismatch, e.span, "1",
                          std::to_string(explicit_call_type_args.size()));
                    err_(e.span, "core::mem metadata helper requires one explicit type argument");
                    check_all_arg_exprs_only();
                    return types_.error();
                }
                cache_external_callee_(direct_ident_symbol, direct_sym.declared_type);
                return types_.builtin(ty::Builtin::kUSize);
            }

            if (impl_binding == ImplBindingKind::kStepNext) {
                if (form != CallForm::kPositionalOnly || outside_positional.size() != 1 || !outside_labeled.empty()) {
                    return fail_core_shape_("core::iter::step_next expects positional(T)");
                }
                if (explicit_call_type_args.size() != 1) {
                    diag_(diag::Code::kGenericArityMismatch, e.span, "1",
                          std::to_string(explicit_call_type_args.size()));
                    err_(e.span, "core::iter::step_next requires one explicit type argument");
                    check_all_arg_exprs_only();
                    return types_.error();
                }

                const ty::TypeId value_ty = explicit_call_type_args[0];
                const CoercionPlan value_plan = classify_assign_with_coercion_(
                    AssignSite::CallArg,
                    value_ty,
                    outside_positional[0]->expr,
                    outside_positional[0]->span
                );
                if (!value_plan.ok) {
                    return fail_core_shape_("core::iter::step_next argument must be assignable to T");
                }

                bool step_ok = builtin_family_proto_satisfied_by_primitive_name_(value_ty, "Step");
                if (!step_ok) {
                    ImportedProtoIdentity identity{};
                    identity.bundle = "core";
                    identity.module_head = "core::constraints";
                    identity.path = "core::constraints::Step";
                    if (auto proto_sid = resolve_imported_proto_sid_by_identity_(identity, e.span)) {
                        step_ok = type_satisfies_proto_constraint_(value_ty, *proto_sid, e.span);
                    }
                }
                if (!step_ok) {
                    diag_(diag::Code::kGenericConstraintUnsatisfied, e.span,
                          "T", "core::constraints::Step", types_.to_string(value_ty));
                    err_(e.span, "core::iter::step_next requires T: Step");
                    check_all_arg_exprs_only();
                    return types_.error();
                }

                cache_external_callee_(direct_ident_symbol, direct_sym.declared_type);
                return types_.make_optional(value_ty);
            }

            if (is_core_module_fn_(direct_ident_symbol, "mem", "take")) {
                enum class EscapeMemPlaceStatus : uint8_t { kNotEscapePlace, kOk, kHardError };
                auto add_escape_mem_diag = [&](Span primary_span,
                                               Span label_span,
                                               std::string primary,
                                               std::string label,
                                               std::string note,
                                               std::string help) {
                    diag::Diagnostic d(diag::Severity::kError, diag::Code::kTypeErrorGeneric, primary_span);
                    d.add_arg(std::move(primary));
                    d.add_label(label_span, std::move(label));
                    if (!note.empty()) d.add_note(std::move(note));
                    if (!help.empty()) d.add_help(std::move(help));
                    if (diag_bag_) diag_bag_->add(std::move(d));
                };
                auto is_optional_owner_cell_place_type = [&](ty::TypeId t) -> bool {
                    if (t == ty::kInvalidType || is_error_(t) || t >= types_.count()) return false;
                    const auto& tt = types_.get(t);
                    return tt.kind == ty::Kind::kOptional &&
                           tt.elem != ty::kInvalidType &&
                           tt.elem < types_.count() &&
                           types_.get(tt.elem).kind == ty::Kind::kEscape;
                };
                auto classify_escape_take_place = [&](const ast::Arg* arg,
                                                      ty::TypeId& out_place_ty) -> EscapeMemPlaceStatus {
                    out_place_ty = ty::kInvalidType;
                    if (arg == nullptr || arg->expr == ast::k_invalid_expr) {
                        return EscapeMemPlaceStatus::kNotEscapePlace;
                    }

                    const auto& arg_expr = ast_.expr(arg->expr);
                    if (arg_expr.kind == ast::ExprKind::kUnary &&
                        arg_expr.op == K::kAmp &&
                        arg_expr.a != ast::k_invalid_expr) {
                        const ty::TypeId operand_t = check_expr_place_no_read_(arg_expr.a);
                        if (!is_error_(operand_t) && is_optional_owner_cell_place_type(operand_t)) {
                            add_escape_mem_diag(
                                e.span,
                                arg->span,
                                "core::mem::take on `(~T)?` expects a place, not `&mut`",
                                "this argument already borrows an optional owner-cell place",
                                "`(~T)?` remains move-only and is not source-level borrowable",
                                "pass the writable `(~T)?` place directly, for example `mem::take(slot)`");
                            err_(arg->span, "core::mem::take on optional owner-cell place expects a place, not `&mut`");
                            return EscapeMemPlaceStatus::kHardError;
                        }
                    }

                    if (!is_place_expr_(arg->expr)) return EscapeMemPlaceStatus::kNotEscapePlace;

                    const ty::TypeId place_t = check_expr_place_no_read_(arg->expr);
                    if (is_error_(place_t)) return EscapeMemPlaceStatus::kHardError;
                    if (!is_optional_owner_cell_place_type(place_t)) {
                        return EscapeMemPlaceStatus::kNotEscapePlace;
                    }

                    const auto root = root_place_symbol_(arg->expr);
                    if (!root.has_value() || !is_mutable_symbol_(*root)) {
                        add_escape_mem_diag(
                            e.span,
                            arg->span,
                            "core::mem::take requires a writable `(~T)?` place",
                            "this optional owner-cell place is not writable",
                            "`take` extracts the current owner and writes `null` back into the same cell",
                            "make the root binding mutable, or use local/state restructuring so the owner lives in a mutable `(~T)?` slot");
                        err_(arg->span, "core::mem::take requires writable optional owner-cell place");
                        return EscapeMemPlaceStatus::kHardError;
                    }
                    if (!ensure_symbol_readable_(*root, arg->span)) {
                        return EscapeMemPlaceStatus::kHardError;
                    }
                    if (is_global_like_symbol_(*root)) {
                        diag_(diag::Code::kMoveFromGlobalOrStaticForbidden, arg->span, sym_.symbol(*root).name);
                        err_(arg->span, "move from global/static optional owner-cell place is not allowed");
                        return EscapeMemPlaceStatus::kHardError;
                    }

                    out_place_ty = place_t;
                    return EscapeMemPlaceStatus::kOk;
                };

                if (form == CallForm::kPositionalOnly &&
                    outside_positional.size() == 1 &&
                    outside_labeled.empty()) {
                    ty::TypeId place_ty = ty::kInvalidType;
                    const EscapeMemPlaceStatus place_status =
                        classify_escape_take_place(outside_positional[0], place_ty);
                    if (place_status == EscapeMemPlaceStatus::kOk) {
                        if (explicit_call_type_args.size() > 1) {
                            diag_(diag::Code::kGenericArityMismatch, e.span, "0 or 1",
                                  std::to_string(explicit_call_type_args.size()));
                            err_(e.span, "core::mem::take on optional owner-cell place accepts at most one explicit type argument");
                            check_all_arg_exprs_only();
                            return types_.error();
                        }

                        if (explicit_call_type_args.size() == 1 && explicit_call_type_args[0] != place_ty) {
                            add_escape_mem_diag(
                                e.span,
                                outside_positional[0]->span,
                                "core::mem::take explicit type argument must match the optional owner-cell place type",
                                "the first argument has type `" + types_.to_string(place_ty) + "`",
                                "place-first `core::mem::take` uses the destination `(~T)?` place type as its canonical value type",
                                "remove the explicit type argument, or change it to `" + types_.to_string(place_ty) + "`");
                            err_(e.span, "core::mem::take explicit type argument mismatch");
                            check_all_arg_exprs_only();
                            return types_.error();
                        }

                        if (auto root = root_place_symbol_(outside_positional[0]->expr)) {
                            mark_symbol_initialized_(*root);
                        }
                        cache_external_callee_(direct_ident_symbol, direct_sym.declared_type);
                        return place_ty;
                    }
                    if (place_status == EscapeMemPlaceStatus::kHardError) {
                        check_all_arg_exprs_only();
                        return types_.error();
                    }

                    const auto& place_expr = ast_.expr(outside_positional[0]->expr);
                    const bool used_amp =
                        place_expr.kind == ast::ExprKind::kUnary &&
                        place_expr.op == K::kAmp;
                    const ty::TypeId place_like_t =
                        used_amp && place_expr.a != ast::k_invalid_expr
                            ? check_expr_place_no_read_(place_expr.a)
                            : check_expr_place_no_read_(outside_positional[0]->expr);
                    if (!is_error_(place_like_t) &&
                        (is_place_expr_(outside_positional[0]->expr) || used_amp)) {
                        add_escape_mem_diag(
                            e.span,
                            outside_positional[0]->span,
                            "place-first core::mem::take is only available for optional owner-cell places",
                            "this argument has type `" + types_.to_string(place_like_t) + "`",
                            "this round only opens `core::mem::take` for writable projected `(~T)?` places",
                            "use consume-binding on a `(~T)?` place, or use `core::mem::replace`/`swap` for plain `~T` owner cells");
                        err_(e.span, "place-first core::mem::take only supports optional owner-cell places");
                        check_all_arg_exprs_only();
                        return types_.error();
                    }
                }

                add_escape_mem_diag(
                    e.span,
                    e.span,
                    "core::mem::take in this round only supports `mem::take(place)` for projected `(~T)?` places",
                    "ordinary non-owner optional `take` is not opened in this round",
                    "`take` is a shorthand for optional owner-cell extraction plus null writeback",
                    "pass a writable `(~T)?` place directly, or keep using consume-binding on the same place");
                err_(e.span, "core::mem::take only supports optional owner-cell places in this round");
                check_all_arg_exprs_only();
                return types_.error();
            }

            if (is_core_module_fn_(direct_ident_symbol, "mem", "replace")) {
                enum class EscapeMemPlaceStatus : uint8_t { kNotEscapePlace, kOk, kHardError };
                auto add_escape_mem_diag = [&](Span primary_span,
                                               Span label_span,
                                               std::string primary,
                                               std::string label,
                                               std::string note,
                                               std::string help) {
                    diag::Diagnostic d(diag::Severity::kError, diag::Code::kTypeErrorGeneric, primary_span);
                    d.add_arg(std::move(primary));
                    d.add_label(label_span, std::move(label));
                    if (!note.empty()) d.add_note(std::move(note));
                    if (!help.empty()) d.add_help(std::move(help));
                    if (diag_bag_) diag_bag_->add(std::move(d));
                };
                auto is_owner_cell_place_type = [&](ty::TypeId t) -> bool {
                    if (t == ty::kInvalidType || is_error_(t) || t >= types_.count()) return false;
                    const auto& tt = types_.get(t);
                    if (tt.kind == ty::Kind::kEscape) return true;
                    return tt.kind == ty::Kind::kOptional &&
                           tt.elem != ty::kInvalidType &&
                           tt.elem < types_.count() &&
                           types_.get(tt.elem).kind == ty::Kind::kEscape;
                };
                auto is_owner_carrying_place_type = [&](ty::TypeId t) -> bool {
                    return t != ty::kInvalidType && !is_error_(t) && type_contains_escape_(t);
                };
                auto owner_cell_label = [&](ty::TypeId t) -> std::string {
                    if (t != ty::kInvalidType &&
                        t < types_.count() &&
                        types_.get(t).kind == ty::Kind::kOptional) {
                        return "`(~T)?`";
                    }
                    return "`~T`";
                };
                auto owner_place_label = [&](ty::TypeId t) -> std::string {
                    if (is_owner_cell_place_type(t)) return owner_cell_label(t);
                    return "`" + types_.to_string(t) + "`";
                };
                auto classify_escape_mem_place = [&](const ast::Arg* arg,
                                                     std::string_view op_name,
                                                     bool require_readable,
                                                     ty::TypeId& out_place_ty) -> EscapeMemPlaceStatus {
                    out_place_ty = ty::kInvalidType;
                    if (arg == nullptr || arg->expr == ast::k_invalid_expr) {
                        return EscapeMemPlaceStatus::kNotEscapePlace;
                    }

                    const auto& arg_expr = ast_.expr(arg->expr);
                    if (arg_expr.kind == ast::ExprKind::kUnary &&
                        arg_expr.op == K::kAmp &&
                        arg_expr.a != ast::k_invalid_expr) {
                        const ty::TypeId operand_t = check_expr_place_no_read_(arg_expr.a);
                        if (!is_error_(operand_t) && is_owner_carrying_place_type(operand_t)) {
                            const std::string owner_label = owner_place_label(operand_t);
                            add_escape_mem_diag(
                                e.span,
                                arg->span,
                                "core::mem::" + std::string(op_name) + " on " + owner_label +
                                    " expects a place, not `&mut`",
                                "this argument already borrows an owner-carrying place",
                                owner_label + " remains move-only and is not source-level borrowable",
                                "pass the writable owner-carrying place directly, for example `mem::" +
                                    std::string(op_name) + "(slot, value)`");
                            err_(arg->span, "core::mem::replace/swap on owner-cell place expects a place, not `&mut`");
                            return EscapeMemPlaceStatus::kHardError;
                        }
                    }

                    if (!is_place_expr_(arg->expr)) return EscapeMemPlaceStatus::kNotEscapePlace;

                    const ty::TypeId place_t = check_expr_place_no_read_(arg->expr);
                    if (is_error_(place_t)) return EscapeMemPlaceStatus::kHardError;
                    if (!is_owner_carrying_place_type(place_t)) {
                        return EscapeMemPlaceStatus::kNotEscapePlace;
                    }

                    const auto root = root_place_symbol_(arg->expr);
                    if (!root.has_value() || !is_mutable_symbol_(*root)) {
                        const std::string owner_label = owner_place_label(place_t);
                        add_escape_mem_diag(
                            e.span,
                            arg->span,
                            "core::mem::" + std::string(op_name) + " requires a writable " + owner_label + " place",
                            "this owner-carrying place is not writable",
                            "replace/swap mutates the owner-carrying place in place",
                            "make the root binding mutable, or move through `(~T)?` consume-binding instead");
                        err_(arg->span, "core::mem::replace/swap requires writable owner-cell place");
                        return EscapeMemPlaceStatus::kHardError;
                    }

                    if (require_readable && !ensure_symbol_readable_(*root, arg->span)) {
                        return EscapeMemPlaceStatus::kHardError;
                    }

                    if (is_global_like_symbol_(*root)) {
                        diag_(diag::Code::kMoveFromGlobalOrStaticForbidden, arg->span, sym_.symbol(*root).name);
                        err_(arg->span, "move from global/static owner-carrying place is not allowed");
                        return EscapeMemPlaceStatus::kHardError;
                    }

                    out_place_ty = place_t;
                    return EscapeMemPlaceStatus::kOk;
                };

                if (form == CallForm::kPositionalOnly &&
                    outside_positional.size() == 2 &&
                    outside_labeled.empty()) {
                    ty::TypeId place_ty = ty::kInvalidType;
                    const EscapeMemPlaceStatus place_status =
                        classify_escape_mem_place(outside_positional[0], "replace", /*require_readable=*/true, place_ty);
                    if (place_status == EscapeMemPlaceStatus::kOk) {
                        if (explicit_call_type_args.size() > 1) {
                            diag_(diag::Code::kGenericArityMismatch, e.span, "0 or 1",
                                  std::to_string(explicit_call_type_args.size()));
                            err_(e.span, "core::mem::replace on owner-cell place accepts at most one explicit type argument");
                            check_all_arg_exprs_only();
                            return types_.error();
                        }

                        ty::TypeId value_ty = place_ty;
                        if (explicit_call_type_args.size() == 1) {
                            value_ty = explicit_call_type_args[0];
                            if (value_ty != place_ty) {
                                const std::string owner_label = owner_place_label(place_ty);
                                add_escape_mem_diag(
                                    e.span,
                                    outside_positional[0]->span,
                                    "core::mem::replace explicit type argument must match the owner-cell place type",
                                    "the first argument has type `" + types_.to_string(place_ty) + "`",
                                    "place-first `core::mem::replace` for " + owner_label + " uses the destination place type as its canonical value type",
                                    "remove the explicit type argument, or change it to `" + types_.to_string(place_ty) + "`");
                                err_(e.span, "core::mem::replace explicit type argument mismatch");
                                check_all_arg_exprs_only();
                                return types_.error();
                            }
                        }

                        const CoercionPlan value_plan = classify_assign_with_coercion_(
                            AssignSite::CallArg, value_ty, outside_positional[1]->expr, outside_positional[1]->span);
                        if (!value_plan.ok) {
                            const std::string owner_label = owner_place_label(value_ty);
                            add_escape_mem_diag(
                                e.span,
                                outside_positional[1]->span,
                                "core::mem::replace second argument must be assignable to the destination owner-carrying place",
                                "expected `" + types_.to_string(value_ty) + "` here",
                                "place-first `core::mem::replace` moves a replacement value into the existing " + owner_label + " place",
                                "pass a value of type `" + types_.to_string(value_ty) + "` or use `(~T)?` consume-binding to extract one");
                            err_(e.span, "core::mem::replace second argument mismatch");
                            check_all_arg_exprs_only();
                            return types_.error();
                        }

                        mark_expr_move_consumed_(outside_positional[1]->expr, value_ty, outside_positional[1]->span);
                        if (auto lhs_root = root_place_symbol_(outside_positional[0]->expr)) {
                            mark_symbol_initialized_(*lhs_root);
                        }
                        cache_external_callee_(direct_ident_symbol, direct_sym.declared_type);
                        return value_ty;
                    }
                    if (place_status == EscapeMemPlaceStatus::kHardError) {
                        check_all_arg_exprs_only();
                        return types_.error();
                    }

                    const auto& place_expr = ast_.expr(outside_positional[0]->expr);
                    if (place_expr.kind != ast::ExprKind::kUnary || place_expr.op != K::kAmp) {
                        const ty::TypeId place_like_t = check_expr_place_no_read_(outside_positional[0]->expr);
                        if (!is_error_(place_like_t) && is_place_expr_(outside_positional[0]->expr)) {
                            add_escape_mem_diag(
                                e.span,
                                outside_positional[0]->span,
                                "place-first core::mem::replace is only available for owner-carrying places",
                                "this place has type `" + types_.to_string(place_like_t) + "`",
                                "ordinary values still use the existing `&mut T` memory helper path",
                                "use `mem::replace<T>(&mut place, value)` for ordinary `T`, or pass a place whose type carries `~` ownership directly");
                            err_(e.span, "place-first core::mem::replace only supports owner-carrying places");
                            check_all_arg_exprs_only();
                            return types_.error();
                        }
                    }
                }

                if (form != CallForm::kPositionalOnly || outside_positional.size() != 2 || !outside_labeled.empty()) {
                    return fail_core_shape_("core::mem::replace expects positional(&mut T, T)");
                }
                if (explicit_call_type_args.size() != 1) {
                    diag_(diag::Code::kGenericArityMismatch, e.span, "1",
                          std::to_string(explicit_call_type_args.size()));
                    err_(e.span, "core::mem::replace requires one explicit type argument");
                    check_all_arg_exprs_only();
                    return types_.error();
                }
                const ty::TypeId value_ty = explicit_call_type_args[0];
                const ty::TypeId place_ty = types_.make_borrow(value_ty, /*is_mut=*/true);
                const CoercionPlan place_plan = classify_assign_with_coercion_(
                    AssignSite::CallArg, place_ty, outside_positional[0]->expr, outside_positional[0]->span);
                if (!place_plan.ok) {
                    return fail_core_shape_("core::mem::replace first argument must be &mut T");
                }
                const CoercionPlan value_plan = classify_assign_with_coercion_(
                    AssignSite::CallArg, value_ty, outside_positional[1]->expr, outside_positional[1]->span);
                if (!value_plan.ok) {
                    return fail_core_shape_("core::mem::replace second argument must be assignable to T");
                }
                cache_external_callee_(direct_ident_symbol, direct_sym.declared_type);
                return value_ty;
            }

            if (is_core_module_fn_(direct_ident_symbol, "mem", "swap")) {
                enum class EscapeMemPlaceStatus : uint8_t { kNotEscapePlace, kOk, kHardError };
                auto add_escape_mem_diag = [&](Span primary_span,
                                               Span label_span,
                                               std::string primary,
                                               std::string label,
                                               std::string note,
                                               std::string help) {
                    diag::Diagnostic d(diag::Severity::kError, diag::Code::kTypeErrorGeneric, primary_span);
                    d.add_arg(std::move(primary));
                    d.add_label(label_span, std::move(label));
                    if (!note.empty()) d.add_note(std::move(note));
                    if (!help.empty()) d.add_help(std::move(help));
                    if (diag_bag_) diag_bag_->add(std::move(d));
                };
                auto is_owner_cell_place_type = [&](ty::TypeId t) -> bool {
                    if (t == ty::kInvalidType || is_error_(t) || t >= types_.count()) return false;
                    const auto& tt = types_.get(t);
                    if (tt.kind == ty::Kind::kEscape) return true;
                    return tt.kind == ty::Kind::kOptional &&
                           tt.elem != ty::kInvalidType &&
                           tt.elem < types_.count() &&
                           types_.get(tt.elem).kind == ty::Kind::kEscape;
                };
                auto is_owner_carrying_place_type = [&](ty::TypeId t) -> bool {
                    return t != ty::kInvalidType && !is_error_(t) && type_contains_escape_(t);
                };
                auto owner_cell_label = [&](ty::TypeId t) -> std::string {
                    if (t != ty::kInvalidType &&
                        t < types_.count() &&
                        types_.get(t).kind == ty::Kind::kOptional) {
                        return "`(~T)?`";
                    }
                    return "`~T`";
                };
                auto owner_place_label = [&](ty::TypeId t) -> std::string {
                    if (is_owner_cell_place_type(t)) return owner_cell_label(t);
                    return "`" + types_.to_string(t) + "`";
                };
                auto classify_escape_mem_place = [&](const ast::Arg* arg,
                                                     ty::TypeId& out_place_ty) -> EscapeMemPlaceStatus {
                    out_place_ty = ty::kInvalidType;
                    if (arg == nullptr || arg->expr == ast::k_invalid_expr) {
                        return EscapeMemPlaceStatus::kNotEscapePlace;
                    }

                    const auto& arg_expr = ast_.expr(arg->expr);
                    if (arg_expr.kind == ast::ExprKind::kUnary &&
                        arg_expr.op == K::kAmp &&
                        arg_expr.a != ast::k_invalid_expr) {
                        const ty::TypeId operand_t = check_expr_place_no_read_(arg_expr.a);
                        if (!is_error_(operand_t) && is_owner_carrying_place_type(operand_t)) {
                            const std::string owner_label = owner_place_label(operand_t);
                            add_escape_mem_diag(
                                e.span,
                                arg->span,
                                "core::mem::swap on " + owner_label + " expects places, not `&mut`",
                                "this argument already borrows an owner-carrying place",
                                owner_label + " remains move-only and is not source-level borrowable",
                                "pass the writable owner-carrying places directly, for example `mem::swap(lhs, rhs)`");
                            err_(arg->span, "core::mem::swap on owner-cell place expects places, not `&mut`");
                            return EscapeMemPlaceStatus::kHardError;
                        }
                    }

                    if (!is_place_expr_(arg->expr)) return EscapeMemPlaceStatus::kNotEscapePlace;

                    const ty::TypeId place_t = check_expr_place_no_read_(arg->expr);
                    if (is_error_(place_t)) return EscapeMemPlaceStatus::kHardError;
                    if (!is_owner_carrying_place_type(place_t)) {
                        return EscapeMemPlaceStatus::kNotEscapePlace;
                    }

                    const auto root = root_place_symbol_(arg->expr);
                    if (!root.has_value() || !is_mutable_symbol_(*root)) {
                        const std::string owner_label = owner_place_label(place_t);
                        add_escape_mem_diag(
                            e.span,
                            arg->span,
                            "core::mem::swap requires writable " + owner_label + " places",
                            "this owner-carrying place is not writable",
                            "swap mutates both owner-carrying places in place",
                            "make the root binding mutable, or move through `(~T)?` consume-binding instead");
                        err_(arg->span, "core::mem::swap requires writable owner-cell places");
                        return EscapeMemPlaceStatus::kHardError;
                    }
                    if (!ensure_symbol_readable_(*root, arg->span)) {
                        return EscapeMemPlaceStatus::kHardError;
                    }
                    if (is_global_like_symbol_(*root)) {
                        diag_(diag::Code::kMoveFromGlobalOrStaticForbidden, arg->span, sym_.symbol(*root).name);
                        err_(arg->span, "move from global/static owner-carrying place is not allowed");
                        return EscapeMemPlaceStatus::kHardError;
                    }

                    out_place_ty = place_t;
                    return EscapeMemPlaceStatus::kOk;
                };

                if (form == CallForm::kPositionalOnly &&
                    outside_positional.size() == 2 &&
                    outside_labeled.empty()) {
                    ty::TypeId lhs_place_ty = ty::kInvalidType;
                    ty::TypeId rhs_place_ty = ty::kInvalidType;
                    const EscapeMemPlaceStatus lhs_status =
                        classify_escape_mem_place(outside_positional[0], lhs_place_ty);
                    const EscapeMemPlaceStatus rhs_status =
                        classify_escape_mem_place(outside_positional[1], rhs_place_ty);

                    if (lhs_status == EscapeMemPlaceStatus::kHardError ||
                        rhs_status == EscapeMemPlaceStatus::kHardError) {
                        check_all_arg_exprs_only();
                        return types_.error();
                    }

                    if (lhs_status == EscapeMemPlaceStatus::kOk ||
                        rhs_status == EscapeMemPlaceStatus::kOk) {
                        if (lhs_status != EscapeMemPlaceStatus::kOk ||
                            rhs_status != EscapeMemPlaceStatus::kOk) {
                            add_escape_mem_diag(
                                e.span,
                                e.span,
                                "core::mem::swap on owner-carrying places requires both arguments to be writable owner-carrying places",
                                "swap needs two owner-carrying places of the same type",
                                "mixed owner-carrying/non-owner swap is not supported in this round",
                                "pass two writable places of the same owner-carrying type, or use the existing `&mut T` swap path for ordinary values");
                            err_(e.span, "core::mem::swap requires two writable owner-carrying places");
                            check_all_arg_exprs_only();
                            return types_.error();
                        }

                        if (explicit_call_type_args.size() > 1) {
                            diag_(diag::Code::kGenericArityMismatch, e.span, "0 or 1",
                                  std::to_string(explicit_call_type_args.size()));
                            err_(e.span, "core::mem::swap on owner-cell places accepts at most one explicit type argument");
                            check_all_arg_exprs_only();
                            return types_.error();
                        }
                        if (lhs_place_ty != rhs_place_ty) {
                            add_escape_mem_diag(
                                e.span,
                                outside_positional[1]->span,
                                "core::mem::swap requires both owner-cell places to have the same type",
                                "this place has type `" + types_.to_string(rhs_place_ty) + "`",
                                "swap exchanges owner cells without coercion",
                                "make both places use the same owner-cell type before swapping");
                            err_(e.span, "core::mem::swap type mismatch");
                            check_all_arg_exprs_only();
                            return types_.error();
                        }
                        if (explicit_call_type_args.size() == 1 &&
                            explicit_call_type_args[0] != lhs_place_ty) {
                            add_escape_mem_diag(
                                e.span,
                                outside_positional[0]->span,
                                "core::mem::swap explicit type argument must match the owner-cell place type",
                                "the first argument has type `" + types_.to_string(lhs_place_ty) + "`",
                                "place-first `core::mem::swap` uses the destination place type as its canonical value type",
                                "remove the explicit type argument, or change it to `" + types_.to_string(lhs_place_ty) + "`");
                            err_(e.span, "core::mem::swap explicit type argument mismatch");
                            check_all_arg_exprs_only();
                            return types_.error();
                        }

                        if (auto lhs_root = root_place_symbol_(outside_positional[0]->expr)) {
                            mark_symbol_initialized_(*lhs_root);
                        }
                        if (auto rhs_root = root_place_symbol_(outside_positional[1]->expr)) {
                            mark_symbol_initialized_(*rhs_root);
                        }
                        cache_external_callee_(direct_ident_symbol, direct_sym.declared_type);
                        return types_.builtin(ty::Builtin::kUnit);
                    }

                    const auto& lhs_expr = ast_.expr(outside_positional[0]->expr);
                    if ((lhs_expr.kind != ast::ExprKind::kUnary || lhs_expr.op != K::kAmp) &&
                        is_place_expr_(outside_positional[0]->expr)) {
                        const ty::TypeId lhs_place_like_t = check_expr_place_no_read_(outside_positional[0]->expr);
                        if (!is_error_(lhs_place_like_t)) {
                            add_escape_mem_diag(
                                e.span,
                                outside_positional[0]->span,
                                "place-first core::mem::swap is only available for owner-carrying places",
                                "this place has type `" + types_.to_string(lhs_place_like_t) + "`",
                                "ordinary values still use the existing `&mut T` memory helper path",
                                "use `mem::swap<T>(&mut lhs, &mut rhs)` for ordinary `T`, or pass places whose type carries `~` ownership directly");
                            err_(e.span, "place-first core::mem::swap only supports owner-carrying places");
                            check_all_arg_exprs_only();
                            return types_.error();
                        }
                    }
                }

                if (form != CallForm::kPositionalOnly || outside_positional.size() != 2 || !outside_labeled.empty()) {
                    return fail_core_shape_("core::mem::swap expects positional(&mut T, &mut T)");
                }
                if (explicit_call_type_args.size() != 1) {
                    diag_(diag::Code::kGenericArityMismatch, e.span, "1",
                          std::to_string(explicit_call_type_args.size()));
                    err_(e.span, "core::mem::swap requires one explicit type argument");
                    check_all_arg_exprs_only();
                    return types_.error();
                }
                const ty::TypeId value_ty = explicit_call_type_args[0];
                const ty::TypeId place_ty = types_.make_borrow(value_ty, /*is_mut=*/true);
                const CoercionPlan lhs_plan = classify_assign_with_coercion_(
                    AssignSite::CallArg, place_ty, outside_positional[0]->expr, outside_positional[0]->span);
                const CoercionPlan rhs_plan = classify_assign_with_coercion_(
                    AssignSite::CallArg, place_ty, outside_positional[1]->expr, outside_positional[1]->span);
                if (!lhs_plan.ok || !rhs_plan.ok) {
                    return fail_core_shape_("core::mem::swap arguments must both be &mut T");
                }
                cache_external_callee_(direct_ident_symbol, direct_sym.declared_type);
                return types_.builtin(ty::Builtin::kUnit);
            }
        }

        // ------------------------------------------------------------
        // 2.75) indirect C ABI call via function value / fnptr alias
        // ------------------------------------------------------------
        if (overload_decl_ids.empty()) {
            ty::TypeId indirect_callee_t = callee_t;
            if ((indirect_callee_t == ty::kInvalidType ||
                 resolve_c_abi_fn_type(indirect_callee_t) == ty::kInvalidType) &&
                e.a != ast::k_invalid_expr &&
                (size_t)e.a < ast_.exprs().size()) {
                const auto& callee_expr = ast_.expr(e.a);
                if (callee_expr.kind == ast::ExprKind::kIdent) {
                    if (auto sid = lookup_symbol_(callee_expr.text)) {
                        if (*sid < sym_.symbols().size()) {
                            const auto& ss = sym_.symbol(*sid);
                            if (ss.declared_type != ty::kInvalidType) {
                                indirect_callee_t = ss.declared_type;
                            }
                        }
                    }
                }
            }

            const ty::TypeId c_abi_fn_t = resolve_c_abi_fn_type(indirect_callee_t);
            bool callee_is_c_abi = false;
            bool callee_is_c_variadic = false;
            ty::CCallConv callee_c_callconv = ty::CCallConv::kDefault;
            classify_c_abi_fn_type(indirect_callee_t, callee_is_c_abi, callee_is_c_variadic, callee_c_callconv);
            if (callee_is_c_abi) {
                if (form != CallForm::kPositionalOnly) {
                    diag_(diag::Code::kCAbiCallPositionalOnly, e.span);
                    err_(e.span, "C ABI call currently supports positional arguments only");
                    check_all_arg_exprs_only();
                    return types_.error();
                }
                std::vector<ast::ExprId> arg_exprs{};
                arg_exprs.reserve(outside_positional.size());
                for (const auto* a : outside_positional) {
                    if (a != nullptr) arg_exprs.push_back(a->expr);
                }
                return check_c_abi_call_with_positions(c_abi_fn_t, cimport_callee_symbol, arg_exprs, e.span);
            }
        }

        // ------------------------------------------------------------
        // 3) fallback: overload 집합을 못 찾으면 function type call-shape로 검사
        // ------------------------------------------------------------
        if (overload_decl_ids.empty()) {
            const ty::TypeId indirect_fn_t = resolve_any_fn_type(callee_t);
            if (indirect_fn_t == ty::kInvalidType || types_.get(indirect_fn_t).kind != ty::Kind::kFn) {
                check_all_arg_exprs_only();
                return fallback_ret;
            }

            const uint32_t total_cnt = types_.get(indirect_fn_t).param_count;
            const uint32_t pos_cnt = types_.fn_positional_count(indirect_fn_t);
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
                p.type = types_.fn_param_at(indirect_fn_t, i);
                p.name = std::string(types_.fn_param_label_at(indirect_fn_t, i));
                p.has_default = types_.fn_param_has_default_at(indirect_fn_t, i);

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

            if (types_.fn_is_throwing(indirect_fn_t) &&
                !types_.fn_is_c_abi(indirect_fn_t) &&
                !in_try_expr_context_ &&
                !fn_ctx_.is_throwing) {
                diag_(diag::Code::kThrowingCallRequiresTryExpr, e.span);
                err_(e.span, "non-throwing function must use try expression for throwing function value call");
                check_all_arg_exprs_only();
                return types_.error();
            }

            const auto check_arg_against_type = [&](const ast::Arg& a, ty::TypeId expected, uint32_t idx) {
                if (a.expr == ast::k_invalid_expr) {
                    diag_(diag::Code::kTypeArgTypeMismatch, a.span,
                        std::to_string(idx), types_.to_string(expected), "<missing>");
                    err_(a.span, "argument type mismatch");
                    return;
                }
                if (is_c_char_ptr_type(expected) && is_plain_string_literal_expr(a.expr)) {
                    return;
                }
                if (is_c_char_ptr_type(expected) && is_core_ext_cstr_type(check_expr_(a.expr))) {
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

            cache_call_fn_type_(indirect_fn_t);
            return types_.get(indirect_fn_t).ret;
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
            bool has_type_mismatch = false;
            std::string eq_lhs{};
            std::string eq_rhs{};
            std::string eq_concrete_lhs{};
            std::string eq_concrete_rhs{};
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
            bool is_imported_template = false;
            bool needs_materialization = false;
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
            c.is_imported_template =
                imported_fn_template_sid_set_.find(sid) != imported_fn_template_sid_set_.end();
            c.is_generic = (def.fn_generic_param_count > 0);
            c.needs_materialization = c.is_generic || c.is_imported_template;
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
                GenericConstraintFailure failure{};
                if (evaluate_generic_constraint_(cc, c.generic_bindings, e.span, failure)) continue;

                switch (failure.kind) {
                    case GenericConstraintFailure::Kind::kUnknownTypeParam:
                        generic_failure.has_infer_fail = true;
                        break;
                    case GenericConstraintFailure::Kind::kProtoNotFound:
                        generic_failure.has_proto_not_found = true;
                        generic_failure.proto_not_found = failure.rhs_proto;
                        break;
                    case GenericConstraintFailure::Kind::kProtoUnsatisfied:
                        generic_failure.has_unsatisfied = true;
                        generic_failure.unsat_type_param = failure.lhs_type_param;
                        generic_failure.unsat_proto = failure.rhs_proto;
                        generic_failure.unsat_concrete = failure.concrete_lhs;
                        break;
                    case GenericConstraintFailure::Kind::kTypeMismatch:
                        generic_failure.has_type_mismatch = true;
                        generic_failure.eq_lhs = failure.lhs_type_param;
                        generic_failure.eq_rhs = failure.rhs_type_repr;
                        generic_failure.eq_concrete_lhs = failure.concrete_lhs;
                        generic_failure.eq_concrete_rhs = failure.concrete_rhs;
                        break;
                    case GenericConstraintFailure::Kind::kNone:
                        break;
                }
                c.generic_viable = false;
                break;
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

        auto emit_generic_call_failure_diag_ = [&](diag::Code code,
                                                   std::string_view a0 = {},
                                                   std::string_view a1 = {},
                                                   std::string_view a2 = {},
                                                   std::string_view a3 = {}) {
            if (!diag_bag_) return;
            diag::Diagnostic d(diag::Severity::kError, code, e.span);
            if (!a0.empty()) d.add_arg(a0);
            if (!a1.empty()) d.add_arg(a1);
            if (!a2.empty()) d.add_arg(a2);
            if (!a3.empty()) d.add_arg(a3);
            switch (code) {
                case diag::Code::kGenericConstraintProtoNotFound:
                    d.add_note("proto-target import ergonomics only applies to public exported proto targets");
                    d.add_note("generic call '" + callee_name + "' depends on a proto target that could not be resolved");
                    d.add_help("add an explicit import for the proto, or export the proto through a public path");
                    break;
                case diag::Code::kGenericConstraintUnsatisfied:
                    d.add_note("constraint checking happens after substituting inferred or explicit generic arguments");
                    d.add_help("choose type arguments that satisfy the required proto, or relax the constraint");
                    break;
                case diag::Code::kGenericConstraintTypeMismatch:
                    d.add_note("generic equality constraints are checked after the concrete type tuple is fixed");
                    d.add_help("make both sides reduce to the same concrete type");
                    break;
                case diag::Code::kGenericTypeArgInferenceFailed:
                    d.add_note("the compiler could not infer all generic arguments from this call form");
                    d.add_help("add explicit type arguments to the call");
                    break;
                default:
                    break;
            }
            diag_bag_->add(std::move(d));
        };

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
                emit_generic_call_failure_diag_(
                    diag::Code::kGenericConstraintProtoNotFound,
                    generic_failure.proto_not_found
                );
                err_(e.span, "generic constraint references unknown proto");
                check_all_arg_exprs_only();
                return fallback_ret;
            }
            if (generic_failure.has_unsatisfied) {
                emit_generic_call_failure_diag_(
                    diag::Code::kGenericConstraintUnsatisfied,
                    generic_failure.unsat_type_param,
                    generic_failure.unsat_proto,
                    generic_failure.unsat_concrete
                );
                err_(e.span, "generic constraint is not satisfied");
                check_all_arg_exprs_only();
                return fallback_ret;
            }
            if (generic_failure.has_type_mismatch) {
                emit_generic_call_failure_diag_(
                    diag::Code::kGenericConstraintTypeMismatch,
                    generic_failure.eq_lhs,
                    generic_failure.eq_rhs,
                    generic_failure.eq_concrete_lhs,
                    generic_failure.eq_concrete_rhs
                );
                err_(e.span, "generic equality constraint is not satisfied");
                check_all_arg_exprs_only();
                return fallback_ret;
            }
            if (generic_failure.has_infer_fail) {
                emit_generic_call_failure_diag_(diag::Code::kGenericTypeArgInferenceFailed, callee_name);
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
                emit_generic_call_failure_diag_(
                    diag::Code::kGenericConstraintProtoNotFound,
                    generic_failure.proto_not_found
                );
                err_(e.span, "generic constraint references unknown proto");
                check_all_arg_exprs_only();
                return fallback_ret;
            }
            if (generic_failure.has_unsatisfied) {
                emit_generic_call_failure_diag_(
                    diag::Code::kGenericConstraintUnsatisfied,
                    generic_failure.unsat_type_param,
                    generic_failure.unsat_proto,
                    generic_failure.unsat_concrete
                );
                err_(e.span, "generic constraint is not satisfied");
                check_all_arg_exprs_only();
                return fallback_ret;
            }
            if (generic_failure.has_type_mismatch) {
                emit_generic_call_failure_diag_(
                    diag::Code::kGenericConstraintTypeMismatch,
                    generic_failure.eq_lhs,
                    generic_failure.eq_rhs,
                    generic_failure.eq_concrete_lhs,
                    generic_failure.eq_concrete_rhs
                );
                err_(e.span, "generic equality constraint is not satisfied");
                check_all_arg_exprs_only();
                return fallback_ret;
            }
            if (generic_failure.has_infer_fail) {
                emit_generic_call_failure_diag_(diag::Code::kGenericTypeArgInferenceFailed, callee_name);
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
        if (selected.needs_materialization) {
            MonoRequest req{};
            req.templ.template_sid = selected.decl_id;
            req.templ.source = selected.is_imported_template
                ? MonoTemplateRef::SourceKind::kImportedFn
                : MonoTemplateRef::SourceKind::kLocalFn;
            req.templ.producer_bundle = current_bundle_name_();
            req.templ.template_symbol = fn_qualified_name_by_stmt_.count(selected.decl_id)
                ? fn_qualified_name_by_stmt_.at(selected.decl_id)
                : std::string(ast_.stmt(selected.decl_id).name);
            if (selected.is_imported_template) {
                if (auto it = imported_fn_template_index_by_sid_.find(selected.decl_id);
                    it != imported_fn_template_index_by_sid_.end() &&
                    it->second < explicit_imported_fn_templates_.size()) {
                    const auto& imported = explicit_imported_fn_templates_[it->second];
                    req.templ.producer_bundle = imported.producer_bundle;
                    req.templ.template_symbol = imported.lookup_name.empty()
                        ? req.templ.template_symbol
                        : imported.lookup_name;
                    req.templ.link_name = imported.link_name;
                }
            }
            req.concrete_args = selected.generic_concrete_args;
            req.target_lane = "default";
            req.abi_lane = "static";
            auto inst = ensure_monomorphized_free_function_(req, e.span);
            if (!inst.has_value()) {
                check_all_arg_exprs_only();
                return types_.error();
            }
            selected_decl_sid = inst->decl_sid;
        }
        if (call_expr_id != ast::k_invalid_expr &&
            call_expr_id < expr_overload_target_cache_.size()) {
            expr_overload_target_cache_[call_expr_id] = selected_decl_sid;
        }
        if (call_expr_id != ast::k_invalid_expr &&
            call_expr_id < expr_external_callee_symbol_cache_.size()) {
            expr_external_callee_symbol_cache_[call_expr_id] = sema::SymbolTable::kNoScope;
        }
        if (call_expr_id != ast::k_invalid_expr &&
            call_expr_id < expr_external_callee_type_cache_.size()) {
            expr_external_callee_type_cache_[call_expr_id] = ty::kInvalidType;
        }
        if (call_expr_id != ast::k_invalid_expr &&
            call_expr_id < expr_external_receiver_expr_cache_.size()) {
            expr_external_receiver_expr_cache_[call_expr_id] = ast::k_invalid_expr;
        }
        if (call_expr_id != ast::k_invalid_expr &&
            call_expr_id < expr_ctor_owner_type_cache_.size()) {
            expr_ctor_owner_type_cache_[call_expr_id] = is_ctor_call ? ctor_owner_type : ty::kInvalidType;
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

        if (selected_is_c_abi) {
            std::vector<ast::ExprId> arg_exprs{};
            arg_exprs.reserve(outside_positional.size());
            for (const auto* a : outside_positional) {
                if (a != nullptr) arg_exprs.push_back(a->expr);
            }

            ty::TypeId c_abi_fn_t = callee_t;
            if (selected_decl_sid != ast::k_invalid_stmt &&
                (size_t)selected_decl_sid < ast_.stmts().size()) {
                const auto& selected_decl = ast_.stmt(selected_decl_sid);
                if (selected_decl.type != ty::kInvalidType &&
                    selected_decl.type < types_.count() &&
                    types_.get(selected_decl.type).kind == ty::Kind::kFn) {
                    c_abi_fn_t = selected_decl.type;
                }
            }
            cache_call_fn_type_(c_abi_fn_t);
            return check_c_abi_call_with_positions(c_abi_fn_t,
                                                   sema::SymbolTable::kNoScope,
                                                   arg_exprs,
                                                   e.span);
        }

        if (selected_decl_sid != ast::k_invalid_stmt && (size_t)selected_decl_sid < ast_.stmts().size()) {
            const auto& selected_decl = ast_.stmt(selected_decl_sid);
            if (selected_decl.kind == ast::StmtKind::kFnDecl &&
                selected_decl.is_throwing &&
                !in_try_expr_context_ &&
                !fn_ctx_.is_throwing) {
                diag_(diag::Code::kThrowingCallRequiresTryExpr, e.span);
                err_(e.span, "non-throwing function must use try expression for throwing call");
                check_all_arg_exprs_only();
                return types_.error();
            }
            if (selected_decl.kind == ast::StmtKind::kFnDecl &&
                selected_decl.type != ty::kInvalidType &&
                types_.is_fn(selected_decl.type)) {
                cache_call_fn_type_(selected_decl.type);
            }
        }

        const auto check_arg_against_param_final = [&](const ast::Arg& a, const ParamInfo& p) {
            if (a.expr == ast::k_invalid_expr) {
                diag_(diag::Code::kTypeArgTypeMismatch, a.span,
                    std::to_string(p.decl_index), types_.to_string(p.type), "<missing>");
                err_(a.span, "argument type mismatch for parameter '" + p.name + "'");
                return;
            }

            if ((size_t)a.expr < ast_.exprs().size()) {
                const auto& ax = ast_.expr(a.expr);
                if (selected_is_c_abi &&
                    ax.kind == ast::ExprKind::kStringLit && ax.string_is_format) {
                    diag_(diag::Code::kCAbiFormatStringForbidden, a.span);
                    err_(a.span, "format-string literal is forbidden in C ABI call");
                    return;
                }
                if (is_c_char_ptr_type(p.type) && is_plain_string_literal_expr(a.expr)) {
                    return;
                }
                if (is_c_char_ptr_type(p.type) && is_core_ext_cstr_type(check_expr_(a.expr))) {
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

        if (call_expr_id != ast::k_invalid_expr &&
            call_expr_id < expr_call_fn_type_cache_.size() &&
            expr_call_fn_type_cache_[call_expr_id] == ty::kInvalidType &&
            callee_t != ty::kInvalidType &&
            types_.is_fn(callee_t)) {
            cache_call_fn_type_(callee_t);
        }
        return selected.ret;
    }
