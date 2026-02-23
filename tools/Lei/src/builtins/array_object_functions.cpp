#include <lei/builtins/ValueUtil.hpp>

#include <algorithm>
#include <string>
#include <vector>

namespace lei::builtins::detail {

namespace {

std::optional<eval::Value> arr_len(const std::vector<eval::Value>& args,
                                   const ast::Span& span,
                                   diag::Bag& diags) {
    if (!util::expect_arg_count(args, 1, "arr.len", span, diags)) return std::nullopt;
    if (!args[0].is_array()) {
        diags.add(diag::Code::L_TYPE_MISMATCH, span.file, span.line, span.column, "arr.len expects array");
        return std::nullopt;
    }
    return util::make_int(static_cast<int64_t>(std::get<eval::Value::Array>(args[0].data).size()));
}

std::optional<eval::Value> arr_concat(const std::vector<eval::Value>& args,
                                      const ast::Span& span,
                                      diag::Bag& diags) {
    if (!util::expect_arg_count(args, 2, "arr.concat", span, diags)) return std::nullopt;
    if (!args[0].is_array() || !args[1].is_array()) {
        diags.add(diag::Code::L_TYPE_MISMATCH, span.file, span.line, span.column, "arr.concat expects array, array");
        return std::nullopt;
    }
    auto out = std::get<eval::Value::Array>(args[0].data);
    const auto& b = std::get<eval::Value::Array>(args[1].data);
    out.insert(out.end(), b.begin(), b.end());
    return util::make_array(std::move(out));
}

std::optional<eval::Value> arr_contains(const std::vector<eval::Value>& args,
                                        const ast::Span& span,
                                        diag::Bag& diags) {
    if (!util::expect_arg_count(args, 2, "arr.contains", span, diags)) return std::nullopt;
    if (!args[0].is_array()) {
        diags.add(diag::Code::L_TYPE_MISMATCH, span.file, span.line, span.column, "arr.contains expects array");
        return std::nullopt;
    }
    const auto& arr = std::get<eval::Value::Array>(args[0].data);
    for (const auto& v : arr) {
        if (util::deep_equal(v, args[1], span, diags)) return util::make_bool(true);
        if (diags.has_error()) return std::nullopt;
    }
    return util::make_bool(false);
}

std::optional<eval::Value> arr_uniq(const std::vector<eval::Value>& args,
                                    const ast::Span& span,
                                    diag::Bag& diags) {
    if (!util::expect_arg_count(args, 1, "arr.uniq", span, diags)) return std::nullopt;
    if (!args[0].is_array()) {
        diags.add(diag::Code::L_TYPE_MISMATCH, span.file, span.line, span.column, "arr.uniq expects array");
        return std::nullopt;
    }
    const auto& arr = std::get<eval::Value::Array>(args[0].data);
    eval::Value::Array out{};
    for (const auto& v : arr) {
        bool seen = false;
        for (const auto& e : out) {
            if (util::deep_equal(v, e, span, diags)) {
                seen = true;
                break;
            }
            if (diags.has_error()) return std::nullopt;
        }
        if (!seen) out.push_back(v);
    }
    return util::make_array(std::move(out));
}

std::optional<eval::Value> arr_sorted(const std::vector<eval::Value>& args,
                                      const ast::Span& span,
                                      diag::Bag& diags) {
    std::vector<std::string> values;
    if (!util::expect_arg_count(args, 1, "arr.sorted", span, diags)) return std::nullopt;
    if (!util::arg_as_string_array(args, 0, values, "arr.sorted", span, diags)) return std::nullopt;
    std::sort(values.begin(), values.end());
    eval::Value::Array out{};
    out.reserve(values.size());
    for (auto& v : values) out.push_back(util::make_string(std::move(v)));
    return util::make_array(std::move(out));
}

std::optional<eval::Value> arr_slice(const std::vector<eval::Value>& args,
                                     const ast::Span& span,
                                     diag::Bag& diags) {
    int64_t begin = 0;
    int64_t end = 0;
    if (!util::expect_arg_count(args, 3, "arr.slice", span, diags)) return std::nullopt;
    if (!args[0].is_array()) {
        diags.add(diag::Code::L_TYPE_MISMATCH, span.file, span.line, span.column, "arr.slice expects array");
        return std::nullopt;
    }
    if (!util::arg_as_int(args, 1, begin, "arr.slice", span, diags)) return std::nullopt;
    if (!util::arg_as_int(args, 2, end, "arr.slice", span, diags)) return std::nullopt;

    const auto& arr = std::get<eval::Value::Array>(args[0].data);
    const int64_t n = static_cast<int64_t>(arr.size());
    begin = std::clamp<int64_t>(begin, 0, n);
    end = std::clamp<int64_t>(end, 0, n);
    if (end < begin) end = begin;

    eval::Value::Array out{};
    for (int64_t i = begin; i < end; ++i) out.push_back(arr[static_cast<size_t>(i)]);
    return util::make_array(std::move(out));
}

std::optional<eval::Value> obj_keys(const std::vector<eval::Value>& args,
                                    const ast::Span& span,
                                    diag::Bag& diags) {
    if (!util::expect_arg_count(args, 1, "obj.keys", span, diags)) return std::nullopt;
    auto obj = util::materialize_object(args[0], span, diags, "obj.keys");
    if (!obj) return std::nullopt;
    eval::Value::Array out{};
    out.reserve(obj->size());
    for (const auto& [k, _] : *obj) out.push_back(util::make_string(k));
    return util::make_array(std::move(out));
}

std::optional<eval::Value> obj_has(const std::vector<eval::Value>& args,
                                   const ast::Span& span,
                                   diag::Bag& diags) {
    std::string key;
    if (!util::expect_arg_count(args, 2, "obj.has", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 1, key, "obj.has", span, diags)) return std::nullopt;

    const auto before = diags.all().size();
    auto v = util::object_get(args[0], key, span, diags, false);
    if (!v && diags.all().size() == before) return util::make_bool(false);
    if (!v) return std::nullopt;
    return util::make_bool(true);
}

std::optional<eval::Value> obj_get(const std::vector<eval::Value>& args,
                                   const ast::Span& span,
                                   diag::Bag& diags) {
    std::string key;
    if (!util::expect_arg_range(args, 2, 3, "obj.get", span, diags)) return std::nullopt;
    if (!util::arg_as_string(args, 1, key, "obj.get", span, diags)) return std::nullopt;

    const auto before = diags.all().size();
    auto v = util::object_get(args[0], key, span, diags, false);
    if (v) return *v;
    if (diags.all().size() != before) return std::nullopt;
    if (args.size() == 3) return args[2];

    diags.add(diag::Code::L_UNKNOWN_IDENTIFIER,
              span.file,
              span.line,
              span.column,
              "obj.get missing key without default: " + key);
    return std::nullopt;
}

std::optional<eval::Value> obj_values(const std::vector<eval::Value>& args,
                                      const ast::Span& span,
                                      diag::Bag& diags) {
    if (!util::expect_arg_count(args, 1, "obj.values", span, diags)) return std::nullopt;
    auto obj = util::materialize_object(args[0], span, diags, "obj.values");
    if (!obj) return std::nullopt;
    eval::Value::Array out{};
    out.reserve(obj->size());
    for (const auto& [_, v] : *obj) out.push_back(v);
    return util::make_array(std::move(out));
}

eval::Value make_arr_namespace() {
    eval::Value::Object arr{};
    arr["len"] = util::make_native_function("arr.len", arr_len);
    arr["concat"] = util::make_native_function("arr.concat", arr_concat);
    arr["contains"] = util::make_native_function("arr.contains", arr_contains);
    arr["uniq"] = util::make_native_function("arr.uniq", arr_uniq);
    arr["sorted"] = util::make_native_function("arr.sorted", arr_sorted);
    arr["slice"] = util::make_native_function("arr.slice", arr_slice);
    return util::make_object(std::move(arr));
}

eval::Value make_obj_namespace() {
    eval::Value::Object obj{};
    obj["keys"] = util::make_native_function("obj.keys", obj_keys);
    obj["has"] = util::make_native_function("obj.has", obj_has);
    obj["get"] = util::make_native_function("obj.get", obj_get);
    obj["values"] = util::make_native_function("obj.values", obj_values);
    return util::make_object(std::move(obj));
}

} // namespace

void register_array_object_functions(eval::BuiltinRegistry& reg) {
    reg.register_value("arr", [] { return make_arr_namespace(); });
    reg.register_value("obj", [] { return make_obj_namespace(); });
}

} // namespace lei::builtins::detail

