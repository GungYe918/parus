#include <lei/builtins/ValueUtil.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lei::builtins::detail {

namespace {

#ifndef LEI_PARUS_VERSION_MAJOR
#define LEI_PARUS_VERSION_MAJOR 0
#endif

#ifndef LEI_PARUS_VERSION_MINOR
#define LEI_PARUS_VERSION_MINOR 1
#endif

#ifndef LEI_PARUS_VERSION_PATCH
#define LEI_PARUS_VERSION_PATCH 0
#endif

#ifndef LEI_PARUS_TOOL_PARUSC
#define LEI_PARUS_TOOL_PARUSC "parusc"
#endif

#ifndef LEI_PARUS_TOOL_PARUSD
#define LEI_PARUS_TOOL_PARUSD "parusd"
#endif

#ifndef LEI_PARUS_TOOL_PARUS_LLD
#define LEI_PARUS_TOOL_PARUS_LLD "parus-lld"
#endif

#ifndef LEI_PARUS_ENABLE_AOT_BACKEND
#define LEI_PARUS_ENABLE_AOT_BACKEND 1
#endif

#ifndef LEI_PARUS_ENABLE_JIT_BACKEND
#define LEI_PARUS_ENABLE_JIT_BACKEND 1
#endif

#ifndef LEI_PARUS_ENABLE_WASM_BACKEND
#define LEI_PARUS_ENABLE_WASM_BACKEND 1
#endif

#ifndef LEI_PARUS_AOT_ENABLE_LLVM
#define LEI_PARUS_AOT_ENABLE_LLVM 1
#endif

#ifndef LEI_PARUS_LLVM_LANE_SELECTED
#define LEI_PARUS_LLVM_LANE_SELECTED 20
#endif

#ifndef LEI_PARUS_DEFAULT_TARGET
#define LEI_PARUS_DEFAULT_TARGET ""
#endif

#ifndef LEI_PARUS_MACRO_BUDGET_AOT_DEPTH
#define LEI_PARUS_MACRO_BUDGET_AOT_DEPTH 64
#endif

#ifndef LEI_PARUS_MACRO_BUDGET_AOT_STEPS
#define LEI_PARUS_MACRO_BUDGET_AOT_STEPS 20000
#endif

#ifndef LEI_PARUS_MACRO_BUDGET_AOT_OUTPUT_TOKENS
#define LEI_PARUS_MACRO_BUDGET_AOT_OUTPUT_TOKENS 200000
#endif

#ifndef LEI_PARUS_MACRO_BUDGET_JIT_DEPTH
#define LEI_PARUS_MACRO_BUDGET_JIT_DEPTH 32
#endif

#ifndef LEI_PARUS_MACRO_BUDGET_JIT_STEPS
#define LEI_PARUS_MACRO_BUDGET_JIT_STEPS 8000
#endif

#ifndef LEI_PARUS_MACRO_BUDGET_JIT_OUTPUT_TOKENS
#define LEI_PARUS_MACRO_BUDGET_JIT_OUTPUT_TOKENS 80000
#endif

#ifndef LEI_PARUS_MACRO_BUDGET_HARD_MAX_DEPTH
#define LEI_PARUS_MACRO_BUDGET_HARD_MAX_DEPTH 256
#endif

#ifndef LEI_PARUS_MACRO_BUDGET_HARD_MAX_STEPS
#define LEI_PARUS_MACRO_BUDGET_HARD_MAX_STEPS 200000
#endif

#ifndef LEI_PARUS_MACRO_BUDGET_HARD_MAX_OUTPUT_TOKENS
#define LEI_PARUS_MACRO_BUDGET_HARD_MAX_OUTPUT_TOKENS 1000000
#endif

std::string host_os() {
#if defined(_WIN32)
    return "windows";
#elif defined(__APPLE__) && defined(__MACH__)
    return "macos";
#elif defined(__linux__)
    return "linux";
#else
    return "unknown";
#endif
}

std::string host_arch() {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "aarch64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__arm__) || defined(_M_ARM)
    return "arm";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown";
#endif
}

std::string host_target_triple() {
    const auto arch = host_arch();
    const auto os = host_os();
    if (os == "macos") return arch + "-apple-darwin";
    if (os == "linux") return arch + "-unknown-linux-gnu";
    if (os == "windows") return arch + "-pc-windows-msvc";
    return arch + "-unknown-unknown";
}

std::string parus_default_target_string() {
    constexpr std::string_view configured = LEI_PARUS_DEFAULT_TARGET;
    if (!configured.empty()) return std::string(configured);
    return host_target_triple();
}

std::string parus_version_string() {
    return std::to_string(LEI_PARUS_VERSION_MAJOR) + "."
         + std::to_string(LEI_PARUS_VERSION_MINOR) + "."
         + std::to_string(LEI_PARUS_VERSION_PATCH);
}

eval::Value::Array make_string_array(std::initializer_list<const char*> values) {
    eval::Value::Array out{};
    out.reserve(values.size());
    for (const char* v : values) out.push_back(util::make_string(v));
    return out;
}

bool backend_enabled_impl(std::string_view name) {
    if (name == "aot") return LEI_PARUS_ENABLE_AOT_BACKEND != 0;
    if (name == "jit") return LEI_PARUS_ENABLE_JIT_BACKEND != 0;
    if (name == "wasm") return LEI_PARUS_ENABLE_WASM_BACKEND != 0;
    return false;
}

bool aot_engine_enabled_impl(std::string_view name) {
    if (name == "llvm") {
        return LEI_PARUS_ENABLE_AOT_BACKEND != 0 && LEI_PARUS_AOT_ENABLE_LLVM != 0;
    }
    return false;
}

bool llvm_lane_supported_impl(int64_t lane) {
    return lane == 20 || lane == 21;
}

std::optional<std::string> find_tool_path(std::string_view name) {
    const char* path_env = std::getenv("PATH");
    if (!path_env) return std::nullopt;
    const std::string path_list(path_env);
#ifdef _WIN32
    const char sep = ';';
#else
    const char sep = ':';
#endif
    size_t pos = 0;
    while (pos <= path_list.size()) {
        const size_t next = path_list.find(sep, pos);
        const std::string_view part = (next == std::string::npos)
            ? std::string_view(path_list).substr(pos)
            : std::string_view(path_list).substr(pos, next - pos);
        if (!part.empty()) {
            std::filesystem::path p(part);
            p /= std::string(name);
            std::error_code ec{};
            if (std::filesystem::exists(p, ec)) return p.lexically_normal().string();
#ifdef _WIN32
            p += ".exe";
            if (std::filesystem::exists(p, ec)) return p.lexically_normal().string();
#endif
        }
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return std::nullopt;
}

std::string normalize_bundle_name_impl(std::string name) {
    std::string out{};
    out.reserve(name.size());
    bool prev_underscore = false;
    for (char ch : name) {
        const unsigned char u = static_cast<unsigned char>(ch);
        char mapped = 0;
        if (std::isalnum(u)) {
            mapped = static_cast<char>(std::tolower(u));
        } else {
            mapped = '_';
        }

        if (mapped == '_') {
            if (prev_underscore) continue;
            prev_underscore = true;
        } else {
            prev_underscore = false;
        }
        out.push_back(mapped);
    }

    while (!out.empty() && out.front() == '_') out.erase(out.begin());
    while (!out.empty() && out.back() == '_') out.pop_back();
    if (out.empty()) return "bundle";
    return out;
}

std::optional<eval::Value> fn_default_target(const std::vector<eval::Value>& args,
                                             const ast::Span& span,
                                             diag::Bag& diags) {
    if (!util::expect_arg_count(args, 0, "parus.default_target", span, diags)) return std::nullopt;
    return util::make_string(parus_default_target_string());
}

std::optional<eval::Value> fn_host_target(const std::vector<eval::Value>& args,
                                          const ast::Span& span,
                                          diag::Bag& diags) {
    if (!util::expect_arg_count(args, 0, "parus.host_target", span, diags)) return std::nullopt;
    return util::make_string(host_target_triple());
}

std::optional<eval::Value> fn_tool_path(const std::vector<eval::Value>& args,
                                        const ast::Span& span,
                                        diag::Bag& diags) {
    std::string name;
    if (!util::expect_arg_count(args, 1, "parus.tool_path", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, name, "parus.tool_path", span, diags)) return std::nullopt;
    auto found = find_tool_path(name);
    return util::make_string(found.has_value() ? *found : name);
}

std::optional<eval::Value> fn_backend_enabled(const std::vector<eval::Value>& args,
                                              const ast::Span& span,
                                              diag::Bag& diags) {
    std::string name;
    if (!util::expect_arg_count(args, 1, "parus.backend_enabled", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, name, "parus.backend_enabled", span, diags)) return std::nullopt;
    return util::make_bool(backend_enabled_impl(name));
}

std::optional<eval::Value> fn_aot_engine_enabled(const std::vector<eval::Value>& args,
                                                 const ast::Span& span,
                                                 diag::Bag& diags) {
    std::string name;
    if (!util::expect_arg_count(args, 1, "parus.aot_engine_enabled", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, name, "parus.aot_engine_enabled", span, diags)) return std::nullopt;
    return util::make_bool(aot_engine_enabled_impl(name));
}

std::optional<eval::Value> fn_llvm_lane_selected(const std::vector<eval::Value>& args,
                                                 const ast::Span& span,
                                                 diag::Bag& diags) {
    if (!util::expect_arg_count(args, 0, "parus.llvm_lane_selected", span, diags)) return std::nullopt;
    return util::make_int(LEI_PARUS_LLVM_LANE_SELECTED);
}

std::optional<eval::Value> fn_llvm_lane_supported(const std::vector<eval::Value>& args,
                                                  const ast::Span& span,
                                                  diag::Bag& diags) {
    int64_t lane = 0;
    if (!util::expect_arg_count(args, 1, "parus.llvm_lane_supported", span, diags)) return std::nullopt;
    if (!util::arg_as_int(args, 0, lane, "parus.llvm_lane_supported", span, diags)) return std::nullopt;
    return util::make_bool(llvm_lane_supported_impl(lane));
}

std::optional<eval::Value> fn_make_parusc_cmd(const std::vector<eval::Value>& args,
                                              const ast::Span& span,
                                              diag::Bag& diags) {
    std::vector<std::string> tail;
    if (!util::expect_arg_count(args, 1, "parus.make_parusc_cmd", span, diags)) return std::nullopt;
    if (!util::arg_as_string_array(args, 0, tail, "parus.make_parusc_cmd", span, diags)) return std::nullopt;
    eval::Value::Array out{};
    out.push_back(util::make_string(LEI_PARUS_TOOL_PARUSC));
    for (auto& arg : tail) out.push_back(util::make_string(std::move(arg)));
    return util::make_array(std::move(out));
}

std::optional<eval::Value> fn_make_link_cmd(const std::vector<eval::Value>& args,
                                            const ast::Span& span,
                                            diag::Bag& diags) {
    std::vector<std::string> tail;
    if (!util::expect_arg_count(args, 1, "parus.make_link_cmd", span, diags)) return std::nullopt;
    if (!util::arg_as_string_array(args, 0, tail, "parus.make_link_cmd", span, diags)) return std::nullopt;
    eval::Value::Array out{};
    out.push_back(util::make_string(LEI_PARUS_TOOL_PARUS_LLD));
    for (auto& arg : tail) out.push_back(util::make_string(std::move(arg)));
    return util::make_array(std::move(out));
}

std::optional<eval::Value> fn_normalize_bundle_name(const std::vector<eval::Value>& args,
                                                    const ast::Span& span,
                                                    diag::Bag& diags) {
    std::string name;
    if (!util::expect_arg_count(args, 1, "parus.normalize_bundle_name", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, name, "parus.normalize_bundle_name", span, diags)) return std::nullopt;
    return util::make_string(normalize_bundle_name_impl(std::move(name)));
}

eval::Value make_tools_object() {
    eval::Value::Object tools{};
    tools["parusc"] = util::make_string(LEI_PARUS_TOOL_PARUSC);
    tools["parusd"] = util::make_string(LEI_PARUS_TOOL_PARUSD);
    tools["parus_lld"] = util::make_string(LEI_PARUS_TOOL_PARUS_LLD);
    return util::make_object(std::move(tools));
}

eval::Value make_backends_object() {
    eval::Value::Array enabled{};
    if (LEI_PARUS_ENABLE_AOT_BACKEND != 0) enabled.push_back(util::make_string("aot"));
    if (LEI_PARUS_ENABLE_JIT_BACKEND != 0) enabled.push_back(util::make_string("jit"));
    if (LEI_PARUS_ENABLE_WASM_BACKEND != 0) enabled.push_back(util::make_string("wasm"));

    eval::Value::Object backends{};
    backends["supported"] = util::make_array(make_string_array({"aot", "jit", "wasm"}));
    backends["enabled"] = util::make_array(std::move(enabled));
    return util::make_object(std::move(backends));
}

eval::Value make_aot_object() {
    eval::Value::Array engines{};
    if (LEI_PARUS_ENABLE_AOT_BACKEND != 0 && LEI_PARUS_AOT_ENABLE_LLVM != 0) {
        engines.push_back(util::make_string("llvm"));
    }
    eval::Value::Object aot{};
    aot["engines"] = util::make_array(std::move(engines));
    return util::make_object(std::move(aot));
}

eval::Value make_llvm_object() {
    eval::Value::Object llvm{};
    llvm["lanes_supported"] = util::make_array({
        util::make_int(20),
        util::make_int(21),
    });
    llvm["lane_selected"] = util::make_int(LEI_PARUS_LLVM_LANE_SELECTED);
    return util::make_object(std::move(llvm));
}

eval::Value make_macro_budget_object() {
    eval::Value::Object default_aot{};
    default_aot["depth"] = util::make_int(LEI_PARUS_MACRO_BUDGET_AOT_DEPTH);
    default_aot["steps"] = util::make_int(LEI_PARUS_MACRO_BUDGET_AOT_STEPS);
    default_aot["output_tokens"] = util::make_int(LEI_PARUS_MACRO_BUDGET_AOT_OUTPUT_TOKENS);

    eval::Value::Object default_jit{};
    default_jit["depth"] = util::make_int(LEI_PARUS_MACRO_BUDGET_JIT_DEPTH);
    default_jit["steps"] = util::make_int(LEI_PARUS_MACRO_BUDGET_JIT_STEPS);
    default_jit["output_tokens"] = util::make_int(LEI_PARUS_MACRO_BUDGET_JIT_OUTPUT_TOKENS);

    eval::Value::Object hard_max{};
    hard_max["depth"] = util::make_int(LEI_PARUS_MACRO_BUDGET_HARD_MAX_DEPTH);
    hard_max["steps"] = util::make_int(LEI_PARUS_MACRO_BUDGET_HARD_MAX_STEPS);
    hard_max["output_tokens"] = util::make_int(LEI_PARUS_MACRO_BUDGET_HARD_MAX_OUTPUT_TOKENS);

    eval::Value::Object macro_budget{};
    macro_budget["default_aot"] = util::make_object(std::move(default_aot));
    macro_budget["default_jit"] = util::make_object(std::move(default_jit));
    macro_budget["hard_max"] = util::make_object(std::move(hard_max));
    return util::make_object(std::move(macro_budget));
}

eval::Value make_parus_dynamic_namespace() {
    static const std::vector<std::string> kKeys = {
        "version_major",
        "version_minor",
        "version_patch",
        "version_string",
        "tools",
        "backends",
        "aot",
        "llvm",
        "linker",
        "diag",
        "langs",
        "opt_levels",
        "macro_budget",
        "default_target",
        "host_target",
        "tool_path",
        "backend_enabled",
        "aot_engine_enabled",
        "llvm_lane_selected",
        "llvm_lane_supported",
        "make_parusc_cmd",
        "make_link_cmd",
        "normalize_bundle_name",
    };

    auto resolver = [](std::string_view key,
                       const ast::Span&,
                       diag::Bag&) -> std::optional<eval::Value> {
        if (key == "version_major") return util::make_int(LEI_PARUS_VERSION_MAJOR);
        if (key == "version_minor") return util::make_int(LEI_PARUS_VERSION_MINOR);
        if (key == "version_patch") return util::make_int(LEI_PARUS_VERSION_PATCH);
        if (key == "version_string") return util::make_string(parus_version_string());
        if (key == "tools") return make_tools_object();
        if (key == "backends") return make_backends_object();
        if (key == "aot") return make_aot_object();
        if (key == "llvm") return make_llvm_object();
        if (key == "linker") {
            eval::Value::Object linker{};
            linker["modes"] = util::make_array(make_string_array({"static", "shared", "parlib"}));
            return util::make_object(std::move(linker));
        }
        if (key == "diag") {
            eval::Value::Object diag{};
            diag["formats"] = util::make_array(make_string_array({"text", "json"}));
            return util::make_object(std::move(diag));
        }
        if (key == "langs") return util::make_array(make_string_array({"parus", "lei"}));
        if (key == "opt_levels") return util::make_array(make_string_array({"0", "1", "2", "3", "s", "z"}));
        if (key == "macro_budget") return make_macro_budget_object();

        if (key == "default_target") return util::make_native_function("parus.default_target", fn_default_target);
        if (key == "host_target") return util::make_native_function("parus.host_target", fn_host_target);
        if (key == "tool_path") return util::make_native_function("parus.tool_path", fn_tool_path);
        if (key == "backend_enabled") return util::make_native_function("parus.backend_enabled", fn_backend_enabled);
        if (key == "aot_engine_enabled") return util::make_native_function("parus.aot_engine_enabled", fn_aot_engine_enabled);
        if (key == "llvm_lane_selected") return util::make_native_function("parus.llvm_lane_selected", fn_llvm_lane_selected);
        if (key == "llvm_lane_supported") return util::make_native_function("parus.llvm_lane_supported", fn_llvm_lane_supported);
        if (key == "make_parusc_cmd") return util::make_native_function("parus.make_parusc_cmd", fn_make_parusc_cmd);
        if (key == "make_link_cmd") return util::make_native_function("parus.make_link_cmd", fn_make_link_cmd);
        if (key == "normalize_bundle_name") return util::make_native_function("parus.normalize_bundle_name", fn_normalize_bundle_name);

        return std::nullopt;
    };

    auto keys_provider = []() -> std::vector<std::string> { return kKeys; };
    return util::make_dynamic_object("parus", std::move(resolver), std::move(keys_provider));
}

} // namespace

void register_parus_helper_functions(eval::BuiltinRegistry& reg) {
    reg.register_value("parus", [] { return make_parus_dynamic_namespace(); });
}

} // namespace lei::builtins::detail
