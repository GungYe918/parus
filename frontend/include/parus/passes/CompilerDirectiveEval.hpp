#pragma once
#include <parus/ast/Nodes.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/passes/Passes.hpp>


namespace parus::passes {

    /// @brief Evaluate `$[...]` compiler directives and prune disabled items.
    void evaluate_compiler_directives(
        ast::AstArena& ast,
        ast::StmtId program_root,
        diag::Bag& bag,
        const PassOptions& opt
    );

} // namespace parus::passes
