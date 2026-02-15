// compiler/src/sir/sir_builder_expr_stmt.cpp
#include "sir_builder_internal.hpp"
#include <gaupel/syntax/TokenKind.hpp>


namespace gaupel::sir::detail {

    ValueId lower_expr(
        Module& m,
        bool& out_has_any_write,
        const gaupel::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        gaupel::ast::ExprId eid
    ) {
        (void)sym; // SIR builder does not sym.lookup (nres provides SymbolId)

        if (eid == gaupel::ast::k_invalid_expr) return k_invalid_value;

        const auto& e = ast.expr(eid);

        Value v{};
        v.span = e.span;
        v.type = type_of_ast_expr(tyck, eid);

        switch (e.kind) {
            case gaupel::ast::ExprKind::kIntLit:
                v.kind = ValueKind::kIntLit;
                v.text = e.text;
                break;
            case gaupel::ast::ExprKind::kFloatLit:
                v.kind = ValueKind::kFloatLit;
                v.text = e.text;
                break;
            case gaupel::ast::ExprKind::kStringLit:
                v.kind = ValueKind::kStringLit;
                v.text = e.text;
                break;
            case gaupel::ast::ExprKind::kCharLit:
                v.kind = ValueKind::kCharLit;
                v.text = e.text;
                break;
            case gaupel::ast::ExprKind::kBoolLit:
                v.kind = ValueKind::kBoolLit;
                v.text = e.text;
                break;
            case gaupel::ast::ExprKind::kNullLit:
                v.kind = ValueKind::kNullLit;
                break;

            case gaupel::ast::ExprKind::kIdent: {
                v.kind = ValueKind::kLocal;
                v.text = e.text;
                v.sym = resolve_symbol_from_expr(nres, eid);
                break;
            }

            case gaupel::ast::ExprKind::kUnary: {
                if (e.op == gaupel::syntax::TokenKind::kAmp) {
                    v.kind = ValueKind::kBorrow;
                    v.borrow_is_mut = e.unary_is_mut;
                    v.op = (uint32_t)e.op;
                    v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                    v.origin_sym = resolve_root_place_symbol_from_expr(ast, nres, e.a);
                    break;
                }
                if (e.op == gaupel::syntax::TokenKind::kAmpAmp) {
                    v.kind = ValueKind::kEscape;
                    v.op = (uint32_t)e.op;
                    v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                    v.origin_sym = resolve_root_place_symbol_from_expr(ast, nres, e.a);
                    break;
                }

                v.kind = ValueKind::kUnary;
                v.op = (uint32_t)e.op;
                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                break;
            }

            case gaupel::ast::ExprKind::kPostfixUnary: {
                // v0: postfix++ only
                v.kind = ValueKind::kPostfixInc;
                v.op = (uint32_t)e.op;
                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                break;
            }

            case gaupel::ast::ExprKind::kBinary: {
                v.kind = ValueKind::kBinary;
                v.op = (uint32_t)e.op;
                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                v.b = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.b);
                break;
            }

            case gaupel::ast::ExprKind::kAssign: {
                v.kind = ValueKind::kAssign;
                v.op = (uint32_t)e.op;
                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                v.b = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.b);
                break;
            }

