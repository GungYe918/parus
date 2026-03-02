// frontend/include/parus/ty/TypePool.hpp
#pragma once
#include <parus/ty/Type.hpp>
#include <string_view>
#include <vector>
#include <string>
#include <ostream>
#include <cctype>


namespace parus::ty {

    class TypePool {
    public:
        TypePool() {
            // reserve: 128
            types_.reserve(128);
            fn_params_.reserve(256);
            fn_param_labels_.reserve(256);
            fn_param_has_default_.reserve(256);
            user_path_segs_.reserve(256);
            named_type_args_.reserve(256);

            // [0] canonical error type
            {
                Type err{};
                err.kind = Kind::kError;
                types_.push_back(err);
                error_id_ = 0;
            }

            // canonical builtins are created eagerly.
            // NOTE: builtins start after error type.
            for (int i = 0; i <= (int)Builtin::kInferInteger; ++i) {
                Type t{};
                t.kind = Kind::kBuiltin;
                t.builtin = (Builtin)i;
                builtin_ids_.push_back((TypeId)types_.size());
                types_.push_back(t);
            }
        }

        TypeId error() const {  return error_id_;  }

        TypeId builtin(Builtin b) const {
            return builtin_ids_[(int)b];
        }

        const Type& get(TypeId id) const { return types_[id]; }

        uint32_t count() const { return (uint32_t)types_.size(); }

        // ---- user-defined named type (path [+generic args]) interning ----
        // Stores path segments as a slice into user_path_segs_ to avoid string flatten.
        //
        // Example: Foo::Bar::Baz is stored as segs ["Foo","Bar","Baz"].
        TypeId make_named_user_path(const std::string_view* segs, uint32_t seg_count) {
            return make_named_user_path_with_args(segs, seg_count, nullptr, 0);
        }

        TypeId make_named_user_path_with_args(const std::string_view* segs,
                                              uint32_t seg_count,
                                              const TypeId* args,
                                              uint32_t arg_count) {
            if (!segs || seg_count == 0) {
                Type t{};
                t.kind = Kind::kNamedUser;
                t.path_begin = 0;
                t.path_count = 0;
                t.named_arg_begin = 0;
                t.named_arg_count = 0;
                return push_(t);
            }

            // Snapshot inputs to avoid alias/lifetime issues when caller-provided
            // string_views point to transient buffers or to our own growable storage.
            std::vector<std::string> seg_snapshot{};
            seg_snapshot.reserve(seg_count);
            for (uint32_t k = 0; k < seg_count; ++k) {
                seg_snapshot.emplace_back(segs[k]);
            }

            std::vector<TypeId> arg_snapshot{};
            arg_snapshot.reserve(arg_count);
            for (uint32_t k = 0; k < arg_count; ++k) {
                arg_snapshot.push_back(args ? args[k] : error());
            }

            // linear search v0: compare segment slices
            for (TypeId i = 0; i < (TypeId)types_.size(); ++i) {
                const auto& t = types_[i];
                if (t.kind != Kind::kNamedUser) continue;
                if (t.path_count != seg_count) continue;
                if (t.named_arg_count != arg_count) continue;

                bool same = true;
                for (uint32_t k = 0; k < seg_count; ++k) {
                    if (user_path_segs_[t.path_begin + k] != seg_snapshot[k]) { same = false; break; }
                }
                if (!same) continue;
                for (uint32_t k = 0; k < arg_count; ++k) {
                    if (named_type_args_[t.named_arg_begin + k] != arg_snapshot[k]) { same = false; break; }
                }
                if (same) return i;
            }

            Type t{};
            t.kind = Kind::kNamedUser;
            t.path_begin = (uint32_t)user_path_segs_.size();
            t.path_count = seg_count;
            t.named_arg_begin = (uint32_t)named_type_args_.size();
            t.named_arg_count = arg_count;

            for (uint32_t k = 0; k < seg_count; ++k) user_path_segs_.push_back(seg_snapshot[k]);
            for (uint32_t k = 0; k < arg_count; ++k) named_type_args_.push_back(arg_snapshot[k]);

            return push_(t);
        }

