#include <lei/builtins/ValueUtil.hpp>

#include <lei/os/File.hpp>

#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace lei::builtins::detail {

namespace {

std::string to_slash(std::string s) {
    for (auto& c : s) {
        if (c == '\\') c = '/';
    }
    return s;
}

std::string to_native_path(std::string s) {
#ifdef _WIN32
    for (auto& c : s) {
        if (c == '/') c = '\\';
    }
#endif
    return s;
}

std::vector<std::string> split_segments(std::string_view p) {
    std::vector<std::string> out{};
    size_t pos = 0;
    while (pos <= p.size()) {
        const size_t next = p.find('/', pos);
        const std::string_view part = (next == std::string_view::npos)
            ? p.substr(pos)
            : p.substr(pos, next - pos);
        if (!part.empty() && part != ".") out.emplace_back(part);
        if (next == std::string_view::npos) break;
        pos = next + 1;
    }
    return out;
}

bool has_wild(std::string_view seg) {
    return seg == "**" || seg.find('*') != std::string_view::npos || seg.find('?') != std::string_view::npos;
}

bool match_segment_pattern(std::string_view pattern, std::string_view text) {
    size_t p = 0;
    size_t t = 0;
    size_t star = std::string_view::npos;
    size_t match = 0;
    while (t < text.size()) {
        if (p < pattern.size() && (pattern[p] == '?' || pattern[p] == text[t])) {
            ++p;
            ++t;
            continue;
        }
        if (p < pattern.size() && pattern[p] == '*') {
            star = p++;
            match = t;
            continue;
        }
        if (star != std::string_view::npos) {
            p = star + 1;
            t = ++match;
            continue;
        }
        return false;
    }
    while (p < pattern.size() && pattern[p] == '*') ++p;
    return p == pattern.size();
}

bool match_path_segments(const std::vector<std::string>& pats,
                         size_t pi,
                         const std::vector<std::string>& segs,
                         size_t si) {
    if (pi == pats.size()) return si == segs.size();
    if (pats[pi] == "**") {
        for (size_t k = si; k <= segs.size(); ++k) {
            if (match_path_segments(pats, pi + 1, segs, k)) return true;
        }
        return false;
    }
    if (si >= segs.size()) return false;
    if (!match_segment_pattern(pats[pi], segs[si])) return false;
    return match_path_segments(pats, pi + 1, segs, si + 1);
}

std::optional<std::string> make_rel_slash(const std::filesystem::path& base,
                                          const std::filesystem::path& p) {
    namespace fs = std::filesystem;
    std::error_code ec{};
    fs::path rel = fs::relative(p, base, ec);
    if (ec) rel = p.lexically_relative(base);
    if (rel.empty()) rel = p;
    return to_slash(rel.lexically_normal().string());
}

std::set<std::string> glob_collect(const std::filesystem::path& base_root,
                                   const std::vector<std::string>& patterns,
                                   diag::Bag& diags,
                                   const ast::Span& span) {
    namespace fs = std::filesystem;
    std::set<std::string> out{};

    for (const auto& raw : patterns) {
        const std::string pat = to_slash(raw);
        const auto pat_segs = split_segments(pat);
        if (pat_segs.empty()) continue;

        size_t pivot = 0;
        while (pivot < pat_segs.size() && !has_wild(pat_segs[pivot])) ++pivot;

        fs::path search_root = base_root;
        for (size_t i = 0; i < pivot; ++i) search_root /= pat_segs[i];

        if (pivot == pat_segs.size()) {
            const fs::path candidate = base_root / fs::path(raw);
            std::error_code ec{};
            if (fs::exists(candidate, ec) && fs::is_regular_file(candidate, ec)) {
                auto rel = make_rel_slash(base_root, candidate);
                if (rel.has_value()) out.insert(*rel);
            }
            continue;
        }

        std::error_code ec{};
        if (!fs::exists(search_root, ec)) continue;

        auto try_add = [&](const fs::path& p) {
            if (!fs::is_regular_file(p, ec)) return;
            auto rel = make_rel_slash(base_root, p);
            if (!rel.has_value()) return;
            const auto segs = split_segments(*rel);
            if (match_path_segments(pat_segs, 0, segs, 0)) out.insert(*rel);
        };

        try_add(search_root);
        for (fs::recursive_directory_iterator it(search_root, ec), end; !ec && it != end; it.increment(ec)) {
            if (ec) break;
            try_add(it->path());
        }
        if (ec) {
            diags.add(diag::Code::L_TYPE_MISMATCH,
                      span.file,
                      span.line,
                      span.column,
                      "fs.glob traversal failed: " + search_root.string());
            return {};
        }
    }

    return out;
}

std::string hex_u32(uint32_t v) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(8, '0');
    for (int i = 7; i >= 0; --i) {
        out[static_cast<size_t>(i)] = kHex[v & 0xFu];
        v >>= 4u;
    }
    return out;
}

