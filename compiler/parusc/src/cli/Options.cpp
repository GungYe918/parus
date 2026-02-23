// compiler/parusc/src/cli/Options.cpp
#include <parusc/cli/Options.hpp>

#include <limits>
#include <optional>
#include <string_view>
#include <vector>

namespace parusc::cli {

    namespace {

        /// @brief `-Xparus` 내부 옵션 하나를 파싱한다.
        bool parse_internal_opt_(Options& out, std::string_view token) {
            if (token == "-token-dump") {
                out.internal.token_dump = true;
                return true;
            }
            if (token == "-ast-dump") {
                out.internal.ast_dump = true;
                return true;
            }
            if (token == "-sir-dump") {
                out.internal.sir_dump = true;
                return true;
            }
            if (token == "-oir-dump") {
                out.internal.oir_dump = true;
                return true;
            }
            if (token == "-emit-llvm-ir") {
                out.internal.emit_llvm_ir = true;
                return true;
            }
            if (token == "-emit-object") {
                out.internal.emit_object = true;
                return true;
            }
            if (token == "-macro-token-experimental") {
                out.internal.macro_token_experimental = true;
                return true;
            }
            return false;
        }

        /// @brief `-fmax-errors=N` 형식의 값을 읽는다.
        void parse_max_errors_(Options& out, std::string_view arg) {
            constexpr std::string_view kPrefix = "-fmax-errors=";
            if (arg.size() < kPrefix.size()) return;
            if (arg.substr(0, kPrefix.size()) != kPrefix) return;
            try {
                int v = std::stoi(std::string(arg.substr(kPrefix.size())));
                if (v < 1) v = 1;
                out.max_errors = static_cast<uint32_t>(v);
            } catch (...) {
                // 무시: 기본값 유지
            }
        }

        /// @brief `-O0..-O3` 최적화 레벨을 읽는다.
        bool parse_opt_level_(Options& out, std::string_view arg) {
            if (arg.size() != 3) return false;
            if (arg[0] != '-' || arg[1] != 'O') return false;
            if (arg[2] < '0' || arg[2] > '3') return false;
            out.opt_level = static_cast<uint8_t>(arg[2] - '0');
            return true;
        }

        /// @brief `-fuse-linker=<mode>` 값을 파싱한다.
        bool parse_linker_mode_(Options& out, std::string_view arg) {
            constexpr std::string_view kPrefix = "-fuse-linker=";
            if (!arg.starts_with(kPrefix)) return false;

            out.linker_mode_explicit = true;

            const std::string_view mode = arg.substr(kPrefix.size());
            if (mode == "auto" || mode == "parus-lld") {
                out.linker_mode = (mode == "auto")
                    ? LinkerMode::kAuto
                    : LinkerMode::kParusLld;
                return true;
            }
            if (mode == "lld" || mode == "system-lld") {
                out.linker_mode = LinkerMode::kSystemLld;
                return true;
            }
            if (mode == "clang" || mode == "system-clang") {
                out.linker_mode = LinkerMode::kSystemClang;
                return true;
            }

            out.ok = false;
            out.error = "unsupported linker mode: " + std::string(mode);
            return true;
        }

        /// @brief 옵션 다음의 필수 값을 읽는다.
        std::optional<std::string_view> read_next_(
            const std::vector<std::string_view>& args,
            size_t& i
        ) {
            if (i + 1 >= args.size()) return std::nullopt;
            ++i;
            return args[i];
        }

        /// @brief `--diag-format[=]text|json` 값을 파싱한다.
        bool parse_diag_format_(
            Options& out,
            const std::vector<std::string_view>& args,
            size_t& i
        ) {
            std::string_view value{};
            const auto a = args[i];

            constexpr std::string_view kOpt = "--diag-format";
            constexpr std::string_view kOptEq = "--diag-format=";
            if (a == kOpt) {
                const auto v = read_next_(args, i);
                if (!v) {
                    out.ok = false;
                    out.error = "--diag-format requires text or json";
                    return true;
                }
                value = *v;
            } else if (a.starts_with(kOptEq)) {
                value = a.substr(kOptEq.size());
            } else {
                return false;
            }

            if (value == "text") {
                out.diag_format = DiagFormat::kText;
                return true;
            }
            if (value == "json") {
                out.diag_format = DiagFormat::kJson;
                return true;
            }

            out.ok = false;
            out.error = "unsupported --diag-format value: " + std::string(value);
            return true;
        }

