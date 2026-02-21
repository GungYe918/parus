#pragma once

#include <lei/ast/Nodes.hpp>
#include <lei/diag/DiagCode.hpp>
#include <lei/parse/Parser.hpp>

#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

namespace lei::eval {

struct EvaluatorBudget {
    uint32_t max_steps = 200000;
    uint32_t max_call_depth = 64;
    uint32_t max_nodes = 200000;
};

struct FunctionValue {
    std::string module_path;
    std::string name;
    std::vector<std::string> params;
    const ast::Expr* body = nullptr;
};

struct BuiltinFunction;

struct Value {
    using Object = std::map<std::string, Value>;
    using Array = std::vector<Value>;
    using Function = FunctionValue;
    using NativeFunction = std::shared_ptr<BuiltinFunction>;

    std::variant<int64_t, double, std::string, bool, Object, Array, Function, NativeFunction> data;

    bool is_int() const { return std::holds_alternative<int64_t>(data); }
    bool is_float() const { return std::holds_alternative<double>(data); }
    bool is_string() const { return std::holds_alternative<std::string>(data); }
    bool is_bool() const { return std::holds_alternative<bool>(data); }
    bool is_object() const { return std::holds_alternative<Object>(data); }
    bool is_array() const { return std::holds_alternative<Array>(data); }
    bool is_function() const { return std::holds_alternative<Function>(data); }
    bool is_native_function() const { return std::holds_alternative<NativeFunction>(data); }

    const Object* as_object() const { return std::get_if<Object>(&data); }
    Object* as_object() { return std::get_if<Object>(&data); }
};

struct BuiltinFunction {
    using Callback = std::function<std::optional<Value>(const std::vector<Value>& args,
                                                        const ast::Span& span,
                                                        diag::Bag& diags)>;

    std::string name{};
    Callback callback{};
};

class BuiltinRegistry {
public:
    using ValueFactory = std::function<Value()>;

    void register_value(std::string name, ValueFactory factory);
    void register_native_function(std::string name, BuiltinFunction::Callback callback);

    void inject_into(std::unordered_map<std::string, Value>& symbols) const;
    bool has_symbol(std::string_view name) const;

private:
    std::unordered_map<std::string, ValueFactory> factories_{};
};

struct BuiltinLiterals {
    std::string base_symbol = "base";
    std::string version_field = "version";
    std::string backend_field = "backend";
    std::string defaults_field = "defaults";
    std::string profile_field = "profile";
    std::string opt_field = "opt";
};

BuiltinRegistry make_default_builtin_registry(const BuiltinLiterals& literals = {});

class Evaluator {
public:
    Evaluator(EvaluatorBudget budget,
              diag::Bag& diags,
              BuiltinRegistry builtins = make_default_builtin_registry(),
              parse::ParserControl parser_control = {})
        : budget_(budget),
          diags_(diags),
          builtins_(std::move(builtins)),
          parser_control_(parser_control) {}

    std::optional<Value> evaluate_entry(const std::filesystem::path& entry_path);

private:
    struct ModuleContext {
        std::string path;
        ast::Program program{};
        std::unordered_map<std::string, Value> symbols{};
        std::unordered_map<std::string, Value> exports{};
        std::optional<Value> build{};
        bool evaluated = false;
    };

    using ModulePtr = std::shared_ptr<ModuleContext>;

    ModulePtr load_module(const std::filesystem::path& path);
    ModulePtr evaluate_module(const std::filesystem::path& path);

    std::optional<Value> eval_expr(ModulePtr mod,
                                   const ast::Expr* expr,
                                   std::unordered_map<std::string, Value>& locals,
                                   uint32_t call_depth);

    std::optional<Value> eval_binary(ModulePtr mod,
                                     const ast::Expr* expr,
                                     std::unordered_map<std::string, Value>& locals,
                                     uint32_t call_depth);

    std::optional<Value> merge_objects(const Value& lhs, const Value& rhs, const ast::Span& span);
    std::optional<Value> overlay_defaults(const Value& lhs, const Value& rhs, const ast::Span& span);

    void step_or_budget_error(const ast::Span& span);
    void add_diag(diag::Code code, const ast::Span& span, std::string msg);

    EvaluatorBudget budget_{};
    diag::Bag& diags_;
    BuiltinRegistry builtins_{};
    parse::ParserControl parser_control_{};
    uint64_t steps_ = 0;
    uint64_t node_visits_ = 0;

    std::unordered_map<std::string, ModulePtr> module_cache_{};
    std::unordered_map<std::string, uint8_t> module_mark_{}; // 0=unseen,1=visiting,2=done
    std::vector<std::string> call_stack_{};
};

std::string to_string(const Value& v);

} // namespace lei::eval
