#pragma once

#include <lei/eval/Evaluator.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace lei::builtins::util {

inline eval::Value make_int(int64_t v) {
    eval::Value out{};
    out.data = v;
    return out;
}

inline eval::Value make_float(double v) {
    eval::Value out{};
    out.data = v;
    return out;
}

inline eval::Value make_string(std::string v) {
    eval::Value out{};
    out.data = std::move(v);
    return out;
}

inline eval::Value make_bool(bool v) {
    eval::Value out{};
    out.data = v;
    return out;
}

inline eval::Value make_array(eval::Value::Array arr = {}) {
    eval::Value out{};
    out.data = std::move(arr);
    return out;
}

inline eval::Value make_object(eval::Value::Object obj = {}) {
    eval::Value out{};
    out.data = std::move(obj);
    return out;
}

inline eval::Value make_native_function(std::string name,
                                        eval::BuiltinFunction::Callback callback) {
    auto fn = std::make_shared<eval::BuiltinFunction>();
    fn->name = std::move(name);
    fn->callback = std::move(callback);
    eval::Value out{};
    out.data = std::move(fn);
    return out;
}

inline eval::Value make_dynamic_object(std::string name,
                                       eval::DynamicObject::MemberResolver resolver,
                                       eval::DynamicObject::KeysProvider keys) {
    auto dyn = std::make_shared<eval::DynamicObject>();
    dyn->name = std::move(name);
    dyn->resolve = std::move(resolver);
    dyn->keys = std::move(keys);
    eval::Value out{};
    out.data = std::move(dyn);
    return out;
}

inline std::string type_name(const eval::Value& v) {
    if (v.is_int()) return "int";
    if (v.is_float()) return "float";
    if (v.is_string()) return "string";
    if (v.is_bool()) return "bool";
    if (v.is_array()) return "array";
    if (v.is_object()) return "object";
    if (v.is_dynamic_object()) return "dynamic_object";
    if (v.is_function()) return "function";
    if (v.is_native_function()) return "native_function";
    if (v.is_template()) return "template";
    if (v.is_patch()) return "patch";
    return "unknown";
}

inline bool expect_arg_count(const std::vector<eval::Value>& args,
                             size_t expected,
                             std::string_view fn,
                             const ast::Span& span,
                             diag::Bag& diags) {
    if (args.size() == expected) return true;
    diags.add(diag::Code::L_TYPE_MISMATCH,
              span.file,
              span.line,
              span.column,
              std::string(fn) + " expects " + std::to_string(expected)
                  + " args, got " + std::to_string(args.size()));
    return false;
}

inline bool expect_arg_range(const std::vector<eval::Value>& args,
                             size_t min_count,
                             size_t max_count,
                             std::string_view fn,
                             const ast::Span& span,
                             diag::Bag& diags) {
    if (args.size() >= min_count && args.size() <= max_count) return true;
    diags.add(diag::Code::L_TYPE_MISMATCH,
              span.file,
              span.line,
              span.column,
              std::string(fn) + " expects "
                  + std::to_string(min_count) + ".." + std::to_string(max_count)
                  + " args, got " + std::to_string(args.size()));
    return false;
}

inline bool arg_as_int(const std::vector<eval::Value>& args,
                       size_t idx,
                       int64_t& out,
                       std::string_view fn,
                       const ast::Span& span,
                       diag::Bag& diags) {
    if (idx >= args.size() || !args[idx].is_int()) {
        diags.add(diag::Code::L_TYPE_MISMATCH,
                  span.file,
                  span.line,
                  span.column,
                  std::string(fn) + " arg[" + std::to_string(idx)
                      + "] must be int, got " + (idx < args.size() ? type_name(args[idx]) : "missing"));
        return false;
    }
    out = std::get<int64_t>(args[idx].data);
    return true;
}

