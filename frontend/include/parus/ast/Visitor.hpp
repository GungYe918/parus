#pragma once

#include <parus/ast/Nodes.hpp>

namespace parus::ast {

    class TreeVisitor {
    public:
        virtual ~TreeVisitor() = default;

        virtual void enter_stmt(StmtId, const Stmt&) {}
        virtual void leave_stmt(StmtId, const Stmt&) {}

        virtual void enter_expr(ExprId, const Expr&) {}
        virtual void leave_expr(ExprId, const Expr&) {}
    };

    namespace detail {

        inline void visit_expr_inner(const AstArena& ast, ExprId id, TreeVisitor& v);
        inline void visit_stmt_inner(const AstArena& ast, StmtId id, TreeVisitor& v);

        inline void visit_call_args_inner(const AstArena& ast, const Expr& e, TreeVisitor& v) {
            const auto& args = ast.args();
            const uint64_t begin = e.arg_begin;
            const uint64_t end = begin + e.arg_count;
            if (begin > args.size() || end > args.size()) return;

            for (uint32_t i = 0; i < e.arg_count; ++i) {
                const auto& a = args[e.arg_begin + i];
                if (a.expr != k_invalid_expr && !a.is_hole) {
                    visit_expr_inner(ast, a.expr, v);
                }
            }
        }

        inline void visit_field_init_inner(const AstArena& ast, const Expr& e, TreeVisitor& v) {
            const auto& inits = ast.field_init_entries();
            const uint64_t begin = e.field_init_begin;
            const uint64_t end = begin + e.field_init_count;
            if (begin > inits.size() || end > inits.size()) return;

            for (uint32_t i = 0; i < e.field_init_count; ++i) {
                const auto& ent = inits[e.field_init_begin + i];
                if (ent.expr != k_invalid_expr) visit_expr_inner(ast, ent.expr, v);
            }
        }

        inline void visit_expr_inner(const AstArena& ast, ExprId id, TreeVisitor& v) {
            if (id == k_invalid_expr) return;
            const auto& e = ast.expr(id);
            v.enter_expr(id, e);

            switch (e.kind) {
                case ExprKind::kUnary:
                case ExprKind::kPostfixUnary:
                case ExprKind::kCast:
                    visit_expr_inner(ast, e.a, v);
                    break;

                case ExprKind::kBinary:
                case ExprKind::kAssign:
                    visit_expr_inner(ast, e.a, v);
                    visit_expr_inner(ast, e.b, v);
                    break;

                case ExprKind::kTernary:
                case ExprKind::kIfExpr:
                    visit_expr_inner(ast, e.a, v);
                    visit_expr_inner(ast, e.b, v);
                    visit_expr_inner(ast, e.c, v);
                    break;

                case ExprKind::kCall:
                    visit_expr_inner(ast, e.a, v);
                    visit_call_args_inner(ast, e, v);
                    break;

                case ExprKind::kIndex:
                    visit_expr_inner(ast, e.a, v);
                    visit_expr_inner(ast, e.b, v);
                    break;

                case ExprKind::kLoop:
                    visit_expr_inner(ast, e.loop_iter, v);
                    visit_stmt_inner(ast, e.loop_body, v);
                    break;

                case ExprKind::kBlockExpr:
                    // e.a stores block stmt id by parser convention.
                    if (e.a != k_invalid_expr) {
                        visit_stmt_inner(ast, static_cast<StmtId>(e.a), v);
                    }
                    visit_expr_inner(ast, e.b, v);
                    break;

                case ExprKind::kFieldInit:
                    visit_field_init_inner(ast, e, v);
                    break;

                default:
                    break;
            }

            v.leave_expr(id, e);
        }

        inline void visit_stmt_inner(const AstArena& ast, StmtId id, TreeVisitor& v) {
            if (id == k_invalid_stmt) return;
            const auto& s = ast.stmt(id);
            v.enter_stmt(id, s);

            switch (s.kind) {
                case StmtKind::kExprStmt:
                    visit_expr_inner(ast, s.expr, v);
                    break;

                case StmtKind::kVar:
                case StmtKind::kReturn:
                    visit_expr_inner(ast, s.init, v);
                    visit_expr_inner(ast, s.expr, v);
                    break;

                case StmtKind::kIf:
                    visit_expr_inner(ast, s.expr, v);
                    visit_stmt_inner(ast, s.a, v);
                    visit_stmt_inner(ast, s.b, v);
                    break;

                case StmtKind::kWhile:
                    visit_expr_inner(ast, s.expr, v);
                    visit_stmt_inner(ast, s.a, v);
                    break;

                case StmtKind::kDoScope:
                    visit_stmt_inner(ast, s.a, v);
                    break;

                case StmtKind::kDoWhile:
                    visit_stmt_inner(ast, s.a, v);
                    visit_expr_inner(ast, s.expr, v);
                    break;

                case StmtKind::kManual:
                    visit_stmt_inner(ast, s.a, v);
                    break;

                case StmtKind::kSwitch: {
                    visit_expr_inner(ast, s.expr, v);
                    const auto& cases = ast.switch_cases();
                    const uint64_t begin = s.case_begin;
                    const uint64_t end = begin + s.case_count;
                    if (begin <= cases.size() && end <= cases.size()) {
                        for (uint32_t i = 0; i < s.case_count; ++i) {
                            visit_stmt_inner(ast, cases[s.case_begin + i].body, v);
                        }
                    }
                    break;
                }

                case StmtKind::kFnDecl: {
                    const auto& params = ast.params();
                    const uint64_t begin = s.param_begin;
                    const uint64_t end = begin + s.param_count;
                    if (begin <= params.size() && end <= params.size()) {
                        for (uint32_t i = 0; i < s.param_count; ++i) {
                            const auto& p = params[s.param_begin + i];
                            if (p.has_default) visit_expr_inner(ast, p.default_expr, v);
                        }
                    }
                    visit_stmt_inner(ast, s.a, v);
                    break;
                }

                case StmtKind::kActsDecl:
                case StmtKind::kBlock: {
                    const auto& kids = ast.stmt_children();
                    const uint64_t begin = s.stmt_begin;
                    const uint64_t end = begin + s.stmt_count;
                    if (begin <= kids.size() && end <= kids.size()) {
                        for (uint32_t i = 0; i < s.stmt_count; ++i) {
                            visit_stmt_inner(ast, kids[s.stmt_begin + i], v);
                        }
                    }
                    break;
                }

                case StmtKind::kNestDecl:
                    if (!s.nest_is_file_directive) visit_stmt_inner(ast, s.a, v);
                    break;

                case StmtKind::kUse:
                    visit_expr_inner(ast, s.expr, v);
                    break;

                default:
                    break;
            }

            v.leave_stmt(id, s);
        }

    } // namespace detail

    inline void visit_expr_tree(const AstArena& ast, ExprId root, TreeVisitor& visitor) {
        detail::visit_expr_inner(ast, root, visitor);
    }

    inline void visit_stmt_tree(const AstArena& ast, StmtId root, TreeVisitor& visitor) {
        detail::visit_stmt_inner(ast, root, visitor);
    }

} // namespace parus::ast
