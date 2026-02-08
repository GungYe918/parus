#pragma once
#include <gaupel/ast/Nodes.hpp>
#include <gaupel/diag/Diagnostic.hpp>


namespace gaupel::passes {

    // & / && 가 "place expr" 에만 적용되는지 검사
    void check_place_expr(const ast::AstArena& ast, ast::ExprId root, diag::Bag& bag);

} // namespace gaupel::passes