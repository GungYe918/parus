#include <lei/builtins/ValueUtil.hpp>

#include <sstream>
#include <string>

namespace lei::builtins::detail {

namespace {

std::optional<eval::Value> fn_len(const std::vector<eval::Value>& args,
                                  const ast::Span& span,
                                  diag::Bag& diags) {
    if (!util::expect_arg_count(args, 1, "len", span, diags)) return std::nullopt;
    const auto& v = args[0];
    if (v.is_string()) return util::make_int(static_cast<int64_t>(std::get<std::string>(v.data).size()));
    if (v.is_array()) return util::make_int(static_cast<int64_t>(std::get<eval::Value::Array>(v.data).size()));
    if (v.is_object()) return util::make_int(static_cast<int64_t>(std::get<eval::Value::Object>(v.data).size()));
    if (v.is_dynamic_object()) {
        auto obj = util::materialize_object(v, span, diags, "len");
        if (!obj) return std::nullopt;
        return util::make_int(static_cast<int64_t>(obj->size()));
    }
    diags.add(diag::Code::L_TYPE_MISMATCH,
              span.file,
              span.line,
              span.column,
              "len expects string/array/object, got " + util::type_name(v));
    return std::nullopt;
}

std::optional<eval::Value> fn_type_name(const std::vector<eval::Value>& args,
                                        const ast::Span& span,
                                        diag::Bag& diags) {
    if (!util::expect_arg_count(args, 1, "type_name", span, diags)) return std::nullopt;
    return util::make_string(util::type_name(args[0]));
}

std::optional<eval::Value> fn_to_int(const std::vector<eval::Value>& args,
                                     const ast::Span& span,
                                     diag::Bag& diags) {
    if (!util::expect_arg_count(args, 1, "to_int", span, diags)) return std::nullopt;
    const auto& v = args[0];
    if (v.is_int()) return v;
    if (v.is_float()) return util::make_int(static_cast<int64_t>(std::get<double>(v.data)));
    if (v.is_bool()) return util::make_int(std::get<bool>(v.data) ? 1 : 0);
    if (v.is_string()) {
        try {
            size_t pos = 0;
            const auto& s = std::get<std::string>(v.data);
            const auto n = std::stoll(s, &pos, 10);
            if (pos != s.size()) throw std::invalid_argument("trailing");
            return util::make_int(static_cast<int64_t>(n));
        } catch (...) {
            diags.add(diag::Code::L_TYPE_MISMATCH,
                      span.file,
                      span.line,
                      span.column,
                      "to_int cannot parse string");
            return std::nullopt;
        }
    }
    diags.add(diag::Code::L_TYPE_MISMATCH,
              span.file,
              span.line,
              span.column,
              "to_int expects int/float/bool/string, got " + util::type_name(v));
    return std::nullopt;
}

std::optional<eval::Value> fn_to_float(const std::vector<eval::Value>& args,
                                       const ast::Span& span,
                                       diag::Bag& diags) {
    if (!util::expect_arg_count(args, 1, "to_float", span, diags)) return std::nullopt;
    const auto& v = args[0];
    if (v.is_float()) return v;
    if (v.is_int()) return util::make_float(static_cast<double>(std::get<int64_t>(v.data)));
    if (v.is_bool()) return util::make_float(std::get<bool>(v.data) ? 1.0 : 0.0);
    if (v.is_string()) {
        try {
            size_t pos = 0;
            const auto& s = std::get<std::string>(v.data);
            const auto n = std::stod(s, &pos);
            if (pos != s.size()) throw std::invalid_argument("trailing");
            return util::make_float(n);
        } catch (...) {
            diags.add(diag::Code::L_TYPE_MISMATCH,
                      span.file,
                      span.line,
                      span.column,
                      "to_float cannot parse string");
            return std::nullopt;
        }
    }
    diags.add(diag::Code::L_TYPE_MISMATCH,
              span.file,
              span.line,
              span.column,
              "to_float expects int/float/bool/string, got " + util::type_name(v));
    return std::nullopt;
}

std::optional<eval::Value> fn_to_string(const std::vector<eval::Value>& args,
                                        const ast::Span& span,
                                        diag::Bag& diags) {
    if (!util::expect_arg_count(args, 1, "to_string", span, diags)) return std::nullopt;
    const auto& v = args[0];
    if (v.is_string()) return v;
    if (v.is_int()) return util::make_string(std::to_string(std::get<int64_t>(v.data)));
    if (v.is_bool()) return util::make_string(std::get<bool>(v.data) ? "true" : "false");
    if (v.is_float()) {
        std::ostringstream oss;
        oss << std::get<double>(v.data);
        return util::make_string(oss.str());
    }
    return util::make_string(eval::to_string(v));
}

std::optional<eval::Value> fn_to_bool(const std::vector<eval::Value>& args,
                                      const ast::Span& span,
                                      diag::Bag& diags) {
    if (!util::expect_arg_count(args, 1, "to_bool", span, diags)) return std::nullopt;
    const auto& v = args[0];
    if (v.is_bool()) return v;
    if (v.is_int()) return util::make_bool(std::get<int64_t>(v.data) != 0);
    if (v.is_float()) return util::make_bool(std::get<double>(v.data) != 0.0);
    if (v.is_string()) return util::make_bool(!std::get<std::string>(v.data).empty());
    diags.add(diag::Code::L_TYPE_MISMATCH,
              span.file,
              span.line,
              span.column,
              "to_bool expects int/float/bool/string, got " + util::type_name(v));
    return std::nullopt;
}

std::optional<eval::Value> fn_deep_equal(const std::vector<eval::Value>& args,
                                         const ast::Span& span,
                                         diag::Bag& diags) {
    if (!util::expect_arg_count(args, 2, "deep_equal", span, diags)) return std::nullopt;
    return util::make_bool(util::deep_equal(args[0], args[1], span, diags));
}

} // namespace

void register_core_functions(eval::BuiltinRegistry& reg) {
    reg.register_native_function("len", fn_len);
    reg.register_native_function("type_name", fn_type_name);
    reg.register_native_function("to_int", fn_to_int);
    reg.register_native_function("to_float", fn_to_float);
    reg.register_native_function("to_string", fn_to_string);
    reg.register_native_function("to_bool", fn_to_bool);
    reg.register_native_function("deep_equal", fn_deep_equal);
}

} // namespace lei::builtins::detail

