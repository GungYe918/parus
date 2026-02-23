#include <lei/cache/GraphCache.hpp>

#include <lei/os/File.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>

namespace lei::cache {

namespace {

uint64_t fnv1a64(std::string_view s) {
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

std::string hex64(uint64_t v) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<size_t>(i)] = kHex[v & 0xfu];
        v >>= 4u;
    }
    return out;
}

std::string trim(std::string s) {
    auto is_space = [](unsigned char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::filesystem::path cache_root() {
    return std::filesystem::path(".lei-cache");
}

std::filesystem::path meta_path_for(std::string_view key) {
    return graph_cache_dir() / (std::string(key) + ".meta.json");
}

std::filesystem::path graph_json_path_for(std::string_view key) {
    return graph_cache_dir() / (std::string(key) + ".json");
}

std::filesystem::path ninja_path_for(std::string_view key) {
    return ninja_cache_dir() / (std::string(key) + ".ninja");
}

bool write_atomic_text(const std::filesystem::path& path, const std::string& text, lei::diag::Bag& diags) {
    std::error_code ec{};
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        diags.add(lei::diag::Code::B_NINJA_EMIT_FAILED,
                  path.string(),
                  1,
                  1,
                  "failed to create cache directory: " + path.parent_path().string());
        return false;
    }

    const auto tmp = path.string() + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary);
        if (!ofs) {
            diags.add(lei::diag::Code::B_NINJA_EMIT_FAILED,
                      path.string(),
                      1,
                      1,
                      "failed to open cache tmp file: " + tmp);
            return false;
        }
        ofs << text;
        if (!ofs.good()) {
            diags.add(lei::diag::Code::B_NINJA_EMIT_FAILED,
                      path.string(),
                      1,
                      1,
                      "failed to write cache tmp file: " + tmp);
            return false;
        }
    }

    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
        if (ec) {
            diags.add(lei::diag::Code::B_NINJA_EMIT_FAILED,
                      path.string(),
                      1,
                      1,
                      "failed to rename cache tmp file: " + tmp);
            return false;
        }
    }

    return true;
}

std::string encode_meta(const GraphCacheMeta& meta) {
    std::ostringstream oss;
    oss << "schema_version=" << meta.schema_version << "\n";
    oss << "entry_file=" << meta.entry_file << "\n";
    oss << "entry_plan=" << meta.entry_plan << "\n";
    oss << "builtin_fingerprint=" << meta.builtin_fingerprint << "\n";
    oss << "graph_json_hash=" << meta.graph_json_hash << "\n";
    oss << "ninja_hash=" << meta.ninja_hash << "\n";
    for (const auto& m : meta.modules) {
        oss << "module=" << m.path << "\t" << m.hash << "\n";
    }
    return oss.str();
}

std::optional<GraphCacheMeta> decode_meta(const std::string& text) {
    GraphCacheMeta meta{};
    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim(std::move(line));
        if (line.empty()) continue;
        const auto pos = line.find('=');
        if (pos == std::string::npos) continue;
        const std::string key = line.substr(0, pos);
        const std::string value = line.substr(pos + 1);

        if (key == "schema_version") {
            meta.schema_version = value;
        } else if (key == "entry_file") {
            meta.entry_file = value;
        } else if (key == "entry_plan") {
            meta.entry_plan = value;
        } else if (key == "builtin_fingerprint") {
            meta.builtin_fingerprint = value;
        } else if (key == "graph_json_hash") {
            meta.graph_json_hash = value;
        } else if (key == "ninja_hash") {
            meta.ninja_hash = value;
        } else if (key == "module") {
            const auto tab = value.find('\t');
            if (tab == std::string::npos) continue;
            ModuleHash m{};
            m.path = value.substr(0, tab);
            m.hash = value.substr(tab + 1);
            meta.modules.push_back(std::move(m));
        }
    }

    if (meta.entry_file.empty()) return std::nullopt;
    return meta;
}

} // namespace

std::string hash_text(std::string_view text) {
    return hex64(fnv1a64(text));
}

std::string hash_file(const std::filesystem::path& path) {
    const auto read = lei::os::read_text_file(path.string());
    if (!read.ok) return {};
    return hash_text(read.text);
}

std::string make_cache_key(const std::string& entry_file, const std::string& entry_plan) {
    return hex64(fnv1a64(entry_file + "::" + entry_plan));
}

std::filesystem::path graph_cache_dir() {
    return cache_root() / "graph";
}

std::filesystem::path ninja_cache_dir() {
    return cache_root() / "ninja";
}

bool validate_cache_meta(const GraphCacheMeta& meta, lei::diag::Bag& diags) {
    if (meta.schema_version != "v1") return false;
    if (meta.builtin_fingerprint != "lei-builtins-v1") return false;

    for (const auto& m : meta.modules) {
        const auto h = hash_file(m.path);
        if (h.empty() || h != m.hash) {
            return false;
        }
    }

    if (meta.ninja_hash.empty() || meta.graph_json_hash.empty()) {
        diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                  "<cache>",
                  1,
                  1,
                  "cache metadata missing ninja/graph hash");
        return false;
    }

    return true;
}

std::optional<GraphCacheLoad> load_graph_cache(const std::string& entry_file,
                                               const std::string& entry_plan,
                                               lei::diag::Bag& diags) {
    const auto key = make_cache_key(entry_file, entry_plan);
    const auto meta_file = meta_path_for(key);
    const auto graph_file = graph_json_path_for(key);
    const auto ninja_file = ninja_path_for(key);

    const auto read_meta = lei::os::read_text_file(meta_file.string());
    const auto read_graph = lei::os::read_text_file(graph_file.string());
    const auto read_ninja = lei::os::read_text_file(ninja_file.string());
    if (!read_meta.ok || !read_graph.ok || !read_ninja.ok) return std::nullopt;

    auto meta_opt = decode_meta(read_meta.text);
    if (!meta_opt.has_value()) return std::nullopt;
    if (meta_opt->entry_file != entry_file || meta_opt->entry_plan != entry_plan) return std::nullopt;
    if (!validate_cache_meta(*meta_opt, diags)) return std::nullopt;

    if (hash_text(read_graph.text) != meta_opt->graph_json_hash) return std::nullopt;
    if (hash_text(read_ninja.text) != meta_opt->ninja_hash) return std::nullopt;

    GraphCacheLoad out{};
    out.meta = std::move(*meta_opt);
    out.graph_json = read_graph.text;
    out.ninja_text = read_ninja.text;
    return out;
}

bool store_graph_cache(const std::string& entry_file,
                       const std::string& entry_plan,
                       const GraphCacheMeta& meta,
                       const std::string& graph_json,
                       const std::string& ninja_text,
                       lei::diag::Bag& diags) {
    const auto key = make_cache_key(entry_file, entry_plan);
    auto write_meta = meta;
    write_meta.entry_file = entry_file;
    write_meta.entry_plan = entry_plan;
    write_meta.graph_json_hash = hash_text(graph_json);
    write_meta.ninja_hash = hash_text(ninja_text);

    const auto meta_file = meta_path_for(key);
    const auto graph_file = graph_json_path_for(key);
    const auto ninja_file = ninja_path_for(key);

    if (!write_atomic_text(graph_file, graph_json, diags)) return false;
    if (!write_atomic_text(ninja_file, ninja_text, diags)) return false;
    if (!write_atomic_text(meta_file, encode_meta(write_meta), diags)) return false;
    return true;
}

} // namespace lei::cache