std::string sha256_bytes(const std::vector<uint8_t>& input) {
    static constexpr std::array<uint32_t, 64> k = {
        0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u, 0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
        0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u, 0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
        0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu, 0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
        0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u, 0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
        0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u, 0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
        0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u, 0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
        0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u, 0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
        0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u, 0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
    };

    auto rotr = [](uint32_t x, uint32_t n) -> uint32_t { return (x >> n) | (x << (32u - n)); };
    auto ch = [](uint32_t x, uint32_t y, uint32_t z) -> uint32_t { return (x & y) ^ (~x & z); };
    auto maj = [](uint32_t x, uint32_t y, uint32_t z) -> uint32_t { return (x & y) ^ (x & z) ^ (y & z); };
    auto bsig0 = [&](uint32_t x) -> uint32_t { return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22); };
    auto bsig1 = [&](uint32_t x) -> uint32_t { return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25); };
    auto ssig0 = [&](uint32_t x) -> uint32_t { return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3); };
    auto ssig1 = [&](uint32_t x) -> uint32_t { return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10); };

    std::vector<uint8_t> msg = input;
    const uint64_t bit_len = static_cast<uint64_t>(msg.size()) * 8u;
    msg.push_back(0x80u);
    while ((msg.size() % 64u) != 56u) msg.push_back(0u);
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<uint8_t>((bit_len >> (8u * i)) & 0xFFu));

    uint32_t h0 = 0x6a09e667u;
    uint32_t h1 = 0xbb67ae85u;
    uint32_t h2 = 0x3c6ef372u;
    uint32_t h3 = 0xa54ff53au;
    uint32_t h4 = 0x510e527fu;
    uint32_t h5 = 0x9b05688cu;
    uint32_t h6 = 0x1f83d9abu;
    uint32_t h7 = 0x5be0cd19u;

    for (size_t chunk = 0; chunk < msg.size(); chunk += 64u) {
        std::array<uint32_t, 64> w{};
        for (size_t i = 0; i < 16; ++i) {
            const size_t p = chunk + i * 4u;
            w[i] = (static_cast<uint32_t>(msg[p]) << 24u)
                 | (static_cast<uint32_t>(msg[p + 1]) << 16u)
                 | (static_cast<uint32_t>(msg[p + 2]) << 8u)
                 | static_cast<uint32_t>(msg[p + 3]);
        }
        for (size_t i = 16; i < 64; ++i) {
            w[i] = ssig1(w[i - 2]) + w[i - 7] + ssig0(w[i - 15]) + w[i - 16];
        }

        uint32_t a = h0;
        uint32_t b = h1;
        uint32_t c = h2;
        uint32_t d = h3;
        uint32_t e = h4;
        uint32_t f = h5;
        uint32_t g = h6;
        uint32_t h = h7;

        for (size_t i = 0; i < 64; ++i) {
            const uint32_t t1 = h + bsig1(e) + ch(e, f, g) + k[i] + w[i];
            const uint32_t t2 = bsig0(a) + maj(a, b, c);
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }

        h0 += a; h1 += b; h2 += c; h3 += d;
        h4 += e; h5 += f; h6 += g; h7 += h;
    }

    return hex_u32(h0) + hex_u32(h1) + hex_u32(h2) + hex_u32(h3)
         + hex_u32(h4) + hex_u32(h5) + hex_u32(h6) + hex_u32(h7);
}

