#include <lei/builtins/ValueUtil.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace lei::builtins::detail {

namespace {

struct SemVer {
    int64_t major = 0;
    int64_t minor = 0;
    int64_t patch = 0;
    std::vector<std::string> prerelease{};
    std::string build{};
};

std::vector<std::string> split_sv(std::string_view s, char sep) {
    std::vector<std::string> out{};
    size_t pos = 0;
    while (pos <= s.size()) {
        const size_t next = s.find(sep, pos);
        if (next == std::string_view::npos) {
            out.emplace_back(s.substr(pos));
            break;
        }
        out.emplace_back(s.substr(pos, next - pos));
        pos = next + 1;
    }
    return out;
}

bool parse_i64(std::string_view s, int64_t& out) {
    if (s.empty()) return false;
    for (char c : s) {
        if (!std::isdigit(static_cast<unsigned char>(c))) return false;
    }
    try {
        out = std::stoll(std::string(s));
    } catch (...) {
        return false;
    }
    return true;
}

std::optional<SemVer> parse_semver_text(std::string s) {
    s = util::trim_ascii(std::move(s));
    if (s.empty()) return std::nullopt;

    SemVer out{};
    std::string core = s;

    const size_t plus = core.find('+');
    if (plus != std::string::npos) {
        out.build = core.substr(plus + 1);
        core = core.substr(0, plus);
    }

    const size_t dash = core.find('-');
    if (dash != std::string::npos) {
        out.prerelease = split_sv(core.substr(dash + 1), '.');
        core = core.substr(0, dash);
    }

    const auto parts = split_sv(core, '.');
    if (parts.size() != 3) return std::nullopt;
    if (!parse_i64(parts[0], out.major)) return std::nullopt;
    if (!parse_i64(parts[1], out.minor)) return std::nullopt;
    if (!parse_i64(parts[2], out.patch)) return std::nullopt;
    return out;
}

std::string semver_to_string(const SemVer& v) {
    std::ostringstream oss;
    oss << v.major << "." << v.minor << "." << v.patch;
    if (!v.prerelease.empty()) {
        oss << "-";
        for (size_t i = 0; i < v.prerelease.size(); ++i) {
            if (i != 0) oss << ".";
            oss << v.prerelease[i];
        }
    }
    if (!v.build.empty()) oss << "+" << v.build;
    return oss.str();
}

int compare_ident(std::string_view a, std::string_view b) {
    int64_t ai = 0;
    int64_t bi = 0;
    const bool an = parse_i64(a, ai);
    const bool bn = parse_i64(b, bi);
    if (an && bn) {
        if (ai < bi) return -1;
        if (ai > bi) return 1;
        return 0;
    }
    if (an && !bn) return -1;
    if (!an && bn) return 1;
    if (a < b) return -1;
    if (a > b) return 1;
    return 0;
}

int semver_compare(const SemVer& a, const SemVer& b) {
    if (a.major < b.major) return -1;
    if (a.major > b.major) return 1;
    if (a.minor < b.minor) return -1;
    if (a.minor > b.minor) return 1;
    if (a.patch < b.patch) return -1;
    if (a.patch > b.patch) return 1;

    const bool apre = !a.prerelease.empty();
    const bool bpre = !b.prerelease.empty();
    if (!apre && !bpre) return 0;
    if (!apre && bpre) return 1;
    if (apre && !bpre) return -1;

    const size_t n = std::max(a.prerelease.size(), b.prerelease.size());
    for (size_t i = 0; i < n; ++i) {
        if (i >= a.prerelease.size()) return -1;
        if (i >= b.prerelease.size()) return 1;
        const int cmp = compare_ident(a.prerelease[i], b.prerelease[i]);
        if (cmp != 0) return cmp;
    }
    return 0;
}

std::optional<SemVer> semver_from_value(const eval::Value& v) {
    if (v.is_string()) return parse_semver_text(std::get<std::string>(v.data));
    if (v.is_object()) {
        const auto& obj = std::get<eval::Value::Object>(v.data);
        auto it0 = obj.find("major");
        auto it1 = obj.find("minor");
        auto it2 = obj.find("patch");
        if (it0 == obj.end() || it1 == obj.end() || it2 == obj.end()) return std::nullopt;
        if (!it0->second.is_int() || !it1->second.is_int() || !it2->second.is_int()) return std::nullopt;
        SemVer out{};
        out.major = std::get<int64_t>(it0->second.data);
        out.minor = std::get<int64_t>(it1->second.data);
        out.patch = std::get<int64_t>(it2->second.data);
        auto itp = obj.find("prerelease");
        if (itp != obj.end() && itp->second.is_string()) {
            out.prerelease = split_sv(std::get<std::string>(itp->second.data), '.');
        }
        auto itb = obj.find("build");
        if (itb != obj.end() && itb->second.is_string()) {
            out.build = std::get<std::string>(itb->second.data);
        }
        return out;
    }
    return std::nullopt;
}

std::optional<eval::Value> semver_parse(const std::vector<eval::Value>& args,
                                        const ast::Span& span,
                                        diag::Bag& diags) {
    std::string s;
    if (!util::expect_arg_count(args, 1, "semver.parse", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, s, "semver.parse", span, diags)) return std::nullopt;
    auto v = parse_semver_text(s);
    if (!v) {
        diags.add(diag::Code::L_TYPE_MISMATCH, span.file, span.line, span.column, "invalid semver text");
        return std::nullopt;
    }
    eval::Value::Object obj{};
    obj["major"] = util::make_int(v->major);
    obj["minor"] = util::make_int(v->minor);
    obj["patch"] = util::make_int(v->patch);
    obj["prerelease"] = util::make_string([&]() {
        std::string out;
        for (size_t i = 0; i < v->prerelease.size(); ++i) {
            if (i != 0) out += ".";
            out += v->prerelease[i];
        }
        return out;
    }());
    obj["build"] = util::make_string(v->build);
    obj["raw"] = util::make_string(semver_to_string(*v));
    return util::make_object(std::move(obj));
}

std::optional<eval::Value> semver_compare_fn(const std::vector<eval::Value>& args,
                                             const ast::Span& span,
                                             diag::Bag& diags) {
    if (!util::expect_arg_count(args, 2, "semver.compare", span, diags)) return std::nullopt;
    auto a = semver_from_value(args[0]);
    auto b = semver_from_value(args[1]);
    if (!a || !b) {
        diags.add(diag::Code::L_TYPE_MISMATCH,
                  span.file,
                  span.line,
                  span.column,
                  "semver.compare expects semver string/object arguments");
        return std::nullopt;
    }
    return util::make_int(static_cast<int64_t>(semver_compare(*a, *b)));
}

bool semver_satisfies_single(const SemVer& v, std::string range) {
    range = util::trim_ascii(std::move(range));
    if (range.empty()) return false;

    auto parse_target = [&](std::string_view text) -> std::optional<SemVer> {
        return parse_semver_text(std::string(text));
    };

    auto cmp_ok = [&](const SemVer& rhs, std::string_view op) -> bool {
        const int c = semver_compare(v, rhs);
        if (op == "=") return c == 0;
        if (op == ">") return c > 0;
        if (op == ">=") return c >= 0;
        if (op == "<") return c < 0;
        if (op == "<=") return c <= 0;
        return false;
    };

    if (range.starts_with("^")) {
        auto base = parse_target(range.substr(1));
        if (!base) return false;
        SemVer upper = *base;
        if (base->major > 0) {
            upper.major = base->major + 1;
            upper.minor = 0;
            upper.patch = 0;
            upper.prerelease.clear();
            upper.build.clear();
        } else if (base->minor > 0) {
            upper.minor = base->minor + 1;
            upper.patch = 0;
            upper.prerelease.clear();
            upper.build.clear();
        } else {
            upper.patch = base->patch + 1;
            upper.prerelease.clear();
            upper.build.clear();
        }
        return semver_compare(v, *base) >= 0 && semver_compare(v, upper) < 0;
    }

    if (range.starts_with("~")) {
        auto base = parse_target(range.substr(1));
        if (!base) return false;
        SemVer upper = *base;
        upper.minor = base->minor + 1;
        upper.patch = 0;
        upper.prerelease.clear();
        upper.build.clear();
        return semver_compare(v, *base) >= 0 && semver_compare(v, upper) < 0;
    }

    for (const auto& op : std::array<std::string_view, 5>{">=", "<=", ">", "<", "="}) {
        if (range.starts_with(op)) {
            auto rhs = parse_target(range.substr(op.size()));
            if (!rhs) return false;
            return cmp_ok(*rhs, op);
        }
    }

    auto rhs = parse_target(range);
    if (!rhs) return false;
    return semver_compare(v, *rhs) == 0;
}

std::optional<eval::Value> semver_satisfies(const std::vector<eval::Value>& args,
                                            const ast::Span& span,
                                            diag::Bag& diags) {
    std::string range;
    if (!util::expect_arg_count(args, 2, "semver.satisfies", span, diags)) return std::nullopt;
    auto v = semver_from_value(args[0]);
    if (!v) {
        diags.add(diag::Code::L_TYPE_MISMATCH,
                  span.file,
                  span.line,
                  span.column,
                  "semver.satisfies arg[0] must be semver string/object");
        return std::nullopt;
    }
    if (!util::arg_as_string(args, 1, range, "semver.satisfies", span, diags)) return std::nullopt;
    return util::make_bool(semver_satisfies_single(*v, range));
}

std::optional<eval::Value> semver_bump(const std::vector<eval::Value>& args,
                                       const ast::Span& span,
                                       diag::Bag& diags) {
    std::string part;
    if (!util::expect_arg_count(args, 2, "semver.bump", span, diags)) return std::nullopt;
    auto v = semver_from_value(args[0]);
    if (!v) {
        diags.add(diag::Code::L_TYPE_MISMATCH,
                  span.file,
                  span.line,
                  span.column,
                  "semver.bump arg[0] must be semver string/object");
        return std::nullopt;
    }
    if (!util::arg_as_string(args, 1, part, "semver.bump", span, diags)) return std::nullopt;
    if (part == "major") {
        ++v->major;
        v->minor = 0;
        v->patch = 0;
    } else if (part == "minor") {
        ++v->minor;
        v->patch = 0;
    } else if (part == "patch") {
        ++v->patch;
    } else {
        diags.add(diag::Code::L_TYPE_MISMATCH,
                  span.file,
                  span.line,
                  span.column,
                  "semver.bump part must be 'major', 'minor', or 'patch'");
        return std::nullopt;
    }
    v->prerelease.clear();
    v->build.clear();
    return util::make_string(semver_to_string(*v));
}

eval::Value make_semver_namespace() {
    eval::Value::Object semver{};
    semver["parse"] = util::make_native_function("semver.parse", semver_parse);
    semver["compare"] = util::make_native_function("semver.compare", semver_compare_fn);
    semver["satisfies"] = util::make_native_function("semver.satisfies", semver_satisfies);
    semver["bump"] = util::make_native_function("semver.bump", semver_bump);
    return util::make_object(std::move(semver));
}

} // namespace

void register_semver_functions(eval::BuiltinRegistry& reg) {
    reg.register_value("semver", [] { return make_semver_namespace(); });
}

} // namespace lei::builtins::detail

