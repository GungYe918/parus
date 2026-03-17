#include "evaluator_internal.hpp"

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
    return n == "bundle" || n == "module" || n == "master" || n == "task" || n == "codegen";
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

std::optional<Value> resolve_object_member(const Value& base,
                                           std::string_view key,
                                           const ast::Span& span,
                                           diag::Bag& diags) {
    if (const auto* obj = base.as_object()) {
        auto it = obj->find(std::string(key));
        if (it == obj->end()) {
            diags.add(diag::Code::L_UNKNOWN_IDENTIFIER,
                      span.file,
                      span.line,
                      span.column,
                      "unknown object key: " + std::string(key));
            return std::nullopt;
        }
        return it->second;
    }

    if (const auto* dyn = base.as_dynamic_object()) {
        if (!dyn->resolve) {
            diags.add(diag::Code::L_TYPE_MISMATCH,
                      span.file,
                      span.line,
                      span.column,
                      "dynamic object has no member resolver");
            return std::nullopt;
        }

        const auto before = diags.all().size();
        auto resolved = dyn->resolve(key, span, diags);
        if (!resolved && diags.all().size() == before) {
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
              "member access requires object");
    return std::nullopt;
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
    auto it = obj.find("modules");
    if (it == obj.end() || !it->second.is_array()) {
        return std::string("bundle.modules must be [object]");
    }
    const auto& arr = std::get<Value::Array>(it->second.data);
    if (arr.empty()) {
        return std::string("bundle.modules must not be empty");
    }
    for (const auto& v : arr) {
        if (!v.is_object()) {
            return std::string("bundle.modules must contain only object");
        }
    }
    auto legacy_it = obj.find("sources");
    if (legacy_it != obj.end()) {
        return std::string("bundle.sources is removed; use bundle.modules");
    }
    if (auto it = obj.find("cimport"); it != obj.end()) {
        if (!it->second.is_object()) {
            return std::string("bundle.cimport must be object");
        }
        const auto* cobj = it->second.as_object();
        auto is_it = cobj->find("isystem");
        if (is_it != cobj->end()) {
            if (!is_it->second.is_array()) {
                return std::string("bundle.cimport.isystem must be [string]");
            }
            const auto& arr = std::get<Value::Array>(is_it->second.data);
            for (const auto& v : arr) {
                if (!v.is_string()) return std::string("bundle.cimport.isystem must contain only string");
            }
        }
    }
    return std::nullopt;
}

std::optional<std::string> validate_module(const Value::Object& obj) {
    auto src_it = obj.find("sources");
    if (src_it == obj.end() || !src_it->second.is_array()) {
        return std::string("module.sources must be [string]");
    }
    const auto& srcs = std::get<Value::Array>(src_it->second.data);
    if (srcs.empty()) {
        return std::string("module.sources must not be empty");
    }
    for (const auto& v : srcs) {
        if (!v.is_string()) {
            return std::string("module.sources must contain only string");
        }
    }
    auto imp_it = obj.find("imports");
    if (imp_it != obj.end()) {
        if (!imp_it->second.is_array()) {
            return std::string("module.imports must be [string]");
        }
        const auto& imps = std::get<Value::Array>(imp_it->second.data);
        for (const auto& v : imps) {
            if (!v.is_string()) {
                return std::string("module.imports must contain only string");
            }
        }
    }
    if (auto it = obj.find("cimport"); it != obj.end()) {
        if (!it->second.is_object()) {
            return std::string("module.cimport must be object");
        }
        const auto* cobj = it->second.as_object();
        auto is_it = cobj->find("isystem");
        if (is_it != cobj->end()) {
            if (!is_it->second.is_array()) {
                return std::string("module.cimport.isystem must be [string]");
            }
            const auto& arr = std::get<Value::Array>(is_it->second.data);
            for (const auto& v : arr) {
                if (!v.is_string()) return std::string("module.cimport.isystem must contain only string");
            }
        }
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
    spec->fields["modules"] = TemplateField{make_array_schema(make_scalar_schema(SchemaType::Kind::kObject)), true, std::nullopt};
    spec->fields["deps"] = TemplateField{make_array_schema(make_scalar_schema(SchemaType::Kind::kString)), true, std::optional<Value>{make_array({})}};
    spec->fields["cimport"] = TemplateField{
        make_scalar_schema(SchemaType::Kind::kObject),
        false,
        std::optional<Value>{make_object({{"isystem", make_array({})}})}
    };
    spec->validator = validate_bundle;
    return spec;
}

std::shared_ptr<TemplateSpec> make_module_template() {
    auto spec = std::make_shared<TemplateSpec>();
    spec->name = "module";

    spec->fields["sources"] = TemplateField{make_array_schema(make_scalar_schema(SchemaType::Kind::kString)), true, std::nullopt};
    spec->fields["imports"] = TemplateField{make_array_schema(make_scalar_schema(SchemaType::Kind::kString)), false, std::optional<Value>{make_array({})}};
    spec->fields["cimport"] = TemplateField{
        make_scalar_schema(SchemaType::Kind::kObject),
        false,
        std::optional<Value>{make_object({{"isystem", make_array({})}})}
    };
    spec->validator = validate_module;
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
    builtins::register_builtin_constants(reg);
    builtins::register_builtin_functions(reg);
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
    reg.register_plan("module", make_module_template);
    reg.register_plan("master", make_master_template);
    reg.register_plan("task", make_task_template);
    reg.register_plan("codegen", make_codegen_template);
    return reg;
}

Evaluator::ModulePtr Evaluator::load_module(const std::filesystem::path& path) {
    const std::string key = lei::os::normalize_path(path.string());
    auto it = module_cache_.find(key);
    if (it != module_cache_.end()) return it->second;

    auto mod = std::make_shared<ModuleContext>();
    mod->path = key;

    if (options_.source_overlay) {
        auto overlaid = options_.source_overlay(key);
        if (overlaid.has_value()) {
            mod->program = parse::parse_source(*overlaid, key, diags_, parser_control_);
            module_cache_[key] = mod;
            return mod;
        }
    }

    const auto read = lei::os::read_text_file(key);
    if (!read.ok) {
        diags_.add(diag::Code::L_IMPORT_NOT_FOUND, key, 1, 1, "cannot open LEI module: " + read.err);
        return nullptr;
    }

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

std::vector<std::string> Evaluator::loaded_module_paths() const {
    std::vector<std::string> out{};
    out.reserve(module_cache_.size());
    for (const auto& [path, _] : module_cache_) out.push_back(path);
    std::sort(out.begin(), out.end());
    return out;
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
