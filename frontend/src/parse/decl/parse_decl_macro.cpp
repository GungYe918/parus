// frontend/src/parse/decl/parse_decl_macro.cpp
#include <parus/parse/Parser.hpp>
#include <parus/syntax/TokenKind.hpp>

namespace parus {

    namespace {

        ast::MacroMatchKind parse_match_kind_(const Token& t, bool& ok) {
            ok = true;
            if (t.kind != syntax::TokenKind::kIdent) {
                ok = false;
                return ast::MacroMatchKind::kExpr;
            }
            if (t.lexeme == "expr") return ast::MacroMatchKind::kExpr;
            if (t.lexeme == "stmt") return ast::MacroMatchKind::kStmt;
            if (t.lexeme == "item") return ast::MacroMatchKind::kItem;
            if (t.lexeme == "type") return ast::MacroMatchKind::kType;
            if (t.lexeme == "token") return ast::MacroMatchKind::kToken;
            ok = false;
            return ast::MacroMatchKind::kExpr;
        }

        ast::MacroOutKind parse_out_kind_(const Token& t, bool& ok) {
            ok = true;
            if (t.kind != syntax::TokenKind::kIdent) {
                ok = false;
                return ast::MacroOutKind::kExpr;
            }
            if (t.lexeme == "expr") return ast::MacroOutKind::kExpr;
            if (t.lexeme == "stmt") return ast::MacroOutKind::kStmt;
            if (t.lexeme == "item") return ast::MacroOutKind::kItem;
            if (t.lexeme == "type") return ast::MacroOutKind::kType;
            ok = false;
            return ast::MacroOutKind::kExpr;
        }

        ast::MacroFragKind parse_frag_kind_(const Token& t, bool& ok) {
            ok = true;
            if (t.kind != syntax::TokenKind::kIdent) {
                ok = false;
                return ast::MacroFragKind::kExpr;
            }
            if (t.lexeme == "expr") return ast::MacroFragKind::kExpr;
            if (t.lexeme == "stmt") return ast::MacroFragKind::kStmt;
            if (t.lexeme == "item") return ast::MacroFragKind::kItem;
            if (t.lexeme == "type") return ast::MacroFragKind::kType;
            if (t.lexeme == "path") return ast::MacroFragKind::kPath;
            if (t.lexeme == "ident") return ast::MacroFragKind::kIdent;
            if (t.lexeme == "block") return ast::MacroFragKind::kBlock;
            if (t.lexeme == "tt") return ast::MacroFragKind::kTt;
            ok = false;
            return ast::MacroFragKind::kExpr;
        }

        bool is_variadic_marker_(const Cursor& cursor) {
            using K = syntax::TokenKind;
            if (cursor.peek().kind == K::kDotDot && cursor.peek(1).kind == K::kDot) return true;
            if (cursor.peek().kind == K::kDot && cursor.peek(1).kind == K::kDot && cursor.peek(2).kind == K::kDot) return true;
            return false;
        }

    } // namespace

    bool Parser::parse_decl_macro() {
        using K = syntax::TokenKind;

        const Token macro_kw = cursor_.peek();
        if (!is_context_keyword(macro_kw, "macro")) {
            diag_report(diag::Code::kExpectedToken, macro_kw.span, "macro");
            return false;
        }
        cursor_.bump(); // macro

        const Token name_tok = cursor_.peek();
        if (name_tok.kind != K::kIdent) {
            diag_report(diag::Code::kUnexpectedToken, name_tok.span, "macro name");
            return false;
        }
        cursor_.bump();

        if (!cursor_.eat(K::kArrow)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "->");
            recover_to_delim(K::kLBrace, K::kSemicolon, K::kRBrace);
        }

