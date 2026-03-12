// frontend/include/parus/passes/CanonicalizePipe.hpp
#pragma once
#include <parus/ast/Nodes.hpp>
#include <parus/diag/Diagnostic.hpp>

namespace parus::passes {

    // Mutating pass:
    // - canonicalize `lhs |> f(label: _)` into call form
    // - mark rewritten calls with Expr::call_from_pipe=true
    // - reject `<|` in v1 as not-supported-yet
    void canonicalize_pipe(ast::AstArena& ast, ast::StmtId root, diag::Bag& bag);

} // namespace parus::passes