        // Convenience: intern a path
        TypeId intern_path(const std::string_view* segs, uint32_t seg_count) {
            // Builtin is only allowed for single-segment identifiers.
            if (segs && seg_count == 1) {
                Builtin b{};
                if (builtin_from_name(segs[0], b)) return builtin(b);
            }
            return make_named_user_path(segs, seg_count);
        }

        TypeId intern_named_path_with_args(const std::string_view* segs,
                                           uint32_t seg_count,
                                           const TypeId* args,
                                           uint32_t arg_count) {
            return make_named_user_path_with_args(segs, seg_count, args, arg_count);
        }

        // intern optional/array (simple linear search v0)
        TypeId make_optional(TypeId elem) {
            for (TypeId i = 0; i < (TypeId)types_.size(); ++i) {
                const auto& t = types_[i];
                if (t.kind == Kind::kOptional && t.elem == elem) return i;
            }

            Type t{};
            t.kind = Kind::kOptional;
            t.elem = elem;
            return push_(t);
        }

        /// @brief 배열 타입을 intern한다. `has_size=true`이면 `T[N]`, 아니면 `T[]`이다.
        TypeId make_array(TypeId elem, bool has_size = false, uint32_t size = 0) {
            for (TypeId i = 0; i < (TypeId)types_.size(); ++i) {
                const auto& t = types_[i];
                if (t.kind != Kind::kArray) continue;
                if (t.elem != elem) continue;
                if (t.array_has_size != has_size) continue;
                if (has_size && t.array_size != size) continue;
                return i;
            }

            Type t{};
            t.kind = Kind::kArray;
            t.elem = elem;
            t.array_has_size = has_size;
            t.array_size = size;
            return push_(t);
        }

        TypeId make_borrow(TypeId elem, bool is_mut) {
            for (TypeId i = 0; i < (TypeId)types_.size(); ++i) {
                const auto& t = types_[i];
                if (t.kind == Kind::kBorrow && t.elem == elem && t.borrow_is_mut == is_mut) return i;
            }

            Type t{};
            t.kind = Kind::kBorrow;
            t.elem = elem;
            t.borrow_is_mut = is_mut;
            return push_(t);
        }

        TypeId make_escape(TypeId elem) {
            for (TypeId i = 0; i < (TypeId)types_.size(); ++i) {
                const auto& t = types_[i];
                if (t.kind == Kind::kEscape && t.elem == elem) return i;
            }

            Type t{};
            t.kind = Kind::kEscape;
            t.elem = elem;
            return push_(t);
        }

        TypeId make_ptr(TypeId elem, bool is_mut) {
            for (TypeId i = 0; i < (TypeId)types_.size(); ++i) {
                const auto& t = types_[i];
                if (t.kind == Kind::kPtr && t.elem == elem && t.ptr_is_mut == is_mut) return i;
            }

            Type t{};
            t.kind = Kind::kPtr;
            t.elem = elem;
            t.ptr_is_mut = is_mut;
            return push_(t);
        }

