// compiler/src/sir/sir_builder.cpp
#include <gaupel/sir/Builder.hpp>

namespace gaupel::sir {

    static TypeId type_of_ast_expr(const tyck::TyckResult& tyck, gaupel::ast::ExprId eid) {
        if (eid == gaupel::ast::k_invalid_expr) return k_invalid_type;
        if ((size_t)eid >= tyck.expr_types.size()) return k_invalid_type;
        return tyck.expr_types[eid];
    }

    // -----------------------------
    // NEW: NameResolveResult 기반 심볼 resolve
    // -----------------------------
    static SymbolId resolve_symbol_from_expr(
        const passes::NameResolveResult& nres,
        gaupel::ast::ExprId eid
    ) {
        if (eid == gaupel::ast::k_invalid_expr) return k_invalid_symbol;
        if ((size_t)eid >= nres.expr_to_symbol.size()) return k_invalid_symbol;
        return nres.expr_to_symbol[eid];
    }

    static SymbolId resolve_symbol_from_stmt(
        const passes::NameResolveResult& nres,
        gaupel::ast::StmtId sid
    ) {
        if (sid == gaupel::ast::k_invalid_stmt) return k_invalid_symbol;
        if ((size_t)sid >= nres.stmt_to_symbol.size()) return k_invalid_symbol;
        return nres.stmt_to_symbol[sid];
    }

    // place 분류(tyck의 is_place_expr_와 규칙 맞추기)
    static PlaceClass classify_place_from_ast(const gaupel::ast::AstArena& ast, gaupel::ast::ExprId eid) {
        if (eid == gaupel::ast::k_invalid_expr) return PlaceClass::kNotPlace;
        const auto& e = ast.expr(eid);
        if (e.kind == gaupel::ast::ExprKind::kIdent) return PlaceClass::kLocal;
        if (e.kind == gaupel::ast::ExprKind::kIndex) return PlaceClass::kIndex;
        return PlaceClass::kNotPlace;
    }

    static EffectClass classify_effect(ValueKind k) {
        switch (k) {
            case ValueKind::kAssign:
            case ValueKind::kPostfixInc:
                return EffectClass::kMayWrite;
            case ValueKind::kCall:
                return EffectClass::kUnknown;
            default:
                return EffectClass::kPure;
        }
    }

    // forward decl
    static ValueId lower_expr(
        Module& m,
        const gaupel::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        gaupel::ast::ExprId eid
    );

    static uint32_t lower_stmt_into_block(
        Module& m,
        const gaupel::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        gaupel::ast::StmtId sid
    );

    static BlockId lower_block_stmt(
        Module& m,
        const gaupel::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        gaupel::ast::StmtId block_sid
    ) {
        const auto& bs = ast.stmt(block_sid);

        Block b{};
        b.span = bs.span;
        b.stmt_begin = (uint32_t)m.stmts.size();
        b.stmt_count = 0;

        BlockId bid = m.add_block(b);

        for (uint32_t i = 0; i < bs.stmt_count; ++i) {
            const auto child = ast.stmt_children()[bs.stmt_begin + i];
            (void)lower_stmt_into_block(m, ast, sym, nres, tyck, child);
            m.blocks[bid].stmt_count++;
        }

        return bid;
    }

