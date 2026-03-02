// frontend/src/sir/sir_builder_expr_stmt.cpp
#include "sir_builder_internal.hpp"
#include <parus/syntax/TokenKind.hpp>


namespace parus::sir::detail {

    static SwitchCasePatKind lower_switch_pat_kind_(parus::ast::CasePatKind k) {
        using A = parus::ast::CasePatKind;
        switch (k) {
            case A::kInt:    return SwitchCasePatKind::kInt;
            case A::kChar:   return SwitchCasePatKind::kChar;
            case A::kString: return SwitchCasePatKind::kString;
            case A::kBool:   return SwitchCasePatKind::kBool;
            case A::kNull:   return SwitchCasePatKind::kNull;
            case A::kIdent:  return SwitchCasePatKind::kIdent;
            case A::kError:
            default:
                return SwitchCasePatKind::kError;
        }
    }

    ValueId lower_expr(
        Module& m,
        bool& out_has_any_write,
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        parus::ast::ExprId eid
    ) {
        (void)sym; // SIR builder does not sym.lookup (nres provides SymbolId)

        if (eid == parus::ast::k_invalid_expr) return k_invalid_value;

        const auto& e = ast.expr(eid);

        Value v{};
        v.span = e.span;
        v.type = type_of_ast_expr(tyck, eid);
        const ast::StmtId overload_sid =
            ((size_t)eid < tyck.expr_overload_target.size())
                ? tyck.expr_overload_target[eid]
                : ast::k_invalid_stmt;

        switch (e.kind) {
            case parus::ast::ExprKind::kIntLit:
                v.kind = ValueKind::kIntLit;
                v.text = e.text;
                break;
            case parus::ast::ExprKind::kFloatLit:
                v.kind = ValueKind::kFloatLit;
                v.text = e.text;
                break;
            case parus::ast::ExprKind::kStringLit:
                v.kind = ValueKind::kStringLit;
                v.text = e.string_folded_text.empty() ? e.text : e.string_folded_text;
                break;
            case parus::ast::ExprKind::kCharLit:
                v.kind = ValueKind::kCharLit;
                v.text = e.text;
                break;
            case parus::ast::ExprKind::kBoolLit:
                v.kind = ValueKind::kBoolLit;
                v.text = e.text;
                break;
            case parus::ast::ExprKind::kNullLit:
                v.kind = ValueKind::kNullLit;
                break;

            case parus::ast::ExprKind::kIdent: {
                v.kind = ValueKind::kLocal;
                v.text = e.text;
                v.sym = resolve_symbol_from_expr(nres, tyck, eid);
                break;
            }

            case parus::ast::ExprKind::kUnary: {
                if (e.op == parus::syntax::TokenKind::kAmp) {
                    v.kind = ValueKind::kBorrow;
                    v.borrow_is_mut = e.unary_is_mut;
                    v.op = (uint32_t)e.op;
                    v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                    v.origin_sym = resolve_root_place_symbol_from_expr(ast, nres, tyck, e.a);
                    break;
                }
                if (e.op == parus::syntax::TokenKind::kCaretAmp) {
                    v.kind = ValueKind::kEscape;
                    v.op = (uint32_t)e.op;
                    v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                    v.origin_sym = resolve_root_place_symbol_from_expr(ast, nres, tyck, e.a);
                    break;
                }

                v.kind = ValueKind::kUnary;
                v.op = (uint32_t)e.op;
                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                break;
            }

            case parus::ast::ExprKind::kPostfixUnary: {
                // v0: postfix++ only
                if (overload_sid != ast::k_invalid_stmt) {
                    v.kind = ValueKind::kCall;
                    v.callee_sym = resolve_symbol_from_stmt(nres, overload_sid);
                    v.callee_decl_stmt = overload_sid;
                    v.a = k_invalid_value;

                    v.arg_begin = (uint32_t)m.args.size();
                    v.arg_count = 0;

                    Arg a0{};
                    a0.kind = ArgKind::kPositional;
                    a0.value = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                    a0.span = e.span;
                    m.add_arg(a0);
                    v.arg_count = 1;
                } else {
                    v.kind = ValueKind::kPostfixInc;
                    v.op = (uint32_t)e.op;
                    v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                }
                break;
            }

            case parus::ast::ExprKind::kBinary: {
                if (e.op == parus::syntax::TokenKind::kDot) {
                    v.kind = ValueKind::kField;
                    v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                    if (e.b != parus::ast::k_invalid_expr) {
                        const auto& rhs = ast.expr(e.b);
                        v.text = rhs.text;
                    }
                    break;
                }

                if (overload_sid != ast::k_invalid_stmt) {
                    v.kind = ValueKind::kCall;
                    v.callee_sym = resolve_symbol_from_stmt(nres, overload_sid);
                    v.callee_decl_stmt = overload_sid;
                    v.a = k_invalid_value;
                    v.arg_begin = (uint32_t)m.args.size();
                    v.arg_count = 0;

                    Arg a0{};
                    a0.kind = ArgKind::kPositional;
                    a0.value = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                    a0.span = ast.expr(e.a).span;
                    m.add_arg(a0);
                    v.arg_count++;

                    Arg a1{};
                    a1.kind = ArgKind::kPositional;
                    a1.value = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.b);
                    a1.span = ast.expr(e.b).span;
                    m.add_arg(a1);
                    v.arg_count++;
                } else {
                    v.kind = ValueKind::kBinary;
                    v.op = (uint32_t)e.op;
                    v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                    v.b = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.b);
                }
                break;
            }

