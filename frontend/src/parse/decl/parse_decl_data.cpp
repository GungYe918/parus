// frontend/src/parse/parse_decl_data.cpp
#include <parus/parse/Parser.hpp>
#include <parus/syntax/TokenKind.hpp>
#include <parus/ty/Type.hpp>
#include <parus/ty/TypePool.hpp>

#include <string>
#include <limits>
#include <vector>


namespace parus {

    namespace {

        /// @brief 링크 ABI 문자열 토큰이 C ABI(`"C"`)인지 검사한다.
        bool is_c_abi_lit_(const Token& t) {
            if (t.kind != syntax::TokenKind::kStringLit) return false;
            return t.lexeme == "\"C\"";
        }

        /// @brief align 인자 토큰에서 u32 정수값을 추출한다.
        bool parse_u32_align_lit_(const Token& t, uint32_t& out) {
            if (t.kind != syntax::TokenKind::kIntLit) return false;

            uint64_t v = 0;
            bool saw_digit = false;
            for (const char c : t.lexeme) {
                if (c == '_') continue;
                if (c < '0' || c > '9') break;
                saw_digit = true;
                v = v * 10u + static_cast<uint64_t>(c - '0');
                if (v > static_cast<uint64_t>(std::numeric_limits<uint32_t>::max())) return false;
            }
            if (!saw_digit) return false;
            out = static_cast<uint32_t>(v);
            return true;
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
        ast::TypeNodeId type_node = ast::k_invalid_type_node;
        if (!cursor_.eat(K::kColon)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ":");
        } else {
            auto parsed = parse_type();
            type_id = parsed.id;
            type_node = parsed.node;
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
            diag_report(
                diag::Code::kAbiCGlobalMustBeStatic,
                start,
                name.empty() ? "<unnamed>" : name
            );
        }

        const Span end = stmt_consume_semicolon_or_recover(cursor_.prev().span);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kVar;
        s.span = span_join(start, end);
        s.name = name;
        s.type = type_id;
        s.type_node = type_node;
        s.init = init;
        s.is_set = false; // C ABI global 선언은 set/let 경로를 사용하지 않는다.
        s.is_mut = is_mut;
        s.is_static = is_static;
        s.is_export = is_export;
        s.is_extern = is_extern;
        s.link_abi = has_c_abi ? ast::LinkAbi::kC : ast::LinkAbi::kNone;
        return ast_.add_stmt(s);
    }

    /// @brief `field layout(c)? align(n)? Name { member: Type; ... }` 선언을 파싱한다.
    ast::StmtId Parser::parse_decl_field() {
        using K = syntax::TokenKind;

        const Token start_tok = cursor_.peek();
        Span start = start_tok.span;

        // ABI 규칙: field에는 export/extern를 붙이지 않는다.
        while (cursor_.at(K::kKwExport) || cursor_.at(K::kKwExtern)) {
            const Token bad = cursor_.bump();
            start = bad.span;
            diag_report(diag::Code::kUnexpectedToken, bad.span,
                        "'export/extern' is not allowed on field declarations");
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

        ast::FieldLayout field_layout = ast::FieldLayout::kNone;
        uint32_t field_align = 0;
        bool seen_layout = false;
        bool seen_align = false;

        // qualifier loop: layout(c) / align(n)
        while (!is_aborted()) {
            if (cursor_.at(K::kKwLayout) ||
                (cursor_.peek().kind == K::kIdent && cursor_.peek().lexeme == "layout")) {
                const Token qtok = cursor_.bump();
                if (seen_layout) {
                    diag_report(diag::Code::kUnexpectedToken, qtok.span, "duplicated layout(...) in field declaration");
                }
                seen_layout = true;

                if (!cursor_.eat(K::kLParen)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "(");
                }

                const Token arg = cursor_.peek();
                if (arg.kind == K::kIdent && arg.lexeme == "c") {
                    field_layout = ast::FieldLayout::kC;
                    cursor_.bump();
                } else {
                    diag_report(diag::Code::kUnexpectedToken, arg.span, "only layout(c) is supported");
                    if (arg.kind != K::kRParen && arg.kind != K::kEof) cursor_.bump();
                }

                if (!cursor_.eat(K::kRParen)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
                    recover_to_delim(K::kRParen, K::kIdent, K::kLBrace);
                    cursor_.eat(K::kRParen);
                }
                continue;
            }

            if (cursor_.at(K::kKwAlign) ||
                (cursor_.peek().kind == K::kIdent && cursor_.peek().lexeme == "align")) {
                const Token qtok = cursor_.bump();
                if (seen_align) {
                    diag_report(diag::Code::kUnexpectedToken, qtok.span, "duplicated align(...) in field declaration");
                }
                seen_align = true;

                if (!cursor_.eat(K::kLParen)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "(");
                }

                const Token arg = cursor_.peek();
                uint32_t parsed_align = 0;
                if (parse_u32_align_lit_(arg, parsed_align)) {
                    field_align = parsed_align;
                    cursor_.bump();
                } else {
                    diag_report(diag::Code::kUnexpectedToken, arg.span, "align(...) requires a positive integer literal");
                    if (arg.kind != K::kRParen && arg.kind != K::kEof) cursor_.bump();
                }

                if (!cursor_.eat(K::kRParen)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
                    recover_to_delim(K::kRParen, K::kIdent, K::kLBrace);
                    cursor_.eat(K::kRParen);
                }
                continue;
            }

            break;
        }