std::optional<eval::Value> path_join(const std::vector<eval::Value>& args,
                                     const ast::Span& span,
                                     diag::Bag& diags) {
    std::vector<std::string> parts;
    if (!util::expect_arg_count(args, 1, "path.join", span, diags)) return std::nullopt;
    if (!util::arg_as_string_array(args, 0, parts, "path.join", span, diags)) return std::nullopt;
    std::filesystem::path out;
    for (const auto& p : parts) out /= std::filesystem::path(p);
    return util::make_string(out.lexically_normal().string());
}

std::optional<eval::Value> path_normalize(const std::vector<eval::Value>& args,
                                          const ast::Span& span,
                                          diag::Bag& diags) {
    std::string p;
    if (!util::expect_arg_count(args, 1, "path.normalize", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, p, "path.normalize", span, diags)) return std::nullopt;
    return util::make_string(lei::os::normalize_path(p));
}

std::optional<eval::Value> path_dirname(const std::vector<eval::Value>& args,
                                        const ast::Span& span,
                                        diag::Bag& diags) {
    std::string p;
    if (!util::expect_arg_count(args, 1, "path.dirname", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, p, "path.dirname", span, diags)) return std::nullopt;
    return util::make_string(std::filesystem::path(p).parent_path().string());
}

std::optional<eval::Value> path_basename(const std::vector<eval::Value>& args,
                                         const ast::Span& span,
                                         diag::Bag& diags) {
    std::string p;
    if (!util::expect_arg_count(args, 1, "path.basename", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, p, "path.basename", span, diags)) return std::nullopt;
    return util::make_string(std::filesystem::path(p).filename().string());
}

std::optional<eval::Value> path_stem(const std::vector<eval::Value>& args,
                                     const ast::Span& span,
                                     diag::Bag& diags) {
    std::string p;
    if (!util::expect_arg_count(args, 1, "path.stem", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, p, "path.stem", span, diags)) return std::nullopt;
    return util::make_string(std::filesystem::path(p).stem().string());
}

std::optional<eval::Value> path_ext(const std::vector<eval::Value>& args,
                                    const ast::Span& span,
                                    diag::Bag& diags) {
    std::string p;
    if (!util::expect_arg_count(args, 1, "path.ext", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, p, "path.ext", span, diags)) return std::nullopt;
    return util::make_string(std::filesystem::path(p).extension().string());
}

std::optional<eval::Value> path_is_abs(const std::vector<eval::Value>& args,
                                       const ast::Span& span,
                                       diag::Bag& diags) {
    std::string p;
    if (!util::expect_arg_count(args, 1, "path.is_abs", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, p, "path.is_abs", span, diags)) return std::nullopt;
    return util::make_bool(std::filesystem::path(p).is_absolute());
}

std::optional<eval::Value> path_rel(const std::vector<eval::Value>& args,
                                    const ast::Span& span,
                                    diag::Bag& diags) {
    std::string base;
    std::string target;
    if (!util::expect_arg_count(args, 2, "path.rel", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, base, "path.rel", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 1, target, "path.rel", span, diags)) return std::nullopt;
    std::error_code ec{};
    auto rel = std::filesystem::relative(std::filesystem::path(target), std::filesystem::path(base), ec);
    if (ec) rel = std::filesystem::path(target).lexically_relative(std::filesystem::path(base));
    return util::make_string(rel.lexically_normal().string());
}

std::optional<eval::Value> path_to_slash(const std::vector<eval::Value>& args,
                                         const ast::Span& span,
                                         diag::Bag& diags) {
    std::string p;
    if (!util::expect_arg_count(args, 1, "path.to_slash", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, p, "path.to_slash", span, diags)) return std::nullopt;
    return util::make_string(to_slash(std::move(p)));
}

