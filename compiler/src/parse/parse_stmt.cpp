// compiler/src/parse/parse_stmt.cpp
#include <gaupel/parse/Parser.hpp>
#include <gaupel/syntax/TokenKind.hpp>

#include <vector>


namespace gaupel {
    
    ast::StmtId Parser::parse_stmt() {
        return parse_stmt_inner();
    }

    ast::StmtId Parser::parse_program() {
        uint32_t begin = static_cast<uint32_t>(ast_.stmt_children().size());
        uint32_t count = 0;

        Span first = cursor_.peek().span;
        Span last  = first;

        while (!cursor_.at(syntax::TokenKind::kEof)) {
            ast::StmtId s = parse_stmt_inner();
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

    bool Parser::is_fn_decl_start(syntax::TokenKind k) const {
        using K = syntax::TokenKind;
        return k == K::kAt
            || k == K::kKwExport
            || k == K::kKwFn
            || k == K::kKwPure
            || k == K::kKwComptime;
    }

    ast::StmtId Parser::parse_stmt_inner() {
        const auto& tok = cursor_.peek();

        if (is_fn_decl_start(tok.kind)) {
            return parse_fn_decl_stmt();
        }

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

        // keyword stmts
        if (tok.kind == syntax::TokenKind::kKwIf)       return parse_if_stmt();
        if (tok.kind == syntax::TokenKind::kKwWhile)    return parse_while_stmt();
        if (tok.kind == syntax::TokenKind::kKwReturn)   return parse_return_stmt();
        if (tok.kind == syntax::TokenKind::kKwBreak)    return parse_break_stmt();
        if (tok.kind == syntax::TokenKind::kKwContinue) return parse_continue_stmt();
        if (tok.kind == syntax::TokenKind::kKwLet
        ||  tok.kind == syntax::TokenKind::kKwSet)      return parse_var_stmt();

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
        s.stmt_begin = begin;
        s.stmt_count = count;
        return ast_.add_stmt(s);
    }

    ast::StmtId Parser::parse_expr_stmt() {
        const auto start_tok = cursor_.peek();
        ast::ExprId e = parse_expr();

        // ';' 누락 recovery의 fallback은 "expr의 끝"으로 설정
        const Span expr_end = ast_.expr(e).span;
        const Span term_end = consume_semicolon_or_recover(expr_end);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kExprStmt;
        s.span = span_join(start_tok.span, term_end);
        s.expr = e;
        return ast_.add_stmt(s);
    }

    ast::StmtId Parser::parse_required_block(std::string_view ctx) {
        (void)ctx;
        if (!cursor_.at(syntax::TokenKind::kLBrace)) {
            report(diag::Code::kExpectedToken, cursor_.peek().span, "{");

            // recovery: "블록이 없으면" 다음 stmt 경계까지는 스킵
            sync_to_stmt_boundary();
            if (cursor_.at(syntax::TokenKind::kSemicolon)) cursor_.bump();

            ast::Stmt s{};
            s.kind = ast::StmtKind::kBlock;
            s.span = cursor_.peek().span;
            s.stmt_begin = static_cast<uint32_t>(ast_.stmt_children().size());
            s.stmt_count = 0;
            return ast_.add_stmt(s);
        }
        return parse_block_stmt();
    }

    ast::StmtId Parser::parse_var_stmt() {
        const Token& kw = cursor_.peek();
        const bool is_set = (kw.kind == syntax::TokenKind::kKwSet);
        cursor_.bump(); // consume let/set

        // let mut x : T = expr;
        bool is_mut = false;
        if (cursor_.at(syntax::TokenKind::kKwMut)) {
            is_mut = true;
            cursor_.bump();
        }

        // name
        const Token& name_tok = cursor_.peek();
        std::string_view name{};
        if (name_tok.kind == syntax::TokenKind::kIdent) {
            name = name_tok.lexeme;
            cursor_.bump();
        } else {
            report(diag::Code::kUnexpectedToken, name_tok.span, "identifier");
        }

        ast::TypeId type_id = ast::k_invalid_type;

        // let requires ": Type"
        if (!is_set) {
            const Token& maybe_colon = cursor_.peek();
            if (!cursor_.at(syntax::TokenKind::kColon)) {
                // let must have type annotation in v0
                report(diag::Code::kUnexpectedToken, maybe_colon.span, "':' (type annotation required for let)");
            } else {
                cursor_.bump(); // ':'
                type_id = parse_type();
            }
        } else {
            // set forbids ": Type" (v0 rule)
            if (cursor_.at(syntax::TokenKind::kColon)) {
                report(diag::Code::kUnexpectedToken, cursor_.peek().span, "type annotation not allowed for set in v0");
                cursor_.bump();
                // best-effort: still parse a type and discard to keep cursor sane
                (void)parse_type();
            }
        }

        // initializer
        ast::ExprId init = ast::k_invalid_expr;

        if (cursor_.at(syntax::TokenKind::kAssign)) {
            const Token& eq = cursor_.peek();
            cursor_.bump(); // '='
            init = parse_expr();
            (void)eq;
        } else {
            // set requires initializer to infer type
            if (is_set) {
                report(diag::Code::kUnexpectedToken, cursor_.peek().span, "'=' initializer required for set");
            }
        }

        // semicolon
        const Span end = consume_semicolon_or_recover(cursor_.prev().span);

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

    ast::StmtId Parser::parse_while_stmt() {
        const Token kw = cursor_.bump(); // while

        ast::ExprId cond = parse_expr();
        ast::StmtId body = parse_required_block("while");

        ast::Stmt s{};
        s.kind = ast::StmtKind::kWhile;
        s.expr = cond;   // condition
        s.a = body;      // body block
        s.span = span_join(kw.span, ast_.stmt(body).span);
        return ast_.add_stmt(s);
    }

    ast::StmtId Parser::parse_if_stmt() {
        // parse first "if"
        const Token if_kw = cursor_.bump(); // if
        ast::ExprId cond0 = parse_expr();
        ast::StmtId then0 = parse_required_block("if");

        // parse trailing elif arms into temp vectors (local)
        struct ElifArm { ast::ExprId cond; ast::StmtId block; Span span; };
        std::vector<ElifArm> elifs;

        while (cursor_.at(syntax::TokenKind::kKwElif)) {
            const Token elif_kw = cursor_.bump();
            ast::ExprId c = parse_expr();
            ast::StmtId b = parse_required_block("elif");
            elifs.push_back(ElifArm{c, b, span_join(elif_kw.span, ast_.stmt(b).span)});
        }

        // optional else
        ast::StmtId else_block = ast::k_invalid_stmt;
        if (cursor_.at(syntax::TokenKind::kKwElse)) {
            cursor_.bump();
            else_block = parse_required_block("else");
        } else {
            // no else => synthesize empty block as else? (선택)
            // v0는 else가 없어도 괜찮으니 invalid로 둠.
        }

        // desugar: build from last elif backwards
        ast::StmtId tail_else = else_block;
        for (int i = static_cast<int>(elifs.size()) - 1; i >= 0; --i) {
            ast::Stmt nested{};
            nested.kind = ast::StmtKind::kIf;
            nested.expr = elifs[i].cond;
            nested.a = elifs[i].block;
            nested.b = tail_else; // may be invalid
            nested.span = span_join(elifs[i].span, (tail_else != ast::k_invalid_stmt) ? ast_.stmt(tail_else).span : ast_.stmt(elifs[i].block).span);
            tail_else = ast_.add_stmt(nested);
        }

        // final root if
        ast::Stmt root{};
        root.kind = ast::StmtKind::kIf;
        root.expr = cond0;
        root.a = then0;
        root.b = tail_else; // may be invalid
        root.span = span_join(if_kw.span, (tail_else != ast::k_invalid_stmt) ? ast_.stmt(tail_else).span : ast_.stmt(then0).span);
        return ast_.add_stmt(root);
    }

    ast::StmtId Parser::parse_return_stmt() {
        const Token kw = cursor_.bump(); // return

        ast::ExprId v = ast::k_invalid_expr;
        Span fallback = kw.span;

        if (!cursor_.at(syntax::TokenKind::kSemicolon)) {
            v = parse_expr();
            fallback = ast_.expr(v).span;
        }

        const Span term_end = consume_semicolon_or_recover(fallback);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kReturn;
        s.expr = v;
        s.span = span_join(kw.span, term_end);
        return ast_.add_stmt(s);
    }

    ast::StmtId Parser::parse_break_stmt() {
        const Token kw = cursor_.bump(); // break
        const Span term_end = consume_semicolon_or_recover(kw.span);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kBreak;
        s.span = span_join(kw.span, term_end);
        return ast_.add_stmt(s);
    }

    ast::StmtId Parser::parse_continue_stmt() {
        const Token kw = cursor_.bump(); // continue
        const Span term_end = consume_semicolon_or_recover(kw.span);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kContinue;
        s.span = span_join(kw.span, term_end);
        return ast_.add_stmt(s);
    }

    ast::StmtId Parser::parse_fn_decl_stmt() {
        using K = syntax::TokenKind;

        const Token start_tok = cursor_.peek();
        Span start = start_tok.span;
        
        bool is_export = false;
        bool is_pure = false;
        bool is_comptime = false;
        
        // header modifiers until 'fn'
        while (1) {
            if (cursor_.at(K::kAt)) {
                Token at = cursor_.bump();
                (void)at;

                // @pure / @comptime only (v0 minimal)
                if (cursor_.at(K::kKwPure)) { is_pure = true; cursor_.bump(); continue; }
                if (cursor_.at(K::kKwComptime)) { is_comptime = true; cursor_.bump(); continue; }

                // unknown attribute: consume one token to avoid infinite loop
                report(diag::Code::kUnexpectedToken, cursor_.peek().span, "attribute (pure/comptime)");
                cursor_.bump();
                continue;
            }

            if (cursor_.at(K::kKwPure)) { is_pure = true; cursor_.bump(); continue; }
            if (cursor_.at(K::kKwComptime)) { is_comptime = true; cursor_.bump(); continue; }
            if (cursor_.at(K::kKwExport)) { is_export = true; cursor_.bump(); continue; }

            break;
        }

        // require 'fn'
        if (!cursor_.at(K::kKwFn)) {
            report(diag::Code::kExpectedToken, cursor_.peek().span, "fn");
            // recovery: bail to stmt boundary
            sync_to_stmt_boundary();
            if (cursor_.at(K::kSemicolon)) cursor_.bump();

            ast::Stmt s{};
            s.kind = ast::StmtKind::kError;
            s.span = span_join(start, cursor_.prev().span);
            return ast_.add_stmt(s);
        }
        const Token fn_kw = cursor_.bump(); (void)fn_kw;

        // name
        std::string_view name{};
        const Token name_tok = cursor_.peek();
        if (name_tok.kind == K::kIdent) {
            name = name_tok.lexeme;
            cursor_.bump();
        } else {
            report(diag::Code::kUnexpectedToken, name_tok.span, "identifier (function name)");
        }

        // optional '?': fn name?
        bool is_throwing = false;
        if (cursor_.at(K::kQuestion)) {
            is_throwing = true;
            cursor_.bump();
        }

        // params: '(' (name ':' Type) (',' ...) ')'
        uint32_t param_begin = static_cast<uint32_t>(ast_.params().size());
        uint32_t param_count = 0;

        if (!cursor_.eat(K::kLParen)) {
            report(diag::Code::kExpectedToken, cursor_.peek().span, "(");
            recover_to_delim(K::kLParen, K::kArrow, K::kLBrace);
            cursor_.eat(K::kLParen);
        }

        if (!cursor_.at(K::kRParen)) {
            while (!cursor_.at(K::kRParen) && !cursor_.at(K::kEof)) {
                Token p0 = cursor_.peek();

                // param name
                std::string_view p_name{};
                if (p0.kind == K::kIdent) {
                    p_name = p0.lexeme;
                    cursor_.bump();
                } else {
                    report(diag::Code::kUnexpectedToken, p0.span, "identifier (param name)");
                    // recovery: to ',' or ')'
                    recover_to_delim(K::kComma, K::kRParen);
                    if (cursor_.eat(K::kComma)) continue;
                    break;
                }

                // ':'
                if (!cursor_.eat(K::kColon)) {
                    report(diag::Code::kExpectedToken, cursor_.peek().span, ":");
                    recover_to_delim(K::kComma, K::kRParen);
                    if (cursor_.eat(K::kComma)) continue;
                    // try continue anyway
                }

                // type
                ast::TypeId p_ty = parse_type();

                ast::Param p{};
                p.name = p_name;
                p.type = p_ty;
                p.span = span_join(p0.span, ast_.type_node(p_ty).span);
                ast_.add_param(p);
                ++param_count;

                if (cursor_.eat(K::kComma)) continue;
                break;
            }
        }

        if (!cursor_.eat(K::kRParen)) {
            report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
            recover_to_delim(K::kRParen, K::kArrow, K::kLBrace);
            cursor_.eat(K::kRParen);
        }

        // '->' ReturnType
        if (!cursor_.at(K::kArrow)) {
            // fallback: if lexer not updated, accept '-' '>' pair
            if (cursor_.at(K::kMinus) && cursor_.peek(1).kind == K::kGt) {
                cursor_.bump(); // '-'
                cursor_.bump(); // '>'
            } else {
                report(diag::Code::kExpectedToken, cursor_.peek().span, "->");
                recover_to_delim(K::kArrow, K::kLBrace, K::kSemicolon);
                cursor_.eat(K::kArrow);
            }
        } else {
            cursor_.bump(); // '->'
        }

        ast::TypeId ret_ty = parse_type();

        // body block (required)
        ast::StmtId body = parse_required_block("fn");

        // optional semicolon after function decl (accept both styles)
        Span end_sp = ast_.stmt(body).span;
        if (cursor_.at(K::kSemicolon)) {
            end_sp = cursor_.bump().span;
        }

        ast::Stmt s{};
        s.kind = ast::StmtKind::kFnDecl;
        s.span = span_join(start, end_sp);

        s.name = name;
        s.type = ret_ty;  // return type
        s.a = body;       // body block

        s.is_export = is_export;
        s.is_pure = is_pure;
        s.is_comptime = is_comptime;
        s.is_throwing = is_throwing;

        s.param_begin = param_begin;
        s.param_count = param_count;

        return ast_.add_stmt(s);
    }

    void Parser::sync_to_stmt_boundary() {
        // stmt 경계: ';' or '}' or EOF
        while (!cursor_.at(syntax::TokenKind::kSemicolon) &&
               !cursor_.at(syntax::TokenKind::kRBrace) &&
               !cursor_.at(syntax::TokenKind::kEof)) {
            cursor_.bump(); // 반드시 전진
        }
    }

    Span Parser::consume_semicolon_or_recover(Span fallback_end) {
        if (cursor_.at(syntax::TokenKind::kSemicolon)) {
            const Token semi = cursor_.bump();
            return semi.span;
        }

        // 여기서 ';'가 필요한 상황
        const Token t = cursor_.peek();
        report(diag::Code::kExpectedToken, t.span, syntax::token_kind_name(syntax::TokenKind::kSemicolon));

        // recovery: 다음 stmt 경계까지 스킵
        Span last = fallback_end;
        while (!cursor_.at(syntax::TokenKind::kSemicolon) &&
               !cursor_.at(syntax::TokenKind::kRBrace) &&
               !cursor_.at(syntax::TokenKind::kEof)) {
            last = cursor_.bump().span;
        }

        // ';'를 만나면 소비해서 다음 stmt 시작점을 안정화
        if (cursor_.at(syntax::TokenKind::kSemicolon)) {
            last = cursor_.bump().span;
        }

        return last;
    }

}