// compiler/src/parse/parse_stmt.cpp
#include <gaupel/parse/Parser.hpp>
#include <gaupel/syntax/TokenKind.hpp>

#include <vector>


namespace gaupel::detail {

    static bool is_case_pattern_tok(syntax::TokenKind k) {
        using K = syntax::TokenKind;

        return k == K::kIntLit || k == K::kCharLit || k == K::kStringLit
            || k == K::kKwTrue || k == K::kKwFalse || k == K::kKwNull
            || k == K::kIdent;
    }

    static ast::CasePatKind case_pat_kind_from_tok(const Token& t) {
        using K = syntax::TokenKind;

        switch (t.kind) {
            case K::kIntLit:    return ast::CasePatKind::kInt;
            case K::kCharLit:   return ast::CasePatKind::kChar;
            case K::kStringLit: return ast::CasePatKind::kString;
            case K::kKwTrue:
            case K::kKwFalse:   return ast::CasePatKind::kBool;
            case K::kKwNull:    return ast::CasePatKind::kNull;
            case K::kIdent:     return ast::CasePatKind::kIdent;
            default:            return ast::CasePatKind::kError;
        }
    }

} // namespace gaupel::detail

namespace gaupel {

    ast::StmtId Parser::parse_stmt() {
        return parse_stmt_any();
    }

    ast::StmtId Parser::parse_program() {
        // NOTE: 전역 stmt_children_에 즉시 push하지 말고,
        //       top-level stmt들을 로컬에 모았다가 마지막에 한 번에 커밋
        std::vector<ast::StmtId> top;
        top.reserve(64);

        Span first = cursor_.peek().span;
        Span last  = first;

        while (!cursor_.at(syntax::TokenKind::kEof)) {
            ast::StmtId s = parse_stmt_any();
            top.push_back(s);
            last = ast_.stmt(s).span;
        }

        if (top.empty()) {
            first = cursor_.peek().span;
            last  = first;
        }

        // commit (append in one contiguous slice)
        uint32_t begin = static_cast<uint32_t>(ast_.stmt_children().size());
        for (auto id : top) {
            ast_.add_stmt_child(id);
        }

        ast::Stmt root{};
        root.kind = ast::StmtKind::kBlock;
        root.span = span_join(first, last);
        root.stmt_begin = begin;
        root.stmt_count = static_cast<uint32_t>(top.size());
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
        if (tok.kind == syntax::TokenKind::kKwSwitch)   return parse_stmt_switch();
        if (tok.kind == syntax::TokenKind::kKwUse)      return parse_stmt_use();
        if (tok.kind == syntax::TokenKind::kKwLet
        ||  tok.kind == syntax::TokenKind::kKwSet)      return parse_stmt_var();
        if (tok.kind == syntax::TokenKind::kKwPub 
        || tok.kind == syntax::TokenKind::kKwSub) {
            diag_report(diag::Code::kPubSubOnlyAllowedInClass, tok.span);
            cursor_.bump(); // pub/sub 소비

            // 뒤가 fn/export/fn-attrs면 decl로 계속 파싱해서 연쇄 오류를 막는다.
            if (is_decl_start(cursor_.peek().kind)) {
                return parse_decl_any();
            }

            // 아니면 그냥 에러 stmt
            ast::Stmt s{};
            s.kind = ast::StmtKind::kError;
            s.span = tok.span;
            return ast_.add_stmt(s);
        }

        return parse_stmt_expr();
    }

    // '{ ... }' 블록 파싱
    ast::StmtId Parser::parse_stmt_block() {
        const Token lb = cursor_.peek();
        diag_expect(syntax::TokenKind::kLBrace);

        // NOTE: 자식 stmt를 전역 stmt_children_에 즉시 push하지 말고,
        //       로컬에 모았다가 '}'를 확인한 뒤 한 번에 커밋한다.
        std::vector<ast::StmtId> local;
        local.reserve(16);

        while (!cursor_.at(syntax::TokenKind::kRBrace) && !cursor_.at(syntax::TokenKind::kEof)) {
            ast::StmtId child = parse_stmt_any();
            local.push_back(child);
        }

        const Token rb = cursor_.peek();
        diag_expect(syntax::TokenKind::kRBrace);

        // commit (append in one contiguous slice)
        uint32_t begin = static_cast<uint32_t>(ast_.stmt_children().size());
        for (auto id : local) {
            ast_.add_stmt_child(id);
        }

        ast::Stmt s{};
        s.kind = ast::StmtKind::kBlock;
        s.span = span_join(lb.span, rb.span);
        s.stmt_begin = begin;
        s.stmt_count = static_cast<uint32_t>(local.size());
        return ast_.add_stmt(s);
    }

