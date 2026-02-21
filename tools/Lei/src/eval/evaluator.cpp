#include <lei/eval/Evaluator.hpp>

#include <lei/parse/Parser.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace lei::eval {

namespace {

std::string normalize_path(const std::filesystem::path& p) {
    std::error_code ec;
    auto c = std::filesystem::weakly_canonical(p, ec);
    if (ec) return p.lexically_normal().string();
    return c.string();
}

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

bool value_equal(const Value& a, const Value& b) {
    if (a.data.index() == b.data.index()) {
        if (auto pa = std::get_if<int64_t>(&a.data)) return *pa == std::get<int64_t>(b.data);
        if (auto pa = std::get_if<double>(&a.data)) return *pa == std::get<double>(b.data);
        if (auto pa = std::get_if<std::string>(&a.data)) return *pa == std::get<std::string>(b.data);
        if (auto pa = std::get_if<bool>(&a.data)) return *pa == std::get<bool>(b.data);
    }
    double da = 0.0;
    double db = 0.0;
    if (numeric_promote(a, da) && numeric_promote(b, db)) {
        return da == db;
    }
    return false;
}

} // namespace

Evaluator::ModulePtr Evaluator::load_module(const std::filesystem::path& path) {
    const std::string key = normalize_path(path);
    auto it = module_cache_.find(key);
    if (it != module_cache_.end()) return it->second;

    std::ifstream ifs(key);
    if (!ifs) {
        diags_.add(diag::Code::L_IMPORT_NOT_FOUND, key, 1, 1, "cannot open LEI module");
        return nullptr;
    }

    std::stringstream buf;
    buf << ifs.rdbuf();

    auto mod = std::make_shared<ModuleContext>();
    mod->path = key;

    auto toks = parse::lex(buf.str(), key, diags_);
    parse::Parser p(std::move(toks), key, diags_);
    mod->program = p.parse_program();

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

    // Pass 1: predeclare defs for forward reference support.
    for (const auto& it : mod->program.items) {
        if (it.kind != ast::ItemKind::kDef) continue;
        Value fv{};
        fv.data = FunctionValue{mod->path, it.def.name, it.def.params, it.def.body.get()};
        mod->symbols[it.def.name] = fv;
        if (it.def.is_export) mod->exports[it.def.name] = fv;
    }

    // Pass 2: evaluate the rest.
    for (const auto& it : mod->program.items) {
        if (it.kind == ast::ItemKind::kDef) continue;

        if (it.kind == ast::ItemKind::kImportIntrinsic) {
            for (const auto& name : it.import_spec.names) {
                if (name == "base") {
                    mod->symbols[name] = make_intrinsic_base();
                } else {
                    add_diag(diag::Code::L_IMPORT_SYMBOL_NOT_FOUND, it.span,
                             "unknown intrinsic symbol: " + name);
                    return nullptr;
                }
            }
            continue;
        }

        if (it.kind == ast::ItemKind::kImportFrom) {
            std::filesystem::path import_path = std::filesystem::path(mod->path).parent_path() / it.import_spec.from_path;
            auto imported = evaluate_module(import_path);
            if (!imported) return nullptr;
            for (const auto& sym : it.import_spec.names) {
                auto ex = imported->exports.find(sym);
                if (ex == imported->exports.end()) {
                    add_diag(diag::Code::L_IMPORT_SYMBOL_NOT_FOUND, it.span,
                             "module '" + imported->path + "' does not export '" + sym + "'");
                    return nullptr;
                }
                mod->symbols[sym] = ex->second;
            }
            continue;
        }

        if (it.kind == ast::ItemKind::kLet || it.kind == ast::ItemKind::kConst) {
            std::unordered_map<std::string, Value> locals;
            auto v = eval_expr(mod, it.binding.value.get(), locals, 0);
            if (!v) return nullptr;
            mod->symbols[it.binding.name] = *v;
            if (it.binding.is_export) {
                mod->exports[it.binding.name] = *v;
            }
            continue;
        }

        if (it.kind == ast::ItemKind::kAssert) {
            std::unordered_map<std::string, Value> locals;
            auto v = eval_expr(mod, it.expr.get(), locals, 0);
            if (!v) return nullptr;
            if (!v->is_bool() || !std::get<bool>(v->data)) {
                add_diag(diag::Code::L_TYPE_MISMATCH, it.span, "assert expects true bool expression");
                return nullptr;
            }
            continue;
        }

        if (it.kind == ast::ItemKind::kExportBuild) {
            std::unordered_map<std::string, Value> locals;
            auto v = eval_expr(mod, it.expr.get(), locals, 0);
            if (!v) return nullptr;
            mod->build = *v;
            continue;
        }
    }

    mod->evaluated = true;
    module_mark_[key] = 2;
    return mod;
}

