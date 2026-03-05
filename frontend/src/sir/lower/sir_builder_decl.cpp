// frontend/src/sir/sir_builder_decl.cpp
#include <parus/sir/Builder.hpp>
#include "sir_builder_internal.hpp"
#include <unordered_set>


namespace parus::sir::detail {

    FnMode lower_fn_mode(parus::ast::FnMode m) {
        switch (m) {
            case parus::ast::FnMode::kPub: return FnMode::kPub;
            case parus::ast::FnMode::kSub: return FnMode::kSub;
            case parus::ast::FnMode::kNone:
            default: return FnMode::kNone;
        }
    }

    FieldLayout lower_field_layout(parus::ast::FieldLayout l) {
        switch (l) {
            case parus::ast::FieldLayout::kC:
                return FieldLayout::kC;
            case parus::ast::FieldLayout::kNone:
            default:
                return FieldLayout::kNone;
        }
    }

    /// @brief AST 함수 선언 1개를 SIR Func로 lower하여 모듈에 추가한다.
    FuncId lower_func_decl_(
        Module& m,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        parus::ast::StmtId sid,
        bool is_acts_member,
        ActsId owner_acts
    ) {
        const auto& s = ast.stmt(sid);
        if (s.kind != ast::StmtKind::kFnDecl) {
            return k_invalid_func;
        }

        Func f{};
        f.span = s.span;

        // signature & ret
        f.sig = s.type;
        f.ret = s.fn_ret;

        // decl symbol (def name)
        f.sym = resolve_symbol_from_stmt(nres, sid);

        // nested namespace를 통과한 내부 함수는 qualified symbol name을 사용한다.
        // C ABI 함수는 선언 원문 이름을 그대로 유지한다(no-mangle).
        if (s.link_abi == parus::ast::LinkAbi::kC) {
            f.name = s.name;
        } else {
            auto qit = tyck.fn_qualified_names.find(sid);
            if (qit != tyck.fn_qualified_names.end()) {
                f.name = std::string_view(qit->second);
            } else if (f.sym != k_invalid_symbol && (size_t)f.sym < sym.symbols().size()) {
                f.name = sym.symbol(f.sym).name;
            } else {
                f.name = s.name;
            }
        }

        // qualifiers / mode
        f.is_export = s.is_export;
        f.is_extern = s.is_extern;
        f.fn_mode = lower_fn_mode(s.fn_mode);
        f.abi = (s.link_abi == parus::ast::LinkAbi::kC) ? FuncAbi::kC : FuncAbi::kParus;

        f.is_pure = s.is_pure;
        f.is_comptime = s.is_comptime;
        f.is_commit = s.is_commit;
        f.is_recast = s.is_recast;
        f.is_throwing = s.is_throwing;

        f.positional_param_count = s.positional_param_count;
        f.has_named_group = s.has_named_group;
        f.is_acts_member = is_acts_member;
        f.owner_acts = owner_acts;

        // attrs slice
        f.attr_begin = (uint32_t)m.attrs.size();
        f.attr_count = 0;
        for (uint32_t ai = 0; ai < s.attr_count; ++ai) {
            const auto& aa = ast.fn_attrs()[s.attr_begin + ai];
            Attr sa{};
            sa.name = aa.name;
            sa.span = aa.span;
            m.add_attr(sa);
            f.attr_count++;
        }

        // params slice
        f.param_begin = (uint32_t)m.params.size();
        f.param_count = 0;

        bool has_any_write = false;
        for (uint32_t pi = 0; pi < s.param_count; ++pi) {
            const uint32_t param_index = s.param_begin + pi;
            const auto& p = ast.params()[param_index];

            Param sp{};
            sp.name = p.name;
            sp.type = p.type;
            sp.is_mut = p.is_mut;
            sp.is_named_group = p.is_named_group;
            sp.span = p.span;

            sp.has_default = p.has_default;
            if (p.has_default && p.default_expr != ast::k_invalid_expr) {
                sp.default_value = lower_expr(m, has_any_write, ast, sym, nres, tyck, p.default_expr);
            }

            // param SymbolId binding
            sp.sym = resolve_symbol_from_param_index(nres, tyck, param_index);

            m.add_param(sp);
            f.param_count++;
        }

        // body
        if (s.a != ast::k_invalid_stmt) {
            f.entry = lower_block_stmt(m, has_any_write, ast, sym, nres, tyck, s.a);
        }
        f.origin_stmt = sid;

        f.has_any_write = has_any_write;
        return m.add_func(f);
    }

