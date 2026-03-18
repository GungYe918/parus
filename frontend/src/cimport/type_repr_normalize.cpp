#include <parus/cimport/TypeReprNormalize.hpp>
#include <parus/cimport/TypeSemantic.hpp>

#include <parus/ast/Nodes.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/lex/Lexer.hpp>
#include <parus/parse/Parser.hpp>

#include <unordered_map>
#include <vector>

namespace parus::cimport {

    std::optional<ty::Builtin> parse_core_builtin_use_payload(std::string_view payload) {
        if (!payload.starts_with("parus_core_builtin_use|")) return std::nullopt;
        std::string_view name{};

        size_t pos = 0;
        while (pos < payload.size()) {
            size_t next = payload.find('|', pos);
            if (next == std::string_view::npos) next = payload.size();
            const std::string_view part = payload.substr(pos, next - pos);
            const size_t eq = part.find('=');
            if (eq != std::string_view::npos && eq + 1 < part.size()) {
                const std::string_view key = part.substr(0, eq);
                const std::string_view val = part.substr(eq + 1);
                if (key == "name") {
                    name = val;
                }
            }
            if (next == payload.size()) break;
            pos = next + 1;
        }

        if (name.empty()) return std::nullopt;
        ty::Builtin builtin{};
        if (!ty::TypePool::c_builtin_from_name(name, builtin)) return std::nullopt;
        return builtin;
    }

    ty::TypeId canonicalize_core_ext_type_repr(ty::TypeId t, ty::TypePool& types) {
        using TypeId = ty::TypeId;
        std::unordered_map<TypeId, TypeId> memo{};

        auto walk = [&](auto&& self, TypeId cur) -> TypeId {
            if (cur == ty::kInvalidType) return cur;
            if (auto it = memo.find(cur); it != memo.end()) return it->second;

            const auto& tt = types.get(cur);
            TypeId out = cur;

            if (tt.kind == ty::Kind::kNamedUser) {
                std::vector<std::string_view> path{};
                std::vector<TypeId> args{};
                if (types.decompose_named_user(cur, path, args) && args.empty() && !path.empty()) {
                    ty::Builtin cb{};
                    if (ty::TypePool::c_builtin_from_name(path.back(), cb)) {
                        out = types.builtin(cb);
                    }
                }
            } else if (tt.kind == ty::Kind::kOptional) {
                const TypeId elem = self(self, tt.elem);
                if (elem != tt.elem) out = types.make_optional(elem);
            } else if (tt.kind == ty::Kind::kArray) {
                const TypeId elem = self(self, tt.elem);
                if (elem != tt.elem) out = types.make_array(elem, tt.array_has_size, tt.array_size);
            } else if (tt.kind == ty::Kind::kBorrow) {
                const TypeId elem = self(self, tt.elem);
                if (elem != tt.elem) out = types.make_borrow(elem, tt.borrow_is_mut);
            } else if (tt.kind == ty::Kind::kEscape) {
                const TypeId elem = self(self, tt.elem);
                if (elem != tt.elem) out = types.make_escape(elem);
            } else if (tt.kind == ty::Kind::kPtr) {
                const TypeId elem = self(self, tt.elem);
                if (elem != tt.elem) out = types.make_ptr(elem, tt.ptr_is_mut);
            } else if (tt.kind == ty::Kind::kFn) {
                std::vector<TypeId> params{};
                std::vector<std::string_view> labels{};
                std::vector<uint8_t> defaults{};
                params.reserve(tt.param_count);
                labels.reserve(tt.param_count);
                defaults.reserve(tt.param_count);
                bool changed = false;
                for (uint32_t i = 0; i < tt.param_count; ++i) {
                    const TypeId p = types.fn_param_at(cur, i);
                    const TypeId np = self(self, p);
                    if (p != np) changed = true;
                    params.push_back(np);
                    labels.push_back(types.fn_param_label_at(cur, i));
                    defaults.push_back(types.fn_param_has_default_at(cur, i) ? 1u : 0u);
                }
                const TypeId nr = self(self, tt.ret);
                if (nr != tt.ret) changed = true;
                if (changed) {
                    out = types.make_fn(
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
            }

            memo[cur] = out;
            return out;
        };

        return walk(walk, t);
    }

    ty::TypeId parse_external_type_repr(
        std::string_view type_repr,
        std::string_view type_semantic,
        std::string_view inst_payload,
        ty::TypePool& types
    ) {
        if (const auto builtin = parse_core_builtin_use_payload(inst_payload); builtin.has_value()) {
            return types.builtin(*builtin);
        }
        if (!type_semantic.empty()) {
            TypeSemanticNode node{};
            if (parse_type_semantic(type_semantic, node)) {
                const ty::TypeId semantic_ty = build_type_from_semantic(node, types);
                if (semantic_ty != ty::kInvalidType) {
                    return canonicalize_core_ext_type_repr(semantic_ty, types);
                }
            }
        }
        if (type_repr.empty()) return ty::kInvalidType;

        diag::Bag bag{};
        Lexer lexer(type_repr, /*file_id=*/1u, &bag);
        auto toks = lexer.lex_all();
        if (bag.has_error()) return ty::kInvalidType;

        ast::AstArena ast{};
        ParserFeatureFlags flags{};
        Parser parser(toks, ast, types, &bag, /*max_errors=*/16, flags);
        ty::TypeId out = ty::kInvalidType;
        (void)parser.parse_type_full_for_macro(&out);
        if (bag.has_error()) return ty::kInvalidType;

        return canonicalize_core_ext_type_repr(out, types);
    }

} // namespace parus::cimport
