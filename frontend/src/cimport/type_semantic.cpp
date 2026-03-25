#include <parus/cimport/TypeSemantic.hpp>

#include <parus/ty/TypePool.hpp>

#include <cctype>

namespace parus::cimport {
    namespace {

        char encode_callconv_(ty::CCallConv cc) {
            switch (cc) {
                case ty::CCallConv::kCdecl: return 'c';
                case ty::CCallConv::kStdCall: return 's';
                case ty::CCallConv::kFastCall: return 'f';
                case ty::CCallConv::kVectorCall: return 'v';
                case ty::CCallConv::kWin64: return 'w';
                case ty::CCallConv::kSysV: return 'y';
                case ty::CCallConv::kDefault:
                default:
                    return 'd';
            }
        }

        ty::CCallConv decode_callconv_(char ch) {
            switch (ch) {
                case 'c': return ty::CCallConv::kCdecl;
                case 's': return ty::CCallConv::kStdCall;
                case 'f': return ty::CCallConv::kFastCall;
                case 'v': return ty::CCallConv::kVectorCall;
                case 'w': return ty::CCallConv::kWin64;
                case 'y': return ty::CCallConv::kSysV;
                case 'd':
                default:
                    return ty::CCallConv::kDefault;
            }
        }

        struct Parser {
            std::string_view text{};
            size_t pos = 0;

            bool eof() const { return pos >= text.size(); }

            bool eat(char ch) {
                if (eof() || text[pos] != ch) return false;
                ++pos;
                return true;
            }

            bool parse_u32_until_(char delim, uint32_t& out) {
                out = 0;
                if (eof() || !std::isdigit(static_cast<unsigned char>(text[pos]))) return false;
                uint64_t value = 0;
                while (!eof() && text[pos] != delim) {
                    const char ch = text[pos];
                    if (!std::isdigit(static_cast<unsigned char>(ch))) return false;
                    value = value * 10u + static_cast<uint64_t>(ch - '0');
                    if (value > 0xFFFF'FFFFull) return false;
                    ++pos;
                }
                out = static_cast<uint32_t>(value);
                return eat(delim);
            }

            bool parse_len_string_(std::string& out) {
                uint32_t len = 0;
                if (!parse_u32_until_('_', len)) return false;
                if (pos + len > text.size()) return false;
                out.assign(text.substr(pos, len));
                pos += len;
                return true;
            }

            bool parse_node(TypeSemanticNode& out) {
                if (eof()) return false;
                const char tag = text[pos++];
                switch (tag) {
                    case 'b': {
                        out = TypeSemanticNode{};
                        out.kind = TypeSemanticKind::kBuiltin;
                        return parse_len_string_(out.name);
                    }
                    case 'n': {
                        out = TypeSemanticNode{};
                        out.kind = TypeSemanticKind::kNamed;
                        if (!parse_len_string_(out.name)) return false;
                        if (!eat('[')) return true;
                        while (!eof() && text[pos] != ']') {
                            TypeSemanticNode child{};
                            if (!parse_node(child)) return false;
                            out.children.push_back(std::move(child));
                        }
                        return eat(']');
                    }
                    case 'o': {
                        out = TypeSemanticNode{};
                        out.kind = TypeSemanticKind::kOptional;
                        if (!eat('[')) return false;
                        out.children.resize(1);
                        if (!parse_node(out.children[0])) return false;
                        return eat(']');
                    }
                    case 'r': {
                        out = TypeSemanticNode{};
                        out.kind = TypeSemanticKind::kBorrow;
                        if (eof()) return false;
                        const char mut = text[pos++];
                        if (mut != '0' && mut != '1') return false;
                        out.ptr_is_mut = (mut == '1');
                        if (!eat('[')) return false;
                        out.children.resize(1);
                        if (!parse_node(out.children[0])) return false;
                        return eat(']');
                    }
                    case 'e': {
                        out = TypeSemanticNode{};
                        out.kind = TypeSemanticKind::kEscape;
                        if (!eat('[')) return false;
                        out.children.resize(1);
                        if (!parse_node(out.children[0])) return false;
                        return eat(']');
                    }
                    case 'p': {
                        out = TypeSemanticNode{};
                        out.kind = TypeSemanticKind::kPtr;
                        if (eof()) return false;
                        const char mut = text[pos++];
                        if (mut != '0' && mut != '1') return false;
                        out.ptr_is_mut = (mut == '1');
                        if (!eat('[')) return false;
                        out.children.resize(1);
                        if (!parse_node(out.children[0])) return false;
                        return eat(']');
                    }
                    case 'a': {
                        out = TypeSemanticNode{};
                        out.kind = TypeSemanticKind::kArray;
                        if (eof()) return false;
                        const char has = text[pos++];
                        if (has != '0' && has != '1') return false;
                        out.array_has_size = (has == '1');
                        if (!eat('_')) return false;
                        if (!parse_u32_until_('[', out.array_size)) return false;
                        out.children.resize(1);
                        if (!parse_node(out.children[0])) return false;
                        return eat(']');
                    }
                    case 'f': {
                        out = TypeSemanticNode{};
                        out.kind = TypeSemanticKind::kFn;
                        if (pos + 3 > text.size()) return false;
                        const char cabi = text[pos++];
                        const char variadic = text[pos++];
                        const char cc = text[pos++];
                        if ((cabi != '0' && cabi != '1') || (variadic != '0' && variadic != '1')) {
                            return false;
                        }
                        out.fn_is_c_abi = (cabi == '1');
                        out.fn_is_variadic = (variadic == '1');
                        out.fn_callconv = decode_callconv_(cc);
                        uint32_t param_count = 0;
                        if (!parse_u32_until_('_', param_count)) return false;
                        if (!eat('[')) return false;
                        out.children.resize(static_cast<size_t>(param_count) + 1u);
                        for (size_t i = 0; i < out.children.size(); ++i) {
                            if (!parse_node(out.children[i])) return false;
                        }
                        return eat(']');
                    }
                    default:
                        return false;
                }
            }
        };

