#pragma once

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lei::ast {

struct Span {
    std::string file;
    uint32_t line = 1;
    uint32_t column = 1;
};

struct Expr;
struct Stmt;
struct Block;

struct TypeNode {
    enum class Kind : uint8_t {
        kInt,
        kFloat,
        kString,
        kBool,
        kArray,
    } kind = Kind::kInt;

    std::unique_ptr<TypeNode> element{};
};

struct PathSegment {
    enum class Kind : uint8_t { kField, kIndex } kind = Kind::kField;
    std::string field{};
    std::shared_ptr<Expr> index{};
};

struct Path {
    std::vector<PathSegment> segments{};
};

enum class ExprKind : uint8_t {
    kInt,
    kFloat,
    kString,
    kBool,
    kIdent,
    kNamespaceRef,
    kObject,
    kArray,
    kPlanPatch,
    kUnary,
    kBinary,
    kCall,
    kMember,
    kIndex,
};

struct ObjectItem {
    std::string key;
    std::unique_ptr<Expr> value;
};

struct PlanAssign {
    Path path{};
    std::unique_ptr<Expr> value{};
};

struct Expr {
    ExprKind kind = ExprKind::kIdent;
    Span span{};

    int64_t int_value = 0;
    double float_value = 0.0;
    bool bool_value = false;
    std::string text{};

    std::vector<std::string> ns_parts{};
    std::vector<ObjectItem> object_items{};
    std::vector<std::unique_ptr<Expr>> array_items{};
    std::vector<PlanAssign> plan_patch_items{};
    std::vector<std::unique_ptr<Expr>> call_args{};

    std::unique_ptr<Expr> lhs{};
    std::unique_ptr<Expr> rhs{};
};

struct LetStmt {
    std::string name{};
    std::optional<TypeNode> type{};
    std::unique_ptr<Expr> value{};
};

struct AssignStmt {
    Path path{};
    std::unique_ptr<Expr> value{};
};

struct ForStmt {
    std::string iter_name{};
    std::unique_ptr<Expr> iterable{};
    std::shared_ptr<Block> body{};
};

struct IfStmt {
    std::unique_ptr<Expr> cond{};
    std::shared_ptr<Block> then_block{};
    std::shared_ptr<Block> else_block{};
};

struct ReturnStmt {
    std::unique_ptr<Expr> value{};
};

enum class StmtKind : uint8_t {
    kLet,
    kVar,
    kAssign,
    kFor,
    kIf,
    kReturn,
    kAssert,
    kExpr,
};

struct Stmt {
    StmtKind kind = StmtKind::kExpr;
    Span span{};

    LetStmt let_decl{};
    AssignStmt assign{};
    ForStmt for_stmt{};
    IfStmt if_stmt{};
    ReturnStmt ret{};
    std::unique_ptr<Expr> expr{};
};

struct Block {
    std::vector<Stmt> statements{};
};

struct Param {
    std::string name{};
    std::optional<TypeNode> type{};
};

struct ProtoField {
    std::string name{};
    TypeNode type{};
    std::unique_ptr<Expr> default_value{};
};

struct ProtoDecl {
    std::string name{};
    std::vector<ProtoField> fields{};
};

struct PlanDecl {
    std::string name{};
    bool is_expr_form = false;
    std::vector<PlanAssign> body_items{};
    std::unique_ptr<Expr> expr{};
};

struct DefDecl {
    std::string name{};
    std::vector<Param> params{};
    std::optional<TypeNode> return_type{};
    std::shared_ptr<Block> body{};
};

struct ImportAlias {
    std::string alias{};
    std::string from_path{};
};

enum class ItemKind : uint8_t {
    kImportAlias,
    kProto,
    kPlan,
    kExportPlan,
    kExportPlanRef,
    kLet,
    kVar,
    kDef,
    kAssert,
};

struct Item {
    ItemKind kind = ItemKind::kAssert;
    Span span{};

    ImportAlias import_alias{};
    ProtoDecl proto{};
    PlanDecl plan{};
    std::string export_plan_ref{};
    LetStmt binding{};
    DefDecl def{};
    std::unique_ptr<Expr> expr{};
};

struct Program {
    std::vector<Item> items{};
};

} // namespace lei::ast
