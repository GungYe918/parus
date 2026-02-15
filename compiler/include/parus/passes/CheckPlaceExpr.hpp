#pragma once
#include <parus/ast/Nodes.hpp>
#include <parus/diag/Diagnostic.hpp>


namespace parus::passes {

    // & / && 가 "place expr" 에만 적용되는지 검사
    void check_place_expr(const ast::AstArena& ast, ast::ExprId root, diag::Bag& bag);

} // namespace parus::passes