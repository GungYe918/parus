#pragma once
#include <parus/ast/Nodes.hpp>
#include <parus/diag/Diagnostic.hpp>


namespace parus::passes {

    // 단일 expr 노드가 place 규칙을 위반하는지 검사한다.
    // (트리 순회는 호출자가 담당)
    void check_place_expr_node(const ast::AstArena& ast, ast::ExprId id, diag::Bag& bag);

    // & / ^& 가 "place expr" 에만 적용되는지 검사
    void check_place_expr(const ast::AstArena& ast, ast::ExprId root, diag::Bag& bag);

} // namespace parus::passes
