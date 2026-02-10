// compiler/include/gaupel/sir/Builder.hpp
#pragma once
#include <gaupel/sir/SIR.hpp>

#include <gaupel/ast/Nodes.hpp>
#include <gaupel/sema/SymbolTable.hpp>
#include <gaupel/tyck/TypeCheck.hpp>
#include <gaupel/ty/TypePool.hpp>
#include <gaupel/passes/NameResolve.hpp>


namespace gaupel::sir {

    struct BuildOptions {
        bool preserve_spans = true;
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

} // namespace gaupel::sir