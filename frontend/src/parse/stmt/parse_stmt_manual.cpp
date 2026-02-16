// frontend/src/parse/stmt/parse_stmt_manual.cpp
#include <parus/parse/Parser.hpp>
#include <parus/syntax/TokenKind.hpp>

namespace parus {

    /// @brief `manual[perm,...] { ... }` 문장을 파싱한다.
    ///
    /// v0 권한:
    /// - get (읽기)
    /// - set (쓰기, get 포함)
    /// - abi (ABI 경계 호출)
    ast::StmtId Parser::parse_stmt_manual() {
        using K = syntax::TokenKind;

        const Token manual_kw = cursor_.bump(); // 'manual'

        auto make_empty_block = [&]() -> ast::StmtId {
            ast::Stmt empty{};
            empty.kind = ast::StmtKind::kBlock;
            empty.span = cursor_.peek().span;
            empty.stmt_begin = static_cast<uint32_t>(ast_.stmt_children().size());
            empty.stmt_count = 0;
            return ast_.add_stmt(empty);
        };

        uint8_t perm_mask = 0;

        if (!cursor_.eat(K::kLBracket)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "[");
            recover_to_delim(K::kLBracket, K::kLBrace, K::kSemicolon);
            (void)cursor_.eat(K::kLBracket);
        }

        while (!cursor_.at(K::kEof) && !cursor_.at(K::kRBracket)) {
            const Token p = cursor_.peek();
            uint8_t bit = 0;

            if (p.kind == K::kKwSet) {
                cursor_.bump();
                bit = ast::kManualPermSet | ast::kManualPermGet;
                if ((perm_mask & ast::kManualPermSet) != 0) {
                    diag_report(diag::Code::kUnexpectedToken, p.span, "duplicated manual permission: set");
                }
            } else if (p.kind == K::kIdent && p.lexeme == "get") {
                cursor_.bump();
                bit = ast::kManualPermGet;
                if ((perm_mask & ast::kManualPermGet) != 0) {
                    diag_report(diag::Code::kUnexpectedToken, p.span, "duplicated manual permission: get");
                }
            } else if (p.kind == K::kIdent && p.lexeme == "abi") {
                cursor_.bump();
                bit = ast::kManualPermAbi;
                if ((perm_mask & ast::kManualPermAbi) != 0) {
                    diag_report(diag::Code::kUnexpectedToken, p.span, "duplicated manual permission: abi");
                }
            } else {
                diag_report(diag::Code::kUnexpectedToken, p.span,
                            "manual permission ('get', 'set', 'abi')");
                cursor_.bump();
            }

            perm_mask |= bit;

            if (cursor_.at(K::kComma)) {
                cursor_.bump();
                continue;
            }
            if (cursor_.at(K::kRBracket)) {
                break;
            }

            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ", or ]");
            recover_to_delim(K::kComma, K::kRBracket, K::kLBrace);
            if (cursor_.at(K::kComma)) {
                cursor_.bump();
            }
        }

        if (!cursor_.eat(K::kRBracket)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "]");
            recover_to_delim(K::kRBracket, K::kLBrace, K::kSemicolon);
            (void)cursor_.eat(K::kRBracket);
        }

        if (perm_mask == 0) {
            diag_report(diag::Code::kUnexpectedToken, manual_kw.span,
                        "manual[] requires at least one permission");
        }

        ast::StmtId body = ast::k_invalid_stmt;
        if (!cursor_.at(K::kLBrace)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "{");
            recover_to_delim(K::kLBrace, K::kSemicolon, K::kRBrace);
            if (cursor_.at(K::kLBrace)) body = parse_stmt_block();
            else body = make_empty_block();
        } else {
            body = parse_stmt_block();
        }

        ast::Stmt s{};
        s.kind = ast::StmtKind::kManual;
        s.a = body;
        s.manual_perm_mask = perm_mask;
        s.span = span_join(manual_kw.span, ast_.stmt(body).span);
        return ast_.add_stmt(s);
    }

} // namespace parus
