// compiler/src/passes/passes.cpp
#include <gaupel/passes/Passes.hpp>

#include <gaupel/passes/CheckPipeHole.hpp>
#include <gaupel/passes/CheckTopLevelDeclOnly.hpp>
#include <gaupel/passes/NameResolve.hpp>

namespace gaupel::passes {

    void run_all_on_expr(const ast::AstArena& ast, ast::ExprId root, diag::Bag& bag) {
        if (root == ast::k_invalid_expr) return;
        // expr 트리 전체 스캔 패스
        check_pipe_hole(ast, root, bag);
        // (향후) CheckPlaceExpr 등 추가
    }

    void run_all_on_stmt(
        const ast::AstArena& ast,
        ast::StmtId root,
        sema::SymbolTable& out_sym,
        diag::Bag& bag,
        const PassOptions& opt
    ) {
        if (root == ast::k_invalid_stmt) return;

        // 1) NameResolve (스코프/식별자/let/set/파라미터)
        NameResolveOptions nopt = opt.name_resolve;
        name_resolve_stmt_tree(ast, root, out_sym, bag, nopt);
        // 2) stmt 내부 expr들에 대해 expr passes 실행(최소: pipe-hole)
        //    (지금은 NameResolve가 expr를 이미 훑지만, 파이프/기타 expr-pass는 별개)
        //    v0 간단: stmt 순회하면서 expr 루트마다 run_all_on_expr 호출
        struct Walker {
            const ast::AstArena& ast;
            diag::Bag& bag;

            void on_expr(ast::ExprId e) { run_all_on_expr(ast, e, bag); }

            void on_stmt(ast::StmtId s) {
                if (s == ast::k_invalid_stmt) return;
                const auto& st = ast.stmt(s);

                switch (st.kind) {
                    case ast::StmtKind::kExprStmt: on_expr(st.expr); break;
                    case ast::StmtKind::kVar:      on_expr(st.init); break;
                    case ast::StmtKind::kIf:
                        on_expr(st.expr); on_stmt(st.a); on_stmt(st.b); break;
                    case ast::StmtKind::kWhile:
                        on_expr(st.expr); on_stmt(st.a); break;
                    case ast::StmtKind::kReturn:
                        on_expr(st.expr); break;

                    case ast::StmtKind::kBlock: {
                        const auto& kids = ast.stmt_children();
                        for (uint32_t i = 0; i < st.stmt_count; ++i) on_stmt(kids[st.stmt_begin + i]);
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

                    case ast::StmtKind::kSwitch: {
                        on_expr(st.expr);
                        const auto& cs = ast.switch_cases();
                        for (uint32_t i = 0; i < st.case_count; ++i) on_stmt(cs[st.case_begin + i].body);
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

        Walker w{ast, bag};
        w.on_stmt(root);
    }

    void run_all_on_program(
        const ast::AstArena& ast,
        ast::StmtId program_root,
        sema::SymbolTable& out_sym,
        diag::Bag& bag,
        const PassOptions& opt
    ) {
        if (program_root == ast::k_invalid_stmt) return;

        // 0) Top-level decl-only 체크
        check_top_level_decl_only(ast, program_root, bag);

        // 1) 나머지 stmt 기반 패스 실행
        run_all_on_stmt(ast, program_root, out_sym, bag, opt);
    }

} // namespace gaupel::passes