    // expr ';' 파싱
    ast::StmtId Parser::parse_stmt_expr() {
        const auto start_tok = cursor_.peek();
        ast::ExprId e = parse_expr();

        const Span expr_end = ast_.expr(e).span;

        // ---- "expr-with-block"는 세미콜론 없이 stmt로 허용 ----
        // loop { ... }
        // if cond { ... } else { ... }
        // { ... }  (block expr)
        //
        // 단, 일반 표현식은 기존처럼 ';' 필요.
        auto is_expr_with_block = [&](ast::ExprId id) -> bool {
            const auto& ex = ast_.expr(id);
            switch (ex.kind) {
                case ast::ExprKind::kLoop:
                case ast::ExprKind::kIfExpr:     // parse_expr_if에서 생성
                case ast::ExprKind::kBlockExpr:  // parse_expr_block에서 생성
                    return true;
                default:
                    return false;
            }
        };

        Span term_end = expr_end;

        if (cursor_.at(syntax::TokenKind::kSemicolon)) {
            term_end = cursor_.bump().span;
        } else {
            // 세미콜론이 없을 때:
            // - expr-with-block이면 OK
            // - 아니면 기존 복구 루틴(세미콜론 요구)
            if (!is_expr_with_block(e)) {
                term_end = stmt_consume_semicolon_or_recover(expr_end);
            } else {
                // allow no-semicolon
                term_end = expr_end;
            }
        }

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

            // NOTE: 빈 블록은 자식이 없으므로, 현재 전역 children size를 begin으로 잡고 count=0.
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

        auto type_id = ast::k_invalid_type;

        if (!is_set) {
            if (!cursor_.at(syntax::TokenKind::kColon)) {
                diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                            "':' (type annotation required for let)");
            } else {
                cursor_.bump();
                type_id = parse_type().id;
            }
        } else {
            if (cursor_.at(syntax::TokenKind::kColon)) {
                diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                            "type annotation not allowed for set");
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
        const Token kw = cursor_.bump(); // 'while'

        // expect '('
        bool has_paren = false;
        if (cursor_.eat(syntax::TokenKind::kLParen)) {
            has_paren = true;
        } else {
            diag_report(diag::Code::kWhileHeaderExpectedLParen, cursor_.peek().span);
        }

        ast::ExprId cond = parse_expr();

        // expect ')'
        if (has_paren) {
            if (!cursor_.eat(syntax::TokenKind::kRParen)) {
                diag_report(diag::Code::kWhileHeaderExpectedRParen, cursor_.peek().span);
                recover_to_delim(syntax::TokenKind::kRParen, syntax::TokenKind::kLBrace, syntax::TokenKind::kSemicolon);
                cursor_.eat(syntax::TokenKind::kRParen);
            }
        }

        // body must be a block (DEDICATED)
        ast::StmtId body{};
        if (!cursor_.at(syntax::TokenKind::kLBrace)) {
            diag_report(diag::Code::kWhileBodyExpectedBlock, cursor_.peek().span);
            stmt_sync_to_boundary();
            if (cursor_.at(syntax::TokenKind::kSemicolon)) cursor_.bump();

            ast::Stmt empty{};
            empty.kind = ast::StmtKind::kBlock;
            empty.span = cursor_.peek().span;
            empty.stmt_begin = static_cast<uint32_t>(ast_.stmt_children().size());
            empty.stmt_count = 0;
            body = ast_.add_stmt(empty);
        } else {
            body = parse_stmt_block();
        }

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

        ast::ExprId v = ast::k_invalid_expr;
        Span fallback = kw.span;

        // break <expr> ;  허용 (세미콜론 전까지 expr 존재하면 파싱)
        if (!cursor_.at(syntax::TokenKind::kSemicolon)) {
            v = parse_expr();
            fallback = ast_.expr(v).span;
        }

        const Span term_end = stmt_consume_semicolon_or_recover(fallback);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kBreak;
        s.expr = v; // 없으면 invalid
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

    ast::StmtId Parser::parse_stmt_switch() {
        using K = syntax::TokenKind;

        const Token sw = cursor_.peek();
        cursor_.bump(); // 'switch'

        if (!cursor_.eat(K::kLParen)) {
            diag_report(diag::Code::kSwitchHeaderExpectedLParen, cursor_.peek().span);
            recover_to_delim(K::kLParen, K::kLBrace, K::kSemicolon);
            cursor_.eat(K::kLParen);
        }

        ast::ExprId scrut = parse_expr();

        if (!cursor_.eat(K::kRParen)) {
            diag_report(diag::Code::kSwitchHeaderExpectedRParen, cursor_.peek().span);
            recover_to_delim(K::kRParen, K::kLBrace, K::kSemicolon);
            cursor_.eat(K::kRParen);
        }

        if (!cursor_.eat(K::kLBrace)) {
            diag_report(diag::Code::kSwitchBodyExpectedLBrace, cursor_.peek().span);
            recover_to_delim(K::kLBrace, K::kSemicolon, K::kRBrace);
            cursor_.eat(K::kLBrace);
        }

        const uint32_t case_begin = (uint32_t)ast_.switch_cases().size();
        uint32_t case_count = 0;
        bool has_default = false;

        auto parse_case_body_block = [&](std::string_view /*ctx*/) -> ast::StmtId {
            if (!cursor_.at(K::kLBrace)) {
                diag_report(diag::Code::kSwitchCaseBodyExpectedBlock, cursor_.peek().span);
                stmt_sync_to_boundary();
                if (cursor_.at(K::kSemicolon)) cursor_.bump();

                ast::Stmt empty{};
                empty.kind = ast::StmtKind::kBlock;
                empty.span = cursor_.peek().span;
                empty.stmt_begin = static_cast<uint32_t>(ast_.stmt_children().size());
                empty.stmt_count = 0;
                return ast_.add_stmt(empty);
            }
            return parse_stmt_block();
        };

        while (!cursor_.at(K::kRBrace) && !cursor_.at(K::kEof) && !is_aborted()) {
            const Token t = cursor_.peek();

            if (t.kind == K::kKwCase) {
                const Token case_kw = cursor_.bump();

                const Token pat = cursor_.peek();
                if (!detail::is_case_pattern_tok(pat.kind)) {
                    diag_report(diag::Code::kSwitchCaseExpectedPattern, pat.span);
                    recover_to_delim(K::kColon, K::kKwCase, K::kKwDefault);
                } else {
                    cursor_.bump();
                }

                if (!cursor_.eat(K::kColon)) {
                    diag_report(diag::Code::kSwitchCaseExpectedColon, cursor_.peek().span);
                    recover_to_delim(K::kColon, K::kLBrace, K::kKwCase);
                    cursor_.eat(K::kColon);
                }

                ast::StmtId body = parse_case_body_block("case");

                ast::SwitchCase c{};
                c.is_default = false;
                c.pat_kind = detail::case_pat_kind_from_tok(pat);
                c.pat_text = pat.lexeme;
                c.body = body;
                c.span = span_join(case_kw.span, ast_.stmt(body).span);

                ast_.add_switch_case(c);
                ++case_count;
                continue;
            }

            if (t.kind == K::kKwDefault) {
                const Token def_kw = cursor_.bump();

                if (has_default) {
                    diag_report(diag::Code::kSwitchDefaultDuplicate, def_kw.span);
                }
                has_default = true;

                if (!cursor_.eat(K::kColon)) {
                    diag_report(diag::Code::kSwitchCaseExpectedColon, cursor_.peek().span);
                    recover_to_delim(K::kColon, K::kLBrace, K::kKwCase);
                    cursor_.eat(K::kColon);
                }

                ast::StmtId body = parse_case_body_block("default");

                ast::SwitchCase c{};
                c.is_default = true;
                c.pat_kind = ast::CasePatKind::kError;
                c.body = body;
                c.span = span_join(def_kw.span, ast_.stmt(body).span);

                ast_.add_switch_case(c);
                ++case_count;
                continue;
            }

            // only case/default allowed
            diag_report(diag::Code::kSwitchOnlyCaseOrDefaultAllowed, t.span);
            recover_to_delim(K::kKwCase, K::kKwDefault, K::kRBrace);
            if (cursor_.at(K::kKwCase) || cursor_.at(K::kKwDefault) || cursor_.at(K::kRBrace)) continue;
            if (!cursor_.at(K::kEof)) cursor_.bump();
        }

        const Token rb = cursor_.peek();
        if (!cursor_.eat(K::kRBrace)) {
            diag_report(diag::Code::kSwitchBodyExpectedRBrace, cursor_.peek().span);
            recover_to_delim(K::kRBrace, K::kSemicolon);
            cursor_.eat(K::kRBrace);
        }

        if (case_count == 0) {
            diag_report(diag::Code::kSwitchNeedsAtLeastOneCase, rb.span);
        }

        ast::Stmt s{};
        s.kind = ast::StmtKind::kSwitch;
        s.expr = scrut;
        s.case_begin = case_begin;
        s.case_count = case_count;
        s.has_default = has_default;
        s.span = span_join(sw.span, cursor_.prev().span);
        return ast_.add_stmt(s);
    }

    std::pair<uint32_t, uint32_t> Parser::parse_path_segments() {
        using K = syntax::TokenKind;

        // Path := Ident ('::' Ident)*
        uint32_t begin = (uint32_t)ast_.path_segs().size();
        uint32_t count = 0;

        const Token first = cursor_.peek();
        if (first.kind != K::kIdent) {
            diag_report(diag::Code::kUnexpectedToken, first.span, "identifier (path segment)");
            return { begin, count };
        }

        cursor_.bump();
        ast_.add_path_seg(first.lexeme);
        ++count;

        while (cursor_.at(K::kColonColon)) {
            cursor_.bump(); // ::

            const Token seg = cursor_.peek();
            if (seg.kind != K::kIdent) {
                diag_report(diag::Code::kUnexpectedToken, seg.span, "identifier (path segment)");
                break;
            }
            cursor_.bump();
            ast_.add_path_seg(seg.lexeme);
            ++count;
        }

        return { begin, count };
    }

    std::string_view Parser::parse_module_path_to_string(bool& out_is_angle) {
        using K = syntax::TokenKind;
        out_is_angle = false;

        // ModulePath := "<...>" | StringLit
        if (cursor_.at(K::kStringLit)) {
            const Token s = cursor_.bump();
            return s.lexeme; // lexer가 따옴표 포함/미포함 어느쪽이든 그대로 보관
        }

        // '<' 는 TokenKind::kLt 로 들어옴
        if (!cursor_.at(K::kLt)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "< or string literal");
            return {};
        }

        out_is_angle = true;
        cursor_.bump(); // '<'

        std::string acc;

        while (!cursor_.at(K::kGt) && !cursor_.at(K::kEof)) {
            const Token t = cursor_.bump();

            // lexeme이 있으면 그대로, 없으면 kind name 사용(예: eof 같은)
            if (!t.lexeme.empty()) acc.append(t.lexeme.data(), t.lexeme.size());
            else {
                auto nm = syntax::token_kind_name(t.kind);
                acc.append(nm.data(), nm.size());
            }
        }

        if (!cursor_.eat(K::kGt)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ">");
            recover_to_delim(K::kGt, K::kSemicolon);
            cursor_.eat(K::kGt);
        }

