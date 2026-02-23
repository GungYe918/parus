#include <lei/eval/Evaluator.hpp>

#include <lei/os/File.hpp>
#include <lei/parse/Parser.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <sstream>

namespace lei::eval {

namespace {

bool numeric_promote(const Value& v, double& out) {
    if (auto p = std::get_if<int64_t>(&v.data)) {
        out = static_cast<double>(*p);
        return true;
    }
    if (auto p = std::get_if<double>(&v.data)) {
        out = *p;
        return true;
    }
    return false;
}

bool is_builtin_template_name(std::string_view n) {
    return n == "bundle" || n == "master" || n == "task" || n == "codegen";
}

SchemaType make_scalar_schema(SchemaType::Kind k) {
    SchemaType ty{};
    ty.kind = k;
    return ty;
}

SchemaType make_array_schema(SchemaType elem) {
    SchemaType ty{};
    ty.kind = SchemaType::Kind::kArray;
    ty.element = std::make_shared<SchemaType>(std::move(elem));
    return ty;
}

SchemaType schema_from_type_node(const ast::TypeNode& ty) {
    using TK = ast::TypeNode::Kind;
    switch (ty.kind) {
        case TK::kInt:
            return make_scalar_schema(SchemaType::Kind::kInt);
        case TK::kFloat:
            return make_scalar_schema(SchemaType::Kind::kFloat);
        case TK::kString:
            return make_scalar_schema(SchemaType::Kind::kString);
        case TK::kBool:
            return make_scalar_schema(SchemaType::Kind::kBool);
        case TK::kArray: {
            if (!ty.element) {
                return make_array_schema(make_scalar_schema(SchemaType::Kind::kAny));
            }
            return make_array_schema(schema_from_type_node(*ty.element));
        }
    }
    return make_scalar_schema(SchemaType::Kind::kAny);
}

bool schema_equal(const SchemaType& a, const SchemaType& b) {
    if (a.kind != b.kind) return false;
    if (a.kind != SchemaType::Kind::kArray) return true;
    if (!a.element && !b.element) return true;
    if (!a.element || !b.element) return false;
    return schema_equal(*a.element, *b.element);
}

std::string schema_name(const SchemaType& ty) {
    switch (ty.kind) {
        case SchemaType::Kind::kAny:
            return "any";
        case SchemaType::Kind::kInt:
            return "int";
        case SchemaType::Kind::kFloat:
            return "float";
        case SchemaType::Kind::kString:
            return "string";
        case SchemaType::Kind::kBool:
            return "bool";
        case SchemaType::Kind::kObject:
            return "object";
        case SchemaType::Kind::kArray:
            return "[" + (ty.element ? schema_name(*ty.element) : std::string("any")) + "]";
    }
    return "unknown";
}

Value make_string(std::string s) {
    Value v;
    v.data = std::move(s);
    return v;
}

Value make_bool(bool b) {
    Value v;
    v.data = b;
    return v;
}

Value make_object(std::map<std::string, Value> obj = {}) {
    Value v;
    v.data = std::move(obj);
    return v;
}

Value make_array(std::vector<Value> arr = {}) {
    Value v;
    v.data = std::move(arr);
    return v;
}

bool value_equal_strict(const Value& a, const Value& b) {
    if (a.data.index() != b.data.index()) return false;

    if (auto x = std::get_if<int64_t>(&a.data)) return *x == std::get<int64_t>(b.data);
    if (auto x = std::get_if<double>(&a.data)) return *x == std::get<double>(b.data);
    if (auto x = std::get_if<std::string>(&a.data)) return *x == std::get<std::string>(b.data);
    if (auto x = std::get_if<bool>(&a.data)) return *x == std::get<bool>(b.data);

    if (auto x = std::get_if<Value::Array>(&a.data)) {
        const auto& y = std::get<Value::Array>(b.data);
        if (x->size() != y.size()) return false;
        for (size_t i = 0; i < x->size(); ++i) {
            if (!value_equal_strict((*x)[i], y[i])) return false;
        }
        return true;
    }

    if (auto x = std::get_if<Value::Object>(&a.data)) {
        const auto& y = std::get<Value::Object>(b.data);
        if (x->size() != y.size()) return false;
        for (const auto& [k, xv] : *x) {
            auto it = y.find(k);
            if (it == y.end()) return false;
            if (!value_equal_strict(xv, it->second)) return false;
        }
        return true;
    }

    return false;
}

Value default_container_for_next(const RuntimePathSegment* next) {
    if (!next) return make_object();
    if (next->kind == RuntimePathSegment::Kind::kIndex) return make_array();
    return make_object();
}

bool apply_runtime_path(Value& root,
                        const std::vector<RuntimePathSegment>& path,
                        const Value& value,
                        diag::Bag& diags,
                        const ast::Span& span,
                        diag::Code code) {
    if (path.empty()) {
        diags.add(code, span.file, span.line, span.column, "empty path assignment is not allowed");
        return false;
    }

    Value* cur = &root;
    for (size_t i = 0; i < path.size(); ++i) {
        const bool is_last = (i + 1 == path.size());
        const RuntimePathSegment* next = is_last ? nullptr : &path[i + 1];

        if (path[i].kind == RuntimePathSegment::Kind::kField) {
            if (!cur->is_object()) {
                cur->data = Value::Object{};
            }
            auto* obj = cur->as_object();
            auto it = obj->find(path[i].field);

            if (is_last) {
                (*obj)[path[i].field] = value;
                return true;
            }

            if (it == obj->end()) {
                (*obj)[path[i].field] = default_container_for_next(next);
                it = obj->find(path[i].field);
            } else {
                if (next && next->kind == RuntimePathSegment::Kind::kField && !it->second.is_object()) {
                    it->second = make_object();
                }
                if (next && next->kind == RuntimePathSegment::Kind::kIndex && !it->second.is_array()) {
                    it->second = make_array();
                }
            }

            cur = &it->second;
            continue;
        }

        if (path[i].index < 0) {
            diags.add(code,
                      span.file,
                      span.line,
                      span.column,
                      "negative index in path assignment");
            return false;
        }

        if (!cur->is_array()) {
            cur->data = Value::Array{};
        }

        auto* arr = cur->as_array();
        const size_t idx = static_cast<size_t>(path[i].index);
        if (arr->size() <= idx) {
            arr->resize(idx + 1, default_container_for_next(next));
        }

        if (is_last) {
            (*arr)[idx] = value;
            return true;
        }

        auto& slot = (*arr)[idx];
        if (next && next->kind == RuntimePathSegment::Kind::kField && !slot.is_object()) {
            slot = make_object();
        }
        if (next && next->kind == RuntimePathSegment::Kind::kIndex && !slot.is_array()) {
            slot = make_array();
        }

        cur = &slot;
    }

    return true;
}

std::optional<std::string> validate_bundle(const Value::Object& obj) {
    auto it = obj.find("sources");
    if (it == obj.end() || !it->second.is_array()) {
        return std::string("bundle.sources must be [string]");
    }
    const auto& arr = std::get<Value::Array>(it->second.data);
    if (arr.empty()) {
        return std::string("bundle.sources must not be empty");
    }
    return std::nullopt;
}

std::optional<std::string> validate_task(const Value::Object& obj) {
    auto it = obj.find("run");
    if (it == obj.end() || !it->second.is_array()) {
        return std::string("task.run must be [string]");
    }
    const auto& arr = std::get<Value::Array>(it->second.data);
    if (arr.empty()) {
        return std::string("task.run must not be empty");
    }
    return std::nullopt;
}

std::optional<std::string> validate_codegen(const Value::Object& obj) {
    auto out_it = obj.find("outputs");
    auto in_it = obj.find("inputs");
    if (out_it == obj.end() || !out_it->second.is_array()) {
        return std::string("codegen.outputs must be [string]");
    }
    const auto& outs = std::get<Value::Array>(out_it->second.data);
    if (outs.empty()) {
        return std::string("codegen.outputs must not be empty");
    }

    if (in_it != obj.end() && in_it->second.is_array()) {
        const auto& ins = std::get<Value::Array>(in_it->second.data);
        for (const auto& iv : ins) {
            auto isp = std::get_if<std::string>(&iv.data);
            if (!isp) continue;
            for (const auto& ov : outs) {
                auto osp = std::get_if<std::string>(&ov.data);
                if (osp && *osp == *isp) {
                    return std::string("codegen.inputs/outputs path conflict: ") + *osp;
                }
            }
        }
    }

    return std::nullopt;
}

std::shared_ptr<TemplateSpec> make_bundle_template() {
    auto spec = std::make_shared<TemplateSpec>();
    spec->name = "bundle";

    spec->fields["name"] = TemplateField{make_scalar_schema(SchemaType::Kind::kString), true, std::nullopt};
    spec->fields["kind"] = TemplateField{make_scalar_schema(SchemaType::Kind::kString), true, std::nullopt};
    spec->fields["sources"] = TemplateField{make_array_schema(make_scalar_schema(SchemaType::Kind::kString)), true, std::nullopt};
    spec->fields["deps"] = TemplateField{make_array_schema(make_scalar_schema(SchemaType::Kind::kString)), true, std::optional<Value>{make_array({})}};
    spec->validator = validate_bundle;
    return spec;
}

std::shared_ptr<TemplateSpec> make_master_template() {
    auto spec = std::make_shared<TemplateSpec>();
    spec->name = "master";

    spec->fields["project"] = TemplateField{make_scalar_schema(SchemaType::Kind::kObject), true, std::nullopt};
    spec->fields["bundles"] = TemplateField{make_array_schema(make_scalar_schema(SchemaType::Kind::kObject)), false, std::optional<Value>{make_array({})}};
    spec->fields["tasks"] = TemplateField{make_array_schema(make_scalar_schema(SchemaType::Kind::kObject)), false, std::optional<Value>{make_array({})}};
    spec->fields["codegens"] = TemplateField{make_array_schema(make_scalar_schema(SchemaType::Kind::kObject)), false, std::optional<Value>{make_array({})}};

    return spec;
}

std::shared_ptr<TemplateSpec> make_task_template() {
    auto spec = std::make_shared<TemplateSpec>();
    spec->name = "task";

    spec->fields["name"] = TemplateField{make_scalar_schema(SchemaType::Kind::kString), true, std::nullopt};
    spec->fields["run"] = TemplateField{make_array_schema(make_scalar_schema(SchemaType::Kind::kString)), true, std::nullopt};
    spec->fields["deps"] = TemplateField{make_array_schema(make_scalar_schema(SchemaType::Kind::kString)), false, std::optional<Value>{make_array({})}};
    spec->fields["cwd"] = TemplateField{make_scalar_schema(SchemaType::Kind::kString), false, std::optional<Value>{make_string(".")}};
    spec->fields["inputs"] = TemplateField{make_array_schema(make_scalar_schema(SchemaType::Kind::kString)), false, std::optional<Value>{make_array({})}};
    spec->fields["outputs"] = TemplateField{make_array_schema(make_scalar_schema(SchemaType::Kind::kString)), false, std::optional<Value>{make_array({})}};
    spec->fields["always_run"] = TemplateField{make_scalar_schema(SchemaType::Kind::kBool), false, std::optional<Value>{make_bool(false)}};
    spec->validator = validate_task;
    return spec;
}

std::shared_ptr<TemplateSpec> make_codegen_template() {
    auto spec = std::make_shared<TemplateSpec>();
    spec->name = "codegen";

    spec->fields["name"] = TemplateField{make_scalar_schema(SchemaType::Kind::kString), true, std::nullopt};
    spec->fields["tool"] = TemplateField{make_array_schema(make_scalar_schema(SchemaType::Kind::kString)), true, std::nullopt};
    spec->fields["inputs"] = TemplateField{make_array_schema(make_scalar_schema(SchemaType::Kind::kString)), true, std::nullopt};
    spec->fields["outputs"] = TemplateField{make_array_schema(make_scalar_schema(SchemaType::Kind::kString)), true, std::nullopt};
    spec->fields["args"] = TemplateField{make_array_schema(make_scalar_schema(SchemaType::Kind::kString)), false, std::optional<Value>{make_array({})}};
    spec->fields["deps"] = TemplateField{make_array_schema(make_scalar_schema(SchemaType::Kind::kString)), false, std::optional<Value>{make_array({})}};
    spec->fields["cwd"] = TemplateField{make_scalar_schema(SchemaType::Kind::kString), false, std::optional<Value>{make_string(".")}};
    spec->fields["deterministic"] = TemplateField{make_scalar_schema(SchemaType::Kind::kBool), false, std::optional<Value>{make_bool(true)}};
    spec->validator = validate_codegen;
    return spec;
}

} // namespace

void BuiltinRegistry::register_value(std::string name, ValueFactory factory) {
    factories_[std::move(name)] = std::move(factory);
}

void BuiltinRegistry::register_native_function(std::string name, BuiltinFunction::Callback callback) {
    std::string stable_name = std::move(name);
    factories_[stable_name] = [stable_name, callback = std::move(callback)]() mutable {
        auto fn = std::make_shared<BuiltinFunction>();
        fn->name = stable_name;
        fn->callback = callback;

        Value v;
        v.data = std::move(fn);
        return v;
    };
}

void BuiltinRegistry::inject_into(std::unordered_map<std::string, Value>& symbols) const {
    for (const auto& [name, factory] : factories_) {
        symbols[name] = factory();
    }
}

bool BuiltinRegistry::has_symbol(std::string_view name) const {
    return factories_.find(std::string(name)) != factories_.end();
}

BuiltinRegistry make_default_builtin_registry() {
    BuiltinRegistry reg;
    return reg;
}

void BuiltinPlanRegistry::register_plan(std::string name, TemplateFactory factory) {
    factories_[std::move(name)] = std::move(factory);
}

void BuiltinPlanRegistry::inject_into(std::unordered_map<std::string, Value>& symbols) const {
    for (const auto& [name, factory] : factories_) {
        Value v;
        v.data = factory();
        symbols[name] = std::move(v);
    }
}

bool BuiltinPlanRegistry::has_plan(std::string_view name) const {
    return factories_.find(std::string(name)) != factories_.end();
}

BuiltinPlanRegistry make_default_builtin_plan_registry() {
    BuiltinPlanRegistry reg;
    reg.register_plan("bundle", make_bundle_template);
    reg.register_plan("master", make_master_template);
    reg.register_plan("task", make_task_template);
    reg.register_plan("codegen", make_codegen_template);
    return reg;
}

Evaluator::ModulePtr Evaluator::load_module(const std::filesystem::path& path) {
    const std::string key = lei::os::normalize_path(path.string());
    auto it = module_cache_.find(key);
    if (it != module_cache_.end()) return it->second;

    const auto read = lei::os::read_text_file(key);
    if (!read.ok) {
        diags_.add(diag::Code::L_IMPORT_NOT_FOUND, key, 1, 1, "cannot open LEI module: " + read.err);
        return nullptr;
    }

    auto mod = std::make_shared<ModuleContext>();
    mod->path = key;
    mod->program = parse::parse_source(read.text, key, diags_, parser_control_);

    module_cache_[key] = mod;
    return mod;
}

Evaluator::ModulePtr Evaluator::evaluate_module(const std::filesystem::path& path) {
    auto mod = load_module(path);
    if (!mod) return nullptr;

    const std::string key = mod->path;
    const uint8_t mark = module_mark_[key];
    if (mark == 1) {
        diags_.add(diag::Code::L_IMPORT_CYCLE, key, 1, 1, "import cycle detected: " + key);
        return nullptr;
    }
    if (mark == 2 && mod->evaluated) {
        return mod;
    }

    module_mark_[key] = 1;

    std::unordered_map<std::string, Value> seed{};
    builtins_.inject_into(seed);
    builtin_plans_.inject_into(seed);
    for (auto& [name, val] : seed) {
        mod->symbols[name] = Variable{val, false};
    }

    // Predeclare def for forward references.
    for (const auto& it : mod->program.items) {
        if (it.kind != ast::ItemKind::kDef) continue;
        Value fv{};
        std::vector<std::string> params;
        params.reserve(it.def.params.size());
        for (const auto& p : it.def.params) params.push_back(p.name);
        fv.data = FunctionValue{mod->path, it.def.name, std::move(params), it.def.body.get()};
        mod->symbols[it.def.name] = Variable{fv, false};
    }

    // Evaluate in order.
    std::vector<std::pair<std::string, ast::Span>> export_plan_refs;

    for (const auto& it : mod->program.items) {
        if (diags_.has_error()) return nullptr;

        switch (it.kind) {
            case ast::ItemKind::kImportAlias: {
                const std::string import_path = lei::os::resolve_relative_path(mod->path, it.import_alias.from_path);
                auto imported = evaluate_module(import_path);
                if (!imported) return nullptr;
                mod->aliases[it.import_alias.alias] = imported;
                break;
            }
            case ast::ItemKind::kProto: {
                auto spec = std::make_shared<TemplateSpec>();
                spec->name = it.proto.name;

                ExecState st{};
                st.scopes.push_back({});

                for (const auto& f : it.proto.fields) {
                    TemplateField tf{};
                    tf.type = schema_from_type_node(f.type);
                    tf.required = (f.default_value == nullptr);
                    if (f.default_value) {
                        auto dv = eval_expr(mod, f.default_value.get(), st, 0);
                        if (!dv) return nullptr;
                        tf.default_value = *dv;
                    }
                    spec->fields[f.name] = std::move(tf);
                }

                Value v{};
                v.data = spec;
                mod->symbols[it.proto.name] = Variable{v, false};
                break;
            }
            case ast::ItemKind::kPlan:
            case ast::ItemKind::kExportPlan: {
                ExecState st{};
                st.scopes.push_back({});

                Value plan_value{};
                if (it.plan.is_expr_form) {
                    auto v = eval_expr(mod, it.plan.expr.get(), st, 0);
                    if (!v) return nullptr;
                    plan_value = *v;
                } else {
                    Value::Object root{};
                    Value root_val{};
                    root_val.data = root;
                    for (const auto& a : it.plan.body_items) {
                        auto pa = eval_patch_assign(mod, a, st, 0);
                        if (!pa) return nullptr;
                        if (!apply_runtime_path(root_val,
                                                pa->path,
                                                pa->value,
                                                diags_,
                                                it.span,
                                                diag::Code::L_TYPE_MISMATCH)) {
                            return nullptr;
                        }
                    }
                    plan_value = root_val;
                }

                mod->symbols[it.plan.name] = Variable{plan_value, false};

                if (it.kind == ast::ItemKind::kExportPlan) {
                    if (it.plan.name == "master") {
                        add_diag(diag::Code::L_MASTER_EXPORT_FORBIDDEN,
                                 it.span,
                                 "engine policy forbids exporting plan 'master'");
                        return nullptr;
                    }
                    mod->exports[it.plan.name] = plan_value;
                }
                break;
            }
            case ast::ItemKind::kExportPlanRef: {
                if (it.export_plan_ref == "master") {
                    add_diag(diag::Code::L_MASTER_EXPORT_FORBIDDEN,
                             it.span,
                             "engine policy forbids exporting plan 'master'");
                    return nullptr;
                }
                export_plan_refs.push_back({it.export_plan_ref, it.span});
                break;
            }
            case ast::ItemKind::kLet:
            case ast::ItemKind::kVar: {
                ExecState st{};
                st.scopes.push_back({});
                auto v = eval_expr(mod, it.binding.value.get(), st, 0);
                if (!v) return nullptr;
                mod->symbols[it.binding.name] = Variable{*v, it.kind == ast::ItemKind::kVar};
                break;
            }
            case ast::ItemKind::kDef:
                break;

            case ast::ItemKind::kAssert: {
                ExecState st{};
                st.scopes.push_back({});
                auto v = eval_expr(mod, it.expr.get(), st, 0);
                if (!v) return nullptr;
                if (!v->is_bool() || !std::get<bool>(v->data)) {
                    add_diag(diag::Code::L_TYPE_MISMATCH, it.span, "assert expects true bool expression");
                    return nullptr;
                }
                break;
            }
        }
    }

    for (const auto& [name, sp] : export_plan_refs) {
        auto it = mod->symbols.find(name);
        if (it == mod->symbols.end()) {
            add_diag(diag::Code::L_EXPORT_PLAN_NOT_FOUND, sp, "export plan reference not found: " + name);
            return nullptr;
        }
        mod->exports[name] = it->second.value;
    }

    mod->evaluated = true;
    module_mark_[key] = 2;
    return mod;
}

std::optional<Value> Evaluator::evaluate_entry(const std::filesystem::path& entry_path,
                                               EvaluateOptions options) {
    options_ = std::move(options);
    steps_ = 0;
    node_visits_ = 0;
    total_loop_steps_ = 0;
    module_cache_.clear();
    module_mark_.clear();
    call_stack_.clear();

    auto mod = evaluate_module(entry_path);
    if (!mod) return std::nullopt;
    if (diags_.has_error()) return std::nullopt;

    auto it = mod->symbols.find(options_.entry_plan);
    if (it == mod->symbols.end()) {
        ast::Span sp{mod->path, 1, 1};
        add_diag(diag::Code::L_PLAN_NOT_FOUND,
                 sp,
                 "entry plan not found: " + options_.entry_plan);
        return std::nullopt;
    }

    auto root = std::get_if<Value::Object>(&it->second.value.data);
    if (root) {
        auto build_it = root->find("build");
        if (build_it != root->end()) {
            add_diag(diag::Code::L_LEGACY_EXPLICIT_GRAPH_REMOVED,
                     ast::Span{mod->path, 1, 1},
                     "explicit graph declaration via 'build' is removed; declare graph fields at entry plan root");
            return std::nullopt;
        }
    }

    return it->second.value;
}

void Evaluator::step_or_budget_error(const ast::Span& span) {
    ++steps_;
    ++node_visits_;
    if (steps_ > budget_.max_steps || node_visits_ > budget_.max_nodes) {
        add_diag(diag::Code::L_BUDGET_EXCEEDED, span, "evaluation budget exceeded");
    }
}

void Evaluator::add_diag(diag::Code code, const ast::Span& span, std::string msg) {
    diags_.add(code, span.file, span.line, span.column, std::move(msg));
}

bool Evaluator::validate_schema_type(const Value& value,
                                     const SchemaType& ty,
                                     std::string path,
                                     const ast::Span& span,
                                     diag::Code mismatch_code) {
    switch (ty.kind) {
        case SchemaType::Kind::kAny:
            return true;
        case SchemaType::Kind::kInt:
            if (value.is_int()) return true;
            break;
        case SchemaType::Kind::kFloat:
            if (value.is_float()) return true;
            break;
        case SchemaType::Kind::kString:
            if (value.is_string()) return true;
            break;
        case SchemaType::Kind::kBool:
            if (value.is_bool()) return true;
            break;
        case SchemaType::Kind::kObject:
            if (value.is_object()) return true;
            break;
        case SchemaType::Kind::kArray: {
            if (!value.is_array()) break;
            if (!ty.element) return true;
            const auto& arr = std::get<Value::Array>(value.data);
            for (size_t i = 0; i < arr.size(); ++i) {
                if (!validate_schema_type(arr[i],
                                          *ty.element,
                                          path + "[" + std::to_string(i) + "]",
                                          span,
                                          mismatch_code)) {
                    return false;
                }
            }
            return true;
        }
    }

    add_diag(mismatch_code,
             span,
             "schema type mismatch at '" + path + "': expected " + schema_name(ty));
    return false;
}

std::optional<Value> Evaluator::apply_template(const TemplateSpec& spec,
                                               const Value& rhs,
                                               const ast::Span& span) {
    Value base_obj{};

    if (rhs.is_object()) {
        base_obj = rhs;
    } else if (rhs.is_patch()) {
        Value empty{};
        empty.data = Value::Object{};
        auto patched = apply_patch_to_value(empty, *std::get<Value::Patch>(rhs.data), span);
        if (!patched) return std::nullopt;
        base_obj = *patched;
    } else {
        const auto code = is_builtin_template_name(spec.name)
                              ? diag::Code::L_BUILTIN_PLAN_SCHEMA_VIOLATION
                              : diag::Code::L_PROTO_TYPE_MISMATCH;
        add_diag(code,
                 span,
                 "template '" + spec.name + "' requires object/patch operand");
        return std::nullopt;
    }

    auto* obj = base_obj.as_object();
    if (!obj) {
        add_diag(diag::Code::L_TYPE_MISMATCH, span, "template application requires object value");
        return std::nullopt;
    }

    const auto missing_code = is_builtin_template_name(spec.name)
                                  ? diag::Code::L_BUILTIN_PLAN_SCHEMA_VIOLATION
                                  : diag::Code::L_PROTO_REQUIRED_FIELD_MISSING;
    const auto mismatch_code = is_builtin_template_name(spec.name)
                                   ? diag::Code::L_BUILTIN_PLAN_SCHEMA_VIOLATION
                                   : diag::Code::L_PROTO_TYPE_MISMATCH;

    for (const auto& [name, field] : spec.fields) {
        auto it = obj->find(name);
        if (it == obj->end()) {
            if (field.default_value.has_value()) {
                (*obj)[name] = *field.default_value;
                continue;
            }
            if (field.required) {
                add_diag(missing_code,
                         span,
                         "required field missing in template '" + spec.name + "': " + name);
                return std::nullopt;
            }
            continue;
        }

        if (!validate_schema_type(it->second,
                                  field.type,
                                  name,
                                  span,
                                  mismatch_code)) {
            return std::nullopt;
        }
    }

    if (spec.validator) {
        auto err = spec.validator(*obj);
        if (err.has_value()) {
            add_diag(is_builtin_template_name(spec.name)
                         ? diag::Code::L_BUILTIN_PLAN_SCHEMA_VIOLATION
                         : diag::Code::L_PROTO_TYPE_MISMATCH,
                     span,
                     "template '" + spec.name + "' validation failed: " + *err);
            return std::nullopt;
        }
    }

    return base_obj;
}

std::optional<Value> Evaluator::apply_patch_to_value(const Value& base,
                                                     const PatchValue& patch,
                                                     const ast::Span& span) {
    Value out = base;
    if (!out.is_object()) {
        out.data = Value::Object{};
    }

    for (const auto& a : patch.assigns) {
        if (!apply_runtime_path(out,
                                a.path,
                                a.value,
                                diags_,
                                span,
                                diag::Code::L_TYPE_MISMATCH)) {
            return std::nullopt;
        }
    }

    return out;
}

std::optional<Value> Evaluator::merge_values(const Value& lhs,
                                             const Value& rhs,
                                             const ast::Span& span,
                                             std::string path) {
    if (lhs.is_template() && rhs.is_template()) {
        auto l = std::get<Value::Template>(lhs.data);
        auto r = std::get<Value::Template>(rhs.data);
        if (!l || !r) {
            add_diag(diag::Code::L_MERGE_CONFLICT, span, "invalid template value");
            return std::nullopt;
        }

        auto merged = std::make_shared<TemplateSpec>();
        merged->name = l->name + "&" + r->name;
        merged->fields = l->fields;

        for (const auto& [name, rf] : r->fields) {
            auto it = merged->fields.find(name);
            if (it == merged->fields.end()) {
                merged->fields[name] = rf;
                continue;
            }

            auto& lf = it->second;
            if (!schema_equal(lf.type, rf.type)) {
                add_diag(diag::Code::L_MERGE_CONFLICT,
                         span,
                         "template field type conflict on '" + name + "'");
                return std::nullopt;
            }

            lf.required = lf.required || rf.required;

            if (lf.default_value.has_value() && rf.default_value.has_value()) {
                if (!value_equal_strict(*lf.default_value, *rf.default_value)) {
                    add_diag(diag::Code::L_MERGE_CONFLICT,
                             span,
                             "template field default conflict on '" + name + "'");
                    return std::nullopt;
                }
            } else if (!lf.default_value.has_value() && rf.default_value.has_value()) {
                lf.default_value = rf.default_value;
            }
        }

        if (l->validator && r->validator) {
            auto lv = l->validator;
            auto rv = r->validator;
            merged->validator = [lv, rv](const Value::Object& obj) -> std::optional<std::string> {
                if (auto e = lv(obj); e.has_value()) return e;
                return rv(obj);
            };
        } else if (l->validator) {
            merged->validator = l->validator;
        } else if (r->validator) {
            merged->validator = r->validator;
        }

        Value out{};
        out.data = merged;
        return out;
    }

    if (lhs.is_template()) {
        auto spec = std::get<Value::Template>(lhs.data);
        return apply_template(*spec, rhs, span);
    }
    if (rhs.is_template()) {
        auto spec = std::get<Value::Template>(rhs.data);
        return apply_template(*spec, lhs, span);
    }

    if (lhs.is_patch()) {
        Value empty{};
        empty.data = Value::Object{};
        auto lv = apply_patch_to_value(empty, *std::get<Value::Patch>(lhs.data), span);
        if (!lv) return std::nullopt;
        return merge_values(*lv, rhs, span, std::move(path));
    }

    if (rhs.is_patch()) {
        return apply_patch_to_value(lhs, *std::get<Value::Patch>(rhs.data), span);
    }

    if (lhs.is_object() && rhs.is_object()) {
        Value::Object out = std::get<Value::Object>(lhs.data);
        const auto& ro = std::get<Value::Object>(rhs.data);

        for (const auto& [k, rv] : ro) {
            auto it = out.find(k);
            const std::string child_path = path.empty() ? k : (path + "." + k);
            if (it == out.end()) {
                out[k] = rv;
                continue;
            }

            auto mv = merge_values(it->second, rv, span, child_path);
            if (!mv) return std::nullopt;
            it->second = *mv;
        }

        return make_object(std::move(out));
    }

    if (lhs.is_array() && rhs.is_array()) {
        const auto& la = std::get<Value::Array>(lhs.data);
        const auto& ra = std::get<Value::Array>(rhs.data);
        if (la.size() != ra.size()) {
            add_diag(diag::Code::L_MERGE_CONFLICT,
                     span,
                     "array merge conflict at '" + path + "': length mismatch");
            return std::nullopt;
        }

        Value::Array out;
        out.reserve(la.size());
        for (size_t i = 0; i < la.size(); ++i) {
            auto mv = merge_values(la[i],
                                   ra[i],
                                   span,
                                   path + "[" + std::to_string(i) + "]");
            if (!mv) return std::nullopt;
            out.push_back(*mv);
        }
        return make_array(std::move(out));
    }

    if (value_equal_strict(lhs, rhs)) return lhs;

    const std::string loc = path.empty() ? "<root>" : path;
    add_diag(diag::Code::L_MERGE_CONFLICT,
             span,
             "strict merge conflict at '" + loc + "'");
    return std::nullopt;
}

std::optional<Evaluator::Variable*> Evaluator::lookup_variable(ModulePtr mod,
                                                               ExecState& st,
                                                               std::string_view name) {
    for (auto it = st.scopes.rbegin(); it != st.scopes.rend(); ++it) {
        auto vit = it->vars.find(std::string(name));
        if (vit != it->vars.end()) return &vit->second;
    }

    auto git = mod->symbols.find(std::string(name));
    if (git != mod->symbols.end()) return &git->second;

    return std::nullopt;
}

std::optional<Value> Evaluator::lookup_value(ModulePtr mod,
                                             ExecState& st,
                                             std::string_view name,
                                             const ast::Span& span) {
    auto v = lookup_variable(mod, st, name);
    if (!v.has_value()) {
        add_diag(diag::Code::L_UNKNOWN_IDENTIFIER, span, "unknown identifier: " + std::string(name));
        return std::nullopt;
    }
    return v.value()->value;
}

std::optional<PatchAssign> Evaluator::eval_patch_assign(ModulePtr mod,
                                                        const ast::PlanAssign& assign,
                                                        ExecState& st,
                                                        uint32_t call_depth) {
    PatchAssign out{};

    for (const auto& seg : assign.path.segments) {
        RuntimePathSegment rs{};
        rs.kind = (seg.kind == ast::PathSegment::Kind::kField)
                      ? RuntimePathSegment::Kind::kField
                      : RuntimePathSegment::Kind::kIndex;
        rs.field = seg.field;

        if (seg.kind == ast::PathSegment::Kind::kIndex) {
            auto idx_v = eval_expr(mod, seg.index.get(), st, call_depth);
            if (!idx_v) return std::nullopt;
            if (!idx_v->is_int()) {
                add_diag(diag::Code::L_TYPE_MISMATCH,
                         assign.value->span,
                         "path index must evaluate to int");
                return std::nullopt;
            }
            rs.index = std::get<int64_t>(idx_v->data);
            if (rs.index < 0) {
                add_diag(diag::Code::L_TYPE_MISMATCH,
                         assign.value->span,
                         "path index must be non-negative");
                return std::nullopt;
            }
        }

        out.path.push_back(std::move(rs));
    }

    auto v = eval_expr(mod, assign.value.get(), st, call_depth);
    if (!v) return std::nullopt;
    out.value = *v;
    return out;
}

bool Evaluator::assign_value(ModulePtr mod,
                             ExecState& st,
                             const ast::Path& path,
                             Value value,
                             const ast::Span& span,
                             bool strict_existing_root,
                             uint32_t call_depth) {
    if (path.segments.empty()) {
        add_diag(diag::Code::L_TYPE_MISMATCH, span, "assignment path is empty");
        return false;
    }

    if (path.segments[0].kind != ast::PathSegment::Kind::kField) {
        add_diag(diag::Code::L_TYPE_MISMATCH, span, "assignment root must be identifier");
        return false;
    }

    const std::string root_name = path.segments[0].field;
    auto root_var = lookup_variable(mod, st, root_name);
    if (!root_var.has_value()) {
        if (strict_existing_root) {
            add_diag(diag::Code::L_UNKNOWN_IDENTIFIER, span, "unknown assignment root: " + root_name);
            return false;
        }

        if (st.scopes.empty()) st.scopes.push_back({});
        st.scopes.back().vars[root_name] = Variable{make_object(), true};
        root_var = lookup_variable(mod, st, root_name);
    }

    Variable* var = *root_var;
    if (!var->mutable_binding) {
        add_diag(diag::Code::L_TYPE_MISMATCH, span, "cannot assign to immutable binding: " + root_name);
        return false;
    }

    if (path.segments.size() == 1) {
        var->value = std::move(value);
        return true;
    }

    std::vector<RuntimePathSegment> runtime_path;
    runtime_path.reserve(path.segments.size() - 1);

    for (size_t i = 1; i < path.segments.size(); ++i) {
        RuntimePathSegment rs{};
        rs.kind = (path.segments[i].kind == ast::PathSegment::Kind::kField)
                      ? RuntimePathSegment::Kind::kField
                      : RuntimePathSegment::Kind::kIndex;
        rs.field = path.segments[i].field;

        if (path.segments[i].kind == ast::PathSegment::Kind::kIndex) {
            auto idx_v = eval_expr(mod, path.segments[i].index.get(), st, call_depth);
            if (!idx_v) return false;
            if (!idx_v->is_int()) {
                add_diag(diag::Code::L_TYPE_MISMATCH, span, "assignment index must be int");
                return false;
            }
            rs.index = std::get<int64_t>(idx_v->data);
            if (rs.index < 0) {
                add_diag(diag::Code::L_TYPE_MISMATCH, span, "assignment index must be non-negative");
                return false;
            }
        }

        runtime_path.push_back(std::move(rs));
    }

    if (!apply_runtime_path(var->value,
                            runtime_path,
                            value,
                            diags_,
                            span,
                            diag::Code::L_TYPE_MISMATCH)) {
        return false;
    }

    return true;
}

bool Evaluator::exec_block(ModulePtr mod,
                           const ast::Block* block,
                           ExecState& st,
                           uint32_t call_depth,
                           bool push_scope) {
    if (!block) return true;

    if (push_scope) st.scopes.push_back({});

    for (const auto& stmt : block->statements) {
        if (st.returning) break;
        if (!exec_stmt(mod, stmt, st, call_depth)) {
            if (push_scope) st.scopes.pop_back();
            return false;
        }
        if (diags_.has_error()) {
            if (push_scope) st.scopes.pop_back();
            return false;
        }
    }

    if (push_scope) st.scopes.pop_back();
    return !diags_.has_error();
}

bool Evaluator::exec_stmt(ModulePtr mod,
                          const ast::Stmt& stmt,
                          ExecState& st,
                          uint32_t call_depth) {
    switch (stmt.kind) {
        case ast::StmtKind::kLet:
        case ast::StmtKind::kVar: {
            auto v = eval_expr(mod, stmt.let_decl.value.get(), st, call_depth);
            if (!v) return false;
            if (st.scopes.empty()) st.scopes.push_back({});
            st.scopes.back().vars[stmt.let_decl.name] = Variable{*v, stmt.kind == ast::StmtKind::kVar};
            return true;
        }

        case ast::StmtKind::kAssign: {
            auto v = eval_expr(mod, stmt.assign.value.get(), st, call_depth);
            if (!v) return false;
            return assign_value(mod,
                                st,
                                stmt.assign.path,
                                *v,
                                stmt.span,
                                true,
                                call_depth);
        }

        case ast::StmtKind::kFor: {
            auto iter = eval_expr(mod, stmt.for_stmt.iterable.get(), st, call_depth);
            if (!iter) return false;
            if (!iter->is_array()) {
                add_diag(diag::Code::L_TYPE_MISMATCH, stmt.span, "for iterable must be array");
                return false;
            }

            const auto& arr = std::get<Value::Array>(iter->data);
            if (arr.size() > budget_.max_loop_iters) {
                add_diag(diag::Code::L_BUDGET_EXCEEDED,
                         stmt.span,
                         "for loop iteration budget exceeded");
                return false;
            }

            for (const auto& iv : arr) {
                ++total_loop_steps_;
                if (total_loop_steps_ > budget_.max_total_loop_steps) {
                    add_diag(diag::Code::L_BUDGET_EXCEEDED,
                             stmt.span,
                             "total loop step budget exceeded");
                    return false;
                }

                st.scopes.push_back({});
                st.scopes.back().vars[stmt.for_stmt.iter_name] = Variable{iv, false};
                if (!exec_block(mod, stmt.for_stmt.body.get(), st, call_depth, false)) {
                    st.scopes.pop_back();
                    return false;
                }
                st.scopes.pop_back();
                if (st.returning) break;
            }

            return true;
        }

        case ast::StmtKind::kIf: {
            auto cond = eval_expr(mod, stmt.if_stmt.cond.get(), st, call_depth);
            if (!cond) return false;
            if (!cond->is_bool()) {
                add_diag(diag::Code::L_TYPE_MISMATCH, stmt.span, "if condition must be bool");
                return false;
            }

            if (std::get<bool>(cond->data)) {
                return exec_block(mod, stmt.if_stmt.then_block.get(), st, call_depth, true);
            }

            if (stmt.if_stmt.else_block) {
                return exec_block(mod, stmt.if_stmt.else_block.get(), st, call_depth, true);
            }

            return true;
        }

        case ast::StmtKind::kReturn: {
            if (!st.in_function) {
                add_diag(diag::Code::L_TYPE_MISMATCH, stmt.span, "return is only valid in function body");
                return false;
            }
            auto v = eval_expr(mod, stmt.ret.value.get(), st, call_depth);
            if (!v) return false;
            st.returning = true;
            st.return_value = *v;
            return true;
        }

        case ast::StmtKind::kAssert: {
            auto v = eval_expr(mod, stmt.expr.get(), st, call_depth);
            if (!v) return false;
            if (!v->is_bool() || !std::get<bool>(v->data)) {
                add_diag(diag::Code::L_TYPE_MISMATCH, stmt.span, "assert expects true bool expression");
                return false;
            }
            return true;
        }

        case ast::StmtKind::kExpr: {
            auto v = eval_expr(mod, stmt.expr.get(), st, call_depth);
            return v.has_value();
        }
    }

    add_diag(diag::Code::L_TYPE_MISMATCH, stmt.span, "unsupported statement kind");
    return false;
}

std::optional<Value> Evaluator::eval_binary(ModulePtr mod,
                                            const ast::Expr* expr,
                                            ExecState& st,
                                            uint32_t call_depth) {
    auto lv = eval_expr(mod, expr->lhs.get(), st, call_depth);
    auto rv = eval_expr(mod, expr->rhs.get(), st, call_depth);
    if (!lv || !rv) return std::nullopt;

    const std::string& op = expr->text;

    if (op == "&") {
        return merge_values(*lv, *rv, expr->span, "");
    }

    if (op == "&&" || op == "||") {
        if (!lv->is_bool() || !rv->is_bool()) {
            add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "logical operators require bool");
            return std::nullopt;
        }
        const bool a = std::get<bool>(lv->data);
        const bool b = std::get<bool>(rv->data);
        Value out{};
        out.data = (op == "&&") ? (a && b) : (a || b);
        return out;
    }

