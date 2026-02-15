#include <gaupel/parse/Parser.hpp>
#include <gaupel/syntax/TokenKind.hpp>

namespace gaupel {

    /// @brief `do { ... }` / `do { ... } while (cond);` 구문을 파싱한다.
    ast::StmtId Parser::parse_stmt_do() {
        using K = syntax::TokenKind;

        const Token do_kw = cursor_.bump(); // 'do'

        auto make_empty_block = [&]() -> ast::StmtId {
            ast::Stmt empty{};
            empty.kind = ast::StmtKind::kBlock;
            empty.span = cursor_.peek().span;
            empty.stmt_begin = static_cast<uint32_t>(ast_.stmt_children().size());
            empty.stmt_count = 0;
            return ast_.add_stmt(empty);
        };

        // do 본문은 항상 block이어야 한다.
        ast::StmtId body = ast::k_invalid_stmt;
        if (!cursor_.at(K::kLBrace)) {
            diag_report(diag::Code::kDoBodyExpectedBlock, cursor_.peek().span);
            recover_to_delim(K::kLBrace, K::kKwWhile, K::kSemicolon);
            if (cursor_.at(K::kLBrace)) body = parse_stmt_block();
            else body = make_empty_block();
        } else {
            body = parse_stmt_block();
        }

        // do-while 확장: do { ... } while (cond);
        if (cursor_.at(K::kKwWhile)) {
            const Token while_kw = cursor_.bump();

            bool has_lparen = false;
            if (cursor_.eat(K::kLParen)) {
                has_lparen = true;
            } else {
                diag_report(diag::Code::kDoWhileExpectedLParen, cursor_.peek().span);
            }

            ast::ExprId cond = ast::k_invalid_expr;
            if (!cursor_.at(K::kRParen) && !cursor_.at(K::kSemicolon) && !cursor_.at(K::kEof)) {
                cond = parse_expr();
            } else {
                // 조건식이 비어 있으면 후속 파싱 안정성을 위해 Error expr를 넣는다.
                ast::Expr err{};
                err.kind = ast::ExprKind::kError;
                err.span = cursor_.peek().span;
                err.text = "do_while_missing_cond";
                cond = ast_.add_expr(err);
            }

            if (has_lparen) {
                if (!cursor_.eat(K::kRParen)) {
                    diag_report(diag::Code::kDoWhileExpectedRParen, cursor_.peek().span);
                    recover_to_delim(K::kRParen, K::kSemicolon, K::kRBrace);
                    cursor_.eat(K::kRParen);
                }
            }

            // do-while은 ';'를 요구한다.
            Span end = while_kw.span;
            if (cursor_.at(K::kSemicolon)) {
                end = cursor_.bump().span;
            } else {
                diag_report(diag::Code::kDoWhileExpectedSemicolon, cursor_.peek().span);
                stmt_sync_to_boundary();
                if (cursor_.at(K::kSemicolon)) {
                    end = cursor_.bump().span;
                } else {
                    end = cursor_.prev().span;
                }
            }

            ast::Stmt s{};
            s.kind = ast::StmtKind::kDoWhile;
            s.expr = cond;
            s.a = body;
            s.span = span_join(do_kw.span, end);
            return ast_.add_stmt(s);
        }

        ast::Stmt s{};
        s.kind = ast::StmtKind::kDoScope;
        s.a = body;
        s.span = span_join(do_kw.span, ast_.stmt(body).span);
        return ast_.add_stmt(s);
    }

} // namespace gaupel

