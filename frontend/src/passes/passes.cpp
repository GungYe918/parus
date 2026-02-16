// frontend/src/passes/passes.cpp
#include <parus/passes/Passes.hpp>

#include <parus/passes/CheckPipeHole.hpp>
#include <parus/passes/CheckTopLevelDeclOnly.hpp>


namespace parus::passes {

    void run_on_expr(const ast::AstArena& ast, ast::ExprId root, diag::Bag& bag) {
        if (root == ast::k_invalid_expr) return;
        check_pipe_hole(ast, root, bag);
    }

    // stmt 트리를 훑으며 expr 루트들을 run_on_expr로 넘기기 위한 워커
    struct ExprWalker {
        const ast::AstArena& ast;
        diag::Bag& bag;

        void on_expr(ast::ExprId e) { run_on_expr(ast, e, bag); }

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
                case ast::StmtKind::kDoScope:
                    on_stmt(st.a);
                    break;
                case ast::StmtKind::kDoWhile:
                    on_stmt(st.a);
                    on_expr(st.expr);
                    break;
                case ast::StmtKind::kManual:
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
                    on_stmt(st.a);
                    break;
                }

                case ast::StmtKind::kFieldDecl:
                    // field 멤버는 타입 + 이름만 있으므로 expr 없음
                    break;

                case ast::StmtKind::kActsDecl: {
                    const auto& kids = ast.stmt_children();
                    for (uint32_t i = 0; i < st.stmt_count; ++i) {
                        on_stmt(kids[st.stmt_begin + i]);
                    }
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

                case ast::StmtKind::kNestDecl:
                    if (!st.nest_is_file_directive) on_stmt(st.a);
                    break;

                default:
                    break;
            }
        }
    };

    PassResults run_on_stmt_tree(
        const ast::AstArena& ast,
        ast::StmtId root,
        diag::Bag& bag,
        const PassOptions& opt
    ) {
        PassResults res{};

        if (root == ast::k_invalid_stmt) {
            // 결과는 비어있지만, API는 일관되게 반환
            return res;
        }

        // 1) NameResolve (심볼테이블 생성/채움 + nres 생성)
        name_resolve_stmt_tree(ast, root, res.sym, bag, opt.name_resolve, res.name_resolve);

        // 2) expr passes (pipe-hole 등)
        ExprWalker w{ast, bag};
        w.on_stmt(root);

        return res;
    }

    PassResults run_on_program(
        const ast::AstArena& ast,
        ast::StmtId program_root,
        diag::Bag& bag,
        const PassOptions& opt
    ) {
        if (program_root == ast::k_invalid_stmt) {
            return PassResults{};
        }

        // 0) Top-level decl-only 체크
        check_top_level_decl_only(ast, program_root, bag);

        // 1) stmt 기반 패스
        return run_on_stmt_tree(ast, program_root, bag, opt);
    }

} // namespace parus::passes
