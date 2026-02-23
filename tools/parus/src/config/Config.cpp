#include <parus_tool/config/Config.hpp>

#include <parus_tool/config/TomlLite.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <set>
#include <sstream>
#include <unordered_set>

namespace parus_tool::config {

namespace {

std::string getenv_string(const char* key) {
    if (key == nullptr) return {};
    const char* p = std::getenv(key);
    if (p == nullptr) return {};
    return std::string(p);
}

std::filesystem::path home_dir() {
    std::string home = getenv_string("HOME");
#if defined(_WIN32)
    if (home.empty()) {
        home = getenv_string("USERPROFILE");
    }
#endif
    return home.empty() ? std::filesystem::current_path() : std::filesystem::path(home);
}

std::filesystem::path compute_global_config_path() {
    const std::string xdg = getenv_string("XDG_CONFIG_HOME");
    if (!xdg.empty()) {
        return std::filesystem::path(xdg) / "parus" / "config.toml";
    }

#if defined(__APPLE__)
    return home_dir() / "Library" / "Application Support" / "parus" / "config.toml";
#else
    return home_dir() / ".config" / "parus" / "config.toml";
#endif
}

const std::unordered_set<std::string>& known_keys_() {
    static const std::unordered_set<std::string> k{
        "core.config_version",
        "core.profile",

        "diag.lang",
        "diag.format",
        "diag.color",
        "diag.context",
        "diag.max_errors",
        "diag.show_code_frame",

        "check.follow_lei_sources",
        "check.run_lei_validate",
        "check.run_parus_syntax",
        "check.diag_format",
        "check.diag_lang",
        "check.diag_context",
        "check.macro_budget.max_depth",
        "check.macro_budget.max_steps",
        "check.macro_budget.max_output_tokens",
        "check.macro_budget.strict_clamp",

        "doctor.style",
        "doctor.show_hints",
        "doctor.strict_default",
        "doctor.quick_default",
        "doctor.color",

        "toolchain.root",
        "toolchain.parus_sysroot",
        "toolchain.parusc_path",
        "toolchain.parusd_path",
        "toolchain.parus_lld_path",
        "toolchain.lei_path",

        "lsp.diag_lang",
        "lsp.diag_format",
        "lsp.diag_context",

        "ui.progress",
        "ui.progress_style",
        "ui.emoji",
    };
    return k;
}

template <typename T>
const T* as_ptr(const Value* v) {
    if (v == nullptr) return nullptr;
    return std::get_if<T>(v);
}

std::string json_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

std::string toml_escape(std::string_view s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c); break;
        }
    }
    return out;
}

void merge_into(FlatMap& base, const FlatMap& override_map) {
    for (const auto& [k, v] : override_map) {
        base[k] = v;
    }
}

void filter_unknown_keys(FlatMap& values, std::vector<std::string>& warnings, std::string_view source_name) {
    std::vector<std::string> to_erase{};
    for (const auto& [k, _] : values) {
        if (!is_known_key(k)) {
            warnings.push_back(std::string(source_name) + ": unknown key '" + k + "' ignored");
            to_erase.push_back(k);
        }
    }
    for (const auto& k : to_erase) {
        values.erase(k);
    }
}

std::string section_of(const std::string& key) {
    const auto pos = key.rfind('.');
    if (pos == std::string::npos) return {};
    return key.substr(0, pos);
}

std::string leaf_of(const std::string& key) {
    const auto pos = key.rfind('.');
    if (pos == std::string::npos) return key;
    return key.substr(pos + 1);
}

void apply_env_string(std::string& dst, const char* key) {
    const auto v = getenv_string(key);
    if (!v.empty()) dst = v;
}

void apply_env_int(int64_t& dst, const char* key) {
    const auto v = getenv_string(key);
    if (v.empty()) return;
    try {
        dst = std::stoll(v);
    } catch (...) {
    }
}

std::string normalize_lang(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "en" || value == "ko" || value == "auto") return value;
    return "auto";
}

std::string normalize_diag_format(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "text" || value == "json") return value;
    return "text";
}

std::string normalize_color(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "auto" || value == "always" || value == "never") return value;
    return "auto";
}

std::string normalize_style(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "summary" || value == "verbose") return value;
    return "summary";
}

