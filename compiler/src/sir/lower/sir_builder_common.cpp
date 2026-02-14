// compiler/src/sir/sir_builder_common.cpp
#include "sir_builder_internal.hpp"
#include <gaupel/syntax/TokenKind.hpp>


namespace gaupel::sir::detail {

    TypeId type_of_ast_expr(const tyck::TyckResult& tyck, gaupel::ast::ExprId eid) {
        if (eid == gaupel::ast::k_invalid_expr) return k_invalid_type;
        if ((size_t)eid >= tyck.expr_types.size()) return k_invalid_type;
        return tyck.expr_types[eid];
    }

    // -----------------------------
    // NameResolveResult 기반 심볼 resolve
    // -----------------------------
    SymbolId resolve_symbol_from_expr(
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

    SymbolId resolve_root_place_symbol_from_expr(
        const gaupel::ast::AstArena& ast,
        const passes::NameResolveResult& nres,
        gaupel::ast::ExprId eid
    ) {
        if (eid == gaupel::ast::k_invalid_expr) return k_invalid_symbol;
        const auto& e = ast.expr(eid);

        if (e.kind == gaupel::ast::ExprKind::kIdent) {
            return resolve_symbol_from_expr(nres, eid);
        }
        if (e.kind == gaupel::ast::ExprKind::kIndex) {
            return resolve_root_place_symbol_from_expr(ast, nres, e.a);
        }
        return k_invalid_symbol;
    }

    SymbolId resolve_symbol_from_stmt(
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

    SymbolId resolve_symbol_from_param_index(
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
    TypeId resolve_decl_type_from_symbol_uses(
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
    PlaceClass classify_place_from_ast(const gaupel::ast::AstArena& ast, gaupel::ast::ExprId eid) {
        if (eid == gaupel::ast::k_invalid_expr) return PlaceClass::kNotPlace;
        const auto& e = ast.expr(eid);

        switch (e.kind) {
            case gaupel::ast::ExprKind::kIdent:
                return PlaceClass::kLocal;
            case gaupel::ast::ExprKind::kIndex: {
                // slice range index(a..b / a..:b)는 view 생성식으로 취급하여 place에서 제외한다.
                if (e.b != gaupel::ast::k_invalid_expr) {
                    const auto& ie = ast.expr(e.b);
                    if (ie.kind == gaupel::ast::ExprKind::kBinary &&
                        (ie.op == gaupel::syntax::TokenKind::kDotDot ||
                         ie.op == gaupel::syntax::TokenKind::kDotDotColon)) {
                        return PlaceClass::kNotPlace;
                    }
                }
                return PlaceClass::kIndex;
            }

            // future:
            // case gaupel::ast::ExprKind::kField: return PlaceClass::kField;
            default:
                return PlaceClass::kNotPlace;
        }
    }

    // -----------------------------
    // Effect classification (v0 fixed)
    // -----------------------------
    EffectClass classify_effect(ValueKind k) {
        switch (k) {
            case ValueKind::kAssign:
            case ValueKind::kPostfixInc:
            case ValueKind::kEscape:
                return EffectClass::kMayWrite;

            case ValueKind::kCall:
                return EffectClass::kUnknown;

            default:
                return EffectClass::kPure;
        }
    }

    EffectClass join_effect_(EffectClass a, EffectClass b) {
        auto rank = [](EffectClass e) -> int {
            switch (e) {
                case EffectClass::kPure: return 0;
                case EffectClass::kMayWrite: return 1;
                case EffectClass::kUnknown: return 2;
            }
            return 2;
        };
        return (rank(a) >= rank(b)) ? a : b;
    }

    EffectClass effect_of_block_(const Module& m, BlockId bid);

    EffectClass effect_of_stmt_(const Module& m, const Stmt& s) {
        EffectClass eff = EffectClass::kPure;

        auto join_value = [&](ValueId vid) {
            if (vid != k_invalid_value && (size_t)vid < m.values.size()) {
                eff = join_effect_(eff, m.values[vid].effect);
            }
        };
        auto join_block = [&](BlockId blk) {
            eff = join_effect_(eff, effect_of_block_(m, blk));
        };

        switch (s.kind) {
            case StmtKind::kExprStmt:
            case StmtKind::kReturn:
            case StmtKind::kBreak:
                join_value(s.expr);
                break;

            case StmtKind::kVarDecl:
                join_value(s.init);
                break;

            case StmtKind::kIfStmt:
                join_value(s.expr);
                if (s.a != k_invalid_block) join_block(s.a);
                if (s.b != k_invalid_block) join_block(s.b);
                break;

            case StmtKind::kWhileStmt:
                join_value(s.expr);
                if (s.a != k_invalid_block) join_block(s.a);
                break;

            default:
                break;
        }

        return eff;
    }

    EffectClass effect_of_block_(const Module& m, BlockId bid) {
        if (bid == k_invalid_block || (size_t)bid >= m.blocks.size()) return EffectClass::kPure;
        const auto& b = m.blocks[bid];

        EffectClass eff = EffectClass::kPure;
        for (uint32_t i = 0; i < b.stmt_count; ++i) {
            const uint32_t sid = b.stmt_begin + i;
            if ((size_t)sid >= m.stmts.size()) break;
            eff = join_effect_(eff, effect_of_stmt_(m, m.stmts[sid]));
        }
        return eff;
    }

    // forward decl
    ValueId lower_expr(
        Module& m,
        bool& out_has_any_write,
        const gaupel::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        gaupel::ast::ExprId eid
    );

    Stmt lower_stmt_(
        Module& m,
        bool& out_has_any_write,
        const gaupel::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        gaupel::ast::StmtId sid
    );

    BlockId lower_block_stmt(
        Module& m,
        bool& out_has_any_write,
        const gaupel::ast::AstArena& ast,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        gaupel::ast::StmtId block_sid
    ) {
        const auto& bs = ast.stmt(block_sid);
        if (bs.kind != gaupel::ast::StmtKind::kBlock) {
            return k_invalid_block;
        }

        Block b{};
        b.span = bs.span;
        b.stmt_begin = (uint32_t)m.stmts.size();
        b.stmt_count = bs.stmt_count;

        BlockId bid = m.add_block(b);

        // Reserve fixed contiguous slots for this block's direct statements.
        // Nested blocks lower into slots appended after this reserved range.
        m.stmts.resize((size_t)m.blocks[bid].stmt_begin + (size_t)m.blocks[bid].stmt_count);

        for (uint32_t i = 0; i < bs.stmt_count; ++i) {
            const auto child = ast.stmt_children()[bs.stmt_begin + i];
            m.stmts[m.blocks[bid].stmt_begin + i] =
                lower_stmt_(m, out_has_any_write, ast, sym, nres, tyck, child);
        }

        return bid;
    }

    // -----------------------------
    // helper: lower "maybe expr id" that might actually be a StmtId (legacy/quirk)
    // -----------------------------
    bool is_valid_expr_id_(const gaupel::ast::AstArena& ast, gaupel::ast::ExprId id) {
        return id != gaupel::ast::k_invalid_expr && (size_t)id < ast.exprs().size();
    }
    bool is_valid_stmt_id_(const gaupel::ast::AstArena& ast, gaupel::ast::StmtId id) {
        return id != gaupel::ast::k_invalid_stmt && (size_t)id < ast.stmts().size();
    }

    // Create a "block expression value" from a block stmt id (tail optional).
    ValueId lower_block_value_(
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
        bv.effect = effect_of_block_(m, bid);
        if (bv.b != k_invalid_value && (size_t)bv.b < m.values.size()) {
            bv.effect = join_effect_(bv.effect, m.values[bv.b].effect);
        }
        if (bv.effect == EffectClass::kMayWrite) {
            out_has_any_write = true;
        }
        return m.add_value(bv);
    }

    ValueId lower_expr_or_stmt_as_value_(
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

} // namespace gaupel::sir::detail
