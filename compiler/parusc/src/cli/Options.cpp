// compiler/parusc/src/cli/Options.cpp
#include <parusc/cli/Options.hpp>

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

        /// @brief 옵션 다음의 필수 값을 읽는다.
        std::optional<std::string_view> read_next_(
            const std::vector<std::string_view>& args,
            size_t& i
        ) {
            if (i + 1 >= args.size()) return std::nullopt;
            ++i;
            return args[i];
        }

    } // namespace

    void print_usage(std::ostream& os) {
        os
            << "parusc [options] <input.pr>\n"
            << "  parusc main.pr -o main\n"
            << "  parusc --version\n"
            << "\n"
            << "General options:\n"
            << "  -h, --help\n"
            << "  --version\n"
            << "  -o <path>             Output path (default: a.out)\n"
            << "  -O0|-O1|-O2|-O3       Optimization level\n"
            << "  --lang en|ko          Diagnostic language\n"
            << "  --context <N>         Context line count for diagnostics\n"
            << "  -fmax-errors=<N>\n"
            << "  -Wshadow | -Werror=shadow\n"
            << "\n"
            << "Developer-only options (must be passed through -Xparus):\n"
            << "  -Xparus -token-dump\n"
            << "  -Xparus -ast-dump\n"
            << "  -Xparus -sir-dump\n"
            << "  -Xparus -oir-dump\n"
            << "  -Xparus -emit-llvm-ir\n"
            << "  -Xparus -emit-object\n";
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

            if (parse_opt_level_(out, a)) continue;
            parse_max_errors_(out, a);
            if (a.size() >= 13 && a.substr(0, 13) == "-fmax-errors=") continue;

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

        if (out.output_path.empty()) {
            if (out.internal.emit_object) out.output_path = "a.o";
            else if (out.internal.emit_llvm_ir) out.output_path = "a.ll";
            else out.output_path = "a.out";
        }

        return out;
    }

} // namespace parusc::cli