std::string normalize_progress_style(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (value == "compact" || value == "detailed") return value;
    return "compact";
}

} // namespace

bool is_known_key(std::string_view key) {
    return known_keys_().contains(std::string(key));
}

std::optional<std::filesystem::path> find_project_root(std::filesystem::path start) {
    std::error_code ec{};
    if (start.empty()) start = std::filesystem::current_path(ec);
    if (ec) return std::nullopt;

    if (!std::filesystem::exists(start, ec)) return std::nullopt;
    if (!std::filesystem::is_directory(start, ec)) {
        start = start.parent_path();
    }

    for (std::filesystem::path cur = start; !cur.empty(); cur = cur.parent_path()) {
        if (std::filesystem::exists(cur / "config.lei", ec) ||
            std::filesystem::exists(cur / ".git", ec)) {
            return cur;
        }
        const auto parent = cur.parent_path();
        if (parent == cur) break;
    }
    return std::nullopt;
}

Paths resolve_paths(const std::optional<std::filesystem::path>& anchor) {
    std::filesystem::path start{};
    if (anchor.has_value()) {
        start = *anchor;
    } else {
        std::error_code ec{};
        start = std::filesystem::current_path(ec);
        if (ec) start = ".";
    }

    Paths out{};
    out.global_config = compute_global_config_path();
    out.project_root = find_project_root(start);
    if (out.project_root.has_value()) {
        out.project_config = *out.project_root / ".parus" / "config.toml";
    }
    return out;
}

LoadedConfig load(const std::optional<std::filesystem::path>& anchor) {
    LoadedConfig out{};
    out.paths = resolve_paths(anchor);

    {
        std::string err{};
        if (!toml_lite::parse_file(out.paths.global_config, out.global_values, out.warnings, err)) {
            out.warnings.push_back("failed to load global config: " + err);
        }
    }
    filter_unknown_keys(out.global_values, out.warnings, out.paths.global_config.string());
    if (!out.paths.project_config.empty()) {
        std::string err{};
        if (!toml_lite::parse_file(out.paths.project_config, out.project_values, out.warnings, err)) {
            out.warnings.push_back("failed to load project config: " + err);
        }
        filter_unknown_keys(out.project_values, out.warnings, out.paths.project_config.string());
    }

    out.effective_values = out.global_values;
    merge_into(out.effective_values, out.project_values);
    return out;
}