    /// @brief AST field 선언을 SIR field 메타로 lower한다.
    FieldId lower_field_decl_(
        Module& m,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        parus::ast::StmtId sid
    ) {
        const auto& s = ast.stmt(sid);
        if (s.kind != ast::StmtKind::kFieldDecl) {
            return k_invalid_field;
        }

        FieldDecl f{};
        f.span = s.span;
        f.is_export = s.is_export;
        f.sym = resolve_symbol_from_stmt(nres, sid);
        if (f.sym != k_invalid_symbol && (size_t)f.sym < sym.symbols().size()) {
            f.name = sym.symbol(f.sym).name;
        } else {
            f.name = s.name;
        }
        f.layout = lower_field_layout(s.field_layout);
        f.align = s.field_align;
        f.self_type = s.type;

        f.member_begin = (uint32_t)m.field_members.size();
        f.member_count = 0;

        const uint32_t begin = s.field_member_begin;
        const uint32_t end = s.field_member_begin + s.field_member_count;
        if (begin < ast.field_members().size() && end <= ast.field_members().size()) {
            for (uint32_t i = begin; i < end; ++i) {
                const auto& am = ast.field_members()[i];
                FieldMember sm{};
                sm.name = am.name;
                sm.type = am.type;
                sm.span = am.span;
                m.add_field_member(sm);
                f.member_count++;
            }
        }

        return m.add_field(f);
    }

    /// @brief AST class 선언의 인스턴스 필드 메타를 SIR field 메타로 lower한다.
    FieldId lower_class_field_decl_(
        Module& m,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        parus::ast::StmtId sid
    ) {
        const auto& s = ast.stmt(sid);
        if (s.kind != ast::StmtKind::kClassDecl) {
            return k_invalid_field;
        }
        if (s.field_member_count == 0) {
            return k_invalid_field;
        }

        FieldDecl f{};
        f.span = s.span;
        f.is_export = s.is_export;
        f.sym = resolve_symbol_from_stmt(nres, sid);
        if (f.sym != k_invalid_symbol && (size_t)f.sym < sym.symbols().size()) {
            f.name = sym.symbol(f.sym).name;
        } else {
            f.name = s.name;
        }
        f.layout = FieldLayout::kNone;
        f.align = 0;
        f.self_type = s.type;

        f.member_begin = (uint32_t)m.field_members.size();
        f.member_count = 0;

        const uint32_t begin = s.field_member_begin;
        const uint32_t end = s.field_member_begin + s.field_member_count;
        if (begin <= ast.field_members().size() && end <= ast.field_members().size()) {
            for (uint32_t i = begin; i < end; ++i) {
                const auto& am = ast.field_members()[i];
                FieldMember sm{};
                sm.name = am.name;
                sm.type = am.type;
                sm.span = am.span;
                m.add_field_member(sm);
                f.member_count++;
            }
        }

        if (f.member_count == 0) {
            return k_invalid_field;
        }
        return m.add_field(f);
    }