        // ---- function signature type interning ----
        // positional_param_count:
        // - if UINT32_MAX, treat as "all positional"
        // labels/default flags:
        // - nullptr means all empty-label / no-default.
        TypeId make_fn(TypeId ret,
                       const TypeId* params,
                       uint32_t param_count,
                       uint32_t positional_param_count = 0xFFFF'FFFFu,
                       const std::string_view* labels = nullptr,
                       const uint8_t* has_default = nullptr) {
            if (positional_param_count == 0xFFFF'FFFFu) {
                positional_param_count = param_count;
            }
            if (positional_param_count > param_count) {
                positional_param_count = param_count;
            }

            // linear search v0 (ok)
            for (TypeId i = 0; i < (TypeId)types_.size(); ++i) {
                const auto& t = types_[i];
                if (t.kind != Kind::kFn) continue;
                if (t.ret != ret) continue;
                if (t.param_count != param_count) continue;
                if (t.positional_param_count != positional_param_count) continue;

                bool same = true;
                for (uint32_t k = 0; k < param_count; ++k) {
                    if (fn_params_[t.param_begin + k] != params[k]) { same = false; break; }
                }
                if (!same) continue;

                for (uint32_t k = 0; k < param_count; ++k) {
                    const std::string_view lhs_label = fn_param_labels_[t.label_begin + k];
                    const std::string_view rhs_label = labels ? labels[k] : std::string_view{};
                    if (lhs_label != rhs_label) {
                        same = false;
                        break;
                    }
                    const uint8_t lhs_def = fn_param_has_default_[t.default_begin + k];
                    const uint8_t rhs_def = has_default ? has_default[k] : 0u;
                    if (lhs_def != rhs_def) {
                        same = false;
                        break;
                    }
                }
                if (same) return i;
            }

            Type t{};
            t.kind = Kind::kFn;
            t.ret = ret;
            t.param_begin = (uint32_t)fn_params_.size();
            t.param_count = param_count;
            t.positional_param_count = positional_param_count;
            t.label_begin = (uint32_t)fn_param_labels_.size();
            t.default_begin = (uint32_t)fn_param_has_default_.size();

            for (uint32_t k = 0; k < param_count; ++k) {
                fn_params_.push_back(params ? params[k] : error());
                fn_param_labels_.push_back(labels ? labels[k] : std::string_view{});
                fn_param_has_default_.push_back(has_default ? has_default[k] : 0u);
            }

            return push_(t);
        }

        // ---- def signature introspection ----
        bool is_fn(TypeId id) const {
            return id != kInvalidType && id < types_.size() && types_[id].kind == Kind::kFn;
        }

        TypeId fn_param_at(TypeId def, uint32_t i) const {
            if (!is_fn(def)) return error();
            const Type& t = types_[def];
            if (i >= t.param_count) return error();
            return fn_params_[t.param_begin + i];
        }

        uint32_t fn_positional_count(TypeId def) const {
            if (!is_fn(def)) return 0;
            const Type& t = types_[def];
            return t.positional_param_count;
        }

        std::string_view fn_param_label_at(TypeId def, uint32_t i) const {
            if (!is_fn(def)) return {};
            const Type& t = types_[def];
            if (i >= t.param_count) return {};
            return fn_param_labels_[t.label_begin + i];
        }

        bool fn_param_has_default_at(TypeId def, uint32_t i) const {
            if (!is_fn(def)) return false;
            const Type& t = types_[def];
            if (i >= t.param_count) return false;
            return fn_param_has_default_[t.default_begin + i] != 0;
        }

        // convenience: ident -> (builtin or named_user)
        TypeId intern_ident(std::string_view name) {
            TypeId parsed = kInvalidType;
            if (parse_generic_applied_ident_(name, parsed)) return parsed;
            const std::string_view segs[1] = { name };
            return intern_path(segs, 1);
        }

        bool decompose_named_user(TypeId id,
                                  std::vector<std::string_view>& out_path,
                                  std::vector<TypeId>& out_args) const {
            out_path.clear();
            out_args.clear();
            if (id == kInvalidType || id >= types_.size()) return false;
            const auto& t = types_[id];
            if (t.kind != Kind::kNamedUser) return false;
            if (t.path_count == 0) return false;
            out_path.reserve(t.path_count);
            out_args.reserve(t.named_arg_count);
            for (uint32_t i = 0; i < t.path_count; ++i) {
                out_path.push_back(user_path_segs_[t.path_begin + i]);
            }
            for (uint32_t i = 0; i < t.named_arg_count; ++i) {
                out_args.push_back(named_type_args_[t.named_arg_begin + i]);
            }
            return true;
        }

