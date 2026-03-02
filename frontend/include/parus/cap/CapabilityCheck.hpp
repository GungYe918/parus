// frontend/include/parus/cap/CapabilityCheck.hpp
#pragma once

#include <parus/ast/Nodes.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/passes/NameResolve.hpp>
#include <parus/ty/TypePool.hpp>
#include <parus/tyck/TypeCheck.hpp>

#include <cstdint>

namespace parus::cap {

    struct CapabilityResult {
        bool ok = true;
        uint32_t error_count = 0;
    };

    /// @brief `&`, `&mut`, `^&` capability 규칙을 AST 단계에서 검사한다.
    CapabilityResult run_capability_check(
        const ast::AstArena& ast,
        ast::StmtId program_root,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        const ty::TypePool& types,
        diag::Bag& bag
    );

} // namespace parus::cap
