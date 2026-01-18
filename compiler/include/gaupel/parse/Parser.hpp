// compiler/include/gaupel/parse/Parser.hpp
#pragma once
#include <gaupel/parse/Cursor.hpp>
#include <gaupel/ast/Nodes.hpp>
#include <gaupel/diag/Diagnostic.hpp>

#include <vector>


namespace gaupel {

    class Parser {
    public:
        Parser(const std::vector<Token>& tokens, ast::AstArena& ast, diag::Bag* diags = nullptr)
            : cursor_(tokens), ast_(ast), diags_(diags) {}

        ast::ExprId parse_expr();

        ast::StmtId parse_stmt();
        ast::StmtId parse_block_stmt();

    private:
        void report(diag::Code code, Span span, std::string_view a0 = {});
        void report_int(diag::Code code, Span span, int v0);

        bool expect(syntax::TokenKind k);

        ast::ExprId parse_expr_pratt(int min_prec, int ternary_depth);
        ast::ExprId parse_prefix(int ternary_depth);
        ast::ExprId parse_primary(int ternary_depth);
        ast::ExprId parse_postfix(ast::ExprId base, int ternary_depth);

        ast::ExprId parse_call(ast::ExprId callee, const Token& lparen_tok, int ternary_depth);
        ast::ExprId parse_index(ast::ExprId base, const Token& lbracket_tok, int ternary_depth);

        ast::StmtId parse_stmt_inner();
        ast::StmtId parse_expr_stmt();

        ast::Arg parse_arg(int ternary_depth);

        Span span_join(Span a, Span b) const;

        Cursor cursor_;
        ast::AstArena& ast_;
        diag::Bag* diags_ = nullptr;
    };

} // namespace gaupel