        // builtin name -> Builtin (aliases 포함)
        static bool builtin_from_name(std::string_view name, Builtin& out) {
            // exact
            if (name == "null")   { out = Builtin::kNull; return true; }

            if (name == "void")  { out = Builtin::kUnit; return true; }
            if (name == "never") { out = Builtin::kNever; return true; }

            if (name == "bool")   { out = Builtin::kBool; return true; }
            if (name == "char")   { out = Builtin::kChar; return true; }
            if (name == "text")   { out = Builtin::kText; return true; }

            if (name == "i8")   { out = Builtin::kI8; return true; }
            if (name == "i16")  { out = Builtin::kI16; return true; }
            if (name == "i32")  { out = Builtin::kI32; return true; }
            if (name == "i64")  { out = Builtin::kI64; return true; }
            if (name == "i128") { out = Builtin::kI128; return true; }

            if (name == "u8")   { out = Builtin::kU8; return true; }
            if (name == "u16")  { out = Builtin::kU16; return true; }
            if (name == "u32")  { out = Builtin::kU32; return true; }
            if (name == "u64")  { out = Builtin::kU64; return true; }
            if (name == "u128") { out = Builtin::kU128; return true; }

            if (name == "isize") { out = Builtin::kISize; return true; }
            if (name == "usize") { out = Builtin::kUSize; return true; }

            if (name == "f32") { out = Builtin::kF32; return true; }
            if (name == "f64") { out = Builtin::kF64; return true; }
            if (name == "f128") { out = Builtin::kF128; return true; }

            // NOTE:
            // - Builtin::kInferInteger is INTERNAL ONLY.
            // - Builtin::kUnit is represented as "void" in source; users must not spell "unit".
            // - Users must not be able to spell it in source.
            return false;
        }

        // --------------------
        // Debug helpers
        // --------------------

        static std::string_view builtin_name(Builtin b) {
            switch (b) {
                case Builtin::kNull:   return "null";

                case Builtin::kUnit:  return "void";
                case Builtin::kNever: return "never";

                case Builtin::kBool:   return "bool";
                case Builtin::kChar:   return "char";
                case Builtin::kText:   return "text";

                case Builtin::kI8:   return "i8";
                case Builtin::kI16:  return "i16";
                case Builtin::kI32:  return "i32";
                case Builtin::kI64:  return "i64";
                case Builtin::kI128: return "i128";

                case Builtin::kU8:   return "u8";
                case Builtin::kU16:  return "u16";
                case Builtin::kU32:  return "u32";
                case Builtin::kU64:  return "u64";
                case Builtin::kU128: return "u128";

                case Builtin::kISize: return "isize";
                case Builtin::kUSize: return "usize";

                case Builtin::kF32: return "f32";
                case Builtin::kF64: return "f64";
                case Builtin::kF128: return "f128";

                case Builtin::kInferInteger: return "unsuffixed integer literal";
            }
            return "<builtin?>";
        }

        std::string to_string(TypeId id) const {
            std::string out;
            render_into_(out, id, /*parent_ctx=*/RenderCtx::kTop);
            return out;
        }

        // Export-index canonical format:
        // - stable and parser-friendly
        // - function types do not include parameter labels/default markers
        std::string to_export_string(TypeId id) const {
            std::string out;
            render_into_export_(out, id);
            return out;
        }

