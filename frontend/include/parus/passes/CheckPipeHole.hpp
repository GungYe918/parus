// frontend/include/parus/passes/CheckPipeHole.hpp
#pragma once
#include <parus/ast/Nodes.hpp>
#include <parus/diag/Diagnostic.hpp>


namespace parus::passes {

    // 단일 expr 노드가 pipe 규칙을 위반하는지 검사한다.
    // (트리 순회는 호출자가 담당)
    void check_pipe_hole_node(const ast::AstArena& ast, ast::ExprId id, diag::Bag& bag);

    void check_pipe_hole(const ast::AstArena& ast, ast::ExprId root, diag::Bag& bag);

} // namespace parus::passes
