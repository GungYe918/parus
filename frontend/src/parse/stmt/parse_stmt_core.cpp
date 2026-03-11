// frontend/src/parse/parse_stmt_core.cpp
#include <parus/parse/Parser.hpp>
#include <parus/syntax/TokenKind.hpp>

#include <vector>


namespace parus::detail {

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
} // namespace parus::detail

namespace parus {
    
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
            if (aborted_) break;

            const size_t before = cursor_.pos();
            ast::StmtId s = parse_stmt_any();
            if (s != ast::k_invalid_stmt) {
                top.push_back(s);
                last = ast_.stmt(s).span;
            }

            if (aborted_) break;
            if (cursor_.pos() == before && !cursor_.at(syntax::TokenKind::kEof)) {
                const auto stuck = cursor_.peek();
                diag_report(diag::Code::kUnexpectedToken, stuck.span, syntax::token_kind_name(stuck.kind));
                cursor_.bump();
            }
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
        using K = syntax::TokenKind;

        // compiler directives:
        // - $[If(cond)] <item-decl>
        // - $[Foo(...)] <item-decl>
        // - $![key.path = target::path];
        if (tok.kind == K::kDollar && cursor_.peek(1).kind == K::kLBracket) {
            return parse_stmt_compiler_directive();
        }
        if (tok.kind == K::kDollar &&
            cursor_.peek(1).kind == K::kBang &&
            cursor_.peek(2).kind == K::kLBracket) {
            return parse_stmt_compiler_intrinsic_directive();
        }

        // decl start => decl 파서로 위임
        if (is_decl_start(tok.kind)) {
            return parse_decl_any();
        }

