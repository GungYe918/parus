// compiler/src/passes/check_pipe_hole.cpp
#include <gaupel/ast/Nodes.hpp>
#include <gaupel/diag/Diagnostic.hpp>
#include <gaupel/diag/DiagCode.hpp>
#include <gaupel/syntax/TokenKind.hpp>


namespace gaupel::passes {

    static void report(diag::Bag& bag, diag::Code code, Span span, int v0 = -1) {
        diag::Diagnostic d(diag::Severity::kError, code, span);
        if (v0 >= 0) d.add_arg_int(v0);
        bag.add(std::move(d));
    }

    static bool is_call(const ast::Expr& e)      {  return e.kind == ast::ExprKind::kCall;  }
    static bool is_hole_expr(const ast::Expr& e) {  return e.kind == ast::ExprKind::kHole;  }

    static void walk_expr(const ast::AstArena& ast, ast::ExprId id, diag::Bag& bag);

    static void check_pipe(const ast::AstArena& ast, const ast::Expr& pipe_expr, diag::Bag& bag) {
        const auto& rhs = ast.expr(pipe_expr.b);

        if (!is_call(rhs)) {
            report(bag, diag::Code::kPipeRhsMustBeCall, pipe_expr.span);
            return;
        }

        // 1) labeled/positional mix check inside call
        bool any_labeled = false;
        bool any_positional = false;

        int hole_count = 0;

        const auto& args = ast.args();
        for (uint32_t i = 0; i < rhs.arg_count; ++i) {
            const auto& a = args[rhs.arg_begin + i];
            any_labeled |= a.has_label;
            any_positional |= !a.has_label;

            if (a.is_hole) {
                ++hole_count;
                if (!a.has_label) {
                    report(bag, diag::Code::kPipeHoleMustBeLabeled, a.span);
                }
            } else if (a.expr != ast::k_invalid_expr) {
                const auto& ex = ast.expr(a.expr);
                if (is_hole_expr(ex)) {
                    // positional "_" or "_" used as expression -> not allowed for pipe
                    report(bag, diag::Code::kPipeHolePositionalNotAllowed, ex.span);
                }
            }
        }

        if (any_labeled && any_positional) {
            report(bag, diag::Code::kCallArgMixNotAllowed, rhs.span);
        }

        if (hole_count != 1) {
            report(bag, diag::Code::kPipeHoleCountMismatch, rhs.span, hole_count);
        }
    }

    static void walk_expr(const ast::AstArena& ast, ast::ExprId id, diag::Bag& bag) {
        const auto& e = ast.expr(id);

        switch(e.kind) {
            case ast::ExprKind::kUnary:
            case ast::ExprKind::kPostfixUnary:
                walk_expr(ast, e.a, bag);
                break;

            case ast::ExprKind::kBinary:
                // pipe operator
                if (e.op == syntax::TokenKind::kLessLess) {
                    check_pipe(ast, e, bag);
                }
                walk_expr(ast, e.a, bag);
                walk_expr(ast, e.b, bag);
                break;

            case ast::ExprKind::kTernary:
                walk_expr(ast, e.a, bag);
                walk_expr(ast, e.b, bag);
                walk_expr(ast, e.c, bag);
                break;

            case ast::ExprKind::kCall: {
                walk_expr(ast, e.a, bag);
                const auto& args = ast.args();
                for (uint32_t i = 0; i < e.arg_count; ++i) {
                    const auto& a = args[e.arg_begin + i];
                    if (!a.is_hole && a.expr != ast::k_invalid_expr) {
                        walk_expr(ast, a.expr, bag);
                    }
                }
                break;
            }

            case ast::ExprKind::kIndex:
                walk_expr(ast, e.a, bag);
                walk_expr(ast, e.b, bag);
                break;

            default:
                break;
        }
    }

    void check_pipe_hole(const ast::AstArena& ast, ast::ExprId root, diag::Bag& bag) {
        walk_expr(ast, root, bag);
    }


} // namespace gaupel::passes