inline bool arg_as_float(const std::vector<eval::Value>& args,
                         size_t idx,
                         double& out,
                         std::string_view fn,
                         const ast::Span& span,
                         diag::Bag& diags) {
    if (idx >= args.size()) {
        diags.add(diag::Code::L_TYPE_MISMATCH,
                  span.file,
                  span.line,
                  span.column,
                  std::string(fn) + " arg[" + std::to_string(idx) + "] is missing");
        return false;
    }
    if (args[idx].is_float()) {
        out = std::get<double>(args[idx].data);
        return true;
    }
    if (args[idx].is_int()) {
        out = static_cast<double>(std::get<int64_t>(args[idx].data));
        return true;
    }
    diags.add(diag::Code::L_TYPE_MISMATCH,
              span.file,
              span.line,
              span.column,
              std::string(fn) + " arg[" + std::to_string(idx)
                  + "] must be float/int, got " + type_name(args[idx]));
    return false;
}

inline bool arg_as_string(const std::vector<eval::Value>& args,
                          size_t idx,
                          std::string& out,
                          std::string_view fn,
                          const ast::Span& span,
                          diag::Bag& diags) {
    if (idx >= args.size() || !args[idx].is_string()) {
        diags.add(diag::Code::L_TYPE_MISMATCH,
                  span.file,
                  span.line,
                  span.column,
                  std::string(fn) + " arg[" + std::to_string(idx)
                      + "] must be string, got " + (idx < args.size() ? type_name(args[idx]) : "missing"));
        return false;
    }
    out = std::get<std::string>(args[idx].data);
    return true;
}

inline bool arg_as_bool(const std::vector<eval::Value>& args,
                        size_t idx,
                        bool& out,
                        std::string_view fn,
                        const ast::Span& span,
                        diag::Bag& diags) {
    if (idx >= args.size() || !args[idx].is_bool()) {
        diags.add(diag::Code::L_TYPE_MISMATCH,
                  span.file,
                  span.line,
                  span.column,
                  std::string(fn) + " arg[" + std::to_string(idx)
                      + "] must be bool, got " + (idx < args.size() ? type_name(args[idx]) : "missing"));
        return false;
    }
    out = std::get<bool>(args[idx].data);
    return true;
}

inline bool arg_as_string_array(const std::vector<eval::Value>& args,
                                size_t idx,
                                std::vector<std::string>& out,
                                std::string_view fn,
                                const ast::Span& span,
                                diag::Bag& diags) {
    if (idx >= args.size() || !args[idx].is_array()) {
        diags.add(diag::Code::L_TYPE_MISMATCH,
                  span.file,
                  span.line,
                  span.column,
                  std::string(fn) + " arg[" + std::to_string(idx)
                      + "] must be [string], got " + (idx < args.size() ? type_name(args[idx]) : "missing"));
        return false;
    }
    const auto& arr = std::get<eval::Value::Array>(args[idx].data);
    out.clear();
    out.reserve(arr.size());
    for (size_t i = 0; i < arr.size(); ++i) {
        if (!arr[i].is_string()) {
            diags.add(diag::Code::L_TYPE_MISMATCH,
                      span.file,
                      span.line,
                      span.column,
                      std::string(fn) + " arg[" + std::to_string(idx)
                          + "][" + std::to_string(i) + "] must be string");
            return false;
        }
        out.push_back(std::get<std::string>(arr[i].data));
    }
    return true;
}

inline std::optional<eval::Value::Object> materialize_object(const eval::Value& value,
                                                             const ast::Span& span,
                                                             diag::Bag& diags,
                                                             std::string_view fn_name) {
    if (auto obj = value.as_object()) return *obj;
    if (auto dyn = value.as_dynamic_object()) {
        if (!dyn->resolve || !dyn->keys) {
            diags.add(diag::Code::L_TYPE_MISMATCH,
                      span.file,
                      span.line,
                      span.column,
                      std::string(fn_name) + " requires enumerable object; dynamic object is not enumerable");
            return std::nullopt;
        }
        auto keys = dyn->keys();
        std::sort(keys.begin(), keys.end());
        keys.erase(std::unique(keys.begin(), keys.end()), keys.end());

        eval::Value::Object out{};
        for (const auto& k : keys) {
            const auto before = diags.all().size();
            auto v = dyn->resolve(k, span, diags);
            if (!v) {
                if (diags.all().size() == before) {
                    diags.add(diag::Code::L_UNKNOWN_IDENTIFIER,
                              span.file,
                              span.line,
                              span.column,
                              "unknown dynamic object key: " + k);
                }
                return std::nullopt;
            }
            out[k] = *v;
        }
        return out;
    }
    diags.add(diag::Code::L_TYPE_MISMATCH,
              span.file,
              span.line,
              span.column,
              std::string(fn_name) + " expects object, got " + type_name(value));
    return std::nullopt;
}

