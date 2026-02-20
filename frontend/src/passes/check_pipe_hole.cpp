// frontend/src/passes/check_pipe_hole.cpp
#include <parus/ast/Nodes.hpp>
#include <parus/ast/Visitor.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/diag/DiagCode.hpp>
#include <parus/syntax/TokenKind.hpp>

namespace parus::passes {

    static void report(diag::Bag& bag, diag::Code code, Span span, int v0 = -1) {
        diag::Diagnostic d(diag::Severity::kError, code, span);
        if (v0 >= 0) d.add_arg_int(v0);
        bag.add(std::move(d));
    }

    static bool is_call(const ast::Expr& e)      { return e.kind == ast::ExprKind::kCall; }
    static bool is_hole_expr(const ast::Expr& e) { return e.kind == ast::ExprKind::kHole; }

    struct PipeScan {
        bool any_labeled = false;
        bool any_positional = false;
        int hole_count = 0;
    };

    static void scan_arg_list_for_pipe(
        const ast::AstArena& ast,
        const std::vector<ast::Arg>& args,
        uint32_t begin,
        uint32_t count,
        diag::Bag& bag,
        PipeScan& scan
    ) {
        for (uint32_t i = 0; i < count; ++i) {
            const auto& a = args[begin + i];

            // labeled / positional 분류
            const bool labeled = a.has_label;
            scan.any_labeled |= labeled;
            scan.any_positional |= !labeled;

            // hole 카운트/규칙
            if (a.is_hole) {
                ++scan.hole_count;
                if (!labeled) {
                    report(bag, diag::Code::kPipeHoleMustBeLabeled, a.span);
                }
                continue;
            }

            // expr 쪽에 "_"가 들어온 경우(특히 positional "_" 같은 케이스) 방지
            if (a.expr != ast::k_invalid_expr) {
                const auto& ex = ast.expr(a.expr);
                if (is_hole_expr(ex)) {
                    report(bag, diag::Code::kPipeHolePositionalNotAllowed, ex.span);
                }
            }
        }
    }

    // call_expr: pipe에 사용되는 "함수 호출" Expr
    static void validate_pipe_call(const ast::AstArena& ast, const ast::Expr& call_expr, diag::Bag& bag) {
        PipeScan scan{};

        // call의 기본 인자 리스트 스캔
        const auto& args = ast.args();
        scan_arg_list_for_pipe(ast, args, call_expr.arg_begin, call_expr.arg_count, bag, scan);

        // labeled/positional 혼용 금지
        if (scan.any_labeled && scan.any_positional) {
            report(bag, diag::Code::kCallArgMixNotAllowed, call_expr.span);
        }

        // hole 개수는 정확히 1개
        if (scan.hole_count != 1) {
            report(bag, diag::Code::kPipeHoleCountMismatch, call_expr.span, scan.hole_count);
        }
    }

    static void check_pipe(const ast::AstArena& ast, const ast::Expr& pipe_expr, diag::Bag& bag) {
        const auto& lhs = ast.expr(pipe_expr.a);
        const auto& rhs = ast.expr(pipe_expr.b);

        // |> : RHS가 call 이어야 함
        if (pipe_expr.op == syntax::TokenKind::kPipeFwd) {
            if (!is_call(rhs)) {
                report(bag, diag::Code::kPipeFwdRhsMustBeCall, pipe_expr.span);
                return;
            }
            validate_pipe_call(ast, rhs, bag);
            return;
        }

        // <| : LHS가 call 이어야 함
        if (pipe_expr.op == syntax::TokenKind::kPipeRev) {
            if (!is_call(lhs)) {
                report(bag, diag::Code::kPipeRevLhsMustBeCall, pipe_expr.span);
                return;
            }
            validate_pipe_call(ast, lhs, bag);
            return;
        }
    }

    class PipeHoleVisitor final : public ast::TreeVisitor {
    public:
        PipeHoleVisitor(const ast::AstArena& ast, diag::Bag& bag) : ast_(ast), bag_(bag) {}

        void enter_expr(ast::ExprId, const ast::Expr& e) override {
            if (e.kind != ast::ExprKind::kBinary) return;
            if (e.op != syntax::TokenKind::kPipeFwd && e.op != syntax::TokenKind::kPipeRev) return;
            check_pipe(ast_, e, bag_);
        }

    private:
        const ast::AstArena& ast_;
        diag::Bag& bag_;
    };

    void check_pipe_hole(const ast::AstArena& ast, ast::ExprId root, diag::Bag& bag) {
        PipeHoleVisitor visitor(ast, bag);
        ast::visit_expr_tree(ast, root, visitor);
    }

} // namespace parus::passes