            case parus::ast::ExprKind::kAssign: {
                v.kind = ValueKind::kAssign;
                v.op = (uint32_t)e.op;
                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                v.b = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.b);
                break;
            }

            case parus::ast::ExprKind::kTernary: {
                // keep as if-expr in SIR
                v.kind = ValueKind::kIfExpr;
                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                v.b = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.b);
                v.c = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.c);
                break;
            }

            case parus::ast::ExprKind::kIfExpr: {
                // structured if-expr:
                // - v.a = cond
                // - v.b = then value (or wrapped block)
                // - v.c = else value (or wrapped block)
                v.kind = ValueKind::kIfExpr;

                // cond is always ExprId in v0.
                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);

                v.b = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.b);
                v.c = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.c);
                break;
            }

            case parus::ast::ExprKind::kBlockExpr: {
                const parus::ast::StmtId blk = e.block_stmt;
                if (is_valid_stmt_id_(ast, blk)) {
                    // create dedicated kBlockExpr node, return it directly (no extra wrapper)
                    return lower_block_value_(m, out_has_any_write, ast, sym, nres, tyck,
                                            blk, e.block_tail, e.span, v.type);
                }
                v.kind = ValueKind::kError;
                break;
            }

            case parus::ast::ExprKind::kLoop: {
                // loop expr lowering:
                // - v.op   : loop_has_header (0/1)
                // - v.text : loop_var (if any)
                // - v.a    : iter value
                // - v.b    : BlockId (stored in ValueId slot)
                v.kind = ValueKind::kLoopExpr;
                v.op = e.loop_has_header ? 1u : 0u;
                v.text = e.loop_var;

                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.loop_iter);

                const parus::ast::StmtId body = e.loop_body;
                if (is_valid_stmt_id_(ast, body)) {
                    const BlockId bid = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, body);
                    v.b = (ValueId)bid; // BlockId stored in ValueId slot by convention.
                } else {
                    v.b = k_invalid_value;
                }

                break;
            }

            case parus::ast::ExprKind::kCall:
            case parus::ast::ExprKind::kSpawn: {
                v.kind = ValueKind::kCall;
                if ((size_t)eid < tyck.expr_ctor_owner_type.size()) {
                    const auto owner_ty = tyck.expr_ctor_owner_type[eid];
                    if (owner_ty != parus::ty::kInvalidType) {
                        v.call_is_ctor = true;
                        v.ctor_owner_type = owner_ty;
                    }
                }
                bool inject_implicit_receiver = false;
                parus::ast::ExprId receiver_eid = parus::ast::k_invalid_expr;
                uint32_t receiver_param_index = 0xFFFF'FFFFu;
                if (overload_sid != ast::k_invalid_stmt) {
                    v.callee_sym = resolve_symbol_from_stmt(nres, overload_sid);
                    v.callee_decl_stmt = overload_sid;

                    // acts-for method call sugar:
                    //   obj.m(...)  -> T::m(obj, ...)  when selected overload has `self` receiver.
                    if ((size_t)overload_sid < ast.stmts().size() &&
                        e.a != parus::ast::k_invalid_expr) {
                        const auto& callee_expr = ast.expr(e.a);
                        if (callee_expr.kind == parus::ast::ExprKind::kBinary &&
                            callee_expr.op == parus::syntax::TokenKind::kDot &&
                            callee_expr.a != parus::ast::k_invalid_expr) {
                            const auto& def = ast.stmt(overload_sid);
                            if (def.kind == parus::ast::StmtKind::kFnDecl && def.param_count > 0) {
                                for (uint32_t pi = 0; pi < def.param_count; ++pi) {
                                    const auto& p = ast.params()[def.param_begin + pi];
                                    if (!p.is_self) continue;
                                    inject_implicit_receiver = true;
                                    receiver_eid = callee_expr.a;
                                    receiver_param_index = pi;
                                    break;
                                }
                            }
                        }
                    }
                }

                // callee
                if (overload_sid != ast::k_invalid_stmt) {
                    v.a = k_invalid_value;
                } else {
                    v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                }

                // args slice into Module::args
                v.arg_begin = (uint32_t)m.args.size();
                v.arg_count = 0;

                if (inject_implicit_receiver &&
                    receiver_param_index == 0 &&
                    receiver_eid != parus::ast::k_invalid_expr) {
                    Arg recv{};
                    recv.kind = ArgKind::kPositional;
                    recv.has_label = false;
                    recv.is_hole = false;
                    recv.span = ast.expr(receiver_eid).span;
                    recv.value = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, receiver_eid);
                    m.add_arg(recv);
                    v.arg_count++;
                }

                for (uint32_t i = 0; i < e.arg_count; ++i) {
                    const auto& aa = ast.args()[e.arg_begin + i];
                    Arg parent{};
                    parent.span = aa.span;
                    parent.has_label = aa.has_label;
                    parent.is_hole = aa.is_hole;
                    parent.label = aa.label;
                    parent.kind = (aa.kind == parus::ast::ArgKind::kLabeled)
                        ? ArgKind::kLabeled
                        : ArgKind::kPositional;

                    if (!aa.is_hole && aa.expr != parus::ast::k_invalid_expr) {
                        parent.value = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, aa.expr);
                    } else {
                        parent.value = k_invalid_value;
                    }

                    m.add_arg(parent);
                    v.arg_count++;
                    continue; // IMPORTANT: keep processing remaining args
                }

                if (inject_implicit_receiver &&
                    receiver_param_index != 0 &&
                    receiver_eid != parus::ast::k_invalid_expr) {
                    Arg recv{};
                    recv.kind = ArgKind::kPositional;
                    recv.has_label = false;
                    recv.is_hole = false;
                    recv.span = ast.expr(receiver_eid).span;
                    recv.value = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, receiver_eid);
                    m.add_arg(recv);
                    v.arg_count++;
                }

                break;
            }

            case parus::ast::ExprKind::kArrayLit: {
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

                        if (!aa.is_hole && aa.expr != parus::ast::k_invalid_expr) {
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

            case parus::ast::ExprKind::kFieldInit: {
                v.kind = ValueKind::kFieldInit;
                v.text = e.text;
                v.arg_begin = (uint32_t)m.args.size();
                v.arg_count = 0;

                const auto& inits = ast.field_init_entries();
                const uint64_t begin = e.field_init_begin;
                const uint64_t end = begin + e.field_init_count;
                if (begin <= inits.size() && end <= inits.size()) {
                    for (uint32_t i = 0; i < e.field_init_count; ++i) {
                        const auto& ent = inits[e.field_init_begin + i];
                        Arg a{};
                        a.kind = ArgKind::kLabeled;
                        a.has_label = true;
                        a.label = ent.name;
                        a.span = ent.span;
                        if (ent.expr != parus::ast::k_invalid_expr) {
                            a.value = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, ent.expr);
                        }
                        m.add_arg(a);
                        v.arg_count++;
                    }
                }
                break;
            }

            case parus::ast::ExprKind::kIndex: {
                v.kind = ValueKind::kIndex;
                v.a = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.a);
                v.b = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, e.b);
                break;
            }

            case parus::ast::ExprKind::kCast: {
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
            case parus::ast::ExprKind::kHole:
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
            case ValueKind::kField:
                join_child(v.a);
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
            case ValueKind::kFieldInit:
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
        const parus::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        parus::ast::StmtId sid
    ) {
        const auto& s = ast.stmt(sid);

        Stmt out{};
        out.span = s.span;

        switch (s.kind) {
            case parus::ast::StmtKind::kExprStmt:
                out.kind = StmtKind::kExprStmt;
                out.expr = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.expr);
                break;

            case parus::ast::StmtKind::kVar: {
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

            case parus::ast::StmtKind::kIf:
                out.kind = StmtKind::kIfStmt;
                out.expr = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.expr);
                if (s.a != parus::ast::k_invalid_stmt) out.a = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, s.a);
                if (s.b != parus::ast::k_invalid_stmt) out.b = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, s.b);
                break;

            case parus::ast::StmtKind::kWhile:
                out.kind = StmtKind::kWhileStmt;
                out.expr = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.expr);
                if (s.a != parus::ast::k_invalid_stmt) out.a = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, s.a);
                break;
            case parus::ast::StmtKind::kDoScope:
                out.kind = StmtKind::kDoScopeStmt;
                if (s.a != parus::ast::k_invalid_stmt) out.a = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, s.a);
                break;
            case parus::ast::StmtKind::kDoWhile:
                out.kind = StmtKind::kDoWhileStmt;
                out.expr = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.expr);
                if (s.a != parus::ast::k_invalid_stmt) out.a = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, s.a);
                break;
            case parus::ast::StmtKind::kManual:
                out.kind = StmtKind::kManualStmt;
                out.manual_perm_mask = s.manual_perm_mask;
                if (s.a != parus::ast::k_invalid_stmt) out.a = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, s.a);
                break;

            case parus::ast::StmtKind::kCommitStmt:
                out.kind = StmtKind::kCommitStmt;
                break;

            case parus::ast::StmtKind::kRecastStmt:
                out.kind = StmtKind::kRecastStmt;
                break;

            case parus::ast::StmtKind::kReturn:
                out.kind = StmtKind::kReturn;
                out.expr = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.expr);
                break;

            case parus::ast::StmtKind::kBreak:
                out.kind = StmtKind::kBreak;
                out.expr = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.expr);
                break;

            case parus::ast::StmtKind::kContinue:
                out.kind = StmtKind::kContinue;
                break;

            case parus::ast::StmtKind::kBlock:
                out.kind = StmtKind::kExprStmt;
                out.expr = lower_block_value_(m, out_has_any_write, ast, sym, nres, tyck, sid,
                                            parus::ast::k_invalid_expr, s.span, k_invalid_type);
                break;

            case parus::ast::StmtKind::kSwitch: {
                out.kind = StmtKind::kSwitch;
                out.expr = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, s.expr);
                out.case_begin = (uint32_t)m.switch_cases.size();
                out.case_count = 0;
                out.has_default = s.has_default;

                const uint64_t cb = s.case_begin;
                const uint64_t ce = cb + s.case_count;
                if (cb <= ast.switch_cases().size() && ce <= ast.switch_cases().size()) {
                    for (uint32_t i = 0; i < s.case_count; ++i) {
                        const auto& ac = ast.switch_cases()[s.case_begin + i];
                        SwitchCase sc{};
                        sc.is_default = ac.is_default;
                        sc.pat_kind = lower_switch_pat_kind_(ac.pat_kind);
                        sc.pat_text = ac.pat_text;
                        sc.span = ac.span;
                        if (ac.body != parus::ast::k_invalid_stmt) {
                            sc.body = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, ac.body);
                        }
                        (void)m.add_switch_case(sc);
                        out.case_count++;
                    }
                }
                break;
            }

            default:
                out.kind = StmtKind::kError;
                break;
        }

        return out;
    }

} // namespace parus::sir::detail