    /// @brief AST actor 선언의 draft 필드 메타를 SIR field 메타로 lower한다.
    FieldId lower_actor_field_decl_(
        Module& m,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        parus::ast::StmtId sid
    ) {
        const auto& s = ast.stmt(sid);
        if (s.kind != ast::StmtKind::kActorDecl) {
            return k_invalid_field;
        }
        if (s.field_member_count == 0) {
            return k_invalid_field;
        }

        FieldDecl f{};
        f.span = s.span;
        f.is_export = s.is_export;
        f.sym = resolve_symbol_from_stmt(nres, sid);
        if (f.sym != k_invalid_symbol && (size_t)f.sym < sym.symbols().size()) {
            f.name = sym.symbol(f.sym).name;
        } else {
            f.name = s.name;
        }
        f.layout = FieldLayout::kNone;
        f.align = 0;
        f.self_type = s.type;

        f.member_begin = (uint32_t)m.field_members.size();
        f.member_count = 0;

        const uint32_t begin = s.field_member_begin;
        const uint32_t end = s.field_member_begin + s.field_member_count;
        if (begin <= ast.field_members().size() && end <= ast.field_members().size()) {
            for (uint32_t i = begin; i < end; ++i) {
                const auto& am = ast.field_members()[i];
                FieldMember sm{};
                sm.name = am.name;
                sm.type = am.type;
                sm.span = am.span;
                m.add_field_member(sm);
                f.member_count++;
            }
        }

        if (f.member_count == 0) {
            return k_invalid_field;
        }
        return m.add_field(f);
    }

    /// @brief AST enum 선언의 내부 레이아웃 메타를 SIR field 메타로 lower한다.
    FieldId lower_enum_field_decl_(
        Module& m,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const parus::ty::TypePool& types,
        parus::ast::StmtId sid
    ) {
        const auto& s = ast.stmt(sid);
        if (s.kind != ast::StmtKind::kEnumDecl) {
            return k_invalid_field;
        }
        auto& ast_mut = const_cast<parus::ast::AstArena&>(ast);

        FieldDecl f{};
        f.span = s.span;
        f.is_export = s.is_export;
        f.sym = resolve_symbol_from_stmt(nres, sid);
        if (f.sym != k_invalid_symbol && (size_t)f.sym < sym.symbols().size()) {
            f.name = sym.symbol(f.sym).name;
        } else {
            f.name = s.name;
        }
        f.layout = lower_field_layout(s.field_layout);
        f.align = s.field_align;
        f.self_type = s.type;
        f.member_begin = (uint32_t)m.field_members.size();
        f.member_count = 0;

        // Every enum uses an internal tag lane.
        FieldMember tag{};
        tag.name = "__tag";
        tag.type = types.builtin(parus::ty::Builtin::kI32);
        tag.span = s.span;
        m.add_field_member(tag);
        f.member_count++;

        const uint64_t vb = s.enum_variant_begin;
        const uint64_t ve = vb + s.enum_variant_count;
        if (vb <= ast.enum_variant_decls().size() && ve <= ast.enum_variant_decls().size()) {
            for (uint32_t vi = 0; vi < s.enum_variant_count; ++vi) {
                const auto& v = ast.enum_variant_decls()[s.enum_variant_begin + vi];
                const uint64_t pb = v.payload_begin;
                const uint64_t pe = pb + v.payload_count;
                if (pb > ast.field_members().size() || pe > ast.field_members().size()) continue;
                for (uint32_t mi = v.payload_begin; mi < v.payload_begin + v.payload_count; ++mi) {
                    const auto& am = ast.field_members()[mi];
                    FieldMember sm{};
                    // Unique per-variant storage lane name.
                    sm.name = ast_mut.add_owned_string(
                        std::string("__v") + std::to_string(vi) + "_" + std::string(am.name));
                    sm.type = am.type;
                    sm.span = am.span;
                    m.add_field_member(sm);
                    f.member_count++;
                }
            }
        }

        return m.add_field(f);
    }

