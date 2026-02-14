// compiler/src/sir/sir_builder.cpp
#include <gaupel/sir/Builder.hpp>

namespace gaupel::sir {

    static TypeId type_of_ast_expr(const tyck::TyckResult& tyck, gaupel::ast::ExprId eid) {
        if (eid == gaupel::ast::k_invalid_expr) return k_invalid_type;
        if ((size_t)eid >= tyck.expr_types.size()) return k_invalid_type;
        return tyck.expr_types[eid];
    }

    // -----------------------------
    // NameResolveResult 기반 심볼 resolve
    // -----------------------------
    static SymbolId resolve_symbol_from_expr(
        const passes::NameResolveResult& nres,
        gaupel::ast::ExprId eid
    ) {
        if (eid == gaupel::ast::k_invalid_expr) return k_invalid_symbol;
        if ((size_t)eid >= nres.expr_to_resolved.size()) return k_invalid_symbol;

        const auto rid = nres.expr_to_resolved[(uint32_t)eid];
        if (rid == passes::NameResolveResult::k_invalid_resolved) return k_invalid_symbol;
        if ((size_t)rid >= nres.resolved.size()) return k_invalid_symbol;

        return (SymbolId)nres.resolved[rid].sym;
    }

    static SymbolId resolve_symbol_from_stmt(
        const passes::NameResolveResult& nres,
        gaupel::ast::StmtId sid
    ) {
        if (sid == gaupel::ast::k_invalid_stmt) return k_invalid_symbol;
        if ((size_t)sid >= nres.stmt_to_resolved.size()) return k_invalid_symbol;

        const auto rid = nres.stmt_to_resolved[(uint32_t)sid];
        if (rid == passes::NameResolveResult::k_invalid_resolved) return k_invalid_symbol;
        if ((size_t)rid >= nres.resolved.size()) return k_invalid_symbol;

        return (SymbolId)nres.resolved[rid].sym;
    }

    static SymbolId resolve_symbol_from_param_index(
        const passes::NameResolveResult& nres,
        uint32_t param_index
    ) {
        if ((size_t)param_index >= nres.param_to_resolved.size()) return k_invalid_symbol;

        const auto rid = nres.param_to_resolved[param_index];
        if (rid == passes::NameResolveResult::k_invalid_resolved) return k_invalid_symbol;
        if ((size_t)rid >= nres.resolved.size()) return k_invalid_symbol;

        return (SymbolId)nres.resolved[rid].sym;
    }

    // Resolve the most concrete type we can observe from identifier use-sites
    // that bind to the same symbol.
    static TypeId resolve_decl_type_from_symbol_uses(
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        SymbolId sym_id
    ) {
        if (sym_id == k_invalid_symbol) return k_invalid_type;
        if (nres.expr_to_resolved.empty()) return k_invalid_type;

        const uint32_t expr_n = (uint32_t)nres.expr_to_resolved.size();
        for (uint32_t eid = 0; eid < expr_n; ++eid) {
            const auto rid = nres.expr_to_resolved[eid];
            if (rid == passes::NameResolveResult::k_invalid_resolved) continue;
            if ((size_t)rid >= nres.resolved.size()) continue;
            if ((SymbolId)nres.resolved[rid].sym != sym_id) continue;

            const TypeId t = type_of_ast_expr(tyck, (gaupel::ast::ExprId)eid);
            if (t != k_invalid_type) return t;
        }

        return k_invalid_type;
    }

    // -----------------------------
    // Place classification (v0 fixed)
    // -----------------------------
    static PlaceClass classify_place_from_ast(const gaupel::ast::AstArena& ast, gaupel::ast::ExprId eid) {
        if (eid == gaupel::ast::k_invalid_expr) return PlaceClass::kNotPlace;
        const auto& e = ast.expr(eid);

        switch (e.kind) {
            case gaupel::ast::ExprKind::kIdent:
                return PlaceClass::kLocal;
            case gaupel::ast::ExprKind::kIndex:
                return PlaceClass::kIndex;

            // future:
            // case gaupel::ast::ExprKind::kField: return PlaceClass::kField;
            default:
                return PlaceClass::kNotPlace;
        }
    }

