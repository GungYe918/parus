#pragma once

#include <parus/ast/Nodes.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/ty/TypePool.hpp>

#include <cstdint>
#include <string_view>
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

    struct MacroTokenRange {
        uint32_t begin = 0;
        uint32_t count = 0;
    };

    struct MacroCaptureBinding {
        std::string_view name{};
        bool variadic = false;
        std::vector<MacroTokenRange> ranges{};
    };

    enum class TokenArmMatchStatus : uint8_t {
        kNoMatch = 0,
        kMatch,
        kError,
    };

    TokenArmMatchStatus match_token_arm(
        ast::AstArena& ast,
        ty::TypePool& types,
        const ast::MacroArm& arm,
        uint32_t arg_begin,
        uint32_t arg_count,
        Span call_span,
        diag::Bag& diags,
        std::vector<MacroCaptureBinding>& out_captures
    );

    bool substitute_token_template(
        ast::AstArena& ast,
        const ast::MacroArm& arm,
        const std::vector<MacroCaptureBinding>& captures,
        Span call_span,
        diag::Bag& diags,
        std::vector<Token>& out_tokens
    );

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
