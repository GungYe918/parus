// frontend/src/passes/canonicalize_pipe.cpp
#include <parus/passes/CanonicalizePipe.hpp>

#include <parus/ast/Visitor.hpp>
#include <parus/diag/DiagCode.hpp>
#include <parus/syntax/TokenKind.hpp>

#include <vector>

namespace parus::passes {

    namespace {

        struct PipeExprCollector final : public ast::TreeVisitor {
            std::vector<ast::ExprId> ids;

            void leave_expr(ast::ExprId id, const ast::Expr& e) override {
                if (e.kind != ast::ExprKind::kBinary) return;
                if (e.op == syntax::TokenKind::kPipeFwd ||
                    e.op == syntax::TokenKind::kPipeRev) {
                    ids.push_back(id);
                }
            }
        };

        bool has_labeled_hole_shape_(
            const ast::AstArena& ast,
            const ast::Expr& call,
            uint32_t& out_hole_pos
        ) {
            out_hole_pos = 0xFFFF'FFFFu;
            if (call.kind != ast::ExprKind::kCall) return false;

            const auto& args = ast.args();
            const uint64_t begin = call.arg_begin;
            const uint64_t end = begin + call.arg_count;
            if (begin > args.size() || end > args.size()) return false;

            uint32_t hole_count = 0;
            for (uint32_t i = 0; i < call.arg_count; ++i) {
                const auto& a = args[call.arg_begin + i];
                if (!a.is_hole) continue;

                const bool labeled = a.has_label || a.kind == ast::ArgKind::kLabeled;
                if (!labeled) return false;

                ++hole_count;
                out_hole_pos = i;
            }
            return hole_count == 1;
        }

        void rewrite_pipe_forward_(
            ast::AstArena& ast,
            ast::ExprId pipe_eid
        ) {
            if (pipe_eid == ast::k_invalid_expr || (size_t)pipe_eid >= ast.exprs().size()) return;
            const ast::Expr pipe = ast.expr(pipe_eid);
            if (pipe.kind != ast::ExprKind::kBinary || pipe.op != syntax::TokenKind::kPipeFwd) return;
            if (pipe.a == ast::k_invalid_expr || pipe.b == ast::k_invalid_expr) return;
            if ((size_t)pipe.b >= ast.exprs().size()) return;

            const ast::Expr rhs_call = ast.expr(pipe.b);
            if (rhs_call.kind != ast::ExprKind::kCall) return;

            uint32_t hole_pos = 0xFFFF'FFFFu;
            if (!has_labeled_hole_shape_(ast, rhs_call, hole_pos)) return;

            const auto& args = ast.args();
            const uint64_t begin = rhs_call.arg_begin;
            const uint64_t end = begin + rhs_call.arg_count;
            if (begin > args.size() || end > args.size()) return;

            std::vector<ast::Arg> copied_args;
            copied_args.reserve(rhs_call.arg_count);
            for (uint32_t i = 0; i < rhs_call.arg_count; ++i) {
                ast::Arg a = args[rhs_call.arg_begin + i];
                if (a.is_hole && i == hole_pos) {
                    a.is_hole = false;
                    a.expr = pipe.a; // lhs injected once
                }
                copied_args.push_back(a);
            }

            ast::Expr rewritten = rhs_call;
            rewritten.span = pipe.span;
            rewritten.arg_begin = static_cast<uint32_t>(ast.args().size());
            rewritten.arg_count = rhs_call.arg_count;
            rewritten.call_from_pipe = true;

            for (const auto& a : copied_args) {
                ast.add_arg(a);
            }

            ast.expr_mut(pipe_eid) = rewritten;
        }

    } // namespace

    void canonicalize_pipe(ast::AstArena& ast, ast::StmtId root, diag::Bag& bag) {
        if (root == ast::k_invalid_stmt) return;

        PipeExprCollector collector;
        ast::visit_stmt_tree(ast, root, collector);

        for (const ast::ExprId eid : collector.ids) {
            if (eid == ast::k_invalid_expr || (size_t)eid >= ast.exprs().size()) continue;
            const ast::Expr cur = ast.expr(eid);
            if (cur.kind != ast::ExprKind::kBinary) continue;

            if (cur.op == syntax::TokenKind::kPipeRev) {
                diag::Diagnostic d(diag::Severity::kError, diag::Code::kPipeRevNotSupportedYet, cur.span);
                bag.add(std::move(d));

                ast::Expr err{};
                err.kind = ast::ExprKind::kError;
                err.span = cur.span;
                err.text = "pipe_reverse_not_supported";
                ast.expr_mut(eid) = err;
                continue;
            }

            if (cur.op != syntax::TokenKind::kPipeFwd) continue;
            rewrite_pipe_forward_(ast, eid);
        }
    }

} // namespace parus::passes
