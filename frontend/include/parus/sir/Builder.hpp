// frontend/include/parus/sir/Builder.hpp
#pragma once
#include <parus/sir/SIR.hpp>

#include <parus/ast/Nodes.hpp>
#include <parus/sema/SymbolTable.hpp>
#include <parus/tyck/TypeCheck.hpp>
#include <parus/ty/TypePool.hpp>
#include <parus/passes/NameResolve.hpp>

#include <string>
#include <vector>


namespace parus::sir {

    struct BuildOptions {
        bool preserve_spans = true;
        bool bundle_enabled = false;
        std::string bundle_name{};
        std::string current_source_norm{};
        std::vector<std::string> bundle_sources_norm{};
    };

    Module build_sir_module(
        const ast::AstArena& ast,
        ast::StmtId program_root,
        const sema::SymbolTable& sym,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        const ty::TypePool& types,
        const BuildOptions& opt
    );

} // namespace parus::sir
