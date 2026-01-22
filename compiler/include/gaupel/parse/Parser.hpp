// compiler/include/gaupel/parse/Parser.hpp
#pragma once
#include <gaupel/parse/Cursor.hpp>
#include <gaupel/ast/Nodes.hpp>
#include <gaupel/diag/Diagnostic.hpp>

#include <vector>


namespace gaupel {

    class Parser {
    public:
        Parser(const std::vector<Token>& tokens, ast::AstArena& ast, diag::Bag* diags = nullptr, uint32_t max_errors = 64)
            : cursor_(tokens), ast_(ast), diags_(diags), max_errors_(max_errors) {
            if (diags_ && diags_->has_code(diag::Code::kInvalidUtf8)) {
                lexer_fatal_ = true;
                aborted_ = true; // lexer fatal이면 파싱 자체도 중단 상태로 취급해도 됨
            }
        }

        ast::ExprId parse_expr();

        ast::StmtId parse_stmt();

        /// @brief  EOF까지 stmt를 반복 파싱하여 프로그램 노드를 생성
        /// @details gaupelc에서 여러 stmt를 한 번에 검증할 때 사용
        ast::StmtId parse_program();

    private:
        void report(diag::Code code, Span span, std::string_view a0 = {});
        void report_int(diag::Code code, Span span, int v0);

        bool expect(syntax::TokenKind k);

        bool is_aborted() const {  return aborted_;  }
        bool is_fn_decl_start(syntax::TokenKind k) const;

        ast::ExprId parse_expr_pratt(int min_prec, int ternary_depth);
        ast::ExprId parse_prefix(int ternary_depth);
        ast::ExprId parse_primary(int ternary_depth);
        ast::ExprId parse_postfix(ast::ExprId base, int ternary_depth);

        ast::ExprId parse_call(ast::ExprId callee, const Token& lparen_tok, int ternary_depth);
        ast::ExprId parse_index(ast::ExprId base, const Token& lbracket_tok, int ternary_depth);

        ast::TypeId parse_type();

        ast::StmtId parse_stmt_inner();
        ast::StmtId parse_expr_stmt();
        ast::StmtId parse_block_stmt();

        ast::StmtId parse_var_stmt();
        
        ast::StmtId parse_if_stmt();
        ast::StmtId parse_while_stmt();
        ast::StmtId parse_return_stmt();
        ast::StmtId parse_break_stmt();
        ast::StmtId parse_continue_stmt();

        ast::StmtId parse_fn_decl_stmt();

        // helper: require a block after if/while
        ast::StmtId parse_required_block(std::string_view ctx);

        ast::Arg parse_arg(int ternary_depth);
        ast::Arg parse_named_group_call_arg(int ternary_depth);

        Span span_join(Span a, Span b) const;

        // stmt 종결자 ';'를 요구하되, 없으면 stmt 경계까지 recovery
        // - ';'가 있으면 그 span 반환
        // - 없으면 에러 찍고, ';' 또는 '}' 또는 EOF까지 스킵
        //   - 그 과정에서 ';'를 만나면 소비하고 그 span 반환
        //   - '}'/EOF에서 멈추면 fallback_end 또는 마지막으로 소비한 토큰 span 반환
        Span consume_semicolon_or_recover(Span fallback_end);

        // 현재 위치에서 stmt 경계까지 스킵(세미콜론은 소비하지 않음)
        void sync_to_stmt_boundary();

        void recover_to_delim(
            syntax::TokenKind stop0,
            syntax::TokenKind stop1 = syntax::TokenKind::kError,
            syntax::TokenKind stop2 = syntax::TokenKind::kError
        );

        Cursor cursor_;
        ast::AstArena& ast_;
        diag::Bag* diags_ = nullptr;

        // diagnostic quality controls
        uint32_t last_diag_lo_ = 0xFFFFFFFFu;
        diag::Code last_diag_code_ = diag::Code::kUnexpectedToken; // 초기값
        uint32_t parse_error_count_ = 0;
        static constexpr uint32_t kMaxParseErrors = 1024;

        uint32_t max_errors_ = 64;
        bool lexer_fatal_ = false;
        bool aborted_ = false;
        bool too_many_errors_emitted_ = false;
    };

} // namespace gaupel
