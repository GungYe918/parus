// frontend/src/oir/oir_builder.cpp
#include <parus/oir/Builder.hpp>
#include <parus/oir/OIR.hpp>

#include <parus/ast/Nodes.hpp>
#include <parus/cimport/TypeReprNormalize.hpp>
#include <parus/common/ModulePath.hpp>
#include <parus/sir/Verify.hpp>
#include <parus/syntax/TokenKind.hpp>

#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <string>
#include <cctype>
#include <optional>
#include <sstream>
#include <algorithm>


namespace parus::oir {

    namespace {

        enum class ActorEnterMode : uint32_t {
            kInit = 0,
            kSub = 1,
            kPub = 2,
        };

        struct ParsedCImportWrapperPayload {
            bool is_wrapper = false;
            std::string callee_link_name{};
            std::vector<uint32_t> arg_map{};
        };

        struct ParsedExternalRecordLayout {
            bool ok = false;
            FieldLayout layout = FieldLayout::None;
            uint32_t size = 0;
            uint32_t align = 0;
            std::vector<FieldMemberLayout> members{};
        };

        struct ParsedExternalParusFieldMember {
            std::string name{};
            parus::ty::TypeId type = parus::ty::kInvalidType;
        };

        struct ParsedExternalParusFieldDecl {
            bool ok = false;
            FieldLayout layout = FieldLayout::None;
            uint32_t explicit_align = 0;
            std::vector<std::string> generic_params{};
            std::vector<ParsedExternalParusFieldMember> members{};
        };

        std::string normalize_symbol_fragment_(std::string_view in);
        uint64_t fnv1a64_(std::string_view s);

        bool known_concrete_leaf_type_name_without_symbols_(
            const parus::sir::Module& sir,
            const parus::ty::TypePool& types,
            std::string_view leaf
        );

        bool leaf_name_resolves_to_concrete_type_(
            const parus::sir::Module& sir,
            const parus::ty::TypePool& types,
            const parus::sema::SymbolTable* sym,
            std::string_view leaf
        );

