#pragma once

#include <parus/ast/Nodes.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/ty/TypePool.hpp>

#include <cstdint>
#include <vector>

namespace parus::macro {

    inline constexpr uint32_t k_macro_budget_hard_max_depth = 256;
    inline constexpr uint32_t k_macro_budget_hard_max_steps = 200000;
    inline constexpr uint32_t k_macro_budget_hard_max_output_tokens = 1000000;

    struct ExpansionBudget {
        uint32_t max_depth = 64;
        uint32_t max_steps = 20000;
        uint32_t max_output_tokens = 200000;
    };

    struct BudgetClampResult {
        bool any = false;
        bool depth = false;
        bool steps = false;
        bool output_tokens = false;
    };

    struct MacroExpansionContext {
        ast::AstArena& ast;
        ty::TypePool& types;
        diag::Bag& diags;
        ExpansionBudget budget{};
        uint32_t steps = 0;
        std::vector<Span> stack{};
    };

    ExpansionBudget default_budget_aot();
    ExpansionBudget default_budget_jit();
    BudgetClampResult clamp_budget(ExpansionBudget& budget);

    void apply_binder_hygiene(
        ast::AstArena& ast,
        std::vector<Token>& tokens,
        const std::vector<uint8_t>& generated_mask
    );

    bool expand_program(
        ast::AstArena& ast,
        ty::TypePool& types,
        ast::StmtId root,
        diag::Bag& diags,
        ExpansionBudget budget = {}
    );

} // namespace parus::macro
