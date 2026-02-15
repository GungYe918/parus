// tools/parusc/src/cli/Options.cpp
#include "Options.hpp"

#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace parusc::cli {

    namespace {

        /// @brief `--lang` 옵션을 파싱한다.
        parus::diag::Language parse_lang(const std::vector<std::string_view>& args) {
            for (size_t i = 0; i + 1 < args.size(); ++i) {
                if (args[i] == "--lang") {
                    if (args[i + 1] == "ko") return parus::diag::Language::kKo;
                    return parus::diag::Language::kEn;
                }
            }
            return parus::diag::Language::kEn;
        }

        /// @brief `-fmax-errors=N` 옵션을 파싱한다.
        uint32_t parse_max_errors(const std::vector<std::string_view>& args) {
            uint32_t v = 64;
            for (auto a : args) {
                constexpr std::string_view key = "-fmax-errors=";
                if (a.size() >= key.size() && a.substr(0, key.size()) == key) {
                    try {
                        int n = std::stoi(std::string(a.substr(key.size())));
                        if (n < 1) n = 1;
                        v = static_cast<uint32_t>(n);
                    } catch (...) {
                    }
                }
            }
            return v;
        }

        /// @brief `--context N` 옵션을 파싱한다.
        uint32_t parse_context(const std::vector<std::string_view>& args) {
            for (size_t i = 0; i + 1 < args.size(); ++i) {
                if (args[i] == "--context") {
                    try {
                        int v = std::stoi(std::string(args[i + 1]));
                        if (v < 0) v = 0;
                        return static_cast<uint32_t>(v);
                    } catch (...) {
                        return 2;
                    }
                }
            }
            return 2;
        }

        /// @brief shadowing 관련 경고/에러 모드를 파싱한다.
        parus::passes::ShadowingMode parse_shadowing_mode(const std::vector<std::string_view>& args) {
            using M = parus::passes::ShadowingMode;
            bool warn = false;
            bool err = false;

            for (auto a : args) {
                if (a == "-Wshadow") warn = true;
                if (a == "-Werror=shadow") err = true;
            }

            if (err) return M::kError;
            if (warn) return M::kWarn;
            return M::kAllow;
        }

        /// @brief `--dump oir` 또는 `--dump-oir` 플래그를 파싱한다.
        bool parse_dump_oir(const std::vector<std::string_view>& args) {
            for (size_t i = 0; i < args.size(); ++i) {
                if (args[i] == "--dump") {
                    if (i + 1 < args.size() && args[i + 1] == "oir") return true;
                } else if (args[i] == "--dump-oir") {
                    return true;
                }
            }
            return false;
        }

        /// @brief 특정 키 플래그 위치를 탐색한다.
        std::optional<size_t> find_flag(const std::vector<std::string_view>& args, std::string_view key) {
            for (size_t i = 0; i < args.size(); ++i) {
                if (args[i] == key) return i;
            }
            return std::nullopt;
        }

    } // namespace

    void print_usage(std::ostream& os) {
        os
            << "parusc\n"
            << "  --version\n"
            << "  --expr \"<expr>\" [--lang en|ko] [--context N]\n"
            << "  --stmt \"<stmt>\" [--lang en|ko] [--context N]\n"
            << "  --all  \"<program>\" [--lang en|ko] [--context N] [--dump oir]\n"
            << "  --file <path> [--lang en|ko] [--context N] [--dump oir]\n"
            << "\n"
            << "Options:\n"
            << "  -fmax-errors=N\n"
            << "  -Wshadow            (emit warning on shadowing)\n"
            << "  -Werror=shadow      (treat shadowing as error)\n"
            << "  --dump oir          (dump OIR after SIR build)\n";
    }

    Options parse_options(int argc, char** argv) {
        Options opt{};

        if (argc <= 1) {
            opt.mode = Mode::kUsage;
            return opt;
        }

        std::vector<std::string_view> args;
        args.reserve(static_cast<size_t>(argc - 1));
        for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

        for (auto a : args) {
            if (a == "--version") {
                opt.mode = Mode::kVersion;
                return opt;
            }
        }

        opt.lang = parse_lang(args);
        opt.context_lines = parse_context(args);
        opt.max_errors = parse_max_errors(args);
        opt.dump_oir = parse_dump_oir(args);
        opt.pass_opt.name_resolve.shadowing = parse_shadowing_mode(args);

        if (auto i = find_flag(args, "--expr")) {
            if (*i + 1 >= args.size()) {
                opt.ok = false;
                opt.error = "--expr requires a string";
                return opt;
            }
            opt.mode = Mode::kExpr;
            opt.payload = std::string(args[*i + 1]);
            return opt;
        }

        if (auto i = find_flag(args, "--stmt")) {
            if (*i + 1 >= args.size()) {
                opt.ok = false;
                opt.error = "--stmt requires a string";
                return opt;
            }
            opt.mode = Mode::kStmt;
            opt.payload = std::string(args[*i + 1]);
            return opt;
        }

        if (auto i = find_flag(args, "--all")) {
            if (*i + 1 >= args.size()) {
                opt.ok = false;
                opt.error = "--all requires a string";
                return opt;
            }
            opt.mode = Mode::kAll;
            opt.payload = std::string(args[*i + 1]);
            return opt;
        }

        if (auto i = find_flag(args, "--file")) {
            if (*i + 1 >= args.size()) {
                opt.ok = false;
                opt.error = "--file requires a path";
                return opt;
            }
            opt.mode = Mode::kFile;
            opt.payload = std::string(args[*i + 1]);
            return opt;
        }

        opt.mode = Mode::kUsage;
        return opt;
    }

} // namespace parusc::cli
