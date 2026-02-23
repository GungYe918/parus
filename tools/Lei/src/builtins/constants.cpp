#include <lei/builtins/ValueUtil.hpp>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace lei::builtins::detail {

namespace {

#ifndef LEI_PARUS_LLVM_USE_TOOLCHAIN_DEFAULT
#define LEI_PARUS_LLVM_USE_TOOLCHAIN_DEFAULT 0
#endif

#ifndef LEI_PARUS_LLVM_REQUIRE_TOOLCHAIN_DEFAULT
#define LEI_PARUS_LLVM_REQUIRE_TOOLCHAIN_DEFAULT 0
#endif

std::string detect_os() {
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

std::string detect_arch() {
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

std::string detect_family() {
#if defined(_WIN32)
    return "windows";
#else
    return "unix";
#endif
}

std::string detect_host_triple() {
    const auto arch = detect_arch();
    const auto os = detect_os();
    if (os == "macos") return arch + "-apple-darwin";
    if (os == "linux") return arch + "-unknown-linux-gnu";
    if (os == "windows") return arch + "-pc-windows-msvc";
    return arch + "-unknown-unknown";
}

bool path_has_executable(std::string_view base, std::string_view name) {
    namespace fs = std::filesystem;
    fs::path p(base);
    p /= std::string(name);
    if (fs::exists(p)) return true;
#ifdef _WIN32
    if (fs::exists(p.string() + ".exe")) return true;
#endif
    return false;
}

bool command_exists(std::string_view name) {
    const char* path_env = std::getenv("PATH");
    if (!path_env) return false;
    std::string path_list(path_env);
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
        if (!part.empty() && path_has_executable(part, name)) return true;
        if (next == std::string::npos) break;
        pos = next + 1;
    }
    return false;
}

eval::Value make_lei_constants() {
    eval::Value::Object obj{};
    obj["version"] = util::make_string("0.1.0");
    obj["engine_name"] = util::make_string("LEI");
    obj["engine_semver"] = util::make_string("0.1.0");
    obj["entry_plan_default"] = util::make_string("master");
    obj["view_formats"] = util::make_array({
        util::make_string("json"),
        util::make_string("text"),
        util::make_string("dot"),
    });
    obj["reserved_plan_names"] = util::make_array({
        util::make_string("bundle"),
        util::make_string("master"),
        util::make_string("task"),
        util::make_string("codegen"),
    });
    obj["syntax_generation"] = util::make_string("v0.4");
    return util::make_object(std::move(obj));
}

eval::Value make_host_dynamic() {
    static const std::vector<std::string> kKeys = {
        "os",
        "arch",
        "family",
        "exe_suffix",
        "shared_lib_suffix",
        "static_lib_suffix",
        "path_sep",
        "path_list_sep",
        "case_sensitive_fs",
        "endian",
        "cpu_count",
        "triple",
    };

    auto resolver = [](std::string_view key,
                       const ast::Span&,
                       diag::Bag&) -> std::optional<eval::Value> {
        if (key == "os") return util::make_string(detect_os());
        if (key == "arch") return util::make_string(detect_arch());
        if (key == "family") return util::make_string(detect_family());
#ifdef _WIN32
        if (key == "exe_suffix") return util::make_string(".exe");
        if (key == "shared_lib_suffix") return util::make_string(".dll");
        if (key == "static_lib_suffix") return util::make_string(".lib");
        if (key == "path_sep") return util::make_string("\\");
        if (key == "path_list_sep") return util::make_string(";");
        if (key == "case_sensitive_fs") return util::make_bool(false);
#else
        if (key == "exe_suffix") return util::make_string("");
#if defined(__APPLE__) && defined(__MACH__)
        if (key == "shared_lib_suffix") return util::make_string(".dylib");
#else
        if (key == "shared_lib_suffix") return util::make_string(".so");
#endif
        if (key == "static_lib_suffix") return util::make_string(".a");
        if (key == "path_sep") return util::make_string("/");
        if (key == "path_list_sep") return util::make_string(":");
        if (key == "case_sensitive_fs") return util::make_bool(true);
#endif
#if defined(__BYTE_ORDER__) && (__BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
        if (key == "endian") return util::make_string("big");
#else
        if (key == "endian") return util::make_string("little");
#endif
        if (key == "cpu_count") {
            const auto n = std::thread::hardware_concurrency();
            return util::make_int(static_cast<int64_t>(n == 0 ? 1 : n));
        }
        if (key == "triple") return util::make_string(detect_host_triple());
        return std::nullopt;
    };

    auto keys_provider = []() -> std::vector<std::string> { return kKeys; };
    return util::make_dynamic_object("host", std::move(resolver), std::move(keys_provider));
}

eval::Value make_toolchain_constants() {
    eval::Value::Object obj{};
    obj["generator_default"] = util::make_string(command_exists("ninja") ? "Ninja" : "Unix Makefiles");
    obj["llvm_use_toolchain_default"] = util::make_bool(LEI_PARUS_LLVM_USE_TOOLCHAIN_DEFAULT != 0);
    obj["llvm_require_toolchain_default"] = util::make_bool(LEI_PARUS_LLVM_REQUIRE_TOOLCHAIN_DEFAULT != 0);
    return util::make_object(std::move(obj));
}

} // namespace

void register_constant_values(eval::BuiltinRegistry& reg) {
    reg.register_value("lei", [] { return make_lei_constants(); });
    reg.register_value("host", [] { return make_host_dynamic(); });
    reg.register_value("toolchain", [] { return make_toolchain_constants(); });
}

} // namespace lei::builtins::detail