std::optional<Value> Evaluator::evaluate_entry(const std::filesystem::path& entry_path) {
    auto mod = evaluate_module(entry_path);
    if (!mod) return std::nullopt;
    if (!mod->build.has_value()) {
        diags_.add(diag::Code::B_INVALID_BUILD_SHAPE, mod->path, 1, 1,
                   "entry module must define 'export build ...;'");
        return std::nullopt;
    }
    return mod->build;
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

Value Evaluator::make_intrinsic_base() const {
    Value::Object defaults;
    defaults["profile"].data = std::string("debug");
    defaults["opt"].data = int64_t{0};

    Value::Object root;
    root["version"].data = std::string("0.1");
    root["backend"].data = std::string("ninja");
    root["defaults"].data = defaults;

    Value v;
    v.data = std::move(root);
    return v;
}

std::optional<Value> Evaluator::merge_objects(const Value& lhs, const Value& rhs, const ast::Span& span) {
    auto lo = lhs.as_object();
    auto ro = rhs.as_object();
    if (!lo || !ro) {
        add_diag(diag::Code::L_MERGE_CONFLICT, span, "'&' requires object operands");
        return std::nullopt;
    }

    Value::Object out = *lo;
    for (const auto& [k, rv] : *ro) {
        auto it = out.find(k);
        if (it == out.end()) {
            out[k] = rv;
            continue;
        }

        if (it->second.is_object() && rv.is_object()) {
            Value ltmp; ltmp.data = *it->second.as_object();
            auto merged = merge_objects(ltmp, rv, span);
            if (!merged) return std::nullopt;
            it->second = *merged;
            continue;
        }

        if (value_equal(it->second, rv)) continue;

        add_diag(diag::Code::L_MERGE_CONFLICT, span, "object merge conflict on key '" + k + "'");
        return std::nullopt;
    }

    Value v;
    v.data = std::move(out);
    return v;
}

std::optional<Value> Evaluator::overlay_defaults(const Value& lhs, const Value& rhs, const ast::Span& span) {
    auto lo = lhs.as_object();
    auto ro = rhs.as_object();
    if (!lo || !ro) {
        add_diag(diag::Code::L_DEFAULT_OVERLAY_INVALID, span, "'?=' requires object operands");
        return std::nullopt;
    }

    Value::Object out = *lo;
    for (const auto& [k, rv] : *ro) {
        auto it = out.find(k);
        if (it == out.end()) {
            out[k] = rv;
            continue;
        }

        if (it->second.is_object() && rv.is_object()) {
            Value ltmp; ltmp.data = *it->second.as_object();
            auto nested = overlay_defaults(ltmp, rv, span);
            if (!nested) return std::nullopt;
            it->second = *nested;
        }
    }

    Value v;
    v.data = std::move(out);
    return v;
}

std::optional<Value> Evaluator::eval_binary(ModulePtr mod,
                                            const ast::Expr* expr,
                                            std::unordered_map<std::string, Value>& locals,
                                            uint32_t call_depth) {
    auto lv = eval_expr(mod, expr->lhs.get(), locals, call_depth);
    auto rv = eval_expr(mod, expr->rhs.get(), locals, call_depth);
    if (!lv || !rv) return std::nullopt;

    const std::string& op = expr->text;

    if (op == "&") return merge_objects(*lv, *rv, expr->span);
    if (op == "?=") return overlay_defaults(*lv, *rv, expr->span);

    if (op == "&&" || op == "||") {
        if (!lv->is_bool() || !rv->is_bool()) {
            add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "logical operators require bool");
            return std::nullopt;
        }
        const bool a = std::get<bool>(lv->data);
        const bool b = std::get<bool>(rv->data);
        Value out;
        out.data = (op == "&&") ? (a && b) : (a || b);
        return out;
    }

    if (op == "==" || op == "!=") {
        const bool eq = value_equal(*lv, *rv);
        Value out;
        out.data = (op == "==") ? eq : !eq;
        return out;
    }

    if (op == "+") {
        if (lv->is_string() && rv->is_string()) {
            Value out;
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

        Value out;
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
                                          std::unordered_map<std::string, Value>& locals,
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
        case ast::ExprKind::kIdent: {
            auto lit = locals.find(expr->text);
            if (lit != locals.end()) return lit->second;
            auto git = mod->symbols.find(expr->text);
            if (git != mod->symbols.end()) return git->second;
            add_diag(diag::Code::L_UNKNOWN_IDENTIFIER, expr->span, "unknown identifier: " + expr->text);
            return std::nullopt;
        }
        case ast::ExprKind::kObject: {
            Value::Object obj;
            for (const auto& item : expr->object_items) {
                auto v = eval_expr(mod, item.value.get(), locals, call_depth);
                if (!v) return std::nullopt;
                obj[item.key] = *v;
            }
            Value out;
            out.data = std::move(obj);
            return out;
        }
        case ast::ExprKind::kArray: {
            Value::Array arr;
            for (const auto& item : expr->array_items) {
                auto v = eval_expr(mod, item.value.get(), locals, call_depth);
                if (!v) return std::nullopt;
                if (!item.spread) {
                    arr.push_back(*v);
                    continue;
                }
                auto spread = std::get_if<Value::Array>(&v->data);
                if (!spread) {
                    add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "spread requires array value");
                    return std::nullopt;
                }
                arr.insert(arr.end(), spread->begin(), spread->end());
            }
            Value out;
            out.data = std::move(arr);
            return out;
        }
        case ast::ExprKind::kUnary: {
            auto rhs = eval_expr(mod, expr->rhs.get(), locals, call_depth);
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
            return eval_binary(mod, expr, locals, call_depth);

        case ast::ExprKind::kIf: {
            auto c = eval_expr(mod, expr->cond.get(), locals, call_depth);
            if (!c) return std::nullopt;
            if (!c->is_bool()) {
                add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "if condition must be bool");
                return std::nullopt;
            }
            return std::get<bool>(c->data)
                       ? eval_expr(mod, expr->then_expr.get(), locals, call_depth)
                       : eval_expr(mod, expr->else_expr.get(), locals, call_depth);
        }

        case ast::ExprKind::kMatch: {
            auto m = eval_expr(mod, expr->cond.get(), locals, call_depth);
            if (!m) return std::nullopt;

            for (const auto& arm : expr->match_arms) {
                bool hit = false;
                if (arm.pattern.wildcard) {
                    hit = true;
                } else if (arm.pattern.int_value.has_value()) {
                    Value p; p.data = *arm.pattern.int_value;
                    hit = value_equal(*m, p);
                } else if (arm.pattern.float_value.has_value()) {
                    Value p; p.data = *arm.pattern.float_value;
                    hit = value_equal(*m, p);
                } else if (arm.pattern.string_value.has_value()) {
                    Value p; p.data = *arm.pattern.string_value;
                    hit = value_equal(*m, p);
                } else if (arm.pattern.bool_value.has_value()) {
                    Value p; p.data = *arm.pattern.bool_value;
                    hit = value_equal(*m, p);
                }

                if (hit) return eval_expr(mod, arm.value.get(), locals, call_depth);
            }

            add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "match has no matching arm");
            return std::nullopt;
        }

        case ast::ExprKind::kMember: {
            auto base = eval_expr(mod, expr->lhs.get(), locals, call_depth);
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

        case ast::ExprKind::kCall: {
            auto callee = eval_expr(mod, expr->lhs.get(), locals, call_depth);
            if (!callee) return std::nullopt;
            if (!callee->is_function()) {
                add_diag(diag::Code::L_TYPE_MISMATCH, expr->span, "call target is not a function");
                return std::nullopt;
            }

            const auto& fn = std::get<FunctionValue>(callee->data);
            if (call_depth + 1 > budget_.max_call_depth) {
                add_diag(diag::Code::L_BUDGET_EXCEEDED, expr->span, "max_call_depth exceeded");
                return std::nullopt;
            }

            std::vector<Value> args;
            args.reserve(expr->call_args.size());
            for (const auto& a : expr->call_args) {
                auto v = eval_expr(mod, a.get(), locals, call_depth);
                if (!v) return std::nullopt;
                args.push_back(*v);
            }

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
            std::unordered_map<std::string, Value> call_locals;
            for (size_t i = 0; i < fn.params.size(); ++i) {
                call_locals[fn.params[i]] = args[i];
            }
            auto ret = eval_expr(fn_mod, fn.body, call_locals, call_depth + 1);
            call_stack_.pop_back();
            return ret;
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
    return "<unknown>";
}

} // namespace lei::eval
