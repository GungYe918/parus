// frontend/src/sir/sir_builder_decl.cpp
#include <parus/sir/Builder.hpp>
#include "sir_builder_internal.hpp"


namespace parus::sir::detail {

    FnMode lower_fn_mode(parus::ast::FnMode m) {
        switch (m) {
            case parus::ast::FnMode::kPub: return FnMode::kPub;
            case parus::ast::FnMode::kSub: return FnMode::kSub;
            case parus::ast::FnMode::kNone:
            default: return FnMode::kNone;
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
        f.name = s.name;

        // signature & ret
        f.sig = s.type;
        f.ret = s.fn_ret;

        // decl symbol (fn name)
        f.sym = resolve_symbol_from_stmt(nres, sid);

        // qualifiers / mode
        f.is_export = s.is_export;
        f.fn_mode = lower_fn_mode(s.fn_mode);

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
            sp.sym = resolve_symbol_from_param_index(nres, param_index);

            m.add_param(sp);
            f.param_count++;
        }

        // body
        if (s.a != ast::k_invalid_stmt) {
            f.entry = lower_block_stmt(m, has_any_write, ast, sym, nres, tyck, s.a);
        }

        f.has_any_write = has_any_write;
        return m.add_func(f);
    }

    /// @brief AST field 선언을 SIR field 메타로 lower한다.
    FieldId lower_field_decl_(
        Module& m,
        const parus::ast::AstArena& ast,
        const passes::NameResolveResult& nres,
        parus::ast::StmtId sid
    ) {
        const auto& s = ast.stmt(sid);
        if (s.kind != ast::StmtKind::kFieldDecl) {
            return k_invalid_field;
        }

        FieldDecl f{};
        f.span = s.span;
        f.name = s.name;
        f.is_export = s.is_export;
        f.sym = resolve_symbol_from_stmt(nres, sid);

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
        (void)opt;

        Module m{};

        // program root must be a block
        if (program_root == ast::k_invalid_stmt || (size_t)program_root >= ast.stmts().size()) {
            return m;
        }
        const auto& prog = ast.stmt(program_root);
        if (prog.kind != ast::StmtKind::kBlock) {
            return m;
        }

        for (uint32_t i = 0; i < prog.stmt_count; ++i) {
            const auto sid = ast.stmt_children()[prog.stmt_begin + i];
            const auto& s = ast.stmt(sid);

            if (s.kind == ast::StmtKind::kFnDecl) {
                (void)lower_func_decl_(m, ast, sym, nres, tyck, sid, /*is_acts_member=*/false, k_invalid_acts);
                continue;
            }

            if (s.kind == ast::StmtKind::kFieldDecl) {
                (void)lower_field_decl_(m, ast, nres, sid);
                continue;
            }

            if (s.kind == ast::StmtKind::kActsDecl) {
                ActsDecl a{};
                a.span = s.span;
                a.name = s.name;
                a.sym = resolve_symbol_from_stmt(nres, sid);
                a.is_export = s.is_export;
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

                        const FuncId fid = lower_func_decl_(m, ast, sym, nres, tyck, member_sid,
                                                            /*is_acts_member=*/true, aid);
                        if (fid != k_invalid_func) {
                            m.acts[aid].func_count++;
                        }
                    }
                }
                continue;
            }

            if (s.kind == ast::StmtKind::kVar) {
                GlobalVarDecl g{};
                g.span = s.span;
                g.name = s.name;
                g.sym = resolve_symbol_from_stmt(nres, sid);
                g.is_set = s.is_set;
                g.is_mut = s.is_mut;
                g.is_static = s.is_static;

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

                (void)m.add_global(g);
                continue;
            }
        }

        return m;
    }

} // namespace parus::sir