        bool validate_syntax_only_conflicts_(Options& out) {
            if (!out.syntax_only) return true;

            if (out.output_path_explicit) {
                out.ok = false;
                out.error = "-fsyntax-only cannot be combined with -o";
                return false;
            }
            if (out.emit_object || out.internal.emit_object || out.internal.emit_llvm_ir) {
                out.ok = false;
                out.error = "-fsyntax-only cannot be combined with -Xparus emit options";
                return false;
            }
            if (out.target_triple_explicit || out.sysroot_path_explicit || out.apple_sdk_root_explicit
                || out.linker_mode_explicit || out.link_fallback_explicit) {
                out.ok = false;
                out.error = "-fsyntax-only cannot be combined with backend/linker options";
                return false;
            }
            return true;
        }

        bool parse_macro_budget_opt_(Options& out, std::string_view arg) {
            struct OptMap {
                std::string_view prefix;
                uint32_t* field = nullptr;
            };
            OptMap maps[] = {
                {"-fmacro-max-depth=", &out.macro_budget.max_depth},
                {"-fmacro-max-steps=", &out.macro_budget.max_steps},
                {"-fmacro-max-output-tokens=", &out.macro_budget.max_output_tokens},
            };

            for (const auto& m : maps) {
                if (!arg.starts_with(m.prefix)) continue;
                const auto value_text = arg.substr(m.prefix.size());
                if (value_text.empty()) {
                    out.ok = false;
                    out.error = std::string(m.prefix) + " requires a number";
                    return true;
                }
                try {
                    const long long v = std::stoll(std::string(value_text));
                    if (v <= 0) {
                        *m.field = 0;
                    } else if (static_cast<unsigned long long>(v) > std::numeric_limits<uint32_t>::max()) {
                        *m.field = std::numeric_limits<uint32_t>::max();
                    } else {
                        *m.field = static_cast<uint32_t>(v);
                    }
                } catch (...) {
                    out.ok = false;
                    out.error = std::string(m.prefix) + " requires a valid number";
                }
                return true;
            }
            return false;
        }

        bool parse_bundle_value_opt_(
            const std::vector<std::string_view>& args,
            size_t& i,
            std::string_view arg,
            std::string_view name,
            std::string& out_value,
            Options& out
        ) {
            const std::string opt = std::string("--") + std::string(name);
            const std::string opt_eq = opt + "=";
            if (arg == opt) {
                const auto v = read_next_(args, i);
                if (!v || v->empty()) {
                    out.ok = false;
                    out.error = opt + " requires a value";
                    return true;
                }
                out_value = std::string(*v);
                out.bundle.enabled = true;
                return true;
            }
            if (arg.starts_with(opt_eq)) {
                const auto v = arg.substr(opt_eq.size());
                if (v.empty()) {
                    out.ok = false;
                    out.error = opt + " requires a value";
                    return true;
                }
                out_value = std::string(v);
                out.bundle.enabled = true;
                return true;
            }
            return false;
        }

        bool parse_bundle_repeat_opt_(
            const std::vector<std::string_view>& args,
            size_t& i,
            std::string_view arg,
            std::string_view name,
            std::vector<std::string>& out_values,
            Options& out
        ) {
            const std::string opt = std::string("--") + std::string(name);
            const std::string opt_eq = opt + "=";
            if (arg == opt) {
                const auto v = read_next_(args, i);
                if (!v || v->empty()) {
                    out.ok = false;
                    out.error = opt + " requires a value";
                    return true;
                }
                out_values.emplace_back(*v);
                out.bundle.enabled = true;
                return true;
            }
            if (arg.starts_with(opt_eq)) {
                const auto v = arg.substr(opt_eq.size());
                if (v.empty()) {
                    out.ok = false;
                    out.error = opt + " requires a value";
                    return true;
                }
                out_values.emplace_back(v);
                out.bundle.enabled = true;
                return true;
            }
            return false;
        }

        bool validate_bundle_opts_(Options& out) {
            if (!out.bundle.enabled) return true;
            if (out.bundle.bundle_name.empty()) {
                out.ok = false;
                out.error = "bundle mode requires --bundle-name <name>";
                return false;
            }
            if (out.bundle.bundle_sources.empty()) {
                out.ok = false;
                out.error = "bundle mode requires at least one --bundle-source <path>";
                return false;
            }
            return true;
        }

        void clamp_macro_budget_and_collect_warnings_(Options& out) {
            const auto before = out.macro_budget;
            const auto clamped = parus::macro::clamp_budget(out.macro_budget);
            if (!clamped.any) return;

            if (clamped.depth) {
                out.warnings.push_back(
                    "macro budget clamped: max_depth "
                    + std::to_string(before.max_depth)
                    + " -> "
                    + std::to_string(out.macro_budget.max_depth));
            }
            if (clamped.steps) {
                out.warnings.push_back(
                    "macro budget clamped: max_steps "
                    + std::to_string(before.max_steps)
                    + " -> "
                    + std::to_string(out.macro_budget.max_steps));
            }
            if (clamped.output_tokens) {
                out.warnings.push_back(
                    "macro budget clamped: max_output_tokens "
                    + std::to_string(before.max_output_tokens)
                    + " -> "
                    + std::to_string(out.macro_budget.max_output_tokens));
            }
        }

    } // namespace