        if (!cursor_.eat(K::kLBrace)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "{");
            recover_to_delim(K::kLBrace, K::kSemicolon, K::kRBrace);
            cursor_.eat(K::kLBrace);
        }

        const uint32_t group_begin = static_cast<uint32_t>(ast_.macro_groups().size());
        uint32_t group_count = 0;

        while (!cursor_.at(K::kRBrace) && !cursor_.at(K::kEof) && !is_aborted()) {
            const Token with_kw = cursor_.peek();
            if (!is_context_keyword(with_kw, "with")) {
                diag_report(diag::Code::kUnexpectedToken, with_kw.span, "with");
                recover_to_delim(K::kRBrace, K::kSemicolon);
                if (cursor_.eat(K::kSemicolon)) continue;
                break;
            }
            cursor_.bump(); // with

            bool mk_ok = false;
            const Token mk_tok = cursor_.peek();
            const auto mk = parse_match_kind_(mk_tok, mk_ok);
            if (!mk_ok) {
                diag_report(diag::Code::kUnexpectedToken, mk_tok.span, "match kind (expr|stmt|item|type|token)");
                recover_to_delim(K::kLBrace, K::kRBrace, K::kSemicolon);
            } else {
                cursor_.bump();
            }

            if (!cursor_.eat(K::kLBrace)) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "{");
                recover_to_delim(K::kLBrace, K::kRBrace, K::kSemicolon);
                cursor_.eat(K::kLBrace);
            }

            const uint32_t arm_begin = static_cast<uint32_t>(ast_.macro_arms().size());
            uint32_t arm_count = 0;

            const bool token_group_enabled = parser_features_.macro_with_token;
            if (mk == ast::MacroMatchKind::kToken && !token_group_enabled) {
                diag_report(diag::Code::kMacroTokenExperimentalRequired, mk_tok.span);
            }

            while (!cursor_.at(K::kRBrace) && !cursor_.at(K::kEof) && !is_aborted()) {
                if (mk == ast::MacroMatchKind::kToken && !token_group_enabled) {
                    recover_to_delim(K::kRBrace, K::kSemicolon);
                    if (cursor_.eat(K::kSemicolon)) continue;
                    break;
                }

                if (!cursor_.eat(K::kLParen)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "(");
                    recover_to_delim(K::kSemicolon, K::kRBrace);
                    if (cursor_.eat(K::kSemicolon)) continue;
                    break;
                }

                const uint32_t cap_begin = static_cast<uint32_t>(ast_.macro_captures().size());
                uint32_t cap_count = 0;
                while (!cursor_.at(K::kRParen) && !cursor_.at(K::kEof)) {
                    if (mk == ast::MacroMatchKind::kToken) {
                        (void)cursor_.eat(K::kDollar);
                    }

                    const Token cap_name = cursor_.peek();
                    if (cap_name.kind != K::kIdent) {
                        diag_report(diag::Code::kUnexpectedToken, cap_name.span, "capture name");
                        recover_to_delim(K::kRParen, K::kComma);
                        if (cursor_.eat(K::kComma)) continue;
                        break;
                    }
                    cursor_.bump();

                    if (!cursor_.eat(K::kColon)) {
                        diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ":");
                        recover_to_delim(K::kRParen, K::kComma);
                        if (cursor_.eat(K::kComma)) continue;
                        break;
                    }

                    bool fk_ok = false;
                    const Token fk_tok = cursor_.peek();
                    const auto fk = parse_frag_kind_(fk_tok, fk_ok);
                    if (!fk_ok) {
                        diag_report(diag::Code::kUnexpectedToken, fk_tok.span, "fragment kind");
                        recover_to_delim(K::kRParen, K::kComma);
                        if (cursor_.eat(K::kComma)) continue;
                        break;
                    }
                    cursor_.bump();

                    bool variadic = false;
                    if (is_variadic_marker_(cursor_)) {
                        variadic = true;
                        if (cursor_.peek().kind == K::kDotDot) {
                            cursor_.bump();
                            cursor_.bump();
                        } else {
                            cursor_.bump();
                            cursor_.bump();
                            cursor_.bump();
                        }
                    }

                    ast::MacroTypedCapture cap{};
                    cap.name = cap_name.lexeme;
                    cap.frag = fk;
                    cap.variadic = variadic;
                    cap.span = span_join(cap_name.span, cursor_.prev().span);
                    ast_.add_macro_capture(cap);
                    ++cap_count;

                    if (cursor_.eat(K::kComma)) {
                        if (cursor_.at(K::kRParen)) break;
                        continue;
                    }
                    break;
                }

                if (!cursor_.eat(K::kRParen)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
                    recover_to_delim(K::kRParen, K::kSemicolon, K::kRBrace);
                    cursor_.eat(K::kRParen);
                }

                if (!cursor_.eat(K::kAssign)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "=");
                }
                if (!cursor_.eat(K::kGt)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ">");
                }

                bool ok_out = false;
                const Token out_tok = cursor_.peek();
                const auto out_kind = parse_out_kind_(out_tok, ok_out);
                if (!ok_out) {
                    diag_report(diag::Code::kUnexpectedToken, out_tok.span, "output kind (expr|stmt|item|type)");
                } else {
                    cursor_.bump();
                }

                if (!cursor_.eat(K::kLBrace)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "{");
                    recover_to_delim(K::kLBrace, K::kSemicolon, K::kRBrace);
                    cursor_.eat(K::kLBrace);
                }

                const Token body_lb = cursor_.prev();
                const uint32_t tpl_begin = static_cast<uint32_t>(ast_.macro_tokens().size());
                uint32_t tpl_count = 0;
                int depth = 1;
                while (!cursor_.at(K::kEof) && depth > 0) {
                    const Token t = cursor_.peek();
                    if (t.kind == K::kLBrace) {
                        ++depth;
                        ast_.add_macro_token(cursor_.bump());
                        ++tpl_count;
                        continue;
                    }
                    if (t.kind == K::kRBrace) {
                        --depth;
                        if (depth == 0) {
                            cursor_.bump(); // consume closing template brace
                            break;
                        }
                        ast_.add_macro_token(cursor_.bump());
                        ++tpl_count;
                        continue;
                    }
                    ast_.add_macro_token(cursor_.bump());
                    ++tpl_count;
                }

                if (depth != 0) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "}");
                }

                if (!cursor_.eat(K::kSemicolon)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ";");
                }

                ast::MacroArm arm{};
                arm.capture_begin = cap_begin;
                arm.capture_count = cap_count;
                arm.out_kind = out_kind;
                arm.template_token_begin = tpl_begin;
                arm.template_token_count = tpl_count;
                arm.token_pattern = (mk == ast::MacroMatchKind::kToken);
                arm.span = span_join(body_lb.span, cursor_.prev().span);
                ast_.add_macro_arm(arm);
                ++arm_count;
            }

            if (!cursor_.eat(K::kRBrace)) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "}");
                recover_to_delim(K::kRBrace, K::kSemicolon);
                cursor_.eat(K::kRBrace);
            }

            ast::MacroGroup g{};
            g.match_kind = mk;
            g.arm_begin = arm_begin;
            g.arm_count = arm_count;
            g.phase2_token_group = (mk == ast::MacroMatchKind::kToken);
            g.span = span_join(with_kw.span, cursor_.prev().span);
            ast_.add_macro_group(g);
            ++group_count;
        }

        if (!cursor_.eat(K::kRBrace)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "}");
            recover_to_delim(K::kRBrace, K::kSemicolon);
            cursor_.eat(K::kRBrace);
        }

        ast::MacroDecl decl{};
        decl.name = name_tok.lexeme;
        decl.group_begin = group_begin;
        decl.group_count = group_count;
        decl.scope_depth = macro_scope_depth_;
        decl.span = span_join(macro_kw.span, cursor_.prev().span);
        ast_.add_macro_decl(decl);

        return true;
    }

} // namespace parus
