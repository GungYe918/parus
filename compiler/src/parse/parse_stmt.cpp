// compiler/src/parse/parse_stmt.cpp
#include <gaupel/parse/Parser.hpp>
#include <gaupel/syntax/TokenKind.hpp>


namespace gaupel {
    
    ast::StmtId Parser::parse_stmt() {
        return parse_stmt_inner();
    }

    ast::StmtId Parser::parse_stmt_inner() {
        const auto& tok = cursor_.peek();

        // empty stmt: ';'
        if (tok.kind == syntax::TokenKind::kSemicolon) {
            const Token semi = cursor_.bump();
            ast::Stmt s{};
            s.kind = ast::StmtKind::kEmpty;
            s.span = semi.span;
            return ast_.add_stmt(s);
        }

        // block stmt: '{' ... '}'
        if (tok.kind == syntax::TokenKind::kLBrace) {
            return parse_block_stmt();
        }

        // expr stmt: Expr ';'
        return parse_expr_stmt();
    }

    ast::StmtId Parser::parse_block_stmt() {
        const Token lb = cursor_.peek();
        expect(syntax::TokenKind::kLBrace);

        uint32_t begin = static_cast<uint32_t>(ast_.stmt_children().size());
        uint32_t count = 0;

        while (!cursor_.at(syntax::TokenKind::kRBrace) && !cursor_.at(syntax::TokenKind::kEof)) {
            ast::StmtId child = parse_stmt_inner();
            ast_.add_stmt_child(child);
            ++count;
        }

        const Token rb = cursor_.peek();
        expect(syntax::TokenKind::kRBrace);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kBlock;
        s.span = span_join(lb.span, rb.span);
        s.stmt_begin = begin;   // now indexes into stmt_children_ pool
        s.stmt_count = count;
        return ast_.add_stmt(s);
    }

    ast::StmtId Parser::parse_expr_stmt() {
        const auto start_tok = cursor_.peek();
        ast::ExprId e = parse_expr();
        const auto end_span = ast_.expr(e).span;

        // require ';'
        expect(syntax::TokenKind::kSemicolon);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kExprStmt;
        s.span = span_join(start_tok.span, end_span);
        s.expr = e;
        return ast_.add_stmt(s);
    }

}