        std::string_view name{};
        const Token name_tok = cursor_.peek();
        if (name_tok.kind == K::kIdent) {
            name = name_tok.lexeme;
            cursor_.bump();
        } else {
            diag_report(diag::Code::kFieldNameExpected, name_tok.span);
        }

        const uint32_t impl_begin = static_cast<uint32_t>(ast_.path_refs().size());
        uint32_t impl_count = 0;
        if (cursor_.eat(K::kColon)) {
            while (!cursor_.at(K::kLBrace) && !cursor_.at(K::kEof) && !is_aborted()) {
                const Token pstart = cursor_.peek();
                const auto [pb, pc] = parse_path_segments(/*allow_leading_coloncolon=*/true);
                if (pc == 0) {
                    diag_report(diag::Code::kUnexpectedToken, pstart.span, "proto path");
                    recover_to_delim(K::kComma, K::kLBrace, K::kSemicolon);
                    if (cursor_.eat(K::kComma)) continue;
                    break;
                }

                ast::PathRef pr{};
                pr.path_begin = pb;
                pr.path_count = pc;
                pr.span = span_join(pstart.span, cursor_.prev().span);
                ast_.add_path_ref(pr);
                ++impl_count;

                if (cursor_.eat(K::kComma)) continue;
                break;
            }
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

            // field 내부 함수/선언은 금지: member: Type; 혹은 (legacy) Type member;만 허용
            if (is_decl_start(cursor_.peek().kind) || cursor_.at(K::kKwIf) || cursor_.at(K::kKwWhile)) {
                diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                            "field member declaration 'name: Type;' (use class for value+behavior)");
                recover_to_delim(K::kSemicolon, K::kRBrace);
                cursor_.eat(K::kSemicolon);
                continue;
            }

            if (cursor_.at(K::kKwMut)) {
                diag_report(diag::Code::kFieldMemberMutNotAllowed, cursor_.peek().span);
                cursor_.bump();
            }

            std::string_view member_name{};
            ParsedType parsed_ty{};
            Token member_name_tok{};
            bool parse_ok = true;

            // ABI 문법 우선: name: Type;
            if (cursor_.peek().kind == K::kIdent && cursor_.peek(1).kind == K::kColon) {
                member_name_tok = cursor_.peek();
                member_name = member_name_tok.lexeme;
                cursor_.bump();
                cursor_.bump(); // :
                parsed_ty = parse_type();
                if (parsed_ty.id == ty::kInvalidType) {
                    parse_ok = false;
                }
            } else {
                // legacy 호환: Type name;
                parsed_ty = parse_type();
                if (parsed_ty.id == ty::kInvalidType) {
                    parse_ok = false;
                } else {
                    member_name_tok = cursor_.peek();
                    if (member_name_tok.kind == K::kIdent) {
                        member_name = member_name_tok.lexeme;
                        cursor_.bump();
                    } else {
                        diag_report(diag::Code::kFieldMemberNameExpected, member_name_tok.span);
                        parse_ok = false;
                    }
                }
            }

