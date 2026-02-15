// compiler/include/parus/passes/CheckPipeHole.hpp
#pragma once
#include <parus/ast/Nodes.hpp>
#include <parus/diag/Diagnostic.hpp>


namespace parus::passes {

    void check_pipe_hole(const ast::AstArena& ast, ast::ExprId root, diag::Bag& bag);

} // namespace parus::passes