        bool parse_u32_sv_(std::string_view text, uint32_t& out) {
            out = 0;
            if (text.empty()) return false;
            uint64_t value = 0;
            for (const char ch : text) {
                if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
                value = value * 10u + static_cast<uint64_t>(ch - '0');
                if (value > 0xFFFF'FFFFull) return false;
            }
            out = static_cast<uint32_t>(value);
            return true;
        }

        std::string payload_unescape_value_(std::string_view raw) {
            std::string out{};
            out.reserve(raw.size());
            for (size_t i = 0; i < raw.size(); ++i) {
                const char c = raw[i];
                if (c == '%' && i + 2 < raw.size()) {
                    auto hexv = [](char ch) -> int {
                        if (ch >= '0' && ch <= '9') return ch - '0';
                        if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
                        if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
                        return -1;
                    };
                    const int hi = hexv(raw[i + 1]);
                    const int lo = hexv(raw[i + 2]);
                    if (hi >= 0 && lo >= 0) {
                        out.push_back(static_cast<char>((hi << 4) | lo));
                        i += 2;
                        continue;
                    }
                }
                out.push_back(c);
            }
            return out;
        }

        std::string last_path_segment_(std::string_view qname) {
            const size_t pos = qname.rfind("::");
            return (pos == std::string_view::npos)
                ? std::string(qname)
                : std::string(qname.substr(pos + 2));
        }

        std::string build_parus_link_name_from_qname_and_sig_(
            std::string_view bundle_name,
            std::string_view qname,
            std::string_view sig_repr,
            std::string_view mode = "none"
        ) {
            std::string path = "_";
            std::string base = std::string(qname);
            if (const size_t pos = base.rfind("::"); pos != std::string::npos) {
                path = base.substr(0, pos);
                base = base.substr(pos + 2);
                size_t p = 0;
                while ((p = path.find("::", p)) != std::string::npos) {
                    path.replace(p, 2, "__");
                    p += 2;
                }
            }

            const std::string bundle =
                bundle_name.empty() ? std::string("main") : std::string(bundle_name);
            const std::string sig =
                sig_repr.empty() ? std::string("def(?)") : std::string(sig_repr);
            const std::string canonical =
                "bundle=" + bundle + "|path=" + path +
                "|name=" + base +
                "|mode=" + std::string(mode) + "|recv=none|sig=" + sig;
            std::ostringstream hs;
            hs << std::hex << fnv1a64_(canonical);

            return "p$" + normalize_symbol_fragment_(bundle) + "$" +
                normalize_symbol_fragment_(path) + "$" +
                normalize_symbol_fragment_(base) + "$M" +
                normalize_symbol_fragment_(mode) + "$Rnone$S" +
                normalize_symbol_fragment_(sig) + "$H" + hs.str();
        }

        std::string relativize_external_fn_sig_repr_(
            const parus::sema::Symbol& ss,
            TypeId fn_type,
            const parus::ty::TypePool& types
        ) {
            std::string sig = types.to_export_string(fn_type);
            if (sig.empty()) return sig;

            auto replace_all_ = [](std::string& text,
                                   std::string_view from,
                                   std::string_view to) {
                if (from.empty()) return;
                size_t pos = 0;
                while ((pos = text.find(from, pos)) != std::string::npos) {
                    text.replace(pos, from.size(), to);
                    pos += to.size();
                }
            };

            if (!ss.decl_module_head.empty()) {
                const std::string full_module_prefix = ss.decl_module_head + "::";
                replace_all_(sig, full_module_prefix, "");
            }
            if (!ss.decl_bundle_name.empty()) {
                const std::string bundle_prefix = ss.decl_bundle_name + "::";
                replace_all_(sig, bundle_prefix, "");
            }
            if (const std::string short_head =
                    parus::short_core_module_head(ss.decl_bundle_name, ss.decl_module_head);
                !short_head.empty()) {
                const std::string short_module_prefix = short_head + "::";
                replace_all_(sig, short_module_prefix, "");
            }
            return sig;
        }

        std::string maybe_specialize_external_generic_link_name_(
            const parus::sema::Symbol& ss,
            TypeId fn_type,
            const parus::ty::TypePool& types
        ) {
            if (ss.kind != parus::sema::SymbolKind::kFn) {
                return !ss.link_name.empty() ? ss.link_name : ss.name;
            }
            if (fn_type == parus::ty::kInvalidType || fn_type >= types.count() ||
                !types.is_fn(fn_type) || types.fn_is_c_abi(fn_type)) {
                return !ss.link_name.empty() ? ss.link_name : ss.name;
            }
            if (ss.external_payload.find("parus_generic_decl") == std::string::npos) {
                return !ss.link_name.empty() ? ss.link_name : ss.name;
            }

            // Generic external decls are exported with template-shaped link names.
            // When we materialize a concrete instance, remangle from the concrete fn type
            // so installed/local builds agree on a unique symbol per monomorphization.
            const std::string bundle =
                ss.decl_bundle_name.empty() ? std::string("main") : ss.decl_bundle_name;
            const std::string leaf = last_path_segment_(!ss.name.empty() ? ss.name : ss.link_name);
            return build_parus_link_name_from_qname_and_sig_(
                bundle,
                leaf,
                relativize_external_fn_sig_repr_(ss, fn_type, types)
            );
        }

        ParsedExternalRecordLayout parse_external_record_layout_payload_(std::string_view payload) {
            ParsedExternalRecordLayout out{};
            const bool is_struct = payload.starts_with("parus_c_import_struct|");
            const bool is_union = payload.starts_with("parus_c_import_union|");
            if (!is_struct && !is_union) return out;

            std::string_view size_text{};
            std::string_view align_text{};
            std::string_view fields_text{};
            size_t pos = 0;
            while (pos < payload.size()) {
                size_t next = payload.find('|', pos);
                if (next == std::string_view::npos) next = payload.size();
                const std::string_view part = payload.substr(pos, next - pos);
                const size_t eq = part.find('=');
                if (eq != std::string_view::npos && eq + 1 < part.size()) {
                    const std::string_view key = part.substr(0, eq);
                    const std::string_view val = part.substr(eq + 1);
                    if (key == "size") size_text = val;
                    else if (key == "align") align_text = val;
                    else if (key == "fields") fields_text = val;
                }
                if (next == payload.size()) break;
                pos = next + 1;
            }

            if (!parse_u32_sv_(size_text, out.size) || !parse_u32_sv_(align_text, out.align)) {
                return {};
            }
            out.layout = is_struct ? FieldLayout::C : FieldLayout::None;

            size_t begin = 0;
            while (begin < fields_text.size()) {
                const size_t comma = fields_text.find(',', begin);
                const size_t end = (comma == std::string_view::npos) ? fields_text.size() : comma;
                const std::string_view one = fields_text.substr(begin, end - begin);
                if (!one.empty()) {
                    const size_t colon = one.find(':');
                    if (colon == std::string_view::npos || colon == 0 || colon + 1 >= one.size()) {
                        return {};
                    }

                    FieldMemberLayout member{};
                    member.name.assign(one.substr(0, colon));
                    std::string_view type_text = one.substr(colon + 1);
                    std::string_view suffix{};
                    if (const size_t at = type_text.find('@'); at != std::string_view::npos) {
                        suffix = type_text.substr(at + 1);
                        type_text = type_text.substr(0, at);
                    }

                    std::vector<std::string_view> parts{};
                    if (!suffix.empty()) {
                        size_t sb = 0;
                        while (true) {
                            const size_t sep = suffix.find('@', sb);
                            if (sep == std::string_view::npos) {
                                parts.push_back(suffix.substr(sb));
                                break;
                            }
                            parts.push_back(suffix.substr(sb, sep - sb));
                            sb = sep + 1;
                        }
                    }

                    (void)type_text;
                    member.type = parus::ty::kInvalidType;

                    if (is_struct) {
                        uint32_t offset_bytes = 0;
                        uint32_t bit_width = 0;
                        uint32_t storage_offset_bytes = 0;
                        if (!parts.empty() && !parse_u32_sv_(parts[0], offset_bytes)) return {};
                        if (parts.size() >= 4 && !parse_u32_sv_(parts[3], bit_width)) return {};
                        if (parts.size() >= 6 && !parse_u32_sv_(parts[5], storage_offset_bytes)) return {};
                        member.offset = (bit_width > 0) ? storage_offset_bytes : offset_bytes;
                    } else {
                        member.offset = 0;
                    }

                    out.members.push_back(std::move(member));
                }

                if (comma == std::string_view::npos) break;
                begin = comma + 1;
            }

            out.ok = true;
            return out;
        }

        ParsedExternalParusFieldDecl parse_external_parus_field_decl_payload_(
            std::string_view payload,
            const parus::ty::TypePool& types_in
        ) {
            ParsedExternalParusFieldDecl out{};
            if (!payload.starts_with("parus_field_decl|")) return out;
            auto& types = const_cast<parus::ty::TypePool&>(types_in);

            size_t pos = 0;
            while (pos < payload.size()) {
                size_t next = payload.find('|', pos);
                if (next == std::string_view::npos) next = payload.size();
                const std::string_view part = payload.substr(pos, next - pos);
                const size_t eq = part.find('=');
                if (eq != std::string_view::npos && eq + 1 < part.size()) {
                    const std::string_view key = part.substr(0, eq);
                    const std::string_view val = part.substr(eq + 1);
                    if (key == "layout") {
                        out.layout = (val == "c") ? FieldLayout::C : FieldLayout::None;
                    } else if (key == "align") {
                        if (!parse_u32_sv_(val, out.explicit_align)) return {};
                    } else if (key == "gparam") {
                        const std::string param = payload_unescape_value_(val);
                        if (std::find(out.generic_params.begin(), out.generic_params.end(), param) ==
                            out.generic_params.end()) {
                            out.generic_params.push_back(param);
                        }
                    } else if (key == "field") {
                        const size_t colon = val.find(':');
                        if (colon == std::string_view::npos || colon == 0 || colon + 1 >= val.size()) {
                            return {};
                        }

                        ParsedExternalParusFieldMember member{};
                        member.name.assign(val.substr(0, colon));

                        const std::string encoded_type = payload_unescape_value_(val.substr(colon + 1));
                        std::string_view type_text = encoded_type;
                        std::string_view type_semantic{};
                        if (const size_t at = encoded_type.find('@'); at != std::string::npos) {
                            type_semantic = std::string_view(encoded_type).substr(at + 1);
                            type_text = std::string_view(encoded_type).substr(0, at);
                        }

                        member.type = parus::cimport::parse_external_type_repr(
                            type_text,
                            type_semantic,
                            {},
                            types
                        );
                        if (member.type == parus::ty::kInvalidType) return {};
                        out.members.push_back(std::move(member));
                    }
                }
                if (next == payload.size()) break;
                pos = next + 1;
            }

            out.ok = true;
            return out;
        }

        parus::ty::TypeId substitute_external_generic_params_(
            const parus::ty::TypePool& types_in,
            parus::ty::TypeId src,
            const std::unordered_map<std::string, parus::ty::TypeId>& subst
        ) {
            auto& types = const_cast<parus::ty::TypePool&>(types_in);
            if (src == parus::ty::kInvalidType) return src;
            const auto& tt = types.get(src);
            switch (tt.kind) {
                case parus::ty::Kind::kNamedUser: {
                    std::vector<std::string_view> path{};
                    std::vector<parus::ty::TypeId> args{};
                    if (!types.decompose_named_user(src, path, args) || path.empty()) {
                        return src;
                    }

                    if (args.empty() && path.size() == 1) {
                        const auto it = subst.find(std::string(path.front()));
                        if (it != subst.end()) return it->second;
                    }

                    bool changed = false;
                    for (auto& arg : args) {
                        const auto next = substitute_external_generic_params_(types, arg, subst);
                        if (next != arg) {
                            arg = next;
                            changed = true;
                        }
                    }
                    if (!changed) return src;
                    return types.intern_named_path_with_args(
                        path.data(),
                        static_cast<uint32_t>(path.size()),
                        args.empty() ? nullptr : args.data(),
                        static_cast<uint32_t>(args.size())
                    );
                }
                case parus::ty::Kind::kOptional: {
                    const auto elem = substitute_external_generic_params_(types, tt.elem, subst);
                    return elem == tt.elem ? src : types.make_optional(elem);
                }
                case parus::ty::Kind::kArray: {
                    const auto elem = substitute_external_generic_params_(types, tt.elem, subst);
                    return elem == tt.elem ? src : types.make_array(elem, tt.array_has_size, tt.array_size);
                }
                case parus::ty::Kind::kBorrow: {
                    const auto elem = substitute_external_generic_params_(types, tt.elem, subst);
                    return elem == tt.elem ? src : types.make_borrow(elem, tt.borrow_is_mut);
                }
                case parus::ty::Kind::kEscape: {
                    const auto elem = substitute_external_generic_params_(types, tt.elem, subst);
                    return elem == tt.elem ? src : types.make_escape(elem);
                }
                case parus::ty::Kind::kPtr: {
                    const auto elem = substitute_external_generic_params_(types, tt.elem, subst);
                    return elem == tt.elem ? src : types.make_ptr(elem, tt.ptr_is_mut);
                }
                case parus::ty::Kind::kFn: {
                    std::vector<parus::ty::TypeId> params{};
                    std::vector<std::string_view> labels{};
                    std::vector<uint8_t> defaults{};
                    params.reserve(tt.param_count);
                    labels.reserve(tt.param_count);
                    defaults.reserve(tt.param_count);
                    bool changed = false;
                    for (uint32_t i = 0; i < tt.param_count; ++i) {
                        const auto p = types.fn_param_at(src, i);
                        const auto np = substitute_external_generic_params_(types, p, subst);
                        if (np != p) changed = true;
                        params.push_back(np);
                        labels.push_back(types.fn_param_label_at(src, i));
                        defaults.push_back(types.fn_param_has_default_at(src, i) ? 1u : 0u);
                    }
                    const auto nr = substitute_external_generic_params_(types, tt.ret, subst);
                    if (nr != tt.ret) changed = true;
                    if (!changed) return src;
                    return types.make_fn(
                        nr,
                        params.empty() ? nullptr : params.data(),
                        static_cast<uint32_t>(params.size()),
                        tt.positional_param_count,
                        labels.empty() ? nullptr : labels.data(),
                        defaults.empty() ? nullptr : defaults.data(),
                        tt.fn_is_c_abi,
                        tt.fn_is_c_variadic,
                        tt.fn_callconv
                    );
                }
                default:
                    return src;
            }
        }

        ParsedCImportWrapperPayload parse_cimport_wrapper_payload_(std::string_view payload) {
            ParsedCImportWrapperPayload out{};
            if (!payload.starts_with("parus_c_import|")) return out;

            size_t pos = 0;
            while (pos < payload.size()) {
                size_t next = payload.find('|', pos);
                if (next == std::string_view::npos) next = payload.size();
                const std::string_view part = payload.substr(pos, next - pos);
                const size_t eq = part.find('=');
                if (eq != std::string_view::npos && eq + 1 < part.size()) {
                    const std::string_view key = part.substr(0, eq);
                    const std::string_view val = part.substr(eq + 1);
                    if (key == "wrapper") {
                        out.is_wrapper = (val == "1" || val == "true");
                    } else if (key == "wrapper_callee") {
                        out.callee_link_name.assign(val);
                    } else if (key == "wrapper_argmap") {
                        size_t begin = 0;
                        while (begin <= val.size()) {
                            const size_t comma = val.find(',', begin);
                            const size_t end = (comma == std::string_view::npos) ? val.size() : comma;
                            if (end > begin) {
                                try {
                                    out.arg_map.push_back(static_cast<uint32_t>(
                                        std::stoul(std::string(val.substr(begin, end - begin)))));
                                } catch (...) {
                                    out.arg_map.clear();
                                    return out;
                                }
                            }
                            if (comma == std::string_view::npos) break;
                            begin = comma + 1;
                        }
                    }
                }
                if (next == payload.size()) break;
                pos = next + 1;
            }
            return out;
        }

        std::string_view trailing_name_segment_(std::string_view name) {
            const size_t pos = name.rfind("::");
            return (pos == std::string_view::npos) ? name : name.substr(pos + 2);
        }

        std::optional<std::string_view> trailing_instantiated_type_arg_(
            std::string_view name,
            std::string_view leaf
        ) {
            const std::string_view tail = trailing_name_segment_(name);
            if (!tail.ends_with(">")) return std::nullopt;
            const std::string prefix = std::string(leaf) + "<";
            if (!tail.starts_with(prefix)) return std::nullopt;
            return tail.substr(prefix.size(), tail.size() - prefix.size() - 1);
        }

        struct ActorRuntimeFuncs {
            FuncId new_fn = kInvalidId;
            FuncId clone_fn = kInvalidId;
            FuncId release_fn = kInvalidId;
            FuncId enter_fn = kInvalidId;
            FuncId draft_ptr_fn = kInvalidId;
            FuncId commit_fn = kInvalidId;
            FuncId recast_fn = kInvalidId;
            FuncId leave_fn = kInvalidId;
        };

        // OIR building state per function
        struct FuncBuild {
            Module* out = nullptr;
            const parus::sir::Module* sir = nullptr;
            const parus::ty::TypePool* types = nullptr;
            const parus::sema::SymbolTable* symtab = nullptr;
            const std::unordered_map<parus::ty::TypeId, std::pair<uint32_t, uint32_t>>* named_layout_by_type = nullptr;
            const std::unordered_set<TypeId>* actor_types = nullptr;
            const ActorRuntimeFuncs* actor_runtime = nullptr;
            std::unordered_map<parus::sir::ValueId, ValueId>* escape_value_map = nullptr;
            std::unordered_map<parus::sir::SymbolId, FuncId>* fn_symbol_to_func = nullptr;
            std::unordered_map<parus::sir::SymbolId, std::vector<FuncId>>* fn_symbol_to_funcs = nullptr;
            std::unordered_map<std::string, FuncId>* fn_link_name_to_func = nullptr;
            std::unordered_map<std::string, FuncId>* fn_source_name_to_func = nullptr;
            const std::unordered_map<uint32_t, FuncId>* fn_decl_to_func = nullptr;
            const std::unordered_map<parus::sir::SymbolId, uint32_t>* global_symbol_to_global = nullptr;
            const std::unordered_map<TypeId, FuncId>* class_deinit_map = nullptr;
            const std::unordered_map<TypeId, uint32_t>* exc_payload_globals = nullptr;
            const std::unordered_set<FuncId>* throwing_funcs = nullptr;
            std::vector<parus::sir::VerifyError>* build_errors = nullptr;
            uint32_t exc_active_global = kInvalidId;
            uint32_t exc_type_global = kInvalidId;
            bool fn_is_throwing = false;

            Function* def = nullptr;
            FuncId def_id = kInvalidId;
            BlockId cur_bb = kInvalidId;

            // symbol -> SSA value or slot
            struct Binding {
                bool is_slot = false;
                bool is_direct_address = false; // true for symbol that is already an address (e.g. globals)
                ValueId v = kInvalidId; // if is_slot: slot value id, else: SSA value id
                uint32_t cleanup_id = kInvalidId; // cleanup item index for RAII-managed class values
            };

            std::unordered_map<parus::sir::SymbolId, Binding> env;
            std::unordered_map<parus::sir::SymbolId, ValueId> home_slots;

            struct LoopContext {
                BlockId break_bb = kInvalidId;
                BlockId continue_bb = kInvalidId;
                bool expects_break_value = false;
                TypeId break_ty = kInvalidId;
                ValueId continue_value = kInvalidId;
                size_t scope_depth_base = 0; // number of active scopes to keep on break/continue
            };

            std::vector<LoopContext> loop_stack;

            struct CleanupItem {
                parus::sir::SymbolId sym = parus::sir::k_invalid_symbol;
                ValueId slot = kInvalidId;
                TypeId owner_ty = kInvalidId;
                bool moved = false;
            };

            struct ScopeFrame {
                std::vector<std::pair<parus::sir::SymbolId, Binding>> undo{};
                std::vector<uint32_t> cleanup_items{};
                bool cleaned = false;
            };

            struct TryContext {
                BlockId dispatch_bb = kInvalidId;
                BlockId after_bb = kInvalidId;
                size_t scope_depth_base = 0;
                uint32_t clause_begin = 0;
                uint32_t clause_count = 0;
            };

            std::vector<ScopeFrame> env_stack;
            std::vector<CleanupItem> cleanup_items;
            std::vector<TryContext> try_stack;
            std::unordered_set<parus::sir::SymbolId> untyped_catch_binder_symbols;
            ValueId current_actor_ctx = kInvalidId;

            const FieldLayoutDecl* find_field_layout_(TypeId t) const {
                if (out == nullptr) return nullptr;
                for (const auto& f : out->fields) {
                    if (f.self_type == t) return &f;
                }
                return nullptr;
            }

            ValueId add_block_param_local_(BlockId bb, TypeId ty) {
                if (out == nullptr || bb == kInvalidId || static_cast<size_t>(bb) >= out->blocks.size()) {
                    return kInvalidId;
                }
                auto& block = out->blocks[bb];
                Value v{};
                v.ty = ty;
                v.eff = Effect::Pure;
                v.def_a = bb;
                v.def_b = static_cast<uint32_t>(block.params.size());
                const ValueId vid = out->add_value(v);
                block.params.push_back(vid);
                return vid;
            }

            bool func_matches_fn_type_(FuncId fid, TypeId fn_type) const {
                if (out == nullptr || types == nullptr || fid == kInvalidId || fn_type == kInvalidId) return false;
                if (static_cast<size_t>(fid) >= out->funcs.size() || fn_type >= types->count()) return false;
                const auto& tt = types->get(fn_type);
                if (tt.kind != parus::ty::Kind::kFn) return false;
                const auto& f = out->funcs[fid];
                if (f.ret_ty != static_cast<TypeId>(tt.ret)) return false;
                if (f.entry == kInvalidId || static_cast<size_t>(f.entry) >= out->blocks.size()) return false;
                const auto& entry = out->blocks[f.entry];
                if (entry.params.size() != tt.param_count) return false;
                for (uint32_t i = 0; i < tt.param_count; ++i) {
                    const ValueId p = entry.params[i];
                    if (static_cast<size_t>(p) >= out->values.size()) return false;
                    if (out->values[p].ty != static_cast<TypeId>(types->fn_param_at(fn_type, i))) return false;
                }
                return true;
            }

            FuncId ensure_external_func_for_symbol_(parus::sir::SymbolId sid, TypeId fn_type) {
                if (out == nullptr || types == nullptr || symtab == nullptr) return kInvalidId;
                if (sid == parus::sir::k_invalid_symbol ||
                    static_cast<size_t>(sid) >= symtab->symbols().size() ||
                    fn_type == kInvalidId ||
                    fn_type >= types->count()) {
                    return kInvalidId;
                }
                const auto& ss = symtab->symbol(sid);
                if (ss.kind != parus::sema::SymbolKind::kFn) return kInvalidId;
                const auto& fn_tt = types->get(fn_type);
                if (fn_tt.kind != parus::ty::Kind::kFn) return kInvalidId;

                if (fn_symbol_to_funcs != nullptr) {
                    auto fit = fn_symbol_to_funcs->find(sid);
                    if (fit != fn_symbol_to_funcs->end()) {
                        for (const auto fid : fit->second) {
                            if (func_matches_fn_type_(fid, fn_type)) return fid;
                        }
                    }
                }

                const std::string link_name =
                    maybe_specialize_external_generic_link_name_(ss, fn_type, *types);
                if (link_name.empty()) return kInvalidId;

                if (fn_link_name_to_func != nullptr) {
                    auto fit = fn_link_name_to_func->find(link_name);
                    if (fit != fn_link_name_to_func->end() &&
                        func_matches_fn_type_(fit->second, fn_type)) {
                        if (fn_symbol_to_func != nullptr) {
                            (*fn_symbol_to_func)[sid] = fit->second;
                        }
                        if (fn_symbol_to_funcs != nullptr) {
                            auto& fids = (*fn_symbol_to_funcs)[sid];
                            if (std::find(fids.begin(), fids.end(), fit->second) == fids.end()) {
                                fids.push_back(fit->second);
                            }
                        }
                        return fit->second;
                    }
                }

                Function f{};
                f.name = link_name;
                f.source_name = ss.name.empty() ? link_name : ss.name;
                f.abi = types->fn_is_c_abi(fn_type) ? FunctionAbi::C : FunctionAbi::Parus;
                switch (types->fn_callconv(fn_type)) {
                    case parus::ty::CCallConv::kCdecl: f.c_callconv = CCallConv::Cdecl; break;
                    case parus::ty::CCallConv::kStdCall: f.c_callconv = CCallConv::StdCall; break;
                    case parus::ty::CCallConv::kFastCall: f.c_callconv = CCallConv::FastCall; break;
                    case parus::ty::CCallConv::kVectorCall: f.c_callconv = CCallConv::VectorCall; break;
                    case parus::ty::CCallConv::kWin64: f.c_callconv = CCallConv::Win64; break;
                    case parus::ty::CCallConv::kSysV: f.c_callconv = CCallConv::SysV; break;
                    case parus::ty::CCallConv::kDefault:
                    default:
                        f.c_callconv = CCallConv::Default;
                        break;
                }
                f.is_extern = true;
                f.is_c_variadic = types->fn_is_c_variadic(fn_type);
                f.c_fixed_param_count = fn_tt.param_count;
                f.ret_ty = static_cast<TypeId>(fn_tt.ret);

                const BlockId entry = out->add_block(Block{});
                f.entry = entry;
                f.blocks.push_back(entry);

                const FuncId fid = out->add_func(f);
                if (def_id != kInvalidId && static_cast<size_t>(def_id) < out->funcs.size()) {
                    def = &out->funcs[def_id];
                }
                if (fn_link_name_to_func != nullptr && fn_link_name_to_func->find(f.name) == fn_link_name_to_func->end()) {
                    (*fn_link_name_to_func)[f.name] = fid;
                }
                if (fn_source_name_to_func != nullptr &&
                    !f.source_name.empty() &&
                    fn_source_name_to_func->find(f.source_name) == fn_source_name_to_func->end()) {
                    (*fn_source_name_to_func)[f.source_name] = fid;
                }
                if (fn_symbol_to_func != nullptr && fn_symbol_to_func->find(sid) == fn_symbol_to_func->end()) {
                    (*fn_symbol_to_func)[sid] = fid;
                }
                if (fn_symbol_to_funcs != nullptr) {
                    (*fn_symbol_to_funcs)[sid].push_back(fid);
                }
                for (uint32_t pi = 0; pi < fn_tt.param_count; ++pi) {
                    (void)add_block_param_local_(entry, static_cast<TypeId>(types->fn_param_at(fn_type, pi)));
                }
                return fid;
            }

            std::pair<uint32_t, uint32_t> type_size_align_(TypeId tid) const {
                using TK = parus::ty::Kind;
                using TB = parus::ty::Builtin;
                auto align_to_local = [](uint32_t value, uint32_t align) -> uint32_t {
                    if (align == 0 || align == 1) return value;
                    const uint32_t rem = value % align;
                    return (rem == 0) ? value : (value + (align - rem));
                };

                if (types == nullptr || tid == parus::ty::kInvalidType) return {8u, 8u};
                const auto& t = types->get(tid);

                switch (t.kind) {
                    case TK::kError:
                        return {8u, 8u};

                    case TK::kBuiltin:
                        switch (t.builtin) {
                            case TB::kBool:
                            case TB::kI8:
                            case TB::kU8:
                            case TB::kCChar:
                            case TB::kCSChar:
                            case TB::kCUChar:
                                return {1u, 1u};
                            case TB::kI16:
                            case TB::kU16:
                            case TB::kCShort:
                            case TB::kCUShort:
                                return {2u, 2u};
                            case TB::kI32:
                            case TB::kU32:
                            case TB::kF32:
                            case TB::kChar:
                            case TB::kCInt:
                            case TB::kCUInt:
                            case TB::kCFloat:
                                return {4u, 4u};
                            case TB::kText:
                                return {16u, 8u};
                            case TB::kI128:
                            case TB::kU128:
                            case TB::kF128:
                                return {16u, 16u};
                            case TB::kUnit:
                            case TB::kCVoid:
                                return {1u, 1u};
                            case TB::kCLong:
                            case TB::kCULong:
                                return {static_cast<uint32_t>(sizeof(long)), static_cast<uint32_t>(alignof(long))};
                            case TB::kCLongLong:
                            case TB::kCULongLong:
                            case TB::kCDouble:
                                return {8u, 8u};
                            case TB::kCSize:
                            case TB::kCSSize:
                            case TB::kCPtrDiff:
                            case TB::kVaList:
                                return {8u, 8u};
                            case TB::kNever:
                            case TB::kI64:
                            case TB::kU64:
                            case TB::kF64:
                            case TB::kISize:
                            case TB::kUSize:
                            case TB::kNull:
                            case TB::kInferInteger:
                                return {8u, 8u};
                        }
                        return {8u, 8u};

                    case TK::kPtr:
                    case TK::kBorrow:
                    case TK::kEscape:
                    case TK::kFn:
                        return {8u, 8u};

                    case TK::kOptional: {
                        const auto [elem_size, elem_align] = type_size_align_(t.elem);
                        const uint32_t a = std::max<uint32_t>(1u, elem_align);
                        const uint32_t body =
                            align_to_local(1u, a) + std::max<uint32_t>(1u, elem_size);
                        return {body, a};
                    }

                    case TK::kArray: {
                        const auto [elem_size, elem_align] = type_size_align_(t.elem);
                        const uint32_t e = std::max<uint32_t>(1u, elem_size);
                        const uint32_t a = std::max<uint32_t>(1u, elem_align);
                        if (!t.array_has_size) return {16u, 8u};
                        return {e * std::max<uint32_t>(1u, t.array_size), a};
                    }

                    case TK::kNamedUser: {
                        if (actor_types != nullptr && actor_types->find(tid) != actor_types->end()) {
                            return {8u, 8u};
                        }
                        if (named_layout_by_type != nullptr) {
                            auto it = named_layout_by_type->find(tid);
                            if (it != named_layout_by_type->end()) return it->second;
                        }
                        return {8u, 8u};
                    }
                }

                return {8u, 8u};
            }

            bool lookup_user_deinit_for_class_(TypeId t, FuncId& out_fid) const {
                if (class_deinit_map == nullptr || t == kInvalidId) return false;
                auto it = class_deinit_map->find(t);
                if (it == class_deinit_map->end() || it->second == kInvalidId) return false;
                out_fid = it->second;
                return true;
            }

            bool type_needs_drop_rec_(TypeId t, std::unordered_set<TypeId>& visiting) const {
                if (types == nullptr || t == kInvalidId) return false;
                if (!visiting.insert(t).second) return false;

                const auto& tt = types->get(t);
                switch (tt.kind) {
                    case parus::ty::Kind::kOptional:
                        return (tt.elem != kInvalidId) && type_needs_drop_rec_(tt.elem, visiting);

                    case parus::ty::Kind::kArray:
                        if (!tt.array_has_size) return false; // unsized T[] is a non-owning view
                        return (tt.elem != kInvalidId) && type_needs_drop_rec_(tt.elem, visiting);

                    case parus::ty::Kind::kNamedUser: {
                        if (actor_types != nullptr && actor_types->find(t) != actor_types->end()) {
                            return true;
                        }
                        FuncId deinit_fid = kInvalidId;
                        const bool has_user_deinit = lookup_user_deinit_for_class_(t, deinit_fid);
                        const FieldLayoutDecl* layout = find_field_layout_(t);
                        if (has_user_deinit) return true;
                        if (layout == nullptr) return false;
                        for (const auto& m : layout->members) {
                            if (type_needs_drop_rec_(m.type, visiting)) return true;
                        }
                        return false;
                    }

                    default:
                        return false;
                }
            }

            bool type_needs_drop_(TypeId t) const {
                std::unordered_set<TypeId> visiting{};
                return type_needs_drop_rec_(t, visiting);
            }

            void emit_drop(TypeId owner_ty, ValueId slot) {
                if (slot == kInvalidId || owner_ty == kInvalidId) return;
                Inst inst{};
                inst.data = InstDrop{slot, owner_ty};
                inst.eff = Effect::MayWriteMem;
                inst.result = kInvalidId;
                emit_inst(inst);
            }

            void emit_cleanup_item(uint32_t cleanup_id) {
                if (cleanup_id == kInvalidId || cleanup_id >= cleanup_items.size()) return;
                auto& item = cleanup_items[cleanup_id];
                if (item.moved || item.slot == kInvalidId || item.owner_ty == kInvalidId) return;
                emit_drop(item.owner_ty, item.slot);
                item.moved = true;
            }

            void emit_cleanups_to_depth(size_t keep_depth) {
                if (keep_depth > env_stack.size()) keep_depth = env_stack.size();
                for (size_t i = env_stack.size(); i > keep_depth; --i) {
                    auto& frame = env_stack[i - 1];
                    if (frame.cleaned) continue;
                    for (auto it = frame.cleanup_items.rbegin(); it != frame.cleanup_items.rend(); ++it) {
                        emit_cleanup_item(*it);
                    }
                    frame.cleaned = true;
                }
            }

            void push_scope() { env_stack.emplace_back(); }
            void pop_scope() {
                if (env_stack.empty()) return;
                auto& frame = env_stack.back();
                if (!frame.cleaned && !has_term()) {
                    for (auto it = frame.cleanup_items.rbegin(); it != frame.cleanup_items.rend(); ++it) {
                        emit_cleanup_item(*it);
                    }
                    frame.cleaned = true;
                }
                for (auto it = frame.undo.rbegin(); it != frame.undo.rend(); ++it) {
                    env[it->first] = it->second;
                }
                env_stack.pop_back();
            }

            uint32_t register_cleanup(parus::sir::SymbolId sym, ValueId slot, TypeId owner_ty) {
                CleanupItem ci{};
                ci.sym = sym;
                ci.slot = slot;
                ci.owner_ty = owner_ty;
                ci.moved = false;
                const uint32_t id = static_cast<uint32_t>(cleanup_items.size());
                cleanup_items.push_back(ci);
                if (!env_stack.empty()) {
                    env_stack.back().cleanup_items.push_back(id);
                }
                return id;
            }

            void mark_symbol_moved(parus::sir::SymbolId sym, bool moved = true) {
                auto it = env.find(sym);
                if (it == env.end()) return;
                if (it->second.cleanup_id == kInvalidId || it->second.cleanup_id >= cleanup_items.size()) return;
                cleanup_items[it->second.cleanup_id].moved = moved;
            }

            std::optional<parus::sir::SymbolId> movable_source_symbol_(parus::sir::ValueId vid) const {
                if (sir == nullptr || vid == parus::sir::k_invalid_value) return std::nullopt;
                if ((size_t)vid >= sir->values.size()) return std::nullopt;
                const auto& sv = sir->values[vid];
                if (sv.kind == parus::sir::ValueKind::kLocal &&
                    sv.sym != parus::sir::k_invalid_symbol) {
                    return sv.sym;
                }
                return std::nullopt;
            }

            void consume_owned_sir_value_(parus::sir::ValueId vid, TypeId expected_ty) {
                if (!type_needs_drop_(expected_ty)) return;
                auto sym = movable_source_symbol_(vid);
                if (!sym.has_value()) return;
                mark_symbol_moved(*sym, /*moved=*/true);
            }

            void drop_symbol_before_overwrite(parus::sir::SymbolId sym) {
                auto it = env.find(sym);
                if (it == env.end()) return;
                const uint32_t cid = it->second.cleanup_id;
                if (cid == kInvalidId || cid >= cleanup_items.size()) return;
                emit_cleanup_item(cid);
            }

            void bind(parus::sir::SymbolId sym, Binding b) {
                // record previous for undo
                if (!env_stack.empty()) {
                    auto it = env.find(sym);
                    if (it != env.end()) env_stack.back().undo.push_back({sym, it->second});
                    else env_stack.back().undo.push_back({sym, Binding{false, false, kInvalidId, kInvalidId}});
                }
                env[sym] = b;
            }

            void remember_home_slot(parus::sir::SymbolId sym, ValueId slot) {
                if (sym == parus::sir::k_invalid_symbol || slot == kInvalidId) return;
                home_slots[sym] = slot;
            }

            bool should_remember_home_slot(TypeId ty) const {
                if (types == nullptr || ty == kInvalidId) return true;
                const auto& tt = types->get(ty);
                switch (tt.kind) {
                    case parus::ty::Kind::kBorrow:
                    case parus::ty::Kind::kEscape:
                    case parus::ty::Kind::kPtr:
                    case parus::ty::Kind::kFn:
                        return false;
                    default:
                        return true;
                }
            }

            ValueId lookup_bound_slot(parus::sir::SymbolId sym) const {
                if (sym == parus::sir::k_invalid_symbol) return kInvalidId;
                if (auto it = home_slots.find(sym); it != home_slots.end()) {
                    return it->second;
                }
                auto it = env.find(sym);
                if (it == env.end() || !it->second.is_slot) return kInvalidId;
                return it->second.v;
            }

            std::optional<parus::sir::SymbolId> place_root_local_sym(parus::sir::ValueId vid) const {
                if (sir == nullptr || vid == parus::sir::k_invalid_value || (size_t)vid >= sir->values.size()) {
                    return std::nullopt;
                }
                const auto& v = sir->values[vid];
                switch (v.kind) {
                    case parus::sir::ValueKind::kLocal:
                        if (v.sym != parus::sir::k_invalid_symbol) return v.sym;
                        return std::nullopt;
                    case parus::sir::ValueKind::kIndex:
                    case parus::sir::ValueKind::kField:
                        return place_root_local_sym(v.a);
                    default:
                        return std::nullopt;
                }
            }

            // -----------------------
            // OIR creation helpers
            // -----------------------
            ValueId make_value(TypeId ty, Effect eff, uint32_t def_a=kInvalidId, uint32_t def_b=kInvalidId) {
                Value v{};
                v.ty = ty;
                v.eff = eff;
                v.def_a = def_a;
                v.def_b = def_b;
                return out->add_value(v);
            }

            BlockId new_block() {
                Block b{};
                return out->add_block(b);
            }

            ValueId add_block_param(BlockId bb, TypeId ty) {
                // create value as block param
                auto& block = out->blocks[bb];
                uint32_t idx = (uint32_t)block.params.size();
                ValueId vid = make_value(ty, Effect::Pure, /*def_a=*/bb, /*def_b=*/idx);
                block.params.push_back(vid);
                return vid;
            }

            InstId emit_inst(const Inst& inst) {
                InstId iid = out->add_inst(inst);
                // OIR 값 정의 위치(def_a/def_b)를 즉시 동기화한다.
                // - 일반 inst result: def_a = inst_id, def_b = kInvalidId
                // - no-result inst(store 등): 값 메타 갱신 없음
                if (inst.result != kInvalidId && (size_t)inst.result < out->values.size()) {
                    out->values[inst.result].def_a = iid;
                    out->values[inst.result].def_b = kInvalidId;
                }
                out->blocks[cur_bb].insts.push_back(iid);
                return iid;
            }

            ValueId emit_const_int(TypeId ty, std::string text) {
                ValueId r = make_value(ty, Effect::Pure);
                Inst inst{};
                inst.data = InstConstInt{std::move(text)};
                inst.eff = Effect::Pure;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_const_float(TypeId ty, std::string text) {
                ValueId r = make_value(ty, Effect::Pure);
                Inst inst{};
                inst.data = InstConstFloat{std::move(text)};
                inst.eff = Effect::Pure;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_const_char(TypeId ty, uint32_t value) {
                ValueId r = make_value(ty, Effect::Pure);
                Inst inst{};
                inst.data = InstConstChar{value};
                inst.eff = Effect::Pure;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_const_bool(TypeId ty, bool v) {
                ValueId r = make_value(ty, Effect::Pure);
                Inst inst{};
                inst.data = InstConstBool{v};
                inst.eff = Effect::Pure;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_const_text(TypeId ty, std::string bytes) {
                ValueId r = make_value(ty, Effect::Pure);
                Inst inst{};
                inst.data = InstConstText{std::move(bytes)};
                inst.eff = Effect::Pure;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_const_null(TypeId ty) {
                ValueId r = make_value(ty, Effect::Pure);
                Inst inst{};
                inst.data = InstConstNull{};
                inst.eff = Effect::Pure;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_binop(TypeId ty, Effect eff, BinOp op, ValueId lhs, ValueId rhs) {
                ValueId r = make_value(ty, eff);
                Inst inst{};
                inst.data = InstBinOp{op, lhs, rhs};
                inst.eff = eff;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_unary(TypeId ty, Effect eff, UnOp op, ValueId src) {
                ValueId r = make_value(ty, eff);
                Inst inst{};
                inst.data = InstUnary{op, src};
                inst.eff = eff;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_cast(TypeId ty, Effect eff, CastKind kind, TypeId to, ValueId src) {
                ValueId r = make_value(ty, eff);
                Inst inst{};
                inst.data = InstCast{kind, to, src};
                inst.eff = eff;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_func_ref(FuncId fid, const std::string& name) {
                const TypeId ptr_ty =
                    (types != nullptr)
                        ? (TypeId)types->builtin(parus::ty::Builtin::kNull)
                        : kInvalidId;
                ValueId r = make_value(ptr_ty, Effect::Pure);
                Inst inst{};
                inst.data = InstFuncRef{fid, name};
                inst.eff = Effect::Pure;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_global_ref(uint32_t gid, const std::string& name) {
                TypeId slot_ty = kInvalidId;
                if (out != nullptr && (size_t)gid < out->globals.size()) {
                    slot_ty = out->globals[gid].type;
                }
                if (slot_ty == kInvalidId && types != nullptr) {
                    slot_ty = (TypeId)types->builtin(parus::ty::Builtin::kNull);
                }
                ValueId r = make_value(slot_ty, Effect::Pure);
                Inst inst{};
                inst.data = InstGlobalRef{gid, name};
                inst.eff = Effect::Pure;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_call(TypeId ty,
                              ValueId callee,
                              std::vector<ValueId> args,
                              FuncId direct_callee = kInvalidId,
                              bool call_is_c_abi = false,
                              bool call_is_c_variadic = false,
                              CCallConv call_c_callconv = CCallConv::Default,
                              uint32_t call_c_fixed_param_count = 0) {
                ValueId r = make_value(ty, Effect::Call);
                Inst inst{};
                inst.data = InstCall{
                    callee,
                    std::move(args),
                    direct_callee,
                    call_is_c_abi,
                    call_is_c_variadic,
                    call_c_callconv,
                    call_c_fixed_param_count
                };
                inst.eff = Effect::Call;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_direct_call(TypeId ty, FuncId direct_callee, std::vector<ValueId> args) {
                if (direct_callee == kInvalidId ||
                    out == nullptr ||
                    (size_t)direct_callee >= out->funcs.size()) {
                    report_lowering_error("direct call lowering failed: invalid callee id");
                    return emit_const_null(ty);
                }
                const ValueId callee = emit_func_ref(direct_callee, out->funcs[direct_callee].name);
                return emit_call(ty, callee, std::move(args), direct_callee);
            }

            ValueId emit_index(TypeId ty, ValueId base, ValueId index) {
                ValueId r = make_value(ty, Effect::MayReadMem);
                Inst inst{};
                inst.data = InstIndex{base, index};
                inst.eff = Effect::MayReadMem;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_array_len(ValueId base) {
                ValueId r = make_value(i64_type_(), Effect::MayReadMem);
                Inst inst{};
                inst.data = InstArrayLen{base};
                inst.eff = Effect::MayReadMem;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_slice_view(TypeId ty, ValueId base, ValueId lo, ValueId hi, bool hi_inclusive) {
                ValueId r = make_value(ty, Effect::MayTrap);
                Inst inst{};
                inst.data = InstSliceView{
                    .base = base,
                    .lo = lo,
                    .hi = hi,
                    .hi_inclusive = hi_inclusive
                };
                inst.eff = Effect::MayTrap;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_field(TypeId ty, ValueId base, std::string field, ExternalCBitfieldAccess c_bitfield = {}) {
                ValueId r = make_value(ty, Effect::MayReadMem);
                Inst inst{};
                inst.data = InstField{base, std::move(field), c_bitfield};
                inst.eff = Effect::MayReadMem;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_alloca(TypeId slot_ty) {
                // slot value: use special convention: its ty is slot_ty as-is.
                // backend can treat it as addressable slot.
                ValueId r = make_value(slot_ty, Effect::MayWriteMem);
                Inst inst{};
                inst.data = InstAllocaLocal{slot_ty};
                inst.eff = Effect::MayWriteMem;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            ValueId emit_load(TypeId ty, ValueId slot) {
                ValueId r = make_value(ty, Effect::MayReadMem);
                Inst inst{};
                inst.data = InstLoad{slot};
                inst.eff = Effect::MayReadMem;
                inst.result = r;
                emit_inst(inst);
                return r;
            }

            void emit_store(ValueId slot, ValueId val) {
                Inst inst{};
                inst.data = InstStore{slot, val};
                inst.eff = Effect::MayWriteMem;
                inst.result = kInvalidId;
                emit_inst(inst);
            }

            void emit_actor_commit_marker() {
                Inst inst{};
                inst.data = InstActorCommit{current_actor_ctx};
                inst.eff = Effect::MayWriteMem;
                inst.result = kInvalidId;
                emit_inst(inst);
            }

            void emit_actor_recast_marker() {
                Inst inst{};
                inst.data = InstActorRecast{current_actor_ctx};
                inst.eff = Effect::MayReadMem;
                inst.result = kInvalidId;
                emit_inst(inst);
            }

            bool has_exception_globals_() const {
                return exc_active_global != kInvalidId &&
                       exc_type_global != kInvalidId;
            }

            ValueId emit_exc_active_ptr_() {
                if (!has_exception_globals_()) return kInvalidId;
                return emit_global_ref(exc_active_global, "__parus_exc_active");
            }

            ValueId emit_exc_type_ptr_() {
                if (!has_exception_globals_()) return kInvalidId;
                return emit_global_ref(exc_type_global, "__parus_exc_type");
            }

            bool has_exception_payload_global_for_(TypeId payload_ty) const {
                if (exc_payload_globals == nullptr || payload_ty == kInvalidId) return false;
                return exc_payload_globals->find(payload_ty) != exc_payload_globals->end();
            }

            bool value_is_address_like_(ValueId v) const {
                if (out == nullptr || v == kInvalidId) return false;
                if ((size_t)v >= out->values.size()) return false;
                const auto& val = out->values[v];
                // block params are SSA values, not address-producing instructions.
                if (val.def_b != kInvalidId) return false;
                if (val.def_a == kInvalidId || (size_t)val.def_a >= out->insts.size()) return false;
                const auto& inst = out->insts[val.def_a];
                return std::holds_alternative<InstAllocaLocal>(inst.data) ||
                       std::holds_alternative<InstField>(inst.data) ||
                       std::holds_alternative<InstIndex>(inst.data) ||
                       std::holds_alternative<InstGlobalRef>(inst.data);
            }

            ValueId emit_exc_payload_ptr_(TypeId payload_ty) {
                if (!has_exception_payload_global_for_(payload_ty)) return kInvalidId;
                const auto it = exc_payload_globals->find(payload_ty);
                if (it == exc_payload_globals->end()) return kInvalidId;
                if (out != nullptr && (size_t)it->second < out->globals.size()) {
                    return emit_global_ref(it->second, out->globals[it->second].name);
                }
                return emit_global_ref(it->second, "__parus_exc_payload");
            }

            TypeId bool_type_() const {
                return (types != nullptr)
                    ? (TypeId)types->builtin(parus::ty::Builtin::kBool)
                    : kInvalidId;
            }

            TypeId i64_type_() const {
                return (types != nullptr)
                    ? (TypeId)types->builtin(parus::ty::Builtin::kI64)
                    : kInvalidId;
            }

            TypeId u32_type_() const {
                return (types != nullptr)
                    ? (TypeId)types->builtin(parus::ty::Builtin::kU32)
                    : kInvalidId;
            }

            TypeId u64_type_() const {
                return (types != nullptr)
                    ? (TypeId)types->builtin(parus::ty::Builtin::kU64)
                    : kInvalidId;
            }

            TypeId ptr_type_() const {
                return (types != nullptr)
                    ? (TypeId)types->builtin(parus::ty::Builtin::kNull)
                    : kInvalidId;
            }

            bool is_actor_handle_type_(TypeId t) const {
                while (types != nullptr && t != kInvalidId) {
                    const auto& tt = types->get(t);
                    if (tt.kind == parus::ty::Kind::kBorrow) {
                        t = tt.elem;
                        continue;
                    }
                    return tt.kind == parus::ty::Kind::kNamedUser &&
                           actor_types != nullptr &&
                           actor_types->find(t) != actor_types->end();
                }
                return false;
            }

            void emit_set_exc_active_(bool active) {
                if (!has_exception_globals_()) return;
                const ValueId ptr = emit_exc_active_ptr_();
                if (ptr == kInvalidId) return;
                const ValueId vv = emit_const_bool(bool_type_(), active);
                emit_store(ptr, vv);
            }

            void emit_set_exc_type_(TypeId payload_ty) {
                if (!has_exception_globals_()) return;
                const ValueId ptr = emit_exc_type_ptr_();
                if (ptr == kInvalidId) return;
                const uint32_t raw = (payload_ty == kInvalidId) ? 0u : (uint32_t)payload_ty;
                const ValueId vv = emit_const_int(i64_type_(), std::to_string(raw));
                emit_store(ptr, vv);
            }

            void emit_set_exc_type_from_value_(ValueId dynamic_ty) {
                if (!has_exception_globals_()) return;
                const ValueId ptr = emit_exc_type_ptr_();
                if (ptr == kInvalidId || dynamic_ty == kInvalidId) return;
                ValueId store_v = dynamic_ty;
                if (out != nullptr && (size_t)dynamic_ty < out->values.size()) {
                    const TypeId vt = out->values[dynamic_ty].ty;
                    const TypeId i64t = i64_type_();
                    if (vt != kInvalidId && i64t != kInvalidId && vt != i64t) {
                        store_v = emit_cast(i64t, Effect::Pure, CastKind::As, i64t, dynamic_ty);
                    }
                }
                emit_store(ptr, store_v);
            }

            void emit_store_exc_payload_(TypeId payload_ty, ValueId payload) {
                if (payload_ty == kInvalidId || payload == kInvalidId) return;
                const ValueId ptr = emit_exc_payload_ptr_(payload_ty);
                if (ptr == kInvalidId) return;
                ValueId store_v = payload;
                // Struct/enum literals are materialized as temporary slots in OIR.
                // Exception payload globals expect value stores, not slot pointers.
                if (value_is_address_like_(store_v)) {
                    store_v = emit_load(payload_ty, store_v);
                }
                if (out != nullptr && (size_t)store_v < out->values.size()) {
                    const TypeId vt = out->values[store_v].ty;
                    if (vt != kInvalidId && vt != payload_ty) {
                        store_v = coerce_value_for_target(payload_ty, store_v);
                    }
                }
                emit_store(ptr, store_v);
            }

            ValueId emit_load_exc_payload_(TypeId payload_ty) {
                if (payload_ty == kInvalidId) return emit_const_null(kInvalidId);
                const ValueId ptr = emit_exc_payload_ptr_(payload_ty);
                if (ptr == kInvalidId) return emit_const_null(payload_ty);
                return emit_load(payload_ty, ptr);
            }

            ValueId emit_load_exc_active_() {
                if (!has_exception_globals_()) return emit_const_bool(bool_type_(), false);
                const ValueId ptr = emit_exc_active_ptr_();
                if (ptr == kInvalidId) return emit_const_bool(bool_type_(), false);
                return emit_load(bool_type_(), ptr);
            }

            ValueId emit_load_exc_type_() {
                if (!has_exception_globals_()) return emit_const_int(i64_type_(), "0");
                const ValueId ptr = emit_exc_type_ptr_();
                if (ptr == kInvalidId) return emit_const_int(i64_type_(), "0");
                return emit_load(i64_type_(), ptr);
            }

            ValueId emit_default_value_for_type_(TypeId t) {
                if (t == kInvalidId) return emit_const_null(kInvalidId);
                return emit_const_null(t);
            }

            void emit_return_default_for_current_fn_() {
                if (def == nullptr || def->ret_ty == kInvalidId) {
                    ret_void();
                    return;
                }
                ret(emit_default_value_for_type_(def->ret_ty));
            }

            void emit_propagate_throw_(size_t cleanup_keep_depth) {
                emit_cleanups_to_depth(cleanup_keep_depth);
                emit_return_default_for_current_fn_();
            }

            void emit_stmt_boundary_throw_check_() {
                if (!fn_is_throwing) return;
                if (!try_stack.empty()) return;
                if (!has_exception_globals_()) return;
                if (has_term()) return;

                const ValueId active = emit_load_exc_active_();
                const BlockId throw_bb = new_block();
                const BlockId cont_bb = new_block();
                condbr(active, throw_bb, {}, cont_bb, {});

                def->blocks.push_back(throw_bb);
                cur_bb = throw_bb;
                emit_propagate_throw_(/*cleanup_keep_depth=*/0);

                def->blocks.push_back(cont_bb);
                cur_bb = cont_bb;
            }

            void set_term(const Terminator& t) {
                auto& b = out->blocks[cur_bb];
                b.term = t;
                b.has_term = true;
            }

            bool has_term() const {
                return out->blocks[cur_bb].has_term;
            }

            void br(BlockId target, std::vector<ValueId> args = {}) {
                TermBr t{};
                t.target = target;
                t.args = std::move(args);
                set_term(t);
            }

            void condbr(ValueId cond,
                        BlockId then_bb, std::vector<ValueId> then_args,
                        BlockId else_bb, std::vector<ValueId> else_args) {
                TermCondBr t{};
                t.cond = cond;
                t.then_bb = then_bb;
                t.then_args = std::move(then_args);
                t.else_bb = else_bb;
                t.else_args = std::move(else_args);
                set_term(t);
            }

            void ret_void() {
                TermRet t{};
                t.has_value = false;
                t.value = kInvalidId;
                set_term(t);
            }

            void ret(ValueId v) {
                TermRet t{};
                t.has_value = true;
                t.value = v;
                set_term(t);
            }

            // -----------------------
            // SIR -> OIR lowering
            // -----------------------
            ValueId lower_value(parus::sir::ValueId vid);
            ValueId lower_place(parus::sir::ValueId vid, TypeId* out_place_ty = nullptr);
            void    lower_stmt(uint32_t stmt_index);
            void    lower_block(parus::sir::BlockId bid);
            void    lower_block_with_try_guard(parus::sir::BlockId bid, const TryContext& tc);
            ValueId lower_block_expr(parus::sir::ValueId block_expr_vid);
            ValueId lower_if_expr(parus::sir::ValueId if_vid);

            // util: resolve local reading as SSA or load(slot)
            ValueId read_local(parus::sir::SymbolId sym, TypeId want_ty) {
                auto it = env.find(sym);
                if (it == env.end()) {
                    // Fallback: unresolved local slot that is actually a function symbol.
                    // This is required for external/imported functions that are not materialized
                    // as local bindings in the current function environment.
                    if (fn_symbol_to_func != nullptr && sym != parus::sir::k_invalid_symbol) {
                        auto fit = fn_symbol_to_func->find(sym);
                        if (fit != fn_symbol_to_func->end() &&
                            out != nullptr &&
                            (size_t)fit->second < out->funcs.size()) {
                            return emit_func_ref(fit->second, out->funcs[fit->second].name);
                        }
                    }
                    if (symtab != nullptr &&
                        sym != parus::sir::k_invalid_symbol &&
                        static_cast<size_t>(sym) < symtab->symbols().size()) {
                        const auto& ss = symtab->symbol(sym);
                        if (ss.kind == parus::sema::SymbolKind::kFn) {
                            const std::string effective_link_name =
                                maybe_specialize_external_generic_link_name_(ss, ss.declared_type, *types);
                            if (fn_link_name_to_func != nullptr && !effective_link_name.empty()) {
                                auto fit = fn_link_name_to_func->find(effective_link_name);
                                if (fit != fn_link_name_to_func->end() &&
                                    out != nullptr &&
                                    (size_t)fit->second < out->funcs.size()) {
                                    return emit_func_ref(fit->second, out->funcs[fit->second].name);
                                }
                            }
                            if (fn_source_name_to_func != nullptr && !ss.name.empty()) {
                                auto fit = fn_source_name_to_func->find(ss.name);
                                if (fit != fn_source_name_to_func->end() &&
                                    out != nullptr &&
                                    (size_t)fit->second < out->funcs.size()) {
                                    return emit_func_ref(fit->second, out->funcs[fit->second].name);
                                }
                            }
                        }
                    }
                    // unknown -> produce dummy (kept for error recovery)
                    return emit_const_null(want_ty);
                }
                if (!it->second.is_slot) return it->second.v;
                if (it->second.is_direct_address) {
                    if (types != nullptr && want_ty != parus::ty::kInvalidType) {
                        const auto& wt = types->get(want_ty);
                        if (wt.kind == parus::ty::Kind::kNamedUser ||
                            wt.kind == parus::ty::Kind::kArray ||
                            wt.kind == parus::ty::Kind::kOptional ||
                            wt.kind == parus::ty::Kind::kPtr ||
                            wt.kind == parus::ty::Kind::kBorrow ||
                            wt.kind == parus::ty::Kind::kEscape) {
                            return it->second.v;
                        }
                    }
                    return emit_load(want_ty, it->second.v);
                }
                return emit_load(want_ty, it->second.v);
            }

            // util: ensure a symbol has a slot (for write), possibly demote SSA to slot
            ValueId ensure_slot(parus::sir::SymbolId sym, TypeId slot_ty) {
                auto it = env.find(sym);
                if (it != env.end() && it->second.is_slot) return it->second.v;

                uint32_t cleanup_id = kInvalidId;
                if (it != env.end()) cleanup_id = it->second.cleanup_id;

                // create a new slot
                ValueId slot = emit_alloca(slot_ty);

                // if previously SSA value existed, initialize slot with it
                if (it != env.end() && !it->second.is_slot && it->second.v != kInvalidId) {
                    emit_store(slot, it->second.v);
                }

                bind(sym, Binding{true, false, slot, cleanup_id});
                return slot;
            }

            // util: boundary coercion (hybrid nullable policy)
            ValueId coerce_value_for_target(TypeId dst_ty, ValueId src) {
                if (src == kInvalidId) return src;
                if (dst_ty == kInvalidId || types == nullptr || out == nullptr) return src;
                if ((size_t)src >= out->values.size()) return src;

                const TypeId src_ty = out->values[src].ty;
                if (src_ty == dst_ty) {
                    const auto& dt0 = types->get(dst_ty);
                    if ((dt0.kind != parus::ty::Kind::kBorrow &&
                         dt0.kind != parus::ty::Kind::kEscape) &&
                        value_is_address_like_(src)) {
                        return emit_load(dst_ty, src);
                    }
                    return src;
                }

                auto type_contains_unresolved_generic_param = [&](auto&& self, TypeId t) -> bool {
                    if (t == kInvalidId || types == nullptr) return false;
                    const auto& tt = types->get(t);
                    switch (tt.kind) {
                        case parus::ty::Kind::kNamedUser: {
                            std::vector<std::string_view> path{};
                            std::vector<TypeId> args{};
                            if (!types->decompose_named_user(t, path, args) || path.empty()) return false;
                            if (args.empty() && path.size() == 1) {
                                return !leaf_name_resolves_to_concrete_type_(
                                    *sir,
                                    *types,
                                    symtab,
                                    path.front()
                                );
                            }
                            for (const auto arg_t : args) {
                                if (self(self, arg_t)) return true;
                            }
                            return false;
                        }
                        case parus::ty::Kind::kBorrow:
                        case parus::ty::Kind::kEscape:
                        case parus::ty::Kind::kPtr:
                        case parus::ty::Kind::kOptional:
                        case parus::ty::Kind::kArray:
                            return tt.elem != kInvalidId && self(self, tt.elem);
                        case parus::ty::Kind::kFn:
                            for (uint32_t i = 0; i < tt.param_count; ++i) {
                                if (self(self, types->fn_param_at(t, i))) return true;
                            }
                            return tt.ret != kInvalidId && self(self, tt.ret);
                        default:
                            return false;
                    }
                };

                auto is_c_char_like_type = [&](TypeId t) -> bool {
                    if (t == kInvalidId) return false;
                    const auto& et = types->get(t);
                    if (et.kind == parus::ty::Kind::kNamedUser) {
                        std::vector<std::string_view> path{};
                        std::vector<TypeId> args{};
                        if (!types->decompose_named_user(t, path, args)) return false;
                        if (!args.empty() || path.empty()) return false;
                        const std::string_view leaf = path.back();
                        if (!(leaf == "c_char" || leaf == "c_schar" || leaf == "c_uchar")) {
                            return false;
                        }
                        if (path.size() == 1) return true;
                        return path[path.size() - 2] == "ext";
                    }
                    if (et.kind != parus::ty::Kind::kBuiltin) return false;
                    using B = parus::ty::Builtin;
                    switch (et.builtin) {
                        case B::kChar:
                        case B::kI8:
                        case B::kU8:
                        case B::kCChar:
                        case B::kCSChar:
                        case B::kCUChar:
                            return true;
                        default:
                            return false;
                    }
                };

                auto is_c_char_ptr_type = [&](TypeId t) -> bool {
                    if (t == kInvalidId) return false;
                    const auto& tt0 = types->get(t);
                    if (tt0.kind == parus::ty::Kind::kBorrow) t = tt0.elem;
                    if (t == kInvalidId) return false;
                    const auto& tt = types->get(t);
                    if (tt.kind != parus::ty::Kind::kPtr || tt.elem == kInvalidId) return false;
                    return is_c_char_like_type(tt.elem);
                };

                auto is_core_ext_cstr_type = [&](TypeId t) -> bool {
                    if (t == kInvalidId) return false;
                    const auto& tt0 = types->get(t);
                    if (tt0.kind == parus::ty::Kind::kBorrow) t = tt0.elem;
                    if (t == kInvalidId) return false;
                    const auto& tt = types->get(t);
                    if (tt.kind != parus::ty::Kind::kNamedUser) return false;
                    std::vector<std::string_view> path{};
                    std::vector<TypeId> args{};
                    if (!types->decompose_named_user(t, path, args)) return false;
                    if (!args.empty() || path.empty()) return false;
                    if (path.back() != "CStr") return false;
                    if (path.size() == 1) return true;
                    const std::string_view parent = path[path.size() - 2];
                    return parent == "ext" || parent == "core";
                };

                auto emit_core_ext_cstr_ptr_ = [&](ValueId src_value, TypeId want_ptr_ty) -> ValueId {
                    const TypeId src_value_ty =
                        ((size_t)src_value < out->values.size()) ? out->values[src_value].ty : kInvalidId;
                    if (const FieldLayoutDecl* layout = find_field_layout_(src_value_ty)) {
                        for (const auto& m : layout->members) {
                            if (m.name == "ptr_" && m.type != kInvalidId) {
                                return emit_field(m.type, src_value, "ptr_");
                            }
                        }
                    }
                    return emit_field(want_ptr_ty, src_value, "ptr_");
                };

                if (is_c_char_ptr_type(dst_ty) && is_core_ext_cstr_type(src_ty)) {
                    return emit_core_ext_cstr_ptr_(src, dst_ty);
                }

                const auto& dt = types->get(dst_ty);
                if ((dt.kind == parus::ty::Kind::kBorrow ||
                     dt.kind == parus::ty::Kind::kEscape) &&
                    dt.elem != kInvalidId) {
                    if (value_is_address_like_(src)) {
                        return src;
                    }
                    TypeId slot_ty = dt.elem;
                    if (src_ty != kInvalidId &&
                        type_contains_unresolved_generic_param(type_contains_unresolved_generic_param, slot_ty)) {
                        slot_ty = src_ty;
                    }
                    ValueId slot = emit_alloca(slot_ty);
                    ValueId store_v = src;
                    if (out != nullptr &&
                        (size_t)store_v < out->values.size() &&
                        out->values[store_v].ty != slot_ty) {
                        store_v = coerce_value_for_target(slot_ty, store_v);
                    }
                    emit_store(slot, store_v);
                    return slot;
                }

                if (dt.kind == parus::ty::Kind::kArray && !dt.array_has_size &&
                    src_ty != kInvalidId) {
                    const auto& st = types->get(src_ty);
                    if (st.kind == parus::ty::Kind::kArray &&
                        st.array_has_size &&
                        st.elem == dt.elem) {
                        const ValueId zero = emit_const_int(i64_type_(), "0");
                        const ValueId hi = emit_array_len(src);
                        return emit_slice_view(dst_ty, src, zero, hi, /*hi_inclusive=*/false);
                    }
                }

                if (dt.kind == parus::ty::Kind::kOptional) {
                    const TypeId elem_ty = dt.elem;
                    const TypeId null_ty = (TypeId)types->builtin(parus::ty::Builtin::kNull);

                    if (src_ty == null_ty) {
                        return emit_const_null(dst_ty);
                    }
                    if (elem_ty != kInvalidId && src_ty == elem_ty) {
                        // Optional some(T): represent as typed cast at OIR boundary.
                        return emit_cast(dst_ty, Effect::Pure, CastKind::As, dst_ty, src);
                    }
                }

                if (value_is_address_like_(src) && src_ty != kInvalidId) {
                    const ValueId loaded = emit_load(src_ty, src);
                    if (loaded == src) return loaded;
                    return coerce_value_for_target(dst_ty, loaded);
                }

                return src;
            }

            void report_lowering_error(std::string message) {
                if (build_errors == nullptr) return;
                build_errors->push_back(parus::sir::VerifyError{std::move(message)});
            }
        };

        static std::optional<BinOp> map_binop(parus::syntax::TokenKind k) {
            using TK = parus::syntax::TokenKind;
            switch (k) {
                case TK::kPlus:              return std::optional<BinOp>{BinOp::Add};
                case TK::kMinus:             return std::optional<BinOp>{BinOp::Sub};
                case TK::kStar:              return std::optional<BinOp>{BinOp::Mul};
                case TK::kSlash:             return std::optional<BinOp>{BinOp::Div};
                case TK::kPercent:           return std::optional<BinOp>{BinOp::Rem};
                case TK::kLt:                return std::optional<BinOp>{BinOp::Lt};
                case TK::kLtEq:              return std::optional<BinOp>{BinOp::Le};
                case TK::kGt:                return std::optional<BinOp>{BinOp::Gt};
                case TK::kGtEq:              return std::optional<BinOp>{BinOp::Ge};
                case TK::kEqEq:              return std::optional<BinOp>{BinOp::Eq};
                case TK::kBangEq:            return std::optional<BinOp>{BinOp::Ne};
                case TK::kKwAnd:             return std::optional<BinOp>{BinOp::LogicalAnd};
                case TK::kKwOr:              return std::optional<BinOp>{BinOp::LogicalOr};
                case TK::kQuestionQuestion:  return std::optional<BinOp>{BinOp::NullCoalesce};
                default:                     return std::nullopt;
            }
        }

        static std::optional<UnOp> map_unary(parus::syntax::TokenKind k) {
            using TK = parus::syntax::TokenKind;
            switch (k) {
                case TK::kPlus:  return std::optional<UnOp>{UnOp::Plus};
                case TK::kMinus: return std::optional<UnOp>{UnOp::Neg};
                case TK::kKwNot: return std::optional<UnOp>{UnOp::Not};
                case TK::kBang:  return std::optional<UnOp>{UnOp::BitNot};
                default:         return std::nullopt;
            }
        }

        std::optional<std::string> parse_float_literal_text_(std::string_view text) {
            std::string out;
            out.reserve(text.size());

            size_t i = 0;
            if (!text.empty() && (text[0] == '+' || text[0] == '-')) {
                out.push_back(text[0]);
                i = 1;
            }

            bool saw_digit = false;
            auto append_digits = [&](size_t& pos) {
                while (pos < text.size()) {
                    const char c = text[pos];
                    if (c >= '0' && c <= '9') {
                        out.push_back(c);
                        saw_digit = true;
                        ++pos;
                        continue;
                    }
                    if (c == '_') {
                        ++pos;
                        continue;
                    }
                    break;
                }
            };

            append_digits(i);

            bool has_dot = false;
            if (i < text.size() && text[i] == '.') {
                has_dot = true;
                out.push_back('.');
                ++i;
                append_digits(i);
            }

            bool has_exp = false;
            if (i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
                has_exp = true;
                out.push_back(text[i++]);
                if (i < text.size() && (text[i] == '+' || text[i] == '-')) {
                    out.push_back(text[i++]);
                }
                size_t exp_digits_begin = out.size();
                while (i < text.size()) {
                    const char c = text[i];
                    if (c >= '0' && c <= '9') {
                        out.push_back(c);
                        ++i;
                        continue;
                    }
                    if (c == '_') {
                        ++i;
                        continue;
                    }
                    break;
                }
                if (out.size() == exp_digits_begin) return std::nullopt;
            }

            if (!saw_digit || (!has_dot && !has_exp)) return std::nullopt;

            const std::string_view suffix = text.substr(i);
            if (!(suffix.empty() || suffix == "f" || suffix == "f32" || suffix == "lf" || suffix == "f64" || suffix == "f128")) {
                return std::nullopt;
            }
            return out;
        }

        std::string parse_int_literal_text_(std::string_view text) {
            std::string out;
            out.reserve(text.size());

            size_t i = 0;
            if (!text.empty() && (text[0] == '+' || text[0] == '-')) {
                out.push_back(text[0]);
                i = 1;
            }

            bool saw_digit = false;
            for (; i < text.size(); ++i) {
                const char c = text[i];
                if (c >= '0' && c <= '9') {
                    out.push_back(c);
                    saw_digit = true;
                    continue;
                }
                if (c == '_') continue;
                break;
            }
            if (!saw_digit) return "0";
            return out;
        }

        bool is_hex_digit_(char c) {
            return (c >= '0' && c <= '9') ||
                   (c >= 'a' && c <= 'f') ||
                   (c >= 'A' && c <= 'F');
        }

        uint8_t hex_digit_value_(char c) {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + (c - 'a'));
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(10 + (c - 'A'));
            return 0;
        }

        bool is_valid_unicode_scalar_(uint32_t cp) {
            return cp <= 0x10FFFFu && !(cp >= 0xD800u && cp <= 0xDFFFu);
        }

        std::optional<uint32_t> parse_unicode_escape_code_(std::string_view body) {
            if (body.size() < 5 || body[0] != '\\' || body[1] != 'u' || body[2] != '{' || body.back() != '}') {
                return std::nullopt;
            }
            const std::string_view hex = body.substr(3, body.size() - 4);
            if (hex.empty()) return std::nullopt;

            uint32_t cp = 0;
            for (const char ch : hex) {
                if (!is_hex_digit_(ch)) return std::nullopt;
                const uint32_t digit = static_cast<uint32_t>(hex_digit_value_(ch));
                if (cp > 0x10FFFFu / 16u) return std::nullopt;
                cp = cp * 16u + digit;
                if (cp > 0x10FFFFu) return std::nullopt;
            }
            if (!is_valid_unicode_scalar_(cp)) return std::nullopt;
            return cp;
        }

        std::optional<uint32_t> decode_single_utf8_codepoint_(std::string_view body) {
            if (body.empty()) return std::nullopt;
            const auto b0 = static_cast<unsigned char>(body[0]);
            if (b0 < 0x80u) {
                if (body.size() != 1) return std::nullopt;
                return static_cast<uint32_t>(b0);
            }

            auto cont = [&](size_t idx) -> std::optional<uint32_t> {
                if (idx >= body.size()) return std::nullopt;
                const auto bx = static_cast<unsigned char>(body[idx]);
                if ((bx & 0xC0u) != 0x80u) return std::nullopt;
                return static_cast<uint32_t>(bx & 0x3Fu);
            };

            if ((b0 & 0xE0u) == 0xC0u) {
                if (body.size() != 2) return std::nullopt;
                const auto c1 = cont(1);
                if (!c1.has_value()) return std::nullopt;
                const uint32_t cp = (static_cast<uint32_t>(b0 & 0x1Fu) << 6u) | *c1;
                if (cp < 0x80u || !is_valid_unicode_scalar_(cp)) return std::nullopt;
                return cp;
            }
            if ((b0 & 0xF0u) == 0xE0u) {
                if (body.size() != 3) return std::nullopt;
                const auto c1 = cont(1);
                const auto c2 = cont(2);
                if (!c1.has_value() || !c2.has_value()) return std::nullopt;
                const uint32_t cp =
                    (static_cast<uint32_t>(b0 & 0x0Fu) << 12u) |
                    (*c1 << 6u) |
                    *c2;
                if (cp < 0x800u || !is_valid_unicode_scalar_(cp)) return std::nullopt;
                return cp;
            }
            if ((b0 & 0xF8u) == 0xF0u) {
                if (body.size() != 4) return std::nullopt;
                const auto c1 = cont(1);
                const auto c2 = cont(2);
                const auto c3 = cont(3);
                if (!c1.has_value() || !c2.has_value() || !c3.has_value()) return std::nullopt;
                const uint32_t cp =
                    (static_cast<uint32_t>(b0 & 0x07u) << 18u) |
                    (*c1 << 12u) |
                    (*c2 << 6u) |
                    *c3;
                if (cp < 0x10000u || !is_valid_unicode_scalar_(cp)) return std::nullopt;
                return cp;
            }
            return std::nullopt;
        }

        std::optional<uint32_t> parse_char_literal_code_(std::string_view text) {
            if (text.size() < 3 || text.front() != '\'' || text.back() != '\'') return std::nullopt;
            std::string_view body = text.substr(1, text.size() - 2);
            if (body.empty()) return std::nullopt;
            if (body[0] != '\\') {
                return decode_single_utf8_codepoint_(body);
            }

            if (body.size() == 2) {
                switch (body[1]) {
                    case 'n': return static_cast<uint32_t>('\n');
                    case 'r': return static_cast<uint32_t>('\r');
                    case 't': return static_cast<uint32_t>('\t');
                    case '\\': return static_cast<uint32_t>('\\');
                    case '\'': return static_cast<uint32_t>('\'');
                    case '0': return static_cast<uint32_t>('\0');
                    default: return std::nullopt;
                }
            }
            if (auto cp = parse_unicode_escape_code_(body); cp.has_value()) {
                return cp;
            }
            return std::nullopt;
        }

        std::string decode_escaped_string_body_(std::string_view body) {
            std::string out;
            out.reserve(body.size());

            for (size_t i = 0; i < body.size(); ++i) {
                const char c = body[i];
                if (c != '\\') {
                    out.push_back(c);
                    continue;
                }

                if (i + 1 >= body.size()) {
                    out.push_back('\\');
                    break;
                }

                const char esc = body[++i];
                switch (esc) {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case '\\': out.push_back('\\'); break;
                    case '"': out.push_back('"'); break;
                    case '\'': out.push_back('\''); break;
                    case '0': out.push_back('\0'); break;
                    case 'x': {
                        if (i + 2 < body.size() &&
                            is_hex_digit_(body[i + 1]) &&
                            is_hex_digit_(body[i + 2])) {
                            const uint8_t hi = hex_digit_value_(body[i + 1]);
                            const uint8_t lo = hex_digit_value_(body[i + 2]);
                            out.push_back(static_cast<char>((hi << 4) | lo));
                            i += 2;
                            break;
                        }
                        out.push_back('x');
                        break;
                    }
                    default:
                        out.push_back(esc);
                        break;
                }
            }
            return out;
        }

        bool starts_with_(std::string_view s, std::string_view pfx) {
            return s.size() >= pfx.size() && s.substr(0, pfx.size()) == pfx;
        }

        bool ends_with_(std::string_view s, std::string_view sfx) {
            return s.size() >= sfx.size() && s.substr(s.size() - sfx.size()) == sfx;
        }

        std::string parse_string_literal_bytes_(std::string_view text) {
            if (starts_with_(text, "c\"") && text.size() >= 3 && text.back() == '"') {
                const auto body = text.substr(2, text.size() - 3);
                return decode_escaped_string_body_(body);
            }

            if (starts_with_(text, "cr\"") && text.size() >= 4 && text.back() == '"') {
                return std::string(text.substr(3, text.size() - 4));
            }

            if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
                const auto body = text.substr(1, text.size() - 2);
                return decode_escaped_string_body_(body);
            }

            if (starts_with_(text, "$\"") && text.size() >= 3 && text.back() == '"') {
                const auto body = text.substr(2, text.size() - 3);
                return decode_escaped_string_body_(body);
            }

            if (starts_with_(text, "R\"\"\"") && ends_with_(text, "\"\"\"") && text.size() >= 7) {
                return std::string(text.substr(4, text.size() - 7));
            }

            if (starts_with_(text, "F\"\"\"") && ends_with_(text, "\"\"\"") && text.size() >= 7) {
                // v0: F-string interpolation is not yet lowered. Keep body as raw UTF-8 text.
                return std::string(text.substr(4, text.size() - 7));
            }

            return std::string(text);
        }

        std::string normalize_symbol_fragment_(std::string_view in) {
            std::string out;
            out.reserve(in.size());
            for (char c : in) {
                const unsigned char u = static_cast<unsigned char>(c);
                if (std::isalnum(u) || c == '_') out.push_back(c);
                else out.push_back('_');
            }
            if (out.empty()) out = "_";
            return out;
        }

        uint64_t fnv1a64_(std::string_view s) {
            uint64_t h = 1469598103934665603ull;
            for (char c : s) {
                h ^= static_cast<unsigned char>(c);
                h *= 1099511628211ull;
            }
            return h;
        }

        /// @brief 함수 이름 + 시그니처를 기반으로 OIR 내부 함수명을 생성한다.
        std::string mangle_func_name_(
            const parus::sir::Func& sf,
            const parus::sir::Module& sir,
            const parus::ty::TypePool& types,
            std::string_view bundle_name
        ) {
            std::string sig{};
            const uint64_t pb = sf.param_begin;
            const uint64_t pe = pb + sf.param_count;
            if (pb <= sir.params.size() && pe <= sir.params.size() &&
                sf.ret != parus::ty::kInvalidType) {
                sig = "def(";
                for (uint32_t pi = 0; pi < sf.param_count; ++pi) {
                    if (pi != 0) sig += ", ";
                    sig += types.to_export_string(sir.params[sf.param_begin + pi].type);
                }
                sig += ") -> ";
                sig += types.to_export_string(sf.ret);
            } else {
                sig = (sf.sig != parus::ty::kInvalidType)
                    ? types.to_export_string(sf.sig)
                    : std::string("def(?)");
            }

            const std::string qname(sf.name);
            std::string mode = "none";
            switch (sf.fn_mode) {
                case parus::sir::FnMode::kPub: mode = "pub"; break;
                case parus::sir::FnMode::kSub: mode = "sub"; break;
                case parus::sir::FnMode::kNone: default: mode = "none"; break;
            }
            return build_parus_link_name_from_qname_and_sig_(
                bundle_name,
                qname,
                sig,
                mode
            );
        }

        std::string make_module_init_symbol_name_(
            std::string_view bundle_name,
            std::string_view current_source_norm
        ) {
            const std::string bundle = bundle_name.empty() ? std::string("main") : std::string(bundle_name);
            const std::string canonical =
                "bundle=" + bundle + "|source=" + std::string(current_source_norm);
            std::ostringstream hs;
            hs << std::hex << fnv1a64_(canonical);
            return "__parus_module_init__" + hs.str();
        }

        bool known_concrete_leaf_type_name_without_symbols_(
            const parus::sir::Module& sir,
            const parus::ty::TypePool& types,
            std::string_view leaf
        ) {
            auto matches_named_leaf_ = [&](TypeId t) -> bool {
                if (t == parus::ty::kInvalidType) return false;
                std::vector<std::string_view> path{};
                std::vector<TypeId> args{};
                if (!types.decompose_named_user(t, path, args) || path.empty()) return false;
                return path.back() == leaf;
            };

            for (const auto& fd : sir.fields) {
                if (matches_named_leaf_(fd.self_type)) return true;
            }
            for (const auto& ad : sir.acts) {
                if (matches_named_leaf_(ad.target_type)) return true;
            }
            for (const auto& fn : sir.funcs) {
                if (matches_named_leaf_(fn.actor_owner_type)) return true;
            }
            return false;
        }

        bool leaf_name_resolves_to_concrete_type_(
            const parus::sir::Module& sir,
            const parus::ty::TypePool& types,
            const parus::sema::SymbolTable* sym,
            std::string_view leaf
        ) {
            if (sym != nullptr) {
                if (auto sid = sym->lookup(std::string(leaf))) {
                    const auto& ss = sym->symbol(*sid);
                    if (ss.kind == parus::sema::SymbolKind::kType ||
                        ss.kind == parus::sema::SymbolKind::kField) {
                        return true;
                    }
                }
            }
            return known_concrete_leaf_type_name_without_symbols_(sir, types, leaf);
        }

        /// @brief SIR 함수 ABI를 OIR 함수 ABI로 변환한다.
        FunctionAbi map_func_abi_(parus::sir::FuncAbi abi) {
            switch (abi) {
                case parus::sir::FuncAbi::kC: return FunctionAbi::C;
                case parus::sir::FuncAbi::kParus:
                default: return FunctionAbi::Parus;
            }
        }

        CCallConv map_c_callconv_(parus::sir::CCallConv cc) {
            switch (cc) {
                case parus::sir::CCallConv::kCdecl: return CCallConv::Cdecl;
                case parus::sir::CCallConv::kStdCall: return CCallConv::StdCall;
                case parus::sir::CCallConv::kFastCall: return CCallConv::FastCall;
                case parus::sir::CCallConv::kVectorCall: return CCallConv::VectorCall;
                case parus::sir::CCallConv::kWin64: return CCallConv::Win64;
                case parus::sir::CCallConv::kSysV: return CCallConv::SysV;
                case parus::sir::CCallConv::kDefault:
                default:
                    return CCallConv::Default;
            }
        }

        CCallConv map_c_callconv_(parus::ty::CCallConv cc) {
            switch (cc) {
                case parus::ty::CCallConv::kCdecl: return CCallConv::Cdecl;
                case parus::ty::CCallConv::kStdCall: return CCallConv::StdCall;
                case parus::ty::CCallConv::kFastCall: return CCallConv::FastCall;
                case parus::ty::CCallConv::kVectorCall: return CCallConv::VectorCall;
                case parus::ty::CCallConv::kWin64: return CCallConv::Win64;
                case parus::ty::CCallConv::kSysV: return CCallConv::SysV;
                case parus::ty::CCallConv::kDefault:
                default:
                    return CCallConv::Default;
            }
        }

        FieldLayout map_field_layout_(parus::sir::FieldLayout layout) {
            switch (layout) {
                case parus::sir::FieldLayout::kC:
                    return FieldLayout::C;
                case parus::sir::FieldLayout::kNone:
                default:
                    return FieldLayout::None;
            }
        }

        uint32_t align_to_(uint32_t value, uint32_t align) {
            if (align == 0 || align == 1) return value;
            const uint32_t rem = value % align;
            return (rem == 0) ? value : (value + (align - rem));
        }

        // -----------------------
        // Lower expressions
        // -----------------------
        ValueId FuncBuild::lower_block_expr(parus::sir::ValueId block_expr_vid) {
            const auto& v = sir->values[block_expr_vid];
            // SIR kBlockExpr: v.a = BlockId, v.b = last expr value (convention in your dumps)
            parus::sir::BlockId bid = (parus::sir::BlockId)v.a;
            parus::sir::ValueId last = (parus::sir::ValueId)v.b;

            // BlockExpr executes statements in that SIR block in current control-flow
            push_scope();
            lower_block(bid);
            ValueId outv = (last != parus::sir::k_invalid_value) ? lower_value(last) : emit_const_null(v.type);
            pop_scope();
            return outv;
        }

        ValueId FuncBuild::lower_if_expr(parus::sir::ValueId if_vid) {
            const auto& v = sir->values[if_vid];
            // SIR kIfExpr: v.a = cond, v.b = then blockexpr/value, v.c = else blockexpr/value
            auto cond_sir = (parus::sir::ValueId)v.a;
            auto then_sir = (parus::sir::ValueId)v.b;
            auto else_sir = (parus::sir::ValueId)v.c;

            ValueId cond = lower_value(cond_sir);

            // create blocks
            BlockId then_bb = new_block();
            BlockId else_bb = new_block();
            BlockId join_bb = new_block();

            // join has one param: result of if expr
            ValueId join_param = add_block_param(join_bb, v.type);

            // terminate current with condbr
            condbr(cond, then_bb, {}, else_bb, {});

            // THEN
            def->blocks.push_back(then_bb);
            cur_bb = then_bb;
            push_scope();
            ValueId then_val = lower_value(then_sir);
            pop_scope();
            if (!has_term()) br(join_bb, {then_val});

            // ELSE
            def->blocks.push_back(else_bb);
            cur_bb = else_bb;
            push_scope();
            ValueId else_val = lower_value(else_sir);
            pop_scope();
            if (!has_term()) br(join_bb, {else_val});

            // JOIN
            def->blocks.push_back(join_bb);
            cur_bb = join_bb;

            // NOTE: In v0 we do not yet verify arg counts strictly here,
            // but verify() will later ensure terminators exist.
            (void)join_param;
            return join_param;
        }

        ValueId FuncBuild::lower_place(parus::sir::ValueId vid, TypeId* out_place_ty) {
            if (out_place_ty != nullptr) {
                *out_place_ty = parus::ty::kInvalidType;
            }
            if (vid == parus::sir::k_invalid_value || sir == nullptr ||
                (size_t)vid >= sir->values.size()) {
                report_lowering_error("place lowering failed: invalid SIR value id");
                return kInvalidId;
            }

            const auto& v = sir->values[vid];
            const TypeId place_ty =
                (v.place_elem_type != parus::sir::k_invalid_type)
                    ? (TypeId)v.place_elem_type
                    : (TypeId)v.type;
            if (out_place_ty != nullptr) {
                *out_place_ty = place_ty;
            }

            auto lower_subplace_base = [&](parus::sir::ValueId base_vid) -> ValueId {
                if (sir == nullptr || base_vid == parus::sir::k_invalid_value ||
                    (size_t)base_vid >= sir->values.size()) {
                    return lower_place(base_vid);
                }
                const auto& base_sv = sir->values[base_vid];
                const TypeId base_ty =
                    (base_sv.place_elem_type != parus::sir::k_invalid_type)
                        ? (TypeId)base_sv.place_elem_type
                        : (TypeId)base_sv.type;
                if (types != nullptr && base_ty != kInvalidId) {
                    const auto& bt = types->get(base_ty);
                    if (bt.kind == parus::ty::Kind::kBorrow ||
                        bt.kind == parus::ty::Kind::kEscape ||
                        bt.kind == parus::ty::Kind::kPtr) {
                        return lower_value(base_vid);
                    }
                }
                return lower_place(base_vid);
            };

            if (v.kind == parus::sir::ValueKind::kIndex ||
                v.kind == parus::sir::ValueKind::kField) {
                if (auto root_sym = place_root_local_sym(vid); root_sym.has_value()) {
                    if (const ValueId root_slot = lookup_bound_slot(*root_sym); root_slot != kInvalidId) {
                        const auto rebuild = [&](parus::sir::ValueId cur_vid) -> ValueId {
                            const auto& cur = sir->values[cur_vid];
                            const TypeId cur_place_ty =
                                (cur.place_elem_type != parus::sir::k_invalid_type)
                                    ? (TypeId)cur.place_elem_type
                                    : (TypeId)cur.type;
                            switch (cur.kind) {
                                case parus::sir::ValueKind::kLocal:
                                    return root_slot;
                                case parus::sir::ValueKind::kIndex: {
                                    ValueId base_place = lower_subplace_base(cur.a);
                                    ValueId idx = lower_value(cur.b);
                                    return emit_index(cur_place_ty, base_place, idx);
                                }
                                case parus::sir::ValueKind::kField: {
                                    ValueId base_place = lower_subplace_base(cur.a);
                                    return emit_field(cur_place_ty, base_place, std::string(cur.text), {
                                        .is_valid = cur.external_c_bitfield.is_valid,
                                        .storage_offset_bytes = cur.external_c_bitfield.storage_offset_bytes,
                                        .bit_offset = cur.external_c_bitfield.bit_offset,
                                        .bit_width = cur.external_c_bitfield.bit_width,
                                        .bit_signed = cur.external_c_bitfield.bit_signed,
                                    });
                                }
                                default:
                                    report_lowering_error("place lowering failed: rooted rebuild hit non-place node");
                                    return lower_value(cur_vid);
                            }
                        };
                        return rebuild(vid);
                    }
                }
            }

            switch (v.kind) {
                case parus::sir::ValueKind::kLocal:
                    if (const ValueId slot = lookup_bound_slot(v.sym); slot != kInvalidId) {
                        return slot;
                    }
                    return ensure_slot(v.sym, place_ty);

                case parus::sir::ValueKind::kIndex: {
                    ValueId base_place = lower_subplace_base(v.a);
                    ValueId idx = lower_value(v.b);
                    return emit_index(place_ty, base_place, idx);
                }

                case parus::sir::ValueKind::kField: {
                    ValueId base_place = lower_subplace_base(v.a);
                    return emit_field(place_ty, base_place, std::string(v.text), {
                        .is_valid = v.external_c_bitfield.is_valid,
                        .storage_offset_bytes = v.external_c_bitfield.storage_offset_bytes,
                        .bit_offset = v.external_c_bitfield.bit_offset,
                        .bit_width = v.external_c_bitfield.bit_width,
                        .bit_signed = v.external_c_bitfield.bit_signed,
                    });
                }

                case parus::sir::ValueKind::kUnary: {
                    auto tk = static_cast<parus::syntax::TokenKind>(v.op);
                    if (tk == parus::syntax::TokenKind::kStar) {
                        return lower_value(v.a);
                    }
                    break;
                }

                default:
                    break;
            }

            report_lowering_error("place lowering failed: value is not an addressable place");
            return lower_value(vid);
        }

        ValueId FuncBuild::lower_value(parus::sir::ValueId vid) {
            const auto& v = sir->values[vid];

            switch (v.kind) {
            case parus::sir::ValueKind::kIntLit:
                return emit_const_int(v.type, std::string(v.text));

            case parus::sir::ValueKind::kFloatLit: {
                const auto lit = parse_float_literal_text_(v.text);
                if (!lit.has_value()) {
                    report_lowering_error(
                        std::string("unsupported or invalid float literal during OIR lowering: '") +
                        std::string(v.text) + "'");
                    return emit_const_null(v.type);
                }
                return emit_const_float(v.type, *lit);
            }

            case parus::sir::ValueKind::kCharLit: {
                const auto code = parse_char_literal_code_(v.text);
                if (!code.has_value()) {
                    report_lowering_error(
                        std::string("unsupported or invalid char literal during OIR lowering: '") +
                        std::string(v.text) + "'");
                    return emit_const_null(v.type);
                }
                return emit_const_char(v.type, *code);
            }

            case parus::sir::ValueKind::kBoolLit:
                return emit_const_bool(v.type, v.text == "true");

            case parus::sir::ValueKind::kStringLit:
                return emit_const_text(v.type, parse_string_literal_bytes_(v.text));

            case parus::sir::ValueKind::kNullLit:
                return emit_const_null(v.type);

            case parus::sir::ValueKind::kArrayLit: {
                // v0: 배열 리터럴은 함수 로컬 slot에 materialize한 뒤
                // 해당 slot 포인터 값을 결과로 전달한다.
                // (LLVM lowering 단계에서 index/store/load가 실제 주소 계산으로 변환됨)
                ValueId arr_slot = emit_alloca(v.type);

                TypeId elem_ty = v.type;
                TypeId idx_ty = v.type;
                if (types != nullptr && v.type != parus::ty::kInvalidType) {
                    const auto& t = types->get(v.type);
                    if (t.kind == parus::ty::Kind::kArray && t.elem != parus::ty::kInvalidType) {
                        elem_ty = (TypeId)t.elem;
                    }
                    idx_ty = (TypeId)types->builtin(parus::ty::Builtin::kI64);
                }

                const uint64_t arg_end = (uint64_t)v.arg_begin + (uint64_t)v.arg_count;
                if (arg_end <= (uint64_t)sir->args.size()) {
                    for (uint32_t i = 0; i < v.arg_count; ++i) {
                        const auto& a = sir->args[v.arg_begin + i];
                        if (a.is_hole || a.value == parus::sir::k_invalid_value) continue;

                        ValueId elem_val = lower_value(a.value);
                        ValueId idx_val = emit_const_int(idx_ty, std::to_string(i));
                        ValueId elem_place = emit_index(elem_ty, arr_slot, idx_val);
                        emit_store(elem_place, elem_val);
                    }
                }
                return arr_slot;
            }

            case parus::sir::ValueKind::kFieldInit: {
                if (types != nullptr && v.type != kInvalidId) {
                    const auto& tv = types->get(v.type);
                    if (tv.kind == parus::ty::Kind::kBuiltin &&
                        tv.builtin == parus::ty::Builtin::kText) {
                        ValueId text_slot = emit_alloca(v.type);
                        const uint64_t arg_end = (uint64_t)v.arg_begin + (uint64_t)v.arg_count;
                        if (arg_end <= (uint64_t)sir->args.size()) {
                            for (uint32_t i = 0; i < v.arg_count; ++i) {
                                const auto& a = sir->args[v.arg_begin + i];
                                if (a.value == parus::sir::k_invalid_value) continue;

                                ValueId rhs = lower_value(a.value);
                                TypeId member_ty = kInvalidId;
                                if (a.label == "data") {
                                    if (rhs != kInvalidId && (size_t)rhs < out->values.size()) {
                                        member_ty = out->values[rhs].ty;
                                    }
                                } else if (a.label == "len") {
                                    member_ty = (TypeId)types->builtin(parus::ty::Builtin::kUSize);
                                }
                                if (member_ty == kInvalidId) continue;
                                rhs = coerce_value_for_target(member_ty, rhs);
                                ValueId place = emit_field(member_ty, text_slot, std::string(a.label));
                                emit_store(place, rhs);
                                consume_owned_sir_value_(a.value, member_ty);
                            }
                        }
                        return text_slot;
                    }
                }

                // v0: struct 리터럴은 임시 슬롯에 멤버를 순서대로 store하여 물질화한다.
                // 반환값은 aggregate slot 포인터 표현을 따른다.
                ValueId obj_slot = emit_alloca(v.type);

                const FieldLayoutDecl* layout = nullptr;
                for (const auto& f : out->fields) {
                    if (f.self_type == v.type) {
                        layout = &f;
                        break;
                    }
                }

                auto lookup_member_ty = [&](std::string_view member) -> TypeId {
                    if (layout == nullptr) return kInvalidId;
                    for (const auto& m : layout->members) {
                        if (m.name == member) return m.type;
                    }
                    return kInvalidId;
                };

                const uint64_t arg_end = (uint64_t)v.arg_begin + (uint64_t)v.arg_count;
                if (arg_end <= (uint64_t)sir->args.size()) {
                    for (uint32_t i = 0; i < v.arg_count; ++i) {
                        const auto& a = sir->args[v.arg_begin + i];
                        if (a.value == parus::sir::k_invalid_value) continue;

                        ValueId rhs = lower_value(a.value);
                        TypeId member_ty = lookup_member_ty(a.label);
                        if (member_ty == kInvalidId &&
                            rhs != kInvalidId &&
                            (size_t)rhs < out->values.size()) {
                            member_ty = out->values[rhs].ty;
                        }
                        if (member_ty == kInvalidId) member_ty = v.type;

                        rhs = coerce_value_for_target(member_ty, rhs);
                        ValueId place = emit_field(member_ty, obj_slot, std::string(a.label));
                        emit_store(place, rhs);
                        consume_owned_sir_value_(a.value, member_ty);
                    }
                }

                return obj_slot;
            }

            case parus::sir::ValueKind::kLocal:
                return read_local(v.sym, v.type);

            case parus::sir::ValueKind::kBorrow:
            case parus::sir::ValueKind::kEscape:
                // v0: borrow/escape는 컴파일타임 capability 토큰이다.
                // OIR에서는 비물질화 원칙을 유지하고 원본 값으로 전달한다.
                {
                    const ValueId lowered = lower_value(v.a);
                    if (v.kind == parus::sir::ValueKind::kBorrow) {
                        TypeId pointee_ty = v.type;
                        if (types != nullptr && pointee_ty != kInvalidId) {
                            const auto& bt = types->get(pointee_ty);
                            if (bt.kind == parus::ty::Kind::kBorrow && bt.elem != kInvalidId) {
                                pointee_ty = bt.elem;
                            }
                        }

                        const auto operand_kind =
                            ((size_t)v.a < sir->values.size())
                                ? sir->values[v.a].kind
                                : parus::sir::ValueKind::kError;

                        if (value_is_address_like_(lowered)) {
                            return lowered;
                        }

                        if (v.origin_sym != parus::sir::k_invalid_symbol &&
                            operand_kind == parus::sir::ValueKind::kLocal &&
                            pointee_ty != kInvalidId) {
                            return ensure_slot(v.origin_sym, pointee_ty);
                        }

                        if (pointee_ty != kInvalidId) {
                            ValueId slot = emit_alloca(pointee_ty);
                            ValueId store_v = lowered;
                            if (out != nullptr &&
                                (size_t)store_v < out->values.size() &&
                                out->values[store_v].ty != pointee_ty) {
                                store_v = coerce_value_for_target(pointee_ty, store_v);
                            }
                            emit_store(slot, store_v);
                            return slot;
                        }
                    }
                    if (v.kind == parus::sir::ValueKind::kEscape && escape_value_map != nullptr) {
                        (*escape_value_map)[vid] = lowered;
                    }
                    if (v.kind == parus::sir::ValueKind::kEscape &&
                        v.origin_sym != parus::sir::k_invalid_symbol) {
                        mark_symbol_moved(v.origin_sym, /*moved=*/true);
                    }
                    return lowered;
                }

            case parus::sir::ValueKind::kUnary: {
                ValueId src = lower_value(v.a);
                auto tk = static_cast<parus::syntax::TokenKind>(v.op);
                if (tk == parus::syntax::TokenKind::kStar) {
                    return emit_load(v.type, src);
                }
                if (tk == parus::syntax::TokenKind::kKwTry) {
                    // try expr: capture throwing-call state as nullable.
                    // - if exception flag is set -> null optional
                    // - else -> optional-some(src)
                    const ValueId active = emit_load_exc_active_();
                    const BlockId fail_bb = new_block();
                    const BlockId succ_bb = new_block();
                    const BlockId join_bb = new_block();
                    const ValueId join_param = add_block_param(join_bb, v.type);
                    condbr(active, fail_bb, {}, succ_bb, {});

                    def->blocks.push_back(fail_bb);
                    cur_bb = fail_bb;
                    emit_set_exc_active_(false);
                    emit_set_exc_type_(kInvalidId);
                    {
                        const ValueId null_opt = emit_const_null(v.type);
                        br(join_bb, {null_opt});
                    }

                    def->blocks.push_back(succ_bb);
                    cur_bb = succ_bb;
                    {
                        ValueId some_opt = src;
                        TypeId src_ty = kInvalidId;
                        if (src != kInvalidId && out != nullptr && (size_t)src < out->values.size()) {
                            src_ty = out->values[src].ty;
                        }
                        if (types != nullptr && v.type != kInvalidId) {
                            const auto& tt = types->get(v.type);
                            if (!(tt.kind == parus::ty::Kind::kOptional && tt.elem != kInvalidId && tt.elem == src_ty)) {
                                some_opt = emit_cast(v.type, Effect::Pure, CastKind::As, v.type, src);
                            }
                        }
                        br(join_bb, {some_opt});
                    }

                    def->blocks.push_back(join_bb);
                    cur_bb = join_bb;
                    return join_param;
                }
                if (tk == parus::syntax::TokenKind::kKwCopy ||
                    tk == parus::syntax::TokenKind::kKwClone) {
                    if (tk == parus::syntax::TokenKind::kKwClone &&
                        is_actor_handle_type_(v.type) &&
                        actor_runtime != nullptr &&
                        actor_runtime->clone_fn != kInvalidId) {
                        return emit_direct_call(v.type, actor_runtime->clone_fn, {src});
                    }
                    // copy/clone trivial builtin fast-path: value passthrough.
                    // Non-trivial paths are already lowered as direct calls from SIR.
                    return src;
                }
                auto op = map_unary(tk);
                if (!op.has_value()) {
                    report_lowering_error(
                        std::string("unsupported unary operator in OIR lowering: ") +
                        std::string(parus::syntax::token_kind_name(tk)));
                    return emit_const_null(v.type);
                }
                return emit_unary(v.type, Effect::Pure, *op, src);
            }

            case parus::sir::ValueKind::kBinary: {
                ValueId lhs = lower_value(v.a);
                ValueId rhs = lower_value(v.b);

                auto tk = static_cast<parus::syntax::TokenKind>(v.op);
                auto op = map_binop(tk);
                if (!op.has_value()) {
                    report_lowering_error(
                        std::string("unsupported binary operator in OIR lowering: ") +
                        std::string(parus::syntax::token_kind_name(tk)));
                    return emit_const_null(v.type);
                }

                // v0: 대부분 pure로 둔다. (??/비교도 pure)
                return emit_binop(v.type, Effect::Pure, *op, lhs, rhs);
            }

            case parus::sir::ValueKind::kCast: {
                ValueId src = lower_value(v.a);

                // SIR: v.op는 ast::CastKind 저장(이미 dump_sir_module도 그렇게 해석중)
                auto ck_ast = (parus::ast::CastKind)v.op;

                CastKind ck = CastKind::As;
                Effect eff = Effect::Pure;

                switch (ck_ast) {
                    case parus::ast::CastKind::kAs:
                        ck = CastKind::As;  eff = Effect::Pure;   break;
                    case parus::ast::CastKind::kAsOptional:
                        ck = CastKind::AsQ; eff = Effect::Pure;   break;
                    case parus::ast::CastKind::kAsForce:
                        ck = CastKind::AsB; eff = Effect::MayTrap;break;
                }

                return emit_cast(v.type, eff, ck, v.cast_to, src);
            }

            case parus::sir::ValueKind::kCall:
            case parus::sir::ValueKind::kPipeCall: {
                std::vector<ValueId> args;
                std::vector<parus::sir::ValueId> arg_value_ids;
                args.reserve(v.arg_count);
                arg_value_ids.reserve(v.arg_count);

                uint32_t i = 0;
                while (i < v.arg_count) {
                    const uint32_t aid = v.arg_begin + i;
                    if ((size_t)aid >= sir->args.size()) break;
                    const auto& a = sir->args[aid];

                    if (!a.is_hole && a.value != parus::sir::k_invalid_value) {
                        args.push_back(lower_value(a.value));
                        arg_value_ids.push_back(a.value);
                    }
                    ++i;
                }

                if (v.core_call_kind != parus::sir::CoreCallKind::kNone) {
                    auto emit_step_next_core_ = [&](ValueId cur, TypeId step_ty, TypeId opt_ty) -> ValueId {
                        if (types == nullptr || step_ty == kInvalidId || opt_ty == kInvalidId) {
                            report_lowering_error("core::iter::step_next lowering failed: invalid type metadata");
                            return emit_const_null(opt_ty);
                        }

                        auto emit_step_limit_ = [&](TypeId ty) -> ValueId {
                            using B = parus::ty::Builtin;
                            const auto& tt = types->get(ty);
                            if (tt.kind != parus::ty::Kind::kBuiltin) return emit_const_null(ty);
                            switch (tt.builtin) {
                                case B::kI8: return emit_const_int(ty, "127");
                                case B::kI16: return emit_const_int(ty, "32767");
                                case B::kI32: return emit_const_int(ty, "2147483647");
                                case B::kI64: return emit_const_int(ty, "9223372036854775807");
                                case B::kI128: return emit_const_int(ty, "170141183460469231731687303715884105727");
                                case B::kU8: return emit_const_int(ty, "255");
                                case B::kU16: return emit_const_int(ty, "65535");
                                case B::kU32: return emit_const_int(ty, "4294967295");
                                case B::kU64: return emit_const_int(ty, "18446744073709551615");
                                case B::kU128: return emit_const_int(ty, "340282366920938463463374607431768211455");
                                case B::kISize:
                                    return emit_const_int(
                                        ty,
                                        (sizeof(size_t) == 8u)
                                            ? "9223372036854775807"
                                            : "2147483647"
                                    );
                                case B::kUSize:
                                    return emit_const_int(
                                        ty,
                                        (sizeof(size_t) == 8u)
                                            ? "18446744073709551615"
                                            : "4294967295"
                                    );
                                case B::kChar: return emit_const_char(ty, 0x10FFFFu);
                                default: return emit_const_null(ty);
                            }
                        };

                        auto emit_step_one_ = [&](TypeId ty) -> ValueId {
                            using B = parus::ty::Builtin;
                            const auto& tt = types->get(ty);
                            if (tt.kind != parus::ty::Kind::kBuiltin) return emit_const_int(ty, "1");
                            if (tt.builtin == B::kChar) return emit_const_char(ty, 1u);
                            return emit_const_int(ty, "1");
                        };

                        const ValueId limit = emit_step_limit_(step_ty);
                        const ValueId at_end = emit_binop(bool_type_(), Effect::Pure, BinOp::Eq, cur, limit);
                        const BlockId fail_bb = new_block();
                        const BlockId succ_bb = new_block();
                        const BlockId join_bb = new_block();
                        const ValueId join_param = add_block_param(join_bb, opt_ty);
                        condbr(at_end, fail_bb, {}, succ_bb, {});

                        def->blocks.push_back(fail_bb);
                        cur_bb = fail_bb;
                        br(join_bb, {emit_const_null(opt_ty)});

                        def->blocks.push_back(succ_bb);
                        cur_bb = succ_bb;
                        {
                            const ValueId next_v =
                                emit_binop(step_ty, Effect::Pure, BinOp::Add, cur, emit_step_one_(step_ty));
                            const ValueId some_v = emit_cast(opt_ty, Effect::Pure, CastKind::As, opt_ty, next_v);
                            br(join_bb, {some_v});
                        }

                        def->blocks.push_back(join_bb);
                        cur_bb = join_bb;
                        return join_param;
                    };

                    switch (v.core_call_kind) {
                        case parus::sir::CoreCallKind::kMemSizeOf: {
                            const auto [size_bytes, align_bytes] = type_size_align_(v.core_call_type_arg);
                            (void)align_bytes;
                            return emit_const_int(v.type, std::to_string(std::max<uint32_t>(1u, size_bytes)));
                        }
                        case parus::sir::CoreCallKind::kMemAlignOf: {
                            const auto [size_bytes, align_bytes] = type_size_align_(v.core_call_type_arg);
                            (void)size_bytes;
                            return emit_const_int(v.type, std::to_string(std::max<uint32_t>(1u, align_bytes)));
                        }
                        case parus::sir::CoreCallKind::kHintSpinLoop: {
                            return emit_const_null(v.type);
                        }
                        case parus::sir::CoreCallKind::kStepNext: {
                            if (args.size() != 1u) {
                                report_lowering_error("core::iter::step_next lowering failed: expected 1 argument");
                                return emit_const_null(v.type);
                            }
                            const TypeId step_ty =
                                (v.core_call_type_arg != kInvalidId)
                                    ? v.core_call_type_arg
                                    : (((size_t)args[0] < out->values.size()) ? out->values[args[0]].ty : kInvalidId);
                            return emit_step_next_core_(args[0], step_ty, v.type);
                        }
                        case parus::sir::CoreCallKind::kMemReplace: {
                            if (args.size() != 2u) {
                                report_lowering_error("core::mem::replace lowering failed: expected 2 arguments");
                                return emit_const_null(v.type);
                            }
                            const ValueId slot = args[0];
                            if (!value_is_address_like_(slot)) {
                                report_lowering_error("core::mem::replace lowering failed: first argument is not addressable");
                                return emit_const_null(v.type);
                            }
                            const TypeId elem_ty =
                                (v.core_call_type_arg != kInvalidId) ? v.core_call_type_arg : v.type;
                            const ValueId oldv = emit_load(elem_ty, slot);
                            const ValueId newv = coerce_value_for_target(elem_ty, args[1]);
                            emit_store(slot, newv);
                            return oldv;
                        }
                        case parus::sir::CoreCallKind::kMemSwap: {
                            if (args.size() != 2u) {
                                report_lowering_error("core::mem::swap lowering failed: expected 2 arguments");
                                return emit_const_null(v.type);
                            }
                            const ValueId lhs = args[0];
                            const ValueId rhs = args[1];
                            if (!value_is_address_like_(lhs) || !value_is_address_like_(rhs)) {
                                report_lowering_error("core::mem::swap lowering failed: arguments must both be addressable");
                                return emit_const_null(v.type);
                            }
                            const TypeId elem_ty =
                                (v.core_call_type_arg != kInvalidId)
                                    ? v.core_call_type_arg
                                    : ((types != nullptr) ? (TypeId)types->builtin(parus::ty::Builtin::kUnit) : kInvalidId);
                            const ValueId lhs_v = emit_load(elem_ty, lhs);
                            const ValueId rhs_v = emit_load(elem_ty, rhs);
                            emit_store(lhs, rhs_v);
                            emit_store(rhs, lhs_v);
                            return emit_const_null(v.type);
                        }
                        case parus::sir::CoreCallKind::kNone:
                            break;
                    }
                }

                const bool ctor_call =
                    v.call_is_ctor &&
                    v.ctor_owner_type != parus::sir::k_invalid_type;
                const TypeId ctor_owner_ty = ctor_call ? (TypeId)v.ctor_owner_type : kInvalidId;

                ValueId callee = kInvalidId;
                FuncId direct_callee = kInvalidId;
                if (v.callee_decl_stmt != 0xFFFF'FFFFu && fn_decl_to_func != nullptr) {
                    auto dit = fn_decl_to_func->find(v.callee_decl_stmt);
                    if (dit != fn_decl_to_func->end() &&
                        (size_t)dit->second < out->funcs.size()) {
                        direct_callee = dit->second;
                    }
                }

                // 오버로드 decl-id가 이미 선택된 경우(direct_callee 유효)는
                // 해당 결정을 유지한다. 심볼 기반 재선택은 decl-id 정보가
                // 없는 일반 call 경로에서만 수행한다.
                if (direct_callee == kInvalidId &&
                    v.callee_sym != parus::sir::k_invalid_symbol &&
                    v.callee_fn_type != parus::sir::k_invalid_type) {
                    direct_callee = ensure_external_func_for_symbol_(
                        v.callee_sym, static_cast<TypeId>(v.callee_fn_type));
                }

                if (callee == kInvalidId &&
                    direct_callee == kInvalidId &&
                    v.callee_sym != parus::sir::k_invalid_symbol &&
                    fn_symbol_to_funcs != nullptr) {
                    auto fit = fn_symbol_to_funcs->find(v.callee_sym);
                    if (fit != fn_symbol_to_funcs->end()) {
                        FuncId best = kInvalidId;
                        uint32_t best_exact = 0;
                        for (const FuncId fid : fit->second) {
                            if ((size_t)fid >= out->funcs.size()) continue;
                            const auto& f = out->funcs[fid];
                            if (f.entry == kInvalidId || (size_t)f.entry >= out->blocks.size()) continue;
                            const auto& entry = out->blocks[f.entry];
                            size_t expected_param_count = args.size();
                            if (ctor_call && f.is_actor_init) {
                                expected_param_count += 2u;
                            }
                            if (entry.params.size() != expected_param_count) continue;

                            uint32_t exact = 0;
                            bool ok = true;
                            for (size_t ai = 0; ai < args.size(); ++ai) {
                                const auto p = entry.params[ai];
                                if ((size_t)p >= out->values.size() || (size_t)args[ai] >= out->values.size()) {
                                    ok = false;
                                    break;
                                }
                                if (out->values[p].ty == out->values[args[ai]].ty) {
                                    exact++;
                                }
                            }
                            if (!ok) continue;
                            if (best == kInvalidId || exact > best_exact) {
                                best = fid;
                                best_exact = exact;
                            }
                        }
                        if (best != kInvalidId) {
                            direct_callee = best;
                        }
                    }
                }

                if (callee == kInvalidId &&
                    direct_callee == kInvalidId &&
                    v.callee_sym != parus::sir::k_invalid_symbol &&
                    fn_symbol_to_func != nullptr) {
                    auto fit = fn_symbol_to_func->find(v.callee_sym);
                    if (fit != fn_symbol_to_func->end() &&
                        (size_t)fit->second < out->funcs.size()) {
                        direct_callee = fit->second;
                    }
                }
                if (direct_callee == kInvalidId &&
                    v.callee_sym != parus::sir::k_invalid_symbol &&
                    symtab != nullptr &&
                    static_cast<size_t>(v.callee_sym) < symtab->symbols().size()) {
                    const auto& ss = symtab->symbol(v.callee_sym);
                    if (ss.kind == parus::sema::SymbolKind::kFn) {
                        const std::string effective_link_name =
                            maybe_specialize_external_generic_link_name_(ss, ss.declared_type, *types);
                        if (fn_link_name_to_func != nullptr && !effective_link_name.empty()) {
                            auto fit = fn_link_name_to_func->find(effective_link_name);
                            if (fit != fn_link_name_to_func->end() &&
                                out != nullptr &&
                                (size_t)fit->second < out->funcs.size()) {
                                direct_callee = fit->second;
                            }
                        }
                        if (direct_callee == kInvalidId &&
                            fn_source_name_to_func != nullptr &&
                            !ss.name.empty()) {
                            auto fit = fn_source_name_to_func->find(ss.name);
                            if (fit != fn_source_name_to_func->end() &&
                                out != nullptr &&
                                (size_t)fit->second < out->funcs.size()) {
                                direct_callee = fit->second;
                            }
                        }
                    }
                }

                if (callee == kInvalidId && direct_callee == kInvalidId) {
                    if (v.a != parus::sir::k_invalid_value) {
                        callee = lower_value(v.a);
                    } else if (v.callee_sym != parus::sir::k_invalid_symbol) {
                        callee = read_local(v.callee_sym, ptr_type_());
                    }
                }

                if (direct_callee == kInvalidId) {
                    if (callee == kInvalidId) {
                        report_lowering_error("call lowering failed: unresolved callee value");
                        return emit_const_null(v.type);
                    }
                    if (out != nullptr &&
                        (size_t)callee < out->values.size()) {
                        const auto& cv = out->values[callee];
                        if (cv.def_a != kInvalidId &&
                            (size_t)cv.def_a < out->insts.size() &&
                            std::holds_alternative<InstConstNull>(out->insts[cv.def_a].data)) {
                            report_lowering_error("call lowering failed: callee resolved to null value");
                            return emit_const_null(v.type);
                        }
                    }
                }

                bool direct_callee_throwing = false;
                if (direct_callee != kInvalidId && throwing_funcs != nullptr) {
                    direct_callee_throwing = (throwing_funcs->find(direct_callee) != throwing_funcs->end());
                }

                const Function* direct_target =
                    (direct_callee != kInvalidId &&
                     out != nullptr &&
                     (size_t)direct_callee < out->funcs.size())
                        ? &out->funcs[direct_callee]
                        : nullptr;
                ValueId ctor_tmp_slot = kInvalidId;
                if (ctor_call && !(direct_target != nullptr && direct_target->is_actor_init)) {
                    ctor_tmp_slot = emit_alloca(ctor_owner_ty);
                    args.push_back(ctor_tmp_slot);
                }

                auto actor_mode_for_ = [](const Function& f) -> ActorEnterMode {
                    if (f.is_actor_init) return ActorEnterMode::kInit;
                    if (f.name.find("$Msub$") != std::string::npos) return ActorEnterMode::kSub;
                    return ActorEnterMode::kPub;
                };

                auto coerce_args_for_direct_ = [&](std::vector<ValueId>& inout_args) {
                    if (direct_target == nullptr) return;
                    if (direct_target->entry == kInvalidId ||
                        (size_t)direct_target->entry >= out->blocks.size()) {
                        return;
                    }
                    auto is_core_ext_cstr_type = [&](TypeId t) -> bool {
                        if (t == kInvalidId) return false;
                        const auto& tt0 = types->get(t);
                        if (tt0.kind == parus::ty::Kind::kBorrow) t = tt0.elem;
                        if (t == kInvalidId) return false;
                        const auto& tt = types->get(t);
                        if (tt.kind != parus::ty::Kind::kNamedUser) return false;
                        std::vector<std::string_view> path{};
                        std::vector<TypeId> args{};
                        if (!types->decompose_named_user(t, path, args)) return false;
                        if (!args.empty() || path.empty()) return false;
                        if (path.back() != "CStr") return false;
                        if (path.size() == 1) return true;
                        const std::string_view parent = path[path.size() - 2];
                        return parent == "ext" || parent == "core";
                    };
                    const auto& entry = out->blocks[direct_target->entry];
                    const size_t n = std::min(entry.params.size(), inout_args.size());
                    auto try_preserve_local_slot_for_borrow_param_ =
                        [&](size_t ai, TypeId param_ty) -> ValueId {
                            if (types == nullptr ||
                                sir == nullptr ||
                                param_ty == kInvalidId ||
                                ai >= arg_value_ids.size()) {
                                return kInvalidId;
                            }

                            const auto& pt = types->get(param_ty);
                            if ((pt.kind != parus::ty::Kind::kBorrow &&
                                 pt.kind != parus::ty::Kind::kEscape) ||
                                pt.elem == kInvalidId) {
                                return kInvalidId;
                            }

                            const parus::sir::ValueId arg_vid = arg_value_ids[ai];
                            if (arg_vid == parus::sir::k_invalid_value ||
                                (size_t)arg_vid >= sir->values.size()) {
                                return kInvalidId;
                            }

                            const auto& arg_sv = sir->values[arg_vid];
                            const auto arg_already_borrow_like = [&]() -> bool {
                                if (arg_sv.type == kInvalidId || static_cast<size_t>(arg_sv.type) >= types->count()) {
                                    return false;
                                }
                                const auto& arg_tt = types->get(arg_sv.type);
                                return (arg_tt.kind == parus::ty::Kind::kBorrow ||
                                        arg_tt.kind == parus::ty::Kind::kEscape) &&
                                       arg_tt.elem == pt.elem;
                            }();
                            switch (arg_sv.kind) {
                                case parus::sir::ValueKind::kLocal:
                                    if (arg_already_borrow_like) {
                                        break;
                                    }
                                    if (arg_sv.sym != parus::sir::k_invalid_symbol) {
                                        return ensure_slot(arg_sv.sym, pt.elem);
                                    }
                                    break;
                                case parus::sir::ValueKind::kBorrow:
                                case parus::sir::ValueKind::kEscape:
                                    if (arg_already_borrow_like) {
                                        break;
                                    }
                                    if (arg_sv.origin_sym != parus::sir::k_invalid_symbol) {
                                        return ensure_slot(arg_sv.origin_sym, pt.elem);
                                    }
                                    break;
                                default:
                                    break;
                            }
                            return kInvalidId;
                        };
                    for (size_t ai = 0; ai < n; ++ai) {
                        const ValueId p = entry.params[ai];
                        if ((size_t)p >= out->values.size()) continue;
                        const TypeId param_ty = out->values[p].ty;
                        if (const ValueId slot = try_preserve_local_slot_for_borrow_param_(ai, param_ty);
                            slot != kInvalidId) {
                            inout_args[ai] = slot;
                            continue;
                        }
                        inout_args[ai] = coerce_value_for_target(param_ty, inout_args[ai]);
                    }

                    if (direct_target->abi == FunctionAbi::C &&
                        direct_target->is_c_variadic &&
                        inout_args.size() > n) {
                        auto emit_core_ext_cstr_ptr_ = [&](ValueId src_value) -> ValueId {
                            const TypeId src_value_ty =
                                ((size_t)src_value < out->values.size()) ? out->values[src_value].ty : kInvalidId;
                            if (const FieldLayoutDecl* layout = find_field_layout_(src_value_ty)) {
                                for (const auto& m : layout->members) {
                                    if (m.name == "ptr_" && m.type != kInvalidId) {
                                        return emit_field(m.type, src_value, "ptr_");
                                    }
                                }
                            }
                            return emit_field(ptr_type_(), src_value, "ptr_");
                        };
                        for (size_t ai = n; ai < inout_args.size(); ++ai) {
                            const ValueId v = inout_args[ai];
                            if ((size_t)v >= out->values.size()) continue;
                            const TypeId vty = out->values[v].ty;
                            if (!is_core_ext_cstr_type(vty)) continue;
                            inout_args[ai] = emit_core_ext_cstr_ptr_(v);
                        }
                    }
                };

                auto consume_direct_owned_args_ = [&]() {
                    if (direct_target == nullptr) return;
                    if (direct_target->entry == kInvalidId ||
                        (size_t)direct_target->entry >= out->blocks.size()) {
                        return;
                    }
                    const auto& entry = out->blocks[direct_target->entry];
                    const size_t n = std::min(entry.params.size(), arg_value_ids.size());
                    for (size_t ai = 0; ai < n; ++ai) {
                        if (direct_target->is_actor_member &&
                            direct_target->actor_ctx_param_index != kInvalidId &&
                            direct_target->actor_ctx_param_index > 0 &&
                            ai == (size_t)(direct_target->actor_ctx_param_index - 1)) {
                            continue;
                        }
                        const ValueId p = entry.params[ai];
                        if ((size_t)p >= out->values.size()) continue;
                        consume_owned_sir_value_(arg_value_ids[ai], out->values[p].ty);
                    }
                };

                if (direct_callee_throwing) {
                    // call-site starts with a clean exception flag.
                    emit_set_exc_active_(false);
                    emit_set_exc_type_(kInvalidId);
                }

                if (direct_target != nullptr &&
                    actor_runtime != nullptr &&
                    ctor_call &&
                    direct_target->is_actor_init) {
                    const FieldLayoutDecl* layout = find_field_layout_(ctor_owner_ty);
                    if (layout == nullptr) {
                        report_lowering_error("actor constructor lowering failed: missing actor draft layout");
                        return emit_const_null(v.type);
                    }

                    const ValueId type_tag = emit_const_int(u64_type_(), std::to_string((uint32_t)ctor_owner_ty));
                    const ValueId draft_size = emit_const_int(u64_type_(), std::to_string(std::max<uint32_t>(1u, layout->size)));
                    const ValueId draft_align = emit_const_int(u64_type_(), std::to_string(std::max<uint32_t>(1u, layout->align)));
                    const ValueId handle = emit_direct_call(ctor_owner_ty, actor_runtime->new_fn, {type_tag, draft_size, draft_align});
                    const ValueId mode = emit_const_int(u32_type_(), std::to_string((uint32_t)ActorEnterMode::kInit));
                    const ValueId ctx = emit_direct_call(ptr_type_(), actor_runtime->enter_fn, {handle, mode});
                    const ValueId draft = emit_direct_call(ctor_owner_ty, actor_runtime->draft_ptr_fn, {ctx});

                    args.push_back(draft);
                    args.push_back(ctx);
                    coerce_args_for_direct_(args);
                    consume_direct_owned_args_();

                    const TypeId unit_ty =
                        (types != nullptr)
                            ? (TypeId)types->builtin(parus::ty::Builtin::kUnit)
                            : kInvalidId;
                    (void)emit_direct_call(unit_ty, direct_callee, std::move(args));
                    (void)emit_direct_call(unit_ty, actor_runtime->leave_fn, {ctx});
                    return handle;
                }

                if (direct_target != nullptr &&
                    actor_runtime != nullptr &&
                    direct_target->is_actor_member) {
                    if (direct_target->actor_ctx_param_index == kInvalidId ||
                        direct_target->actor_ctx_param_index == 0 ||
                        direct_target->actor_ctx_param_index - 1 >= args.size()) {
                        report_lowering_error("actor method lowering failed: missing hidden receiver");
                        return emit_const_null(v.type);
                    }

                    const size_t receiver_index = direct_target->actor_ctx_param_index - 1;
                    const ValueId handle = args[receiver_index];
                    const ValueId mode = emit_const_int(
                        u32_type_(),
                        std::to_string((uint32_t)actor_mode_for_(*direct_target))
                    );
                    const ValueId ctx = emit_direct_call(ptr_type_(), actor_runtime->enter_fn, {handle, mode});
                    const ValueId draft = emit_direct_call(direct_target->actor_owner_type, actor_runtime->draft_ptr_fn, {ctx});
                    args[receiver_index] = draft;
                    args.push_back(ctx);
                    coerce_args_for_direct_(args);
                    consume_direct_owned_args_();
                    const ValueId result = emit_direct_call(v.type, direct_callee, std::move(args));
                    const TypeId unit_ty =
                        (types != nullptr)
                            ? (TypeId)types->builtin(parus::ty::Builtin::kUnit)
                            : kInvalidId;
                    (void)emit_direct_call(unit_ty, actor_runtime->leave_fn, {ctx});
                    return result;
                }

                coerce_args_for_direct_(args);
                consume_direct_owned_args_();

                if (ctor_call) {
                    const TypeId unit_ty =
                        (types != nullptr)
                            ? (TypeId)types->builtin(parus::ty::Builtin::kUnit)
                            : kInvalidId;
                    (void)emit_call(
                        unit_ty,
                        callee,
                        std::move(args),
                        direct_callee,
                        v.call_is_c_abi,
                        v.call_is_c_variadic,
                        map_c_callconv_(v.call_c_callconv),
                        v.call_c_fixed_param_count
                    );
                    if (ctor_tmp_slot != kInvalidId) return ctor_tmp_slot;
                    return emit_const_null(v.type);
                }

                return emit_call(
                    v.type,
                    callee,
                    std::move(args),
                    direct_callee,
                    v.call_is_c_abi,
                    v.call_is_c_variadic,
                    map_c_callconv_(v.call_c_callconv),
                    v.call_c_fixed_param_count
                );
            }

            case parus::sir::ValueKind::kEnumCtor: {
                const TypeId enum_ty =
                    (v.ctor_owner_type != parus::sir::k_invalid_type)
                        ? (TypeId)v.ctor_owner_type
                        : (TypeId)v.type;
                if (enum_ty == kInvalidId) {
                    report_lowering_error("enum constructor lowering failed: invalid owner type");
                    return emit_const_null(v.type);
                }

                ValueId enum_slot = emit_alloca(enum_ty);
                const FieldLayoutDecl* layout = find_field_layout_(enum_ty);
                if (layout == nullptr) {
                    report_lowering_error("enum constructor lowering failed: missing enum layout");
                    return enum_slot;
                }

                auto field_type_of = [&](std::string_view name) -> TypeId {
                    for (const auto& m : layout->members) {
                        if (m.name == name) return m.type;
                    }
                    return kInvalidId;
                };

                const TypeId tag_ty = field_type_of("__tag");
                const bool tag_only_enum =
                    (layout != nullptr &&
                     layout->members.size() == 1 &&
                     layout->members[0].name == "__tag");
                if (tag_only_enum) {
                    return emit_const_int(enum_ty, std::to_string(v.enum_ctor_tag_value));
                }
                if (tag_ty != kInvalidId) {
                    ValueId tag_v = emit_const_int(tag_ty, std::to_string(v.enum_ctor_tag_value));
                    ValueId tag_p = emit_field(tag_ty, enum_slot, "__tag");
                    emit_store(tag_p, tag_v);
                }

                const uint64_t arg_end = (uint64_t)v.arg_begin + (uint64_t)v.arg_count;
                if (arg_end <= (uint64_t)sir->args.size()) {
                    for (uint32_t i = 0; i < v.arg_count; ++i) {
                        const auto& a = sir->args[v.arg_begin + i];
                        if (a.value == parus::sir::k_invalid_value) continue;

                        const std::string storage_name =
                            std::string("__v") + std::to_string(v.enum_ctor_variant_index) +
                            "_" + std::string(a.label);
                        TypeId member_ty = field_type_of(storage_name);
                        if (member_ty == kInvalidId) continue;

                        ValueId rhs = lower_value(a.value);
                        rhs = coerce_value_for_target(member_ty, rhs);
                        ValueId place = emit_field(member_ty, enum_slot, storage_name);
                        emit_store(place, rhs);
                        consume_owned_sir_value_(a.value, member_ty);
                    }
                }

                return enum_slot;
            }

            case parus::sir::ValueKind::kIndex: {
                ValueId base = lower_value(v.a);
                if (v.b != parus::sir::k_invalid_value &&
                    (size_t)v.b < sir->values.size()) {
                    const auto& sub = sir->values[v.b];
                    if (sub.kind == parus::sir::ValueKind::kBinary) {
                        const auto sub_op = static_cast<parus::syntax::TokenKind>(sub.op);
                        if ((sub_op == parus::syntax::TokenKind::kDotDot ||
                             sub_op == parus::syntax::TokenKind::kDotDotColon) &&
                            sub.a != parus::sir::k_invalid_value &&
                            sub.b != parus::sir::k_invalid_value) {
                            ValueId lo = lower_value(sub.a);
                            ValueId hi = lower_value(sub.b);
                            return emit_slice_view(
                                v.type,
                                base,
                                lo,
                                hi,
                                /*hi_inclusive=*/(sub_op == parus::syntax::TokenKind::kDotDotColon)
                            );
                        }
                    }
                    if (sub.kind == parus::sir::ValueKind::kFieldInit &&
                        types != nullptr &&
                        (size_t)sub.arg_begin + (size_t)sub.arg_count <= sir->args.size()) {
                        bool is_range_value = false;
                        bool hi_inclusive = false;
                        if (sub.type != parus::sir::k_invalid_type) {
                            const auto& sub_ty = types->get(sub.type);
                            if (sub_ty.kind == parus::ty::Kind::kNamedUser) {
                                std::vector<std::string_view> path{};
                                std::vector<parus::ty::TypeId> type_args{};
                                if (types->decompose_named_user(sub.type, path, type_args) && !path.empty()) {
                                    const std::string_view leaf = path.back();
                                    if (leaf == "Range" || leaf == "RangeInclusive") {
                                        is_range_value = true;
                                        hi_inclusive = (leaf == "RangeInclusive");
                                    }
                                }
                            }
                        }
                        if (is_range_value) {
                            ValueId lo = kInvalidId;
                            ValueId hi = kInvalidId;
                            for (uint32_t i = 0; i < sub.arg_count; ++i) {
                                const auto& a = sir->args[sub.arg_begin + i];
                                if (!a.has_label) continue;
                                if (a.label == "start") lo = lower_value(a.value);
                                else if (a.label == "end") hi = lower_value(a.value);
                            }
                            if (lo != kInvalidId && hi != kInvalidId) {
                                return emit_slice_view(v.type, base, lo, hi, hi_inclusive);
                            }
                        }
                    }
                }

                ValueId idx = lower_value(v.b);
                return emit_index(v.type, base, idx);
            }

            case parus::sir::ValueKind::kField: {
                ValueId base = lower_value(v.a);
                if (types != nullptr &&
                    v.a != parus::sir::k_invalid_value &&
                    (size_t)v.a < sir->values.size()) {
                    const auto& base_sv = sir->values[v.a];
                    const auto& bt = types->get(base_sv.type);
                    if (bt.kind == parus::ty::Kind::kArray &&
                        bt.array_has_size &&
                        v.text == "len") {
                        return emit_const_int(v.type, std::to_string(bt.array_size));
                    }
                }
                return emit_field(v.type, base, std::string(v.text), {
                    .is_valid = v.external_c_bitfield.is_valid,
                    .storage_offset_bytes = v.external_c_bitfield.storage_offset_bytes,
                    .bit_offset = v.external_c_bitfield.bit_offset,
                    .bit_width = v.external_c_bitfield.bit_width,
                    .bit_signed = v.external_c_bitfield.bit_signed,
                });
            }

            case parus::sir::ValueKind::kAssign: {
                // v.a = place, v.b = rhs
                const auto& place = sir->values[v.a];
                ValueId rhs = lower_value(v.b);
                const bool is_null_coalesce_assign =
                    (v.op == static_cast<uint32_t>(parus::syntax::TokenKind::kQuestionQuestionAssign));

                if (place.kind == parus::sir::ValueKind::kLocal) {
                    // slot 타입은 place_elem_type 우선 (없으면 기존 place.type)
                    TypeId slot_elem_ty =
                        (place.place_elem_type != parus::sir::k_invalid_type)
                            ? (TypeId)place.place_elem_type
                            : (TypeId)place.type;

                    if (is_null_coalesce_assign && types != nullptr && slot_elem_ty != kInvalidId) {
                        const auto& st = types->get(slot_elem_ty);
                        if (st.kind == parus::ty::Kind::kOptional && st.elem != kInvalidId) {
                            const ValueId lhs_cur = read_local(place.sym, slot_elem_ty);
                            const ValueId rhs_elem = coerce_value_for_target(st.elem, rhs);
                            const ValueId merged_elem = emit_binop(
                                st.elem, Effect::Pure, BinOp::NullCoalesce, lhs_cur, rhs_elem);
                            rhs = coerce_value_for_target(slot_elem_ty, merged_elem);
                        }
                    }

                    ValueId slot = ensure_slot(place.sym, slot_elem_ty);
                    drop_symbol_before_overwrite(place.sym);
                    rhs = coerce_value_for_target(slot_elem_ty, rhs);
                    emit_store(slot, rhs);
                    mark_symbol_moved(place.sym, /*moved=*/false);
                    consume_owned_sir_value_(v.b, slot_elem_ty);
                    return rhs; // assign expr result
                }

                TypeId place_target_ty = parus::ty::kInvalidType;
                ValueId place_v = lower_place(v.a, &place_target_ty);
                if (place_v != kInvalidId && (size_t)place_v < out->values.size()) {
                    if (place_target_ty == kInvalidId) {
                        place_target_ty = out->values[place_v].ty;
                    }
                    if (is_null_coalesce_assign && types != nullptr) {
                        const TypeId target_ty = place_target_ty;
                        if (target_ty != kInvalidId) {
                            const auto& tt = types->get(target_ty);
                            if (tt.kind == parus::ty::Kind::kOptional && tt.elem != kInvalidId) {
                                const ValueId lhs_cur = emit_load(target_ty, place_v);
                                const ValueId rhs_elem = coerce_value_for_target(tt.elem, rhs);
                                const ValueId merged_elem = emit_binop(
                                    tt.elem, Effect::Pure, BinOp::NullCoalesce, lhs_cur, rhs_elem);
                                rhs = coerce_value_for_target(target_ty, merged_elem);
                            }
                        }
                    }
                    if (place_target_ty != kInvalidId) {
                        rhs = coerce_value_for_target(place_target_ty, rhs);
                    }
                }
                emit_store(place_v, rhs);
                if (place.kind == parus::sir::ValueKind::kField ||
                    place.kind == parus::sir::ValueKind::kIndex ||
                    (place.kind == parus::sir::ValueKind::kUnary &&
                     place.op == static_cast<uint32_t>(parus::syntax::TokenKind::kStar))) {
                    const TypeId target_ty = place_target_ty;
                    consume_owned_sir_value_(v.b, target_ty);
                }
                return rhs;
            }

            case parus::sir::ValueKind::kPostfixInc: {
                const auto& place = sir->values[v.a];
                if (place.kind == parus::sir::ValueKind::kLocal) {
                    TypeId slot_elem_ty =
                        (place.place_elem_type != parus::sir::k_invalid_type)
                            ? (TypeId)place.place_elem_type
                            : (TypeId)place.type;
                    ValueId slot = ensure_slot(place.sym, slot_elem_ty);
                    ValueId oldv = emit_load(v.type, slot);
                    ValueId one = emit_const_int(v.type, "1");
                    ValueId next = emit_binop(v.type, Effect::Pure, BinOp::Add, oldv, one);
                    emit_store(slot, next);
                    return oldv;
                }

                TypeId place_target_ty = kInvalidId;
                ValueId slot = lower_place(v.a, &place_target_ty);
                if (place_target_ty == kInvalidId) {
                    place_target_ty = v.type;
                }
                ValueId oldv = emit_load(place_target_ty, slot);
                ValueId one = emit_const_int(v.type, "1");
                ValueId next = emit_binop(v.type, Effect::Pure, BinOp::Add, oldv, one);
                emit_store(slot, next);
                return oldv;
            }

            case parus::sir::ValueKind::kBlockExpr:
                return lower_block_expr(vid);

            case parus::sir::ValueKind::kIfExpr:
                return lower_if_expr(vid);

            case parus::sir::ValueKind::kLoopExpr: {
                const BlockId exit_bb = new_block();
                const bool has_header = (v.op != 0u);
                const bool expects_break_value =
                    (types != nullptr &&
                     v.type != parus::sir::k_invalid_type &&
                     v.type != kInvalidId &&
                     !(types->get(v.type).kind == parus::ty::Kind::kBuiltin &&
                       (types->get(v.type).builtin == parus::ty::Builtin::kNull ||
                        types->get(v.type).builtin == parus::ty::Builtin::kNever)));
                const ValueId break_param =
                    expects_break_value ? add_block_param(exit_bb, v.type) : kInvalidId;

                if (!has_header) {
                    const BlockId body_bb = new_block();
                    if (!has_term()) br(body_bb, {});

                    def->blocks.push_back(body_bb);
                    cur_bb = body_bb;
                    const size_t loop_scope_base = env_stack.size();
                    loop_stack.push_back(LoopContext{
                        .break_bb = exit_bb,
                        .continue_bb = body_bb,
                        .expects_break_value = expects_break_value,
                        .break_ty = (TypeId)v.type,
                        .scope_depth_base = loop_scope_base
                    });
                    push_scope();
                    lower_block((parus::sir::BlockId)v.b);
                    pop_scope();
                    loop_stack.pop_back();
                    if (!has_term()) br(body_bb, {});

                    def->blocks.push_back(exit_bb);
                    cur_bb = exit_bb;
                    if (expects_break_value) return break_param;
                    return emit_const_null(v.type);
                }

                auto loop_kind = v.loop_source_kind;
                TypeId binder_ty = v.loop_binder_type;
                auto derive_loop_source_from_sir_type = [&](parus::sir::ValueId source)
                    -> std::pair<parus::LoopSourceKind, TypeId> {
                    using parus::LoopSourceKind;
                    if (source == parus::sir::k_invalid_value || sir == nullptr || types == nullptr) {
                        return {LoopSourceKind::kNone, kInvalidId};
                    }
                    if ((size_t)source >= sir->values.size()) {
                        return {LoopSourceKind::kNone, kInvalidId};
                    }
                    TypeId tid = sir->values[source].type;
                    for (uint32_t depth = 0; depth < 6 && tid != kInvalidId; ++depth) {
                        const auto& tt = types->get(tid);
                        if (tt.kind == parus::ty::Kind::kArray) {
                            return {
                                tt.array_has_size ? LoopSourceKind::kSizedArray
                                                  : LoopSourceKind::kSliceView,
                                tt.elem
                            };
                        }
                        if ((tt.kind == parus::ty::Kind::kBorrow ||
                             tt.kind == parus::ty::Kind::kEscape ||
                             tt.kind == parus::ty::Kind::kPtr) &&
                            tt.elem != kInvalidId) {
                            tid = tt.elem;
                            continue;
                        }
                        break;
                    }
                    return {LoopSourceKind::kNone, kInvalidId};
                };
                if ((loop_kind == parus::LoopSourceKind::kNone ||
                     loop_kind == parus::LoopSourceKind::kIteratorFutureUnsupported) &&
                    v.a != parus::sir::k_invalid_value) {
                    const auto [derived_kind, derived_binder] = derive_loop_source_from_sir_type(v.a);
                    if (derived_kind != parus::LoopSourceKind::kNone) {
                        loop_kind = derived_kind;
                        if (binder_ty == kInvalidId) binder_ty = derived_binder;
                    }
                }
                if (loop_kind == parus::LoopSourceKind::kNone ||
                    loop_kind == parus::LoopSourceKind::kIteratorFutureUnsupported) {
                    report_lowering_error("loop lowering failed: unsupported header loop source reached OIR");
                    def->blocks.push_back(exit_bb);
                    cur_bb = exit_bb;
                    return emit_const_null(v.type);
                }
                if (binder_ty == kInvalidId || types == nullptr || v.sym == parus::sir::k_invalid_symbol) {
                    report_lowering_error("loop lowering failed: missing binder metadata");
                    def->blocks.push_back(exit_bb);
                    cur_bb = exit_bb;
                    return emit_const_null(v.type);
                }

                auto array_static_len_of = [&](ValueId base) -> std::optional<uint32_t> {
                    if (out == nullptr || types == nullptr || base == kInvalidId) return std::nullopt;
                    if ((size_t)base >= out->values.size()) return std::nullopt;
                    TypeId tid = out->values[base].ty;
                    for (uint32_t depth = 0; depth < 6 && tid != kInvalidId; ++depth) {
                        const auto& tt = types->get(tid);
                        if (tt.kind == parus::ty::Kind::kArray) {
                            if (tt.array_has_size) return tt.array_size;
                            return std::nullopt;
                        }
                        if ((tt.kind == parus::ty::Kind::kBorrow ||
                             tt.kind == parus::ty::Kind::kEscape ||
                             tt.kind == parus::ty::Kind::kPtr) &&
                            tt.elem != kInvalidId) {
                            tid = tt.elem;
                            continue;
                        }
                        break;
                    }
                    return std::nullopt;
                };

                auto resolve_loop_direct_callee_ = [&](uint32_t decl_sid,
                                                       parus::sir::SymbolId callee_sym,
                                                       TypeId fn_type) -> FuncId {
                    if (decl_sid != 0xFFFF'FFFFu && fn_decl_to_func != nullptr) {
                        auto dit = fn_decl_to_func->find(decl_sid);
                        if (dit != fn_decl_to_func->end() &&
                            (size_t)dit->second < out->funcs.size()) {
                            return dit->second;
                        }
                    }
                    if (callee_sym != parus::sir::k_invalid_symbol && fn_type != kInvalidId) {
                        const FuncId fid = ensure_external_func_for_symbol_(callee_sym, fn_type);
                        if (fid != kInvalidId) return fid;
                    }
                    if (callee_sym != parus::sir::k_invalid_symbol &&
                        fn_symbol_to_func != nullptr) {
                        auto fit = fn_symbol_to_func->find(callee_sym);
                        if (fit != fn_symbol_to_func->end() &&
                            (size_t)fit->second < out->funcs.size()) {
                            return fit->second;
                        }
                    }
                    if (callee_sym != parus::sir::k_invalid_symbol &&
                        symtab != nullptr &&
                        static_cast<size_t>(callee_sym) < symtab->symbols().size()) {
                        const auto& ss = symtab->symbol(callee_sym);
                        if (ss.kind == parus::sema::SymbolKind::kFn) {
                            const std::string effective_link_name =
                                maybe_specialize_external_generic_link_name_(ss, ss.declared_type, *types);
                            if (fn_link_name_to_func != nullptr && !effective_link_name.empty()) {
                                auto fit = fn_link_name_to_func->find(effective_link_name);
                                if (fit != fn_link_name_to_func->end() &&
                                    (size_t)fit->second < out->funcs.size()) {
                                    return fit->second;
                                }
                            }
                            if (fn_source_name_to_func != nullptr && !ss.name.empty()) {
                                auto fit = fn_source_name_to_func->find(ss.name);
                                if (fit != fn_source_name_to_func->end() &&
                                    (size_t)fit->second < out->funcs.size()) {
                                    return fit->second;
                                }
                            }
                        }
                    }
                    return kInvalidId;
                };

                auto coerce_args_for_direct_loop_ = [&](FuncId direct_callee, std::vector<ValueId>& inout_args) {
                    if (direct_callee == kInvalidId || out == nullptr || (size_t)direct_callee >= out->funcs.size()) {
                        return;
                    }
                    const auto& target = out->funcs[direct_callee];
                    if (target.entry == kInvalidId || (size_t)target.entry >= out->blocks.size()) {
                        return;
                    }
                    const auto& entry = out->blocks[target.entry];
                    const size_t n = std::min(entry.params.size(), inout_args.size());
                    for (size_t ai = 0; ai < n; ++ai) {
                        const ValueId p = entry.params[ai];
                        if ((size_t)p >= out->values.size()) continue;
                        inout_args[ai] = coerce_value_for_target(out->values[p].ty, inout_args[ai]);
                    }
                };

                if (loop_kind == parus::LoopSourceKind::kSequence) {
                    const ValueId iterable = lower_value(v.a);
                    if (iterable == kInvalidId) {
                        report_lowering_error("sequence loop lowering failed: iterable value did not lower");
                        def->blocks.push_back(exit_bb);
                        cur_bb = exit_bb;
                        return emit_const_null(v.type);
                    }
                    if (v.loop_iterator_type == parus::sir::k_invalid_type) {
                        report_lowering_error("sequence loop lowering failed: missing iterator type metadata");
                        def->blocks.push_back(exit_bb);
                        cur_bb = exit_bb;
                        return emit_const_null(v.type);
                    }

                    const FuncId iter_callee =
                        resolve_loop_direct_callee_(
                            v.loop_iter_decl_stmt,
                            v.loop_iter_external_sym,
                            static_cast<TypeId>(v.loop_iter_fn_type));
                    const FuncId next_callee =
                        resolve_loop_direct_callee_(
                            v.loop_next_decl_stmt,
                            v.loop_next_external_sym,
                            static_cast<TypeId>(v.loop_next_fn_type));
                    if (iter_callee == kInvalidId || next_callee == kInvalidId) {
                        report_lowering_error("sequence loop lowering failed: unresolved iter/next target");
                        def->blocks.push_back(exit_bb);
                        cur_bb = exit_bb;
                        return emit_const_null(v.type);
                    }

                    std::vector<ValueId> iter_args{iterable};
                    coerce_args_for_direct_loop_(iter_callee, iter_args);
                    const ValueId iter_value =
                        emit_direct_call((TypeId)v.loop_iterator_type, iter_callee, std::move(iter_args));
                    const ValueId iter_slot = emit_alloca((TypeId)v.loop_iterator_type);
                    emit_store(iter_slot, iter_value);

                    if (binder_ty == kInvalidId) {
                        report_lowering_error("sequence loop lowering failed: missing binder type metadata");
                        def->blocks.push_back(exit_bb);
                        cur_bb = exit_bb;
                        return emit_const_null(v.type);
                    }
                    const ValueId item_slot = emit_alloca(binder_ty);
                    const BlockId head_bb = new_block();
                    const BlockId body_bb = new_block();
                    const BlockId latch_bb = new_block();

                    if (!has_term()) br(head_bb, {});

                    def->blocks.push_back(head_bb);
                    cur_bb = head_bb;
                    {
                        std::vector<ValueId> next_args{iter_slot, item_slot};
                        coerce_args_for_direct_loop_(next_callee, next_args);
                        const ValueId has_next =
                            emit_direct_call(bool_type_(), next_callee, std::move(next_args));
                        condbr(has_next,
                               body_bb,
                               {},
                               exit_bb,
                               expects_break_value ? std::vector<ValueId>{emit_const_null(v.type)} : std::vector<ValueId>{});
                    }

                    def->blocks.push_back(body_bb);
                    cur_bb = body_bb;
                    const size_t loop_scope_base = env_stack.size();
                    const ValueId elem = emit_load(binder_ty, item_slot);
                    loop_stack.push_back(LoopContext{
                        .break_bb = exit_bb,
                        .continue_bb = latch_bb,
                        .expects_break_value = expects_break_value,
                        .break_ty = (TypeId)v.type,
                        .scope_depth_base = loop_scope_base
                    });
                    push_scope();
                    bind(v.sym, Binding{false, false, elem, kInvalidId});
                    lower_block((parus::sir::BlockId)v.b);
                    pop_scope();
                    loop_stack.pop_back();
                    if (!has_term()) br(latch_bb, {});

                    def->blocks.push_back(latch_bb);
                    cur_bb = latch_bb;
                    if (!has_term()) br(head_bb, {});

                    def->blocks.push_back(exit_bb);
                    cur_bb = exit_bb;
                    if (expects_break_value) return break_param;
                    return emit_const_null(v.type);
                }

                if (loop_kind == parus::LoopSourceKind::kSizedArray ||
                    loop_kind == parus::LoopSourceKind::kSliceView) {
                    const ValueId base = lower_value(v.a);
                    if (base == kInvalidId) {
                        report_lowering_error("loop lowering failed: iterable value did not lower");
                        def->blocks.push_back(exit_bb);
                        cur_bb = exit_bb;
                        return emit_const_null(v.type);
                    }

                    const TypeId idx_ty = i64_type_();
                    ValueId len_v = kInvalidId;
                    if (loop_kind == parus::LoopSourceKind::kSizedArray) {
                        const auto maybe_len = array_static_len_of(base);
                        if (!maybe_len.has_value()) {
                            report_lowering_error("loop lowering failed: sized array length metadata missing");
                            def->blocks.push_back(exit_bb);
                            cur_bb = exit_bb;
                            return emit_const_null(v.type);
                        }
                        len_v = emit_const_int(idx_ty, std::to_string(*maybe_len));
                    } else {
                        len_v = emit_array_len(base);
                    }

                    const BlockId cond_bb = new_block();
                    const BlockId body_bb = new_block();
                    const BlockId latch_bb = new_block();
                    const ValueId cond_idx = add_block_param(cond_bb, idx_ty);
                    const ValueId body_idx = add_block_param(body_bb, idx_ty);
                    const ValueId latch_idx = add_block_param(latch_bb, idx_ty);

                    if (!has_term()) br(cond_bb, {emit_const_int(idx_ty, "0")});

                    def->blocks.push_back(cond_bb);
                    cur_bb = cond_bb;
                    const ValueId cond = emit_binop(bool_type_(), Effect::Pure, BinOp::Lt, cond_idx, len_v);
                    condbr(cond, body_bb, {cond_idx}, exit_bb, expects_break_value ? std::vector<ValueId>{emit_const_null(v.type)} : std::vector<ValueId>{});

                    def->blocks.push_back(body_bb);
                    cur_bb = body_bb;
                    const size_t loop_scope_base = env_stack.size();
                    const ValueId elem = emit_index(binder_ty, base, body_idx);
                    loop_stack.push_back(LoopContext{
                        .break_bb = exit_bb,
                        .continue_bb = latch_bb,
                        .expects_break_value = expects_break_value,
                        .break_ty = (TypeId)v.type,
                        .continue_value = body_idx,
                        .scope_depth_base = loop_scope_base
                    });
                    push_scope();
                    bind(v.sym, Binding{false, false, elem, kInvalidId});
                    lower_block((parus::sir::BlockId)v.b);
                    pop_scope();
                    loop_stack.pop_back();
                    if (!has_term()) br(latch_bb, {body_idx});

                    def->blocks.push_back(latch_bb);
                    cur_bb = latch_bb;
                    const ValueId next_idx = emit_binop(idx_ty, Effect::Pure, BinOp::Add, latch_idx, emit_const_int(idx_ty, "1"));
                    if (!has_term()) br(cond_bb, {next_idx});
                } else {
                    const ValueId start_v = lower_value(v.a);
                    const ValueId end_v = lower_value(v.c);

                    const BlockId cond_bb = new_block();
                    const BlockId body_bb = new_block();
                    const BlockId latch_bb = new_block();
                    const BlockId step_bb =
                        (loop_kind == parus::LoopSourceKind::kRangeInclusive) ? new_block() : kInvalidId;
                    const ValueId cond_cur = add_block_param(cond_bb, binder_ty);
                    const ValueId body_cur = add_block_param(body_bb, binder_ty);
                    const ValueId latch_cur = add_block_param(latch_bb, binder_ty);
                    const ValueId step_cur =
                        (step_bb != kInvalidId) ? add_block_param(step_bb, binder_ty) : kInvalidId;

                    if (!has_term()) br(cond_bb, {start_v});

                    def->blocks.push_back(cond_bb);
                    cur_bb = cond_bb;
                    {
                        const ValueId cond =
                            emit_binop(
                                bool_type_(),
                                Effect::Pure,
                                loop_kind == parus::LoopSourceKind::kRangeInclusive ? BinOp::Le : BinOp::Lt,
                                cond_cur,
                                end_v
                            );
                        condbr(cond, body_bb, {cond_cur}, exit_bb, expects_break_value ? std::vector<ValueId>{emit_const_null(v.type)} : std::vector<ValueId>{});
                    }

                    def->blocks.push_back(body_bb);
                    cur_bb = body_bb;
                    const size_t loop_scope_base = env_stack.size();
                    loop_stack.push_back(LoopContext{
                        .break_bb = exit_bb,
                        .continue_bb = latch_bb,
                        .expects_break_value = expects_break_value,
                        .break_ty = (TypeId)v.type,
                        .continue_value = body_cur,
                        .scope_depth_base = loop_scope_base
                    });
                    push_scope();
                    bind(v.sym, Binding{false, false, body_cur, kInvalidId});
                    lower_block((parus::sir::BlockId)v.b);
                    pop_scope();
                    loop_stack.pop_back();
                    if (!has_term()) br(latch_bb, {body_cur});

                    def->blocks.push_back(latch_bb);
                    cur_bb = latch_bb;
                    if (loop_kind == parus::LoopSourceKind::kRangeInclusive) {
                        const ValueId at_end = emit_binop(bool_type_(), Effect::Pure, BinOp::Eq, latch_cur, end_v);
                        condbr(at_end, exit_bb, expects_break_value ? std::vector<ValueId>{emit_const_null(v.type)} : std::vector<ValueId>{},
                               step_bb, {latch_cur});

                        def->blocks.push_back(step_bb);
                        cur_bb = step_bb;
                    }
                    const ValueId next_v =
                        emit_binop(
                            binder_ty,
                            Effect::Pure,
                            BinOp::Add,
                            (step_cur != kInvalidId) ? step_cur : latch_cur,
                            emit_const_int(binder_ty, "1")
                        );
                    if (!has_term()) br(cond_bb, {next_v});
                }

                def->blocks.push_back(exit_bb);
                cur_bb = exit_bb;
                if (expects_break_value) return break_param;
                return emit_const_null(v.type);
            }

            case parus::sir::ValueKind::kError:
                report_lowering_error("value lowering failed: encountered SIR error value");
                return emit_const_null(v.type);

            default:
                return emit_const_null(v.type);
            }
        }

        // -----------------------
        // Lower statements/blocks
        // -----------------------
        void FuncBuild::lower_stmt(uint32_t stmt_index) {
            const auto& s = sir->stmts[stmt_index];

            switch (s.kind) {
            case parus::sir::StmtKind::kVarDecl: {
                TypeId declared = s.declared_type;
                auto materialize_var_decl_ = [&](ValueId init, bool consume_init_sir) {
                    init = coerce_value_for_target(declared, init);

                    const bool escape_alias_binding =
                        !s.is_static &&
                        s.is_set &&
                        !s.is_mut &&
                        declared != kInvalidId &&
                        types != nullptr &&
                        types->get(declared).kind == parus::ty::Kind::kEscape &&
                        init != kInvalidId &&
                        (size_t)init < out->values.size() &&
                        out->values[init].ty != kInvalidId &&
                        out->values[init].ty < types->count() &&
                        types->get(out->values[init].ty).kind == parus::ty::Kind::kEscape;

                    const auto existing = env.find(s.sym);
                    const bool reassign_existing = s.is_set && existing != env.end();
                    const bool needs_cleanup = type_needs_drop_(declared);

                    if (reassign_existing) {
                        ValueId slot = ensure_slot(s.sym, declared);
                        emit_store(slot, init);
                        if (consume_init_sir) {
                            if (s.init == parus::sir::k_invalid_value) {
                                mark_symbol_moved(s.sym, /*moved=*/true);
                            } else {
                                consume_owned_sir_value_(s.init, declared);
                            }
                        }
                        return;
                    }

                    if (escape_alias_binding) {
                        bind(s.sym, Binding{false, false, init, kInvalidId});
                        return;
                    }

                    if (needs_cleanup) {
                        ValueId slot = emit_alloca(declared);
                        emit_store(slot, init);
                        const uint32_t cleanup_id = register_cleanup(s.sym, slot, declared);
                        bind(s.sym, Binding{true, false, slot, cleanup_id});
                        if (should_remember_home_slot(declared)) {
                            remember_home_slot(s.sym, slot);
                        }
                        if (consume_init_sir) {
                            if (s.init == parus::sir::k_invalid_value) {
                                mark_symbol_moved(s.sym, /*moved=*/true);
                            } else {
                                consume_owned_sir_value_(s.init, declared);
                            }
                        }
                        return;
                    }

                    if (s.is_set || s.is_mut) {
                        ValueId slot = emit_alloca(declared);
                        emit_store(slot, init);
                        bind(s.sym, Binding{true, false, slot, kInvalidId});
                        if (should_remember_home_slot(declared)) {
                            remember_home_slot(s.sym, slot);
                        }
                    } else {
                        bind(s.sym, Binding{false, false, init, kInvalidId});
                    }

                    if (consume_init_sir) {
                        consume_owned_sir_value_(s.init, declared);
                    }
                };

                if (s.has_consume_else &&
                    s.init != parus::sir::k_invalid_value &&
                    (size_t)s.init < sir->values.size()) {
                    const auto& place = sir->values[s.init];
                    TypeId opt_ty = place.type;
                    TypeId payload_ty = declared;
                    if (types != nullptr &&
                        opt_ty != kInvalidId &&
                        opt_ty < types->count()) {
                        const auto& tt = types->get(opt_ty);
                        if (tt.kind == parus::ty::Kind::kOptional && tt.elem != kInvalidId) {
                            payload_ty = tt.elem;
                        }
                    }

                    ValueId place_slot = kInvalidId;
                    ValueId current_opt = emit_const_null(opt_ty);
                    place_slot = lower_place(s.init, &opt_ty);
                    if (place_slot != kInvalidId &&
                        (size_t)place_slot < out->values.size() &&
                        opt_ty == kInvalidId) {
                        opt_ty = out->values[place_slot].ty;
                    }
                    current_opt = emit_load(opt_ty, place_slot);

                    const ValueId is_null =
                        emit_binop(bool_type_(), Effect::Pure, BinOp::Eq, current_opt, emit_const_null(opt_ty));
                    const BlockId fail_bb = new_block();
                    const BlockId succ_bb = new_block();
                    condbr(is_null, fail_bb, {}, succ_bb, {});

                    def->blocks.push_back(fail_bb);
                    cur_bb = fail_bb;
                    push_scope();
                    if (s.b != parus::sir::k_invalid_block) lower_block(s.b);
                    pop_scope();
                    if (!has_term()) {
                        report_lowering_error("consume-binding else block must diverge");
                        br(succ_bb, {});
                    }

                    def->blocks.push_back(succ_bb);
                    cur_bb = succ_bb;
                    const ValueId payload =
                        emit_cast(payload_ty, Effect::MayTrap, CastKind::AsB, payload_ty, current_opt);
                    emit_store(place_slot, emit_const_null(opt_ty));
                    if (place.kind == parus::sir::ValueKind::kLocal &&
                        place.sym != parus::sir::k_invalid_symbol) {
                        mark_symbol_moved(place.sym, /*moved=*/false);
                    }
                    materialize_var_decl_(payload, /*consume_init_sir=*/false);
                    return;
                }

                ValueId init = (s.init != parus::sir::k_invalid_value) ? lower_value(s.init)
                                                                       : emit_const_null(declared);
                materialize_var_decl_(init, /*consume_init_sir=*/true);
                return;
            }

            case parus::sir::StmtKind::kThrowStmt: {
                const ValueId payload = (s.expr != parus::sir::k_invalid_value)
                    ? lower_value(s.expr)
                    : kInvalidId;
                TypeId payload_ty = kInvalidId;
                bool dynamic_rethrow = false;
                if (s.expr != parus::sir::k_invalid_value &&
                    (size_t)s.expr < sir->values.size()) {
                    const auto& sv = sir->values[s.expr];
                    if (sv.kind == parus::sir::ValueKind::kLocal &&
                        sv.sym != parus::sir::k_invalid_symbol &&
                        untyped_catch_binder_symbols.find(sv.sym) != untyped_catch_binder_symbols.end()) {
                        dynamic_rethrow = true;
                    }
                }
                if (!dynamic_rethrow &&
                    payload != kInvalidId &&
                    (size_t)payload < out->values.size()) {
                    payload_ty = out->values[payload].ty;
                }

                if (dynamic_rethrow) {
                    emit_set_exc_type_from_value_(payload);
                } else {
                    emit_store_exc_payload_(payload_ty, payload);
                    emit_set_exc_type_(payload_ty);
                }
                emit_set_exc_active_(true);

                if (!try_stack.empty()) {
                    const auto& tc = try_stack.back();
                    emit_cleanups_to_depth(tc.scope_depth_base);
                    br(tc.dispatch_bb, {});
                    return;
                }

                emit_propagate_throw_(/*cleanup_keep_depth=*/0);
                return;
            }

            case parus::sir::StmtKind::kTryCatchStmt: {
                const BlockId dispatch_bb = new_block();
                const BlockId after_bb = new_block();
                const size_t scope_base = env_stack.size();

                TryContext tc{};
                tc.dispatch_bb = dispatch_bb;
                tc.after_bb = after_bb;
                tc.scope_depth_base = scope_base;
                tc.clause_begin = s.catch_clause_begin;
                tc.clause_count = s.catch_clause_count;

                try_stack.push_back(tc);
                push_scope();
                lower_block_with_try_guard(s.a, tc);
                pop_scope();
                try_stack.pop_back();

                if (!has_term()) {
                    br(after_bb, {});
                }

                def->blocks.push_back(dispatch_bb);
                cur_bb = dispatch_bb;

                BlockId no_throw_or_next = after_bb;
                if (s.catch_clause_count > 0) {
                    no_throw_or_next = new_block();
                }

                const ValueId active = emit_load_exc_active_();
                condbr(active, no_throw_or_next, {}, after_bb, {});

                if (s.catch_clause_count > 0) {
                    def->blocks.push_back(no_throw_or_next);
                    cur_bb = no_throw_or_next;
                }

                const uint64_t cb = s.catch_clause_begin;
                const uint64_t ce = cb + s.catch_clause_count;
                if (cb <= sir->try_catch_clauses.size() && ce <= sir->try_catch_clauses.size()) {
                    for (uint32_t i = 0; i < s.catch_clause_count; ++i) {
                        const auto& cc = sir->try_catch_clauses[s.catch_clause_begin + i];
                        const BlockId catch_bb = new_block();
                        BlockId next_bb = kInvalidId;

                        if (cc.has_typed_bind) {
                            next_bb = new_block();
                            const ValueId throw_ty = emit_load_exc_type_();
                            const ValueId want_ty = emit_const_int(i64_type_(), std::to_string((uint32_t)cc.bind_type));
                            const ValueId cond = emit_binop(bool_type_(), Effect::Pure, BinOp::Eq, throw_ty, want_ty);
                            condbr(cond, catch_bb, {}, next_bb, {});
                        } else {
                            br(catch_bb, {});
                        }

                        def->blocks.push_back(catch_bb);
                        cur_bb = catch_bb;
                        const ValueId thrown_type_for_bind = emit_load_exc_type_();
                        ValueId typed_payload_for_bind = kInvalidId;
                        if (cc.has_typed_bind && cc.bind_type != kInvalidId) {
                            typed_payload_for_bind = emit_load_exc_payload_(cc.bind_type);
                        }
                        emit_set_exc_active_(false);
                        emit_set_exc_type_(kInvalidId);

                        push_scope();
                        if (cc.bind_sym != parus::sir::k_invalid_symbol) {
                            ValueId bind_v = kInvalidId;
                            if (cc.has_typed_bind && cc.bind_type != kInvalidId) {
                                bind_v = typed_payload_for_bind;
                            } else {
                                bind_v = thrown_type_for_bind;
                                untyped_catch_binder_symbols.insert(cc.bind_sym);
                            }
                            bind(cc.bind_sym, Binding{false, false, bind_v, kInvalidId});
                        }
                        lower_block(cc.body);
                        pop_scope();
                        if (!has_term()) {
                            br(after_bb, {});
                        }

                        if (next_bb != kInvalidId) {
                            def->blocks.push_back(next_bb);
                            cur_bb = next_bb;
                        } else {
                            break;
                        }
                    }
                }

                if (!has_term()) {
                    if (!try_stack.empty()) {
                        const auto& outer = try_stack.back();
                        const ValueId throw_ty = emit_load_exc_type_();
                        emit_set_exc_active_(true);
                        const ValueId outer_type_ptr = emit_exc_type_ptr_();
                        if (outer_type_ptr != kInvalidId) {
                            emit_store(outer_type_ptr, throw_ty);
                        }
                        emit_cleanups_to_depth(outer.scope_depth_base);
                        br(outer.dispatch_bb, {});
                    } else {
                        emit_propagate_throw_(/*cleanup_keep_depth=*/0);
                    }
                }

                def->blocks.push_back(after_bb);
                cur_bb = after_bb;
                return;
            }

            case parus::sir::StmtKind::kExprStmt:
                if (s.expr != parus::sir::k_invalid_value) (void)lower_value(s.expr);
                return;

            case parus::sir::StmtKind::kCommitStmt:
                emit_actor_commit_marker();
                return;

            case parus::sir::StmtKind::kRecastStmt:
                emit_actor_recast_marker();
                return;

            case parus::sir::StmtKind::kReturn:
                if (s.expr != parus::sir::k_invalid_value) {
                    ValueId rv = lower_value(s.expr);
                    if (def != nullptr && def->ret_ty != kInvalidId) {
                        rv = coerce_value_for_target(def->ret_ty, rv);
                        consume_owned_sir_value_(s.expr, def->ret_ty);
                    }
                    if (fn_is_throwing && has_exception_globals_()) {
                        const ValueId active = emit_load_exc_active_();
                        const BlockId throw_bb = new_block();
                        const BlockId ret_bb = new_block();
                        condbr(active, throw_bb, {}, ret_bb, {});

                        def->blocks.push_back(throw_bb);
                        cur_bb = throw_bb;
                        if (!try_stack.empty()) {
                            const auto& tc = try_stack.back();
                            emit_cleanups_to_depth(tc.scope_depth_base);
                            br(tc.dispatch_bb, {});
                        } else {
                            emit_propagate_throw_(/*cleanup_keep_depth=*/0);
                        }

                        def->blocks.push_back(ret_bb);
                        cur_bb = ret_bb;
                        emit_cleanups_to_depth(/*keep_depth=*/0);
                        ret(rv);
                    } else {
                        emit_cleanups_to_depth(/*keep_depth=*/0);
                        ret(rv);
                    }
                } else {
                    if (fn_is_throwing && has_exception_globals_()) {
                        const ValueId active = emit_load_exc_active_();
                        const BlockId throw_bb = new_block();
                        const BlockId ret_bb = new_block();
                        condbr(active, throw_bb, {}, ret_bb, {});

                        def->blocks.push_back(throw_bb);
                        cur_bb = throw_bb;
                        if (!try_stack.empty()) {
                            const auto& tc = try_stack.back();
                            emit_cleanups_to_depth(tc.scope_depth_base);
                            br(tc.dispatch_bb, {});
                        } else {
                            emit_propagate_throw_(/*cleanup_keep_depth=*/0);
                        }

                        def->blocks.push_back(ret_bb);
                        cur_bb = ret_bb;
                        emit_cleanups_to_depth(/*keep_depth=*/0);
                        ret_void();
                    } else {
                        emit_cleanups_to_depth(/*keep_depth=*/0);
                        ret_void();
                    }
                }
                return;

            case parus::sir::StmtKind::kWhileStmt: {
                // SIR: s.expr = cond, s.a = body block
                BlockId cond_bb = new_block();
                BlockId body_bb = new_block();
                BlockId exit_bb = new_block();

                // jump to cond
                if (!has_term()) br(cond_bb, {});

                // cond block
                def->blocks.push_back(cond_bb);
                cur_bb = cond_bb;
                ValueId cond = lower_value(s.expr);
                condbr(cond, body_bb, {}, exit_bb, {});

                // body
                def->blocks.push_back(body_bb);
                cur_bb = body_bb;
                const size_t loop_scope_base = env_stack.size();
                loop_stack.push_back(LoopContext{
                    .break_bb = exit_bb,
                    .continue_bb = cond_bb,
                    .expects_break_value = false,
                    .break_ty = kInvalidId,
                    .scope_depth_base = loop_scope_base
                });
                push_scope();
                lower_block(s.a);
                pop_scope();
                loop_stack.pop_back();
                if (!has_term()) br(cond_bb, {});

                // exit
                def->blocks.push_back(exit_bb);
                cur_bb = exit_bb;
                return;
            }

            case parus::sir::StmtKind::kDoScopeStmt: {
                // do { ... } : body를 1회 실행하는 명시 스코프
                push_scope();
                lower_block(s.a);
                pop_scope();
                return;
            }

            case parus::sir::StmtKind::kDoWhileStmt: {
                // do-while: body를 먼저 실행하고 조건을 검사한다.
                BlockId body_bb = new_block();
                BlockId cond_bb = new_block();
                BlockId exit_bb = new_block();

                if (!has_term()) br(body_bb, {});

                // body
                def->blocks.push_back(body_bb);
                cur_bb = body_bb;
                const size_t loop_scope_base = env_stack.size();
                loop_stack.push_back(LoopContext{
                    .break_bb = exit_bb,
                    .continue_bb = cond_bb,
                    .expects_break_value = false,
                    .break_ty = kInvalidId,
                    .scope_depth_base = loop_scope_base
                });
                push_scope();
                lower_block(s.a);
                pop_scope();
                loop_stack.pop_back();
                if (!has_term()) br(cond_bb, {});

                // cond
                def->blocks.push_back(cond_bb);
                cur_bb = cond_bb;
                ValueId cond = lower_value(s.expr);
                condbr(cond, body_bb, {}, exit_bb, {});

                // exit
                def->blocks.push_back(exit_bb);
                cur_bb = exit_bb;
                return;
            }

            case parus::sir::StmtKind::kManualStmt: {
                // manual 블록은 현재 단계에서 별도 runtime 동작 없이 body만 순차 lowering한다.
                push_scope();
                lower_block(s.a);
                pop_scope();
                return;
            }

            case parus::sir::StmtKind::kIfStmt: {
                // v0: stmt-level if (not expression). SIR: s.expr=cond, s.a=then block, s.b=else block
                BlockId then_bb = new_block();
                BlockId else_bb = new_block();
                BlockId join_bb = new_block();

                ValueId cond = lower_value(s.expr);
                condbr(cond, then_bb, {}, else_bb, {});

                // then
                def->blocks.push_back(then_bb);
                cur_bb = then_bb;
                push_scope();
                lower_block(s.a);
                pop_scope();
                if (!has_term()) br(join_bb, {});

                // else
                def->blocks.push_back(else_bb);
                cur_bb = else_bb;
                push_scope();
                if (s.b != parus::sir::k_invalid_block) lower_block(s.b);
                pop_scope();
                if (!has_term()) br(join_bb, {});

                // join
                def->blocks.push_back(join_bb);
                cur_bb = join_bb;
                return;
            }

            case parus::sir::StmtKind::kSwitch: {
                const ValueId scrut = lower_value(s.expr);
                const TypeId scrut_ty =
                    (scrut != kInvalidId && (size_t)scrut < out->values.size())
                        ? out->values[scrut].ty
                        : kInvalidId;
                const TypeId bool_ty =
                    (types != nullptr)
                        ? (TypeId)types->builtin(parus::ty::Builtin::kBool)
                        : kInvalidId;

                const auto emit_case_match_cond = [&](const parus::sir::SwitchCase& c) -> ValueId {
                    switch (c.pat_kind) {
                        case parus::sir::SwitchCasePatKind::kInt: {
                            const ValueId pat = emit_const_int(scrut_ty, parse_int_literal_text_(c.pat_text));
                            return emit_binop(bool_ty, Effect::Pure, BinOp::Eq, scrut, pat);
                        }
                        case parus::sir::SwitchCasePatKind::kBool: {
                            const bool bv = (c.pat_text == "true");
                            const ValueId pat = emit_const_bool(scrut_ty, bv);
                            return emit_binop(bool_ty, Effect::Pure, BinOp::Eq, scrut, pat);
                        }
                        case parus::sir::SwitchCasePatKind::kNull: {
                            const ValueId pat = emit_const_null(scrut_ty);
                            return emit_binop(bool_ty, Effect::Pure, BinOp::Eq, scrut, pat);
                        }
                        case parus::sir::SwitchCasePatKind::kChar: {
                            const auto code = parse_char_literal_code_(c.pat_text);
                            const ValueId pat = emit_const_int(
                                scrut_ty,
                                code.has_value() ? std::to_string(*code) : std::string("0")
                            );
                            return emit_binop(bool_ty, Effect::Pure, BinOp::Eq, scrut, pat);
                        }
                        case parus::sir::SwitchCasePatKind::kString:
                        case parus::sir::SwitchCasePatKind::kIdent:
                        case parus::sir::SwitchCasePatKind::kEnumVariant:
                        case parus::sir::SwitchCasePatKind::kError:
                        default:
                            return emit_const_bool(bool_ty, false);
                    }
                };

                const auto emit_enum_tag_match_cond = [&](ValueId enum_value, const parus::sir::SwitchCase& c) -> ValueId {
                    TypeId tag_ty =
                        (types != nullptr)
                            ? (TypeId)types->builtin(parus::ty::Builtin::kI32)
                            : kInvalidId;
                    if (const auto* layout = find_field_layout_(c.enum_type)) {
                        for (const auto& m : layout->members) {
                            if (m.name == "__tag") {
                                tag_ty = m.type;
                                break;
                            }
                        }
                    }
                    ValueId tag_place = emit_field(tag_ty, enum_value, "__tag");
                    ValueId tag_value = emit_load(tag_ty, tag_place);
                    ValueId pat = emit_const_int(tag_ty, std::to_string(c.enum_tag_value));
                    return emit_binop(bool_ty, Effect::Pure, BinOp::Eq, tag_value, pat);
                };

                const auto bind_enum_case_payload = [&](ValueId enum_value, const parus::sir::SwitchCase& c) {
                    const uint64_t bb = c.enum_bind_begin;
                    const uint64_t be = bb + c.enum_bind_count;
                    if (bb > sir->switch_enum_binds.size() || be > sir->switch_enum_binds.size()) return;
                    for (uint32_t bi = c.enum_bind_begin; bi < c.enum_bind_begin + c.enum_bind_count; ++bi) {
                        const auto& b = sir->switch_enum_binds[bi];
                        if (b.bind_sym == parus::sir::k_invalid_symbol || b.bind_type == parus::sir::k_invalid_type) {
                            continue;
                        }
                        ValueId place = emit_field((TypeId)b.bind_type, enum_value, std::string(b.storage_name));
                        ValueId bound = emit_load((TypeId)b.bind_type, place);
                        bind(b.bind_sym, Binding{false, false, bound, kInvalidId});
                    }
                };

                const BlockId exit_bb = new_block();
                std::optional<parus::sir::SwitchCase> default_case;

                if ((uint64_t)s.case_begin + (uint64_t)s.case_count <= (uint64_t)sir->switch_cases.size()) {
                    for (uint32_t i = 0; i < s.case_count; ++i) {
                        const auto& c = sir->switch_cases[s.case_begin + i];
                        if (c.is_default) {
                            default_case = c;
                            continue;
                        }

                        const BlockId match_bb = new_block();
                        const BlockId next_bb = new_block();
                        if (c.pat_kind == parus::sir::SwitchCasePatKind::kEnumVariant &&
                            c.enum_type != kInvalidId) {
                            bool scrut_is_optional_enum = false;
                            if (types != nullptr && scrut_ty != kInvalidId) {
                                const auto& st = types->get(scrut_ty);
                                scrut_is_optional_enum =
                                    (st.kind == parus::ty::Kind::kOptional && st.elem == c.enum_type);
                            }

                            if (scrut_ty == c.enum_type) {
                                const ValueId cond = emit_enum_tag_match_cond(scrut, c);
                                condbr(cond, match_bb, {}, next_bb, {});
                            } else if (scrut_is_optional_enum) {
                                const BlockId tag_check_bb = new_block();
                                const ValueId null_pat = emit_const_null(scrut_ty);
                                const ValueId non_null = emit_binop(bool_ty, Effect::Pure, BinOp::Ne, scrut, null_pat);
                                condbr(non_null, tag_check_bb, {}, next_bb, {});

                                def->blocks.push_back(tag_check_bb);
                                cur_bb = tag_check_bb;
                                const ValueId narrowed = emit_cast(c.enum_type, Effect::MayTrap, CastKind::AsB, c.enum_type, scrut);
                                const ValueId cond = emit_enum_tag_match_cond(narrowed, c);
                                condbr(cond, match_bb, {}, next_bb, {});
                            } else {
                                const ValueId cond = emit_const_bool(bool_ty, false);
                                condbr(cond, match_bb, {}, next_bb, {});
                            }
                        } else {
                            const ValueId cond = emit_case_match_cond(c);
                            condbr(cond, match_bb, {}, next_bb, {});
                        }

                        def->blocks.push_back(match_bb);
                        cur_bb = match_bb;
                        push_scope();
                        if (c.pat_kind == parus::sir::SwitchCasePatKind::kEnumVariant &&
                            c.enum_type != kInvalidId) {
                            ValueId enum_value = scrut;
                            if (types != nullptr && scrut_ty != kInvalidId) {
                                const auto& st = types->get(scrut_ty);
                                if (st.kind == parus::ty::Kind::kOptional && st.elem == c.enum_type) {
                                    enum_value = emit_cast(c.enum_type, Effect::MayTrap, CastKind::AsB, c.enum_type, scrut);
                                }
                            }
                            bind_enum_case_payload(enum_value, c);
                        }
                        lower_block(c.body);
                        pop_scope();
                        if (!has_term()) br(exit_bb, {});

                        def->blocks.push_back(next_bb);
                        cur_bb = next_bb;
                    }
                }

                if (default_case.has_value()) {
                    const BlockId def_bb = new_block();
                    if (!has_term()) br(def_bb, {});

                    def->blocks.push_back(def_bb);
                    cur_bb = def_bb;
                    push_scope();
                    lower_block(default_case->body);
                    pop_scope();
                    if (!has_term()) br(exit_bb, {});
                } else {
                    if (!has_term()) br(exit_bb, {});
                }

                def->blocks.push_back(exit_bb);
                cur_bb = exit_bb;
                return;
            }

            case parus::sir::StmtKind::kBreak: {
                if (loop_stack.empty()) return;
                const auto& lc = loop_stack.back();

                if (lc.expects_break_value) {
                    ValueId bv = (s.expr != parus::sir::k_invalid_value)
                               ? lower_value(s.expr)
                               : emit_const_null(lc.break_ty);
                    bv = coerce_value_for_target(lc.break_ty, bv);
                    emit_cleanups_to_depth(lc.scope_depth_base);
                    br(lc.break_bb, {bv});
                } else {
                    emit_cleanups_to_depth(lc.scope_depth_base);
                    br(lc.break_bb, {});
                }
                return;
            }

            case parus::sir::StmtKind::kContinue: {
                if (loop_stack.empty()) return;
                const auto& lc = loop_stack.back();
                emit_cleanups_to_depth(lc.scope_depth_base);
                if (lc.continue_value != kInvalidId) {
                    br(lc.continue_bb, {lc.continue_value});
                } else {
                    br(lc.continue_bb, {});
                }
                return;
            }

            default:
                return;
            }
        }

        void FuncBuild::lower_block(parus::sir::BlockId bid) {
            if (bid == parus::sir::k_invalid_block) return;

            const auto& b = sir->blocks[bid];
            for (uint32_t i = 0; i < b.stmt_count; i++) {
                uint32_t si = b.stmt_begin + i;
                if (has_term()) break;
                lower_stmt(si);
                if (has_term()) break;
                emit_stmt_boundary_throw_check_();
            }
        }

        void FuncBuild::lower_block_with_try_guard(parus::sir::BlockId bid, const TryContext& tc) {
            if (bid == parus::sir::k_invalid_block) return;

            const auto& b = sir->blocks[bid];
            for (uint32_t i = 0; i < b.stmt_count; i++) {
                const uint32_t si = b.stmt_begin + i;
                if (has_term()) break;
                lower_stmt(si);
                if (has_term()) break;

                if (!has_exception_globals_()) continue;
                const ValueId active = emit_load_exc_active_();
                const BlockId cleanup_bb = new_block();
                const BlockId cont_bb = new_block();
                condbr(active, cleanup_bb, {}, cont_bb, {});

                def->blocks.push_back(cleanup_bb);
                cur_bb = cleanup_bb;
                emit_cleanups_to_depth(tc.scope_depth_base);
                br(tc.dispatch_bb, {});

                def->blocks.push_back(cont_bb);
                cur_bb = cont_bb;
            }
        }

        /// @brief SIR escape kind를 OIR 힌트 kind로 변환한다.
        EscapeHandleKind map_escape_kind_(parus::sir::EscapeHandleKind k) {
            using SK = parus::sir::EscapeHandleKind;
            switch (k) {
                case SK::kTrivial:    return EscapeHandleKind::Trivial;
                case SK::kStackSlot:  return EscapeHandleKind::StackSlot;
                case SK::kCallerSlot: return EscapeHandleKind::CallerSlot;
                case SK::kHeapBox:    return EscapeHandleKind::HeapBox;
            }
            return EscapeHandleKind::Trivial;
        }

        /// @brief SIR escape boundary를 OIR 힌트 boundary로 변환한다.
        EscapeBoundaryKind map_escape_boundary_(parus::sir::EscapeBoundaryKind k) {
            using SB = parus::sir::EscapeBoundaryKind;
            switch (k) {
                case SB::kNone:   return EscapeBoundaryKind::None;
                case SB::kReturn: return EscapeBoundaryKind::Return;
                case SB::kCallArg:return EscapeBoundaryKind::CallArg;
                case SB::kAbi:    return EscapeBoundaryKind::Abi;
            }
            return EscapeBoundaryKind::None;
        }

    } // namespace

    // ------------------------------------------------------------
    // Builder::build
    // ------------------------------------------------------------
    BuildResult Builder::build() {
        BuildResult out{};
        out.mod.bundle_enabled = sir_.bundle_enabled;
        out.mod.bundle_name = sir_.bundle_name;
        out.mod.current_source_norm = sir_.current_source_norm;
        out.mod.bundle_sources_norm = sir_.bundle_sources_norm;
        out.mod.actor_types = sir_.actor_types;

        // OIR 진입 게이트:
        // - handle 비물질화(materialize_count==0)
        // - static/boundary 규칙
        // - escape 메타 일관성
        // 위 규칙을 만족하지 않으면 OIR lowering 자체를 중단한다.
        out.gate_errors = parus::sir::verify_escape_handles(sir_);
        if (!out.gate_errors.empty()) {
            out.gate_passed = false;
            return out;
        }

        std::unordered_map<parus::ty::TypeId, std::pair<uint32_t, uint32_t>> named_layout_by_type;
        std::unordered_set<TypeId> actor_type_set(sir_.actor_types.begin(), sir_.actor_types.end());
        auto type_size_align = [&](const auto& self, parus::ty::TypeId tid) -> std::pair<uint32_t, uint32_t> {
            using TK = parus::ty::Kind;
            using TB = parus::ty::Builtin;

            if (tid == parus::ty::kInvalidType) return {8u, 8u};
            const auto& t = ty_.get(tid);

            switch (t.kind) {
                case TK::kError:
                    return {8u, 8u};

                case TK::kBuiltin:
                    switch (t.builtin) {
                        case TB::kBool:
                        case TB::kI8:
                        case TB::kU8:
                        case TB::kCChar:
                        case TB::kCSChar:
                        case TB::kCUChar:
                            return {1u, 1u};
                        case TB::kI16:
                        case TB::kU16:
                        case TB::kCShort:
                        case TB::kCUShort:
                            return {2u, 2u};
                        case TB::kI32:
                        case TB::kU32:
                        case TB::kF32:
                        case TB::kChar:
                        case TB::kCInt:
                        case TB::kCUInt:
                        case TB::kCFloat:
                            return {4u, 4u};
                        case TB::kText:
                            return {16u, 8u};
                        case TB::kI128:
                        case TB::kU128:
                        case TB::kF128:
                            return {16u, 16u};
                        case TB::kUnit:
                        case TB::kCVoid:
                            return {1u, 1u};
                        case TB::kCLong:
                        case TB::kCULong:
                            return {static_cast<uint32_t>(sizeof(long)), static_cast<uint32_t>(alignof(long))};
                        case TB::kCLongLong:
                        case TB::kCULongLong:
                        case TB::kCDouble:
                            return {8u, 8u};
                        case TB::kCSize:
                        case TB::kCSSize:
                        case TB::kCPtrDiff:
                        case TB::kVaList:
                            return {8u, 8u};
                        case TB::kNever:
                        case TB::kI64:
                        case TB::kU64:
                        case TB::kF64:
                        case TB::kISize:
                        case TB::kUSize:
                        case TB::kNull:
                        case TB::kInferInteger:
                            return {8u, 8u};
                    }
                    return {8u, 8u};

                case TK::kPtr:
                case TK::kBorrow:
                case TK::kEscape:
                case TK::kFn:
                    return {8u, 8u};

                case TK::kOptional: {
                    const auto [elem_size, elem_align] = self(self, t.elem);
                    const uint32_t a = std::max<uint32_t>(1u, elem_align);
                    const uint32_t body = align_to_(1u, a) + std::max<uint32_t>(1u, elem_size);
                    return {body, a};
                }

                case TK::kArray: {
                    const auto [elem_size, elem_align] = self(self, t.elem);
                    const uint32_t e = std::max<uint32_t>(1u, elem_size);
                    const uint32_t a = std::max<uint32_t>(1u, elem_align);
                    if (!t.array_has_size) return {16u, 8u};
                    return {e * std::max<uint32_t>(1u, t.array_size), a};
                }

                case TK::kNamedUser: {
                    if (actor_type_set.find(tid) != actor_type_set.end()) {
                        return {8u, 8u};
                    }
                    auto it = named_layout_by_type.find(tid);
                    if (it != named_layout_by_type.end()) return it->second;
                    return {8u, 8u};
                }
            }

            return {8u, 8u};
        };

        // 필드 레이아웃 메타를 OIR 모듈로 복사한다.
        for (const auto& sf : sir_.fields) {
            FieldLayoutDecl of{};
            of.name = std::string(sf.name);
            of.self_type = sf.self_type;
            of.layout = map_field_layout_(sf.layout);
            of.align = sf.align;

            const uint64_t begin = sf.member_begin;
            const uint64_t end = begin + sf.member_count;
            uint32_t offset = 0;
            uint32_t struct_align = std::max<uint32_t>(1u, of.align);

            if (begin <= sir_.field_members.size() && end <= sir_.field_members.size()) {
                for (uint32_t i = sf.member_begin; i < sf.member_begin + sf.member_count; ++i) {
                    const auto& sm = sir_.field_members[i];
                    const auto [member_size_raw, member_align_raw] = type_size_align(type_size_align, sm.type);
                    const uint32_t member_size = std::max<uint32_t>(1u, member_size_raw);
                    const uint32_t member_align = std::max<uint32_t>(1u, member_align_raw);

                    if (of.layout == FieldLayout::C) {
                        offset = align_to_(offset, member_align);
                    }

                    FieldMemberLayout om{};
                    om.name = std::string(sm.name);
                    om.type = sm.type;
                    om.offset = offset;
                    of.members.push_back(std::move(om));

                    offset += member_size;
                    struct_align = std::max(struct_align, member_align);
                }
            }

            if (of.layout == FieldLayout::C) {
                of.size = std::max<uint32_t>(1u, align_to_(offset, struct_align));
            } else {
                of.size = std::max<uint32_t>(1u, offset);
            }
            if (of.align == 0) of.align = struct_align;

            const uint32_t idx = out.mod.add_field(of);
            (void)idx;
            if (of.self_type != parus::ty::kInvalidType) {
                named_layout_by_type[of.self_type] = {std::max<uint32_t>(1u, of.size), std::max<uint32_t>(1u, of.align)};
            }
        }

        auto ensure_external_parus_field_layout = [&](auto&& self, parus::ty::TypeId tid) -> bool {
            if (sym_ == nullptr || tid == parus::ty::kInvalidType) return false;
            if (named_layout_by_type.find(tid) != named_layout_by_type.end()) return true;
            if (actor_type_set.find(tid) != actor_type_set.end()) return false;

            const auto& tt = ty_.get(tid);
            if (tt.kind != parus::ty::Kind::kNamedUser) return false;

            std::vector<std::string_view> path{};
            std::vector<parus::ty::TypeId> args{};
            if (!ty_.decompose_named_user(tid, path, args) || path.empty()) return false;

            std::string base_name{};
            for (size_t i = 0; i < path.size(); ++i) {
                if (i != 0) base_name += "::";
                base_name += std::string(path[i]);
            }

            auto type_base_name_ = [&](parus::ty::TypeId sid) -> std::string {
                if (sid == parus::ty::kInvalidType) return {};
                std::vector<std::string_view> spath{};
                std::vector<parus::ty::TypeId> sargs{};
                if (!ty_.decompose_named_user(sid, spath, sargs) || spath.empty()) return {};
                std::string joined{};
                for (size_t i = 0; i < spath.size(); ++i) {
                    if (i != 0) joined += "::";
                    joined += std::string(spath[i]);
                }
                return joined;
            };

            auto same_base_name_ = [&](std::string_view lhs, std::string_view rhs) -> bool {
                if (lhs.empty() || rhs.empty()) return false;
                if (lhs == rhs) return true;
                if (lhs.starts_with("core::") && lhs.substr(std::string_view("core::").size()) == rhs) return true;
                if (rhs.starts_with("core::") && rhs.substr(std::string_view("core::").size()) == lhs) return true;
                const size_t lsplit = lhs.rfind("::");
                const size_t rsplit = rhs.rfind("::");
                const std::string_view lleaf =
                    (lsplit == std::string_view::npos) ? lhs : lhs.substr(lsplit + 2);
                const std::string_view rleaf =
                    (rsplit == std::string_view::npos) ? rhs : rhs.substr(rsplit + 2);
                return lleaf == rleaf;
            };

            auto lookup_external_layout_symbol_ = [&](std::string_view qname)
                -> const parus::sema::Symbol* {
                for (const auto& ss : sym_->symbols()) {
                    if (!ss.is_external) {
                        continue;
                    }
                    const bool has_field_payload =
                        (ss.kind == parus::sema::SymbolKind::kField && !ss.external_payload.empty()) ||
                        (ss.kind == parus::sema::SymbolKind::kType && !ss.external_field_payload.empty());
                    if (!has_field_payload) {
                        continue;
                    }
                    if (ss.name == qname) {
                        return &ss;
                    }
                    if (same_base_name_(type_base_name_(ss.declared_type), qname)) {
                        return &ss;
                    }
                }
                return nullptr;
            };

            const parus::sema::Symbol* layout_sym = lookup_external_layout_symbol_(base_name);
            if (layout_sym == nullptr) {
                const size_t split = base_name.find("::");
                if (split != std::string::npos && split + 2 < base_name.size()) {
                    layout_sym = lookup_external_layout_symbol_(base_name.substr(split + 2));
                }
            }
            if (layout_sym == nullptr) {
                const size_t split = base_name.rfind("::");
                if (split != std::string::npos && split + 2 < base_name.size()) {
                    layout_sym = lookup_external_layout_symbol_(base_name.substr(split + 2));
                }
            }
            if (layout_sym == nullptr) return false;
            const auto& ss = *layout_sym;
            const std::string_view field_payload =
                (!ss.external_field_payload.empty() ? std::string_view(ss.external_field_payload)
                                                   : std::string_view(ss.external_payload));

            const auto parsed = parse_external_parus_field_decl_payload_(field_payload, ty_);
            if (!parsed.ok) return false;
            if (args.size() != parsed.generic_params.size()) {
                return false;
            }

            std::unordered_map<std::string, parus::ty::TypeId> subst{};
            subst.reserve(parsed.generic_params.size());
            for (size_t i = 0; i < parsed.generic_params.size(); ++i) {
                subst.emplace(parsed.generic_params[i], args[i]);
            }

            auto resolve_size_align = [&](auto&& resolve, parus::ty::TypeId cur) -> std::pair<uint32_t, uint32_t> {
                using TK = parus::ty::Kind;
                using TB = parus::ty::Builtin;

                if (cur == parus::ty::kInvalidType) return {8u, 8u};
                const auto& ct = ty_.get(cur);
                switch (ct.kind) {
                    case TK::kError:
                        return {8u, 8u};
                    case TK::kBuiltin:
                        switch (ct.builtin) {
                            case TB::kBool:
                            case TB::kI8:
                            case TB::kU8:
                            case TB::kCChar:
                            case TB::kCSChar:
                            case TB::kCUChar:
                                return {1u, 1u};
                            case TB::kI16:
                            case TB::kU16:
                            case TB::kCShort:
                            case TB::kCUShort:
                                return {2u, 2u};
                            case TB::kI32:
                            case TB::kU32:
                            case TB::kF32:
                            case TB::kChar:
                            case TB::kCInt:
                            case TB::kCUInt:
                            case TB::kCFloat:
                                return {4u, 4u};
                            case TB::kText:
                                return {16u, 8u};
                            case TB::kI128:
                            case TB::kU128:
                            case TB::kF128:
                                return {16u, 16u};
                            case TB::kUnit:
                            case TB::kCVoid:
                                return {1u, 1u};
                            case TB::kCLong:
                            case TB::kCULong:
                                return {static_cast<uint32_t>(sizeof(long)), static_cast<uint32_t>(alignof(long))};
                            case TB::kCLongLong:
                            case TB::kCULongLong:
                            case TB::kCDouble:
                                return {8u, 8u};
                            case TB::kCSize:
                            case TB::kCSSize:
                            case TB::kCPtrDiff:
                            case TB::kVaList:
                                return {8u, 8u};
                            default:
                                return {8u, 8u};
                        }
                    case TK::kPtr:
                    case TK::kBorrow:
                    case TK::kEscape:
                    case TK::kFn:
                        return {8u, 8u};
                    case TK::kOptional: {
                        const auto [elem_size, elem_align] = resolve(resolve, ct.elem);
                        const uint32_t a = std::max<uint32_t>(1u, elem_align);
                        const uint32_t body = align_to_(1u, a) + std::max<uint32_t>(1u, elem_size);
                        return {body, a};
                    }
                    case TK::kArray: {
                        const auto [elem_size, elem_align] = resolve(resolve, ct.elem);
                        const uint32_t e = std::max<uint32_t>(1u, elem_size);
                        const uint32_t a = std::max<uint32_t>(1u, elem_align);
                        if (!ct.array_has_size) return {16u, 8u};
                        return {e * std::max<uint32_t>(1u, ct.array_size), a};
                    }
                    case TK::kNamedUser: {
                        if (actor_type_set.find(cur) != actor_type_set.end()) return {8u, 8u};
                        (void)self(self, cur);
                        auto it = named_layout_by_type.find(cur);
                        if (it != named_layout_by_type.end()) return it->second;
                        return {8u, 8u};
                    }
                }
                return {8u, 8u};
            };

            FieldLayoutDecl of{};
            of.name = ty_.to_string(tid);
            of.self_type = tid;
            of.layout = parsed.layout;
            of.align = parsed.explicit_align;

            uint32_t offset = 0;
            uint32_t struct_align = std::max<uint32_t>(1u, of.align);
            for (const auto& member : parsed.members) {
                const parus::ty::TypeId concrete_member_ty =
                    substitute_external_generic_params_(ty_, member.type, subst);
                const auto [member_size_raw, member_align_raw] = resolve_size_align(resolve_size_align, concrete_member_ty);
                const uint32_t member_size = std::max<uint32_t>(1u, member_size_raw);
                const uint32_t member_align = std::max<uint32_t>(1u, member_align_raw);

                if (of.layout == FieldLayout::C) {
                    offset = align_to_(offset, member_align);
                }

                FieldMemberLayout om{};
                om.name = member.name;
                om.type = concrete_member_ty;
                om.offset = offset;
                of.members.push_back(std::move(om));

                offset += member_size;
                struct_align = std::max(struct_align, member_align);
            }

            if (of.layout == FieldLayout::C) {
                of.size = std::max<uint32_t>(1u, align_to_(offset, struct_align));
            } else {
                of.size = std::max<uint32_t>(1u, offset);
            }
            if (of.align == 0) of.align = struct_align;

            out.mod.add_field(of);
            named_layout_by_type[tid] = {of.size, std::max<uint32_t>(1u, of.align)};
            return true;
        };

        if (sym_ != nullptr) {
            for (const auto& ss : sym_->symbols()) {
                if (!ss.is_external || ss.kind != parus::sema::SymbolKind::kType) continue;
                if (ss.declared_type == parus::ty::kInvalidType || ss.external_payload.empty()) continue;
                if (named_layout_by_type.find(ss.declared_type) != named_layout_by_type.end()) continue;

                const auto parsed = parse_external_record_layout_payload_(ss.external_payload);
                if (!parsed.ok) continue;

                FieldLayoutDecl of{};
                of.name = ss.name;
                of.self_type = ss.declared_type;
                of.layout = parsed.layout;
                of.align = std::max<uint32_t>(1u, parsed.align);
                of.size = std::max<uint32_t>(1u, parsed.size);
                of.members = parsed.members;
                out.mod.add_field(of);
                named_layout_by_type[of.self_type] = {of.size, of.align};
            }
        }

        if (sym_ != nullptr) {
            for (parus::ty::TypeId tid = 0; tid < ty_.count(); ++tid) {
                (void)ensure_external_parus_field_layout(ensure_external_parus_field_layout, tid);
            }
        }

        if (tag_only_enum_type_ids_ != nullptr) {
            auto append_tag_keys = [&](TypeId tid, std::unordered_set<std::string>& out) {
                if (tid == parus::ty::kInvalidType) return;

                std::vector<std::string_view> path{};
                std::vector<TypeId> args{};
                if (!ty_.decompose_named_user(tid, path, args) || path.empty()) {
                    out.insert(ty_.to_string(tid));
                    return;
                }

                std::string full{};
                for (size_t i = 0; i < path.size(); ++i) {
                    if (i != 0) full += "::";
                    full += std::string(path[i]);
                }
                out.insert(full);

                if (path.size() >= 2) {
                    std::string dropped_first{};
                    for (size_t i = 1; i < path.size(); ++i) {
                        if (i != 1) dropped_first += "::";
                        dropped_first += std::string(path[i]);
                    }
                    out.insert(std::move(dropped_first));
                }
            };

            std::unordered_set<std::string> tag_only_enum_names{};
            tag_only_enum_names.reserve(tag_only_enum_type_ids_->size() * 2u);
            for (const auto enum_ty : *tag_only_enum_type_ids_) {
                append_tag_keys(enum_ty, tag_only_enum_names);
            }

            auto add_tag_only_layout = [&](TypeId enum_ty) {
                if (enum_ty == parus::ty::kInvalidType) return;
                if (named_layout_by_type.find(enum_ty) != named_layout_by_type.end()) return;

                FieldLayoutDecl of{};
                of.name = ty_.to_string(enum_ty);
                of.self_type = enum_ty;
                of.layout = FieldLayout::None;
                of.align = 4;
                of.size = 4;

                FieldMemberLayout tag{};
                tag.name = "__tag";
                tag.type = (TypeId)ty_.builtin(parus::ty::Builtin::kI32);
                tag.offset = 0;
                of.members.push_back(std::move(tag));

                out.mod.add_field(of);
                named_layout_by_type[enum_ty] = {4u, 4u};
            };

            for (const auto enum_ty : *tag_only_enum_type_ids_) {
                add_tag_only_layout(enum_ty);
            }

            for (uint32_t i = 0; i < ty_.count(); ++i) {
                const auto cur = static_cast<TypeId>(i);
                const auto& tt = ty_.get(cur);
                if (tt.kind != parus::ty::Kind::kNamedUser) continue;
                std::unordered_set<std::string> cur_keys{};
                append_tag_keys(cur, cur_keys);
                bool matches = false;
                for (const auto& key : cur_keys) {
                    if (tag_only_enum_names.find(key) != tag_only_enum_names.end()) {
                        matches = true;
                        break;
                    }
                }
                if (!matches) continue;
                add_tag_only_layout(cur);
            }
        }

        std::unordered_map<parus::sir::SymbolId, uint32_t> global_symbol_to_global;
        struct GlobalInitItem {
            parus::sir::SymbolId sym = parus::sir::k_invalid_symbol;
            uint32_t gid = kInvalidId;
            parus::sir::ValueId init = parus::sir::k_invalid_value;
            TypeId type = kInvalidId;
        };
        std::vector<GlobalInitItem> global_init_items{};
        for (const auto& sg : sir_.globals) {
            GlobalDecl g{};
            if (sg.abi == parus::sir::FuncAbi::kC || sg.is_export) {
                g.name = std::string(sg.name);
            } else {
                g.name = std::string(sg.name) + "$g";
            }
            g.type = sg.declared_type;
            g.abi = map_func_abi_(sg.abi);
            g.is_extern = sg.is_extern;
            g.is_const = sg.is_const;
            switch (sg.c_tls_kind) {
                case parus::sir::CThreadLocalKind::kDynamic:
                    g.c_tls_kind = CThreadLocalKind::Dynamic;
                    break;
                case parus::sir::CThreadLocalKind::kStatic:
                    g.c_tls_kind = CThreadLocalKind::Static;
                    break;
                case parus::sir::CThreadLocalKind::kNone:
                default:
                    g.c_tls_kind = CThreadLocalKind::None;
                    break;
            }
            switch (sg.const_init.kind) {
                case parus::sir::ConstInitKind::kInt:   g.const_init.kind = ConstInitKind::Int; break;
                case parus::sir::ConstInitKind::kFloat: g.const_init.kind = ConstInitKind::Float; break;
                case parus::sir::ConstInitKind::kBool:  g.const_init.kind = ConstInitKind::Bool; break;
                case parus::sir::ConstInitKind::kChar:  g.const_init.kind = ConstInitKind::Char; break;
                case parus::sir::ConstInitKind::kNone:
                default:                                g.const_init.kind = ConstInitKind::None; break;
            }
            g.const_init.text = sg.const_init.text;
            // runtime init path(module/bundle init) uses store; keep writable in IR.
            g.is_mut = g.is_const
                ? false
                : (sg.is_mut || (!sg.is_extern && sg.init != parus::sir::k_invalid_value));
            g.is_export = sg.is_export;

            const uint32_t gid = out.mod.add_global(g);
            if (sg.sym != parus::sir::k_invalid_symbol) {
                global_symbol_to_global[sg.sym] = gid;
            }
            if (g.is_const && !g.is_extern && g.const_init.kind == ConstInitKind::None) {
                out.gate_errors.push_back(parus::sir::VerifyError{
                    "const global lowering failed: missing const initializer for '" + g.name + "'"
                });
            }
            if (!g.is_const && !sg.is_extern && sg.init != parus::sir::k_invalid_value) {
                global_init_items.push_back(GlobalInitItem{
                    .sym = sg.sym,
                    .gid = gid,
                    .init = sg.init,
                    .type = g.type
                });
            }
        }

        bool has_throwing_function = false;
        for (const auto& sf : sir_.funcs) {
            if (sf.is_throwing) {
                has_throwing_function = true;
                break;
            }
        }
        uint32_t exc_active_gid = kInvalidId;
        uint32_t exc_type_gid = kInvalidId;
        std::unordered_map<TypeId, uint32_t> exc_payload_global_by_type{};
        if (has_throwing_function) {
            GlobalDecl g_active{};
            g_active.name = "__parus_exc_active";
            g_active.type = (TypeId)ty_.builtin(parus::ty::Builtin::kBool);
            g_active.abi = FunctionAbi::Parus;
            g_active.is_extern = false;
            g_active.is_mut = true;
            g_active.is_export = false;
            exc_active_gid = out.mod.add_global(g_active);

            GlobalDecl g_type{};
            g_type.name = "__parus_exc_type";
            g_type.type = (TypeId)ty_.builtin(parus::ty::Builtin::kI64);
            g_type.abi = FunctionAbi::Parus;
            g_type.is_extern = false;
            g_type.is_mut = true;
            g_type.is_export = false;
            exc_type_gid = out.mod.add_global(g_type);

            std::vector<TypeId> throw_payload_types{};
            throw_payload_types.reserve(8);
            std::unordered_set<TypeId> seen_payload_types{};
            for (size_t sid = 0; sid < sir_.stmts.size(); ++sid) {
                const auto& st = sir_.stmts[sid];
                if (st.kind != parus::sir::StmtKind::kThrowStmt) continue;
                if (st.expr == parus::sir::k_invalid_value) continue;
                if ((size_t)st.expr >= sir_.values.size()) continue;
                const auto& sv = sir_.values[st.expr];
                TypeId ty = sv.type;
                if (ty == kInvalidId) continue;
                const auto& tt = ty_.get(ty);
                const bool is_payload_kind =
                    (tt.kind == parus::ty::Kind::kNamedUser ||
                     tt.kind == parus::ty::Kind::kOptional ||
                     tt.kind == parus::ty::Kind::kArray);
                if (!is_payload_kind) continue;
                if (!seen_payload_types.insert(ty).second) continue;
                throw_payload_types.push_back(ty);
            }
            std::sort(throw_payload_types.begin(), throw_payload_types.end());
            for (const auto ty : throw_payload_types) {
                GlobalDecl g_payload{};
                g_payload.name = "__parus_exc_payload$" + std::to_string((uint32_t)ty);
                g_payload.type = ty;
                g_payload.abi = FunctionAbi::Parus;
                g_payload.is_extern = false;
                g_payload.is_mut = true;
                g_payload.is_export = false;
                const uint32_t gid = out.mod.add_global(g_payload);
                exc_payload_global_by_type[ty] = gid;
            }
        }

        std::vector<std::pair<parus::sir::SymbolId, uint32_t>> sorted_globals{};
        sorted_globals.reserve(global_symbol_to_global.size());
        for (const auto& kv : global_symbol_to_global) {
            sorted_globals.push_back(kv);
        }
        std::sort(sorted_globals.begin(), sorted_globals.end(),
                  [](const auto& a, const auto& b) {
                      if (a.second != b.second) return a.second < b.second;
                      return a.first < b.first;
                  });

        auto symbol_is_core_module_fn_ = [&](const parus::sir::Func& sf, std::string_view module_head) -> bool {
            if (sym_ == nullptr ||
                sf.sym == parus::sir::k_invalid_symbol ||
                static_cast<size_t>(sf.sym) >= sym_->symbols().size()) {
                return false;
            }
            const auto& ss = sym_->symbol(sf.sym);
            const bool module_match =
                ss.decl_module_head == module_head ||
                ss.decl_module_head == ("core::" + std::string(module_head));
            return ss.kind == parus::sema::SymbolKind::kFn &&
                   ss.decl_bundle_name == "core" &&
                   module_match;
        };

        struct ParsedImplBindingPayload {
            std::string_view key{};
            bool compiler_owned = false;
        };

        auto parse_impl_binding_payload_ = [](std::string_view payload) -> ParsedImplBindingPayload {
            ParsedImplBindingPayload out{};
            if (!payload.starts_with("parus_impl_binding|key=")) return out;
            std::string_view rest = payload.substr(std::string_view("parus_impl_binding|key=").size());
            std::string_view key = rest;
            std::string_view mode = "compiler";
            if (const size_t split = rest.find('|'); split != std::string_view::npos) {
                key = rest.substr(0, split);
                rest = rest.substr(split + 1);
                while (!rest.empty()) {
                    const size_t next = rest.find('|');
                    const std::string_view part =
                        (next == std::string_view::npos) ? rest : rest.substr(0, next);
                    if (part.starts_with("mode=")) {
                        mode = part.substr(std::string_view("mode=").size());
                    }
                    if (next == std::string_view::npos) break;
                    rest = rest.substr(next + 1);
                }
            }
            out.key = key;
            out.compiler_owned = (mode == "compiler");
            return out;
        };

        auto func_has_impl_binding_ = [&](const parus::sir::Func& sf, std::string_view key) -> bool {
            const auto direct = parse_impl_binding_payload_(sf.external_payload);
            if (direct.compiler_owned && direct.key == key) return true;
            if (sym_ == nullptr ||
                sf.sym == parus::sir::k_invalid_symbol ||
                static_cast<size_t>(sf.sym) >= sym_->symbols().size()) {
                return false;
            }
            const auto via_sym = parse_impl_binding_payload_(sym_->symbol(sf.sym).external_payload);
            return via_sym.compiler_owned && via_sym.key == key;
        };

        auto parse_core_mem_intrinsic_ty_ = [&](const parus::sir::Func& sf, std::string_view leaf) -> std::optional<TypeId> {
            if (!func_has_impl_binding_(sf, leaf == "size_of" ? "Impl::SizeOf" : "Impl::AlignOf")) {
                return std::nullopt;
            }
            const auto type_arg = trailing_instantiated_type_arg_(sf.name, leaf);
            if (!type_arg.has_value()) return std::nullopt;
            auto& mutable_types = const_cast<parus::ty::TypePool&>(ty_);
            const auto parsed = parus::cimport::parse_external_type_repr(*type_arg, {}, mutable_types);
            if (parsed == parus::ty::kInvalidType) return std::nullopt;
            return static_cast<TypeId>(parsed);
        };

        auto add_block_param_oir = [&](BlockId bb, TypeId ty) -> ValueId {
            auto& block = out.mod.blocks[bb];
            Value v{};
            v.ty = ty;
            v.eff = Effect::Pure;
            v.def_a = bb;
            v.def_b = static_cast<uint32_t>(block.params.size());
            const ValueId vid = out.mod.add_value(v);
            block.params.push_back(vid);
            return vid;
        };

        ActorRuntimeFuncs actor_runtime{};
        auto add_runtime_decl_ = [&](std::string name, TypeId ret_ty, std::initializer_list<TypeId> params) -> FuncId {
            Function f{};
            f.name = std::move(name);
            f.source_name = f.name;
            f.abi = FunctionAbi::C;
            f.is_extern = true;
            f.is_c_variadic = false;
            f.c_fixed_param_count = static_cast<uint32_t>(params.size());
            f.ret_ty = ret_ty;
            const BlockId entry = out.mod.add_block(Block{});
            f.entry = entry;
            f.blocks.push_back(entry);
            const FuncId fid = out.mod.add_func(f);
            for (const auto ty : params) {
                (void)add_block_param_oir(entry, ty);
            }
            return fid;
        };

        const TypeId ptr_ty = (TypeId)ty_.builtin(parus::ty::Builtin::kNull);
        const TypeId u32_ty = (TypeId)ty_.builtin(parus::ty::Builtin::kU32);
        const TypeId u64_ty = (TypeId)ty_.builtin(parus::ty::Builtin::kU64);
        const TypeId unit_ty = (TypeId)ty_.builtin(parus::ty::Builtin::kUnit);
        actor_runtime.new_fn = add_runtime_decl_("__parus_actor_new", ptr_ty, {u64_ty, u64_ty, u64_ty});
        actor_runtime.clone_fn = add_runtime_decl_("__parus_actor_clone", ptr_ty, {ptr_ty});
        actor_runtime.release_fn = add_runtime_decl_("__parus_actor_release", unit_ty, {ptr_ty});
        actor_runtime.enter_fn = add_runtime_decl_("__parus_actor_enter", ptr_ty, {ptr_ty, u32_ty});
        actor_runtime.draft_ptr_fn = add_runtime_decl_("__parus_actor_draft_ptr", ptr_ty, {ptr_ty});
        actor_runtime.commit_fn = add_runtime_decl_("__parus_actor_commit", unit_ty, {ptr_ty});
        actor_runtime.recast_fn = add_runtime_decl_("__parus_actor_recast", unit_ty, {ptr_ty});
        actor_runtime.leave_fn = add_runtime_decl_("__parus_actor_leave", unit_ty, {ptr_ty});

        // Build all functions in SIR module.
        // Strategy:
        // 1) 함수 쉘/엔트리 블록을 먼저 전부 생성해 심볼->FuncId를 고정한다.
        // 2) 두 번째 패스에서 바디를 lowering한다.
        // 이렇게 해야 전방 함수 호출/오버로드 호출에서도 direct callee를 안정적으로 참조할 수 있다.
        std::unordered_map<parus::sir::ValueId, ValueId> escape_value_map;
        std::vector<FuncId> sir_to_oir_func(sir_.funcs.size(), kInvalidId);
        std::vector<BlockId> sir_to_entry(sir_.funcs.size(), kInvalidId);
        std::unordered_map<parus::sir::SymbolId, FuncId> fn_symbol_to_func;
        std::unordered_map<parus::sir::SymbolId, std::vector<FuncId>> fn_symbol_to_funcs;
        std::unordered_map<uint32_t, FuncId> fn_decl_to_func;
        std::unordered_map<std::string, FuncId> fn_link_name_to_func;
        std::unordered_map<std::string, FuncId> fn_source_name_to_func;
        std::unordered_set<FuncId> throwing_func_ids;
        const auto sir_type_contains_unresolved_generic_param = [&](auto&& self, TypeId t) -> bool {
            if (t == ty::kInvalidType) return false;
            const auto& tt = ty_.get(t);
            switch (tt.kind) {
                case parus::ty::Kind::kNamedUser: {
                    std::vector<std::string_view> path{};
                    std::vector<TypeId> args{};
                    if (!ty_.decompose_named_user(t, path, args) || path.empty()) return false;
                    if (args.empty() && path.size() == 1) {
                        return !leaf_name_resolves_to_concrete_type_(
                            sir_,
                            ty_,
                            sym_,
                            path.front()
                        );
                    }
                    for (const auto arg_t : args) {
                        if (self(self, arg_t)) return true;
                    }
                    return false;
                }
                case parus::ty::Kind::kBorrow:
                case parus::ty::Kind::kEscape:
                case parus::ty::Kind::kPtr:
                case parus::ty::Kind::kOptional:
                case parus::ty::Kind::kArray:
                    return tt.elem != ty::kInvalidType && self(self, tt.elem);
                case parus::ty::Kind::kFn:
                    for (uint32_t i = 0; i < tt.param_count; ++i) {
                        if (self(self, ty_.fn_param_at(t, i))) return true;
                    }
                    return tt.ret != ty::kInvalidType && self(self, tt.ret);
                default:
                    return false;
            }
        };
        for (size_t i = 0; i < sir_.funcs.size(); ++i) {
            const auto& sf = sir_.funcs[i];
            const bool compiler_owned_impl =
                func_has_impl_binding_(sf, "Impl::SpinLoop") ||
                func_has_impl_binding_(sf, "Impl::StepNext") ||
                func_has_impl_binding_(sf, "Impl::SizeOf") ||
                func_has_impl_binding_(sf, "Impl::AlignOf");
            if (sf.is_extern &&
                (symbol_is_core_module_fn_(sf, "mem") ||
                 func_has_impl_binding_(sf, "Impl::SpinLoop") ||
                 func_has_impl_binding_(sf, "Impl::StepNext"))) {
                continue;
            }
            if (compiler_owned_impl) {
                continue;
            }
            if (sir_type_contains_unresolved_generic_param(sir_type_contains_unresolved_generic_param, sf.ret)) {
                continue;
            }
            bool skip_for_unresolved_param = false;
            for (uint32_t pi = 0; pi < sf.param_count; ++pi) {
                const auto param_idx = sf.param_begin + pi;
                if (param_idx >= sir_.params.size()) continue;
                if (sir_type_contains_unresolved_generic_param(
                        sir_type_contains_unresolved_generic_param,
                        sir_.params[param_idx].type)) {
                    skip_for_unresolved_param = true;
                    break;
                }
            }
            if (skip_for_unresolved_param) {
                continue;
            }
            const auto wrapper_payload = parse_cimport_wrapper_payload_(sf.external_payload);

            Function f{};
            // C ABI 함수는 심볼을 비맹글 기반으로 유지한다.
            if (sf.is_extern && !sf.external_link_name.empty()) {
                f.name = sf.external_link_name;
            } else {
                f.name = (sf.abi == parus::sir::FuncAbi::kC)
                    ? std::string(sf.name)
                    : mangle_func_name_(sf, sir_, ty_, sir_.bundle_name);
            }
            f.source_name = sf.name;
            f.abi = map_func_abi_(sf.abi);
            f.c_callconv = map_c_callconv_(sf.c_callconv);
            f.is_extern = sf.is_extern && !wrapper_payload.is_wrapper;
            f.is_c_variadic = sf.is_c_variadic;
            f.c_fixed_param_count = sf.c_fixed_param_count;
            f.is_pure = sf.is_pure;
            f.is_comptime = sf.is_comptime;
            f.is_const = sf.is_const;
            f.is_actor_member = sf.is_actor_member;
            f.is_actor_init = sf.is_actor_init;
            f.actor_owner_type = sf.actor_owner_type;
            f.ret_ty = (TypeId)sf.ret;

            FuncId fid = kInvalidId;
            BlockId entry = kInvalidId;
            if (auto it = fn_link_name_to_func.find(f.name); it != fn_link_name_to_func.end()) {
                const FuncId cand = it->second;
                if (static_cast<size_t>(cand) < out.mod.funcs.size() &&
                    out.mod.funcs[cand].is_extern) {
                    fid = cand;
                    entry = out.mod.funcs[fid].entry;
                    auto& existing = out.mod.funcs[fid];
                    existing.source_name = sf.name;
                    existing.abi = f.abi;
                    existing.c_callconv = f.c_callconv;
                    existing.is_extern = false;
                    existing.is_c_variadic = f.is_c_variadic;
                    existing.c_fixed_param_count = f.c_fixed_param_count;
                    existing.is_pure = f.is_pure;
                    existing.is_comptime = f.is_comptime;
                    existing.is_const = f.is_const;
                    existing.is_actor_member = f.is_actor_member;
                    existing.is_actor_init = f.is_actor_init;
                    existing.actor_owner_type = f.actor_owner_type;
                    existing.ret_ty = f.ret_ty;
                }
            }

            if (fid == kInvalidId) {
                entry = out.mod.add_block(Block{});
                f.entry = entry;
                f.blocks.push_back(entry);

                fid = out.mod.add_func(f);
                fn_link_name_to_func[out.mod.funcs[fid].name] = fid;
                fn_source_name_to_func[out.mod.funcs[fid].source_name] = fid;
                const uint64_t pend = (uint64_t)sf.param_begin + (uint64_t)sf.param_count;
                if (pend <= (uint64_t)sir_.params.size()) {
                    for (uint32_t pidx = 0; pidx < sf.param_count; ++pidx) {
                        const auto& sp = sir_.params[sf.param_begin + pidx];
                        (void)add_block_param_oir(entry, (TypeId)sp.type);
                    }
                }
            } else if (!sf.name.empty()) {
                fn_source_name_to_func[std::string(sf.name)] = fid;
            }

            sir_to_oir_func[i] = fid;
            sir_to_entry[i] = entry;
            if (sf.is_actor_member || sf.is_actor_init) {
                out.mod.funcs[fid].actor_ctx_param_index =
                    static_cast<uint32_t>(out.mod.blocks[entry].params.size());
                (void)add_block_param_oir(entry, ptr_ty);
            }
            if (sf.is_throwing) {
                throwing_func_ids.insert(fid);
            }

            if (sf.sym != parus::sir::k_invalid_symbol) {
                fn_symbol_to_func[sf.sym] = fid;
                fn_symbol_to_funcs[sf.sym].push_back(fid);
            }
            if (sf.origin_stmt != 0xFFFF'FFFFu) {
                fn_decl_to_func[sf.origin_stmt] = fid;
            }
        }

        if (sym_ != nullptr) {
            for (uint32_t sid = 0; sid < sym_->symbols().size(); ++sid) {
                const auto& ss = sym_->symbol(sid);
                if (!ss.is_external || ss.kind != parus::sema::SymbolKind::kFn) continue;
                if (ss.declared_type == parus::ty::kInvalidType || ss.declared_type >= ty_.count()) continue;
                const auto& fn_ty = ty_.get(ss.declared_type);
                if (fn_ty.kind != parus::ty::Kind::kFn) continue;
                if (sir_type_contains_unresolved_generic_param(
                        sir_type_contains_unresolved_generic_param,
                        ss.declared_type)) {
                    continue;
                }

                const std::string link_name =
                    maybe_specialize_external_generic_link_name_(ss, ss.declared_type, ty_);
                if (link_name.empty()) continue;
                if (fn_link_name_to_func.find(link_name) != fn_link_name_to_func.end()) continue;
                if (!ss.name.empty() && fn_source_name_to_func.find(ss.name) != fn_source_name_to_func.end()) {
                    continue;
                }

                Function f{};
                f.name = link_name;
                f.source_name = ss.name.empty() ? link_name : ss.name;
                f.abi = ty_.fn_is_c_abi(ss.declared_type) ? FunctionAbi::C : FunctionAbi::Parus;
                f.c_callconv = map_c_callconv_(static_cast<parus::sir::CCallConv>(ty_.fn_callconv(ss.declared_type)));
                f.is_extern = true;
                f.is_c_variadic = ty_.fn_is_c_variadic(ss.declared_type);
                f.c_fixed_param_count = fn_ty.param_count;
                f.ret_ty = fn_ty.ret;

                const BlockId entry = out.mod.add_block(Block{});
                f.entry = entry;
                f.blocks.push_back(entry);

                const FuncId fid = out.mod.add_func(f);
                fn_link_name_to_func[f.name] = fid;
                fn_source_name_to_func[f.source_name] = fid;
                fn_symbol_to_func[sid] = fid;
                fn_symbol_to_funcs[sid].push_back(fid);
                for (uint32_t pi = 0; pi < fn_ty.param_count; ++pi) {
                    (void)add_block_param_oir(entry, (TypeId)ty_.fn_param_at(ss.declared_type, pi));
                }
            }
        }

        std::unordered_map<TypeId, FuncId> class_deinit_map;
        auto has_suffix_ = [](std::string_view s, std::string_view suffix) -> bool {
            return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
        };
        for (size_t i = 0; i < sir_.funcs.size(); ++i) {
            if (i >= sir_to_oir_func.size()) continue;
            const auto& sf = sir_.funcs[i];
            if (!has_suffix_(sf.name, "::deinit")) continue;
            const FuncId fid = sir_to_oir_func[i];
            if (fid == kInvalidId) continue;

            const uint64_t pb = sf.param_begin;
            const uint64_t pe = pb + sf.param_count;
            if (pb > sir_.params.size() || pe > sir_.params.size()) continue;

            for (uint32_t pi = 0; pi < sf.param_count; ++pi) {
                const auto& sp = sir_.params[sf.param_begin + pi];
                const auto& pt = ty_.get(sp.type);
                if (pt.kind != parus::ty::Kind::kBorrow) continue;
                if (pt.elem == parus::ty::kInvalidType) continue;
                const auto& et = ty_.get(pt.elem);
                if (et.kind != parus::ty::Kind::kNamedUser) continue;
                class_deinit_map[(TypeId)pt.elem] = fid;
                break;
            }
        }

        for (size_t i = 0; i < sir_.funcs.size(); ++i) {
            const auto& sf = sir_.funcs[i];
            const FuncId fid = sir_to_oir_func[i];
            const BlockId entry = sir_to_entry[i];
            if (fid == kInvalidId || entry == kInvalidId || (size_t)fid >= out.mod.funcs.size()) {
                continue;
            }

            FuncBuild fb{};
            fb.out = &out.mod;
            fb.sir = &sir_;
            fb.types = &ty_;
            fb.symtab = sym_;
            fb.named_layout_by_type = &named_layout_by_type;
            fb.actor_types = &actor_type_set;
            fb.actor_runtime = &actor_runtime;
            fb.escape_value_map = &escape_value_map;
            fb.fn_symbol_to_func = &fn_symbol_to_func;
            fb.fn_symbol_to_funcs = &fn_symbol_to_funcs;
            fb.fn_link_name_to_func = &fn_link_name_to_func;
            fb.fn_source_name_to_func = &fn_source_name_to_func;
            fb.fn_decl_to_func = &fn_decl_to_func;
            fb.global_symbol_to_global = &global_symbol_to_global;
            fb.class_deinit_map = &class_deinit_map;
            fb.exc_payload_globals = &exc_payload_global_by_type;
            fb.throwing_funcs = &throwing_func_ids;
            fb.build_errors = &out.gate_errors;
            fb.exc_active_global = exc_active_gid;
            fb.exc_type_global = exc_type_gid;
            fb.fn_is_throwing = sf.is_throwing;
            fb.def = &out.mod.funcs[fid];
            fb.def_id = fid;
            fb.cur_bb = entry;

            for (const auto& kv : sorted_globals) {
                const auto gid = kv.second;
                if ((size_t)gid >= out.mod.globals.size()) continue;
                const auto& g = out.mod.globals[gid];
                ValueId gref = fb.emit_global_ref(gid, g.name);
                fb.bind(kv.first, FuncBuild::Binding{true, true, gref, kInvalidId});
            }

            fb.push_scope();
            if (exc_active_gid != kInvalidId) {
                fb.emit_set_exc_active_(false);
                fb.emit_set_exc_type_(kInvalidId);
            }
            // 함수 파라미터를 entry block parameter로 시드하고 심볼 바인딩을 연결한다.
            const uint64_t pend = (uint64_t)sf.param_begin + (uint64_t)sf.param_count;
            if (pend <= (uint64_t)sir_.params.size()) {
                for (uint32_t pidx = 0; pidx < sf.param_count; ++pidx) {
                    const auto& sp = sir_.params[sf.param_begin + pidx];
                    if (pidx >= out.mod.blocks[entry].params.size()) continue;
                    ValueId pv = out.mod.blocks[entry].params[pidx];
                    if (sp.sym == parus::sir::k_invalid_symbol) continue;

                    const auto& param_tt = ty_.get((TypeId)sp.type);
                    if ((param_tt.kind == parus::ty::Kind::kBorrow ||
                         param_tt.kind == parus::ty::Kind::kPtr) &&
                        pv != kInvalidId) {
                        // Borrow/raw-pointer params are already address-carrying values.
                        // Binding them as direct-address locals keeps subplace lowering
                        // on the pointee instead of slotifying the pointer bytes.
                        fb.bind(sp.sym, FuncBuild::Binding{true, true, pv, kInvalidId});
                        continue;
                    }

                    const bool needs_cleanup = fb.type_needs_drop_((TypeId)sp.type);
                    if (needs_cleanup) {
                        ValueId slot = fb.emit_alloca((TypeId)sp.type);
                        fb.emit_store(slot, pv);
                        const uint32_t cleanup_id = fb.register_cleanup(sp.sym, slot, (TypeId)sp.type);
                        fb.bind(sp.sym, FuncBuild::Binding{true, false, slot, cleanup_id});
                        if (!(sp.is_self && sp.is_mut) &&
                            fb.should_remember_home_slot((TypeId)sp.type)) {
                            fb.remember_home_slot(sp.sym, slot);
                        }
                    } else if (sp.is_mut) {
                        ValueId slot = fb.emit_alloca((TypeId)sp.type);
                        fb.emit_store(slot, pv);
                        fb.bind(sp.sym, FuncBuild::Binding{true, false, slot, kInvalidId});
                        if (!(sp.is_self && sp.is_mut) &&
                            fb.should_remember_home_slot((TypeId)sp.type)) {
                            fb.remember_home_slot(sp.sym, slot);
                        }
                    } else {
                        fb.bind(sp.sym, FuncBuild::Binding{false, false, pv, kInvalidId});
                    }
                }
            }
            if ((sf.is_actor_member || sf.is_actor_init) &&
                fb.def->actor_ctx_param_index != kInvalidId &&
                (size_t)fb.def->actor_ctx_param_index < out.mod.blocks[entry].params.size()) {
                fb.current_actor_ctx = out.mod.blocks[entry].params[fb.def->actor_ctx_param_index];
            }

            // Imported extern functions stay as declarations in OIR.
            // Lowering them into synthetic null-return stubs breaks installed-core calls
            // by shadowing the real linked implementation with a local body.
            if (out.mod.funcs[fid].is_extern) {
                fb.pop_scope();
                continue;
            }

            if (func_has_impl_binding_(sf, "Impl::SizeOf")) {
                const auto lowered_ty = parse_core_mem_intrinsic_ty_(sf, "size_of");
                if (!lowered_ty.has_value()) {
                    out.gate_errors.push_back({
                        "core::mem::size_of lowering failed: invalid instantiated type in '" +
                        std::string(sf.name) + "'"
                    });
                    ValueId rv = fb.emit_const_int((TypeId)sf.ret, "0");
                    fb.ret(rv);
                    fb.pop_scope();
                    continue;
                }
                const auto [size_bytes, align_bytes] = type_size_align(type_size_align, *lowered_ty);
                (void)align_bytes;
                ValueId rv = fb.emit_const_int((TypeId)sf.ret, std::to_string(std::max<uint32_t>(1u, size_bytes)));
                fb.ret(rv);
                fb.pop_scope();
                continue;
            }

            if (func_has_impl_binding_(sf, "Impl::AlignOf")) {
                const auto lowered_ty = parse_core_mem_intrinsic_ty_(sf, "align_of");
                if (!lowered_ty.has_value()) {
                    out.gate_errors.push_back({
                        "core::mem::align_of lowering failed: invalid instantiated type in '" +
                        std::string(sf.name) + "'"
                    });
                    ValueId rv = fb.emit_const_int((TypeId)sf.ret, "0");
                    fb.ret(rv);
                    fb.pop_scope();
                    continue;
                }
                const auto [size_bytes, align_bytes] = type_size_align(type_size_align, *lowered_ty);
                (void)size_bytes;
                ValueId rv = fb.emit_const_int((TypeId)sf.ret, std::to_string(std::max<uint32_t>(1u, align_bytes)));
                fb.ret(rv);
                fb.pop_scope();
                continue;
            }

            if (func_has_impl_binding_(sf, "Impl::SpinLoop")) {
                ValueId rv = fb.emit_const_null((TypeId)sf.ret);
                fb.ret(rv);
                fb.pop_scope();
                continue;
            }

            if (func_has_impl_binding_(sf, "Impl::StepNext")) {
                const TypeId step_ty =
                    (sf.param_count > 0 && sf.param_begin < sir_.params.size())
                        ? (TypeId)sir_.params[sf.param_begin].type
                        : kInvalidId;
                ValueId cur_v = kInvalidId;
                if (sf.param_count > 0 && !out.mod.blocks[entry].params.empty()) {
                    cur_v = out.mod.blocks[entry].params[0];
                }
                auto emit_step_limit_ = [&](TypeId ty) -> ValueId {
                    using B = parus::ty::Builtin;
                    const auto& tt = ty_.get(ty);
                    switch (tt.builtin) {
                        case B::kI8: return fb.emit_const_int(ty, "127");
                        case B::kI16: return fb.emit_const_int(ty, "32767");
                        case B::kI32: return fb.emit_const_int(ty, "2147483647");
                        case B::kI64: return fb.emit_const_int(ty, "9223372036854775807");
                        case B::kI128: return fb.emit_const_int(ty, "170141183460469231731687303715884105727");
                        case B::kU8: return fb.emit_const_int(ty, "255");
                        case B::kU16: return fb.emit_const_int(ty, "65535");
                        case B::kU32: return fb.emit_const_int(ty, "4294967295");
                        case B::kU64: return fb.emit_const_int(ty, "18446744073709551615");
                        case B::kU128: return fb.emit_const_int(ty, "340282366920938463463374607431768211455");
                        case B::kISize:
                            return fb.emit_const_int(
                                ty,
                                (sizeof(size_t) == 8u)
                                    ? "9223372036854775807"
                                    : "2147483647"
                            );
                        case B::kUSize:
                            return fb.emit_const_int(
                                ty,
                                (sizeof(size_t) == 8u)
                                    ? "18446744073709551615"
                                    : "4294967295"
                            );
                        case B::kChar: return fb.emit_const_char(ty, 0x10FFFFu);
                        default: return fb.emit_const_null(ty);
                    }
                };
                auto emit_step_one_ = [&](TypeId ty) -> ValueId {
                    const auto& tt = ty_.get(ty);
                    if (tt.kind == parus::ty::Kind::kBuiltin &&
                        tt.builtin == parus::ty::Builtin::kChar) {
                        return fb.emit_const_char(ty, 1u);
                    }
                    return fb.emit_const_int(ty, "1");
                };

                if (step_ty == kInvalidId || cur_v == kInvalidId) {
                    ValueId rv = fb.emit_const_null((TypeId)sf.ret);
                    fb.ret(rv);
                    fb.pop_scope();
                    continue;
                }

                const ValueId limit = emit_step_limit_(step_ty);
                const ValueId at_end = fb.emit_binop(fb.bool_type_(), Effect::Pure, BinOp::Eq, cur_v, limit);
                const BlockId fail_bb = fb.new_block();
                const BlockId succ_bb = fb.new_block();
                const BlockId join_bb = fb.new_block();
                const ValueId join_param = fb.add_block_param(join_bb, (TypeId)sf.ret);
                fb.condbr(at_end, fail_bb, {}, succ_bb, {});

                fb.def->blocks.push_back(fail_bb);
                fb.cur_bb = fail_bb;
                fb.br(join_bb, {fb.emit_const_null((TypeId)sf.ret)});

                fb.def->blocks.push_back(succ_bb);
                fb.cur_bb = succ_bb;
                {
                    const ValueId next_v =
                        fb.emit_binop(step_ty, Effect::Pure, BinOp::Add, cur_v, emit_step_one_(step_ty));
                    const ValueId some_v =
                        fb.emit_cast((TypeId)sf.ret, Effect::Pure, CastKind::As, (TypeId)sf.ret, next_v);
                    fb.br(join_bb, {some_v});
                }

                fb.def->blocks.push_back(join_bb);
                fb.cur_bb = join_bb;
                fb.ret(join_param);
                fb.pop_scope();
                continue;
            }

            const auto wrapper_payload = parse_cimport_wrapper_payload_(sf.external_payload);
            if (wrapper_payload.is_wrapper) {
                auto callee_it = fn_link_name_to_func.find(wrapper_payload.callee_link_name);
                if (callee_it == fn_link_name_to_func.end()) {
                    out.gate_errors.push_back({
                        "cimport wrapper lowering failed: unresolved callee '" + wrapper_payload.callee_link_name + "'"
                    });
                    ValueId rv = fb.emit_const_null((TypeId)sf.ret);
                    fb.ret(rv);
                    fb.pop_scope();
                    continue;
                }
                std::vector<ValueId> args{};
                args.reserve(wrapper_payload.arg_map.size());
                bool bad_argmap = false;
                for (const uint32_t param_index : wrapper_payload.arg_map) {
                    if (param_index >= out.mod.blocks[entry].params.size()) {
                        bad_argmap = true;
                        break;
                    }
                    args.push_back(out.mod.blocks[entry].params[param_index]);
                }
                if (bad_argmap) {
                    out.gate_errors.push_back({
                        "cimport wrapper lowering failed: invalid wrapper_argmap for '" + std::string(sf.name) + "'"
                    });
                    ValueId rv = fb.emit_const_null((TypeId)sf.ret);
                    fb.ret(rv);
                    fb.pop_scope();
                    continue;
                }
                ValueId rv = fb.emit_direct_call((TypeId)sf.ret, callee_it->second, std::move(args));
                fb.ret(rv);
                fb.pop_scope();
                continue;
            }

            fb.lower_block(sf.entry);
            fb.pop_scope();

            if (!out.mod.blocks[fb.cur_bb].has_term) {
                ValueId rv = fb.emit_const_null((TypeId)sf.ret);
                fb.ret(rv);
            }
        }

        // Build synthesized module init function:
        // - non-bundle: only when there is at least one runtime global initializer
        // - bundle: always emit (leader can call every module init deterministically)
        const bool need_module_init = sir_.bundle_enabled || !global_init_items.empty();
        if (need_module_init) {
            const TypeId unit_ty = (TypeId)ty_.builtin(parus::ty::Builtin::kUnit);
            Function init_fn{};
            init_fn.name = make_module_init_symbol_name_(sir_.bundle_name, sir_.current_source_norm);
            init_fn.source_name = "__parus_module_init";
            init_fn.abi = FunctionAbi::Parus;
            init_fn.is_extern = false;
            init_fn.is_pure = false;
            init_fn.is_comptime = false;
            init_fn.is_const = false;
            init_fn.ret_ty = unit_ty;
            const BlockId init_entry = out.mod.add_block(Block{});
            init_fn.entry = init_entry;
            init_fn.blocks.push_back(init_entry);
            const FuncId init_fid = out.mod.add_func(init_fn);
            out.mod.module_init_symbol = out.mod.funcs[init_fid].name;

            std::vector<GlobalInitItem> sorted_init_items = global_init_items;
            std::sort(sorted_init_items.begin(), sorted_init_items.end(),
                      [](const GlobalInitItem& a, const GlobalInitItem& b) {
                          if (a.gid != b.gid) return a.gid < b.gid;
                          return a.sym < b.sym;
                      });

            FuncBuild fb{};
            fb.out = &out.mod;
            fb.sir = &sir_;
            fb.types = &ty_;
            fb.symtab = sym_;
            fb.named_layout_by_type = &named_layout_by_type;
            fb.actor_types = &actor_type_set;
            fb.actor_runtime = &actor_runtime;
            fb.escape_value_map = &escape_value_map;
            fb.fn_symbol_to_func = &fn_symbol_to_func;
            fb.fn_symbol_to_funcs = &fn_symbol_to_funcs;
            fb.fn_link_name_to_func = &fn_link_name_to_func;
            fb.fn_source_name_to_func = &fn_source_name_to_func;
            fb.fn_decl_to_func = &fn_decl_to_func;
            fb.global_symbol_to_global = &global_symbol_to_global;
            fb.class_deinit_map = &class_deinit_map;
            fb.exc_payload_globals = &exc_payload_global_by_type;
            fb.throwing_funcs = &throwing_func_ids;
            fb.build_errors = &out.gate_errors;
            fb.exc_active_global = exc_active_gid;
            fb.exc_type_global = exc_type_gid;
            fb.fn_is_throwing = false;
            fb.def = &out.mod.funcs[init_fid];
            fb.cur_bb = init_entry;

            for (const auto& kv : sorted_globals) {
                const auto gid = kv.second;
                if ((size_t)gid >= out.mod.globals.size()) continue;
                const auto& g = out.mod.globals[gid];
                ValueId gref = fb.emit_global_ref(gid, g.name);
                fb.bind(kv.first, FuncBuild::Binding{true, true, gref, kInvalidId});
            }

            fb.push_scope();
            for (const auto& gi : sorted_init_items) {
                if (gi.init == parus::sir::k_invalid_value) continue;
                if (gi.gid == kInvalidId || (size_t)gi.gid >= out.mod.globals.size()) continue;
                const auto& g = out.mod.globals[gi.gid];
                ValueId slot = fb.emit_global_ref(gi.gid, g.name);
                ValueId init_v = fb.lower_value(gi.init);
                init_v = fb.coerce_value_for_target(gi.type, init_v);
                fb.emit_store(slot, init_v);
            }
            fb.pop_scope();

            if (!out.mod.blocks[fb.cur_bb].has_term) {
                ValueId rv = fb.emit_const_null(unit_ty);
                fb.ret(rv);
            }
        }

        if (!out.gate_errors.empty()) {
            out.gate_passed = false;
        }

        // SIR escape-handle 메타를 OIR 힌트로 연결한다.
        for (const auto& h : sir_.escape_handles) {
            auto it = escape_value_map.find(h.escape_value);
            if (it == escape_value_map.end()) continue;

            EscapeHandleHint hint{};
            hint.value = it->second;
            hint.pointee_type = (TypeId)h.pointee_type;
            hint.kind = map_escape_kind_(h.kind);
            hint.boundary = map_escape_boundary_(h.boundary);
            hint.from_static = h.from_static;
            hint.has_drop = h.has_drop;
            hint.abi_pack_required = h.abi_pack_required;
            out.mod.add_escape_hint(hint);
        }

        return out;
    }

} // namespace parus::oir
