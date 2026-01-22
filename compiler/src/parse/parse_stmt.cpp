// compiler/src/parse/parse_stmt.cpp
#include <gaupel/parse/Parser.hpp>
#include <gaupel/syntax/TokenKind.hpp>

#include <vector>


namespace gaupel {

    ast::StmtId Parser::parse_stmt() {
        return parse_stmt_any();
    }

    ast::StmtId Parser::parse_program() {
        uint32_t begin = static_cast<uint32_t>(ast_.stmt_children().size());
        uint32_t count = 0;

        Span first = cursor_.peek().span;
        Span last  = first;

        while (!cursor_.at(syntax::TokenKind::kEof)) {
            ast::StmtId s = parse_stmt_any();
            ast_.add_stmt_child(s);
            ++count;
            last = ast_.stmt(s).span;
        }

        if (count == 0) {
            first = cursor_.peek().span;
            last  = first;
        }

        ast::Stmt root{};
        root.kind = ast::StmtKind::kBlock;
        root.span = span_join(first, last);
        root.stmt_begin = begin;
        root.stmt_count = count;
        return ast_.add_stmt(root);
    }

    // stmt/decl 혼용 엔트리
    ast::StmtId Parser::parse_stmt_any() {
        const auto& tok = cursor_.peek();

        // decl start => decl 파서로 위임
        if (is_decl_start(tok.kind)) {
            return parse_decl_any();
        }

        // empty stmt: ';'
        if (tok.kind == syntax::TokenKind::kSemicolon) {
            const Token semi = cursor_.bump();
            ast::Stmt s{};
            s.kind = ast::StmtKind::kEmpty;
            s.span = semi.span;
            return ast_.add_stmt(s);
        }

        // block
        if (tok.kind == syntax::TokenKind::kLBrace) {
            return parse_stmt_block();
        }

        // keyword stmts
        if (tok.kind == syntax::TokenKind::kKwIf)       return parse_stmt_if();
        if (tok.kind == syntax::TokenKind::kKwWhile)    return parse_stmt_while();
        if (tok.kind == syntax::TokenKind::kKwReturn)   return parse_stmt_return();
        if (tok.kind == syntax::TokenKind::kKwBreak)    return parse_stmt_break();
        if (tok.kind == syntax::TokenKind::kKwContinue) return parse_stmt_continue();
        if (tok.kind == syntax::TokenKind::kKwLet
        ||  tok.kind == syntax::TokenKind::kKwSet)      return parse_stmt_var();

        return parse_stmt_expr();
    }

    // '{ ... }' 블록 파싱
    ast::StmtId Parser::parse_stmt_block() {
        const Token lb = cursor_.peek();
        diag_expect(syntax::TokenKind::kLBrace);

        uint32_t begin = static_cast<uint32_t>(ast_.stmt_children().size());
        uint32_t count = 0;

        while (!cursor_.at(syntax::TokenKind::kRBrace) && !cursor_.at(syntax::TokenKind::kEof)) {
            ast::StmtId child = parse_stmt_any();
            ast_.add_stmt_child(child);
            ++count;
        }

        const Token rb = cursor_.peek();
        diag_expect(syntax::TokenKind::kRBrace);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kBlock;
        s.span = span_join(lb.span, rb.span);
        s.stmt_begin = begin;
        s.stmt_count = count;
        return ast_.add_stmt(s);
    }

    // expr ';' 파싱
    ast::StmtId Parser::parse_stmt_expr() {
        const auto start_tok = cursor_.peek();
        ast::ExprId e = parse_expr();

        const Span expr_end = ast_.expr(e).span;
        const Span term_end = stmt_consume_semicolon_or_recover(expr_end);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kExprStmt;
        s.span = span_join(start_tok.span, term_end);
        s.expr = e;
        return ast_.add_stmt(s);
    }

