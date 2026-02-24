#include <parus_tool/driver/Driver.hpp>

#include <parus_tool/config/Config.hpp>
#include <parus_tool/doctor/Doctor.hpp>
#include <parus_tool/proc/Process.hpp>
#include <parus_tool/toolchain/Resolver.hpp>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <optional>
#include <sstream>
#include <string_view>
#include <unordered_map>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace parus_tool::driver {

namespace {

constexpr const char* kAnsiReset = "\033[0m";
constexpr const char* kAnsiGreen = "\033[32m";
constexpr const char* kAnsiRed = "\033[31m";
constexpr const char* kAnsiOrange = "\033[38;5;208m";

bool use_stderr_color(std::string_view mode) {
    if (mode == "never") return false;
    if (mode == "always") return true;
    if (std::getenv("NO_COLOR") != nullptr) return false;
#if defined(_WIN32)
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(fileno(stderr)) != 0;
#endif
}

std::string paint(std::string_view text, const char* ansi, std::string_view color_mode) {
    if (!use_stderr_color(color_mode)) return std::string(text);
    return std::string(ansi) + std::string(text) + kAnsiReset;
}

std::string tag(std::string_view text, const char* ansi, std::string_view color_mode) {
    if (!use_stderr_color(color_mode)) {
        return "[" + std::string(text) + "]";
    }
    return "[" + std::string(ansi) + std::string(text) + kAnsiReset + "]";
}

struct RuntimeConfig {
    config::LoadedConfig loaded{};
    config::EffectiveSettings settings{};
};

std::string trim(std::string s) {
    auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::optional<std::string> canonical_top_head(std::string_view raw) {
    std::string_view s = raw;
    if (s.empty()) return std::nullopt;
    if (s.starts_with("::")) s.remove_prefix(2);
    if (s.empty() || s.ends_with("::")) return std::nullopt;

    size_t begin = 0;
    std::string top{};
    while (begin < s.size()) {
        const size_t pos = s.find("::", begin);
        const size_t end = (pos == std::string_view::npos) ? s.size() : pos;
        if (end == begin) return std::nullopt;
        const std::string_view seg = s.substr(begin, end - begin);
        if (seg.find(':') != std::string_view::npos) return std::nullopt;
        if (top.empty()) top = std::string(seg);
        if (pos == std::string_view::npos) break;
        begin = pos + 2;
    }
    if (top.empty()) return std::nullopt;
    return top;
}

bool canonicalize_import_heads(std::vector<std::string>& heads, std::string& out_err) {
    std::vector<std::string> normalized{};
    normalized.reserve(heads.size());
    for (const auto& h : heads) {
        auto top = canonical_top_head(h);
        if (!top.has_value()) {
            out_err = "invalid module_imports entry from LEI: '" + h + "'";
            return false;
        }
        normalized.push_back(*top);
    }
    std::sort(normalized.begin(), normalized.end());
    normalized.erase(std::unique(normalized.begin(), normalized.end()), normalized.end());
    heads = std::move(normalized);
    return true;
}

void emit_progress(bool enabled, std::string_view color_mode, int pct, std::string_view message) {
    if (!enabled) return;
    std::ostringstream oss;
    oss << "[" << std::setw(3) << pct << "%]";
    std::cerr << paint(oss.str(), kAnsiGreen, color_mode) << " " << message << "\n";
}

void emit_warn(std::string_view color_mode, std::string_view message) {
    std::cerr << tag("WARN", kAnsiOrange, color_mode) << " " << message << "\n";
}

void emit_fail(std::string_view color_mode, std::string_view message) {
    std::cerr << tag("FAIL", kAnsiRed, color_mode) << " " << message << "\n";
}

void emit_done(bool enabled, std::string_view color_mode, std::string_view message) {
    if (!enabled) return;
    std::cerr << tag("DONE", kAnsiGreen, color_mode) << " " << message << "\n";
}

bool is_pr_entry(const std::string& entry) {
    const auto ext = std::filesystem::path(entry).extension().string();
    return ext == ".pr";
}

struct BundleSourceUnit {
    std::string bundle{};
    std::string module{};
    std::string source{};
    std::vector<std::string> module_imports{};
    std::vector<std::string> bundle_deps{};
};

bool unescape_json_string(std::string_view in, std::string& out) {
    out.clear();
    out.reserve(in.size());
    for (size_t i = 0; i < in.size(); ++i) {
        const char c = in[i];
        if (c != '\\') {
            out.push_back(c);
            continue;
        }
        if (i + 1 >= in.size()) return false;
        const char n = in[++i];
        switch (n) {
            case '"': out.push_back('"'); break;
            case '\\': out.push_back('\\'); break;
            case '/': out.push_back('/'); break;
            case 'b': out.push_back('\b'); break;
            case 'f': out.push_back('\f'); break;
            case 'n': out.push_back('\n'); break;
            case 'r': out.push_back('\r'); break;
            case 't': out.push_back('\t'); break;
            case 'u':
                if (i + 4 >= in.size()) return false;
                i += 4;
                out.push_back('?');
                break;
            default:
                return false;
        }
    }
    return true;
}

bool parse_json_string_field_line(const std::string& line, std::string_view key, std::string& out) {
    const std::string needle = "\"" + std::string(key) + "\":\"";
    const size_t pos = line.find(needle);
    if (pos == std::string::npos) return false;
    size_t i = pos + needle.size();
    std::string raw{};
    bool escaped = false;
    for (; i < line.size(); ++i) {
        const char c = line[i];
        if (escaped) {
            raw.push_back(c);
            escaped = false;
            continue;
        }
        if (c == '\\') {
            raw.push_back(c);
            escaped = true;
            continue;
        }
        if (c == '"') break;
        raw.push_back(c);
    }
    if (i >= line.size() || line[i] != '"') return false;
    return unescape_json_string(raw, out);
}

bool parse_json_string_array_field_line(const std::string& line, std::string_view key, std::vector<std::string>& out) {
    out.clear();
    const std::string needle = "\"" + std::string(key) + "\":[";
    size_t pos = line.find(needle);
    if (pos == std::string::npos) return false;
    pos += needle.size();

    while (pos < line.size()) {
        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
        if (pos >= line.size()) return false;
        if (line[pos] == ']') return true;
        if (line[pos] != '"') return false;
        ++pos;
        std::string raw{};
        bool escaped = false;
        for (; pos < line.size(); ++pos) {
            const char c = line[pos];
            if (escaped) {
                raw.push_back(c);
                escaped = false;
                continue;
            }
            if (c == '\\') {
                raw.push_back(c);
                escaped = true;
                continue;
            }
            if (c == '"') break;
            raw.push_back(c);
        }
        if (pos >= line.size() || line[pos] != '"') return false;
        std::string decoded{};
        if (!unescape_json_string(raw, decoded)) return false;
        out.push_back(std::move(decoded));
        ++pos;
        while (pos < line.size() && std::isspace(static_cast<unsigned char>(line[pos]))) ++pos;
        if (pos < line.size() && line[pos] == ',') {
            ++pos;
            continue;
        }
        if (pos < line.size() && line[pos] == ']') return true;
    }
    return false;
}

std::vector<BundleSourceUnit> parse_bundle_units_json(const std::string& text, std::string& out_err) {
    out_err.clear();
    std::vector<BundleSourceUnit> units{};
    std::istringstream iss(text);
    std::string line{};
    while (std::getline(iss, line)) {
        line = trim(std::move(line));
        if (line.find("\"bundle\"") == std::string::npos ||
            line.find("\"module\"") == std::string::npos ||
            line.find("\"source\"") == std::string::npos ||
            line.find("\"module_imports\"") == std::string::npos ||
            line.find("\"bundle_deps\"") == std::string::npos) {
            continue;
        }

        BundleSourceUnit u{};
        if (!parse_json_string_field_line(line, "bundle", u.bundle) ||
            !parse_json_string_field_line(line, "module", u.module) ||
            !parse_json_string_field_line(line, "source", u.source) ||
            !parse_json_string_array_field_line(line, "module_imports", u.module_imports) ||
            !parse_json_string_array_field_line(line, "bundle_deps", u.bundle_deps)) {
            out_err = "failed to parse LEI --list_sources JSON payload";
            return {};
        }
        if (!canonicalize_import_heads(u.module_imports, out_err)) {
            return {};
        }
        if (!u.bundle.empty() && !u.module.empty() && !u.source.empty()) {
            units.push_back(std::move(u));
        }
    }
    return units;
}

config::Scope to_cfg_scope(cli::ConfigScope s) {
    switch (s) {
        case cli::ConfigScope::kGlobal: return config::Scope::kGlobal;
        case cli::ConfigScope::kProject: return config::Scope::kProject;
        case cli::ConfigScope::kEffective: return config::Scope::kEffective;
        case cli::ConfigScope::kAuto: return config::Scope::kAuto;
    }
    return config::Scope::kAuto;
}

std::optional<std::filesystem::path> config_anchor(const cli::Options& opt) {
    switch (opt.command) {
        case cli::Command::kBuild:
            return std::filesystem::path(opt.build.entry);
        case cli::Command::kCheck:
            return std::filesystem::path(opt.check.entry);
        case cli::Command::kGraph:
            return std::filesystem::path(opt.graph.entry);
        default:
            return std::nullopt;
    }
}

RuntimeConfig load_runtime_config(const cli::Options& opt) {
    RuntimeConfig cfg{};
    const auto anchor = config_anchor(opt);
    cfg.loaded = config::load(anchor);
    cfg.settings = config::materialize(cfg.loaded, &cfg.loaded.warnings);
    if (!opt.toolchain_root.empty()) {
        cfg.settings.toolchain_root = opt.toolchain_root;
    }
    return cfg;
}

std::string resolve_tool_with_config(std::string_view tool_name,
                                     const cli::Options& opt,
                                     const config::EffectiveSettings& settings,
                                     const char* argv0) {
    if (tool_name == "parusc" && !settings.toolchain_parusc_path.empty()) return settings.toolchain_parusc_path;
    if (tool_name == "parusd" && !settings.toolchain_parusd_path.empty()) return settings.toolchain_parusd_path;
    if (tool_name == "parus-lld" && !settings.toolchain_parus_lld_path.empty()) return settings.toolchain_parus_lld_path;
    if (tool_name == "lei" && !settings.toolchain_lei_path.empty()) return settings.toolchain_lei_path;

    toolchain::ResolveOptions ro{};
    ro.toolchain_root = !opt.toolchain_root.empty() ? opt.toolchain_root : settings.toolchain_root;
    ro.argv0 = argv0;
    return toolchain::resolve_tool(tool_name, ro);
}

config::FlatMap make_default_config_map() {
    using config::FlatMap;
    FlatMap v{};
    v["core.config_version"] = int64_t{1};
    v["core.profile"] = std::string("default");

    v["diag.lang"] = std::string("auto");
    v["diag.format"] = std::string("text");
    v["diag.color"] = std::string("auto");
    v["diag.context"] = int64_t{2};
    v["diag.max_errors"] = int64_t{64};
    v["diag.show_code_frame"] = true;

    v["check.follow_lei_sources"] = true;
    v["check.run_lei_validate"] = true;
    v["check.run_parus_syntax"] = true;
    v["check.diag_format"] = std::string("text");
    v["check.diag_lang"] = std::string("auto");
    v["check.diag_context"] = int64_t{2};
    v["check.macro_budget.max_depth"] = int64_t{64};
    v["check.macro_budget.max_steps"] = int64_t{20000};
    v["check.macro_budget.max_output_tokens"] = int64_t{200000};
    v["check.macro_budget.strict_clamp"] = false;

    v["doctor.style"] = std::string("summary");
    v["doctor.show_hints"] = true;
    v["doctor.strict_default"] = false;
    v["doctor.quick_default"] = false;
    v["doctor.color"] = std::string("auto");

    v["toolchain.root"] = std::string("");
    v["toolchain.parus_sysroot"] = std::string("");
    v["toolchain.parusc_path"] = std::string("");
    v["toolchain.parusd_path"] = std::string("");
    v["toolchain.parus_lld_path"] = std::string("");
    v["toolchain.lei_path"] = std::string("");

    v["lsp.diag_lang"] = std::string("auto");
    v["lsp.diag_format"] = std::string("text");
    v["lsp.diag_context"] = int64_t{2};

    v["ui.progress"] = true;
    v["ui.progress_style"] = std::string("compact");
    v["ui.emoji"] = false;
    return v;
}

int run_config(const cli::Options& opt,
               const RuntimeConfig& runtime,
               std::string_view color_mode) {
    using cli::ConfigAction;

    const auto scope = to_cfg_scope(opt.config.scope);
    if (opt.config.action == ConfigAction::kPath) {
        if (scope == config::Scope::kGlobal) {
            std::cout << runtime.loaded.paths.global_config.string() << "\n";
            return 0;
        }
        if (scope == config::Scope::kProject) {
            if (runtime.loaded.paths.project_config.empty()) {
                std::cerr << "error: project root not found; no project config path available\n";
                return 1;
            }
            std::cout << runtime.loaded.paths.project_config.string() << "\n";
            return 0;
        }
        if (scope == config::Scope::kEffective) {
            std::cerr << "error: --effective is not supported for config path\n";
            return 1;
        }
        if (!runtime.loaded.paths.project_config.empty()) {
            std::cout << runtime.loaded.paths.project_config.string() << "\n";
        } else {
            std::cout << runtime.loaded.paths.global_config.string() << "\n";
        }
        return 0;
    }

    if (opt.config.action == ConfigAction::kShow) {
        const auto& values = config::values_for_scope(runtime.loaded, scope);
        if (opt.config.format == cli::ConfigFormat::kJson) {
            std::cout << config::render_json(values) << "\n";
        } else {
            std::cout << config::render_toml(values);
        }
        return 0;
    }

    if (opt.config.action == ConfigAction::kGet) {
        const auto& values = config::values_for_scope(runtime.loaded, scope);
        const auto it = values.find(opt.config.key);
        if (it == values.end()) {
            std::cerr << "error: config key not found: " << opt.config.key << "\n";
            return 1;
        }
        std::cout << config::render_value_text(it->second) << "\n";
        return 0;
    }

    if (opt.config.action == ConfigAction::kSet) {
        if (!config::is_known_key(opt.config.key)) {
            std::cerr << "error: unknown config key: " << opt.config.key << "\n";
            return 1;
        }

        config::Value parsed{};
        std::string parse_err{};
        if (!config::parse_cli_value(opt.config.value, parsed, parse_err)) {
            std::cerr << "error: invalid value for '" << opt.config.key << "': " << parse_err << "\n";
            return 1;
        }

        std::string scope_err{};
        const auto resolved_scope = config::resolve_mutation_scope(scope, runtime.loaded, scope_err);
        if (!scope_err.empty()) {
            std::cerr << "error: " << scope_err << "\n";
            return 1;
        }

        config::FlatMap values = config::values_for_scope(runtime.loaded, resolved_scope);
        values[opt.config.key] = std::move(parsed);

        std::string write_err{};
        if (!config::write_scope_file(runtime.loaded, resolved_scope, values, write_err)) {
            std::cerr << "error: " << write_err << "\n";
            return 1;
        }
        std::cout << "updated " << opt.config.key << "\n";
        return 0;
    }

    if (opt.config.action == ConfigAction::kUnset) {
        if (!config::is_known_key(opt.config.key)) {
            std::cerr << "error: unknown config key: " << opt.config.key << "\n";
            return 1;
        }

        std::string scope_err{};
        const auto resolved_scope = config::resolve_mutation_scope(scope, runtime.loaded, scope_err);
        if (!scope_err.empty()) {
            std::cerr << "error: " << scope_err << "\n";
            return 1;
        }

        config::FlatMap values = config::values_for_scope(runtime.loaded, resolved_scope);
        values.erase(opt.config.key);

        std::string write_err{};
        if (!config::write_scope_file(runtime.loaded, resolved_scope, values, write_err)) {
            std::cerr << "error: " << write_err << "\n";
            return 1;
        }
        std::cout << "unset " << opt.config.key << "\n";
        return 0;
    }

    if (opt.config.action == cli::ConfigAction::kInit) {
        std::string scope_err{};
        const auto resolved_scope = config::resolve_mutation_scope(scope, runtime.loaded, scope_err);
        if (!scope_err.empty()) {
            std::cerr << "error: " << scope_err << "\n";
            return 1;
        }

        config::FlatMap values = config::values_for_scope(runtime.loaded, resolved_scope);
        if (values.empty()) values = make_default_config_map();

        std::string write_err{};
        if (!config::write_scope_file(runtime.loaded, resolved_scope, values, write_err)) {
            std::cerr << "error: " << write_err << "\n";
            return 1;
        }
        if (resolved_scope == config::Scope::kProject) {
            std::cout << "initialized project config: " << runtime.loaded.paths.project_config.string() << "\n";
        } else {
            std::cout << "initialized global config: " << runtime.loaded.paths.global_config.string() << "\n";
        }
        return 0;
    }

    emit_fail(color_mode, "unsupported config action");
    return 1;
}

int run_build(const cli::Options& opt,
              const config::EffectiveSettings& settings,
              const char* argv0) {
    const bool progress = settings.ui_progress;
    emit_progress(progress, settings.diag_color, 10, "Resolving LEI tool");
    const auto lei = resolve_tool_with_config("lei", opt, settings, argv0);
    const auto parusc = resolve_tool_with_config("parusc", opt, settings, argv0);
    const auto parus_lld = resolve_tool_with_config("parus-lld", opt, settings, argv0);

#if defined(_WIN32)
    _putenv_s("PARUSC", parusc.c_str());
    _putenv_s("PARUS_LLD", parus_lld.c_str());
#else
    setenv("PARUSC", parusc.c_str(), 1);
    setenv("PARUS_LLD", parus_lld.c_str(), 1);
#endif

    std::vector<std::string> argv{lei, opt.build.entry, "--build", "--plan", opt.build.plan};
    if (opt.build.jobs.has_value()) {
        argv.push_back("--jobs");
        argv.push_back(std::to_string(*opt.build.jobs));
    }
    if (opt.build.verbose) {
        argv.push_back("--verbose");
    }
    if (opt.build.out_path.has_value()) {
        argv.push_back("--out");
        argv.push_back(*opt.build.out_path);
    }

    emit_progress(progress, settings.diag_color, 35, "Starting build");
    const int rc = proc::run_argv(argv);
    if (rc != 0) {
        emit_fail(settings.diag_color, "Build failed (exit=" + std::to_string(rc) + ")");
        return rc;
    }
    emit_progress(progress, settings.diag_color, 100, "Build completed");
    emit_done(progress, settings.diag_color, "Build completed successfully");
    return 0;
}

int run_graph(const cli::Options& opt,
              const config::EffectiveSettings& settings,
              const char* argv0) {
    const bool progress = settings.ui_progress;
    emit_progress(progress, settings.diag_color, 10, "Resolving LEI tool");
    const auto lei = resolve_tool_with_config("lei", opt, settings, argv0);
    std::vector<std::string> argv{lei, opt.graph.entry, "--view_graph", "--format", opt.graph.format, "--plan", opt.graph.plan};
    emit_progress(progress, settings.diag_color, 40, "Rendering graph");
    const int rc = proc::run_argv(argv);
    if (rc != 0) {
        emit_fail(settings.diag_color, "Graph render failed (exit=" + std::to_string(rc) + ")");
        return rc;
    }
    emit_progress(progress, settings.diag_color, 100, "Graph render completed");
    return 0;
}

int run_lsp(const cli::Options& opt,
            const config::EffectiveSettings& settings,
            const char* argv0) {
    if (!opt.lsp.stdio) {
        std::cerr << "error: lsp mode requires --stdio\n";
        return 1;
    }

    const auto parusd = resolve_tool_with_config("parusd", opt, settings, argv0);
    const bool progress = settings.ui_progress;
    emit_progress(progress, settings.diag_color, 10, "Starting LSP server");
    const int rc = proc::run_argv({parusd, "--stdio"});
    if (rc != 0) {
        emit_fail(settings.diag_color, "LSP server terminated with error (exit=" + std::to_string(rc) + ")");
        return rc;
    }
    emit_done(progress, settings.diag_color, "LSP server exited cleanly");
    return 0;
}

int run_check_one_pr(const std::string& parusc,
                     const std::string& source_path,
                     const std::string& diag_format,
                     const std::string& lang,
                     const std::string& context,
                     const std::vector<std::string>& macro_budget_flags,
                     const std::vector<std::string>& extra_flags = {}) {
    std::vector<std::string> argv{parusc, "-fsyntax-only", source_path};
    if (!diag_format.empty()) {
        argv.push_back("--diag-format");
        argv.push_back(diag_format);
    }
    if (!lang.empty()) {
        argv.push_back("--lang");
        argv.push_back(lang);
    }
    if (!context.empty()) {
        argv.push_back("--context");
        argv.push_back(context);
    }
    for (const auto& f : macro_budget_flags) {
        argv.push_back(f);
    }
    for (const auto& f : extra_flags) {
        argv.push_back(f);
    }
    return proc::run_argv(argv);
}

std::vector<std::string> effective_macro_budget_flags(const cli::Options& opt,
                                                      const config::EffectiveSettings& settings) {
    if (!opt.check.macro_budget_flags.empty()) {
        return opt.check.macro_budget_flags;
    }
    return {
        "-fmacro-max-depth=" + std::to_string(settings.check_macro_budget_max_depth),
        "-fmacro-max-steps=" + std::to_string(settings.check_macro_budget_max_steps),
        "-fmacro-max-output-tokens=" + std::to_string(settings.check_macro_budget_max_output_tokens),
    };
}

int run_check(const cli::Options& opt,
              const config::EffectiveSettings& settings,
              const char* argv0) {
    const bool progress = settings.ui_progress;
    const auto parusc = resolve_tool_with_config("parusc", opt, settings, argv0);
    const std::string diag_format = opt.check.diag_format.value_or(settings.check_diag_format);
    const std::string lang = opt.check.lang.value_or(settings.check_diag_lang);
    const std::string context = opt.check.context.value_or(std::to_string(settings.check_diag_context));
    const auto macro_budget_flags = effective_macro_budget_flags(opt, settings);

    if (is_pr_entry(opt.check.entry)) {
        if (!settings.check_run_parus_syntax) {
            emit_warn(settings.diag_color, "check.run_parus_syntax=false, skipping .pr syntax check");
            return 0;
        }
        emit_progress(progress, settings.diag_color, 20, "Syntax checking " + opt.check.entry);
        const int rc = run_check_one_pr(parusc, opt.check.entry, diag_format, lang, context, macro_budget_flags);
        if (rc != 0) {
            emit_fail(settings.diag_color, "Syntax check failed for " + opt.check.entry + " (exit=" + std::to_string(rc) + ")");
            return rc;
        }
        emit_progress(progress, settings.diag_color, 100, "Syntax check completed");
        return 0;
    }

    emit_progress(progress, settings.diag_color, 10, "Resolving LEI tool");
    const auto lei = resolve_tool_with_config("lei", opt, settings, argv0);

    if (settings.check_run_lei_validate) {
        std::vector<std::string> lei_check{lei, "--check", opt.check.entry, "--plan", opt.check.plan};
        emit_progress(progress, settings.diag_color, 30, "Validating LEI plan");
        const int rc = proc::run_argv(lei_check);
        if (rc != 0) {
            emit_fail(settings.diag_color, "LEI validation failed (exit=" + std::to_string(rc) + ")");
            return rc;
        }
    }

    if (!settings.check_follow_lei_sources) {
        if (settings.check_run_parus_syntax) {
            emit_warn(settings.diag_color, "check.follow_lei_sources=false, skipping source syntax checks");
        }
        emit_progress(progress, settings.diag_color, 100, "Check completed");
        return 0;
    }

    std::string sources_out;
    int list_rc = 1;
    {
        std::vector<std::string> lei_list{
            lei, opt.check.entry, "--list_sources", "--format", "json", "--plan", opt.check.plan
        };
        emit_progress(progress, settings.diag_color, 45, "Collecting source files");
        if (!proc::run_argv_capture_stdout(lei_list, sources_out, list_rc)) {
            emit_fail(settings.diag_color, "Failed to run 'lei --list_sources'");
            return 1;
        }
    }
    if (list_rc != 0) {
        emit_fail(settings.diag_color, "LEI source listing failed (exit=" + std::to_string(list_rc) + ")");
        return list_rc;
    }

    if (!settings.check_run_parus_syntax) {
        emit_progress(progress, settings.diag_color, 100, "Check completed");
        return 0;
    }

    std::error_code ec{};
    const auto entry_abs = std::filesystem::weakly_canonical(std::filesystem::path(opt.check.entry), ec);
    const auto entry_base = (ec ? std::filesystem::path(opt.check.entry) : entry_abs).parent_path();
    const auto bundle_root = entry_base.lexically_normal().string();

    std::string units_err{};
    auto units = parse_bundle_units_json(sources_out, units_err);
    if (!units_err.empty()) {
        emit_fail(settings.diag_color, units_err);
        return 1;
    }
    if (units.empty()) {
        emit_warn(settings.diag_color, "No Parus source files were listed by LEI.");
        emit_progress(progress, settings.diag_color, 100, "Check completed");
        return 0;
    }

    struct BundleInfo {
        std::vector<std::string> sources{};
        std::vector<std::string> deps{};
        std::unordered_map<std::string, std::string> module_head_by_source{};
        std::unordered_map<std::string, std::vector<std::string>> module_imports_by_source{};
    };
    std::unordered_map<std::string, BundleInfo> bundles{};
    std::vector<std::string> bundle_order{};
    for (const auto& u : units) {
        auto it = bundles.find(u.bundle);
        if (it == bundles.end()) {
            bundle_order.push_back(u.bundle);
            it = bundles.emplace(u.bundle, BundleInfo{}).first;
        }
        auto source_path = std::filesystem::path(u.source);
        if (source_path.is_relative()) source_path = entry_base / source_path;
        const auto source_norm = source_path.lexically_normal().string();
        it->second.sources.push_back(source_norm);
        it->second.module_head_by_source[source_norm] = u.module;
        it->second.module_imports_by_source[source_norm] = u.module_imports;
        if (it->second.deps.empty()) {
            it->second.deps = u.bundle_deps;
        }
    }
    for (auto& [name, info] : bundles) {
        std::sort(info.sources.begin(), info.sources.end());
        info.sources.erase(std::unique(info.sources.begin(), info.sources.end()), info.sources.end());
        std::sort(info.deps.begin(), info.deps.end());
        info.deps.erase(std::unique(info.deps.begin(), info.deps.end()), info.deps.end());
    }

    const auto index_dir = (entry_base / ".lei-cache" / "index").lexically_normal();
    ec.clear();
    std::filesystem::create_directories(index_dir, ec);
    if (ec) {
        emit_fail(settings.diag_color, "Failed to create index dir: " + index_dir.string());
        return 1;
    }

    std::unordered_map<std::string, std::string> bundle_index_paths{};
    for (const auto& bname : bundle_order) {
        const auto bit = bundles.find(bname);
        if (bit == bundles.end() || bit->second.sources.empty()) continue;
        const auto idx_path = (index_dir / (bname + ".exports.json")).string();
        bundle_index_paths[bname] = idx_path;

        std::vector<std::string> extra{};
        extra.push_back("--bundle-name");
        extra.push_back(bname);
        extra.push_back("--bundle-root");
        extra.push_back(bundle_root);
        auto first_head = bit->second.module_head_by_source.find(bit->second.sources.front());
        if (first_head != bit->second.module_head_by_source.end()) {
            extra.push_back("--module-head");
            extra.push_back(first_head->second);
        }
        auto first_imports = bit->second.module_imports_by_source.find(bit->second.sources.front());
        if (first_imports != bit->second.module_imports_by_source.end()) {
            for (const auto& mh : first_imports->second) {
                extra.push_back("--module-import");
                extra.push_back(mh);
            }
        }
        extra.push_back("--emit-export-index");
        extra.push_back(idx_path);
        for (const auto& src : bit->second.sources) {
            extra.push_back("--bundle-source");
            extra.push_back(src);
        }
        for (const auto& dep : bit->second.deps) {
            extra.push_back("--bundle-dep");
            extra.push_back(dep);
        }

        emit_progress(progress, settings.diag_color, 52, "Prepass export index for bundle " + bname);
        const int rc = run_check_one_pr(parusc,
                                        bit->second.sources.front(),
                                        diag_format,
                                        lang,
                                        context,
                                        macro_budget_flags,
                                        extra);
        if (rc != 0) {
            emit_fail(settings.diag_color, "Bundle prepass failed for " + bname + " (exit=" + std::to_string(rc) + ")");
            return rc;
        }
    }

    size_t total_sources = 0;
    for (const auto& [_, info] : bundles) total_sources += info.sources.size();
    size_t visited = 0;
    for (const auto& bname : bundle_order) {
        const auto bit = bundles.find(bname);
        if (bit == bundles.end()) continue;
        const auto& info = bit->second;
        for (const auto& src : info.sources) {
            std::vector<std::string> extra{};
            extra.push_back("--bundle-name");
            extra.push_back(bname);
            extra.push_back("--bundle-root");
            extra.push_back(bundle_root);
            auto mhit = info.module_head_by_source.find(src);
            if (mhit != info.module_head_by_source.end()) {
                extra.push_back("--module-head");
                extra.push_back(mhit->second);
            }
            auto miit = info.module_imports_by_source.find(src);
            if (miit != info.module_imports_by_source.end()) {
                for (const auto& mh : miit->second) {
                    extra.push_back("--module-import");
                    extra.push_back(mh);
                }
            }
            for (const auto& all_src : info.sources) {
                extra.push_back("--bundle-source");
                extra.push_back(all_src);
            }
            for (const auto& dep : info.deps) {
                extra.push_back("--bundle-dep");
                extra.push_back(dep);
                std::string dep_idx = (index_dir / (dep + ".exports.json")).string();
                if (auto it = bundle_index_paths.find(dep); it != bundle_index_paths.end()) {
                    dep_idx = it->second;
                }
                extra.push_back("--load-export-index");
                extra.push_back(dep_idx);
            }

            ++visited;
            const int pct = static_cast<int>(55 + (visited * 45) / (total_sources == 0 ? 1 : total_sources));
            emit_progress(progress, settings.diag_color, pct, "Syntax checking " + src);
            const int rc = run_check_one_pr(parusc,
                                            src,
                                            diag_format,
                                            lang,
                                            context,
                                            macro_budget_flags,
                                            extra);
            if (rc != 0) {
                emit_fail(settings.diag_color, "Syntax check failed for " + src + " (exit=" + std::to_string(rc) + ")");
                return rc;
            }
        }
    }

    emit_progress(progress, settings.diag_color, 100, "Check completed");
    return 0;
}

int run_tool(const cli::Options& opt,
             const config::EffectiveSettings& settings,
             const char* argv0) {
    const auto resolved = resolve_tool_with_config(opt.tool.tool_name, opt, settings, argv0);

    std::vector<std::string> argv{resolved};
    argv.insert(argv.end(), opt.tool.passthrough.begin(), opt.tool.passthrough.end());
    return proc::run_argv(argv);
}

int run_doctor(const cli::Options& opt,
               config::EffectiveSettings settings,
               const char* argv0) {
    auto doctor_opt = opt.doctor;
    if (!doctor_opt.quick && settings.doctor_quick_default) doctor_opt.quick = true;
    if (!doctor_opt.strict && settings.doctor_strict_default) doctor_opt.strict = true;
    if (!doctor_opt.verbose && settings.doctor_style == "verbose") doctor_opt.verbose = true;
    doctor_opt.show_hints = settings.doctor_show_hints;
    doctor_opt.color = settings.doctor_color;

    if (!settings.toolchain_parus_sysroot.empty() && std::getenv("PARUS_SYSROOT") == nullptr) {
#if defined(_WIN32)
        _putenv_s("PARUS_SYSROOT", settings.toolchain_parus_sysroot.c_str());
#else
        setenv("PARUS_SYSROOT", settings.toolchain_parus_sysroot.c_str(), 0);
#endif
    }

    const auto toolchain_root = !opt.toolchain_root.empty() ? opt.toolchain_root : settings.toolchain_root;
    auto report = doctor::run(doctor_opt, toolchain_root, argv0);
    if (doctor_opt.json) {
        std::cout << report.render_json() << "\n";
    } else {
        std::cout << report.render_text(doctor_opt.verbose, doctor_opt.show_hints, doctor_opt.color);
    }
    return report.exit_code(doctor_opt.strict);
}

} // namespace

int run(const cli::Options& opt, const char* argv0) {
    const auto runtime = load_runtime_config(opt);
    for (const auto& w : runtime.loaded.warnings) {
        emit_warn(runtime.settings.diag_color, w);
    }

    if (opt.command == cli::Command::kConfig) {
        return run_config(opt, runtime, runtime.settings.diag_color);
    }

    switch (opt.command) {
        case cli::Command::kBuild:
            return run_build(opt, runtime.settings, argv0);
        case cli::Command::kCheck:
            return run_check(opt, runtime.settings, argv0);
        case cli::Command::kGraph:
            return run_graph(opt, runtime.settings, argv0);
        case cli::Command::kLsp:
            return run_lsp(opt, runtime.settings, argv0);
        case cli::Command::kDoctor:
            return run_doctor(opt, runtime.settings, argv0);
        case cli::Command::kTool:
            return run_tool(opt, runtime.settings, argv0);
        default:
            return 1;
    }
}

} // namespace parus_tool::driver