std::optional<eval::Value> path_to_native(const std::vector<eval::Value>& args,
                                          const ast::Span& span,
                                          diag::Bag& diags) {
    std::string p;
    if (!util::expect_arg_count(args, 1, "path.to_native", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, p, "path.to_native", span, diags)) return std::nullopt;
    return util::make_string(to_native_path(std::move(p)));
}

std::optional<eval::Value> fs_exists(const std::vector<eval::Value>& args,
                                     const ast::Span& span,
                                     diag::Bag& diags) {
    std::string p;
    if (!util::expect_arg_count(args, 1, "fs.exists", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, p, "fs.exists", span, diags)) return std::nullopt;
    std::error_code ec{};
    return util::make_bool(std::filesystem::exists(std::filesystem::path(p), ec));
}

std::optional<eval::Value> fs_is_file(const std::vector<eval::Value>& args,
                                      const ast::Span& span,
                                      diag::Bag& diags) {
    std::string p;
    if (!util::expect_arg_count(args, 1, "fs.is_file", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, p, "fs.is_file", span, diags)) return std::nullopt;
    std::error_code ec{};
    return util::make_bool(std::filesystem::is_regular_file(std::filesystem::path(p), ec));
}

std::optional<eval::Value> fs_is_dir(const std::vector<eval::Value>& args,
                                     const ast::Span& span,
                                     diag::Bag& diags) {
    std::string p;
    if (!util::expect_arg_count(args, 1, "fs.is_dir", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, p, "fs.is_dir", span, diags)) return std::nullopt;
    std::error_code ec{};
    return util::make_bool(std::filesystem::is_directory(std::filesystem::path(p), ec));
}

std::optional<eval::Value> fs_glob(const std::vector<eval::Value>& args,
                                   const ast::Span& span,
                                   diag::Bag& diags) {
    std::vector<std::string> patterns;
    if (!util::expect_arg_count(args, 1, "fs.glob", span, diags)) return std::nullopt;
    if (!util::arg_as_string_array(args, 0, patterns, "fs.glob", span, diags)) return std::nullopt;

    const auto matched = glob_collect(std::filesystem::current_path(), patterns, diags, span);
    eval::Value::Array out{};
    out.reserve(matched.size());
    for (const auto& p : matched) out.push_back(util::make_string(p));
    return util::make_array(std::move(out));
}

std::optional<eval::Value> fs_glob_under(const std::vector<eval::Value>& args,
                                         const ast::Span& span,
                                         diag::Bag& diags) {
    std::string root;
    std::vector<std::string> patterns;
    if (!util::expect_arg_count(args, 2, "fs.glob_under", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, root, "fs.glob_under", span, diags)) return std::nullopt;
    if (!util::arg_as_string_array(args, 1, patterns, "fs.glob_under", span, diags)) return std::nullopt;

    const auto matched = glob_collect(std::filesystem::path(root), patterns, diags, span);
    eval::Value::Array out{};
    out.reserve(matched.size());
    for (const auto& p : matched) out.push_back(util::make_string(p));
    return util::make_array(std::move(out));
}

std::optional<eval::Value> fs_read_text(const std::vector<eval::Value>& args,
                                        const ast::Span& span,
                                        diag::Bag& diags) {
    std::string p;
    if (!util::expect_arg_count(args, 1, "fs.read_text", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, p, "fs.read_text", span, diags)) return std::nullopt;
    const auto r = lei::os::read_text_file(p);
    if (!r.ok) {
        diags.add(diag::Code::L_TYPE_MISMATCH,
                  span.file,
                  span.line,
                  span.column,
                  "fs.read_text failed: " + r.err);
        return std::nullopt;
    }
    return util::make_string(r.text);
}