        void dump(std::ostream& os) const {
            os << "TYPE_POOL (count=" << types_.size() << ")\n";
            for (TypeId id = 0; id < (TypeId)types_.size(); ++id) {
                const Type& t = types_[id];

                os << "  [" << id << "] " << to_string(id) << "  ";

                switch (t.kind) {
                    case Kind::kError:
                        os << "(Error)";
                        break;
                    case Kind::kBuiltin:
                        os << "(Builtin=" << builtin_name(t.builtin) << ")";
                        break;
                    case Kind::kOptional:
                        os << "(Optional elem=" << t.elem << ")";
                        break;
                    case Kind::kArray:
                        os << "(Array elem=" << t.elem
                           << " sized=" << (t.array_has_size ? 1 : 0);
                        if (t.array_has_size) os << " size=" << t.array_size;
                        os << ")";
                        break;
                    case Kind::kNamedUser: {
                        os << "(NamedUser path=";
                        if (t.path_count == 0) {
                            os << "<empty>";
                        } else {
                            for (uint32_t k = 0; k < t.path_count; ++k) {
                                if (k) os << "::";
                                os << user_path_segs_[t.path_begin + k];
                            }
                        }
                        if (t.named_arg_count > 0) {
                            os << " args=<";
                            for (uint32_t k = 0; k < t.named_arg_count; ++k) {
                                if (k) os << ",";
                                os << to_string(named_type_args_[t.named_arg_begin + k]);
                            }
                            os << ">";
                        }
                        os << ")";
                        break;
                    }
                    case Kind::kBorrow:
                        os << "(Borrow mut=" << (t.borrow_is_mut ? 1 : 0) << " elem=" << t.elem << ")";
                        break;
                    case Kind::kEscape:
                        os << "(Escape elem=" << t.elem << ")";
                        break;
                    case Kind::kPtr:
                        os << "(Ptr mut=" << (t.ptr_is_mut ? 1 : 0) << " elem=" << t.elem << ")";
                        break;
                    case Kind::kFn:
                        os << "(Fn ret=" << t.ret
                           << " params=[" << t.param_begin
                           << ".." << (t.param_begin + t.param_count) << "]"
                           << " pos=" << t.positional_param_count << ")";
                        break;
                }

                os << "\n";
            }
        }

    private:
        TypeId push_(const Type& t) {
            TypeId id = (TypeId)types_.size();
            types_.push_back(t);
            return id;
        }

        enum class RenderCtx : uint8_t { kTop, kSuffixElem, kFnPart };

        static bool needs_parens_for_suffix_(Kind k) {
            // suffix를 붙일 때 애매해질 수 있는 형태들
            return k == Kind::kFn;
        }

        static bool needs_parens_for_prefix_(Kind k) {
            return k == Kind::kFn;
        }