            if (!parse_ok) {
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
            fm.type_node = parsed_ty.node;
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
        s.is_export = false;
        s.type = name.empty() ? ty::kInvalidType : types_.intern_ident(name);
        s.field_layout = field_layout;
        s.field_align = field_align;
        s.field_member_begin = field_member_begin;
        s.field_member_count = field_member_count;
        s.decl_path_ref_begin = impl_begin;
        s.decl_path_ref_count = impl_count;
        return ast_.add_stmt(s);
    }

    /// @brief `proto Name [: BaseProto, ...] { def sig(...)->T; ... } with require(expr);`
    ast::StmtId Parser::parse_decl_proto() {
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
                        "'extern' is not allowed on proto declarations");
            cursor_.bump();
        }

        if (!cursor_.eat(K::kKwProto)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "proto");
            stmt_sync_to_boundary();

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

        const uint32_t inherit_begin = static_cast<uint32_t>(ast_.path_refs().size());
        uint32_t inherit_count = 0;
        if (cursor_.eat(K::kColon)) {
            while (!cursor_.at(K::kLBrace) && !cursor_.at(K::kEof) && !is_aborted()) {
                const Token pstart = cursor_.peek();
                const auto [pb, pc] = parse_path_segments(/*allow_leading_coloncolon=*/true);
                if (pc == 0) {
                    diag_report(diag::Code::kUnexpectedToken, pstart.span, "proto path");
                    recover_to_delim(K::kComma, K::kLBrace, K::kSemicolon);
                    if (cursor_.eat(K::kComma)) continue;
                    break;
                }
                ast::PathRef pr{};
                pr.path_begin = pb;
                pr.path_count = pc;
                pr.span = span_join(pstart.span, cursor_.prev().span);
                ast_.add_path_ref(pr);
                ++inherit_count;

                if (cursor_.eat(K::kComma)) continue;
                break;
            }
        }

        if (!cursor_.eat(K::kLBrace)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "{");
            recover_to_delim(K::kLBrace, K::kSemicolon, K::kRBrace);
            cursor_.eat(K::kLBrace);
        }

        std::vector<ast::StmtId> members;
        members.reserve(8);
        while (!cursor_.at(K::kRBrace) && !cursor_.at(K::kEof) && !is_aborted()) {
            if (cursor_.eat(K::kSemicolon)) continue;

            if (cursor_.at(K::kKwFn)) {
                const ast::StmtId msid = parse_decl_proto_member_sig();
                if (msid != ast::k_invalid_stmt) members.push_back(msid);
                continue;
            }

            if (cursor_.peek().kind == K::kIdent && cursor_.peek().lexeme == "operator") {
                diag_report(diag::Code::kProtoOperatorNotAllowed, cursor_.peek().span);
                recover_to_delim(K::kSemicolon, K::kRBrace);
                cursor_.eat(K::kSemicolon);
                continue;
            }

            diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                        "proto member signature");
            recover_to_delim(K::kSemicolon, K::kRBrace);
            cursor_.eat(K::kSemicolon);
        }

        if (!cursor_.eat(K::kRBrace)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "}");
            recover_to_delim(K::kRBrace, K::kSemicolon);
            cursor_.eat(K::kRBrace);
        }

        bool has_require = false;
        ast::ExprId require_expr = ast::k_invalid_expr;
        const bool has_with =
            cursor_.at(K::kKwWith) ||
            (cursor_.peek().kind == K::kIdent && cursor_.peek().lexeme == "with");
        if (has_with) {
            cursor_.bump(); // with
            if (!(cursor_.at(K::kKwRequire) ||
                  (cursor_.peek().kind == K::kIdent && cursor_.peek().lexeme == "require"))) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "require");
            } else {
                cursor_.bump(); // require
                if (!cursor_.eat(K::kLParen)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "(");
                }
                if (!cursor_.at(K::kRParen) && !cursor_.at(K::kEof)) {
                    require_expr = parse_expr();
                    has_require = true;
                } else {
                    diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span, "bool expression");
                }
                if (!cursor_.eat(K::kRParen)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
                    recover_to_delim(K::kRParen, K::kSemicolon, K::kRBrace);
                    cursor_.eat(K::kRParen);
                }
            }
        } else {
            diag_report(diag::Code::kProtoRequireMissing, cursor_.peek().span);
        }

        const Span end_sp = stmt_consume_semicolon_or_recover(cursor_.prev().span);

        const uint32_t member_begin = static_cast<uint32_t>(ast_.stmt_children().size());
        for (const auto sid : members) {
            ast_.add_stmt_child(sid);
        }

        ast::Stmt s{};
        s.kind = ast::StmtKind::kProtoDecl;
        s.span = span_join(start, end_sp);
        s.name = name;
        s.is_export = is_export;
        s.type = name.empty() ? ty::kInvalidType : types_.intern_ident(name);
        s.stmt_begin = member_begin;
        s.stmt_count = static_cast<uint32_t>(members.size());
        s.decl_path_ref_begin = inherit_begin;
        s.decl_path_ref_count = inherit_count;
        s.proto_has_require = has_require;
        s.proto_require_expr = require_expr;
        return ast_.add_stmt(s);
    }

    /// @brief `tablet Name [: ProtoA, ...] { let ...; def ... }`
    ast::StmtId Parser::parse_decl_tablet() {
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
                        "'extern' is not allowed on tablet declarations");
            cursor_.bump();
        }

        if (!cursor_.eat(K::kKwTablet)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "tablet");
            stmt_sync_to_boundary();
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

        const uint32_t impl_begin = static_cast<uint32_t>(ast_.path_refs().size());
        uint32_t impl_count = 0;
        if (cursor_.eat(K::kColon)) {
            while (!cursor_.at(K::kLBrace) && !cursor_.at(K::kEof) && !is_aborted()) {
                const Token pstart = cursor_.peek();
                const auto [pb, pc] = parse_path_segments(/*allow_leading_coloncolon=*/true);
                if (pc == 0) {
                    diag_report(diag::Code::kUnexpectedToken, pstart.span, "proto path");
                    recover_to_delim(K::kComma, K::kLBrace, K::kSemicolon);
                    if (cursor_.eat(K::kComma)) continue;
                    break;
                }
                ast::PathRef pr{};
                pr.path_begin = pb;
                pr.path_count = pc;
                pr.span = span_join(pstart.span, cursor_.prev().span);
                ast_.add_path_ref(pr);
                ++impl_count;
                if (cursor_.eat(K::kComma)) continue;
                break;
            }
        }

        if (!cursor_.eat(K::kLBrace)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "{");
            recover_to_delim(K::kLBrace, K::kSemicolon, K::kRBrace);
            cursor_.eat(K::kLBrace);
        }

        std::vector<ast::StmtId> members;
        members.reserve(16);
        while (!cursor_.at(K::kRBrace) && !cursor_.at(K::kEof) && !is_aborted()) {
            if (cursor_.eat(K::kSemicolon)) continue;

            const auto k = cursor_.peek().kind;
            const bool fn_start = (k == K::kAt || k == K::kKwFn || k == K::kKwExport || k == K::kKwExtern);
            if (fn_start) {
                ast::StmtId mid = parse_decl_fn();
                auto& ms = ast_.stmt_mut(mid);
                if (ms.kind == ast::StmtKind::kFnDecl && ms.is_export) {
                    diag_report(diag::Code::kUnexpectedToken, ms.span,
                                "tablet member must not be declared with export");
                    ms.is_export = false;
                }
                if (ms.kind == ast::StmtKind::kFnDecl && ms.is_extern) {
                    diag_report(diag::Code::kUnexpectedToken, ms.span,
                                "tablet member must not be declared with extern");
                    ms.is_extern = false;
                    ms.link_abi = ast::LinkAbi::kNone;
                }
                members.push_back(mid);
                continue;
            }

            const bool var_member_start =
                k == K::kKwLet || k == K::kKwSet || k == K::kKwMut;
            if (var_member_start) {
                const Token member_start = cursor_.peek();
                bool mut_prefix_invalid = false;
                if (cursor_.at(K::kKwMut)) {
                    mut_prefix_invalid = true;
                    diag_report(diag::Code::kVarMutMustFollowKw, cursor_.peek().span);
                    cursor_.bump();
                }

                bool is_set = false;
                bool is_mut = false;
                if (cursor_.at(K::kKwLet) || cursor_.at(K::kKwSet)) {
                    is_set = cursor_.at(K::kKwSet);
                    cursor_.bump();
                } else {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "let/set");
                }

                if (cursor_.at(K::kKwMut)) {
                    is_mut = true;
                    cursor_.bump();
                } else if (mut_prefix_invalid) {
                    is_mut = true;
                }

                std::string_view member_name{};
                const Token mn = cursor_.peek();
                if (mn.kind == K::kIdent) {
                    member_name = mn.lexeme;
                    cursor_.bump();
                } else {
                    diag_report(diag::Code::kVarDeclNameExpected, mn.span);
                }

                ast::TypeId type_id = ast::k_invalid_type;
                ast::TypeNodeId type_node = ast::k_invalid_type_node;
                if (!cursor_.eat(K::kColon)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ":");
                } else {
                    auto parsed = parse_type();
                    type_id = parsed.id;
                    type_node = parsed.node;
                }

                ast::ExprId init = ast::k_invalid_expr;
                if (cursor_.eat(K::kAssign)) {
                    init = parse_expr();
                }
                Span mend = stmt_consume_semicolon_or_recover(cursor_.prev().span);

                ast::Stmt ms{};
                ms.kind = ast::StmtKind::kVar;
                ms.span = span_join(member_start.span, mend);
                ms.is_set = is_set;
                ms.is_mut = is_mut;
                ms.name = member_name;
                ms.type = type_id;
                ms.type_node = type_node;
                ms.init = init;
                members.push_back(ast_.add_stmt(ms));
                continue;
            }

            diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                        "tablet member declaration");
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

        const uint32_t member_begin = static_cast<uint32_t>(ast_.stmt_children().size());
        for (const auto sid : members) {
            ast_.add_stmt_child(sid);
        }

        ast::Stmt s{};
        s.kind = ast::StmtKind::kTabletDecl;
        s.span = span_join(start, end_sp);
        s.name = name;
        s.is_export = is_export;
        s.type = name.empty() ? ty::kInvalidType : types_.intern_ident(name);
        s.stmt_begin = member_begin;
        s.stmt_count = static_cast<uint32_t>(members.size());
        s.decl_path_ref_begin = impl_begin;
        s.decl_path_ref_count = impl_count;
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
        std::vector<std::string_view> labels;
        std::vector<uint8_t> has_default_flags;
        pts.reserve(param_count);
        labels.reserve(param_count);
        has_default_flags.reserve(param_count);
        for (uint32_t i = 0; i < param_count; ++i) {
            const auto& p = ast_.params()[param_begin + i];
            pts.push_back(p.type);
            labels.push_back(p.name);
            has_default_flags.push_back(p.has_default ? 1u : 0u);
        }
        const ty::TypeId sig_id = types_.make_fn(
            ret_ty.id,
            pts.empty() ? nullptr : pts.data(),
            (uint32_t)pts.size(),
            positional_count,
            labels.empty() ? nullptr : labels.data(),
            has_default_flags.empty() ? nullptr : has_default_flags.data()
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
        s.fn_ret_type_node = ret_ty.node;
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
        ast::TypeNodeId acts_target_type_node = ast::k_invalid_type_node;

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
            acts_target_type_node = ty.node;
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
                acts_target_type_node = ty.node;
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
                        "acts member (def declaration only; use class for mixed value+behavior)");
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
        s.acts_target_type_node = acts_target_type_node;
        return ast_.add_stmt(s);
    }

} // namespace parus
