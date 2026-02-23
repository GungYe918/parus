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
    uint32_t max_loop_iters = 100000;
    uint32_t max_total_loop_steps = 200000;
    uint32_t max_tasks = 10000;
    uint32_t max_codegens = 10000;
};

using SourceOverlayLookup = std::function<std::optional<std::string>(std::string_view normalized_path)>;

struct EvaluateOptions {
    std::string entry_plan = "master";
    SourceOverlayLookup source_overlay{};
};

struct FunctionValue {
    std::string module_path;
    std::string name;
    std::vector<std::string> params;
    const ast::Block* body = nullptr;
};

struct BuiltinFunction;
struct TemplateSpec;
struct PatchValue;
struct DynamicObject;

struct Value {
    using Object = std::map<std::string, Value>;
    using Array = std::vector<Value>;
    using Function = FunctionValue;
    using NativeFunction = std::shared_ptr<BuiltinFunction>;
    using Template = std::shared_ptr<TemplateSpec>;
    using Patch = std::shared_ptr<PatchValue>;
    using Dynamic = std::shared_ptr<DynamicObject>;

    std::variant<int64_t, double, std::string, bool, Object, Array, Function, NativeFunction, Template, Patch, Dynamic> data;

    bool is_int() const { return std::holds_alternative<int64_t>(data); }
    bool is_float() const { return std::holds_alternative<double>(data); }
    bool is_string() const { return std::holds_alternative<std::string>(data); }
    bool is_bool() const { return std::holds_alternative<bool>(data); }
    bool is_object() const { return std::holds_alternative<Object>(data); }
    bool is_array() const { return std::holds_alternative<Array>(data); }
    bool is_function() const { return std::holds_alternative<Function>(data); }
    bool is_native_function() const { return std::holds_alternative<NativeFunction>(data); }
    bool is_template() const { return std::holds_alternative<Template>(data); }
    bool is_patch() const { return std::holds_alternative<Patch>(data); }
    bool is_dynamic_object() const { return std::holds_alternative<Dynamic>(data); }

    const Object* as_object() const { return std::get_if<Object>(&data); }
    Object* as_object() { return std::get_if<Object>(&data); }
    const Array* as_array() const { return std::get_if<Array>(&data); }
    Array* as_array() { return std::get_if<Array>(&data); }
    const DynamicObject* as_dynamic_object() const {
        auto p = std::get_if<Dynamic>(&data);
        if (!p) return nullptr;
        return p->get();
    }
};

struct DynamicObject {
    using MemberResolver = std::function<std::optional<Value>(std::string_view key,
                                                              const ast::Span& span,
                                                              diag::Bag& diags)>;
    using KeysProvider = std::function<std::vector<std::string>()>;

    std::string name{};
    MemberResolver resolve{};
    KeysProvider keys{};
};

struct BuiltinFunction {
    using Callback = std::function<std::optional<Value>(const std::vector<Value>& args,
                                                        const ast::Span& span,
                                                        diag::Bag& diags)>;

    std::string name{};
    Callback callback{};
};

struct SchemaType {
    enum class Kind : uint8_t {
        kAny,
        kInt,
        kFloat,
        kString,
        kBool,
        kObject,
        kArray,
    } kind = Kind::kAny;

    std::shared_ptr<SchemaType> element{};
};

struct TemplateField {
    SchemaType type{};
    bool required = true;
    std::optional<Value> default_value{};
};

struct TemplateSpec {
    using Validator = std::function<std::optional<std::string>(const Value::Object&)>;

    std::string name{};
    std::map<std::string, TemplateField> fields{};
    Validator validator{};
};

struct RuntimePathSegment {
    enum class Kind : uint8_t { kField, kIndex } kind = Kind::kField;
    std::string field{};
    int64_t index = 0;
};

struct PatchAssign {
    std::vector<RuntimePathSegment> path{};
    Value value{};
};

struct PatchValue {
    std::vector<PatchAssign> assigns{};
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

BuiltinRegistry make_default_builtin_registry();

class BuiltinPlanRegistry {
public:
    using TemplateFactory = std::function<std::shared_ptr<TemplateSpec>()>;

