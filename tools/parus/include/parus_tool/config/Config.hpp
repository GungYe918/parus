#pragma once

#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace parus_tool::config {

using Value = std::variant<std::string, int64_t, bool, std::vector<std::string>, std::vector<int64_t>>;
using FlatMap = std::map<std::string, Value>;

enum class Scope : uint8_t {
    kAuto,
    kGlobal,
    kProject,
    kEffective,
};

enum class OutputFormat : uint8_t {
    kToml,
    kJson,
};

struct Paths {
    std::filesystem::path global_config{};
    std::filesystem::path project_config{};
    std::optional<std::filesystem::path> project_root{};
};

struct LoadedConfig {
    Paths paths{};
    FlatMap global_values{};
    FlatMap project_values{};
    FlatMap effective_values{};
    std::vector<std::string> warnings{};
};

struct EffectiveSettings {
    int64_t core_config_version = 1;
    std::string core_profile = "default";

    std::string diag_lang = "auto";
    std::string diag_format = "text";
    std::string diag_color = "auto";
    int64_t diag_context = 2;
    int64_t diag_max_errors = 64;
    bool diag_show_code_frame = true;

    bool check_follow_lei_sources = true;
    bool check_run_lei_validate = true;
    bool check_run_parus_syntax = true;
    std::string check_diag_format = "text";
    std::string check_diag_lang = "auto";
    int64_t check_diag_context = 2;
    int64_t check_macro_budget_max_depth = 64;
    int64_t check_macro_budget_max_steps = 20000;
    int64_t check_macro_budget_max_output_tokens = 200000;
    bool check_macro_budget_strict_clamp = false;

    std::string doctor_style = "summary";
    bool doctor_show_hints = true;
    bool doctor_strict_default = false;
    bool doctor_quick_default = false;
    std::string doctor_color = "auto";

    std::string toolchain_root{};
    std::string toolchain_parus_sysroot{};
    std::string toolchain_parusc_path{};
    std::string toolchain_parusd_path{};
    std::string toolchain_parus_lld_path{};
    std::string toolchain_lei_path{};

    std::string lsp_diag_lang = "auto";
    std::string lsp_diag_format = "text";
    int64_t lsp_diag_context = 2;

    bool ui_progress = true;
    std::string ui_progress_style = "compact";
    bool ui_emoji = false;
};

std::optional<std::filesystem::path> find_project_root(std::filesystem::path start);
Paths resolve_paths(const std::optional<std::filesystem::path>& anchor);

LoadedConfig load(const std::optional<std::filesystem::path>& anchor);
EffectiveSettings materialize(const LoadedConfig& cfg, std::vector<std::string>* warnings = nullptr);

bool is_known_key(std::string_view key);
bool parse_cli_value(std::string_view text, Value& out, std::string& err);

const FlatMap& values_for_scope(const LoadedConfig& cfg, Scope scope);
Scope resolve_mutation_scope(Scope requested, const LoadedConfig& cfg, std::string& err);
bool write_scope_file(const LoadedConfig& cfg, Scope scope, const FlatMap& values, std::string& err);

std::string render_toml(const FlatMap& values);
std::string render_json(const FlatMap& values);
std::string render_value_toml(const Value& v);
std::string render_value_json(const Value& v);
std::string render_value_text(const Value& v);

} // namespace parus_tool::config
