#pragma once

#include <lei/ast/Nodes.hpp>
#include <lei/diag/DiagCode.hpp>
#include <lei/parse/Cursor.hpp>
#include <lei/syntax/TokenKind.hpp>

#include <string>
#include <string_view>
#include <vector>

namespace lei::parse {

std::vector<syntax::Token> lex(std::string_view source, std::string_view file_path, diag::Bag& diags);

struct ParserControl {};

class Parser {
public:
    Parser(std::vector<syntax::Token> tokens,
           std::string file_path,
           diag::Bag& diags,
           ParserControl control = {})
        : tokens_(std::move(tokens)),
          file_path_(std::move(file_path)),
          diags_(diags),
          control_(control),
          cursor_(tokens_) {}

    ast::Program parse_program();

private:
    using K = syntax::TokenKind;

    const syntax::Token& peek(size_t k = 0) const;
    bool at(K k) const;
    const syntax::Token& bump();
    bool eat(K k);
    bool expect(K k, std::string_view what);

    ast::Item parse_item();
    ast::Item parse_import_alias();
    ast::Item parse_proto_decl();
    ast::Item parse_plan_decl(bool is_export);
    ast::Item parse_export_stmt();
    ast::Item parse_binding(bool is_var);
    ast::Item parse_def_decl();
    ast::Item parse_assert_item();

    std::optional<ast::TypeNode> parse_type();
    ast::TypeNode parse_type_required(std::string_view where);

    std::shared_ptr<ast::Block> parse_block();
    ast::Stmt parse_stmt();
    ast::Stmt parse_let_stmt(bool is_var);
    ast::Stmt parse_for_stmt();
    ast::Stmt parse_if_stmt();
    ast::Stmt parse_return_stmt();
    ast::Stmt parse_assert_stmt();
    ast::Stmt parse_expr_or_assign_stmt();

    ast::Path parse_path_required(std::string_view where);

    std::unique_ptr<ast::Expr> parse_expr();
    std::unique_ptr<ast::Expr> parse_merge();
    std::unique_ptr<ast::Expr> parse_or();
    std::unique_ptr<ast::Expr> parse_and();
    std::unique_ptr<ast::Expr> parse_eq();
    std::unique_ptr<ast::Expr> parse_add();
    std::unique_ptr<ast::Expr> parse_mul();
    std::unique_ptr<ast::Expr> parse_unary();
    std::unique_ptr<ast::Expr> parse_postfix();
    std::unique_ptr<ast::Expr> parse_primary();

    std::unique_ptr<ast::Expr> parse_object_lit();
    std::unique_ptr<ast::Expr> parse_array_lit();
    std::unique_ptr<ast::Expr> parse_plan_patch_lit();

    ast::Span span_from(const syntax::Token& t) const;
    void diag_expected(const syntax::Token& t, std::string_view what);
    void diag_legacy(const syntax::Token& t, std::string_view legacy);
    void diag_reserved_identifier(const syntax::Token& t, std::string_view where);
    bool is_reserved_name(std::string_view name) const;
    bool validate_decl_name(const syntax::Token& t,
                            std::string_view where,
                            bool allow_master_plan = false);

    bool looks_like_plan_patch() const;
    bool looks_like_path_assign_stmt() const;

    std::vector<syntax::Token> tokens_;
    std::string file_path_;
    diag::Bag& diags_;
    [[maybe_unused]] ParserControl control_{};
    Cursor cursor_;
};

ast::Program parse_source(std::string_view source,
                          std::string_view file_path,
                          diag::Bag& diags,
                          ParserControl control = {});

} // namespace lei::parse