    static ValueId lower_expr(
        Module& m,
        const gaupel::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        gaupel::ast::ExprId eid
    ) {
        (void)sym; // SIR builder는 이제 sym.lookup를 사용하지 않음(심볼은 nres가 제공)

        if (eid == gaupel::ast::k_invalid_expr) return k_invalid_value;

        const auto& e = ast.expr(eid);
        Value v{};
        v.span = e.span;
        v.type = type_of_ast_expr(tyck, eid);

        switch (e.kind) {
            case gaupel::ast::ExprKind::kIntLit:
                v.kind = ValueKind::kIntLit; v.text = e.text; break;
            case gaupel::ast::ExprKind::kFloatLit:
                v.kind = ValueKind::kFloatLit; v.text = e.text; break;
            case gaupel::ast::ExprKind::kStringLit:
                v.kind = ValueKind::kStringLit; v.text = e.text; break;
            case gaupel::ast::ExprKind::kCharLit:
                v.kind = ValueKind::kCharLit; v.text = e.text; break;
            case gaupel::ast::ExprKind::kBoolLit:
                v.kind = ValueKind::kBoolLit; v.text = e.text; break;
            case gaupel::ast::ExprKind::kNullLit:
                v.kind = ValueKind::kNullLit; break;

            case gaupel::ast::ExprKind::kIdent: {
                v.kind = ValueKind::kLocal;
                v.text = e.text;
                v.sym  = resolve_symbol_from_expr(nres, eid);
                break;
            }

            case gaupel::ast::ExprKind::kUnary: {
                v.kind = ValueKind::kUnary;
                v.op = (uint32_t)e.op;
                v.a = lower_expr(m, ast, sym, nres, tyck, e.a);
                break;
            }

            case gaupel::ast::ExprKind::kPostfixUnary: {
                v.kind = ValueKind::kPostfixInc; // v0: postfix++ only
                v.op = (uint32_t)e.op;
                v.a = lower_expr(m, ast, sym, nres, tyck, e.a);
                break;
            }

            case gaupel::ast::ExprKind::kBinary: {
                v.kind = ValueKind::kBinary;
                v.op = (uint32_t)e.op;
                v.a = lower_expr(m, ast, sym, nres, tyck, e.a);
                v.b = lower_expr(m, ast, sym, nres, tyck, e.b);
                break;
            }

            case gaupel::ast::ExprKind::kAssign: {
                v.kind = ValueKind::kAssign;
                v.op = (uint32_t)e.op;
                v.a = lower_expr(m, ast, sym, nres, tyck, e.a);
                v.b = lower_expr(m, ast, sym, nres, tyck, e.b);
                break;
            }

            case gaupel::ast::ExprKind::kTernary: {
                v.kind = ValueKind::kIfExpr;
                v.a = lower_expr(m, ast, sym, nres, tyck, e.a);
                v.b = lower_expr(m, ast, sym, nres, tyck, e.b);
                v.c = lower_expr(m, ast, sym, nres, tyck, e.c);
                break;
            }

            case gaupel::ast::ExprKind::kCall: {
                v.kind = ValueKind::kCall;
                v.a = lower_expr(m, ast, sym, nres, tyck, e.a); // callee
                v.arg_begin = (uint32_t)m.args.size();
                v.arg_count = 0;

                for (uint32_t i = 0; i < e.arg_count; ++i) {
                    const auto& aa = ast.args()[e.arg_begin + i];
                    Arg sa{};
                    sa.span = aa.span;
                    sa.has_label = aa.has_label;
                    sa.is_hole = aa.is_hole;
                    sa.label = aa.label;
                    sa.kind = (aa.kind == gaupel::ast::ArgKind::kPositional) ? ArgKind::kPositional :
                            (aa.kind == gaupel::ast::ArgKind::kLabeled) ? ArgKind::kLabeled :
                            ArgKind::kNamedGroup;

                    if (aa.expr != gaupel::ast::k_invalid_expr) {
                        sa.value = lower_expr(m, ast, sym, nres, tyck, aa.expr);
                    }
                    m.add_arg(sa);
                    v.arg_count++;
                }
                break;
            }

            case gaupel::ast::ExprKind::kIndex: {
                v.kind = ValueKind::kIndex;
                v.a = lower_expr(m, ast, sym, nres, tyck, e.a);
                v.b = lower_expr(m, ast, sym, nres, tyck, e.b);
                break;
            }

            case gaupel::ast::ExprKind::kCast: {
                v.kind = ValueKind::kCast;
                v.a = lower_expr(m, ast, sym, nres, tyck, e.a);
                v.b = k_invalid_value;
                v.op = (uint32_t)e.cast_kind;
                break;
            }

            default:
                v.kind = ValueKind::kError;
                break;
        }

        v.place = classify_place_from_ast(ast, eid);
        v.effect = classify_effect(v.kind);

        return m.add_value(v);
    }
    
    
    static uint32_t lower_stmt_into_block(
        Module& m,
        const gaupel::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        gaupel::ast::StmtId sid
    ) {
        (void)sym;

        const auto& s = ast.stmt(sid);
        Stmt out{};
        out.span = s.span;

        switch (s.kind) {
            case gaupel::ast::StmtKind::kExprStmt:
                out.kind = StmtKind::kExprStmt;
                out.expr = lower_expr(m, ast, sym, nres, tyck, s.expr);
                break;

            case gaupel::ast::StmtKind::kVar: {
                out.kind = StmtKind::kVarDecl;
                out.is_set = s.is_set;
                out.is_mut = s.is_mut;
                out.name = s.name;
                out.init = lower_expr(m, ast, sym, nres, tyck, s.init);

                // 심볼은 decl stmt 기준으로 가져온다.
                out.sym = resolve_symbol_from_stmt(nres, sid);

                // declared_type 정책:
                // - let: annotation 그대로(s.type)
                // - set: init의 tyck 결과를 declared_type로 저장(추론 확정 시점까지 힌트)
                if (!s.is_set) {
                    out.declared_type = s.type;
                } else {
                    out.declared_type = type_of_ast_expr(tyck, s.init);
                }
                break;
            }

            case gaupel::ast::StmtKind::kIf:
                out.kind = StmtKind::kIfStmt;
                out.expr = lower_expr(m, ast, sym, nres, tyck, s.expr);
                if (s.a != gaupel::ast::k_invalid_stmt) out.a = lower_block_stmt(m, ast, sym, nres, tyck, s.a);
                if (s.b != gaupel::ast::k_invalid_stmt) out.b = lower_block_stmt(m, ast, sym, nres, tyck, s.b);
                break;

            case gaupel::ast::StmtKind::kWhile:
                out.kind = StmtKind::kWhileStmt;
                out.expr = lower_expr(m, ast, sym, nres, tyck, s.expr);
                if (s.a != gaupel::ast::k_invalid_stmt) out.a = lower_block_stmt(m, ast, sym, nres, tyck, s.a);
                break;

            case gaupel::ast::StmtKind::kReturn:
                out.kind = StmtKind::kReturn;
                out.expr = lower_expr(m, ast, sym, nres, tyck, s.expr);
                break;

            case gaupel::ast::StmtKind::kBreak:
                out.kind = StmtKind::kBreak;
                out.expr = lower_expr(m, ast, sym, nres, tyck, s.expr);
                break;

            case gaupel::ast::StmtKind::kContinue:
                out.kind = StmtKind::kContinue;
                break;

            default:
                out.kind = StmtKind::kError;
                break;
        }

        return m.add_stmt(out);
    }
    
    
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
        (void)opt;

        Module m{};

        // program root는 block이라고 가정
        const auto& prog = ast.stmt(program_root);

        for (uint32_t i = 0; i < prog.stmt_count; ++i) {
            auto sid = ast.stmt_children()[prog.stmt_begin + i];
            const auto& s = ast.stmt(sid);

            if (s.kind != ast::StmtKind::kFnDecl) {
                continue;
            }

            Func f{};
            f.span = s.span;
            f.name = s.name;
            f.sig  = s.type;

            // ret 추출(best effort)
            if (f.sig != k_invalid_type && types.get(f.sig).kind == gaupel::ty::Kind::kFn) {
                f.ret = types.get(f.sig).ret;
            }

            // fn decl 심볼도 stmt_to_symbol로
            f.sym = resolve_symbol_from_stmt(nres, sid);

            if (s.a != ast::k_invalid_stmt) {
                f.entry = lower_block_stmt(m, ast, sym, nres, tyck, s.a);
            }

            m.add_func(f);
        }

        return m;
    }

} // namespace gaupel::sir