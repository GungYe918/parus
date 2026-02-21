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

struct ParserControl {
    // Backward-compat guard for removed syntax:
    //   import intrinsic { base };
    bool reject_removed_intrinsic_syntax = true;
};

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

    bool is_export_item_start() const;

    ast::Item parse_item();
    ast::Item parse_import();
    ast::Item parse_binding(bool is_const, bool is_export);
    ast::Item parse_def(bool is_export);
    ast::Item parse_assert();
    ast::Item parse_export_build();

    std::optional<std::string> parse_type_name();

    std::unique_ptr<ast::Expr> parse_expr();
    std::unique_ptr<ast::Expr> parse_if_expr();
    std::unique_ptr<ast::Expr> parse_match_expr();
    std::unique_ptr<ast::Expr> parse_default_overlay();
    std::unique_ptr<ast::Expr> parse_merge();
    std::unique_ptr<ast::Expr> parse_or();
    std::unique_ptr<ast::Expr> parse_and();
    std::unique_ptr<ast::Expr> parse_eq();
    std::unique_ptr<ast::Expr> parse_add();
    std::unique_ptr<ast::Expr> parse_mul();
    std::unique_ptr<ast::Expr> parse_unary();
    std::unique_ptr<ast::Expr> parse_postfix();
    std::unique_ptr<ast::Expr> parse_primary();

    std::unique_ptr<ast::Expr> parse_object();
    std::unique_ptr<ast::Expr> parse_array();

    ast::Span span_from(const syntax::Token& t) const;
    void diag_expected(const syntax::Token& t, std::string_view what);

    std::vector<syntax::Token> tokens_;
    std::string file_path_;
    diag::Bag& diags_;
    ParserControl control_{};
    Cursor cursor_;
};

ast::Program parse_source(std::string_view source,
                          std::string_view file_path,
                          diag::Bag& diags,
                          ParserControl control = {});

} // namespace lei::parse
