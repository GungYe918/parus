#pragma once
#include <gaupel/ast/Nodes.hpp>
#include <gaupel/diag/Diagnostic.hpp>


namespace gaupel::passes {

    // root expr 하나에 대해, 현재 등록된 모든 passes를 실행
    void run_all_on_expr(const ast::AstArena& ast, ast::ExprId root, diag::Bag& bag);

    // stmt 트리(프로그램/블록/함수 바디 등)에 대해, 현재 등록된 모든 passes를 실행
    void run_all_on_stmt(const ast::AstArena& ast, ast::StmtId root, diag::Bag& bag);

} // namespace gaupel::passes