    /// @brief AST var 선언 1개를 SIR global 메타로 lower한다.
    void lower_global_var_decl_(
        Module& m,
        bool& out_has_any_write,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        parus::ast::StmtId sid,
        std::string_view forced_name = {},
        SymbolId forced_sym = k_invalid_symbol
    ) {
        const auto& s = ast.stmt(sid);
        if (s.kind != ast::StmtKind::kVar) return;

        GlobalVarDecl g{};
        g.span = s.span;
        g.sym = (forced_sym != k_invalid_symbol) ? forced_sym : resolve_symbol_from_stmt(nres, sid);
        g.is_set = s.is_set;
        g.is_mut = s.is_mut;
        g.is_static = s.is_static;
        g.is_export = s.is_export;
        g.is_extern = s.is_extern;
        g.abi = (s.link_abi == parus::ast::LinkAbi::kC) ? FuncAbi::kC : FuncAbi::kParus;

        if (g.abi == FuncAbi::kC) {
            g.name = s.name;
        } else if (!forced_name.empty()) {
            g.name = forced_name;
        } else if (g.sym != k_invalid_symbol && (size_t)g.sym < sym.symbols().size()) {
            g.name = sym.symbol(g.sym).name;
        } else {
            g.name = s.name;
        }

        g.declared_type = resolve_decl_type_from_symbol_uses(nres, tyck, g.sym);
        if (g.declared_type == k_invalid_type) {
            g.declared_type = s.type;
        }
        if (g.declared_type == k_invalid_type && s.init != ast::k_invalid_expr) {
            g.declared_type = type_of_ast_expr(tyck, s.init);
        }
        if (g.declared_type == k_invalid_type &&
            g.sym != k_invalid_symbol &&
            (size_t)g.sym < sym.symbols().size()) {
            g.declared_type = sym.symbol(g.sym).declared_type;
        }

        if (s.init != ast::k_invalid_expr) {
            g.init = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.init);
        }

        (void)m.add_global(g);
    }

} // namespace parus::sir::detail

namespace parus::sir {

    using namespace detail;

