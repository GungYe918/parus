// frontend/src/passes/check_place_expr.cpp
#include <parus/passes/CheckPlaceExpr.hpp>

#include <parus/ast/Nodes.hpp>
#include <parus/ast/Visitor.hpp>
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

    void check_place_expr_node(const ast::AstArena& ast, ast::ExprId id, diag::Bag& bag) {
        if (id == ast::k_invalid_expr) return;
        const auto& e = ast.expr(id);
        if (e.kind == ast::ExprKind::kUnary || e.kind == ast::ExprKind::kPostfixUnary) {
            check_unary_place_rules(ast, e, bag);
        }
    }

    class PlaceExprVisitor final : public ast::TreeVisitor {
    public:
        PlaceExprVisitor(const ast::AstArena& ast, diag::Bag& bag) : ast_(ast), bag_(bag) {}

        void enter_expr(ast::ExprId id, const ast::Expr&) override {
            check_place_expr_node(ast_, id, bag_);
        }

    private:
        const ast::AstArena& ast_;
        diag::Bag& bag_;
    };

    void check_place_expr(const ast::AstArena& ast, ast::ExprId root, diag::Bag& bag) {
        PlaceExprVisitor visitor(ast, bag);
        ast::visit_expr_tree(ast, root, visitor);
    }

} // namespace parus::passes