    if (op == "==" || op == "!=") {
        const bool eq = value_equal_strict(*lv, *rv);
        Value out{};
        out.data = (op == "==") ? eq : !eq;
        return out;
    }

    if (op == "+") {
        if (lv->is_string() && rv->is_string()) {
            Value out{};
            out.data = std::get<std::string>(lv->data) + std::get<std::string>(rv->data);
            return out;
        }
    }

    if (op == "+" || op == "-" || op == "*" || op == "/") {
        double a = 0.0;
        double b = 0.0;
        if (!numeric_promote(*lv, a) || !numeric_promote(*rv, b)) {
            add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "numeric operator requires int/float");
            return std::nullopt;
        }

        Value out{};
        if (op == "+") out.data = a + b;
        if (op == "-") out.data = a - b;
        if (op == "*") out.data = a * b;
        if (op == "/") out.data = a / b;

        if (lv->is_int() && rv->is_int() && op != "/") {
            const int64_t ai = std::get<int64_t>(lv->data);
            const int64_t bi = std::get<int64_t>(rv->data);
            if (op == "+") out.data = ai + bi;
            if (op == "-") out.data = ai - bi;
            if (op == "*") out.data = ai * bi;
        }

        return out;
    }

    add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "unsupported binary operator: " + op);
    return std::nullopt;
}