    Module build_sir_module(
        const ast::AstArena& ast,
        ast::StmtId program_root,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        const ty::TypePool& types,
        const BuildOptions& opt
    ) {
        (void)sym;
        (void)types;

        Module m{};
        m.bundle_enabled = opt.bundle_enabled;
        m.bundle_name = opt.bundle_name;
        m.current_source_norm = opt.current_source_norm;
        m.bundle_sources_norm = opt.bundle_sources_norm;
        bool global_init_has_any_write = false;
        std::unordered_set<ast::StmtId> lowered_fn_stmt_ids;
        std::unordered_set<ast::StmtId> lowered_decl_stmt_ids;
        std::unordered_set<ast::StmtId> generic_acts_template_sid_set(
            tyck.generic_acts_template_sids.begin(),
            tyck.generic_acts_template_sids.end()
        );

        const auto lower_fn_once = [&](ast::StmtId fn_sid,
                                       bool is_acts_member,
                                       ActsId owner_acts) -> FuncId {
            if (fn_sid == ast::k_invalid_stmt || (size_t)fn_sid >= ast.stmts().size()) {
                return k_invalid_func;
            }
            if (!lowered_fn_stmt_ids.insert(fn_sid).second) {
                return k_invalid_func;
            }
            return lower_func_decl_(m, ast, sym, nres, tyck, fn_sid, is_acts_member, owner_acts);
        };

        // program root must be a block
        if (program_root == ast::k_invalid_stmt || (size_t)program_root >= ast.stmts().size()) {
            return m;
        }
        const auto& prog = ast.stmt(program_root);
        if (prog.kind != ast::StmtKind::kBlock) {
            return m;
        }

        const auto lower_stmt_recursive = [&](auto&& self, parus::ast::StmtId sid) -> void {
            if (sid == ast::k_invalid_stmt || (size_t)sid >= ast.stmts().size()) return;
            const auto& s = ast.stmt(sid);

            if (s.kind == ast::StmtKind::kFnDecl) {
                if (s.fn_generic_param_count > 0) return;
                (void)lower_fn_once(sid, /*is_acts_member=*/false, k_invalid_acts);
                return;
            }

            if (s.kind == ast::StmtKind::kFieldDecl) {
                if (s.decl_generic_param_count > 0) {
                    return;
                }
                (void)lower_field_decl_(m, ast, sym, nres, sid);
                return;
            }

            if (s.kind == ast::StmtKind::kProtoDecl) {
                if (s.decl_generic_param_count > 0) {
                    return;
                }
                if (!lowered_decl_stmt_ids.insert(sid).second) {
                    return;
                }
                const uint32_t begin = s.stmt_begin;
                const uint32_t end = s.stmt_begin + s.stmt_count;
                const auto& kids = ast.stmt_children();
                if (begin < kids.size() && end <= kids.size()) {
                    for (uint32_t i = begin; i < end; ++i) {
                        const auto member_sid = kids[i];
                        if (member_sid == ast::k_invalid_stmt || (size_t)member_sid >= ast.stmts().size()) continue;
                        const auto& member = ast.stmt(member_sid);
                        if (member.kind != ast::StmtKind::kFnDecl) continue;
                        if (member.a == ast::k_invalid_stmt) continue; // signature-only proto member
                        if (member.fn_generic_param_count > 0) continue;
                        (void)lower_fn_once(member_sid, /*is_acts_member=*/false, k_invalid_acts);
                    }
                }
                return;
            }

            if (s.kind == ast::StmtKind::kEnumDecl) {
                if (s.decl_generic_param_count > 0) {
                    return;
                }
                if (!lowered_decl_stmt_ids.insert(sid).second) {
                    return;
                }
                (void)lower_enum_field_decl_(m, ast, sym, nres, types, sid);
                return;
            }

            if (s.kind == ast::StmtKind::kClassDecl) {
                if (s.decl_generic_param_count > 0) {
                    return;
                }
                if (!lowered_decl_stmt_ids.insert(sid).second) {
                    return;
                }
                (void)lower_class_field_decl_(m, ast, sym, nres, sid);

                std::string class_qname = std::string(s.name);
                if (auto class_sym = resolve_symbol_from_stmt(nres, sid);
                    class_sym != k_invalid_symbol && (size_t)class_sym < sym.symbols().size()) {
                    class_qname = sym.symbol(class_sym).name;
                }

                const uint32_t begin = s.stmt_begin;
                const uint32_t end = s.stmt_begin + s.stmt_count;
                const auto& kids = ast.stmt_children();
                if (begin < kids.size() && end <= kids.size()) {
                    for (uint32_t i = begin; i < end; ++i) {
                        const auto member_sid = kids[i];
                        if (member_sid == ast::k_invalid_stmt || (size_t)member_sid >= ast.stmts().size()) continue;
                        const auto& member = ast.stmt(member_sid);
                        if (member.kind == ast::StmtKind::kFnDecl) {
                            if (member.a == ast::k_invalid_stmt) continue; // declaration-only member is not lowered
                            if (member.fn_generic_param_count > 0) continue;
                            (void)lower_fn_once(member_sid, /*is_acts_member=*/false, k_invalid_acts);
                            continue;
                        }

                        if (member.kind == ast::StmtKind::kVar) {
                            std::string vqname = class_qname;
                            if (!vqname.empty()) vqname += "::";
                            vqname += std::string(member.name);

                            SymbolId v_sym = k_invalid_symbol;
                            if (auto sid_sym = sym.lookup(vqname)) {
                                v_sym = *sid_sym;
                            }
                            lower_global_var_decl_(m, global_init_has_any_write, ast, sym, nres, tyck, member_sid, vqname, v_sym);
                            continue;
                        }
                    }
                }
                return;
            }

            if (s.kind == ast::StmtKind::kActorDecl) {
                (void)lower_actor_field_decl_(m, ast, sym, nres, sid);

                const uint32_t begin = s.stmt_begin;
                const uint32_t end = s.stmt_begin + s.stmt_count;
                const auto& kids = ast.stmt_children();
                if (begin < kids.size() && end <= kids.size()) {
                    for (uint32_t i = begin; i < end; ++i) {
                        const auto member_sid = kids[i];
                        if (member_sid == ast::k_invalid_stmt || (size_t)member_sid >= ast.stmts().size()) continue;
                        const auto& member = ast.stmt(member_sid);
                        if (member.kind != ast::StmtKind::kFnDecl) continue;
                        if (member.a == ast::k_invalid_stmt) continue;
                        if (member.fn_generic_param_count > 0) continue;
                        (void)lower_fn_once(member_sid, /*is_acts_member=*/false, k_invalid_acts);
                    }
                }
                return;
            }

            if (s.kind == ast::StmtKind::kActsDecl) {
                if (generic_acts_template_sid_set.find(sid) != generic_acts_template_sid_set.end()) {
                    return;
                }
                if (s.decl_generic_param_count > 0) {
                    return;
                }
                if (!lowered_decl_stmt_ids.insert(sid).second) {
                    return;
                }
                ActsDecl a{};
                a.span = s.span;
                a.sym = resolve_symbol_from_stmt(nres, sid);
                if (a.sym != k_invalid_symbol && (size_t)a.sym < sym.symbols().size()) {
                    a.name = sym.symbol(a.sym).name;
                } else {
                    a.name = s.name;
                }
                a.is_export = s.is_export;
                a.is_for = s.acts_is_for;
                a.has_set_name = s.acts_has_set_name;
                a.target_type = s.acts_target_type;
                a.func_begin = (uint32_t)m.funcs.size();
                a.func_count = 0;

                const ActsId aid = m.add_acts(a);

                const uint32_t begin = s.stmt_begin;
                const uint32_t end = s.stmt_begin + s.stmt_count;
                const auto& kids = ast.stmt_children();
                if (begin < kids.size() && end <= kids.size()) {
                    for (uint32_t k = begin; k < end; ++k) {
                        const auto member_sid = kids[k];
                        if (member_sid == ast::k_invalid_stmt || (size_t)member_sid >= ast.stmts().size()) continue;
                        if (ast.stmt(member_sid).kind != ast::StmtKind::kFnDecl) continue;
                        if (ast.stmt(member_sid).fn_generic_param_count > 0) continue;

                        const FuncId fid = lower_fn_once(member_sid, /*is_acts_member=*/true, aid);
                        if (fid != k_invalid_func) {
                            m.acts[aid].func_count++;
                        }
                    }
                }
                return;
            }

            if (s.kind == ast::StmtKind::kVar) {
                lower_global_var_decl_(m, global_init_has_any_write, ast, sym, nres, tyck, sid);
                return;
            }

            if (s.kind == ast::StmtKind::kNestDecl) {
                if (!s.nest_is_file_directive) {
                    self(self, s.a);
                }
                return;
            }

            if (s.kind == ast::StmtKind::kBlock) {
                const uint64_t begin = s.stmt_begin;
                const uint64_t end = begin + s.stmt_count;
                const auto& kids = ast.stmt_children();
                if (begin <= kids.size() && end <= kids.size()) {
                    for (uint32_t i = 0; i < s.stmt_count; ++i) {
                        self(self, kids[s.stmt_begin + i]);
                    }
                }
                return;
            }
        };

        lower_stmt_recursive(lower_stmt_recursive, program_root);

        for (const auto inst_sid : tyck.generic_instantiated_fn_sids) {
            if (inst_sid == ast::k_invalid_stmt || (size_t)inst_sid >= ast.stmts().size()) continue;
            const auto& fs = ast.stmt(inst_sid);
            if (fs.kind != ast::StmtKind::kFnDecl) continue;
            if (fs.a == ast::k_invalid_stmt) continue;
            (void)lower_fn_once(inst_sid, /*is_acts_member=*/false, k_invalid_acts);
        }

        for (const auto inst_sid : tyck.generic_instantiated_field_sids) {
            if (inst_sid == ast::k_invalid_stmt || (size_t)inst_sid >= ast.stmts().size()) continue;
            lower_stmt_recursive(lower_stmt_recursive, inst_sid);
        }
        for (const auto inst_sid : tyck.generic_instantiated_proto_sids) {
            if (inst_sid == ast::k_invalid_stmt || (size_t)inst_sid >= ast.stmts().size()) continue;
            lower_stmt_recursive(lower_stmt_recursive, inst_sid);
        }
        for (const auto inst_sid : tyck.generic_instantiated_class_sids) {
            if (inst_sid == ast::k_invalid_stmt || (size_t)inst_sid >= ast.stmts().size()) continue;
            lower_stmt_recursive(lower_stmt_recursive, inst_sid);
        }
        for (const auto inst_sid : tyck.generic_instantiated_acts_sids) {
            if (inst_sid == ast::k_invalid_stmt || (size_t)inst_sid >= ast.stmts().size()) continue;
            lower_stmt_recursive(lower_stmt_recursive, inst_sid);
        }
        for (const auto inst_sid : tyck.generic_instantiated_enum_sids) {
            if (inst_sid == ast::k_invalid_stmt || (size_t)inst_sid >= ast.stmts().size()) continue;
            lower_stmt_recursive(lower_stmt_recursive, inst_sid);
        }

        // Inject externally imported function symbols into SIR so OIR can emit stable function refs
        // for cross-module/cross-bundle calls without materializing null callee placeholders.
        std::unordered_set<SymbolId> lowered_fn_symbols{};
        lowered_fn_symbols.reserve(m.funcs.size());
        for (const auto& f : m.funcs) {
            if (f.sym != k_invalid_symbol) lowered_fn_symbols.insert(f.sym);
        }

        const auto& symbols = sym.symbols();
        for (uint32_t sid = 0; sid < static_cast<uint32_t>(symbols.size()); ++sid) {
            const auto& ss = symbols[sid];
            if (!ss.is_external) continue;
            if (ss.kind != sema::SymbolKind::kFn) continue;
            if (!lowered_fn_symbols.insert(sid).second) continue;
            if (ss.declared_type == k_invalid_type) continue;
            if (!types.is_fn(ss.declared_type)) continue;

            Func f{};
            f.span = ss.decl_span;
            f.name = ss.name;
            f.external_link_name = ss.link_name;
            f.sym = sid;
            f.sig = ss.declared_type;
            f.ret = types.get(ss.declared_type).ret;
            f.is_export = ss.is_export;
            f.is_extern = true;
            f.fn_mode = FnMode::kNone;
            f.abi = FuncAbi::kParus;
            f.is_pure = false;
            f.is_comptime = false;
            f.is_commit = false;
            f.is_recast = false;
            f.is_throwing = false;
            f.entry = k_invalid_block;
            f.origin_stmt = 0xFFFF'FFFFu;
            f.has_any_write = false;
            f.is_acts_member = false;
            f.owner_acts = k_invalid_acts;

            const auto& sig = types.get(ss.declared_type);
            f.param_begin = static_cast<uint32_t>(m.params.size());
            f.param_count = 0;
            f.positional_param_count = types.fn_positional_count(ss.declared_type);
            f.has_named_group = (sig.param_count > f.positional_param_count);
            for (uint32_t pi = 0; pi < sig.param_count; ++pi) {
                Param p{};
                p.name = types.fn_param_label_at(ss.declared_type, pi);
                p.type = types.fn_param_at(ss.declared_type, pi);
                p.is_mut = false;
                p.has_default = types.fn_param_has_default_at(ss.declared_type, pi);
                p.default_value = k_invalid_value;
                p.is_named_group = (pi >= f.positional_param_count);
                p.sym = k_invalid_symbol;
                p.span = ss.decl_span;
                (void)m.add_param(p);
                f.param_count++;
            }

            (void)m.add_func(f);
        }

        return m;
    }

} // namespace parus::sir