    void register_plan(std::string name, TemplateFactory factory);
    void inject_into(std::unordered_map<std::string, Value>& symbols) const;
    bool has_plan(std::string_view name) const;

private:
    std::unordered_map<std::string, TemplateFactory> factories_{};
};

BuiltinPlanRegistry make_default_builtin_plan_registry();

class Evaluator {
public:
    Evaluator(EvaluatorBudget budget,
              diag::Bag& diags,
              BuiltinRegistry builtins = make_default_builtin_registry(),
              BuiltinPlanRegistry builtin_plans = make_default_builtin_plan_registry(),
              parse::ParserControl parser_control = {})
        : budget_(budget),
          diags_(diags),
          builtins_(std::move(builtins)),
          builtin_plans_(std::move(builtin_plans)),
          parser_control_(parser_control) {}

    std::optional<Value> evaluate_entry(const std::filesystem::path& entry_path,
                                        EvaluateOptions options = {});

    std::vector<std::string> loaded_module_paths() const;

private:
    struct Variable {
        Value value{};
        bool mutable_binding = false;
    };

    struct ExecScope {
        std::unordered_map<std::string, Variable> vars{};
    };

    struct ExecState {
        std::vector<ExecScope> scopes{};
        bool in_function = false;
        bool returning = false;
        std::optional<Value> return_value{};
    };

    struct ModuleContext {
        std::string path;
        ast::Program program{};
        std::unordered_map<std::string, Variable> symbols{};
        std::unordered_map<std::string, Value> exports{};
        std::unordered_map<std::string, std::shared_ptr<ModuleContext>> aliases{};
        bool evaluated = false;
    };

    using ModulePtr = std::shared_ptr<ModuleContext>;

    ModulePtr load_module(const std::filesystem::path& path);
    ModulePtr evaluate_module(const std::filesystem::path& path);

    std::optional<Value> eval_expr(ModulePtr mod,
                                   const ast::Expr* expr,
                                   ExecState& st,
                                   uint32_t call_depth);

    bool exec_block(ModulePtr mod,
                    const ast::Block* block,
                    ExecState& st,
                    uint32_t call_depth,
                    bool push_scope);

    bool exec_stmt(ModulePtr mod,
                   const ast::Stmt& stmt,
                   ExecState& st,
                   uint32_t call_depth);

    std::optional<Value> eval_binary(ModulePtr mod,
                                     const ast::Expr* expr,
                                     ExecState& st,
                                     uint32_t call_depth);

    std::optional<Value> apply_template(const TemplateSpec& spec,
                                        const Value& rhs,
                                        const ast::Span& span);

    std::optional<Value> merge_values(const Value& lhs,
                                      const Value& rhs,
                                      const ast::Span& span,
                                      std::string path);

    std::optional<Value> apply_patch_to_value(const Value& base,
                                              const PatchValue& patch,
                                              const ast::Span& span);

    std::optional<PatchAssign> eval_patch_assign(ModulePtr mod,
                                                 const ast::PlanAssign& assign,
                                                 ExecState& st,
                                                 uint32_t call_depth);

    std::optional<Variable*> lookup_variable(ModulePtr mod,
                                             ExecState& st,
                                             std::string_view name);

    std::optional<Value> lookup_value(ModulePtr mod,
                                      ExecState& st,
                                      std::string_view name,
                                      const ast::Span& span);

    bool assign_value(ModulePtr mod,
                      ExecState& st,
                      const ast::Path& path,
                      Value value,
                      const ast::Span& span,
                      bool strict_existing_root,
                      uint32_t call_depth);

    bool validate_schema_type(const Value& value,
                              const SchemaType& ty,
                              std::string path,
                              const ast::Span& span,
                              diag::Code mismatch_code);

    void step_or_budget_error(const ast::Span& span);
    void add_diag(diag::Code code, const ast::Span& span, std::string msg);

    EvaluatorBudget budget_{};
    diag::Bag& diags_;
    BuiltinRegistry builtins_{};
    BuiltinPlanRegistry builtin_plans_{};
    parse::ParserControl parser_control_{};
    EvaluateOptions options_{};

    uint64_t steps_ = 0;
    uint64_t node_visits_ = 0;
    uint64_t total_loop_steps_ = 0;

    std::unordered_map<std::string, ModulePtr> module_cache_{};
    std::unordered_map<std::string, uint8_t> module_mark_{}; // 0=unseen,1=visiting,2=done
    std::vector<std::string> call_stack_{};
};

std::string to_string(const Value& v);

} // namespace lei::eval