inline std::optional<eval::Value> object_get(const eval::Value& value,
                                             std::string_view key,
                                             const ast::Span& span,
                                             diag::Bag& diags,
                                             bool report_missing = true) {
    if (auto obj = value.as_object()) {
        auto it = obj->find(std::string(key));
        if (it != obj->end()) return it->second;
        if (report_missing) {
            diags.add(diag::Code::L_UNKNOWN_IDENTIFIER,
                      span.file,
                      span.line,
                      span.column,
                      "unknown object key: " + std::string(key));
        }
        return std::nullopt;
    }
    if (auto dyn = value.as_dynamic_object()) {
        if (!dyn->resolve) {
            diags.add(diag::Code::L_TYPE_MISMATCH,
                      span.file,
                      span.line,
                      span.column,
                      "dynamic object has no resolver");
            return std::nullopt;
        }
        const auto before = diags.all().size();
        auto resolved = dyn->resolve(key, span, diags);
        if (!resolved && report_missing && diags.all().size() == before) {
            diags.add(diag::Code::L_UNKNOWN_IDENTIFIER,
                      span.file,
                      span.line,
                      span.column,
                      "unknown dynamic object key: " + std::string(key));
        }
        return resolved;
    }
    diags.add(diag::Code::L_TYPE_MISMATCH,
              span.file,
              span.line,
              span.column,
              "member access requires object, got " + type_name(value));
    return std::nullopt;
}

inline bool deep_equal(const eval::Value& a,
                       const eval::Value& b,
                       const ast::Span& span,
                       diag::Bag& diags) {
    if (a.data.index() == b.data.index()) {
        if (a.is_int()) return std::get<int64_t>(a.data) == std::get<int64_t>(b.data);
        if (a.is_float()) return std::get<double>(a.data) == std::get<double>(b.data);
        if (a.is_string()) return std::get<std::string>(a.data) == std::get<std::string>(b.data);
        if (a.is_bool()) return std::get<bool>(a.data) == std::get<bool>(b.data);
    }

    if (a.is_array() && b.is_array()) {
        const auto& aa = std::get<eval::Value::Array>(a.data);
        const auto& bb = std::get<eval::Value::Array>(b.data);
        if (aa.size() != bb.size()) return false;
        for (size_t i = 0; i < aa.size(); ++i) {
            if (!deep_equal(aa[i], bb[i], span, diags)) return false;
        }
        return true;
    }

    const auto ao = materialize_object(a, span, diags, "deep_equal");
    if (ao.has_value()) {
        const auto bo = materialize_object(b, span, diags, "deep_equal");
        if (!bo.has_value()) return false;
        if (ao->size() != bo->size()) return false;
        for (const auto& [k, av] : *ao) {
            auto it = bo->find(k);
            if (it == bo->end()) return false;
            if (!deep_equal(av, it->second, span, diags)) return false;
        }
        return true;
    }

    return false;
}

inline std::string trim_ascii(std::string s) {
    auto is_space = [](unsigned char c) -> bool { return std::isspace(c) != 0; };
    while (!s.empty() && is_space(static_cast<unsigned char>(s.front()))) s.erase(s.begin());
    while (!s.empty() && is_space(static_cast<unsigned char>(s.back()))) s.pop_back();
    return s;
}

} // namespace lei::builtins::util

