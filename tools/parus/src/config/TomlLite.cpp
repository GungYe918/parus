#include <parus_tool/config/TomlLite.hpp>

#include <parus_tool/config/Config.hpp>

#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace parus_tool::config::toml_lite {

namespace {

std::string trim(std::string s) {
    const auto is_space = [](unsigned char c) {
        return c == ' ' || c == '\t' || c == '\n' || c == '\r';
    };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

std::string strip_comment(std::string_view line) {
    std::string out{};
    bool in_string = false;
    bool escaped = false;
    for (char c : line) {
        if (!in_string && c == '#') break;
        out.push_back(c);
        if (!in_string) {
            if (c == '"') in_string = true;
            continue;
        }
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        if (c == '"') in_string = false;
    }
    return out;
}

bool parse_string_literal(std::string_view text, std::string& out, std::string& err) {
    if (text.size() < 2 || text.front() != '"' || text.back() != '"') {
        err = "invalid string literal";
        return false;
    }
    out.clear();
    out.reserve(text.size() - 2);
    bool escaped = false;
    for (size_t i = 1; i + 1 < text.size(); ++i) {
        const char c = text[i];
        if (escaped) {
            switch (c) {
                case 'n': out.push_back('\n'); break;
                case 'r': out.push_back('\r'); break;
                case 't': out.push_back('\t'); break;
                case '"': out.push_back('"'); break;
                case '\\': out.push_back('\\'); break;
                default: out.push_back(c); break;
            }
            escaped = false;
            continue;
        }
        if (c == '\\') {
            escaped = true;
            continue;
        }
        out.push_back(c);
    }
    if (escaped) {
        err = "unterminated escape in string literal";
        return false;
    }
    return true;
}

bool parse_int_literal(std::string_view text, int64_t& out) {
    if (text.empty()) return false;
    size_t i = 0;
    if (text[0] == '+' || text[0] == '-') i = 1;
    if (i >= text.size()) return false;
    for (; i < text.size(); ++i) {
        if (!std::isdigit(static_cast<unsigned char>(text[i]))) return false;
    }
    try {
        out = std::stoll(std::string(text));
        return true;
    } catch (...) {
        return false;
    }
}

bool split_array_items(std::string_view text, std::vector<std::string>& out, std::string& err) {
    out.clear();
    std::string cur{};
    bool in_string = false;
    bool escaped = false;
    for (char c : text) {
        if (in_string) {
            cur.push_back(c);
            if (escaped) {
                escaped = false;
                continue;
            }
            if (c == '\\') {
                escaped = true;
                continue;
            }
            if (c == '"') in_string = false;
            continue;
        }
        if (c == '"') {
            in_string = true;
            cur.push_back(c);
            continue;
        }
        if (c == ',') {
            cur = trim(std::move(cur));
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
            continue;
        }
        cur.push_back(c);
    }
    if (in_string) {
        err = "unterminated string in array";
        return false;
    }
    cur = trim(std::move(cur));
    if (!cur.empty()) out.push_back(cur);
    return true;
}

bool parse_value(std::string_view text, Value& out, std::string& err) {
    const std::string v = trim(std::string(text));
    if (v.empty()) {
        err = "empty value";
        return false;
    }

    if (v == "true") {
        out = true;
        return true;
    }
    if (v == "false") {
        out = false;
        return true;
    }

    int64_t iv = 0;
    if (parse_int_literal(v, iv)) {
        out = iv;
        return true;
    }

    if (v.front() == '"' && v.back() == '"') {
        std::string sv{};
        if (!parse_string_literal(v, sv, err)) return false;
        out = std::move(sv);
        return true;
    }

    if (v.front() == '[' && v.back() == ']') {
        const std::string inner = trim(v.substr(1, v.size() - 2));
        if (inner.empty()) {
            out = std::vector<std::string>{};
            return true;
        }
        std::vector<std::string> items{};
        if (!split_array_items(inner, items, err)) return false;
        if (items.empty()) {
            out = std::vector<std::string>{};
            return true;
        }

        bool all_str = true;
        bool all_int = true;
        std::vector<std::string> svals{};
        std::vector<int64_t> ivals{};
        for (const auto& it : items) {
            if (it.size() >= 2 && it.front() == '"' && it.back() == '"') {
                std::string sv{};
                if (!parse_string_literal(it, sv, err)) return false;
                svals.push_back(std::move(sv));
                all_int = false;
                continue;
            }
            int64_t elem = 0;
            if (parse_int_literal(it, elem)) {
                ivals.push_back(elem);
                all_str = false;
                continue;
            }
            all_str = false;
            all_int = false;
            break;
        }
        if (!all_str && !all_int) {
            err = "array values must be homogeneous strings or integers";
            return false;
        }
        if (all_str) {
            out = std::move(svals);
            return true;
        }
        out = std::move(ivals);
        return true;
    }

    err = "unsupported TOML value";
    return false;
}

bool valid_key(std::string_view key) {
    if (key.empty()) return false;
    for (char c : key) {
        const bool ok = std::isalnum(static_cast<unsigned char>(c)) || c == '_' || c == '-' || c == '.';
        if (!ok) return false;
    }
    return true;
}

} // namespace

bool parse_file(const std::filesystem::path& path,
                FlatMap& out,
                std::vector<std::string>& warnings,
                std::string& err) {
    out.clear();
    err.clear();

    if (path.empty()) return true;
    std::error_code ec{};
    if (!std::filesystem::exists(path, ec)) return true;
    if (!std::filesystem::is_regular_file(path, ec)) {
        err = "not a regular file: " + path.string();
        return false;
    }

    std::ifstream ifs(path, std::ios::binary);
    if (!ifs) {
        err = "failed to open file: " + path.string();
        return false;
    }

    std::string section{};
    std::string line{};
    size_t line_no = 0;
    while (std::getline(ifs, line)) {
        ++line_no;
        std::string content = trim(strip_comment(line));
        if (content.empty()) continue;

        if (content.front() == '[') {
            if (content.back() != ']') {
                err = path.string() + ":" + std::to_string(line_no) + ": invalid section header";
                return false;
            }
            const std::string sec = trim(content.substr(1, content.size() - 2));
            if (sec.empty() || !valid_key(sec)) {
                err = path.string() + ":" + std::to_string(line_no) + ": invalid section name";
                return false;
            }
            section = sec;
            continue;
        }

        const auto eq = content.find('=');
        if (eq == std::string::npos) {
            err = path.string() + ":" + std::to_string(line_no) + ": expected '='";
            return false;
        }
        std::string key = trim(content.substr(0, eq));
        std::string rhs = trim(content.substr(eq + 1));
        if (key.empty() || !valid_key(key)) {
            err = path.string() + ":" + std::to_string(line_no) + ": invalid key";
            return false;
        }

        Value parsed{};
        std::string parse_err{};
        if (!parse_value(rhs, parsed, parse_err)) {
            err = path.string() + ":" + std::to_string(line_no) + ": " + parse_err;
            return false;
        }

        const std::string fq = section.empty() ? key : section + "." + key;
        if (out.contains(fq)) {
            warnings.push_back(path.string() + ":" + std::to_string(line_no) + ": duplicate key '" + fq + "', overriding");
        }
        out[fq] = std::move(parsed);
    }

    return true;
}

bool write_file(const std::filesystem::path& path,
                const FlatMap& values,
                std::string& err) {
    err.clear();
    if (path.empty()) {
        err = "empty output path";
        return false;
    }

    std::error_code ec{};
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        err = "failed to create config directory: " + path.parent_path().string();
        return false;
    }

    const std::filesystem::path tmp = path.string() + ".tmp";
    {
        std::ofstream ofs(tmp, std::ios::binary);
        if (!ofs) {
            err = "failed to open temporary file for write: " + tmp.string();
            return false;
        }
        ofs << render_toml(values);
        if (!ofs.good()) {
            err = "failed to write temporary file: " + tmp.string();
            return false;
        }
    }

    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        std::filesystem::remove(path, ec);
        ec.clear();
        std::filesystem::rename(tmp, path, ec);
    }
    if (ec) {
        std::filesystem::remove(tmp, ec);
        err = "failed to move temporary file to final path: " + path.string();
        return false;
    }

    return true;
}

} // namespace parus_tool::config::toml_lite