        void render_into_export_(std::string& out, TypeId id) const {
            if (id == kInvalidType) { out += "<invalid-type>"; return; }
            if (id >= types_.size()) { out += "<bad-type-id>"; return; }

            const Type& t = types_[id];
            switch (t.kind) {
                case Kind::kError:
                    out += "<error>";
                    return;

                case Kind::kBuiltin:
                    out += builtin_name(t.builtin);
                    return;

                case Kind::kNamedUser: {
                    if (t.path_count == 0) { out += "<user-type?>"; return; }
                    for (uint32_t k = 0; k < t.path_count; ++k) {
                        if (k) out += "::";
                        const auto seg = user_path_segs_[t.path_begin + k];
                        out.append(seg.data(), seg.size());
                    }
                    if (t.named_arg_count > 0) {
                        out += "<";
                        for (uint32_t i = 0; i < t.named_arg_count; ++i) {
                            if (i) out += ",";
                            render_into_export_(out, named_type_args_[t.named_arg_begin + i]);
                        }
                        out += ">";
                    }
                    return;
                }

                case Kind::kOptional: {
                    if (t.elem == kInvalidType) { out += "<invalid-elem>?"; return; }
                    const Kind ek = (t.elem < types_.size()) ? types_[t.elem].kind : Kind::kError;
                    if (needs_parens_for_suffix_(ek)) out += "(";
                    render_into_export_(out, t.elem);
                    if (needs_parens_for_suffix_(ek)) out += ")";
                    out += "?";
                    return;
                }

                case Kind::kArray: {
                    if (t.elem == kInvalidType) { out += "<invalid-elem>[]"; return; }
                    const Kind ek = (t.elem < types_.size()) ? types_[t.elem].kind : Kind::kError;
                    const bool paren = needs_parens_for_suffix_(ek) || ek == Kind::kOptional;
                    if (paren) out += "(";
                    render_into_export_(out, t.elem);
                    if (paren) out += ")";
                    if (t.array_has_size) {
                        out += "[";
                        out += std::to_string(t.array_size);
                        out += "]";
                    } else {
                        out += "[]";
                    }
                    return;
                }

                case Kind::kBorrow: {
                    if (t.elem == kInvalidType) { out += (t.borrow_is_mut ? "&mut <invalid>" : "&<invalid>"); return; }

                    const Kind ek = (t.elem < types_.size()) ? types_[t.elem].kind : Kind::kError;
                    if (ek == Kind::kArray) {
                        const auto& arr = types_[t.elem];
                        if (!arr.array_has_size) {
                            out += "&";
                            if (t.borrow_is_mut) out += "mut ";
                            out += "[";
                            render_into_export_(out, arr.elem);
                            out += "]";
                            return;
                        }
                    }

                    out += "&";
                    if (t.borrow_is_mut) out += "mut ";
                    if (needs_parens_for_prefix_(ek)) out += "(";
                    render_into_export_(out, t.elem);
                    if (needs_parens_for_prefix_(ek)) out += ")";
                    return;
                }

                case Kind::kEscape: {
                    if (t.elem == kInvalidType) { out += "^&<invalid>"; return; }
                    const Kind ek = (t.elem < types_.size()) ? types_[t.elem].kind : Kind::kError;
                    out += "^&";
                    if (needs_parens_for_prefix_(ek)) out += "(";
                    render_into_export_(out, t.elem);
                    if (needs_parens_for_prefix_(ek)) out += ")";
                    return;
                }

                case Kind::kPtr: {
                    if (t.elem == kInvalidType) { out += (t.ptr_is_mut ? "ptr mut <invalid>" : "ptr <invalid>"); return; }
                    const Kind ek = (t.elem < types_.size()) ? types_[t.elem].kind : Kind::kError;
                    out += "ptr ";
                    if (t.ptr_is_mut) out += "mut ";
                    if (needs_parens_for_prefix_(ek)) out += "(";
                    render_into_export_(out, t.elem);
                    if (needs_parens_for_prefix_(ek)) out += ")";
                    return;
                }

                case Kind::kFn: {
                    out += "def(";
                    for (uint32_t i = 0; i < t.param_count; ++i) {
                        if (i) out += ", ";
                        const TypeId pid = fn_params_[t.param_begin + i];
                        render_into_export_(out, pid);
                    }
                    out += ") -> ";
                    render_into_export_(out, t.ret);
                    return;
                }
            }
        }

