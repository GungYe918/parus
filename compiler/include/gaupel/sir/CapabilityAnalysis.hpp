#pragma once

#include <gaupel/diag/Diagnostic.hpp>
#include <gaupel/sir/SIR.hpp>
#include <gaupel/ty/TypePool.hpp>

#include <cstdint>
#include <unordered_map>

namespace gaupel::sir {

    struct CapabilitySymbolState {
        bool moved_by_escape = false;
        uint32_t active_shared_borrows = 0;
        bool active_mut_borrow = false;
    };

    struct CapabilityAnalysisResult {
        bool ok = true;
        uint32_t error_count = 0;
        std::unordered_map<SymbolId, CapabilitySymbolState> state_by_symbol;
    };

    /// @brief SIR 단계에서 `&`, `&mut`, `&&` capability 규칙을 정밀 분석한다.
    CapabilityAnalysisResult analyze_capabilities(
        const Module& m,
        const ty::TypePool& types,
        diag::Bag& bag
    );

} // namespace gaupel::sir

