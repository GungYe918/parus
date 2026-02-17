// frontend/src/passes/check_place_expr.cpp
#include <parus/passes/CheckPlaceExpr.hpp>

#include <parus/ast/Nodes.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/diag/DiagCode.hpp>
#include <parus/syntax/TokenKind.hpp>

namespace parus::passes {

    static void report(diag::Bag& bag, diag::Code code, Span span) {
        diag::Diagnostic d(diag::Severity::kError, code, span);
        bag.add(std::move(d));
    }

    static bool is_place_expr(const ast::AstArena& ast, ast::ExprId id) {
        if (id == ast::k_invalid_expr) return false;

        const auto& e = ast.expr(id);

        // v0 기준 place 후보:
        // - Ident
        // - Index(base[...])  (base도 place여야 함)
        switch (e.kind) {
            case ast::ExprKind::kIdent:
                return true;

            case ast::ExprKind::kIndex:
                // base가 place면 base[index]도 place로 인정
                return is_place_expr(ast, e.a);

            default:
                return false;
        }
    }

    static bool is_borrow_unary(const ast::Expr& e) {
        return (e.kind == ast::ExprKind::kUnary) && (e.op == syntax::TokenKind::kAmp);
    }

    static void walk_expr(const ast::AstArena& ast, ast::ExprId id, diag::Bag& bag);

    static void walk_call_args(
        const ast::AstArena& ast,
        uint32_t arg_begin,
        uint32_t arg_count,
        diag::Bag& bag
    ) {
        const auto& args = ast.args();
        for (uint32_t i = 0; i < arg_count; ++i) {
            const auto& a = args[arg_begin + i];

            if (a.kind == ast::ArgKind::kNamedGroup) {
                const auto& ng = ast.named_group_args();
                for (uint32_t j = 0; j < a.child_count; ++j) {
                    const auto& entry = ng[a.child_begin + j];
                    if (!entry.is_hole && entry.expr != ast::k_invalid_expr) {
                        walk_expr(ast, entry.expr, bag);
                    }
                }
                continue;
            }

            if (!a.is_hole && a.expr != ast::k_invalid_expr) {
                walk_expr(ast, a.expr, bag);
            }
        }
    }

    static void check_unary_place_rules(const ast::AstArena& ast, const ast::Expr& u, diag::Bag& bag) {
        // & / && 는 operand가 place여야 함
        if (u.op == syntax::TokenKind::kAmp) {
            if (!is_place_expr(ast, u.a)) {
                report(bag, diag::Code::kBorrowOperandMustBePlace, u.span);
            }
            return;
        }

        if (u.op == syntax::TokenKind::kAmpAmp) {
            // 1) place여야 함
            if (!is_place_expr(ast, u.a)) {
                report(bag, diag::Code::kEscapeOperandMustBePlace, u.span);
                return;
            }

            // 2) "borrow에는 적용 불가" (&&(&x) 같은 형태를 금지하고 싶다면)
            //    - 현재 AST에선 &&의 operand가 &x 라면 operand는 Unary(&)이고 place가 아니므로 위에서 이미 걸림.
            //    - 그래도 혹시 place 판정이 확장되면(예: deref 등) 대비용으로 한 번 더 방어.
            const auto& opnd = ast.expr(u.a);
            if (is_borrow_unary(opnd)) {
                report(bag, diag::Code::kEscapeOperandMustNotBeBorrow, u.span);
            }
            return;
        }
    }

    static void walk_expr(const ast::AstArena& ast, ast::ExprId id, diag::Bag& bag) {
        if (id == ast::k_invalid_expr) return;

        const auto& e = ast.expr(id);

        switch (e.kind) {
            case ast::ExprKind::kUnary:
            case ast::ExprKind::kPostfixUnary:
                check_unary_place_rules(ast, e, bag);
                walk_expr(ast, e.a, bag);
                break;

            case ast::ExprKind::kCast:
                // cast는 place가 아니지만 operand는 검사해야 함
                walk_expr(ast, e.a, bag);
                break;

            case ast::ExprKind::kBinary:
            case ast::ExprKind::kAssign:
                walk_expr(ast, e.a, bag);
                walk_expr(ast, e.b, bag);
                break;

            case ast::ExprKind::kTernary:
                walk_expr(ast, e.a, bag);
                walk_expr(ast, e.b, bag);
                walk_expr(ast, e.c, bag);
                break;

            case ast::ExprKind::kCall:
                walk_expr(ast, e.a, bag);
                walk_call_args(ast, e.arg_begin, e.arg_count, bag);
                break;

            case ast::ExprKind::kIndex:
                walk_expr(ast, e.a, bag);
                walk_expr(ast, e.b, bag);
                break;

            case ast::ExprKind::kLoop:
                if (e.loop_iter != ast::k_invalid_expr) walk_expr(ast, e.loop_iter, bag);
                break;

            case ast::ExprKind::kFieldInit: {
                const auto& inits = ast.field_init_entries();
                const uint64_t begin = e.field_init_begin;
                const uint64_t end = begin + e.field_init_count;
                if (begin <= inits.size() && end <= inits.size()) {
                    for (uint32_t i = 0; i < e.field_init_count; ++i) {
                        const auto& ent = inits[e.field_init_begin + i];
                        if (ent.expr != ast::k_invalid_expr) walk_expr(ast, ent.expr, bag);
                    }
                }
                break;
            }

            default:
                break;
        }
    }

    void check_place_expr(const ast::AstArena& ast, ast::ExprId root, diag::Bag& bag) {
        walk_expr(ast, root, bag);
    }

} // namespace parus::passes
