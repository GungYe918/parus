#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lei::ast {

struct Span {
    std::string file;
    uint32_t line = 1;
    uint32_t column = 1;
};

enum class ExprKind : uint8_t {
    kInt,
    kFloat,
    kString,
    kBool,
    kIdent,
    kObject,
    kArray,
    kUnary,
    kBinary,
    kIf,
    kMatch,
    kCall,
    kMember,
};

struct Expr;

struct ObjectItem {
    std::string key;
    std::unique_ptr<Expr> value;
};

struct ArrayItem {
    bool spread = false;
    std::unique_ptr<Expr> value;
};

struct MatchPattern {
    bool wildcard = false;
    std::optional<int64_t> int_value{};
    std::optional<double> float_value{};
    std::optional<std::string> string_value{};
    std::optional<bool> bool_value{};
};

struct MatchArm {
    MatchPattern pattern;
    std::unique_ptr<Expr> value;
};

struct Expr {
    ExprKind kind = ExprKind::kIdent;
    Span span{};

    // scalar
    int64_t int_value = 0;
    double float_value = 0.0;
    bool bool_value = false;
    std::string text{}; // identifier, string literal, operator, member name

    // composite
    std::vector<ObjectItem> object_items{};
    std::vector<ArrayItem> array_items{};
    std::vector<MatchArm> match_arms{};
    std::vector<std::unique_ptr<Expr>> call_args{};

    std::unique_ptr<Expr> lhs{};
    std::unique_ptr<Expr> rhs{};
    std::unique_ptr<Expr> cond{};
    std::unique_ptr<Expr> then_expr{};
    std::unique_ptr<Expr> else_expr{};
};

enum class ItemKind : uint8_t {
    kImportFrom,
    kLet,
    kConst,
    kDef,
    kAssert,
    kExportBuild,
};

struct ImportSpec {
    std::vector<std::string> names{};
    std::string from_path{};
};

struct BindingDecl {
    std::string name;
    std::optional<std::string> annotated_type{};
    bool is_const = false;
    bool is_export = false;
    std::unique_ptr<Expr> value{};
};

struct DefDecl {
    std::string name;
    std::vector<std::string> params{};
    bool is_export = false;
    std::unique_ptr<Expr> body{};
};

struct Item {
    ItemKind kind = ItemKind::kAssert;
    Span span{};

    ImportSpec import_spec{};
    BindingDecl binding{};
    DefDecl def{};
    std::unique_ptr<Expr> expr{};
};

struct Program {
    std::vector<Item> items{};
};

} // namespace lei::ast