    void print_usage(std::ostream& os) {
        os
            << "parusc [options] <input.pr>\n"
            << "parusc lsp --stdio\n"
            << "  parusc main.pr -o main\n"
            << "  parusc --version\n"
            << "\n"
            << "General options:\n"
            << "  -h, --help\n"
            << "  --version\n"
            << "  -fsyntax-only        Run frontend checks only (no SIR/OIR/backend/link)\n"
            << "  --emit-object        Emit object file (stable alias of -Xparus -emit-object)\n"
            << "  --diag-format text|json\n"
            << "  -o <path>             Output path (default: a.out)\n"
            << "  --target <triple>     Override backend target triple\n"
            << "  --sysroot <path>      Parus sysroot path for link/runtime lookup\n"
            << "  --apple-sdk-root <path>  Explicit Apple SDK root for Darwin linking\n"
            << "  -O0|-O1|-O2|-O3       Optimization level\n"
            << "  --lang en|ko          Diagnostic language\n"
            << "  --context <N>         Context line count for diagnostics\n"
            << "  -fmax-errors=<N>\n"
            << "  -fmacro-max-depth=<N>\n"
            << "  -fmacro-max-steps=<N>\n"
            << "  -fmacro-max-output-tokens=<N>\n"
            << "  -fuse-linker=auto|parus-lld|lld|clang\n"
            << "  --no-link-fallback   Disable linker fallback chain\n"
            << "  --bundle-name <name>\n"
            << "  --bundle-source <path>  (repeatable)\n"
            << "  --bundle-dep <name>     (repeatable)\n"
            << "  --emit-export-index <path>\n"
            << "  --load-export-index <path>  (repeatable)\n"
            << "  -Wshadow | -Werror=shadow\n"
            << "\n"
            << "Developer-only options (must be passed through -Xparus):\n"
            << "  -Xparus -token-dump\n"
            << "  -Xparus -ast-dump\n"
            << "  -Xparus -sir-dump\n"
            << "  -Xparus -oir-dump\n"
            << "  -Xparus -emit-llvm-ir\n"
            << "  -Xparus -emit-object\n"
            << "  -Xparus -macro-token-experimental\n"
            << "\n"
            << "LSP mode:\n"
            << "  parusc lsp --stdio\n";
    }

