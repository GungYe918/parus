// compiler/src/passes/passes.cpp
#include <gaupel/passes/Passes.hpp>

#include <gaupel/passes/CheckPipeHole.hpp>
#include <gaupel/passes/CheckPlaceExpr.hpp>

#include <gaupel/ast/Nodes.hpp>
#include <gaupel/diag/Diagnostic.hpp>


namespace gaupel::passes {

    // stmt 트리 안에서 “expr 루트들”을 찾아서, 그 expr마다 run_all_on_expr을 실행한다.
    struct Runner {
        const ast::AstArena& ast;
        diag::Bag& bag;

        void on_expr(ast::ExprId e) {
            if (e == ast::k_invalid_expr) return;
            run_all_on_expr(ast, e, bag);
        }

        void on_stmt(ast::StmtId s) {
            if (s == ast::k_invalid_stmt) return;

            const auto& st = ast.stmt(s);

            switch (st.kind) {
                case ast::StmtKind::kExprStmt:
                    on_expr(st.expr);
                    break;

                case ast::StmtKind::kVar:
                    on_expr(st.init);
                    break;

                case ast::StmtKind::kIf:
                    on_expr(st.expr);
                    on_stmt(st.a);
                    on_stmt(st.b);
                    break;

                case ast::StmtKind::kWhile:
                    on_expr(st.expr);
                    on_stmt(st.a);
                    break;

                case ast::StmtKind::kReturn:
                    on_expr(st.expr);
                    break;

                case ast::StmtKind::kBlock: {
                    const auto& kids = ast.stmt_children();
                    for (uint32_t i = 0; i < st.stmt_count; ++i) {
                        on_stmt(kids[st.stmt_begin + i]);
                    }
                    break;
                }

                case ast::StmtKind::kFnDecl: {
                    // param defaults
                    const auto& ps = ast.params();
                    for (uint32_t i = 0; i < st.param_count; ++i) {
                        const auto& p = ps[st.param_begin + i];
                        if (p.has_default) on_expr(p.default_expr);
                    }
                    // body
                    on_stmt(st.a);
                    break;
                }

                case ast::StmtKind::kSwitch: {
                    on_expr(st.expr);
                    const auto& cs = ast.switch_cases();
                    for (uint32_t i = 0; i < st.case_count; ++i) {
                        on_stmt(cs[st.case_begin + i].body);
                    }
                    break;
                }

                case ast::StmtKind::kUse:
                    on_expr(st.expr);
                    break;

                default:
                    break;
            }
        }
    };

    void run_all_on_expr(const ast::AstArena& ast, ast::ExprId root, diag::Bag& bag) {
        if (root == ast::k_invalid_expr) return;

        // 등록된 expr 기반 passes:
        check_pipe_hole(ast, root, bag);
        check_place_expr(ast, root, bag);

        // (향후) check_escape_rules(ast, root, bag);
        // (향후) check_call_conventions(ast, root, bag);
    }

    void run_all_on_stmt(const ast::AstArena& ast, ast::StmtId root, diag::Bag& bag) {
        if (root == ast::k_invalid_stmt) return;

        Runner r{ast, bag};
        r.on_stmt(root);

        // (향후) stmt 기반 passes도 여기 추가 가능
    }

} // namespace gaupel::passes