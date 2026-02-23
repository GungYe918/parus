#include <parus_tool/cli/Options.hpp>

#include <cctype>
#include <limits>
#include <string_view>

namespace parus_tool::cli {

namespace {

bool is_command(std::string_view s) {
    return s == "build" || s == "check" || s == "graph" || s == "lsp" ||
           s == "doctor" || s == "config" || s == "tool";
}

Command to_command(std::string_view s) {
    if (s == "build") return Command::kBuild;
    if (s == "check") return Command::kCheck;
    if (s == "graph") return Command::kGraph;
    if (s == "lsp") return Command::kLsp;
    if (s == "doctor") return Command::kDoctor;
    if (s == "config") return Command::kConfig;
    if (s == "tool") return Command::kTool;
    return Command::kNone;
}

bool parse_opt_value(const std::vector<std::string_view>& args,
                     size_t& i,
                     std::string_view key,
                     std::string& out,
                     std::string& err) {
    const auto a = args[i];
    const auto pref = std::string(key) + "=";
    if (a.rfind(pref, 0) == 0) {
        out = std::string(a.substr(pref.size()));
        if (out.empty()) {
            err = std::string(key) + " requires a value";
            return false;
        }
        return true;
    }

    if (i + 1 >= args.size()) {
        err = std::string(key) + " requires a value";
        return false;
    }
    ++i;
    out = std::string(args[i]);
    if (out.empty()) {
        err = std::string(key) + " requires a value";
        return false;
    }
    return true;
}

bool parse_u32(const std::string& s, uint32_t& out) {
    if (s.empty()) return false;
    for (const char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    try {
        const unsigned long long v = std::stoull(s);
        if (v == 0 || v > static_cast<unsigned long long>(std::numeric_limits<uint32_t>::max())) return false;
        out = static_cast<uint32_t>(v);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_config_scope_token(std::string_view a, ConfigScope& scope, bool& scope_set, std::string& err) {
    ConfigScope parsed = ConfigScope::kAuto;
    if (a == "--global") {
        parsed = ConfigScope::kGlobal;
    } else if (a == "--project") {
        parsed = ConfigScope::kProject;
    } else if (a == "--effective") {
        parsed = ConfigScope::kEffective;
    } else {
        return false;
    }
    if (scope_set && scope != parsed) {
        err = "only one config scope may be selected";
        return true;
    }
    scope = parsed;
    scope_set = true;
    return true;
}

ConfigAction parse_config_action(std::string_view a) {
    if (a == "path") return ConfigAction::kPath;
    if (a == "show") return ConfigAction::kShow;
    if (a == "get") return ConfigAction::kGet;
    if (a == "set") return ConfigAction::kSet;
    if (a == "unset") return ConfigAction::kUnset;
    if (a == "init") return ConfigAction::kInit;
    return ConfigAction::kNone;
}

} // namespace

void print_usage(std::ostream& os) {
    os
        << "parus [global-options] <command> [args]\n"
        << "\n"
        << "Global options:\n"
        << "  -h, --help\n"
        << "  --version\n"
        << "  --toolchain-root <path>\n"
        << "\n"
        << "Commands:\n"
        << "  build [entry.lei] [--plan <name>] [--jobs <N>] [--verbose] [--out <path>]\n"
        << "  check [entry] [--plan <name>] [--diag-format <text|json>] [--lang <en|ko>] [--context <N>]\n"
        << "        [-fmacro-max-depth=<N>] [-fmacro-max-steps=<N>] [-fmacro-max-output-tokens=<N>]\n"
        << "  graph [entry.lei] [--plan <name>] [--format <json|text|dot>]\n"
        << "  lsp [--stdio]\n"
        << "  doctor [--quick] [--strict] [--json] [-v|--verbose]\n"
        << "  config path [--global|--project]\n"
        << "  config show [--global|--project|--effective] [--format toml|json]\n"
        << "  config get <key> [--effective|--global|--project]\n"
        << "  config set <key> <value> [--global|--project]\n"
        << "  config unset <key> [--global|--project]\n"
        << "  config init [--global|--project]\n"
        << "  tool <parusc|parusd|parus-lld|lei> -- <args...>\n";
}

Options parse_options(int argc, char** argv) {
    Options out{};

    if (argc <= 1) {
        out.mode = Mode::kUsage;
        return out;
    }

    std::vector<std::string_view> args{};
    args.reserve(static_cast<size_t>(argc - 1));
    for (int i = 1; i < argc; ++i) args.emplace_back(argv[i]);

    size_t i = 0;
    for (; i < args.size(); ++i) {
        const auto a = args[i];
        if (a == "-h" || a == "--help") {
            out.mode = Mode::kUsage;
            return out;
        }
        if (a == "--version") {
            out.mode = Mode::kVersion;
            return out;
        }
        if (a == "--json") {
            out.ok = false;
            out.error = "--json is only available for 'parus doctor --json'";
            return out;
        }
        if (a == "--toolchain-root" || a.rfind("--toolchain-root=", 0) == 0) {
            std::string v;
            if (!parse_opt_value(args, i, "--toolchain-root", v, out.error)) {
                out.ok = false;
                return out;
            }
            out.toolchain_root = std::move(v);
            continue;
        }
        if (is_command(a)) {
            out.command = to_command(a);
            out.mode = Mode::kCommand;
            ++i;
            break;
        }

        if (!a.empty() && a[0] == '-') {
            out.ok = false;
            out.error = "unknown global option: " + std::string(a);
            return out;
        }

        out.ok = false;
        out.error = "unknown command: " + std::string(a);
        return out;
    }

    if (out.mode != Mode::kCommand) {
        out.ok = false;
        out.error = "missing command";
        return out;
    }

    auto parse_common_entry = [&](std::string& entry) {
        if (i >= args.size()) return;
        const auto a = args[i];
        if (!a.empty() && a[0] != '-') {
            entry = std::string(a);
            ++i;
        }
    };

    if (out.command == Command::kBuild) {
        parse_common_entry(out.build.entry);
        for (; i < args.size(); ++i) {
            const auto a = args[i];
            if (a == "--verbose") {
                out.build.verbose = true;
                continue;
            }
            if (a == "--plan" || a.rfind("--plan=", 0) == 0) {
                std::string v;
                if (!parse_opt_value(args, i, "--plan", v, out.error)) {
                    out.ok = false;
                    return out;
                }
                out.build.plan = std::move(v);
                continue;
            }
            if (a == "--jobs" || a.rfind("--jobs=", 0) == 0) {
                std::string v;
                if (!parse_opt_value(args, i, "--jobs", v, out.error)) {
                    out.ok = false;
                    return out;
                }
                uint32_t parsed = 0;
                if (!parse_u32(v, parsed)) {
                    out.ok = false;
                    out.error = "--jobs requires a positive integer";
                    return out;
                }
                out.build.jobs = parsed;
                continue;
            }
            if (a == "--out" || a.rfind("--out=", 0) == 0) {
                std::string v;
                if (!parse_opt_value(args, i, "--out", v, out.error)) {
                    out.ok = false;
                    return out;
                }
                out.build.out_path = std::move(v);
                continue;
            }
            out.ok = false;
            out.error = "unknown build option: " + std::string(a);
            return out;
        }
        return out;
    }

    if (out.command == Command::kCheck) {
        parse_common_entry(out.check.entry);
        for (; i < args.size(); ++i) {
            const auto a = args[i];
            if (a == "--plan" || a.rfind("--plan=", 0) == 0) {
                std::string v;
                if (!parse_opt_value(args, i, "--plan", v, out.error)) {
                    out.ok = false;
                    return out;
                }
                out.check.plan = std::move(v);
                continue;
            }
            if (a == "--diag-format" || a.rfind("--diag-format=", 0) == 0) {
                std::string v;
                if (!parse_opt_value(args, i, "--diag-format", v, out.error)) {
                    out.ok = false;
                    return out;
                }
                out.check.diag_format = std::move(v);
                continue;
            }
            if (a == "--lang" || a.rfind("--lang=", 0) == 0) {
                std::string v;
                if (!parse_opt_value(args, i, "--lang", v, out.error)) {
                    out.ok = false;
                    return out;
                }
                out.check.lang = std::move(v);
                continue;
            }
            if (a == "--context" || a.rfind("--context=", 0) == 0) {
                std::string v;
                if (!parse_opt_value(args, i, "--context", v, out.error)) {
                    out.ok = false;
                    return out;
                }
                out.check.context = std::move(v);
                continue;
            }
            if (a.rfind("-fmacro-max-depth=", 0) == 0 ||
                a.rfind("-fmacro-max-steps=", 0) == 0 ||
                a.rfind("-fmacro-max-output-tokens=", 0) == 0) {
                out.check.macro_budget_flags.emplace_back(a);
                continue;
            }
            out.ok = false;
            out.error = "unknown check option: " + std::string(a);
            return out;
        }
        return out;
    }

    if (out.command == Command::kGraph) {
        parse_common_entry(out.graph.entry);
        for (; i < args.size(); ++i) {
            const auto a = args[i];
            if (a == "--plan" || a.rfind("--plan=", 0) == 0) {
                std::string v;
                if (!parse_opt_value(args, i, "--plan", v, out.error)) {
                    out.ok = false;
                    return out;
                }
                out.graph.plan = std::move(v);
                continue;
            }
            if (a == "--format" || a.rfind("--format=", 0) == 0) {
                std::string v;
                if (!parse_opt_value(args, i, "--format", v, out.error)) {
                    out.ok = false;
                    return out;
                }
                out.graph.format = std::move(v);
                continue;
            }
            out.ok = false;
            out.error = "unknown graph option: " + std::string(a);
            return out;
        }
        return out;
    }

    if (out.command == Command::kLsp) {
        out.lsp.stdio = true;
        for (; i < args.size(); ++i) {
            const auto a = args[i];
            if (a == "--stdio") {
                out.lsp.stdio = true;
                continue;
            }
            out.ok = false;
            out.error = "unknown lsp option: " + std::string(a);
            return out;
        }
        return out;
    }

    if (out.command == Command::kDoctor) {
        for (; i < args.size(); ++i) {
            const auto a = args[i];
            if (a == "--quick") {
                out.doctor.quick = true;
                continue;
            }
            if (a == "--strict") {
                out.doctor.strict = true;
                continue;
            }
            if (a == "--json") {
                out.doctor.json = true;
                continue;
            }
            if (a == "-v" || a == "--verbose") {
                out.doctor.verbose = true;
                continue;
            }
            out.ok = false;
            out.error = "unknown doctor option: " + std::string(a);
            return out;
        }
        return out;
    }

    if (out.command == Command::kConfig) {
        if (i >= args.size()) {
            out.ok = false;
            out.error = "config command requires subcommand";
            return out;
        }

        out.config.action = parse_config_action(args[i]);
        if (out.config.action == ConfigAction::kNone) {
            out.ok = false;
            out.error = "unknown config subcommand: " + std::string(args[i]);
            return out;
        }
        ++i;

        bool scope_set = false;
        auto parse_scope = [&](std::string_view a) -> bool {
            if (!parse_config_scope_token(a, out.config.scope, scope_set, out.error)) return false;
            if (!out.error.empty()) {
                out.ok = false;
                return true;
            }
            return true;
        };

        if (out.config.action == ConfigAction::kPath) {
            for (; i < args.size(); ++i) {
                const auto a = args[i];
                if (a == "--effective") {
                    out.ok = false;
                    out.error = "--effective is not supported for 'config path'";
                    return out;
                }
                if (parse_scope(a)) {
                    if (!out.ok) return out;
                    continue;
                }
                out.ok = false;
                out.error = "unknown config path option: " + std::string(a);
                return out;
            }
            return out;
        }

        if (out.config.action == ConfigAction::kShow) {
            out.config.scope = ConfigScope::kEffective;
            for (; i < args.size(); ++i) {
                const auto a = args[i];
                if (a == "--format" || a.rfind("--format=", 0) == 0) {
                    std::string v;
                    if (!parse_opt_value(args, i, "--format", v, out.error)) {
                        out.ok = false;
                        return out;
                    }
                    if (v == "toml") {
                        out.config.format = ConfigFormat::kToml;
                    } else if (v == "json") {
                        out.config.format = ConfigFormat::kJson;
                    } else {
                        out.ok = false;
                        out.error = "--format must be toml or json";
                        return out;
                    }
                    continue;
                }
                if (parse_scope(a)) {
                    if (!out.ok) return out;
                    continue;
                }
                out.ok = false;
                out.error = "unknown config show option: " + std::string(a);
                return out;
            }
            return out;
        }

        if (out.config.action == ConfigAction::kGet) {
            out.config.scope = ConfigScope::kEffective;
            if (i >= args.size()) {
                out.ok = false;
                out.error = "config get requires a key";
                return out;
            }
            out.config.key = std::string(args[i]);
            ++i;
            for (; i < args.size(); ++i) {
                const auto a = args[i];
                if (parse_scope(a)) {
                    if (!out.ok) return out;
                    continue;
                }
                out.ok = false;
                out.error = "unknown config get option: " + std::string(a);
                return out;
            }
            return out;
        }

        if (out.config.action == ConfigAction::kSet) {
            if (i >= args.size()) {
                out.ok = false;
                out.error = "config set requires a key";
                return out;
            }
            out.config.key = std::string(args[i]);
            ++i;
            if (i >= args.size()) {
                out.ok = false;
                out.error = "config set requires a value";
                return out;
            }
            out.config.value = std::string(args[i]);
            ++i;
            for (; i < args.size(); ++i) {
                const auto a = args[i];
                if (a == "--effective") {
                    out.ok = false;
                    out.error = "--effective is read-only and cannot be used with set";
                    return out;
                }
                if (parse_scope(a)) {
                    if (!out.ok) return out;
                    continue;
                }
                out.ok = false;
                out.error = "unknown config set option: " + std::string(a);
                return out;
            }
            return out;
        }

        if (out.config.action == ConfigAction::kUnset) {
            if (i >= args.size()) {
                out.ok = false;
                out.error = "config unset requires a key";
                return out;
            }
            out.config.key = std::string(args[i]);
            ++i;
            for (; i < args.size(); ++i) {
                const auto a = args[i];
                if (a == "--effective") {
                    out.ok = false;
                    out.error = "--effective is read-only and cannot be used with unset";
                    return out;
                }
                if (parse_scope(a)) {
                    if (!out.ok) return out;
                    continue;
                }
                out.ok = false;
                out.error = "unknown config unset option: " + std::string(a);
                return out;
            }
            return out;
        }

        if (out.config.action == ConfigAction::kInit) {
            for (; i < args.size(); ++i) {
                const auto a = args[i];
                if (a == "--effective") {
                    out.ok = false;
                    out.error = "--effective is read-only and cannot be used with init";
                    return out;
                }
                if (parse_scope(a)) {
                    if (!out.ok) return out;
                    continue;
                }
                out.ok = false;
                out.error = "unknown config init option: " + std::string(a);
                return out;
            }
            return out;
        }
    }

    if (out.command == Command::kTool) {
        if (i >= args.size()) {
            out.ok = false;
            out.error = "tool command requires tool name";
            return out;
        }
        out.tool.tool_name = std::string(args[i]);
        ++i;
        if (out.tool.tool_name != "parusc" &&
            out.tool.tool_name != "parusd" &&
            out.tool.tool_name != "parus-lld" &&
            out.tool.tool_name != "lei") {
            out.ok = false;
            out.error = "unsupported tool: " + out.tool.tool_name;
            return out;
        }

        if (i < args.size() && args[i] == "--") {
            ++i;
        }
        for (; i < args.size(); ++i) {
            out.tool.passthrough.emplace_back(args[i]);
        }
        return out;
    }

    out.ok = false;
    out.error = "unreachable command parse state";
    return out;
}

} // namespace parus_tool::cli
