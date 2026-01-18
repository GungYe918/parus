// compiler/include/gaupel/passes/CheckPipeHole.hpp
#pragma once
#include <gaupel/ast/Nodes.hpp>
#include <gaupel/diag/Diagnostic.hpp>


namespace gaupel::passes {

    void check_pipe_hole(const ast::AstArena& ast, ast::ExprId root, diag::Bag& bag);

} // namespace gaupel::passes