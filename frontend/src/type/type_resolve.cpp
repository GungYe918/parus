#include <parus/type/TypeResolve.hpp>

#include <parus/diag/DiagCode.hpp>

#include <string>
#include <vector>

namespace parus::type {

    namespace {
        static void add_diag_(diag::Bag& diags, diag::Code code, Span span, std::string_view a0 = {}) {
            diag::Diagnostic d(diag::Severity::kError, code, span);
            if (!a0.empty()) d.add_arg(a0);
            diags.add(std::move(d));
        }

        struct Resolver {
            ast::AstArena& ast;
            ty::TypePool& types;
            diag::Bag& diags;
            TypeResolveResult res;

            std::vector<uint8_t> visiting{};
            std::vector<uint8_t> done{};

            ty::TypeId resolve_node(ast::TypeNodeId id) {
                if (id == ast::k_invalid_type_node) return ty::kInvalidType;
                if (id >= ast.type_nodes().size()) return types.error();

                if (done[id]) return ast.type_node(id).resolved_type;
                if (visiting[id]) return types.error();
                visiting[id] = 1;

                auto& n = ast.type_node_mut(id);
                ty::TypeId out = types.error();
                switch (n.kind) {
                    case ast::TypeNodeKind::kError:
                        out = types.error();
                        break;

                    case ast::TypeNodeKind::kNamedPath: {
                        if (n.path_count == 0) {
                            out = types.error();
                            break;
                        }
                        const auto& segs = ast.path_segs();
                        if (n.path_begin >= segs.size() || n.path_begin + n.path_count > segs.size()) {
                            out = types.error();
                            break;
                        }
                        if (n.generic_arg_count == 0) {
                            out = types.intern_path(&segs[n.path_begin], n.path_count);
                            break;
                        }

                        const auto& children = ast.type_node_children();
                        if (n.generic_arg_begin + n.generic_arg_count > children.size()) {
                            out = types.error();
                            break;
                        }

                        std::string flat{};
                        for (uint32_t i = 0; i < n.path_count; ++i) {
                            if (i) flat += "::";
                            flat += std::string(segs[n.path_begin + i]);
                        }
                        flat += "<";
                        for (uint32_t i = 0; i < n.generic_arg_count; ++i) {
                            if (i) flat += ",";
                            const auto aid = resolve_node(children[n.generic_arg_begin + i]);
                            flat += types.to_string(aid);
                        }
                        flat += ">";
                        out = types.intern_ident(ast.add_owned_string(std::move(flat)));
                        break;
                    }

                    case ast::TypeNodeKind::kOptional: {
                        const auto e = resolve_node(n.elem);
                        out = (e == ty::kInvalidType) ? types.error() : types.make_optional(e);
                        break;
                    }

                    case ast::TypeNodeKind::kArray: {
                        const auto e = resolve_node(n.elem);
                        out = (e == ty::kInvalidType)
                            ? types.error()
                            : types.make_array(e, n.array_has_size, n.array_size);
                        break;
                    }

                    case ast::TypeNodeKind::kBorrow: {
                        const auto e = resolve_node(n.elem);
                        out = (e == ty::kInvalidType) ? types.error() : types.make_borrow(e, n.is_mut);
                        break;
                    }

                    case ast::TypeNodeKind::kEscape: {
                        const auto e = resolve_node(n.elem);
                        out = (e == ty::kInvalidType) ? types.error() : types.make_escape(e);
                        break;
                    }

                    case ast::TypeNodeKind::kPtr: {
                        const auto e = resolve_node(n.elem);
                        out = (e == ty::kInvalidType) ? types.error() : types.make_ptr(e, n.is_mut);
                        break;
                    }

                    case ast::TypeNodeKind::kFn: {
                        const auto ret = resolve_node(n.fn_ret);
                        std::vector<ty::TypeId> params{};
                        const auto& children = ast.type_node_children();
                        for (uint32_t i = 0; i < n.fn_param_count; ++i) {
                            const auto ci = n.fn_param_begin + i;
                            if (ci >= children.size()) break;
                            params.push_back(resolve_node(children[ci]));
                        }
                        out = types.make_fn(
                            ret,
                            params.empty() ? nullptr : params.data(),
                            static_cast<uint32_t>(params.size())
                        );
                        break;
                    }

                    case ast::TypeNodeKind::kMacroCall:
                        add_diag_(diags, diag::Code::kMacroReparseFail, n.span, "type macro call");
                        out = types.error();
                        break;
                }

                n.resolved_type = out;
                res.node_types[id] = out;
                visiting[id] = 0;
                done[id] = 1;
                return out;
            }

            void apply_backpatch() {
                for (auto& p : ast.params_mut()) {
                    if (p.type_node != ast::k_invalid_type_node) {
                        p.type = resolve_node(p.type_node);
                    }
                }
                for (auto& f : ast.field_members_mut()) {
                    if (f.type_node != ast::k_invalid_type_node) {
                        f.type = resolve_node(f.type_node);
                    }
                }
                for (auto& e : ast.exprs_mut()) {
                    if (e.cast_type_node != ast::k_invalid_type_node) {
                        e.cast_type = resolve_node(e.cast_type_node);
                    }
                }
                for (auto& s : ast.stmts_mut()) {
                    if (s.type_node != ast::k_invalid_type_node) {
                        s.type = resolve_node(s.type_node);
                    }
                    if (s.fn_ret_type_node != ast::k_invalid_type_node) {
                        s.fn_ret = resolve_node(s.fn_ret_type_node);
                    }
                    if (s.acts_target_type_node != ast::k_invalid_type_node) {
                        s.acts_target_type = resolve_node(s.acts_target_type_node);
                    }
                    if (s.var_acts_target_type_node != ast::k_invalid_type_node) {
                        s.var_acts_target_type = resolve_node(s.var_acts_target_type_node);
                    }
                }
            }
        };
    } // namespace

    TypeResolveResult resolve_program_types(
        ast::AstArena& ast,
        ty::TypePool& types,
        ast::StmtId root,
        diag::Bag& diags
    ) {
        (void)root;

        Resolver r{ast, types, diags, TypeResolveResult{}, {}, {}};
        r.res.node_types.assign(ast.type_nodes().size(), ty::kInvalidType);
        r.visiting.assign(ast.type_nodes().size(), 0);
        r.done.assign(ast.type_nodes().size(), 0);

        r.apply_backpatch();
        r.res.ok = !diags.has_error();
        return r.res;
    }

} // namespace parus::type