        void serialize_into_(std::string& out, const TypeSemanticNode& node) {
            switch (node.kind) {
                case TypeSemanticKind::kBuiltin:
                    out.push_back('b');
                    out += std::to_string(node.name.size());
                    out.push_back('_');
                    out += node.name;
                    return;
                case TypeSemanticKind::kNamed:
                    out.push_back('n');
                    out += std::to_string(node.name.size());
                    out.push_back('_');
                    out += node.name;
                    if (!node.children.empty()) {
                        out.push_back('[');
                        for (const auto& child : node.children) serialize_into_(out, child);
                        out.push_back(']');
                    }
                    return;
                case TypeSemanticKind::kOptional:
                    out.push_back('o');
                    out.push_back('[');
                    if (!node.children.empty()) serialize_into_(out, node.children[0]);
                    out.push_back(']');
                    return;
                case TypeSemanticKind::kBorrow:
                    out.push_back('r');
                    out.push_back(node.ptr_is_mut ? '1' : '0');
                    out.push_back('[');
                    if (!node.children.empty()) serialize_into_(out, node.children[0]);
                    out.push_back(']');
                    return;
                case TypeSemanticKind::kEscape:
                    out.push_back('e');
                    out.push_back('[');
                    if (!node.children.empty()) serialize_into_(out, node.children[0]);
                    out.push_back(']');
                    return;
                case TypeSemanticKind::kPtr:
                    out.push_back('p');
                    out.push_back(node.ptr_is_mut ? '1' : '0');
                    out.push_back('[');
                    if (!node.children.empty()) serialize_into_(out, node.children[0]);
                    out.push_back(']');
                    return;
                case TypeSemanticKind::kArray:
                    out.push_back('a');
                    out.push_back(node.array_has_size ? '1' : '0');
                    out.push_back('_');
                    out += std::to_string(node.array_size);
                    out.push_back('[');
                    if (!node.children.empty()) serialize_into_(out, node.children[0]);
                    out.push_back(']');
                    return;
                case TypeSemanticKind::kFn:
                    out.push_back('f');
                    out.push_back(node.fn_is_c_abi ? '1' : '0');
                    out.push_back(node.fn_is_variadic ? '1' : '0');
                    out.push_back(encode_callconv_(node.fn_callconv));
                    out += std::to_string(node.children.empty() ? 0u
                        : static_cast<uint32_t>(node.children.size() - 1u));
                    out.push_back('_');
                    out.push_back('[');
                    for (const auto& child : node.children) serialize_into_(out, child);
                    out.push_back(']');
                    return;
            }
        }

        void rewrite_node_(TypeSemanticNode& node,
                           std::string_view alias,
                           const std::unordered_set<std::string>& known_type_names) {
            if (node.kind == TypeSemanticKind::kNamed) {
                const bool qualified = node.name.find("::") != std::string::npos;
                if (!qualified &&
                    !alias.empty() &&
                    known_type_names.find(node.name) != known_type_names.end()) {
                    node.name = std::string(alias) + "::" + node.name;
                }
            }
            for (auto& child : node.children) rewrite_node_(child, alias, known_type_names);
        }

