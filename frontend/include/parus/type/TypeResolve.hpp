#pragma once

#include <parus/ast/Nodes.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/ty/TypePool.hpp>

#include <vector>

namespace parus::type {

    struct TypeResolveResult {
        bool ok = true;
        std::vector<ty::TypeId> node_types{};
    };

    TypeResolveResult resolve_program_types(
        ast::AstArena& ast,
        ty::TypePool& types,
        ast::StmtId root,
        diag::Bag& diags
    );

} // namespace parus::type