EffectiveSettings materialize(const LoadedConfig& cfg, std::vector<std::string>* warnings) {
    EffectiveSettings s{};
    const FlatMap& v = cfg.effective_values;

    auto get_string = [&](std::string_view key, std::string& dst) {
        const auto it = v.find(std::string(key));
        if (it == v.end()) return;
        if (const auto* p = as_ptr<std::string>(&it->second); p != nullptr) {
            dst = *p;
            return;
        }
        if (warnings != nullptr) warnings->push_back("config key '" + std::string(key) + "' has wrong type (expected string)");
    };
    auto get_int = [&](std::string_view key, int64_t& dst) {
        const auto it = v.find(std::string(key));
        if (it == v.end()) return;
        if (const auto* p = as_ptr<int64_t>(&it->second); p != nullptr) {
            dst = *p;
            return;
        }
        if (warnings != nullptr) warnings->push_back("config key '" + std::string(key) + "' has wrong type (expected int)");
    };
    auto get_bool = [&](std::string_view key, bool& dst) {
        const auto it = v.find(std::string(key));
        if (it == v.end()) return;
        if (const auto* p = as_ptr<bool>(&it->second); p != nullptr) {
            dst = *p;
            return;
        }
        if (warnings != nullptr) warnings->push_back("config key '" + std::string(key) + "' has wrong type (expected bool)");
    };

    get_int("core.config_version", s.core_config_version);
    get_string("core.profile", s.core_profile);

    get_string("diag.lang", s.diag_lang);
    get_string("diag.format", s.diag_format);
    get_string("diag.color", s.diag_color);
    get_int("diag.context", s.diag_context);
    get_int("diag.max_errors", s.diag_max_errors);
    get_bool("diag.show_code_frame", s.diag_show_code_frame);

    get_bool("check.follow_lei_sources", s.check_follow_lei_sources);
    get_bool("check.run_lei_validate", s.check_run_lei_validate);
    get_bool("check.run_parus_syntax", s.check_run_parus_syntax);
    get_string("check.diag_format", s.check_diag_format);
    get_string("check.diag_lang", s.check_diag_lang);
    get_int("check.diag_context", s.check_diag_context);
    get_int("check.macro_budget.max_depth", s.check_macro_budget_max_depth);
    get_int("check.macro_budget.max_steps", s.check_macro_budget_max_steps);
    get_int("check.macro_budget.max_output_tokens", s.check_macro_budget_max_output_tokens);
    get_bool("check.macro_budget.strict_clamp", s.check_macro_budget_strict_clamp);

    get_string("doctor.style", s.doctor_style);
    get_bool("doctor.show_hints", s.doctor_show_hints);
    get_bool("doctor.strict_default", s.doctor_strict_default);
    get_bool("doctor.quick_default", s.doctor_quick_default);
    get_string("doctor.color", s.doctor_color);

    get_string("toolchain.root", s.toolchain_root);
    get_string("toolchain.parus_sysroot", s.toolchain_parus_sysroot);
    get_string("toolchain.parusc_path", s.toolchain_parusc_path);
    get_string("toolchain.parusd_path", s.toolchain_parusd_path);
    get_string("toolchain.parus_lld_path", s.toolchain_parus_lld_path);
    get_string("toolchain.lei_path", s.toolchain_lei_path);

    get_string("lsp.diag_lang", s.lsp_diag_lang);
    get_string("lsp.diag_format", s.lsp_diag_format);
    get_int("lsp.diag_context", s.lsp_diag_context);

    get_bool("ui.progress", s.ui_progress);
    get_string("ui.progress_style", s.ui_progress_style);
    get_bool("ui.emoji", s.ui_emoji);

    s.diag_lang = normalize_lang(s.diag_lang);
    s.diag_format = normalize_diag_format(s.diag_format);
    s.diag_color = normalize_color(s.diag_color);
    s.check_diag_lang = normalize_lang(s.check_diag_lang);
    s.check_diag_format = normalize_diag_format(s.check_diag_format);
    s.doctor_style = normalize_style(s.doctor_style);
    s.doctor_color = normalize_color(s.doctor_color);
    s.lsp_diag_lang = normalize_lang(s.lsp_diag_lang);
    s.lsp_diag_format = normalize_diag_format(s.lsp_diag_format);
    s.ui_progress_style = normalize_progress_style(s.ui_progress_style);

    apply_env_string(s.toolchain_root, "PARUS_TOOLCHAIN_ROOT");
    apply_env_string(s.toolchain_parus_sysroot, "PARUS_SYSROOT");
    apply_env_string(s.diag_lang, "PARUS_DIAG_LANG");
    apply_env_string(s.diag_format, "PARUS_DIAG_FORMAT");
    apply_env_int(s.diag_context, "PARUS_DIAG_CONTEXT");
    s.diag_lang = normalize_lang(s.diag_lang);
    s.diag_format = normalize_diag_format(s.diag_format);

    if (s.diag_context < 0) s.diag_context = 2;
    if (s.diag_max_errors < 1) s.diag_max_errors = 64;
    if (s.check_diag_context < 0) s.check_diag_context = 2;
    if (s.lsp_diag_context < 0) s.lsp_diag_context = 2;
    if (s.check_macro_budget_max_depth < 1) s.check_macro_budget_max_depth = 64;
    if (s.check_macro_budget_max_steps < 1) s.check_macro_budget_max_steps = 20000;
    if (s.check_macro_budget_max_output_tokens < 1) s.check_macro_budget_max_output_tokens = 200000;

    return s;
}

const FlatMap& values_for_scope(const LoadedConfig& cfg, Scope scope) {
    switch (scope) {
        case Scope::kGlobal:
            return cfg.global_values;
        case Scope::kProject:
            return cfg.project_values;
        case Scope::kEffective:
            return cfg.effective_values;
        case Scope::kAuto:
            if (cfg.paths.project_root.has_value()) return cfg.project_values;
            return cfg.global_values;
    }
    return cfg.effective_values;
}

