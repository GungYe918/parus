#pragma once

#include <lei/diag/DiagCode.hpp>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace lei::cache {

struct ModuleHash {
    std::string path{};
    std::string hash{};
};

struct GraphCacheMeta {
    std::string schema_version{"v1"};
    std::string entry_file{};
    std::string entry_plan{"master"};
    std::string builtin_fingerprint{"lei-builtins-v1"};
    std::vector<ModuleHash> modules{};
    std::string graph_json_hash{};
    std::string ninja_hash{};
};

struct GraphCacheLoad {
    GraphCacheMeta meta{};
    std::string graph_json{};
    std::string ninja_text{};
};

std::string hash_text(std::string_view text);
std::string hash_file(const std::filesystem::path& path);

std::string make_cache_key(const std::string& entry_file, const std::string& entry_plan);
std::filesystem::path graph_cache_dir();
std::filesystem::path ninja_cache_dir();

bool validate_cache_meta(const GraphCacheMeta& meta, lei::diag::Bag& diags);
std::optional<GraphCacheLoad> load_graph_cache(const std::string& entry_file,
                                               const std::string& entry_plan,
                                               lei::diag::Bag& diags);
bool store_graph_cache(const std::string& entry_file,
                       const std::string& entry_plan,
                       const GraphCacheMeta& meta,
                       const std::string& graph_json,
                       const std::string& ninja_text,
                       lei::diag::Bag& diags);

} // namespace lei::cache