        return ast_.add_owned_string(std::move(acc));
    }

    ty::TypeId Parser::parse_ffi_signature_type() {
        using K = syntax::TokenKind;
        // FFISignature := Type "(" (Type ("," Type)*)? ")"
        // 전체는 use func::ffi "<" FFISignature ">" Ident;
        // 여기서는 "<" 를 이미 소비한 상태라고 가정하지 않고, 호출자가 관리.
        auto ret = parse_type();
        if (ret.id == ty::kInvalidType) ret.id = types_.error();

        if (!cursor_.eat(K::kLParen)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "(");
            recover_to_delim(K::kLParen, K::kGt, K::kRParen);
            cursor_.eat(K::kLParen);
        }

        // params (Type list)
        std::vector<ty::TypeId> ps;

        if (!cursor_.at(K::kRParen)) {
            while (!cursor_.at(K::kRParen) && !cursor_.at(K::kEof)) {
                auto pt = parse_type();
                if (pt.id == ty::kInvalidType) pt.id = types_.error();
                ps.push_back(pt.id);

                if (cursor_.eat(K::kComma)) {
                    if (cursor_.at(K::kRParen)) break;
                    continue;
                }
                break;
            }
        }

        if (!cursor_.eat(K::kRParen)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
            recover_to_delim(K::kRParen, K::kGt);
            cursor_.eat(K::kRParen);
        }

        return types_.make_fn(ret.id, ps.data(), (uint32_t)ps.size());
    }

    void Parser::parse_ffi_struct_body(uint32_t& out_begin, uint32_t& out_count) {
        using K = syntax::TokenKind;
        out_begin = (uint32_t)ast_.ffi_fields().size();
        out_count = 0;
        
        if (!cursor_.eat(K::kLBrace)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "{");
            recover_to_delim(K::kLBrace, K::kSemicolon, K::kRBrace);
            cursor_.eat(K::kLBrace);
        }

        while (!cursor_.at(K::kRBrace) && !cursor_.at(K::kEof)) {
            auto ft = parse_type();

            const Token name = cursor_.peek();
            if (name.kind != K::kIdent) {
                diag_report(diag::Code::kUnexpectedToken, name.span, "identifier (ffi field name)");
                recover_to_delim(K::kSemicolon, K::kRBrace);
                cursor_.eat(K::kSemicolon);
                continue;
            }
            cursor_.bump();

            Span end = name.span;

            if (!cursor_.eat(K::kSemicolon)) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ";");
                recover_to_delim(K::kSemicolon, K::kRBrace);
                cursor_.eat(K::kSemicolon);
            } else {
                end = cursor_.prev().span;
            }

            ast::FfiField f{};
            f.type = (ft.id == ty::kInvalidType) ? types_.error() : ft.id;
            f.name = name.lexeme;
            f.span = span_join(ft.span, end);

            ast_.add_ffi_field(f);
            ++out_count;
        }

        if (!cursor_.eat(K::kRBrace)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "}");
            recover_to_delim(K::kRBrace, K::kSemicolon);
            cursor_.eat(K::kRBrace);
        }
    }

    // stmt 경계까지 스킵
    void Parser::stmt_sync_to_boundary() {
        while (!cursor_.at(syntax::TokenKind::kSemicolon) &&
               !cursor_.at(syntax::TokenKind::kRBrace) &&
               !cursor_.at(syntax::TokenKind::kEof)) {
            cursor_.bump();
        }
    }

    ast::StmtId Parser::parse_stmt_use() {
        using K = syntax::TokenKind;
        const Token use_kw = cursor_.bump(); // 'use'
    
        ast::Stmt s{};
        s.kind = ast::StmtKind::kUse;
        s.span = use_kw.span;
        s.use_kind = ast::UseKind::kError;

        // ---- lookahead: "module" ----
        if (cursor_.at(K::kKwModule)) {
            cursor_.bump(); // module

            bool is_angle = false;
            std::string_view mpath = parse_module_path_to_string(is_angle);

            if (!cursor_.eat(K::kKwAs)) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "as");
                recover_to_delim(K::kKwAs, K::kSemicolon);
                cursor_.eat(K::kKwAs);
            }

            const Token al = cursor_.peek();
            if (al.kind != K::kIdent) {
                diag_report(diag::Code::kUnexpectedToken, al.span, "identifier (module alias)");
            } else {
                cursor_.bump();
                s.use_kind = ast::UseKind::kModuleImport;
                s.use_module_path = mpath;
                s.use_module_is_angle = is_angle;
                s.use_module_alias = al.lexeme;
            }

            Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);
            s.span = span_join(use_kw.span, end);
            return ast_.add_stmt(s);
        }

        // ---- FFI: func::ffi / struct::ffi ----
        // pattern: Ident '::' Ident ...
        if (cursor_.peek().kind == K::kIdent && cursor_.peek(1).kind == K::kColonColon && cursor_.peek(2).kind == K::kIdent) {
            const Token head = cursor_.peek();       // func / struct
            const Token tail = cursor_.peek(2);      // ffi
            const bool is_func_ffi = (head.lexeme == "func"   && tail.lexeme == "ffi");
            const bool is_struct_ffi= (head.lexeme == "struct" && tail.lexeme == "ffi");

            if (is_func_ffi) {
                cursor_.bump(); // func
                cursor_.bump(); // ::
                cursor_.bump(); // ffi

                if (!cursor_.eat(K::kLt)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "<");
                    recover_to_delim(K::kLt, K::kSemicolon, K::kGt);
                    cursor_.eat(K::kLt);
                }

                ty::TypeId sig = parse_ffi_signature_type();

                if (!cursor_.eat(K::kGt)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ">");
                    recover_to_delim(K::kGt, K::kSemicolon);
                    cursor_.eat(K::kGt);
                }

                const Token fname = cursor_.peek();
                if (fname.kind != K::kIdent) {
                    diag_report(diag::Code::kUnexpectedToken, fname.span, "identifier (ffi function name)");
                } else {
                    cursor_.bump();
                    s.use_kind = ast::UseKind::kFFIFunc;
                    s.use_name = fname.lexeme;
                    s.type = sig; // fn signature type id
                }

                Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);
                s.span = span_join(use_kw.span, end);
                return ast_.add_stmt(s);
            }

            if (is_struct_ffi) {
                cursor_.bump(); // struct
                cursor_.bump(); // ::
                cursor_.bump(); // ffi

                const Token sname = cursor_.peek();
                if (sname.kind != K::kIdent) {
                    diag_report(diag::Code::kUnexpectedToken, sname.span, "identifier (ffi struct name)");
                } else {
                    cursor_.bump();
                    s.use_kind = ast::UseKind::kFFIStruct;
                    s.use_name = sname.lexeme;
                }

                uint32_t fb = 0, fc = 0;
                parse_ffi_struct_body(fb, fc);
                s.use_field_begin = fb;
                s.use_field_count = fc;

                Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);
                s.span = span_join(use_kw.span, end);
                return ast_.add_stmt(s);
            }
        }

        // ---- common forms start with Ident ----
        const Token first = cursor_.peek();
        if (first.kind != K::kIdent) {
            diag_report(diag::Code::kUnexpectedToken, first.span, "identifier (use target)");
            cursor_.bump();
            Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);
            s.span = span_join(use_kw.span, end);
            return ast_.add_stmt(s);
        }

        // We need to decide among:
        // 1) TypeAlias: Ident '=' Type
        // 2) TextSubst: Ident Expr
        // 3) PathAlias: Path '=' Ident   (Path starts with Ident and may include ::)
        //
        // We'll parse a path first (Ident + ::...).
        auto [pb, pc] = parse_path_segments();

        // If next is '=' => PathAlias (if pc>=2) OR TypeAlias (if pc==1 and lhs is single ident)
        if (cursor_.at(K::kAssign)) {
            cursor_.bump(); // '='

            if (pc == 1) {
                // could be type alias: use Name = Type;
                auto ty = parse_type();
                s.use_kind = ast::UseKind::kTypeAlias;
                s.use_name = ast_.path_segs()[pb]; // lhs ident
                s.type = (ty.id == ty::kInvalidType) ? types_.error() : ty.id;

                Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);
                s.span = span_join(use_kw.span, end);
                return ast_.add_stmt(s);
            } else {
                // path alias: use A::B = name;
                const Token rhs = cursor_.peek();
                if (rhs.kind != K::kIdent) {
                    diag_report(diag::Code::kUnexpectedToken, rhs.span, "identifier (path alias target)");
                } else {
                    cursor_.bump();
                    s.use_kind = ast::UseKind::kPathAlias;
                    s.use_path_begin = pb;
                    s.use_path_count = pc;
                    s.use_rhs_ident = rhs.lexeme;
                }

                Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);
                s.span = span_join(use_kw.span, end);
                return ast_.add_stmt(s);
            }
        }

        // Otherwise: TextSubst must have exactly one ident on lhs (not a path)
        if (pc != 1) {
            diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span, "expected '=' for path alias");
            Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);
            s.span = span_join(use_kw.span, end);
            return ast_.add_stmt(s);
        }

        // TextSubst: use NAME Expr;
        s.use_kind = ast::UseKind::kTextSubst;
        s.use_name = ast_.path_segs()[pb];

        // expr가 없으면(바로 ';') 진단
        if (cursor_.at(K::kSemicolon)) {
            diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span, "expression (use substitution)");
            cursor_.bump(); // consume ';'
            s.span = span_join(use_kw.span, cursor_.prev().span);
            return ast_.add_stmt(s);
        }

        s.expr = parse_expr();
        Span end = stmt_consume_semicolon_or_recover(ast_.expr(s.expr).span);
        s.span = span_join(use_kw.span, end);
        return ast_.add_stmt(s);
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