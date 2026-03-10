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
    const std::string& op = expr->text;

    if (op == "&&") {
        auto lv = eval_expr(mod, expr->lhs.get(), st, call_depth);
        if (!lv) return std::nullopt;
        if (!lv->is_bool()) {
            add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "logical operators require bool");
            return std::nullopt;
        }
        const bool a = std::get<bool>(lv->data);
        if (!a) {
            Value out{};
            out.data = false;
            return out;
        }
        auto rv = eval_expr(mod, expr->rhs.get(), st, call_depth);
        if (!rv) return std::nullopt;
        if (!rv->is_bool()) {
            add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "logical operators require bool");
            return std::nullopt;
        }
        const bool b = std::get<bool>(rv->data);
        Value out{};
        out.data = a && b;
        return out;
    }

    if (op == "||") {
        auto lv = eval_expr(mod, expr->lhs.get(), st, call_depth);
        if (!lv) return std::nullopt;
        if (!lv->is_bool()) {
            add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "logical operators require bool");
            return std::nullopt;
        }
        const bool a = std::get<bool>(lv->data);
        if (a) {
            Value out{};
            out.data = true;
            return out;
        }
        auto rv = eval_expr(mod, expr->rhs.get(), st, call_depth);
        if (!rv) return std::nullopt;
        if (!rv->is_bool()) {
            add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "logical operators require bool");
            return std::nullopt;
        }
        const bool b = std::get<bool>(rv->data);
        Value out{};
        out.data = a || b;
        return out;
    }

    auto lv = eval_expr(mod, expr->lhs.get(), st, call_depth);
    auto rv = eval_expr(mod, expr->rhs.get(), st, call_depth);
    if (!lv || !rv) return std::nullopt;

    if (op == "&") {
        return merge_values(*lv, *rv, expr->span, "");
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
                auto member = resolve_object_member(out, expr->ns_parts[i], expr->span, diags_);
                if (!member) return std::nullopt;
                out = *member;
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
            return resolve_object_member(*base, expr->text, expr->span, diags_);
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
    if (auto p = std::get_if<Value::Dynamic>(&v.data)) {
        if (*p && !(*p)->name.empty()) return "<dynamic " + (*p)->name + ">";
        return "<dynamic>";
    }
    if (std::holds_alternative<Value::Patch>(v.data)) {
        return "<patch>";
    }
    return "<unknown>";
}

} // namespace lei::eval
