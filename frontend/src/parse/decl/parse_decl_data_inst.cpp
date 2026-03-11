// frontend/src/parse/decl/parse_decl_data_inst.cpp
#include <parus/parse/Parser.hpp>
#include <parus/syntax/TokenKind.hpp>

#include <string>


namespace parus {

    /// @brief `inst Name(p0: T0, p1: T1) { ... }` 선언을 파싱한다.
    ast::StmtId Parser::parse_decl_inst() {
        using K = syntax::TokenKind;

        const Token start_tok = cursor_.peek();
        Span start = start_tok.span;

        bool is_export = false;
        if (cursor_.at(K::kKwExport)) {
            is_export = true;
            start = cursor_.bump().span;
        }

        if (cursor_.at(K::kKwExtern)) {
            diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                        "'extern' is not allowed on inst declarations");
            cursor_.bump();
        }

        if (!cursor_.eat(K::kKwInst)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "inst");
            stmt_sync_to_boundary();
            if (cursor_.at(K::kSemicolon)) cursor_.bump();

            ast::Stmt s{};
            s.kind = ast::StmtKind::kError;
            s.span = span_join(start, cursor_.prev().span);
            return ast_.add_stmt(s);
        }

        std::string_view name{};
        const Token name_tok = cursor_.peek();
        if (name_tok.kind == K::kIdent) {
            name = name_tok.lexeme;
            cursor_.bump();
        } else {
            diag_report(diag::Code::kFieldNameExpected, name_tok.span);
        }

        // v1: inst generic parameter clause는 허용하지 않는다.
        if (cursor_.at(K::kLt)) {
            diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                        "inst generic parameter clause is not supported");
            recover_to_delim(K::kLParen, K::kLBrace, K::kSemicolon);
        }

        if (!cursor_.eat(K::kLParen)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "(");
            recover_to_delim(K::kLParen, K::kRParen, K::kLBrace);
            (void)cursor_.eat(K::kLParen);
        }

        const uint32_t param_begin = static_cast<uint32_t>(ast_.params().size());
        uint32_t param_count = 0;
        while (!cursor_.at(K::kRParen) && !cursor_.at(K::kEof) && !is_aborted()) {
            if (cursor_.eat(K::kComma)) {
                if (cursor_.at(K::kRParen)) break;
                continue;
            }

            if (cursor_.at(K::kLBrace)) {
                diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                            "inst named parameter groups are not supported");
                recover_to_delim(K::kRParen, K::kSemicolon, K::kLBrace);
                break;
            }

            const Token pname_tok = cursor_.peek();
            std::string_view pname{};
            if (pname_tok.kind == K::kIdent) {
                pname = pname_tok.lexeme;
                cursor_.bump();
            } else {
                diag_report(diag::Code::kFnParamNameExpected, pname_tok.span);
                recover_to_delim(K::kComma, K::kRParen, K::kSemicolon);
                if (cursor_.eat(K::kComma)) continue;
                break;
            }

            if (!cursor_.eat(K::kColon)) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ":");
            }
            const auto ty = parse_type();

            if (cursor_.eat(K::kAssign)) {
                diag_report(diag::Code::kUnexpectedToken, cursor_.prev().span,
                            "inst parameter default values are not supported");
                if (!cursor_.at(K::kComma) && !cursor_.at(K::kRParen) && !cursor_.at(K::kEof)) {
                    (void)parse_expr();
                }
            }

            ast::Param p{};
            p.name = pname;
            p.type = ty.id;
            p.type_node = ty.node;
            p.is_mut = false;
            p.is_self = false;
            p.self_kind = ast::SelfReceiverKind::kNone;
            p.has_default = false;
            p.default_expr = ast::k_invalid_expr;
            p.is_named_group = false;
            p.span = span_join(pname_tok.span, ty.span);
            (void)ast_.add_param(p);
            ++param_count;

            if (cursor_.eat(K::kComma)) {
                if (cursor_.at(K::kRParen)) break;
                continue;
            }
            break;
        }

        if (!cursor_.eat(K::kRParen)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
            recover_to_delim(K::kRParen, K::kLBrace, K::kSemicolon);
            (void)cursor_.eat(K::kRParen);
        }

        // inst는 암묵 bool 반환이며 explicit `->`은 금지한다.
        if (cursor_.eat(K::kArrow)) {
            diag_report(diag::Code::kUnexpectedToken, cursor_.prev().span,
                        "inst has implicit bool return; remove '-> ...'");
            if (!cursor_.at(K::kLBrace) && !cursor_.at(K::kSemicolon) && !cursor_.at(K::kEof)) {
                (void)parse_type();
            }
        }

        const ast::StmtId body = parse_stmt_required_block("inst");
        const Span end_sp = stmt_consume_optional_semicolon(cursor_.prev().span);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kInstDecl;
        s.span = span_join(start, end_sp);
        s.name = name;
        s.is_export = is_export;
        s.a = body;
        s.param_begin = param_begin;
        s.param_count = param_count;
        s.positional_param_count = param_count;
        s.has_named_group = false;
        s.fn_ret = ty::kInvalidType;
        s.fn_ret_type_node = ast::k_invalid_type_node;
        return ast_.add_stmt(s);
    }

} // namespace parus
