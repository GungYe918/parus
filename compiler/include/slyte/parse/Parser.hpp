// compiler/include/slyte/parse/Parser.hpp
#pragma once
#include <slyte/parse/Cursor.hpp>
#include <slyte/ast/Nodes.hpp>

#include <vector>


namespace slyte {

    class Parser {
    public:
        Parser(const std::vector<Token>& tokens, ast::AstArena& ast)
            : cursor_(tokens), ast_(ast) {}

        ast::ExprId parse_expr();

    private:
        ast::ExprId parse_expr_pratt(int min_prec, int ternary_depth);
        ast::ExprId parse_prefix(int ternary_depth);
        ast::ExprId parse_primary(int ternary_depth);
        ast::ExprId parse_postfix(ast::ExprId base, int ternary_depth);

        ast::ExprId parse_call(ast::ExprId callee, const Token& lparen_tok, int ternary_depth);
        ast::ExprId parse_index(ast::ExprId base, const Token& lbracket_tok, int ternary_depth);

        ast::Arg parse_arg(int ternary_depth);

        Span span_join(Span a, Span b) const;

        Cursor cursor_;
        ast::AstArena& ast_;
    };

} // namespace slyte
