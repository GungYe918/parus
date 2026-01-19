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

        /// @brief  EOF까지 stmt를 반복 파싱하여 프로그램 노드를 생성
        /// @details gaupelc에서 여러 stmt를 한 번에 검증할 때 사용
        ast::StmtId parse_program();

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

        ast::StmtId parse_block_stmt();

        ast::StmtId parse_let_stmt();
        ast::StmtId parse_if_stmt();
        ast::StmtId parse_while_stmt();
        ast::StmtId parse_return_stmt();
        ast::StmtId parse_break_stmt();
        ast::StmtId parse_continue_stmt();

        // helper: require a block after if/while
        ast::StmtId parse_required_block(std::string_view ctx);

        ast::Arg parse_arg(int ternary_depth);

        Span span_join(Span a, Span b) const;

        // stmt 종결자 ';'를 요구하되, 없으면 stmt 경계까지 recovery
        // - ';'가 있으면 그 span 반환
        // - 없으면 에러 찍고, ';' 또는 '}' 또는 EOF까지 스킵
        //   - 그 과정에서 ';'를 만나면 소비하고 그 span 반환
        //   - '}'/EOF에서 멈추면 fallback_end 또는 마지막으로 소비한 토큰 span 반환
        Span consume_semicolon_or_recover(Span fallback_end);

        // 현재 위치에서 stmt 경계까지 스킵(세미콜론은 소비하지 않음)
        void sync_to_stmt_boundary();

        Cursor cursor_;
        ast::AstArena& ast_;
        diag::Bag* diags_ = nullptr;
    };

} // namespace gaupel