            case gaupel::ast::ExprKind::kTernary: {
                // keep as if-expr in SIR
                v.kind = ValueKind::kIfExpr;
                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                v.b = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.b);
                v.c = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.c);
                break;
            }

            case gaupel::ast::ExprKind::kIfExpr: {
                // structured if-expr:
                // - v.a = cond
                // - v.b = then value (or wrapped block)
                // - v.c = else value (or wrapped block)
                v.kind = ValueKind::kIfExpr;

                // cond is always ExprId in v0.
                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);

                // then / else may be ExprId or StmtId (legacy quirk)
                v.b = lower_expr_or_stmt_as_value_(m, out_has_any_write, ast, sym, nres, tyck, e.b, e.span, v.type);
                v.c = lower_expr_or_stmt_as_value_(m, out_has_any_write, ast, sym, nres, tyck, e.c, e.span, v.type);
                break;
            }

            case gaupel::ast::ExprKind::kBlockExpr: {
                // IMPORTANT (current parser):
                // - e.a : StmtId (block stmt), stored in ExprId slot by convention
                // - e.b : tail ExprId (or invalid)
                // - e.c : reserved
                const gaupel::ast::StmtId blk = (gaupel::ast::StmtId)e.a;
                if (is_valid_stmt_id_(ast, blk)) {
                    // create dedicated kBlockExpr node, return it directly (no extra wrapper)
                    return lower_block_value_(m, out_has_any_write, ast, sym, nres, tyck,
                                            blk, e.b, e.span, v.type);
                }
                v.kind = ValueKind::kError;
                break;
            }

            case gaupel::ast::ExprKind::kLoop: {
                // loop expr lowering:
                // - v.op   : loop_has_header (0/1)
                // - v.text : loop_var (if any)
                // - v.a    : iter value
                // - v.b    : BlockId (stored in ValueId slot)
                v.kind = ValueKind::kLoopExpr;
                v.op = e.loop_has_header ? 1u : 0u;
                v.text = e.loop_var;

                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.loop_iter);

                const gaupel::ast::StmtId body = e.loop_body;
                if (is_valid_stmt_id_(ast, body)) {
                    const BlockId bid = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, body);
                    v.b = (ValueId)bid; // BlockId stored in ValueId slot by convention.
                } else {
                    v.b = k_invalid_value;
                }

                break;
            }

            case gaupel::ast::ExprKind::kCall: {
                v.kind = ValueKind::kCall;

                // callee
                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);

                // args slice into Module::args
                v.arg_begin = (uint32_t)m.args.size();
                v.arg_count = 0;

                for (uint32_t i = 0; i < e.arg_count; ++i) {
                    const auto& aa = ast.args()[e.arg_begin + i];

                    // Parent entry (one per "argument slot" in AST args list)
                    Arg parent{};
                    parent.span = aa.span;
                    parent.has_label = aa.has_label;
                    parent.is_hole = aa.is_hole;
                    parent.label = aa.label;

                    parent.kind =
                        (aa.kind == gaupel::ast::ArgKind::kPositional) ? ArgKind::kPositional :
                        (aa.kind == gaupel::ast::ArgKind::kLabeled) ? ArgKind::kLabeled :
                        ArgKind::kNamedGroup;

                    if (aa.kind == gaupel::ast::ArgKind::kNamedGroup) {
                        // NamedGroup children are stored in ast.named_group_args().
                        //
                        // SIR policy:
                        // - emit ONE parent Arg with kind=kNamedGroup
                        // - then emit children Args as adjacent entries in Module::args
                        // - patch parent.child_begin/child_count after children emitted

                        parent.value = k_invalid_value;

                        const uint32_t parent_idx = m.add_arg(parent);
                        v.arg_count++;

                        const uint32_t child_begin = (uint32_t)m.args.size();
                        uint32_t child_emitted = 0;

                        const uint32_t ng_begin = aa.child_begin;
                        const uint32_t ng_end   = aa.child_begin + aa.child_count;

                        if (ng_begin < (uint32_t)ast.named_group_args().size() &&
                            ng_end   <= (uint32_t)ast.named_group_args().size()) {

                            for (uint32_t j = 0; j < aa.child_count; ++j) {
                                const auto& child = ast.named_group_args()[aa.child_begin + j];

                                Arg sc{};
                                sc.span = child.span;
                                sc.has_label = child.has_label;
                                sc.is_hole = child.is_hole;
                                sc.label = child.label;

                                sc.kind =
                                    (child.kind == gaupel::ast::ArgKind::kPositional) ? ArgKind::kPositional :
                                    (child.kind == gaupel::ast::ArgKind::kLabeled) ? ArgKind::kLabeled :
                                    ArgKind::kNamedGroup; // (shouldn't nest in v0)

                                if (!child.is_hole && child.expr != gaupel::ast::k_invalid_expr) {
                                    sc.value = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, child.expr);
                                } else {
                                    sc.value = k_invalid_value;
                                }

                                m.add_arg(sc);
                                v.arg_count++;
                                child_emitted++;
                            }
                        }

                        // patch parent now that children are emitted
                        m.args[parent_idx].child_begin = child_begin;
                        m.args[parent_idx].child_count = child_emitted;

                        continue; // IMPORTANT: keep processing remaining args
                    }

                    // Non-named-group: normal value
                    if (!aa.is_hole && aa.expr != gaupel::ast::k_invalid_expr) {
                        parent.value = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, aa.expr);
                    } else {
                        parent.value = k_invalid_value;
                    }

                    m.add_arg(parent);
                    v.arg_count++;
                    continue; // IMPORTANT: keep processing remaining args
                }

                break;
            }

            case gaupel::ast::ExprKind::kArrayLit: {
                v.kind = ValueKind::kArrayLit;
                v.arg_begin = (uint32_t)m.args.size();
                v.arg_count = 0;

                const auto& args = ast.args();
                const uint32_t end = e.arg_begin + e.arg_count;
                if (e.arg_begin < args.size() && end <= args.size()) {
                    for (uint32_t i = 0; i < e.arg_count; ++i) {
                        const auto& aa = args[e.arg_begin + i];

                        Arg item{};
                        item.kind = ArgKind::kPositional;
                        item.has_label = false;
                        item.is_hole = aa.is_hole;
                        item.span = aa.span;

                        if (!aa.is_hole && aa.expr != gaupel::ast::k_invalid_expr) {
                            item.value = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, aa.expr);
                        } else {
                            item.value = k_invalid_value;
                        }

                        m.add_arg(item);
                        v.arg_count++;
                    }
                }
                break;
            }

            case gaupel::ast::ExprKind::kIndex: {
                v.kind = ValueKind::kIndex;
                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                v.b = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.b);
                break;
            }

            case gaupel::ast::ExprKind::kCast: {
                v.kind = ValueKind::kCast;

                // operand
                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);

                // cast kind: as / as? / as!
                v.op = (uint32_t)e.cast_kind;

                // cast target type: "T"
                v.cast_to = e.cast_type;

                // v.type is already set at function entry:
                //   v.type = type_of_ast_expr(tyck, eid);
                // so dump/lowering can use:
                //   - cast_to for syntactic target
                //   - type for normalized RESULT type (as? => T?)
                break;
            }

            // v0 not-lowered-yet expr kinds
            case gaupel::ast::ExprKind::kHole:
            default:
                v.kind = ValueKind::kError;
                break;
        }

        v.place = classify_place_from_ast(ast, eid);
        v.effect = classify_effect(v.kind);
        auto join_child = [&](ValueId cid) {
            if (cid != k_invalid_value && (size_t)cid < m.values.size()) {
                v.effect = join_effect_(v.effect, m.values[cid].effect);
            }
        };

        switch (v.kind) {
            case ValueKind::kUnary:
            case ValueKind::kBorrow:
            case ValueKind::kEscape:
            case ValueKind::kPostfixInc:
            case ValueKind::kCast:
                join_child(v.a);
                break;

            case ValueKind::kBinary:
            case ValueKind::kAssign:
            case ValueKind::kIndex:
                join_child(v.a);
                join_child(v.b);
                break;

            case ValueKind::kIfExpr:
                join_child(v.a);
                join_child(v.b);
                join_child(v.c);
                break;

            case ValueKind::kCall:
                join_child(v.a);
                break;

            case ValueKind::kArrayLit:
                if ((uint64_t)v.arg_begin + (uint64_t)v.arg_count <= (uint64_t)m.args.size()) {
                    for (uint32_t i = 0; i < v.arg_count; ++i) {
                        join_child(m.args[v.arg_begin + i].value);
                    }
                }
                break;

            case ValueKind::kLoopExpr: {
                join_child(v.a);
                const BlockId body = (BlockId)v.b;
                v.effect = join_effect_(v.effect, effect_of_block_(m, body));
                break;
            }

            default:
                break;
        }

        if (v.effect == EffectClass::kMayWrite) {
            out_has_any_write = true;
        }

        return m.add_value(v);
    }

    Stmt lower_stmt_(
        Module& m,
        bool& out_has_any_write,
        const gaupel::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        gaupel::ast::StmtId sid
    ) {
        const auto& s = ast.stmt(sid);

        Stmt out{};
        out.span = s.span;

        switch (s.kind) {
            case gaupel::ast::StmtKind::kExprStmt:
                out.kind = StmtKind::kExprStmt;
                out.expr = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.expr);
                break;

            case gaupel::ast::StmtKind::kVar: {
                out.kind = StmtKind::kVarDecl;
                out.is_set = s.is_set;
                out.is_mut = s.is_mut;
                out.is_static = s.is_static;
                out.name = s.name;
                out.init = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.init);

                // decl symbol from stmt
                out.sym = resolve_symbol_from_stmt(nres, sid);

                const bool has_sym = (out.sym != k_invalid_symbol && (size_t)out.sym < sym.symbols().size());
                const TypeId use_derived_type = resolve_decl_type_from_symbol_uses(nres, tyck, out.sym);

                // declared_type policy:
                // - let: prefer declared symbol type, fallback annotation
                // - set: prefer use-derived tyck type, then init tyck type, then symbol type
                if (!s.is_set) {
                    out.declared_type = has_sym ? sym.symbol(out.sym).declared_type : k_invalid_type;
                    if (out.declared_type == k_invalid_type) out.declared_type = s.type;
                } else {
                    out.declared_type = use_derived_type;
                    if (out.declared_type == k_invalid_type) {
                        out.declared_type = type_of_ast_expr(tyck, s.init);
                    }
                    if (out.declared_type == k_invalid_type && has_sym) {
                        out.declared_type = sym.symbol(out.sym).declared_type;
                    }
                }
                break;
            }

            case gaupel::ast::StmtKind::kIf:
                out.kind = StmtKind::kIfStmt;
                out.expr = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.expr);
                if (s.a != gaupel::ast::k_invalid_stmt) out.a = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, s.a);
                if (s.b != gaupel::ast::k_invalid_stmt) out.b = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, s.b);
                break;

            case gaupel::ast::StmtKind::kWhile:
                out.kind = StmtKind::kWhileStmt;
                out.expr = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.expr);
                if (s.a != gaupel::ast::k_invalid_stmt) out.a = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, s.a);
                break;

            case gaupel::ast::StmtKind::kReturn:
                out.kind = StmtKind::kReturn;
                out.expr = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.expr);
                break;

            case gaupel::ast::StmtKind::kBreak:
                out.kind = StmtKind::kBreak;
                out.expr = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.expr);
                break;

            case gaupel::ast::StmtKind::kContinue:
                out.kind = StmtKind::kContinue;
                break;

            case gaupel::ast::StmtKind::kBlock:
                out.kind = StmtKind::kExprStmt;
                out.expr = lower_block_value_(m, out_has_any_write, ast, sym, nres, tyck, sid,
                                            gaupel::ast::k_invalid_expr, s.span, k_invalid_type);
                break;

            default:
                out.kind = StmtKind::kError;
                break;
        }

        return out;
    }

} // namespace gaupel::sir::detail
