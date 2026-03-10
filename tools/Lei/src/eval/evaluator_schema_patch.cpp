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

