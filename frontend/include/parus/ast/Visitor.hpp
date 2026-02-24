#pragma once

#include <parus/ast/Nodes.hpp>

namespace parus::ast {

    enum class ExprChildRole : uint8_t {
        kUnaryOperand,
        kCastOperand,
        kBinaryLhs,
        kBinaryRhs,
        kAssignLhs,
        kAssignRhs,
        kTernaryCond,
        kTernaryThen,
        kTernaryElse,
        kIfCond,
        kIfThen,
        kIfElse,
        kCallCallee,
        kCallArg,
        kIndexBase,
        kIndexSubscript,
        kLoopIter,
        kBlockExprStmt,
        kBlockExprTail,
        kFieldInitValue,
    };

    enum class StmtChildRole : uint8_t {
        kExprStmtExpr,
        kVarInit,
        kVarExpr,
        kReturnExpr,
        kIfCond,
        kIfThen,
        kIfElse,
        kWhileCond,
        kWhileBody,
        kDoScopeBody,
        kDoWhileBody,
        kDoWhileCond,
        kManualBody,
        kSwitchCond,
        kSwitchCaseBody,
        kFnParamDefault,
        kFnBody,
        kBlockChild,
        kActsMember,
        kNestBody,
        kUseExpr,
    };

    class TreeVisitor {
    public:
        virtual ~TreeVisitor() = default;

        virtual bool should_descend_stmt(StmtId, const Stmt&) { return true; }
        virtual bool should_descend_expr(ExprId, const Expr&) { return true; }

        virtual bool should_visit_expr_child(ExprId, const Expr&, ExprChildRole, ExprId) { return true; }
        virtual bool should_visit_stmt_child(StmtId, const Stmt&, StmtChildRole, StmtId) { return true; }

        virtual void enter_stmt(StmtId, const Stmt&) {}
        virtual void leave_stmt(StmtId, const Stmt&) {}

        virtual void enter_expr(ExprId, const Expr&) {}
        virtual void leave_expr(ExprId, const Expr&) {}
    };

    namespace detail {

        inline void visit_expr_inner(const AstArena& ast, ExprId id, TreeVisitor& v);
        inline void visit_stmt_inner(const AstArena& ast, StmtId id, TreeVisitor& v);

        inline void visit_expr_child_if_(const AstArena& ast,
                                         ExprId parent_id,
                                         const Expr& parent,
                                         ExprChildRole role,
                                         ExprId child_id,
                                         TreeVisitor& v) {
            if (child_id == k_invalid_expr) return;
            if (!v.should_visit_expr_child(parent_id, parent, role, child_id)) return;
            visit_expr_inner(ast, child_id, v);
        }

        inline void visit_stmt_child_if_(const AstArena& ast,
                                         StmtId parent_id,
                                         const Stmt& parent,
                                         StmtChildRole role,
                                         StmtId child_id,
                                         TreeVisitor& v) {
            if (child_id == k_invalid_stmt) return;
            if (!v.should_visit_stmt_child(parent_id, parent, role, child_id)) return;
            visit_stmt_inner(ast, child_id, v);
        }

        inline void visit_call_args_inner(const AstArena& ast, ExprId id, const Expr& e, TreeVisitor& v) {
            const auto& args = ast.args();
            const uint64_t begin = e.arg_begin;
            const uint64_t end = begin + e.arg_count;
            if (begin > args.size() || end > args.size()) return;

            for (uint32_t i = 0; i < e.arg_count; ++i) {
                const auto& a = args[e.arg_begin + i];
                if (a.expr != k_invalid_expr && !a.is_hole) {
                    if (v.should_visit_expr_child(id, e, ExprChildRole::kCallArg, a.expr)) {
                        visit_expr_inner(ast, a.expr, v);
                    }
                }
            }
        }

        inline void visit_field_init_inner(const AstArena& ast,
                                           ExprId id,
                                           const Expr& e,
                                           TreeVisitor& v) {
            const auto& inits = ast.field_init_entries();
            const uint64_t begin = e.field_init_begin;
            const uint64_t end = begin + e.field_init_count;
            if (begin > inits.size() || end > inits.size()) return;

            for (uint32_t i = 0; i < e.field_init_count; ++i) {
                const auto& ent = inits[e.field_init_begin + i];
                if (ent.expr != k_invalid_expr &&
                    v.should_visit_expr_child(id, e, ExprChildRole::kFieldInitValue, ent.expr))
                {
                    visit_expr_inner(ast, ent.expr, v);
                }
            }
        }