Scope resolve_mutation_scope(Scope requested, const LoadedConfig& cfg, std::string& err) {
    if (requested == Scope::kEffective) {
        err = "effective scope is read-only";
        return Scope::kEffective;
    }
    if (requested == Scope::kGlobal) return Scope::kGlobal;
    if (requested == Scope::kProject) {
        if (!cfg.paths.project_root.has_value()) {
            err = "project root not found for --project scope";
        }
        return Scope::kProject;
    }
    if (cfg.paths.project_root.has_value()) return Scope::kProject;
    return Scope::kGlobal;
}

bool write_scope_file(const LoadedConfig& cfg, Scope scope, const FlatMap& values, std::string& err) {
    if (scope == Scope::kEffective) {
        err = "cannot write effective scope";
        return false;
    }
    if (scope == Scope::kProject) {
        if (!cfg.paths.project_root.has_value()) {
            err = "project root not found";
            return false;
        }
        return toml_lite::write_file(cfg.paths.project_config, values, err);
    }
    return toml_lite::write_file(cfg.paths.global_config, values, err);
}

bool parse_cli_value(std::string_view text, Value& out, std::string& err) {
    std::string parsed;
    parsed.assign(text.begin(), text.end());
    auto trim = [](std::string& s) {
        const auto is_space = [](unsigned char c) {
            return c == ' ' || c == '\t' || c == '\r' || c == '\n';
        };
        while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
        while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
    };
    trim(parsed);
    if (parsed.empty()) {
        err = "value cannot be empty";
        return false;
    }

    if (parsed == "true") {
        out = true;
        return true;
    }
    if (parsed == "false") {
        out = false;
        return true;
    }

    auto is_integer = [&](const std::string& s) {
        if (s.empty()) return false;
        size_t i = 0;
        if (s[0] == '-' || s[0] == '+') i = 1;
        if (i >= s.size()) return false;
        for (; i < s.size(); ++i) {
            if (s[i] < '0' || s[i] > '9') return false;
        }
        return true;
    };
    if (is_integer(parsed)) {
        try {
            out = static_cast<int64_t>(std::stoll(parsed));
            return true;
        } catch (...) {
            err = "invalid integer value: " + parsed;
            return false;
        }
    }

    if (parsed.front() == '[' && parsed.back() == ']') {
        std::string inner = parsed.substr(1, parsed.size() - 2);
        std::vector<std::string> parts{};
        std::string cur{};
        bool in_str = false;
        bool esc = false;
        for (char c : inner) {
            if (in_str) {
                cur.push_back(c);
                if (esc) {
                    esc = false;
                } else if (c == '\\') {
                    esc = true;
                } else if (c == '"') {
                    in_str = false;
                }
                continue;
            }
            if (c == '"') {
                in_str = true;
                cur.push_back(c);
                continue;
            }
            if (c == ',') {
                trim(cur);
                if (!cur.empty()) parts.push_back(cur);
                cur.clear();
                continue;
            }
            cur.push_back(c);
        }
        trim(cur);
        if (!cur.empty()) parts.push_back(cur);

        if (parts.empty()) {
            out = std::vector<std::string>{};
            return true;
        }

        bool all_int = true;
        bool all_str = true;
        std::vector<int64_t> ints{};
        std::vector<std::string> strs{};
        for (auto& p : parts) {
            trim(p);
            if (p.size() >= 2 && p.front() == '"' && p.back() == '"') {
                strs.push_back(p.substr(1, p.size() - 2));
                all_int = false;
                continue;
            }
            if (is_integer(p)) {
                try {
                    ints.push_back(static_cast<int64_t>(std::stoll(p)));
                    all_str = false;
                    continue;
                } catch (...) {
                    err = "invalid integer element in array: " + p;
                    return false;
                }
            }
            all_int = false;
            all_str = false;
            break;
        }
        if (all_int) {
            out = std::move(ints);
            return true;
        }
        if (all_str) {
            out = std::move(strs);
            return true;
        }
        err = "array must be homogeneous [string] or [int]";
        return false;
    }

    if (parsed.size() >= 2 && parsed.front() == '"' && parsed.back() == '"') {
        out = parsed.substr(1, parsed.size() - 2);
        return true;
    }

    // Accept bare word as string for usability.
    out = parsed;
    return true;
}

