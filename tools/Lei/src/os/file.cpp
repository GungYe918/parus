#include <lei/os/File.hpp>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace lei::os {

namespace {

void normalize_newlines_inplace(std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (std::size_t i = 0; i < s.size(); ++i) {
        const char c = s[i];
        if (c == '\r') continue;
        out.push_back(c);
    }
    s.swap(out);
}

} // namespace

ReadTextResult read_text_file(std::string_view path) {
    ReadTextResult r{};

    std::FILE* fp = std::fopen(std::string(path).c_str(), "rb");
    if (!fp) {
        r.err = std::string("cannot open file: ") + std::strerror(errno);
        return r;
    }

    std::fseek(fp, 0, SEEK_END);
    const long sz = std::ftell(fp);
    std::fseek(fp, 0, SEEK_SET);
    if (sz < 0) {
        std::fclose(fp);
        r.err = "cannot read file size";
        return r;
    }

    r.text.resize(static_cast<std::size_t>(sz));
    const std::size_t n = std::fread(r.text.data(), 1, r.text.size(), fp);
    std::fclose(fp);

    if (n != r.text.size()) {
        r.text.clear();
        r.err = "short read";
        return r;
    }

    normalize_newlines_inplace(r.text);
    r.ok = true;
    return r;
}

std::string normalize_path(std::string_view path) {
    namespace fs = std::filesystem;
    std::error_code ec{};
    const fs::path p(path);
    const fs::path c = fs::weakly_canonical(p, ec);
    if (!ec) return c.string();
    return p.lexically_normal().string();
}

std::string resolve_relative_path(std::string_view base_path, std::string_view rel_path) {
    namespace fs = std::filesystem;
    fs::path rel(rel_path);
    if (rel.is_absolute()) return normalize_path(rel.string());

    const fs::path base(base_path);
    const fs::path anchor = base.has_filename() ? base.parent_path() : base;
    return normalize_path((anchor / rel).string());
}

} // namespace lei::os