        void render_into_(std::string& out, TypeId id, RenderCtx /*parent_ctx*/) const {
            if (id == kInvalidType) { out += "<invalid-type>"; return; }
            if (id >= types_.size()) { out += "<bad-type-id>"; return; }

            const Type& t = types_[id];

            switch (t.kind) {
                case Kind::kError:
                    out += "<error>";
                    return;

                case Kind::kBuiltin:
                    out += builtin_name(t.builtin);
                    return;

                case Kind::kNamedUser: {
                    if (t.path_count == 0) { out += "<user-type?>"; return; }

                    for (uint32_t k = 0; k < t.path_count; ++k) {
                        if (k) out += "::";
                        const auto seg = user_path_segs_[t.path_begin + k];
                        out.append(seg.data(), seg.size());
                    }
                    if (t.named_arg_count > 0) {
                        out += "<";
                        for (uint32_t i = 0; i < t.named_arg_count; ++i) {
                            if (i) out += ",";
                            render_into_(out, named_type_args_[t.named_arg_begin + i], RenderCtx::kTop);
                        }
                        out += ">";
                    }
                    return;
                }

                case Kind::kOptional: {
                    // elem?
                    if (t.elem == kInvalidType) { out += "<invalid-elem>?"; return; }

                    const Kind ek = (t.elem < types_.size()) ? types_[t.elem].kind : Kind::kError;

                    if (needs_parens_for_suffix_(ek)) out += "(";
                    render_into_(out, t.elem, RenderCtx::kSuffixElem);
                    if (needs_parens_for_suffix_(ek)) out += ")";
                    out += "?";
                    return;
                }

                case Kind::kArray: {
                    // elem[] / elem[N]
                    if (t.elem == kInvalidType) { out += "<invalid-elem>[]"; return; }

                    const Kind ek = (t.elem < types_.size()) ? types_[t.elem].kind : Kind::kError;

                    // (T?)[] 처럼 명확히 하고 싶으면 Optional도 괄호 처리
                    const bool paren =
                        needs_parens_for_suffix_(ek) || ek == Kind::kOptional;

                    if (paren) out += "(";
                    render_into_(out, t.elem, RenderCtx::kSuffixElem);
                    if (paren) out += ")";
                    if (t.array_has_size) {
                        out += "[";
                        out += std::to_string(t.array_size);
                        out += "]";
                    } else {
                        out += "[]";
                    }
                    return;
                }

                case Kind::kBorrow: {
                    if (t.elem == kInvalidType) { out += (t.borrow_is_mut ? "&mut <invalid>" : "&<invalid>"); return; }

                    const Kind ek = (t.elem < types_.size()) ? types_[t.elem].kind : Kind::kError;
                    if (ek == Kind::kArray) {
                        const auto& arr = types_[t.elem];
                        if (!arr.array_has_size) {
                            // slice-borrow 표기: &[T] / &mut [T]
                            out += "&";
                            if (t.borrow_is_mut) out += "mut ";
                            out += "[";
                            render_into_(out, arr.elem, RenderCtx::kTop);
                            out += "]";
                            return;
                        }
                    }

                    out += "&";
                    if (t.borrow_is_mut) out += "mut ";

                    if (needs_parens_for_prefix_(ek)) out += "(";
                    render_into_(out, t.elem, RenderCtx::kTop);
                    if (needs_parens_for_prefix_(ek)) out += ")";
                    return;
                }

                case Kind::kEscape: {
                    if (t.elem == kInvalidType) { out += "^&<invalid>"; return; }

                    const Kind ek = (t.elem < types_.size()) ? types_[t.elem].kind : Kind::kError;

                    out += "^&";
                    if (needs_parens_for_prefix_(ek)) out += "(";
                    render_into_(out, t.elem, RenderCtx::kTop);
                    if (needs_parens_for_prefix_(ek)) out += ")";
                    return;
                }

                case Kind::kPtr: {
                    if (t.elem == kInvalidType) { out += (t.ptr_is_mut ? "ptr mut <invalid>" : "ptr <invalid>"); return; }
                    const Kind ek = (t.elem < types_.size()) ? types_[t.elem].kind : Kind::kError;
                    out += "ptr ";
                    if (t.ptr_is_mut) out += "mut ";
                    if (needs_parens_for_prefix_(ek)) out += "(";
                    render_into_(out, t.elem, RenderCtx::kTop);
                    if (needs_parens_for_prefix_(ek)) out += ")";
                    return;
                }

                case Kind::kFn: {
                    // (T1, T2) -> R
                    out += "def(";
                    for (uint32_t i = 0; i < t.param_count; ++i) {
                        if (i) out += ", ";
                        if (i >= t.positional_param_count) {
                            if (i == t.positional_param_count) out += "{";
                            const auto lab = fn_param_labels_[t.label_begin + i];
                            if (!lab.empty()) {
                                out.append(lab.data(), lab.size());
                                out += ": ";
                            }
                        } else {
                            const auto lab = fn_param_labels_[t.label_begin + i];
                            if (!lab.empty()) {
                                out.append(lab.data(), lab.size());
                                out += ": ";
                            }
                        }

                        const TypeId pid = fn_params_[t.param_begin + i];
                        render_into_(out, pid, RenderCtx::kFnPart);
                        if (fn_param_has_default_[t.default_begin + i]) out += "=?";
                    }
                    if (t.param_count > t.positional_param_count) out += "}";
                    out += ") -> ";
                    render_into_(out, t.ret, RenderCtx::kFnPart);
                    return;
                }
            }
        }

