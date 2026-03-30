#pragma once

#include <parus/ast/Nodes.hpp>
#include <parus/syntax/TokenKind.hpp>

namespace parus::ast {

    inline bool is_range_index_expr(const AstArena& ast, ExprId eid) {
        if (eid == k_invalid_expr) return false;
        const auto& e = ast.expr(eid);
        if (e.kind != ExprKind::kBinary) return false;
        return e.op == syntax::TokenKind::kDotDot || e.op == syntax::TokenKind::kDotDotColon;
    }

    inline bool is_storage_place_expr(const AstArena& ast, ExprId eid) {
        if (eid == k_invalid_expr) return false;
        const auto& e = ast.expr(eid);

        switch (e.kind) {
            case ExprKind::kIdent:
                return true;

            case ExprKind::kIndex:
                if (is_range_index_expr(ast, e.b)) return false;
                return is_storage_place_expr(ast, e.a);

            case ExprKind::kBinary:
                if (e.op != syntax::TokenKind::kDot || e.b == k_invalid_expr) return false;
                if (const auto& rhs = ast.expr(e.b); rhs.kind != ExprKind::kIdent) return false;
                return is_storage_place_expr(ast, e.a);

            default:
                return false;
        }
    }

} // namespace parus::ast
