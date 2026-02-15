// compiler/include/gaupel/ty/TypePool.hpp
#pragma once
#include <gaupel/ty/Type.hpp>
#include <string_view>
#include <vector>
#include <string>
#include <ostream>


namespace gaupel::ty {

    class TypePool {
    public:
        TypePool() {
            // reserve: 128
            types_.reserve(128);
            fn_params_.reserve(256);
            user_path_segs_.reserve(256);

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

        // ---- user-defined named type (path) interning ----
        // Stores path segments as a slice into user_path_segs_ to avoid string flatten.
        //
        // Example: Foo::Bar::Baz is stored as segs ["Foo","Bar","Baz"].
        TypeId make_named_user_path(const std::string_view* segs, uint32_t seg_count) {
            if (!segs || seg_count == 0) {
                Type t{};
                t.kind = Kind::kNamedUser;
                t.path_begin = 0;
                t.path_count = 0;
                return push_(t);
            }

            // linear search v0: compare segment slices
            for (TypeId i = 0; i < (TypeId)types_.size(); ++i) {
                const auto& t = types_[i];
                if (t.kind != Kind::kNamedUser) continue;
                if (t.path_count != seg_count) continue;

                bool same = true;
                for (uint32_t k = 0; k < seg_count; ++k) {
                    if (user_path_segs_[t.path_begin + k] != segs[k]) { same = false; break; }
                }
                if (same) return i;
            }

            Type t{};
            t.kind = Kind::kNamedUser;
            t.path_begin = (uint32_t)user_path_segs_.size();
            t.path_count = seg_count;

            for (uint32_t k = 0; k < seg_count; ++k) user_path_segs_.push_back(segs[k]);

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

        // ---- function signature type interning ----
        TypeId make_fn(TypeId ret, const TypeId* params, uint32_t param_count) {
            // linear search v0 (ok)
            for (TypeId i = 0; i < (TypeId)types_.size(); ++i) {
                const auto& t = types_[i];
                if (t.kind != Kind::kFn) continue;
                if (t.ret != ret) continue;
                if (t.param_count != param_count) continue;

                bool same = true;
                for (uint32_t k = 0; k < param_count; ++k) {
                    if (fn_params_[t.param_begin + k] != params[k]) { same = false; break; }
                }
                if (same) return i;
            }

            Type t{};
            t.kind = Kind::kFn;
            t.ret = ret;
            t.param_begin = (uint32_t)fn_params_.size();
            t.param_count = param_count;

            for (uint32_t k = 0; k < param_count; ++k) fn_params_.push_back(params[k]);

            return push_(t);
        }

        // ---- fn signature introspection ----
        bool is_fn(TypeId id) const {
            return id != kInvalidType && id < types_.size() && types_[id].kind == Kind::kFn;
        }

        TypeId fn_param_at(TypeId fn, uint32_t i) const {
            if (!is_fn(fn)) return error();
            const Type& t = types_[fn];
            if (i >= t.param_count) return error();
            return fn_params_[t.param_begin + i];
        }

        // convenience: ident -> (builtin or named_user)
        TypeId intern_ident(std::string_view name) {
            const std::string_view segs[1] = { name };
            return intern_path(segs, 1);
        }

        // builtin name -> Builtin (aliases 포함)
        static bool builtin_from_name(std::string_view name, Builtin& out) {
            // exact
            if (name == "null")   { out = Builtin::kNull; return true; }

            if (name == "void")  { out = Builtin::kUnit; return true; }
            if (name == "never") { out = Builtin::kNever; return true; }

            if (name == "bool")   { out = Builtin::kBool; return true; }
            if (name == "char")   { out = Builtin::kChar; return true; }

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

                case Builtin::kInferInteger: return "{integer}";
            }
            return "<builtin?>";
        }

        std::string to_string(TypeId id) const {
            std::string out;
            render_into_(out, id, /*parent_ctx=*/RenderCtx::kTop);
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
                        os << ")";
                        break;
                    }
                    case Kind::kBorrow:
                        os << "(Borrow mut=" << (t.borrow_is_mut ? 1 : 0) << " elem=" << t.elem << ")";
                        break;
                    case Kind::kEscape:
                        os << "(Escape elem=" << t.elem << ")";
                        break;
                    case Kind::kFn:
                        os << "(Fn ret=" << t.ret
                           << " params=[" << t.param_begin
                           << ".." << (t.param_begin + t.param_count) << "])";
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
                    if (t.elem == kInvalidType) { out += "&&<invalid>"; return; }

                    const Kind ek = (t.elem < types_.size()) ? types_[t.elem].kind : Kind::kError;

                    out += "&&";
                    if (needs_parens_for_prefix_(ek)) out += "(";
                    render_into_(out, t.elem, RenderCtx::kTop);
                    if (needs_parens_for_prefix_(ek)) out += ")";
                    return;
                }

                case Kind::kFn: {
                    // (T1, T2) -> R
                    out += "fn(";
                    for (uint32_t i = 0; i < t.param_count; ++i) {
                        if (i) out += ", ";
                        const TypeId pid = fn_params_[t.param_begin + i];
                        render_into_(out, pid, RenderCtx::kFnPart);
                    }
                    out += ") -> ";
                    render_into_(out, t.ret, RenderCtx::kFnPart);
                    return;
                }
            }
        }

        TypeId error_id_ = kInvalidType;

        std::vector<Type> types_;
        std::vector<TypeId> fn_params_;
        std::vector<TypeId> builtin_ids_;
        std::vector<std::string_view> user_path_segs_;
    };      

} // namespace gaupel::ty