    // -----------------------------
    // Effect classification (v0 fixed)
    // -----------------------------
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
        bool& out_has_any_write,
        const gaupel::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        gaupel::ast::ExprId eid
    );

    static uint32_t lower_stmt_into_block(
        Module& m,
        bool& out_has_any_write,
        const gaupel::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        gaupel::ast::StmtId sid
    );

    static BlockId lower_block_stmt(
        Module& m,
        bool& out_has_any_write,
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
            (void)lower_stmt_into_block(m, out_has_any_write, ast, sym, nres, tyck, child);
            m.blocks[bid].stmt_count++;
        }

        return bid;
    }

    // -----------------------------
    // helper: lower "maybe expr id" that might actually be a StmtId (legacy/quirk)
    // -----------------------------
    static bool is_valid_expr_id_(const gaupel::ast::AstArena& ast, gaupel::ast::ExprId id) {
        return id != gaupel::ast::k_invalid_expr && (size_t)id < ast.exprs().size();
    }
    static bool is_valid_stmt_id_(const gaupel::ast::AstArena& ast, gaupel::ast::StmtId id) {
        return id != gaupel::ast::k_invalid_stmt && (size_t)id < ast.stmts().size();
    }

    // Create a "block expression value" from a block stmt id (tail optional).
    static ValueId lower_block_value_(
        Module& m,
        bool& out_has_any_write,
        const gaupel::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        gaupel::ast::StmtId block_sid,
        gaupel::ast::ExprId tail_eid,
        gaupel::Span span,
        TypeId forced_type // optional: if you want to override; otherwise k_invalid_type
    ) {
        Value bv{};
        bv.kind = ValueKind::kBlockExpr;
        bv.span = span;

        // block expr "type" policy:
        // - prefer forced_type if provided
        // - else if tail exists, use tail type
        // - else unknown
        if (forced_type != k_invalid_type) {
            bv.type = forced_type;
        } else if (tail_eid != gaupel::ast::k_invalid_expr) {
            bv.type = type_of_ast_expr(tyck, tail_eid);
        } else {
            bv.type = k_invalid_type;
        }

        const BlockId bid = lower_block_stmt(m, out_has_any_write, ast, sym, nres, tyck, block_sid);
        bv.a = (ValueId)bid; // NOTE: BlockId stored in ValueId slot by convention.

        if (tail_eid != gaupel::ast::k_invalid_expr) {
            bv.b = lower_expr(m, out_has_any_write, ast, sym, nres, tyck, tail_eid);
        }

        bv.place = PlaceClass::kNotPlace;
        bv.effect = EffectClass::kPure; // the block may contain effects, but SIR value itself is structural
        return m.add_value(bv);
    }

    static ValueId lower_expr_or_stmt_as_value_(
        Module& m,
        bool& out_has_any_write,
        const gaupel::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        gaupel::ast::ExprId maybe_expr,
        gaupel::Span span,
        TypeId expected
    ) {
        // Normal path: ExprId.
        if (is_valid_expr_id_(ast, maybe_expr)) {
            return lower_expr(m, out_has_any_write, ast, sym, nres, tyck, maybe_expr);
        }

        // Legacy/quirk path: treat it as StmtId.
        const gaupel::ast::StmtId sid = (gaupel::ast::StmtId)maybe_expr;
        if (is_valid_stmt_id_(ast, sid)) {
            // wrap the statement-block as a block-expression value.
            return lower_block_value_(m, out_has_any_write, ast, sym, nres, tyck, sid,
                                      gaupel::ast::k_invalid_expr, span, expected);
        }

        // fallback
        return k_invalid_value;
    }

    static ValueId lower_expr(
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

        if (v.effect == EffectClass::kMayWrite) {
            out_has_any_write = true;
        }

        return m.add_value(v);
    }
        
    static uint32_t lower_stmt_into_block(
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

            default:
                out.kind = StmtKind::kError;
                break;
        }

        // statement-level write hint
        // (var decl itself is not necessarily a write; init may have write)
        return m.add_stmt(out);
    }

    static FnMode lower_fn_mode(gaupel::ast::FnMode m) {
        switch (m) {
            case gaupel::ast::FnMode::kPub: return FnMode::kPub;
            case gaupel::ast::FnMode::kSub: return FnMode::kSub;
            case gaupel::ast::FnMode::kNone:
            default: return FnMode::kNone;
        }
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
        (void)types;
        (void)opt;

        Module m{};

        // program root must be a block
        const auto& prog = ast.stmt(program_root);

        for (uint32_t i = 0; i < prog.stmt_count; ++i) {
            const auto sid = ast.stmt_children()[prog.stmt_begin + i];
            const auto& s = ast.stmt(sid);

            if (s.kind != ast::StmtKind::kFnDecl) {
                continue;
            }

            Func f{};
            f.span = s.span;
            f.name = s.name;
            
            // signature & ret
            f.sig = s.type;        // now guaranteed fn type
            f.ret = s.fn_ret;      // exact syntactic return type

            // decl symbol (fn name)
            f.sym = resolve_symbol_from_stmt(nres, sid);

            // --- qualifiers / mode (fn decl까지 보존) ---
            f.is_export = s.is_export;
            f.fn_mode = lower_fn_mode(s.fn_mode);

            f.is_pure = s.is_pure;
            f.is_comptime = s.is_comptime;

            f.is_commit = s.is_commit;
            f.is_recast = s.is_recast;

            f.is_throwing = s.is_throwing;

            f.positional_param_count = s.positional_param_count;
            f.has_named_group = s.has_named_group;

            // --- attrs slice ---
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

            // --- params slice ---
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

                // param SymbolId binding: now fixed via param_to_resolved table
                sp.sym = resolve_symbol_from_param_index(nres, param_index);

                m.add_param(sp);
                f.param_count++;
            }

            // --- body ---
            if (s.a != ast::k_invalid_stmt) {
                f.entry = lower_block_stmt(m, has_any_write, ast, sym, nres, tyck, s.a);
            }

            f.has_any_write = has_any_write;

            m.add_func(f);
        }

        return m;
    }

} // namespace gaupel::sir