std::optional<eval::Value> fs_read_lines(const std::vector<eval::Value>& args,
                                         const ast::Span& span,
                                         diag::Bag& diags) {
    auto txt = fs_read_text(args, span, diags);
    if (!txt) return std::nullopt;
    const auto& s = std::get<std::string>(txt->data);
    eval::Value::Array lines{};
    size_t pos = 0;
    while (pos <= s.size()) {
        const size_t next = s.find('\n', pos);
        if (next == std::string::npos) {
            lines.push_back(util::make_string(s.substr(pos)));
            break;
        }
        lines.push_back(util::make_string(s.substr(pos, next - pos)));
        pos = next + 1;
    }
    return util::make_array(std::move(lines));
}

std::optional<eval::Value> fs_sha256(const std::vector<eval::Value>& args,
                                     const ast::Span& span,
                                     diag::Bag& diags) {
    std::string p;
    if (!util::expect_arg_count(args, 1, "fs.sha256", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, p, "fs.sha256", span, diags)) return std::nullopt;

    std::ifstream ifs(p, std::ios::binary);
    if (!ifs) {
        diags.add(diag::Code::L_TYPE_MISMATCH,
                  span.file,
                  span.line,
                  span.column,
                  "fs.sha256 cannot open file: " + p);
        return std::nullopt;
    }
    std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return util::make_string(sha256_bytes(bytes));
}

std::optional<eval::Value> fs_file_size(const std::vector<eval::Value>& args,
                                        const ast::Span& span,
                                        diag::Bag& diags) {
    std::string p;
    if (!util::expect_arg_count(args, 1, "fs.file_size", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, p, "fs.file_size", span, diags)) return std::nullopt;
    std::error_code ec{};
    const auto n = std::filesystem::file_size(std::filesystem::path(p), ec);
    if (ec) {
        diags.add(diag::Code::L_TYPE_MISMATCH,
                  span.file,
                  span.line,
                  span.column,
                  "fs.file_size failed: " + p);
        return std::nullopt;
    }
    return util::make_int(static_cast<int64_t>(n));
}

eval::Value make_path_namespace() {
    eval::Value::Object path{};
    path["join"] = util::make_native_function("path.join", path_join);
    path["normalize"] = util::make_native_function("path.normalize", path_normalize);
    path["dirname"] = util::make_native_function("path.dirname", path_dirname);
    path["basename"] = util::make_native_function("path.basename", path_basename);
    path["stem"] = util::make_native_function("path.stem", path_stem);
    path["ext"] = util::make_native_function("path.ext", path_ext);
    path["is_abs"] = util::make_native_function("path.is_abs", path_is_abs);
    path["rel"] = util::make_native_function("path.rel", path_rel);
    path["to_slash"] = util::make_native_function("path.to_slash", path_to_slash);
    path["to_native"] = util::make_native_function("path.to_native", path_to_native);
    return util::make_object(std::move(path));
}

eval::Value make_fs_namespace() {
    eval::Value::Object fs{};
    fs["exists"] = util::make_native_function("fs.exists", fs_exists);
    fs["is_file"] = util::make_native_function("fs.is_file", fs_is_file);
    fs["is_dir"] = util::make_native_function("fs.is_dir", fs_is_dir);
    fs["glob"] = util::make_native_function("fs.glob", fs_glob);
    fs["glob_under"] = util::make_native_function("fs.glob_under", fs_glob_under);
    fs["read_text"] = util::make_native_function("fs.read_text", fs_read_text);
    fs["read_lines"] = util::make_native_function("fs.read_lines", fs_read_lines);
    fs["sha256"] = util::make_native_function("fs.sha256", fs_sha256);
    fs["file_size"] = util::make_native_function("fs.file_size", fs_file_size);
    return util::make_object(std::move(fs));
}

} // namespace

void register_path_fs_functions(eval::BuiltinRegistry& reg) {
    reg.register_value("path", [] { return make_path_namespace(); });
    reg.register_value("fs", [] { return make_fs_namespace(); });
}

} // namespace lei::builtins::detail

