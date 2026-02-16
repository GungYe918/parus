// frontend/src/parse/parse_decl_data.cpp
#include <parus/parse/Parser.hpp>
#include <parus/syntax/TokenKind.hpp>
#include <parus/ty/Type.hpp>
#include <parus/ty/TypePool.hpp>

#include <string>
#include <vector>


namespace parus {

    namespace {

        /// @brief 링크 ABI 문자열 토큰이 C ABI(`"C"`)인지 검사한다.
        bool is_c_abi_lit_(const Token& t) {
            if (t.kind != syntax::TokenKind::kStringLit) return false;
            return t.lexeme == "\"C\"";
        }

    } // namespace

    /// @brief `extern|export "C" static [mut] NAME: T [= EXPR];` 선언을 파싱한다.
    ast::StmtId Parser::parse_decl_extern_var() {
        using K = syntax::TokenKind;

        const Token start_tok = cursor_.peek();
        Span start = start_tok.span;

        bool is_export = false;
        bool is_extern = false;
        if (cursor_.at(K::kKwExtern)) {
            is_extern = true;
            start = cursor_.bump().span;
        } else if (cursor_.at(K::kKwExport)) {
            is_export = true;
            start = cursor_.bump().span;
        } else {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "extern or export");
        }

        const Token abi_tok = cursor_.peek();
        bool has_c_abi = false;
        if (abi_tok.kind == K::kStringLit) {
            has_c_abi = is_c_abi_lit_(abi_tok);
            if (!has_c_abi) {
                diag_report(diag::Code::kUnexpectedToken, abi_tok.span, "only \"C\" ABI is supported");
            }
            cursor_.bump();
        } else {
            diag_report(diag::Code::kExpectedToken, abi_tok.span, "\"C\"");
        }

        bool is_static = false;
        bool is_mut = false;

        // mut prefix는 금지: 반드시 static 뒤에서만 허용한다.
        if (cursor_.at(K::kKwMut)) {
            diag_report(diag::Code::kVarMutMustFollowKw, cursor_.peek().span);
            cursor_.bump();
        }

