#include <parusc/cli/Options.hpp>

#include <initializer_list>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>

namespace {

    static bool require_(bool cond, const char* msg) {
        if (cond) return true;
        std::cerr << "  - " << msg << "\n";
        return false;
    }

    static parusc::cli::Options parse_(std::initializer_list<std::string_view> args) {
        std::vector<std::string> storage{};
        storage.reserve(args.size() + 1);
        storage.emplace_back("parusc");
        for (const auto a : args) {
            storage.emplace_back(a);
        }

        std::vector<char*> argv{};
        argv.reserve(storage.size());
        for (auto& s : storage) {
            argv.push_back(s.data());
        }

        return parusc::cli::parse_options(static_cast<int>(argv.size()), argv.data());
    }

    static bool test_macro_budget_parse_() {
        const auto opt = parse_({
            "-fmacro-max-depth=77",
            "-fmacro-max-steps=9000",
            "-fmacro-max-output-tokens=333333",
            "main.pr",
        });

        bool ok = true;
        ok &= require_(opt.ok, "option parse must succeed");
        ok &= require_(opt.mode == parusc::cli::Mode::kCompile, "mode must be compile");
        ok &= require_(opt.macro_budget.max_depth == 77, "max_depth must parse");
        ok &= require_(opt.macro_budget.max_steps == 9000, "max_steps must parse");
        ok &= require_(opt.macro_budget.max_output_tokens == 333333, "max_output_tokens must parse");
        ok &= require_(opt.warnings.empty(), "in-range macro budget must not emit clamp warnings");
        return ok;
    }

    static bool test_macro_budget_clamp_hard_max_() {
        const auto opt = parse_({
            "-fmacro-max-depth=9999",
            "-fmacro-max-steps=999999999",
            "-fmacro-max-output-tokens=99999999",
            "main.pr",
        });

        bool ok = true;
        ok &= require_(opt.ok, "option parse must succeed");
        ok &= require_(
            opt.macro_budget.max_depth == parus::macro::k_macro_budget_hard_max_depth,
            "max_depth must clamp to hard max");
        ok &= require_(
            opt.macro_budget.max_steps == parus::macro::k_macro_budget_hard_max_steps,
            "max_steps must clamp to hard max");
        ok &= require_(
            opt.macro_budget.max_output_tokens == parus::macro::k_macro_budget_hard_max_output_tokens,
            "max_output_tokens must clamp to hard max");
        ok &= require_(opt.warnings.size() == 3, "all clamped fields must produce warnings");
        return ok;
    }

    static bool test_macro_budget_clamp_zero_or_negative_() {
        const auto opt = parse_({
            "-fmacro-max-depth=0",
            "-fmacro-max-steps=-1",
            "-fmacro-max-output-tokens=0",
            "main.pr",
        });

        bool ok = true;
        ok &= require_(opt.ok, "option parse must succeed");
        ok &= require_(opt.macro_budget.max_depth == 1, "zero depth must clamp to 1");
        ok &= require_(opt.macro_budget.max_steps == 1, "negative steps must clamp to 1");
        ok &= require_(opt.macro_budget.max_output_tokens == 1, "zero output tokens must clamp to 1");
        ok &= require_(opt.warnings.size() == 3, "zero/negative fields must produce warnings");
        return ok;
    }

    static bool test_macro_token_experimental_flag_() {
        const auto opt = parse_({
            "-Xparus",
            "-macro-token-experimental",
            "main.pr",
        });

        bool ok = true;
        ok &= require_(opt.ok, "option parse must succeed");
        ok &= require_(opt.has_xparus, "has_xparus must be set when internal option is used");
        ok &= require_(
            opt.internal.macro_token_experimental,
            "internal macro token experimental flag must be enabled");
        return ok;
    }

} // namespace

int main() {
    struct Case {
        const char* name;
        bool (*fn)();
    };

    const Case cases[] = {
        {"macro_budget_parse", test_macro_budget_parse_},
        {"macro_budget_clamp_hard_max", test_macro_budget_clamp_hard_max_},
        {"macro_budget_clamp_zero_or_negative", test_macro_budget_clamp_zero_or_negative_},
        {"macro_token_experimental_flag", test_macro_token_experimental_flag_},
    };

    int failed = 0;
    for (const auto& c : cases) {
        std::cout << "[TEST] " << c.name << "\n";
        if (!c.fn()) {
            ++failed;
            std::cout << "  -> FAIL\n";
        } else {
            std::cout << "  -> PASS\n";
        }
    }

    if (failed != 0) {
        std::cout << "\nFAILED " << failed << " test(s)\n";
        return 1;
    }
    std::cout << "\nALL TESTS PASSED\n";
    return 0;
}
