// frontend/include/parus/sir/MutAnalysis.hpp
#pragma once
#include <parus/sir/SIR.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/ty/TypePool.hpp>

#include <unordered_map>

namespace parus::sir {

    struct MutInfo {
        bool declared_mut = false; // let mut
        bool is_set = false;       // set decl
        bool ever_written = false; // assign/postfix++ etc.
        bool illegal_write = false;
    };

    struct MutAnalysisResult {
        std::unordered_map<SymbolId, MutInfo> by_symbol;
    };

    MutAnalysisResult analyze_mut(
        const Module& m,
        const ty::TypePool& types,
        diag::Bag& bag
    );

} // namespace parus::sir