        ty::TypeId intern_named_(std::string_view name, ty::TypePool& types) {
            if (name.empty()) return ty::kInvalidType;
            ty::Builtin builtin{};
            if (ty::TypePool::builtin_from_name(name, builtin) ||
                ty::TypePool::c_builtin_from_name(name, builtin)) {
                return types.builtin(builtin);
            }

            std::vector<std::string_view> segs{};
            size_t begin = 0;
            while (begin < name.size()) {
                const size_t split = name.find("::", begin);
                const size_t end = (split == std::string_view::npos) ? name.size() : split;
                const std::string_view seg = name.substr(begin, end - begin);
                if (seg.empty()) return ty::kInvalidType;
                segs.push_back(seg);
                if (split == std::string_view::npos) break;
                begin = split + 2;
            }
            if (segs.empty()) return ty::kInvalidType;
            return types.intern_path(segs.data(), static_cast<uint32_t>(segs.size()));
        }

        bool split_named_path_(std::string_view name, std::vector<std::string_view>& out_segs) {
            out_segs.clear();
            if (name.empty()) return false;
            size_t begin = 0;
            while (begin < name.size()) {
                const size_t split = name.find("::", begin);
                const size_t end = (split == std::string_view::npos) ? name.size() : split;
                const std::string_view seg = name.substr(begin, end - begin);
                if (seg.empty()) return false;
                out_segs.push_back(seg);
                if (split == std::string_view::npos) break;
                begin = split + 2;
            }
            return !out_segs.empty();
        }

        ty::TypeId build_node_(const TypeSemanticNode& node, ty::TypePool& types) {
            switch (node.kind) {
                case TypeSemanticKind::kBuiltin:
                case TypeSemanticKind::kNamed:
                    if (node.kind == TypeSemanticKind::kBuiltin) {
                        return intern_named_(node.name, types);
                    }
                    if (node.children.empty()) {
                        return intern_named_(node.name, types);
                    }
                    {
                        std::vector<std::string_view> segs{};
                        if (!split_named_path_(node.name, segs)) return ty::kInvalidType;
                        std::vector<ty::TypeId> args{};
                        args.reserve(node.children.size());
                        for (const auto& child : node.children) {
                            const ty::TypeId arg = build_node_(child, types);
                            if (arg == ty::kInvalidType) return ty::kInvalidType;
                            args.push_back(arg);
                        }
                        return types.intern_named_path_with_args(
                            segs.data(),
                            static_cast<uint32_t>(segs.size()),
                            args.data(),
                            static_cast<uint32_t>(args.size())
                        );
                    }
                case TypeSemanticKind::kOptional:
                    if (node.children.size() != 1u) return ty::kInvalidType;
                    return types.make_optional(build_node_(node.children[0], types));
                case TypeSemanticKind::kBorrow:
                    if (node.children.size() != 1u) return ty::kInvalidType;
                    return types.make_borrow(build_node_(node.children[0], types), node.ptr_is_mut);
                case TypeSemanticKind::kEscape:
                    if (node.children.size() != 1u) return ty::kInvalidType;
                    return types.make_escape(build_node_(node.children[0], types));
                case TypeSemanticKind::kPtr:
                    if (node.children.size() != 1u) return ty::kInvalidType;
                    return types.make_ptr(build_node_(node.children[0], types), node.ptr_is_mut);
                case TypeSemanticKind::kArray:
                    if (node.children.size() != 1u) return ty::kInvalidType;
                    return types.make_array(
                        build_node_(node.children[0], types),
                        node.array_has_size,
                        node.array_size
                    );
                case TypeSemanticKind::kFn: {
                    if (node.children.empty()) return ty::kInvalidType;
                    const ty::TypeId ret = build_node_(node.children[0], types);
                    std::vector<ty::TypeId> params{};
                    params.reserve(node.children.size() - 1u);
                    for (size_t i = 1; i < node.children.size(); ++i) {
                        params.push_back(build_node_(node.children[i], types));
                    }
                    return types.make_fn(
                        ret,
                        params.empty() ? nullptr : params.data(),
                        static_cast<uint32_t>(params.size()),
                        static_cast<uint32_t>(params.size()),
                        nullptr,
                        nullptr,
                        node.fn_is_c_abi,
                        node.fn_is_variadic,
                        node.fn_callconv
                    );
                }
            }
            return ty::kInvalidType;
        }

    } // namespace