        if (cursor_.at(K::kKwStatic)) {
            is_static = true;
            cursor_.bump();
        } else {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "static");
        }

        if (cursor_.at(K::kKwMut)) {
            is_mut = true;
            cursor_.bump();
        }

        if (cursor_.at(K::kKwLet) || cursor_.at(K::kKwSet)) {
            diag_report(
                diag::Code::kUnexpectedToken,
                cursor_.peek().span,
                "remove 'let/set' from C ABI global declaration (use: static [mut] name: T)"
            );
            cursor_.bump();
            if (cursor_.at(K::kKwMut)) {
                diag_report(diag::Code::kVarMutMustFollowKw, cursor_.peek().span);
                cursor_.bump();
            }
        }

        std::string_view name{};
        const Token name_tok = cursor_.peek();
        if (name_tok.kind == K::kIdent) {
            name = name_tok.lexeme;
            cursor_.bump();
        } else {
            diag_report(diag::Code::kVarDeclNameExpected, name_tok.span);
            stmt_sync_to_boundary();
            if (cursor_.at(K::kSemicolon)) cursor_.bump();

            ast::Stmt s{};
            s.kind = ast::StmtKind::kError;
            s.span = span_join(start, cursor_.prev().span);
            return ast_.add_stmt(s);
        }

        ast::TypeId type_id = ast::k_invalid_type;
        if (!cursor_.eat(K::kColon)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ":");
        } else {
            type_id = parse_type().id;
        }

        ast::ExprId init = ast::k_invalid_expr;
        if (cursor_.eat(K::kAssign)) {
            if (is_extern) {
                diag_report(diag::Code::kUnexpectedToken, cursor_.prev().span,
                            "extern variable declaration must not have an initializer");
                if (!cursor_.at(K::kSemicolon) && !cursor_.at(K::kRBrace) && !cursor_.at(K::kEof)) {
                    (void)parse_expr();
                }
            } else {
                if (cursor_.at(K::kSemicolon) || cursor_.at(K::kRBrace) || cursor_.at(K::kEof)) {
                    diag_report(diag::Code::kVarDeclInitializerExpected, cursor_.peek().span);
                } else {
                    init = parse_expr();
                }
            }
        } else if (!is_extern) {
            diag_report(diag::Code::kStaticVarRequiresInitializer, cursor_.peek().span);
        }

        if (is_extern && !is_static) {
            diag_report(diag::Code::kUnexpectedToken, start,
                        "extern \"C\" variable declaration requires 'static'");
        }

        const Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kVar;
        s.span = span_join(start, end);
        s.name = name;
        s.type = type_id;
        s.init = init;
        s.is_set = false; // C ABI global 선언은 set/let 경로를 사용하지 않는다.
        s.is_mut = is_mut;
        s.is_static = is_static;
        s.is_export = is_export;
        s.is_extern = is_extern;
        s.link_abi = has_c_abi ? ast::LinkAbi::kC : ast::LinkAbi::kNone;
        return ast_.add_stmt(s);
    }

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
            diag_report(diag::Code::kFieldNameExpected, name_tok.span);
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
                diag_report(diag::Code::kFieldMemberNameExpected, member_name_tok.span);
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

    /// @brief `operator(KEY)(...) -> T { ... }` acts 멤버를 함수 선언 노드로 파싱한다.
    ast::StmtId Parser::parse_decl_acts_operator(ast::TypeId owner_type, bool allow_operator) {
        using K = syntax::TokenKind;

        const Token start_tok = cursor_.peek();
        Span start = start_tok.span;

        if (!(start_tok.kind == K::kIdent && start_tok.lexeme == "operator")) {
            diag_report(diag::Code::kExpectedToken, start_tok.span, "operator");
            ast::Stmt s{};
            s.kind = ast::StmtKind::kError;
            s.span = start_tok.span;
            return ast_.add_stmt(s);
        }

        if (!allow_operator) {
            diag_report(diag::Code::kOperatorDeclOnlyInActsFor, start_tok.span);
            cursor_.bump(); // operator
            int brace_depth = 0;
            while (!cursor_.at(K::kEof)) {
                const Token tk = cursor_.peek();
                if (tk.kind == K::kSemicolon && brace_depth == 0) {
                    cursor_.bump();
                    break;
                }
                if (tk.kind == K::kRBrace && brace_depth == 0) {
                    break;
                }
                cursor_.bump();
                if (tk.kind == K::kLBrace) {
                    ++brace_depth;
                } else if (tk.kind == K::kRBrace && brace_depth > 0) {
                    --brace_depth;
                }
            }

            ast::Stmt s{};
            s.kind = ast::StmtKind::kError;
            s.span = span_join(start, cursor_.prev().span);
            return ast_.add_stmt(s);
        }

        cursor_.bump(); // operator

        if (!cursor_.eat(K::kLParen)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "(");
            recover_to_delim(K::kRParen, K::kArrow, K::kLBrace);
            cursor_.eat(K::kRParen);
        }

        Token op_tok = cursor_.peek();
        bool operator_is_postfix = false;
        bool op_ok = false;

        switch (op_tok.kind) {
            case K::kPlus:
            case K::kMinus:
            case K::kStar:
            case K::kSlash:
            case K::kPercent:
            case K::kEqEq:
            case K::kBangEq:
            case K::kLt:
            case K::kLtEq:
            case K::kGt:
            case K::kGtEq:
                op_ok = true;
                cursor_.bump();
                break;
            case K::kPlusPlus:
                op_ok = true;
                cursor_.bump();
                if (cursor_.peek().kind == K::kIdent) {
                    const auto fixity = cursor_.peek().lexeme;
                    if (fixity == "post") {
                        operator_is_postfix = true;
                        cursor_.bump();
                    } else if (fixity == "pre") {
                        operator_is_postfix = false;
                        cursor_.bump();
                    } else {
                        op_ok = false;
                    }
                } else {
                    op_ok = false;
                }
                break;
            default:
                break;
        }

        if (!op_ok) {
            diag_report(diag::Code::kOperatorKeyExpected, op_tok.span);
            recover_to_delim(K::kRParen, K::kArrow, K::kLBrace);
            cursor_.eat(K::kRParen);
        } else if (!cursor_.eat(K::kRParen)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
            recover_to_delim(K::kRParen, K::kArrow, K::kLBrace);
            cursor_.eat(K::kRParen);
        }

        uint32_t param_begin = 0;
        uint32_t param_count = 0;
        uint32_t positional_count = 0;
        bool has_named_group = false;
        parse_decl_fn_params(param_begin, param_count, positional_count, has_named_group);

        if (!cursor_.at(K::kArrow)) {
            if (cursor_.at(K::kMinus) && cursor_.peek(1).kind == K::kGt) {
                cursor_.bump();
                cursor_.bump();
            } else {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "->");
                recover_to_delim(K::kArrow, K::kLBrace, K::kSemicolon);
                cursor_.eat(K::kArrow);
            }
        } else {
            cursor_.bump();
        }
        auto ret_ty = parse_type();

        ast::StmtId body = parse_stmt_required_block("operator");
        Span end_sp = ast_.stmt(body).span;
        if (cursor_.at(K::kSemicolon)) {
            end_sp = cursor_.bump().span;
        }

        std::vector<ty::TypeId> pts;
        pts.reserve(positional_count);
        for (uint32_t i = 0; i < positional_count; ++i) {
            const auto& p = ast_.params()[param_begin + i];
            pts.push_back(p.type);
        }
        const ty::TypeId sig_id = types_.make_fn(
            ret_ty.id,
            pts.empty() ? nullptr : pts.data(),
            (uint32_t)pts.size()
        );

        const auto op_name = ast_.add_owned_string(
            "__op$" + std::string(op_tok.lexeme) +
            (op_tok.kind == K::kPlusPlus ? (operator_is_postfix ? "post" : "pre") : "") +
            "$" + std::to_string((uint32_t)owner_type)
        );

        ast::Stmt s{};
        s.kind = ast::StmtKind::kFnDecl;
        s.span = span_join(start, end_sp);
        s.name = op_name;
        s.type = sig_id;
        s.fn_ret = ret_ty.id;
        s.a = body;

        s.is_export = false;
        s.fn_mode = ast::FnMode::kNone;
        s.is_throwing = false;
        s.is_pure = false;
        s.is_comptime = false;
        s.is_commit = false;
        s.is_recast = false;
        s.attr_begin = 0;
        s.attr_count = 0;

        s.param_begin = param_begin;
        s.param_count = param_count;
        s.positional_param_count = positional_count;
        s.has_named_group = has_named_group;

        s.fn_is_operator = true;
        s.fn_operator_token = op_tok.kind;
        s.fn_operator_is_postfix = operator_is_postfix;

        return ast_.add_stmt(s);
    }

    /// @brief acts 선언(`acts A`, `acts for T`, `acts Name for T`)을 파싱한다.
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
        bool acts_is_for = false;
        bool acts_has_set_name = false;
        ast::TypeId acts_target_type = ast::k_invalid_type;

        const auto is_for_token = [](const Token& t) -> bool {
            return t.kind == K::kIdent && t.lexeme == "for";
        };

        const Token head_tok = cursor_.peek();
        if (is_for_token(head_tok)) {
            // acts for T { ... }
            acts_is_for = true;
            cursor_.bump(); // for
            auto ty = parse_type();
            acts_target_type = ty.id;
            if (acts_target_type == ast::k_invalid_type) {
                diag_report(diag::Code::kActsForTypeExpected, ty.span);
            }
        } else if (head_tok.kind == K::kIdent) {
            // acts A { ... } or acts A for T { ... }
            name = head_tok.lexeme;
            cursor_.bump();

            if (is_for_token(cursor_.peek())) {
                acts_is_for = true;
                acts_has_set_name = true;
                cursor_.bump(); // for
                auto ty = parse_type();
                acts_target_type = ty.id;
                if (acts_target_type == ast::k_invalid_type) {
                    diag_report(diag::Code::kActsForTypeExpected, ty.span);
                }
            }
        } else {
            diag_report(diag::Code::kActsNameExpected, head_tok.span);
        }

        if (acts_is_for && !acts_has_set_name) {
            name = ast_.add_owned_string("__acts_for$" + std::to_string((uint32_t)acts_target_type));
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

            const auto& tok = cursor_.peek();
            if (tok.kind == K::kIdent && tok.lexeme == "operator") {
                ast::StmtId mid = parse_decl_acts_operator(acts_target_type, acts_is_for);
                if (mid != ast::k_invalid_stmt) {
                    members.push_back(mid);
                }
                continue;
            }

            const auto k = cursor_.peek().kind;
            const bool is_fn_member_start = (k == K::kAt || k == K::kKwFn || k == K::kKwExport || k == K::kKwExtern);
            if (is_fn_member_start) {
                ast::StmtId mid = parse_decl_fn();
                auto& ms = ast_.stmt_mut(mid);
                if (ms.kind == ast::StmtKind::kFnDecl && ms.is_export) {
                    diag_report(diag::Code::kActsMemberExportNotAllowed, ms.span);
                    ms.is_export = false;
                }
                if (ms.kind == ast::StmtKind::kFnDecl && ms.is_extern) {
                    diag_report(diag::Code::kUnexpectedToken, ms.span,
                                "extern function declaration is not allowed inside acts");
                    ms.is_extern = false;
                    ms.link_abi = ast::LinkAbi::kNone;
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
        s.acts_is_for = acts_is_for;
        s.acts_has_set_name = acts_has_set_name;
        s.acts_target_type = acts_target_type;
        return ast_.add_stmt(s);
    }

} // namespace parus