    // 블록이 필수인 구문에서 블록을 강제
    ast::StmtId Parser::parse_stmt_required_block(std::string_view ctx) {
        (void)ctx;
        if (!cursor_.at(syntax::TokenKind::kLBrace)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "{");

            stmt_sync_to_boundary();
            if (cursor_.at(syntax::TokenKind::kSemicolon)) cursor_.bump();

            ast::Stmt s{};
            s.kind = ast::StmtKind::kBlock;
            s.span = cursor_.peek().span;
            s.stmt_begin = static_cast<uint32_t>(ast_.stmt_children().size());
            s.stmt_count = 0;
            return ast_.add_stmt(s);
        }
        return parse_stmt_block();
    }

    // let/set 파싱
    ast::StmtId Parser::parse_stmt_var() {
        const Token& kw = cursor_.peek();
        const bool is_set = (kw.kind == syntax::TokenKind::kKwSet);
        cursor_.bump();

        bool is_mut = false;
        if (cursor_.at(syntax::TokenKind::kKwMut)) {
            is_mut = true;
            cursor_.bump();
        }

        const Token& name_tok = cursor_.peek();
        std::string_view name{};
        if (name_tok.kind == syntax::TokenKind::kIdent) {
            name = name_tok.lexeme;
            cursor_.bump();
        } else {
            diag_report(diag::Code::kUnexpectedToken, name_tok.span, "identifier");
        }

        ast::TypeId type_id = ast::k_invalid_type;

        if (!is_set) {
            if (!cursor_.at(syntax::TokenKind::kColon)) {
                diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                            "':' (type annotation required for let)");
            } else {
                cursor_.bump();
                type_id = parse_type();
            }
        } else {
            if (cursor_.at(syntax::TokenKind::kColon)) {
                diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                            "type annotation not allowed for set in v0");
                cursor_.bump();
                (void)parse_type();
            }
        }

        ast::ExprId init = ast::k_invalid_expr;

        if (cursor_.at(syntax::TokenKind::kAssign)) {
            cursor_.bump();
            init = parse_expr();
        } else {
            if (is_set) {
                diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                            "'=' initializer required for set");
            }
        }

        const Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kVar;
        s.is_set = is_set;
        s.is_mut = is_mut;
        s.name = name;
        s.type = type_id;
        s.init = init;
        s.span = span_join(kw.span, end);
        return ast_.add_stmt(s);
    }

    // while 파싱
    ast::StmtId Parser::parse_stmt_while() {
        const Token kw = cursor_.bump();

        ast::ExprId cond = parse_expr();
        ast::StmtId body = parse_stmt_required_block("while");

        ast::Stmt s{};
        s.kind = ast::StmtKind::kWhile;
        s.expr = cond;
        s.a = body;
        s.span = span_join(kw.span, ast_.stmt(body).span);
        return ast_.add_stmt(s);
    }

    // if/elif/else 파싱(elif는 desugar)
    ast::StmtId Parser::parse_stmt_if() {
        const Token if_kw = cursor_.bump();
        ast::ExprId cond0 = parse_expr();
        ast::StmtId then0 = parse_stmt_required_block("if");

        struct ElifArm { ast::ExprId cond; ast::StmtId block; Span span; };
        std::vector<ElifArm> elifs;

        while (cursor_.at(syntax::TokenKind::kKwElif)) {
            const Token elif_kw = cursor_.bump();
            ast::ExprId c = parse_expr();
            ast::StmtId b = parse_stmt_required_block("elif");
            elifs.push_back(ElifArm{c, b, span_join(elif_kw.span, ast_.stmt(b).span)});
        }

        ast::StmtId else_block = ast::k_invalid_stmt;
        if (cursor_.at(syntax::TokenKind::kKwElse)) {
            cursor_.bump();
            else_block = parse_stmt_required_block("else");
        }

        ast::StmtId tail_else = else_block;
        for (int i = static_cast<int>(elifs.size()) - 1; i >= 0; --i) {
            ast::Stmt nested{};
            nested.kind = ast::StmtKind::kIf;
            nested.expr = elifs[i].cond;
            nested.a = elifs[i].block;
            nested.b = tail_else;
            nested.span = span_join(
                elifs[i].span,
                (tail_else != ast::k_invalid_stmt) ? ast_.stmt(tail_else).span : ast_.stmt(elifs[i].block).span
            );
            tail_else = ast_.add_stmt(nested);
        }

        ast::Stmt root{};
        root.kind = ast::StmtKind::kIf;
        root.expr = cond0;
        root.a = then0;
        root.b = tail_else;
        root.span = span_join(
            if_kw.span,
            (tail_else != ast::k_invalid_stmt) ? ast_.stmt(tail_else).span : ast_.stmt(then0).span
        );
        return ast_.add_stmt(root);
    }

    // return 파싱
    ast::StmtId Parser::parse_stmt_return() {
        const Token kw = cursor_.bump();

        ast::ExprId v = ast::k_invalid_expr;
        Span fallback = kw.span;

        if (!cursor_.at(syntax::TokenKind::kSemicolon)) {
            v = parse_expr();
            fallback = ast_.expr(v).span;
        }

        const Span term_end = stmt_consume_semicolon_or_recover(fallback);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kReturn;
        s.expr = v;
        s.span = span_join(kw.span, term_end);
        return ast_.add_stmt(s);
    }

    // break 파싱
    ast::StmtId Parser::parse_stmt_break() {
        const Token kw = cursor_.bump();
        const Span term_end = stmt_consume_semicolon_or_recover(kw.span);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kBreak;
        s.span = span_join(kw.span, term_end);
        return ast_.add_stmt(s);
    }

    // continue 파싱
    ast::StmtId Parser::parse_stmt_continue() {
        const Token kw = cursor_.bump();
        const Span term_end = stmt_consume_semicolon_or_recover(kw.span);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kContinue;
        s.span = span_join(kw.span, term_end);
        return ast_.add_stmt(s);
    }

    // stmt 경계까지 스킵
    void Parser::stmt_sync_to_boundary() {
        while (!cursor_.at(syntax::TokenKind::kSemicolon) &&
               !cursor_.at(syntax::TokenKind::kRBrace) &&
               !cursor_.at(syntax::TokenKind::kEof)) {
            cursor_.bump();
        }
    }

    // ';'가 없으면 다음 경계까지 복구 후 가능한 경우 ';' 소비
    Span Parser::stmt_consume_semicolon_or_recover(Span fallback_end) {
        if (cursor_.at(syntax::TokenKind::kSemicolon)) {
            return cursor_.bump().span;
        }

        const Token t = cursor_.peek();
        diag_report(diag::Code::kExpectedToken, t.span,
                    syntax::token_kind_name(syntax::TokenKind::kSemicolon));

        Span last = fallback_end;
        while (!cursor_.at(syntax::TokenKind::kSemicolon) &&
               !cursor_.at(syntax::TokenKind::kRBrace) &&
               !cursor_.at(syntax::TokenKind::kEof)) {
            last = cursor_.bump().span;
        }

        if (cursor_.at(syntax::TokenKind::kSemicolon)) {
            last = cursor_.bump().span;
        }

        return last;
    }

} // namespace gaupel