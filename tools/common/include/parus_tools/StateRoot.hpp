#pragma once

#include <filesystem>
#include <optional>
#include <system_error>

namespace parus_tools::paths {

inline std::filesystem::path normalize_anchor(std::filesystem::path anchor) {
    std::error_code ec{};
    if (anchor.empty()) anchor = std::filesystem::current_path(ec);
    if (ec || anchor.empty()) return std::filesystem::path(".");
    if (!std::filesystem::exists(anchor, ec)) {
        anchor = anchor.parent_path();
    } else if (!std::filesystem::is_directory(anchor, ec)) {
        anchor = anchor.parent_path();
    }
    if (anchor.empty()) return std::filesystem::path(".");
    return anchor.lexically_normal();
}

inline std::optional<std::filesystem::path> find_project_root(std::filesystem::path start) {
    std::error_code ec{};
    start = normalize_anchor(std::move(start));
    for (std::filesystem::path cur = start; !cur.empty(); cur = cur.parent_path()) {
        if (std::filesystem::exists(cur / "config.lei", ec) ||
            std::filesystem::exists(cur / ".git", ec)) {
            return cur.lexically_normal();
        }
        const auto parent = cur.parent_path();
        if (parent == cur) break;
    }
    return std::nullopt;
}

inline std::filesystem::path project_root_or_anchor(std::filesystem::path anchor) {
    if (auto root = find_project_root(anchor); root.has_value()) {
        return root->lexically_normal();
    }
    return normalize_anchor(std::move(anchor));
}

inline std::filesystem::path state_root(std::filesystem::path anchor) {
    return project_root_or_anchor(std::move(anchor)) / "target" / "parus";
}

inline std::filesystem::path cache_root(std::filesystem::path anchor) {
    return state_root(std::move(anchor)) / "cache";
}

inline std::filesystem::path graph_cache_dir(std::filesystem::path anchor) {
    return cache_root(std::move(anchor)) / "graph";
}

inline std::filesystem::path ninja_cache_dir(std::filesystem::path anchor) {
    return cache_root(std::move(anchor)) / "ninja";
}

inline std::filesystem::path index_dir(std::filesystem::path anchor) {
    return state_root(std::move(anchor)) / "index";
}

inline std::filesystem::path out_root(std::filesystem::path anchor) {
    return state_root(std::move(anchor)) / "out";
}

inline std::filesystem::path out_obj_dir(std::filesystem::path anchor) {
    return out_root(std::move(anchor)) / "obj";
}

inline std::filesystem::path out_lib_dir(std::filesystem::path anchor) {
    return out_root(std::move(anchor)) / "lib";
}

inline std::filesystem::path out_bin_dir(std::filesystem::path anchor) {
    return out_root(std::move(anchor)) / "bin";
}

inline std::filesystem::path out_task_dir(std::filesystem::path anchor) {
    return out_root(std::move(anchor)) / "task";
}

inline std::filesystem::path out_codegen_dir(std::filesystem::path anchor) {
    return out_root(std::move(anchor)) / "codegen";
}

} // namespace parus_tools::paths