std::optional<Value> Evaluator::eval_expr(ModulePtr mod,
                                          const ast::Expr* expr,
                                          ExecState& st,
                                          uint32_t call_depth) {
    if (!expr) return std::nullopt;
    step_or_budget_error(expr->span);
    if (diags_.has_error()) return std::nullopt;

    switch (expr->kind) {
        case ast::ExprKind::kInt: {
            Value v;
            v.data = expr->int_value;
            return v;
        }

        case ast::ExprKind::kFloat: {
            Value v;
            v.data = expr->float_value;
            return v;
        }

        case ast::ExprKind::kString: {
            Value v;
            v.data = expr->text;
            return v;
        }

        case ast::ExprKind::kBool: {
            Value v;
            v.data = expr->bool_value;
            return v;
        }

        case ast::ExprKind::kIdent:
            return lookup_value(mod, st, expr->text, expr->span);

        case ast::ExprKind::kNamespaceRef: {
            if (expr->ns_parts.size() < 2) {
                add_diag(diag::Code::L_TYPE_MISMATCH,
                         expr->span,
                         "namespace reference must be 'alias::symbol'");
                return std::nullopt;
            }

            auto ait = mod->aliases.find(expr->ns_parts[0]);
            if (ait == mod->aliases.end()) {
                add_diag(diag::Code::L_UNKNOWN_IDENTIFIER,
                         expr->span,
                         "unknown import alias: " + expr->ns_parts[0]);
                return std::nullopt;
            }

            auto sit = ait->second->exports.find(expr->ns_parts[1]);
            if (sit == ait->second->exports.end()) {
                add_diag(diag::Code::L_IMPORT_SYMBOL_NOT_FOUND,
                         expr->span,
                         "module '" + ait->second->path + "' does not export '" + expr->ns_parts[1] + "'");
                return std::nullopt;
            }

            Value out = sit->second;
            for (size_t i = 2; i < expr->ns_parts.size(); ++i) {
                auto* obj = out.as_object();
                if (!obj) {
                    add_diag(diag::Code::L_TYPE_MISMATCH,
                             expr->span,
                             "namespace tail access requires object value");
                    return std::nullopt;
                }
                auto it = obj->find(expr->ns_parts[i]);
                if (it == obj->end()) {
                    add_diag(diag::Code::L_UNKNOWN_IDENTIFIER,
                             expr->span,
                             "unknown object key: " + expr->ns_parts[i]);
                    return std::nullopt;
                }
                out = it->second;
            }

            return out;
        }

        case ast::ExprKind::kObject: {
            Value::Object obj{};
            for (const auto& item : expr->object_items) {
                auto v = eval_expr(mod, item.value.get(), st, call_depth);
                if (!v) return std::nullopt;
                obj[item.key] = *v;
            }
            Value out{};
            out.data = std::move(obj);
            return out;
        }

        case ast::ExprKind::kArray: {
            Value::Array arr{};
            arr.reserve(expr->array_items.size());
            for (const auto& item : expr->array_items) {
                auto v = eval_expr(mod, item.get(), st, call_depth);
                if (!v) return std::nullopt;
                arr.push_back(*v);
            }
            Value out{};
            out.data = std::move(arr);
            return out;
        }

        case ast::ExprKind::kPlanPatch: {
            auto patch = std::make_shared<PatchValue>();
            for (const auto& a : expr->plan_patch_items) {
                auto pa = eval_patch_assign(mod, a, st, call_depth);
                if (!pa) return std::nullopt;
                patch->assigns.push_back(std::move(*pa));
            }
            Value out{};
            out.data = patch;
            return out;
        }

        case ast::ExprKind::kUnary: {
            auto rhs = eval_expr(mod, expr->rhs.get(), st, call_depth);
            if (!rhs) return std::nullopt;
            if (expr->text == "-") {
                if (auto pi = std::get_if<int64_t>(&rhs->data)) {
                    Value out;
                    out.data = -*pi;
                    return out;
                }
                if (auto pf = std::get_if<double>(&rhs->data)) {
                    Value out;
                    out.data = -*pf;
                    return out;
                }
                add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "unary '-' requires int/float");
                return std::nullopt;
            }
            if (expr->text == "!") {
                if (!rhs->is_bool()) {
                    add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "unary '!' requires bool");
                    return std::nullopt;
                }
                Value out;
                out.data = !std::get<bool>(rhs->data);
                return out;
            }
            add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "unsupported unary operator");
            return std::nullopt;
        }

        case ast::ExprKind::kBinary:
            return eval_binary(mod, expr, st, call_depth);

        case ast::ExprKind::kMember: {
            auto base = eval_expr(mod, expr->lhs.get(), st, call_depth);
            if (!base) return std::nullopt;
            auto obj = base->as_object();
            if (!obj) {
                add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "member access requires object");
                return std::nullopt;
            }
            auto it = obj->find(expr->text);
            if (it == obj->end()) {
                add_diag(diag::Code::L_UNKNOWN_IDENTIFIER, expr->span, "unknown object key: " + expr->text);
                return std::nullopt;
            }
            return it->second;
        }

        case ast::ExprKind::kIndex: {
            auto base = eval_expr(mod, expr->lhs.get(), st, call_depth);
            auto idx = eval_expr(mod, expr->rhs.get(), st, call_depth);
            if (!base || !idx) return std::nullopt;

            if (base->is_array()) {
                if (!idx->is_int()) {
                    add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "array index must be int");
                    return std::nullopt;
                }
                const int64_t i = std::get<int64_t>(idx->data);
                if (i < 0) {
                    add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "array index must be non-negative");
                    return std::nullopt;
                }
                const auto& arr = std::get<Value::Array>(base->data);
                if (static_cast<size_t>(i) >= arr.size()) {
                    add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "array index out of range");
                    return std::nullopt;
                }
                return arr[static_cast<size_t>(i)];
            }

            add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "index access requires array");
            return std::nullopt;
        }

        case ast::ExprKind::kCall: {
            auto callee = eval_expr(mod, expr->lhs.get(), st, call_depth);
            if (!callee) return std::nullopt;

            if (call_depth + 1 > budget_.max_call_depth) {
                add_diag(diag::Code::L_BUDGET_EXCEEDED, expr->span, "max_call_depth exceeded");
                return std::nullopt;
            }

            std::vector<Value> args;
            args.reserve(expr->call_args.size());
            for (const auto& a : expr->call_args) {
                auto v = eval_expr(mod, a.get(), st, call_depth);
                if (!v) return std::nullopt;
                args.push_back(*v);
            }

            if (callee->is_native_function()) {
                const auto fn = std::get<Value::NativeFunction>(callee->data);
                if (!fn || !fn->callback) {
                    add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "native function callback is missing");
                    return std::nullopt;
                }
                auto ret = fn->callback(args, expr->span, diags_);
                if (!ret && !diags_.has_error()) {
                    add_diag(diag::Code::L_TYPE_MISMATCH, expr->span,
                             "native function call failed: " + fn->name);
                }
                return ret;
            }

            if (!callee->is_function()) {
                add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "call target is not a function");
                return std::nullopt;
            }

            const auto& fn = std::get<FunctionValue>(callee->data);
            if (args.size() != fn.params.size()) {
                add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "function argument count mismatch");
                return std::nullopt;
            }

            std::string fq = fn.module_path + "::" + fn.name;
            if (std::find(call_stack_.begin(), call_stack_.end(), fq) != call_stack_.end()) {
                add_diag(diag::Code::L_RECURSION_FORBIDDEN, expr->span, "recursive call is forbidden: " + fq);
                return std::nullopt;
            }

            ModulePtr fn_mod;
            if (fn.module_path == mod->path) {
                fn_mod = mod;
            } else {
                fn_mod = evaluate_module(fn.module_path);
            }
            if (!fn_mod) return std::nullopt;

            call_stack_.push_back(fq);
            ExecState call_st{};
            call_st.in_function = true;
            call_st.scopes.push_back({});
            for (size_t i = 0; i < fn.params.size(); ++i) {
                call_st.scopes.back().vars[fn.params[i]] = Variable{args[i], false};
            }

            const bool ok = exec_block(fn_mod, fn.body, call_st, call_depth + 1, false);
            call_stack_.pop_back();

            if (!ok) return std::nullopt;
            if (!call_st.returning || !call_st.return_value.has_value()) {
                add_diag(diag::Code::L_TYPE_MISMATCH,
                         expr->span,
                         "function must return a value: " + fn.name);
                return std::nullopt;
            }

            return *call_st.return_value;
        }
    }

    add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "unsupported expression kind");
    return std::nullopt;
}