    Options parse_options(int argc, char** argv) {
        Options out{};
        if (argc <= 1) {
            out.mode = Mode::kUsage;
            return out;
        }

        std::vector<std::string_view> args;
        args.reserve(static_cast<size_t>(argc - 1));
        for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

        if (!args.empty() && args.front() == "lsp") {
            out.mode = Mode::kLsp;
            for (size_t i = 1; i < args.size(); ++i) {
                const auto a = args[i];

                if (a == "-h" || a == "--help") {
                    out.mode = Mode::kUsage;
                    return out;
                }
                if (a == "--version") {
                    out.mode = Mode::kVersion;
                    return out;
                }
                if (a == "--stdio") {
                    out.lsp_stdio = true;
                    continue;
                }

                out.ok = false;
                out.error = "unknown lsp option: " + std::string(a);
                return out;
            }

            if (!out.lsp_stdio) {
                out.ok = false;
                out.error = "lsp mode requires --stdio";
            }
            return out;
        }

        out.mode = Mode::kCompile;

        for (size_t i = 0; i < args.size(); ++i) {
            const auto a = args[i];

            if (a == "-h" || a == "--help") {
                out.mode = Mode::kUsage;
                return out;
            }

            if (a == "--version") {
                out.mode = Mode::kVersion;
                return out;
            }

            if (a == "-o") {
                const auto v = read_next_(args, i);
                if (!v) {
                    out.ok = false;
                    out.error = "-o requires a path";
                    return out;
                }
                out.output_path = std::string(*v);
                out.output_path_explicit = true;
                continue;
            }

            if (a == "--emit-object") {
                out.emit_object = true;
                continue;
            }

            if (a == "--lang") {
                const auto v = read_next_(args, i);
                if (!v) {
                    out.ok = false;
                    out.error = "--lang requires en or ko";
                    return out;
                }
                out.lang = (*v == "ko") ? parus::diag::Language::kKo : parus::diag::Language::kEn;
                continue;
            }

            if (a == "--context") {
                const auto v = read_next_(args, i);
                if (!v) {
                    out.ok = false;
                    out.error = "--context requires a number";
                    return out;
                }
                try {
                    int n = std::stoi(std::string(*v));
                    if (n < 0) n = 0;
                    out.context_lines = static_cast<uint32_t>(n);
                } catch (...) {
                    out.ok = false;
                    out.error = "--context requires a valid number";
                    return out;
                }
                continue;
            }

            if (a == "--target") {
                const auto v = read_next_(args, i);
                if (!v) {
                    out.ok = false;
                    out.error = "--target requires a triple";
                    return out;
                }
                out.target_triple = std::string(*v);
                out.target_triple_explicit = true;
                continue;
            }

            if (a == "--sysroot") {
                const auto v = read_next_(args, i);
                if (!v) {
                    out.ok = false;
                    out.error = "--sysroot requires a path";
                    return out;
                }
                out.sysroot_path = std::string(*v);
                out.sysroot_path_explicit = true;
                continue;
            }

            if (a == "--apple-sdk-root") {
                const auto v = read_next_(args, i);
                if (!v) {
                    out.ok = false;
                    out.error = "--apple-sdk-root requires a path";
                    return out;
                }
                out.apple_sdk_root = std::string(*v);
                out.apple_sdk_root_explicit = true;
                continue;
            }

            if (a == "-fsyntax-only") {
                out.syntax_only = true;
                continue;
            }

            if (parse_diag_format_(out, args, i)) {
                if (!out.ok) return out;
                continue;
            }

            if (a == "-Wshadow") {
                out.pass_opt.name_resolve.shadowing = parus::passes::ShadowingMode::kWarn;
                continue;
            }

            if (a == "-Werror=shadow") {
                out.pass_opt.name_resolve.shadowing = parus::passes::ShadowingMode::kError;
                continue;
            }

            if (a == "-Xparus") {
                const auto v = read_next_(args, i);
                if (!v) {
                    out.ok = false;
                    out.error = "-Xparus requires one internal argument";
                    return out;
                }
                out.has_xparus = true;
                if (!parse_internal_opt_(out, *v)) {
                    out.ok = false;
                    out.error = "unknown -Xparus argument: " + std::string(*v);
                    return out;
                }
                continue;
            }

            if (a == "--no-link-fallback") {
                out.allow_link_fallback = false;
                out.link_fallback_explicit = true;
                continue;
            }

            if (parse_bundle_value_opt_(args, i, a, "bundle-name", out.bundle.bundle_name, out)) {
                if (!out.ok) return out;
                continue;
            }
            if (parse_bundle_value_opt_(args, i, a, "emit-export-index", out.bundle.emit_export_index_path, out)) {
                if (!out.ok) return out;
                continue;
            }
            if (parse_bundle_repeat_opt_(args, i, a, "bundle-source", out.bundle.bundle_sources, out)) {
                if (!out.ok) return out;
                continue;
            }
            if (parse_bundle_repeat_opt_(args, i, a, "bundle-dep", out.bundle.bundle_deps, out)) {
                if (!out.ok) return out;
                continue;
            }
            if (parse_bundle_repeat_opt_(args, i, a, "load-export-index", out.bundle.load_export_index_paths, out)) {
                if (!out.ok) return out;
                continue;
            }

            if (parse_linker_mode_(out, a)) {
                if (!out.ok) return out;
                continue;
            }

            if (parse_opt_level_(out, a)) continue;
            parse_max_errors_(out, a);
            if (a.size() >= 13 && a.substr(0, 13) == "-fmax-errors=") continue;
            if (parse_macro_budget_opt_(out, a)) {
                if (!out.ok) return out;
                continue;
            }

            if (!a.empty() && a[0] == '-') {
                out.ok = false;
                out.error = "unknown option: " + std::string(a);
                return out;
            }

            out.inputs.push_back(std::string(a));
        }

        if (out.mode == Mode::kCompile && out.inputs.empty()) {
            out.ok = false;
            out.error = "no input file";
            return out;
        }

        if (out.mode == Mode::kCompile && out.inputs.size() > 1) {
            out.ok = false;
            out.error = "multiple input files are not supported yet";
            return out;
        }

        if (!validate_syntax_only_conflicts_(out)) {
            return out;
        }
        if (!validate_bundle_opts_(out)) {
            return out;
        }

        if (out.output_path.empty() && !out.syntax_only) {
            if (out.internal.emit_object) out.output_path = "a.o";
            else if (out.internal.emit_llvm_ir) out.output_path = "a.ll";
            else out.output_path = "a.out";
        }

        clamp_macro_budget_and_collect_warnings_(out);

        return out;
    }

} // namespace parusc::cli
