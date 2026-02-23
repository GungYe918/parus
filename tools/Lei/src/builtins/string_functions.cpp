#include <lei/builtins/ValueUtil.hpp>

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

namespace lei::builtins::detail {

namespace {

std::optional<eval::Value> str_len(const std::vector<eval::Value>& args,
                                   const ast::Span& span,
                                   diag::Bag& diags) {
    std::string s;
    if (!util::expect_arg_count(args, 1, "str.len", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, s, "str.len", span, diags)) return std::nullopt;
    return util::make_int(static_cast<int64_t>(s.size()));
}

std::optional<eval::Value> str_contains(const std::vector<eval::Value>& args,
                                        const ast::Span& span,
                                        diag::Bag& diags) {
    std::string s;
    std::string needle;
    if (!util::expect_arg_count(args, 2, "str.contains", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, s, "str.contains", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 1, needle, "str.contains", span, diags)) return std::nullopt;
    return util::make_bool(s.find(needle) != std::string::npos);
}

std::optional<eval::Value> str_starts_with(const std::vector<eval::Value>& args,
                                           const ast::Span& span,
                                           diag::Bag& diags) {
    std::string s;
    std::string prefix;
    if (!util::expect_arg_count(args, 2, "str.starts_with", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, s, "str.starts_with", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 1, prefix, "str.starts_with", span, diags)) return std::nullopt;
    if (prefix.size() > s.size()) return util::make_bool(false);
    return util::make_bool(s.compare(0, prefix.size(), prefix) == 0);
}

std::optional<eval::Value> str_ends_with(const std::vector<eval::Value>& args,
                                         const ast::Span& span,
                                         diag::Bag& diags) {
    std::string s;
    std::string suffix;
    if (!util::expect_arg_count(args, 2, "str.ends_with", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, s, "str.ends_with", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 1, suffix, "str.ends_with", span, diags)) return std::nullopt;
    if (suffix.size() > s.size()) return util::make_bool(false);
    return util::make_bool(s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0);
}

std::optional<eval::Value> str_split(const std::vector<eval::Value>& args,
                                     const ast::Span& span,
                                     diag::Bag& diags) {
    std::string s;
    std::string sep;
    if (!util::expect_arg_count(args, 2, "str.split", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, s, "str.split", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 1, sep, "str.split", span, diags)) return std::nullopt;

    eval::Value::Array out{};
    if (sep.empty()) {
        out.reserve(s.size());
        for (char c : s) out.push_back(util::make_string(std::string(1, c)));
        return util::make_array(std::move(out));
    }

    size_t pos = 0;
    while (pos <= s.size()) {
        const size_t next = s.find(sep, pos);
        if (next == std::string::npos) {
            out.push_back(util::make_string(s.substr(pos)));
            break;
        }
        out.push_back(util::make_string(s.substr(pos, next - pos)));
        pos = next + sep.size();
    }
    return util::make_array(std::move(out));
}

std::optional<eval::Value> str_join(const std::vector<eval::Value>& args,
                                    const ast::Span& span,
                                    diag::Bag& diags) {
    std::vector<std::string> parts;
    std::string sep;
    if (!util::expect_arg_count(args, 2, "str.join", span, diags)) return std::nullopt;
    if (!util::arg_as_string_array(args, 0, parts, "str.join", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 1, sep, "str.join", span, diags)) return std::nullopt;

    std::string out;
    for (size_t i = 0; i < parts.size(); ++i) {
        if (i != 0) out += sep;
        out += parts[i];
    }
    return util::make_string(std::move(out));
}

std::optional<eval::Value> str_replace(const std::vector<eval::Value>& args,
                                       const ast::Span& span,
                                       diag::Bag& diags) {
    std::string s;
    std::string from;
    std::string to;
    if (!util::expect_arg_count(args, 3, "str.replace", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, s, "str.replace", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 1, from, "str.replace", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 2, to, "str.replace", span, diags)) return std::nullopt;

    if (from.empty()) return util::make_string(s);
    size_t pos = 0;
    while (pos < s.size()) {
        const size_t next = s.find(from, pos);
        if (next == std::string::npos) break;
        s.replace(next, from.size(), to);
        pos = next + to.size();
    }
    return util::make_string(std::move(s));
}

std::optional<eval::Value> str_trim(const std::vector<eval::Value>& args,
                                    const ast::Span& span,
                                    diag::Bag& diags) {
    std::string s;
    if (!util::expect_arg_count(args, 1, "str.trim", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, s, "str.trim", span, diags)) return std::nullopt;
    return util::make_string(util::trim_ascii(std::move(s)));
}

std::optional<eval::Value> str_lower(const std::vector<eval::Value>& args,
                                     const ast::Span& span,
                                     diag::Bag& diags) {
    std::string s;
    if (!util::expect_arg_count(args, 1, "str.lower", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, s, "str.lower", span, diags)) return std::nullopt;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return util::make_string(std::move(s));
}

std::optional<eval::Value> str_upper(const std::vector<eval::Value>& args,
                                     const ast::Span& span,
                                     diag::Bag& diags) {
    std::string s;
    if (!util::expect_arg_count(args, 1, "str.upper", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 0, s, "str.upper", span, diags)) return std::nullopt;
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return util::make_string(std::move(s));
}

eval::Value make_str_namespace() {
    eval::Value::Object str{};
    str["len"] = util::make_native_function("str.len", str_len);
    str["contains"] = util::make_native_function("str.contains", str_contains);
    str["starts_with"] = util::make_native_function("str.starts_with", str_starts_with);
    str["ends_with"] = util::make_native_function("str.ends_with", str_ends_with);
    str["split"] = util::make_native_function("str.split", str_split);
    str["join"] = util::make_native_function("str.join", str_join);
    str["replace"] = util::make_native_function("str.replace", str_replace);
    str["trim"] = util::make_native_function("str.trim", str_trim);
    str["lower"] = util::make_native_function("str.lower", str_lower);
    str["upper"] = util::make_native_function("str.upper", str_upper);
    return util::make_object(std::move(str));
}

} // namespace

void register_string_functions(eval::BuiltinRegistry& reg) {
    reg.register_value("str", [] { return make_str_namespace(); });
}

} // namespace lei::builtins::detail

