// frontend/src/passes/passes.cpp
#include <parus/passes/Passes.hpp>

#include <parus/ast/Visitor.hpp>
#include <parus/passes/CheckPipeHole.hpp>
#include <parus/passes/CheckPlaceExpr.hpp>
#include <parus/passes/CheckTopLevelDeclOnly.hpp>


namespace parus::passes {

    class ExprPassVisitor final : public ast::TreeVisitor {
    public:
        ExprPassVisitor(const ast::AstArena& ast, diag::Bag& bag)
            : ast_(ast), bag_(bag) {}

        void enter_expr(ast::ExprId id, const ast::Expr&) override {
            check_pipe_hole_node(ast_, id, bag_);
            check_place_expr_node(ast_, id, bag_);
        }

    private:
        const ast::AstArena& ast_;
        diag::Bag& bag_;
    };

    void run_on_expr(const ast::AstArena& ast, ast::ExprId root, diag::Bag& bag) {
        if (root == ast::k_invalid_expr) return;
        ExprPassVisitor visitor(ast, bag);
        ast::visit_expr_tree(ast, root, visitor);
    }

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

        // 2) expr passes (pipe-hole, place 규칙)
        ExprPassVisitor visitor(ast, bag);
        ast::visit_stmt_tree(ast, root, visitor);

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
