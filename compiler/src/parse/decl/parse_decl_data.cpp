// compiler/src/parse/parse_decl_data.cpp
#include <gaupel/parse/Parser.hpp>
#include <gaupel/syntax/TokenKind.hpp>
#include <gaupel/ty/Type.hpp>
#include <gaupel/ty/TypePool.hpp>

#include <vector>


namespace gaupel {

    /// @brief `field Name { Type member; ... }` 선언을 파싱한다.
    ast::StmtId Parser::parse_decl_field() {
        using K = syntax::TokenKind;

        const Token start_tok = cursor_.peek();
        Span start = start_tok.span;

        bool is_export = false;
        if (cursor_.at(K::kKwExport)) {
            is_export = true;
            start = cursor_.bump().span; // export
        }

        if (!cursor_.at(K::kKwField)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "field");

            stmt_sync_to_boundary();
            if (cursor_.at(K::kSemicolon)) cursor_.bump();

            ast::Stmt s{};
            s.kind = ast::StmtKind::kError;
            s.span = span_join(start, cursor_.prev().span);
            return ast_.add_stmt(s);
        }
        cursor_.bump(); // field

        std::string_view name{};
        const Token name_tok = cursor_.peek();
        if (name_tok.kind == K::kIdent) {
            name = name_tok.lexeme;
            cursor_.bump();
        } else {
            diag_report(diag::Code::kUnexpectedToken, name_tok.span, "identifier (field name)");
        }

