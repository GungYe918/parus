#include <parus_tool/toolchain/Resolver.hpp>

#include <cstdlib>
#include <filesystem>
#include <vector>

namespace parus_tool::toolchain {

namespace {

std::string getenv_string(const char* key) {
    if (key == nullptr) return {};
    const char* p = std::getenv(key);
    if (p == nullptr) return {};
    return std::string(p);
}

bool is_executable_file(const std::filesystem::path& p) {
    std::error_code ec{};
    return std::filesystem::exists(p, ec) && std::filesystem::is_regular_file(p, ec);
}

void append_candidate(std::vector<std::filesystem::path>& out, const std::filesystem::path& p) {
    if (!p.empty()) out.push_back(p);
}

} // namespace

std::string resolve_tool(std::string_view tool_name, const ResolveOptions& opt) {
    namespace fs = std::filesystem;

    std::vector<fs::path> candidates{};
    if (!opt.toolchain_root.empty()) {
        append_candidate(candidates, fs::path(opt.toolchain_root) / "bin" / std::string(tool_name));
    }

    if (opt.argv0 != nullptr) {
        std::error_code ec{};
        fs::path argv_path(opt.argv0);
        fs::path resolved = fs::weakly_canonical(argv_path, ec);
        if (ec || resolved.empty()) {
            ec.clear();
            resolved = argv_path;
        }
        const fs::path exe_dir = resolved.parent_path();
        append_candidate(candidates, exe_dir / std::string(tool_name));

        // In-tree LEI fallback for build dir layout.
        if (tool_name == "lei") {
            append_candidate(candidates, exe_dir.parent_path().parent_path() / "tools" / "Lei" / "lei");
        }
    }

    const std::string env_root = getenv_string("PARUS_TOOLCHAIN_ROOT");
    if (!env_root.empty()) {
        append_candidate(candidates, fs::path(env_root) / "bin" / std::string(tool_name));
    }

    for (const auto& c : candidates) {
        if (is_executable_file(c)) return c.string();
    }

    return std::string(tool_name);
}

} // namespace parus_tool::toolchain