        /// @brief `static/let/set` 또는 잘못된 `mut <...>` 패턴이 변수 선언 시작인지 lookahead로 판정한다.
        const auto is_var_stmt_start_lookahead = [&](uint32_t off) -> bool {
            const K k = cursor_.peek(off).kind;
            if (k == K::kKwStatic || k == K::kKwConst || k == K::kKwLet || k == K::kKwSet) return true;
            if (k == K::kKwMut) {
                const K k1 = cursor_.peek(off + 1).kind;
                return (k1 == K::kKwStatic || k1 == K::kKwConst || k1 == K::kKwLet || k1 == K::kKwSet);
            }
            return false;
        };

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
            // NOTE:
            // - 하위 호환을 위해 단독 '{ ... }' 블록 문장은 유지한다.
            // - 다만 가독성을 위해 일반 스코프는 'do { ... }'를 권장한다.
            diag_report_warn(diag::Code::kBareBlockScopePreferDo, tok.span);
            return parse_stmt_block();
        }

        // keyword stmts
        if (tok.kind == K::kKwIf)       return parse_stmt_if();
        if (tok.kind == K::kKwWhile)    return parse_stmt_while();
        if (tok.kind == K::kKwDo)       return parse_stmt_do();
        if (tok.kind == K::kKwManual)   return parse_stmt_manual();
        if (tok.kind == K::kKwReturn)   return parse_stmt_return();
        if (tok.kind == K::kKwRequire)  return parse_stmt_require();
        if (tok.kind == K::kKwThrow)    return parse_stmt_throw();
        if (tok.kind == K::kKwBreak)    return parse_stmt_break();
        if (tok.kind == K::kKwContinue) return parse_stmt_continue();
        if (tok.kind == K::kKwTry && cursor_.peek(1).kind == K::kLBrace) return parse_stmt_try_catch();
        if (tok.kind == K::kKwCommit) return parse_stmt_commit();
        if (tok.kind == K::kKwRecast) return parse_stmt_recast();
        if (tok.kind == K::kKwSwitch)   return parse_stmt_switch();
        if (tok.kind == K::kKwUse)      return parse_stmt_use();
        if (tok.kind == K::kKwImport)   return parse_stmt_import();

        if (tok.kind == K::kKwConst && cursor_.peek(1).kind == K::kKwFn) return parse_decl_fn();
        if (tok.kind == K::kKwStatic) return parse_stmt_var();
        if (tok.kind == K::kKwConst) return parse_stmt_var();
        if (tok.kind == K::kKwLet || tok.kind == K::kKwSet) return parse_stmt_var();
        if (tok.kind == K::kKwMut && is_var_stmt_start_lookahead(/*off=*/0)) return parse_stmt_var();

        return parse_stmt_expr();
    }

    ast::StmtId Parser::parse_stmt_compiler_directive() {
        using K = syntax::TokenKind;

        const Token start = cursor_.peek();
        if (!cursor_.eat(K::kDollar)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "$");
            return ast::k_invalid_stmt;
        }
        if (!cursor_.eat(K::kLBracket)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "[");
            recover_to_delim(K::kRBracket, K::kSemicolon, K::kRBrace);
            (void)cursor_.eat(K::kRBracket);
            return ast::k_invalid_stmt;
        }
        ast::ExprId call = ast::k_invalid_expr;
        if (cursor_.at(K::kKwIf)) {
            diag_report(diag::Code::kDirectiveLegacyIfRemoved, cursor_.peek().span);
            cursor_.bump(); // if
            if (cursor_.eat(K::kLParen)) {
                if (!cursor_.at(K::kRParen) && !cursor_.at(K::kEof) && !cursor_.at(K::kRBracket)) {
                    (void)parse_expr();
                }
                if (!cursor_.eat(K::kRParen)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
                    recover_to_delim(K::kRParen, K::kRBracket, K::kSemicolon);
                    (void)cursor_.eat(K::kRParen);
                }
            }
        } else if (!cursor_.at(K::kRBracket) && !cursor_.at(K::kSemicolon) && !cursor_.at(K::kEof)) {
            call = parse_expr();
        }

        if (!cursor_.eat(K::kRBracket)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "]");
            recover_to_delim(K::kRBracket, K::kSemicolon, K::kRBrace);
            (void)cursor_.eat(K::kRBracket);
        }

        bool call_ok = true;
        if (call == ast::k_invalid_expr || (size_t)call >= ast_.exprs().size()) {
            call_ok = false;
        } else {
            const auto& ce = ast_.expr(call);
            if (ce.kind != ast::ExprKind::kCall) {
                call_ok = false;
            }
        }
        if (!call_ok) {
            diag_report(diag::Code::kDirectiveCallExpected, start.span);
        }

        if (!is_decl_start(cursor_.peek().kind)) {
            diag_report(diag::Code::kDirectiveItemExpected, cursor_.peek().span);
            if (!cursor_.at(K::kSemicolon) && !cursor_.at(K::kRBrace) && !cursor_.at(K::kEof)) {
                stmt_sync_to_boundary();
            }
            if (cursor_.at(K::kSemicolon)) (void)cursor_.bump();
            return ast::k_invalid_stmt;
        }

        const ast::StmtId item_sid = parse_decl_any();
        if (!call_ok || item_sid == ast::k_invalid_stmt) {
            return ast::k_invalid_stmt;
        }

        ast::Stmt s{};
        s.kind = ast::StmtKind::kCompilerDirective;
        s.span = span_join(start.span, ast_.stmt(item_sid).span);
        s.expr = call;
        s.a = item_sid;
        return ast_.add_stmt(s);
    }

    ast::StmtId Parser::parse_stmt_compiler_intrinsic_directive() {
        using K = syntax::TokenKind;

        const Token start = cursor_.peek();

        (void)cursor_.eat(K::kDollar);
        if (!cursor_.eat(K::kBang)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "!");
            return ast::k_invalid_stmt;
        }
        if (!cursor_.eat(K::kLBracket)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "[");
            recover_to_delim(K::kRBracket, K::kSemicolon, K::kRBrace);
            (void)cursor_.eat(K::kRBracket);
            return ast::k_invalid_stmt;
        }

        uint32_t key_begin = 0;
        uint32_t key_count = 0;
        Span key_span = start.span;
        const bool key_ok = parse_compiler_directive_path(key_begin, key_count, key_span);

        if (!cursor_.eat(K::kAssign)) {
            diag_report(diag::Code::kDirectiveIntrinsicSyntax, cursor_.peek().span, "expected '='");
            recover_to_delim(K::kAssign, K::kRBracket, K::kSemicolon);
            (void)cursor_.eat(K::kAssign);
        }

        uint32_t target_begin = 0;
        uint32_t target_count = 0;
        Span target_span = key_span;
        const bool target_ok = parse_compiler_directive_path(target_begin, target_count, target_span);

        if (!cursor_.eat(K::kRBracket)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "]");
            recover_to_delim(K::kRBracket, K::kSemicolon, K::kRBrace);
            (void)cursor_.eat(K::kRBracket);
        }

        Span end = target_ok ? target_span : key_span;
        end = stmt_consume_semicolon_or_recover(end);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kCompilerIntrinsicDirective;
        s.span = span_join(start.span, end);
        if (key_ok) {
            s.directive_key_path_begin = key_begin;
            s.directive_key_path_count = key_count;
        }
        if (target_ok) {
            s.directive_target_path_begin = target_begin;
            s.directive_target_path_count = target_count;
        }
        const ast::StmtId sid = ast_.add_stmt(s);
        diag_report_warn(diag::Code::kDirectiveIntrinsicPolicyPending, s.span);
        return sid;
    }
    // '{ ... }' 블록 파싱
    ast::StmtId Parser::parse_stmt_block(bool allow_macro_decl) {
        const Token lb = cursor_.peek();
        diag_expect(syntax::TokenKind::kLBrace);

        // NOTE: 자식 stmt를 전역 stmt_children_에 즉시 push하지 말고,
        //       로컬에 모았다가 '}'를 확인한 뒤 한 번에 커밋한다.
        std::vector<ast::StmtId> local;
        local.reserve(16);

        const uint32_t prev_depth = macro_scope_depth_;
        if (allow_macro_decl) {
            macro_scope_depth_ = prev_depth + 1;
        }

        while (!cursor_.at(syntax::TokenKind::kRBrace) && !cursor_.at(syntax::TokenKind::kEof)) {
            if (aborted_) break;

            const size_t before = cursor_.pos();
            if (is_macro_decl_start() && !allow_macro_decl) {
                diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                            "macro declaration is only allowed at top-level or nest body");
                (void)parse_decl_macro();
                continue;
            }

            ast::StmtId child = parse_stmt_any();
            if (child != ast::k_invalid_stmt) {
                local.push_back(child);
            }

            if (aborted_) break;
            if (cursor_.pos() == before
                && !cursor_.at(syntax::TokenKind::kRBrace)
                && !cursor_.at(syntax::TokenKind::kEof)) {
                const auto stuck = cursor_.peek();
                diag_report(diag::Code::kUnexpectedToken, stuck.span, syntax::token_kind_name(stuck.kind));
                cursor_.bump();
            }
        }

        const Token rb = cursor_.peek();
        diag_expect(syntax::TokenKind::kRBrace);
        macro_scope_depth_ = prev_depth;

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

    // let/set/static 변수 선언 파싱
    ast::StmtId Parser::parse_stmt_var() {
        using K = syntax::TokenKind;

        const Token start_tok = cursor_.peek();

        bool mut_prefix_invalid = false;
        if (cursor_.at(K::kKwMut)) {
            // v0 규칙:
            // - 가변성 표기는 선언 키워드 뒤에만 허용한다.
            //   * let mut x: T = ...
            //   * set mut x = ...
            //   * static mut x: T = ...
            diag_report(diag::Code::kVarMutMustFollowKw, cursor_.peek().span);
            cursor_.bump();
            mut_prefix_invalid = true;
        }

        bool is_static = false;
        bool is_mut = false;
        bool is_set = false;
        bool is_const = false;

        // -------- declaration keyword --------
        if (cursor_.at(K::kKwConst)) {
            is_const = true;
            cursor_.bump(); // const

            if (cursor_.at(K::kKwMut)) {
                diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                            "const declaration cannot use 'mut'");
                cursor_.bump();
            }

            if (cursor_.at(K::kKwStatic)) {
                diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                            "use 'static const ...' order (not 'const static ...')");
                is_static = true;
                cursor_.bump(); // static
            }
        } else if (cursor_.at(K::kKwStatic)) {
            is_static = true;
            cursor_.bump(); // static

            if (cursor_.at(K::kKwConst)) {
                is_const = true;
                cursor_.bump(); // static const
            }

            if (cursor_.at(K::kKwMut)) {
                if (is_const) {
                    diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                                "const declaration cannot use 'mut'");
                    cursor_.bump();
                } else {
                    is_mut = true;
                    cursor_.bump(); // static mut
                }
            }

            // 구문 정리: static은 let/set을 수반하지 않는다.
            if (cursor_.at(K::kKwLet) || cursor_.at(K::kKwSet)) {
                diag_report(
                    diag::Code::kUnexpectedToken,
                    cursor_.peek().span,
                    "remove 'let/set' after 'static' (use: static [mut] name: T = expr;)"
                );
                cursor_.bump();
                if (cursor_.at(K::kKwMut)) {
                    diag_report(diag::Code::kVarMutMustFollowKw, cursor_.peek().span);
                    cursor_.bump();
                }
            }
        } else if (cursor_.at(K::kKwLet) || cursor_.at(K::kKwSet)) {
            const Token kw = cursor_.bump();
            is_set = (kw.kind == K::kKwSet);

            if (cursor_.at(K::kKwMut)) {
                is_mut = true;
                cursor_.bump();
            }
        } else {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "'const', 'static', 'let' or 'set'");
            stmt_sync_to_boundary();
            if (cursor_.at(K::kSemicolon)) cursor_.bump();

            ast::Stmt s{};
            s.kind = ast::StmtKind::kError;
            s.span = span_join(start_tok.span, cursor_.prev().span);
            return ast_.add_stmt(s);
        }

        if (is_const) {
            if (is_set) {
                diag_report(diag::Code::kUnexpectedToken, start_tok.span,
                            "const declaration cannot use 'set'");
                is_set = false;
            }
            if (is_mut) {
                diag_report(diag::Code::kUnexpectedToken, start_tok.span,
                            "const declaration cannot use 'mut'");
                is_mut = false;
            }
            if (cursor_.at(K::kKwLet) || cursor_.at(K::kKwSet)) {
                diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                            "remove 'let/set' from const declaration (use: const name: T = expr;)");
                cursor_.bump();
                if (cursor_.at(K::kKwMut)) {
                    diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                                "const declaration cannot use 'mut'");
                    cursor_.bump();
                }
            }
        }

        // prefix 'mut'가 있었더라도 선언 자체는 계속 파싱하여 연쇄 오류를 줄인다.
        (void)mut_prefix_invalid;

        // ---- name ----
        std::string_view name{};
        const Token name_tok = cursor_.peek();
        if (name_tok.kind == K::kIdent) {
            name = name_tok.lexeme;
            cursor_.bump();
        } else {
            // 이름이 없으면 이후 전체가 꼬이기 쉬우므로 강하게 복구한다.
            diag_report(diag::Code::kVarDeclNameExpected, name_tok.span);

            // stmt 경계까지 정렬
            stmt_sync_to_boundary();
            if (cursor_.at(K::kSemicolon)) cursor_.bump();

            ast::Stmt s{};
            s.kind = ast::StmtKind::kError;
            s.span = span_join(start_tok.span, cursor_.prev().span);
            return ast_.add_stmt(s);
        }

        // ---- type annotation ----
        ast::TypeId type_id = ast::k_invalid_type;
        ast::TypeNodeId type_node = ast::k_invalid_type_node;

        if (is_const || is_static || !is_set) {
            // let: ':' required
            if (!cursor_.eat(K::kColon)) {
                diag_report(diag::Code::kVarDeclTypeAnnotationRequired, cursor_.peek().span);

                // recovery:
                // - 다음이 '=' 이면 "타입 누락 + 초기화만 존재" 케이스
                // - 아니면 ';' 또는 '}' 까지 스킵
                if (!cursor_.at(K::kAssign)) {
                    recover_to_delim(K::kAssign, K::kSemicolon, K::kRBrace);
                    cursor_.eat(K::kAssign); // 있으면 이후 init 파싱으로 연결
                }
            } else {
                // parse type normally
                auto parsed = parse_type();
                type_id = parsed.id;
                type_node = parsed.node;
            }
        } else {
            // set: ':' not allowed
            if (cursor_.at(K::kColon)) {
                diag_report(diag::Code::kVarDeclTypeAnnotationNotAllowed, cursor_.peek().span);
                cursor_.bump(); // ':'
                (void)parse_type(); // consume and discard to keep stream stable
            }
        }

        // ---- optional binding acts sugar ----
        // canonical:
        //   let/set ... = Expr with acts(NameOrDefault);
        //
        // legacy (removed):
        //   let/set ... with acts(NameOrDefault) = Expr;
        bool var_has_acts_binding = false;
        bool var_acts_is_default = false;
        ast::TypeId var_acts_target_type = ast::k_invalid_type;
        ast::TypeNodeId var_acts_target_type_node = ast::k_invalid_type_node;
        uint32_t var_acts_set_path_begin = 0;
        uint32_t var_acts_set_path_count = 0;
        std::string_view var_acts_set_name{};

        const auto is_with_token = [](const Token& tok) -> bool {
            return tok.kind == K::kKwWith;
        };
        const auto parse_with_acts_clause = [&](bool record_binding, bool allow_assign_recovery) {
            if (!(is_with_token(cursor_.peek()) && cursor_.peek(1).kind == K::kKwActs)) return false;
            cursor_.bump(); // with
            cursor_.bump(); // acts

            bool is_default = false;
            uint32_t set_path_begin = 0;
            uint32_t set_path_count = 0;
            std::string_view set_name{};

            if (!cursor_.eat(K::kLParen)) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "(");
                if (allow_assign_recovery) {
                    recover_to_delim(K::kRParen, K::kAssign, K::kSemicolon);
                } else {
                    recover_to_delim(K::kRParen, K::kSemicolon);
                }
            }

            if (cursor_.at(K::kKwDefault)) {
                is_default = true;
                set_name = "default";
                cursor_.bump();
            } else {
                auto [sb, sc] = parse_path_segments();
                set_path_begin = sb;
                set_path_count = sc;
                if (sc > 0) {
                    const auto& segs = ast_.path_segs();
                    set_name = segs[sb + sc - 1];
                } else {
                    diag_report(diag::Code::kActsNameExpected, cursor_.peek().span);
                }
            }

            if (!cursor_.eat(K::kRParen)) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
                if (allow_assign_recovery) {
                    recover_to_delim(K::kRParen, K::kAssign, K::kSemicolon);
                } else {
                    recover_to_delim(K::kRParen, K::kSemicolon);
                }
                cursor_.eat(K::kRParen);
            }

            if (record_binding) {
                var_has_acts_binding = true;
                var_acts_is_default = is_default;
                var_acts_set_path_begin = set_path_begin;
                var_acts_set_path_count = set_path_count;
                var_acts_set_name = set_name;
                if (type_id != ast::k_invalid_type) {
                    var_acts_target_type = type_id;
                    var_acts_target_type_node = type_node;
                }
            }
            return true;
        };

        // legacy placement: `... with acts(...) = expr;`
        if (is_with_token(cursor_.peek()) && cursor_.peek(1).kind == K::kKwActs) {
            diag_report(
                diag::Code::kUnexpectedToken,
                cursor_.peek().span,
                "binding acts clause must appear after initializer (use: 'name = expr with acts(NameOrDefault);')"
            );
            (void)parse_with_acts_clause(/*record_binding=*/false, /*allow_assign_recovery=*/true);
        }

        // ---- initializer ----
        ast::ExprId init = ast::k_invalid_expr;

        bool static_init_diag_emitted = false;
        if (cursor_.eat(K::kAssign)) {
            // "= <expr>"
            // (바로 ';'면 expr 누락)
            if (cursor_.at(K::kSemicolon) || cursor_.at(K::kRBrace) || cursor_.at(K::kEof)) {
                if (is_static) {
                    diag_report(diag::Code::kStaticVarRequiresInitializer, cursor_.peek().span);
                    static_init_diag_emitted = true;
                } else {
                    diag_report(diag::Code::kVarDeclInitializerExpected, cursor_.peek().span);
                }
                // init invalid 유지
            } else {
                init = parse_expr();
            }
        } else {
            // '=' missing
            if (is_static) {
                diag_report(diag::Code::kStaticVarRequiresInitializer, cursor_.peek().span);
                static_init_diag_emitted = true;
                recover_to_delim(K::kSemicolon, K::kRBrace, K::kEof);
            } else if (is_const) {
                diag_report(diag::Code::kVarDeclInitializerExpected, cursor_.peek().span);
                recover_to_delim(K::kSemicolon, K::kRBrace, K::kEof);
            } else if (is_set) {
                diag_report(diag::Code::kSetInitializerRequired, cursor_.peek().span);
                // recovery: ';' or '}' 까지 맞춰서 이후 stmt들이 안 꼬이게 한다.
                recover_to_delim(K::kSemicolon, K::kRBrace, K::kEof);
            }
        }

        if (is_static && init == ast::k_invalid_expr && !static_init_diag_emitted) {
            diag_report(diag::Code::kStaticVarRequiresInitializer, cursor_.peek().span);
        }

        // canonical placement: `... = expr with acts(NameOrDefault);`
        (void)parse_with_acts_clause(/*record_binding=*/true, /*allow_assign_recovery=*/false);

        // ---- ';' or recover ----
        const Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kVar;
        s.is_set = is_set;
        s.is_mut = is_mut;
        s.is_static = is_static;
        s.is_const = is_const;
        s.name = name;
        s.type = type_id;
        s.type_node = type_node;
        s.init = init;
        s.var_has_acts_binding = var_has_acts_binding;
        s.var_acts_is_default = var_acts_is_default;
        s.var_acts_target_type = var_acts_target_type;
        s.var_acts_target_type_node = var_acts_target_type_node;
        s.var_acts_set_path_begin = var_acts_set_path_begin;
        s.var_acts_set_path_count = var_acts_set_path_count;
        s.var_acts_set_name = var_acts_set_name;
        s.span = span_join(start_tok.span, end);
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

    ast::StmtId Parser::parse_stmt_require() {
        using K = syntax::TokenKind;
        const Token kw = cursor_.bump(); // require

        if (!cursor_.eat(K::kLParen)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "(");
            recover_to_delim(K::kLParen, K::kSemicolon, K::kRParen);
            (void)cursor_.eat(K::kLParen);
        }

        ast::ExprId expr = ast::k_invalid_expr;
        Span fallback = kw.span;
        if (!cursor_.at(K::kRParen) && !cursor_.at(K::kSemicolon)) {
            expr = parse_expr();
            fallback = ast_.expr(expr).span;
        }

        if (!cursor_.eat(K::kRParen)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
            recover_to_delim(K::kRParen, K::kSemicolon, K::kRBrace);
            (void)cursor_.eat(K::kRParen);
        }

        const Span term_end = stmt_consume_semicolon_or_recover(fallback);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kRequire;
        s.expr = expr;
        s.span = span_join(kw.span, term_end);
        return ast_.add_stmt(s);
    }

    ast::StmtId Parser::parse_stmt_throw() {
        const Token kw = cursor_.bump();

        ast::ExprId payload = ast::k_invalid_expr;
        Span fallback = kw.span;

        if (cursor_.at(syntax::TokenKind::kSemicolon)) {
            diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span, "throw payload expression");
        } else {
            payload = parse_expr();
            fallback = ast_.expr(payload).span;
        }

        const Span term_end = stmt_consume_semicolon_or_recover(fallback);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kThrow;
        s.expr = payload;
        s.span = span_join(kw.span, term_end);
        return ast_.add_stmt(s);
    }

    ast::StmtId Parser::parse_stmt_try_catch() {
        using K = syntax::TokenKind;

        const Token try_kw = cursor_.bump(); // try
        ast::StmtId try_body = parse_stmt_required_block("try");

        const uint32_t catch_begin = static_cast<uint32_t>(ast_.try_catch_clauses().size());
        uint32_t catch_count = 0;

        while (cursor_.at(K::kKwCatch)) {
            const Token catch_kw = cursor_.bump();
            ast::TryCatchClause clause{};
            clause.span = catch_kw.span;

            if (!cursor_.eat(K::kLParen)) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "(");
                recover_to_delim(K::kLParen, K::kRParen, K::kLBrace);
                cursor_.eat(K::kLParen);
            }

            const Token bind_tok = cursor_.peek();
            if (bind_tok.kind != K::kIdent) {
                diag_report(diag::Code::kCatchBinderNameExpected, bind_tok.span);
            } else {
                cursor_.bump();
                clause.bind_name = bind_tok.lexeme;
            }

            if (cursor_.eat(K::kColon)) {
                auto pty = parse_type();
                clause.has_typed_bind = true;
                clause.bind_type = pty.id;
                clause.bind_type_node = pty.node;
            }

            if (!cursor_.eat(K::kRParen)) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
                recover_to_delim(K::kRParen, K::kLBrace, K::kKwCatch);
                cursor_.eat(K::kRParen);
            }

            clause.body = parse_stmt_required_block("catch");
            if (clause.body != ast::k_invalid_stmt) {
                clause.span = span_join(catch_kw.span, ast_.stmt(clause.body).span);
            }

            ast_.add_try_catch_clause(clause);
            ++catch_count;
        }

        if (catch_count == 0) {
            diag_report(diag::Code::kTryCatchExpectedCatchClause, cursor_.peek().span);
        }

        ast::Stmt s{};
        s.kind = ast::StmtKind::kTryCatch;
        s.a = try_body;
        s.catch_clause_begin = catch_begin;
        s.catch_clause_count = catch_count;
        s.span = (catch_count > 0)
            ? span_join(try_kw.span, ast_.try_catch_clauses()[catch_begin + catch_count - 1].span)
            : span_join(try_kw.span, ast_.stmt(try_body).span);
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

    ast::StmtId Parser::parse_stmt_commit() {
        const Token kw = cursor_.bump();
        const Span term_end = stmt_consume_semicolon_or_recover(kw.span);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kCommitStmt;
        s.span = span_join(kw.span, term_end);
        return ast_.add_stmt(s);
    }

    ast::StmtId Parser::parse_stmt_recast() {
        const Token kw = cursor_.bump();
        const Span term_end = stmt_consume_semicolon_or_recover(kw.span);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kRecastStmt;
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

        auto eat_coloncolon = [&]() -> bool {
            if (cursor_.eat(K::kColonColon)) return true;
            if (cursor_.at(K::kColon) && cursor_.peek(1).kind == K::kColon) {
                cursor_.bump();
                cursor_.bump();
                return true;
            }
            return false;
        };

        auto has_coloncolon_before_case_delim = [&]() -> bool {
            for (uint32_t off = 0; off < 64; ++off) {
                const auto tk = cursor_.peek(off).kind;
                if (tk == K::kColonColon) return true;
                if (tk == K::kColon && cursor_.peek(off + 1).kind == K::kColon) return true;
                if (tk == K::kEof || tk == K::kColon || tk == K::kLBrace || tk == K::kRBrace) break;
            }
            return false;
        };

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

                ast::SwitchCase c{};
                c.is_default = false;
                c.pat_kind = ast::CasePatKind::kError;

                const Token pat = cursor_.peek();
                bool parsed_pattern = false;

                if (pat.kind == K::kIdent && has_coloncolon_before_case_delim()) {
                    const auto cp = save_parse_checkpoint_();
                    ast::PathRef enum_path{};
                    if (parse_type_path_ref(enum_path, /*allow_leading_coloncolon=*/false) &&
                        enum_path.path_count > 0) {
                        std::string_view variant_name{};
                        Span variant_span{};
                        bool have_variant = false;

                        // Preferred form: EnumType::Variant(...)
                        if (eat_coloncolon()) {
                            const Token vtok = cursor_.peek();
                            if (vtok.kind == K::kIdent) {
                                cursor_.bump();
                                variant_name = vtok.lexeme;
                                variant_span = vtok.span;
                                have_variant = true;
                            }
                        } else {
                            // Legacy parse_type_path_ref greedily consumes `Token::Eof` when no
                            // generic args are present. In switch-case enum pattern context,
                            // reinterpret the last path segment as variant.
                            const auto& tn = ast_.type_node(enum_path.type_node);
                            if (tn.kind == ast::TypeNodeKind::kNamedPath &&
                                tn.generic_arg_count == 0 &&
                                enum_path.path_count >= 2) {
                                const auto& segs = ast_.path_segs();
                                variant_name = segs[enum_path.path_begin + enum_path.path_count - 1];
                                variant_span = pat.span;

                                ast::TypeNode prefix_tn = tn;
                                prefix_tn.path_count -= 1;
                                prefix_tn.resolved_type =
                                    types_.intern_path(&segs[prefix_tn.path_begin], prefix_tn.path_count);
                                enum_path.type_node = ast_.add_type_node(prefix_tn);
                                enum_path.type = prefix_tn.resolved_type;
                                have_variant = true;
                            }
                        }

                        if (have_variant) {
                            c.pat_kind = ast::CasePatKind::kEnumVariant;
                            c.enum_type_node = enum_path.type_node;
                            c.enum_type = enum_path.type;
                            c.enum_variant_name = variant_name;
                            c.pat_text = variant_name;
                            c.span = span_join(case_kw.span, variant_span);

                            const uint32_t bind_begin =
                                static_cast<uint32_t>(ast_.switch_enum_binds().size());
                            uint32_t bind_count = 0;
                            if (cursor_.eat(K::kLParen)) {
                                while (!cursor_.at(K::kRParen) && !cursor_.at(K::kEof) && !is_aborted()) {
                                    if (cursor_.eat(K::kComma)) {
                                        if (cursor_.at(K::kRParen)) break;
                                        continue;
                                    }

                                    const Token field_tok = cursor_.peek();
                                    if (field_tok.kind != K::kIdent) {
                                        diag_report(diag::Code::kFieldMemberNameExpected, field_tok.span);
                                        recover_to_delim(K::kComma, K::kRParen, K::kColon);
                                        if (cursor_.eat(K::kComma)) continue;
                                        break;
                                    }
                                    cursor_.bump();

                                    if (!cursor_.eat(K::kColon)) {
                                        diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ":");
                                        recover_to_delim(K::kComma, K::kRParen, K::kColon);
                                        if (cursor_.eat(K::kComma)) continue;
                                        break;
                                    }

                                    const Token bind_tok = cursor_.peek();
                                    if (bind_tok.kind != K::kIdent) {
                                        diag_report(diag::Code::kVarDeclNameExpected, bind_tok.span);
                                        recover_to_delim(K::kComma, K::kRParen, K::kColon);
                                        if (cursor_.eat(K::kComma)) continue;
                                        break;
                                    }
                                    cursor_.bump();

                                    ast::SwitchEnumBind b{};
                                    b.field_name = field_tok.lexeme;
                                    b.bind_name = bind_tok.lexeme;
                                    b.span = span_join(field_tok.span, bind_tok.span);
                                    ast_.add_switch_enum_bind(b);
                                    ++bind_count;

                                    if (cursor_.eat(K::kComma)) {
                                        if (cursor_.at(K::kRParen)) break;
                                        continue;
                                    }
                                    break;
                                }
                                if (!cursor_.eat(K::kRParen)) {
                                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
                                    recover_to_delim(K::kRParen, K::kColon, K::kComma);
                                    cursor_.eat(K::kRParen);
                                }
                            }
                            c.enum_bind_begin = bind_begin;
                            c.enum_bind_count = bind_count;
                            parsed_pattern = true;
                        }
                    }
                    if (!parsed_pattern) {
                        restore_parse_checkpoint_(cp);
                    }
                }

                if (!parsed_pattern) {
                    if (!detail::is_case_pattern_tok(pat.kind)) {
                        diag_report(diag::Code::kSwitchCaseExpectedPattern, pat.span);
                        recover_to_delim(K::kColon, K::kKwCase, K::kKwDefault);
                    } else {
                        cursor_.bump();
                        c.pat_kind = detail::case_pat_kind_from_tok(pat);
                        c.pat_text = pat.lexeme;
                    }
                }

                if (!cursor_.eat(K::kColon)) {
                    diag_report(diag::Code::kSwitchCaseExpectedColon, cursor_.peek().span);
                    recover_to_delim(K::kColon, K::kLBrace, K::kKwCase);
                    cursor_.eat(K::kColon);
                }

                ast::StmtId body = parse_case_body_block("case");

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
    // ';'가 없으면 다음 경계까지 복구 후 가능한 경우 ';' 소비
    Span Parser::stmt_consume_semicolon_or_recover(Span fallback_end) {
        if (cursor_.at(syntax::TokenKind::kSemicolon)) {
            return cursor_.bump().span;
        }

        using K = syntax::TokenKind;
        const Token t = cursor_.peek();
        diag_report(diag::Code::kExpectedToken, t.span,
                    syntax::token_kind_name(syntax::TokenKind::kSemicolon));

        // 다음 토큰이 "새 문장/decl 시작으로 강하게 해석 가능한 경계"면
        // 과도하게 토큰을 먹지 않고 즉시 반환하여 연쇄 파손을 줄인다.
        const K k = t.kind;
        if (is_decl_start(k)
            || is_unambiguous_stmt_start(k)
            || k == K::kKwIf
            || k == K::kKwLoop
            || k == K::kLBrace
            || k == K::kKwElse
            || k == K::kKwElif
            || k == K::kKwCase
            || k == K::kKwCatch
            || k == K::kKwDefault) {
            return fallback_end;
        }

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

    Span Parser::stmt_consume_optional_semicolon(Span fallback_end) {
        if (cursor_.at(syntax::TokenKind::kSemicolon)) {
            return cursor_.bump().span;
        }
        return fallback_end;
    }

} // namespace parus