        if (!cursor_.eat(K::kLBrace)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "{");
            recover_to_delim(K::kLBrace, K::kSemicolon, K::kRBrace);
            cursor_.eat(K::kLBrace);
        }

        const uint32_t field_member_begin = (uint32_t)ast_.field_members().size();
        uint32_t field_member_count = 0;

        while (!cursor_.at(K::kRBrace) && !cursor_.at(K::kEof) && !is_aborted()) {
            if (cursor_.eat(K::kSemicolon)) {
                continue;
            }

            // field 내부 함수/선언은 금지: Type name;만 허용
            if (is_decl_start(cursor_.peek().kind) || cursor_.at(K::kKwIf) || cursor_.at(K::kKwWhile)) {
                diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                            "field member declaration 'Type name;' (use class for value+behavior)");
                recover_to_delim(K::kSemicolon, K::kRBrace);
                cursor_.eat(K::kSemicolon);
                continue;
            }

            const auto parsed_ty = parse_type();
            if (parsed_ty.id == ty::kInvalidType) {
                recover_to_delim(K::kSemicolon, K::kRBrace);
                cursor_.eat(K::kSemicolon);
                continue;
            }

            const Token member_name_tok = cursor_.peek();
            std::string_view member_name{};
            if (member_name_tok.kind == K::kIdent) {
                member_name = member_name_tok.lexeme;
                cursor_.bump();
            } else {
                diag_report(diag::Code::kUnexpectedToken, member_name_tok.span, "identifier (field member name)");
                recover_to_delim(K::kSemicolon, K::kRBrace);
                cursor_.eat(K::kSemicolon);
                continue;
            }

            Span end_span = member_name_tok.span;
            if (!cursor_.eat(K::kSemicolon)) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ";");
                recover_to_delim(K::kSemicolon, K::kRBrace);
                if (cursor_.at(K::kSemicolon)) {
                    end_span = cursor_.bump().span;
                }
            } else {
                end_span = cursor_.prev().span;
            }

            ast::FieldMember fm{};
            fm.type = parsed_ty.id;
            fm.name = member_name;
            fm.span = span_join(parsed_ty.span, end_span);
            ast_.add_field_member(fm);
            ++field_member_count;
        }

        if (!cursor_.eat(K::kRBrace)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "}");
            recover_to_delim(K::kRBrace, K::kSemicolon);
            cursor_.eat(K::kRBrace);
        }
        Span end_sp = cursor_.prev().span;

        if (cursor_.at(K::kSemicolon)) {
            end_sp = cursor_.bump().span;
        }

        ast::Stmt s{};
        s.kind = ast::StmtKind::kFieldDecl;
        s.span = span_join(start, end_sp);
        s.name = name;
        s.is_export = is_export;
        s.field_member_begin = field_member_begin;
        s.field_member_count = field_member_count;
        return ast_.add_stmt(s);
    }

    /// @brief `acts A { fn ... }` 선언을 파싱한다. (v0: 일반 acts만 지원)
    ast::StmtId Parser::parse_decl_acts() {
        using K = syntax::TokenKind;

        const Token start_tok = cursor_.peek();
        Span start = start_tok.span;

        bool is_export = false;
        if (cursor_.at(K::kKwExport)) {
            is_export = true;
            start = cursor_.bump().span; // export
        }

        if (!cursor_.at(K::kKwActs)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "acts");

            stmt_sync_to_boundary();
            if (cursor_.at(K::kSemicolon)) cursor_.bump();

            ast::Stmt s{};
            s.kind = ast::StmtKind::kError;
            s.span = span_join(start, cursor_.prev().span);
            return ast_.add_stmt(s);
        }
        cursor_.bump(); // acts

        std::string_view name{};
        const Token name_tok = cursor_.peek();
        if (name_tok.kind == K::kIdent) {
            name = name_tok.lexeme;
            cursor_.bump();
        } else {
            diag_report(diag::Code::kUnexpectedToken, name_tok.span, "identifier (acts name)");
        }

        if (name == "for") {
            diag_report(diag::Code::kUnexpectedToken, name_tok.span, "'acts for T' is not supported yet; use 'acts A { ... }'");

            // 현재는 acts for를 지원하지 않으므로, 선언 끝까지 통째로 스킵해 연쇄 오류를 줄인다.
            while (!cursor_.at(K::kLBrace) && !cursor_.at(K::kSemicolon) && !cursor_.at(K::kEof)) {
                cursor_.bump();
            }

            if (cursor_.at(K::kLBrace)) {
                int depth = 0;
                while (!cursor_.at(K::kEof)) {
                    const auto tk = cursor_.bump();
                    if (tk.kind == K::kLBrace) {
                        ++depth;
                    } else if (tk.kind == K::kRBrace) {
                        --depth;
                        if (depth <= 0) break;
                    }
                }
            }

            if (cursor_.at(K::kSemicolon)) {
                cursor_.bump();
            }

            ast::Stmt s{};
            s.kind = ast::StmtKind::kActsDecl;
            s.span = span_join(start, cursor_.prev().span);
            s.name = name;
            s.is_export = is_export;
            s.stmt_begin = (uint32_t)ast_.stmt_children().size();
            s.stmt_count = 0;
            return ast_.add_stmt(s);
        }

        if (!cursor_.eat(K::kLBrace)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "{");
            recover_to_delim(K::kLBrace, K::kSemicolon, K::kRBrace);
            cursor_.eat(K::kLBrace);
        }

        std::vector<ast::StmtId> members;
        members.reserve(16);

        while (!cursor_.at(K::kRBrace) && !cursor_.at(K::kEof) && !is_aborted()) {
            if (cursor_.eat(K::kSemicolon)) {
                continue;
            }

            const auto k = cursor_.peek().kind;
            const bool is_fn_member_start = (k == K::kAt || k == K::kKwFn || k == K::kKwExport);
            if (is_fn_member_start) {
                ast::StmtId mid = parse_decl_fn();
                auto& ms = ast_.stmt_mut(mid);
                if (ms.kind == ast::StmtKind::kFnDecl && ms.is_export) {
                    diag_report(diag::Code::kUnexpectedToken, ms.span, "member-level 'export' is not allowed inside acts");
                    ms.is_export = false;
                }
                members.push_back(mid);
                continue;
            }

            diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                        "acts member (fn declaration only; use class for mixed value+behavior)");
            recover_to_delim(K::kSemicolon, K::kRBrace);
            cursor_.eat(K::kSemicolon);
        }

        if (!cursor_.eat(K::kRBrace)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "}");
            recover_to_delim(K::kRBrace, K::kSemicolon);
            cursor_.eat(K::kRBrace);
        }
        Span end_sp = cursor_.prev().span;

        if (cursor_.at(K::kSemicolon)) {
            end_sp = cursor_.bump().span;
        }

        const uint32_t member_begin = (uint32_t)ast_.stmt_children().size();
        for (auto sid : members) {
            ast_.add_stmt_child(sid);
        }

        ast::Stmt s{};
        s.kind = ast::StmtKind::kActsDecl;
        s.span = span_join(start, end_sp);
        s.name = name;
        s.is_export = is_export;
        s.stmt_begin = member_begin;
        s.stmt_count = (uint32_t)members.size();
        return ast_.add_stmt(s);
    }

} // namespace gaupel