std::string render_value_toml(const Value& v) {
    if (const auto* p = as_ptr<std::string>(&v); p != nullptr) {
        return "\"" + toml_escape(*p) + "\"";
    }
    if (const auto* p = as_ptr<int64_t>(&v); p != nullptr) {
        return std::to_string(*p);
    }
    if (const auto* p = as_ptr<bool>(&v); p != nullptr) {
        return *p ? "true" : "false";
    }
    if (const auto* p = as_ptr<std::vector<std::string>>(&v); p != nullptr) {
        std::ostringstream oss;
        oss << "[";
        for (size_t i = 0; i < p->size(); ++i) {
            if (i != 0) oss << ", ";
            oss << "\"" << toml_escape((*p)[i]) << "\"";
        }
        oss << "]";
        return oss.str();
    }
    const auto* p = as_ptr<std::vector<int64_t>>(&v);
    std::ostringstream oss;
    oss << "[";
    if (p != nullptr) {
        for (size_t i = 0; i < p->size(); ++i) {
            if (i != 0) oss << ", ";
            oss << (*p)[i];
        }
    }
    oss << "]";
    return oss.str();
}

std::string render_value_json(const Value& v) {
    if (const auto* p = as_ptr<std::string>(&v); p != nullptr) {
        return "\"" + json_escape(*p) + "\"";
    }
    if (const auto* p = as_ptr<int64_t>(&v); p != nullptr) {
        return std::to_string(*p);
    }
    if (const auto* p = as_ptr<bool>(&v); p != nullptr) {
        return *p ? "true" : "false";
    }
    if (const auto* p = as_ptr<std::vector<std::string>>(&v); p != nullptr) {
        std::ostringstream oss;
        oss << "[";
        for (size_t i = 0; i < p->size(); ++i) {
            if (i != 0) oss << ",";
            oss << "\"" << json_escape((*p)[i]) << "\"";
        }
        oss << "]";
        return oss.str();
    }
    const auto* p = as_ptr<std::vector<int64_t>>(&v);
    std::ostringstream oss;
    oss << "[";
    if (p != nullptr) {
        for (size_t i = 0; i < p->size(); ++i) {
            if (i != 0) oss << ",";
            oss << (*p)[i];
        }
    }
    oss << "]";
    return oss.str();
}

std::string render_value_text(const Value& v) {
    if (const auto* p = as_ptr<std::string>(&v); p != nullptr) return *p;
    if (const auto* p = as_ptr<int64_t>(&v); p != nullptr) return std::to_string(*p);
    if (const auto* p = as_ptr<bool>(&v); p != nullptr) return *p ? "true" : "false";
    if (const auto* p = as_ptr<std::vector<std::string>>(&v); p != nullptr) {
        std::ostringstream oss;
        for (size_t i = 0; i < p->size(); ++i) {
            if (i != 0) oss << ",";
            oss << (*p)[i];
        }
        return oss.str();
    }
    const auto* p = as_ptr<std::vector<int64_t>>(&v);
    std::ostringstream oss;
    if (p != nullptr) {
        for (size_t i = 0; i < p->size(); ++i) {
            if (i != 0) oss << ",";
            oss << (*p)[i];
        }
    }
    return oss.str();
}

std::string render_toml(const FlatMap& values) {
    std::map<std::string, std::map<std::string, Value>> groups{};
    for (const auto& [key, val] : values) {
        groups[section_of(key)][leaf_of(key)] = val;
    }

    std::ostringstream oss;
    bool first_group = true;
    for (const auto& [section, kv] : groups) {
        if (!first_group) oss << "\n";
        first_group = false;
        if (!section.empty()) {
            oss << "[" << section << "]\n";
        }
        for (const auto& [leaf, val] : kv) {
            oss << leaf << " = " << render_value_toml(val) << "\n";
        }
    }
    return oss.str();
}

std::string render_json(const FlatMap& values) {
    std::ostringstream oss;
    oss << "{";
    bool first = true;
    for (const auto& [key, val] : values) {
        if (!first) oss << ",";
        first = false;
        oss << "\"" << json_escape(key) << "\":" << render_value_json(val);
    }
    oss << "}";
    return oss.str();
}

} // namespace parus_tool::config
