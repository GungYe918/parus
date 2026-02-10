// compiler/include/gaupel/sir/MutAnalysis.hpp
#pragma once
#include <gaupel/sir/SIR.hpp>
#include <gaupel/diag/Diagnostic.hpp>

#include <unordered_map>

namespace gaupel::sir {

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
        diag::Bag& bag
    );

} // namespace gaupel::sir