std::string to_string(const Value& v) {
    if (auto p = std::get_if<int64_t>(&v.data)) {
        return std::to_string(*p);
    }
    if (auto p = std::get_if<double>(&v.data)) {
        std::ostringstream oss;
        oss << *p;
        return oss.str();
    }
    if (auto p = std::get_if<std::string>(&v.data)) {
        return "\"" + *p + "\"";
    }
    if (auto p = std::get_if<bool>(&v.data)) {
        return *p ? "true" : "false";
    }
    if (auto p = std::get_if<Value::Array>(&v.data)) {
        std::string out = "[";
        bool first = true;
        for (const auto& x : *p) {
            if (!first) out += ", ";
            first = false;
            out += to_string(x);
        }
        out += "]";
        return out;
    }
    if (auto p = std::get_if<Value::Object>(&v.data)) {
        std::string out = "{";
        bool first = true;
        for (const auto& [k, x] : *p) {
            if (!first) out += ", ";
            first = false;
            out += k + ": " + to_string(x);
        }
        out += "}";
        return out;
    }
    if (auto p = std::get_if<Value::Function>(&v.data)) {
        return "<def " + p->name + ">";
    }
    if (auto p = std::get_if<Value::NativeFunction>(&v.data)) {
        if (*p && !(*p)->name.empty()) {
            return "<builtin " + (*p)->name + ">";
        }
        return "<builtin>";
    }
    if (auto p = std::get_if<Value::Template>(&v.data)) {
        if (*p) return "<template " + (*p)->name + ">";
        return "<template>";
    }
    if (std::holds_alternative<Value::Patch>(v.data)) {
        return "<patch>";
    }
    return "<unknown>";
}

} // namespace lei::eval