        inline void visit_expr_inner(const AstArena& ast, ExprId id, TreeVisitor& v) {
            if (id == k_invalid_expr) return;
            const auto& e = ast.expr(id);
            v.enter_expr(id, e);
            if (!v.should_descend_expr(id, e)) {
                v.leave_expr(id, e);
                return;
            }

            switch (e.kind) {
                case ExprKind::kUnary:
                case ExprKind::kPostfixUnary:
                    visit_expr_child_if_(ast, id, e, ExprChildRole::kUnaryOperand, e.a, v);
                    break;

                case ExprKind::kCast:
                    visit_expr_child_if_(ast, id, e, ExprChildRole::kCastOperand, e.a, v);
                    break;

                case ExprKind::kBinary:
                    visit_expr_child_if_(ast, id, e, ExprChildRole::kBinaryLhs, e.a, v);
                    visit_expr_child_if_(ast, id, e, ExprChildRole::kBinaryRhs, e.b, v);
                    break;

                case ExprKind::kAssign:
                    visit_expr_child_if_(ast, id, e, ExprChildRole::kAssignLhs, e.a, v);
                    visit_expr_child_if_(ast, id, e, ExprChildRole::kAssignRhs, e.b, v);
                    break;

                case ExprKind::kTernary:
                    visit_expr_child_if_(ast, id, e, ExprChildRole::kTernaryCond, e.a, v);
                    visit_expr_child_if_(ast, id, e, ExprChildRole::kTernaryThen, e.b, v);
                    visit_expr_child_if_(ast, id, e, ExprChildRole::kTernaryElse, e.c, v);
                    break;

                case ExprKind::kIfExpr:
                    visit_expr_child_if_(ast, id, e, ExprChildRole::kIfCond, e.a, v);
                    visit_expr_child_if_(ast, id, e, ExprChildRole::kIfThen, e.b, v);
                    visit_expr_child_if_(ast, id, e, ExprChildRole::kIfElse, e.c, v);
                    break;

                case ExprKind::kCall:
                    visit_expr_child_if_(ast, id, e, ExprChildRole::kCallCallee, e.a, v);
                    visit_call_args_inner(ast, id, e, v);
                    break;

                case ExprKind::kIndex:
                    visit_expr_child_if_(ast, id, e, ExprChildRole::kIndexBase, e.a, v);
                    visit_expr_child_if_(ast, id, e, ExprChildRole::kIndexSubscript, e.b, v);
                    break;

                case ExprKind::kLoop:
                    visit_expr_child_if_(ast, id, e, ExprChildRole::kLoopIter, e.loop_iter, v);
                    visit_stmt_inner(ast, e.loop_body, v);
                    break;

                case ExprKind::kBlockExpr:
                    if (e.block_stmt != k_invalid_stmt) {
                        visit_stmt_inner(ast, e.block_stmt, v);
                    }
                    visit_expr_child_if_(ast, id, e, ExprChildRole::kBlockExprTail, e.block_tail, v);
                    break;

                case ExprKind::kFieldInit:
                    visit_field_init_inner(ast, id, e, v);
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
            if (!v.should_descend_stmt(id, s)) {
                v.leave_stmt(id, s);
                return;
            }

            switch (s.kind) {
                case StmtKind::kExprStmt:
                    visit_expr_inner(ast, s.expr, v);
                    break;

                case StmtKind::kVar:
                    visit_expr_inner(ast, s.init, v);
                    visit_expr_inner(ast, s.expr, v);
                    break;

                case StmtKind::kReturn:
                    visit_expr_inner(ast, s.expr, v);
                    break;

                case StmtKind::kIf:
                    visit_expr_inner(ast, s.expr, v);
                    visit_stmt_child_if_(ast, id, s, StmtChildRole::kIfThen, s.a, v);
                    visit_stmt_child_if_(ast, id, s, StmtChildRole::kIfElse, s.b, v);
                    break;

                case StmtKind::kWhile:
                    visit_expr_inner(ast, s.expr, v);
                    visit_stmt_child_if_(ast, id, s, StmtChildRole::kWhileBody, s.a, v);
                    break;

                case StmtKind::kDoScope:
                    visit_stmt_child_if_(ast, id, s, StmtChildRole::kDoScopeBody, s.a, v);
                    break;

                case StmtKind::kDoWhile:
                    visit_stmt_child_if_(ast, id, s, StmtChildRole::kDoWhileBody, s.a, v);
                    visit_expr_inner(ast, s.expr, v);
                    break;

                case StmtKind::kManual:
                    visit_stmt_child_if_(ast, id, s, StmtChildRole::kManualBody, s.a, v);
                    break;

                case StmtKind::kSwitch: {
                    visit_expr_inner(ast, s.expr, v);
                    const auto& cases = ast.switch_cases();
                    const uint64_t begin = s.case_begin;
                    const uint64_t end = begin + s.case_count;
                    if (begin <= cases.size() && end <= cases.size()) {
                        for (uint32_t i = 0; i < s.case_count; ++i) {
                            visit_stmt_child_if_(
                                ast,
                                id,
                                s,
                                StmtChildRole::kSwitchCaseBody,
                                cases[s.case_begin + i].body,
                                v
                            );
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
                            if (p.has_default) {
                                visit_expr_inner(ast, p.default_expr, v);
                            }
                        }
                    }
                    visit_stmt_child_if_(ast, id, s, StmtChildRole::kFnBody, s.a, v);
                    break;
                }

                case StmtKind::kActsDecl:
                case StmtKind::kBlock: {
                    const auto& kids = ast.stmt_children();
                    const uint64_t begin = s.stmt_begin;
                    const uint64_t end = begin + s.stmt_count;
                    if (begin <= kids.size() && end <= kids.size()) {
                        for (uint32_t i = 0; i < s.stmt_count; ++i) {
                            visit_stmt_child_if_(
                                ast,
                                id,
                                s,
                                s.kind == StmtKind::kActsDecl ? StmtChildRole::kActsMember : StmtChildRole::kBlockChild,
                                kids[s.stmt_begin + i],
                                v
                            );
                        }
                    }
                    break;
                }

                case StmtKind::kNestDecl:
                    if (!s.nest_is_file_directive) {
                        visit_stmt_child_if_(ast, id, s, StmtChildRole::kNestBody, s.a, v);
                    }
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