        TypeId error_id_ = kInvalidType;

        std::vector<Type> types_;
        std::vector<TypeId> fn_params_;
        std::vector<std::string_view> fn_param_labels_;
        std::vector<uint8_t> fn_param_has_default_;
        std::vector<TypeId> builtin_ids_;
        std::vector<std::string> user_path_segs_;
        std::vector<TypeId> named_type_args_;

        static std::string trim_copy_(std::string_view sv) {
            size_t b = 0;
            while (b < sv.size() && std::isspace(static_cast<unsigned char>(sv[b]))) ++b;
            size_t e = sv.size();
            while (e > b && std::isspace(static_cast<unsigned char>(sv[e - 1]))) --e;
            return std::string(sv.substr(b, e - b));
        }

        bool parse_generic_applied_ident_(std::string_view raw, TypeId& out) {
            out = kInvalidType;
            if (raw.empty()) return false;
            const size_t lt = raw.find('<');
            if (lt == std::string_view::npos) return false;
            if (raw.back() != '>') return false;

            int depth = 0;
            size_t first_lt = std::string_view::npos;
            size_t matching_gt = std::string_view::npos;
            for (size_t i = 0; i < raw.size(); ++i) {
                const char ch = raw[i];
                if (ch == '<') {
                    if (depth == 0) first_lt = i;
                    ++depth;
                    continue;
                }
                if (ch == '>') {
                    if (depth == 0) return false;
                    --depth;
                    if (depth == 0) matching_gt = i;
                }
            }
            if (depth != 0 || first_lt == std::string_view::npos || matching_gt != raw.size() - 1) {
                return false;
            }

            std::string base = trim_copy_(raw.substr(0, first_lt));
            if (base.empty()) return false;
            const std::string payload = trim_copy_(raw.substr(first_lt + 1, matching_gt - first_lt - 1));
            if (payload.empty()) return false;

            std::vector<std::string_view> segs{};
            {
                size_t pos = 0;
                while (pos < base.size()) {
                    size_t next = base.find("::", pos);
                    if (next == std::string::npos) next = base.size();
                    std::string_view part(base.data() + pos, next - pos);
                    if (part.empty()) return false;
                    segs.push_back(part);
                    pos = next + 2;
                }
            }
            if (segs.empty()) return false;

            std::vector<TypeId> args{};
            int arg_depth = 0;
            size_t part_begin = 0;
            for (size_t i = 0; i <= payload.size(); ++i) {
                const bool at_end = (i == payload.size());
                const char ch = at_end ? '\0' : payload[i];
                if (!at_end) {
                    if (ch == '<') ++arg_depth;
                    else if (ch == '>') --arg_depth;
                }
                if (at_end || (ch == ',' && arg_depth == 0)) {
                    std::string part = trim_copy_(std::string_view(payload).substr(part_begin, i - part_begin));
                    if (part.empty()) return false;
                    const TypeId arg_ty = intern_ident(part);
                    if (arg_ty == kInvalidType) return false;
                    args.push_back(arg_ty);
                    part_begin = i + 1;
                }
            }
            if (args.empty()) return false;

            out = intern_named_path_with_args(
                segs.data(),
                static_cast<uint32_t>(segs.size()),
                args.data(),
                static_cast<uint32_t>(args.size())
            );
            return out != kInvalidType;
        }
    };      

} // namespace parus::ty