    bool parse_type_semantic(std::string_view text, TypeSemanticNode& out) {
        Parser p{ text, 0 };
        if (!p.parse_node(out)) return false;
        return p.pos == text.size();
    }

    std::string serialize_type_semantic(const TypeSemanticNode& node) {
        std::string out{};
        serialize_into_(out, node);
        return out;
    }

    std::string rewrite_type_semantic_with_alias(
        std::string_view text,
        std::string_view alias,
        const std::unordered_set<std::string>& known_type_names
    ) {
        if (text.empty() || alias.empty() || known_type_names.empty()) return std::string(text);
        TypeSemanticNode node{};
        if (!parse_type_semantic(text, node)) return std::string(text);
        rewrite_node_(node, alias, known_type_names);
        return serialize_type_semantic(node);
    }

    ty::TypeId build_type_from_semantic(const TypeSemanticNode& node, ty::TypePool& types) {
        return build_node_(node, types);
    }

    bool build_type_semantic_from_type(ty::TypeId type, const ty::TypePool& types, TypeSemanticNode& out) {
        if (type == ty::kInvalidType || type >= types.count()) return false;
        const auto& tt = types.get(type);
        out = {};
        switch (tt.kind) {
            case ty::Kind::kBuiltin:
                out.kind = TypeSemanticKind::kBuiltin;
                out.name = std::string(types.to_string(type));
                return true;
            case ty::Kind::kNamedUser:
                out.kind = TypeSemanticKind::kNamed;
                {
                    std::vector<std::string_view> path{};
                    std::vector<ty::TypeId> args{};
                    if (!types.decompose_named_user(type, path, args) || path.empty()) {
                        return false;
                    }
                    for (size_t i = 0; i < path.size(); ++i) {
                        if (i) out.name += "::";
                        out.name += std::string(path[i]);
                    }
                    out.children.resize(args.size());
                    for (size_t i = 0; i < args.size(); ++i) {
                        if (!build_type_semantic_from_type(args[i], types, out.children[i])) {
                            return false;
                        }
                    }
                }
                return true;
            case ty::Kind::kOptional:
                out.kind = TypeSemanticKind::kOptional;
                out.children.resize(1);
                return build_type_semantic_from_type(tt.elem, types, out.children[0]);
            case ty::Kind::kBorrow:
                out.kind = TypeSemanticKind::kBorrow;
                out.ptr_is_mut = tt.borrow_is_mut;
                out.children.resize(1);
                return build_type_semantic_from_type(tt.elem, types, out.children[0]);
            case ty::Kind::kEscape:
                out.kind = TypeSemanticKind::kEscape;
                out.children.resize(1);
                return build_type_semantic_from_type(tt.elem, types, out.children[0]);
            case ty::Kind::kPtr:
                out.kind = TypeSemanticKind::kPtr;
                out.ptr_is_mut = tt.ptr_is_mut;
                out.children.resize(1);
                return build_type_semantic_from_type(tt.elem, types, out.children[0]);
            case ty::Kind::kArray:
                out.kind = TypeSemanticKind::kArray;
                out.array_has_size = tt.array_has_size;
                out.array_size = tt.array_size;
                out.children.resize(1);
                return build_type_semantic_from_type(tt.elem, types, out.children[0]);
            case ty::Kind::kFn: {
                out.kind = TypeSemanticKind::kFn;
                out.fn_is_c_abi = tt.fn_is_c_abi;
                out.fn_is_variadic = tt.fn_is_c_variadic;
                out.fn_callconv = tt.fn_callconv;
                out.children.resize(static_cast<size_t>(tt.param_count) + 1u);
                if (!build_type_semantic_from_type(tt.ret, types, out.children[0])) return false;
                for (uint32_t i = 0; i < tt.param_count; ++i) {
                    if (!build_type_semantic_from_type(types.fn_param_at(type, i), types, out.children[static_cast<size_t>(i) + 1u])) {
                        return false;
                    }
                }
                return true;
            }
            default:
                return false;
        }
    }

    std::string serialize_type_semantic_from_type(ty::TypeId type, const ty::TypePool& types) {
        TypeSemanticNode node{};
        if (!build_type_semantic_from_type(type, types, node)) return {};
        return serialize_type_semantic(node);
    }

} // namespace parus::cimport
