// frontend/src/parse/parse_decl_data.cpp
#include <parus/parse/Parser.hpp>
#include <parus/syntax/TokenKind.hpp>
#include <parus/ty/Type.hpp>
#include <parus/ty/TypePool.hpp>

#include <string>
#include <limits>
#include <vector>
#include <unordered_set>


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

    /// @brief `struct layout(c)? align(n)? Name { member: Type; ... }` 선언을 파싱한다.
    ast::StmtId Parser::parse_decl_field() {
        using K = syntax::TokenKind;

        const Token start_tok = cursor_.peek();
        Span start = start_tok.span;

        // ABI 규칙: struct에는 export/extern를 붙이지 않는다.
        while (cursor_.at(K::kKwExport) || cursor_.at(K::kKwExtern)) {
            const Token bad = cursor_.bump();
            start = bad.span;
            diag_report(diag::Code::kUnexpectedToken, bad.span,
                        "'export/extern' is not allowed on struct declarations");
        }

        if (!cursor_.at(K::kKwField)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "struct");

            stmt_sync_to_boundary();
            if (cursor_.at(K::kSemicolon)) cursor_.bump();

            ast::Stmt s{};
            s.kind = ast::StmtKind::kError;
            s.span = span_join(start, cursor_.prev().span);
            return ast_.add_stmt(s);
        }
        const Token kw_tok = cursor_.peek();
        if (kw_tok.kind == K::kKwField && kw_tok.lexeme == "field") {
            diag_report(diag::Code::kUnexpectedToken, kw_tok.span,
                        "'field' keyword is removed; use 'struct'");
        }
        cursor_.bump(); // struct

        ast::FieldLayout field_layout = ast::FieldLayout::kNone;
        uint32_t field_align = 0;
        bool seen_layout = false;
        bool seen_align = false;

        // qualifier loop: layout(c) / align(n)
        while (!is_aborted()) {
            if (cursor_.at(K::kKwLayout)) {
                const Token qtok = cursor_.bump();
                if (seen_layout) {
                    diag_report(diag::Code::kUnexpectedToken, qtok.span, "duplicated layout(...) in struct declaration");
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

            if (cursor_.at(K::kKwAlign)) {
                const Token qtok = cursor_.bump();
                if (seen_align) {
                    diag_report(diag::Code::kUnexpectedToken, qtok.span, "duplicated align(...) in struct declaration");
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

        uint32_t decl_generic_begin = 0;
        uint32_t decl_generic_count = 0;
        (void)parse_decl_generic_param_clause(decl_generic_begin, decl_generic_count);

        const uint32_t impl_begin = static_cast<uint32_t>(ast_.path_refs().size());
        uint32_t impl_count = 0;
        if (cursor_.eat(K::kColon)) {
            while (!cursor_.at(K::kLBrace) && !cursor_.at(K::kEof) && !is_aborted()) {
                ast::PathRef pr{};
                if (!parse_type_path_ref(pr, /*allow_leading_coloncolon=*/true) || pr.path_count == 0) {
                    recover_to_delim(K::kComma, K::kLBrace, K::kSemicolon);
                    if (cursor_.eat(K::kComma)) continue;
                    break;
                }
                ast_.add_path_ref(pr);
                ++impl_count;

                if (cursor_.eat(K::kComma)) continue;
                break;
            }
        }

        uint32_t decl_constraint_begin = 0;
        uint32_t decl_constraint_count = 0;
        (void)parse_decl_fn_constraint_clause(decl_constraint_begin, decl_constraint_count);

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

            // struct 내부 함수/선언은 금지: member: Type; 혹은 (legacy) Type member;만 허용
            if (is_decl_start(cursor_.peek().kind) || cursor_.at(K::kKwIf) || cursor_.at(K::kKwWhile)) {
                diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                            "struct member declaration 'name: Type;' (use class for value+behavior)");
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

            // struct member canonical syntax: `name: Type;`
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
                member_name_tok = cursor_.peek();
                diag_report(diag::Code::kFieldMemberNameExpected, member_name_tok.span);
                parse_ok = false;
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
        const Span end_sp = stmt_consume_optional_semicolon(cursor_.prev().span);

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
        s.decl_generic_param_begin = decl_generic_begin;
        s.decl_generic_param_count = decl_generic_count;
        s.decl_constraint_begin = decl_constraint_begin;
        s.decl_constraint_count = decl_constraint_count;
        return ast_.add_stmt(s);
    }

    /// @brief `enum Name [: ProtoA, ...] with [...] { case A, case B(x: T), ... }`
    ast::StmtId Parser::parse_decl_enum() {
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
                        "'extern' is not allowed on enum declarations");
            cursor_.bump();
        }

        if (!cursor_.eat(K::kKwEnum)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "enum");
            stmt_sync_to_boundary();
            if (cursor_.at(K::kSemicolon)) cursor_.bump();

            ast::Stmt s{};
            s.kind = ast::StmtKind::kError;
            s.span = span_join(start, cursor_.prev().span);
            return ast_.add_stmt(s);
        }

        ast::FieldLayout enum_layout = ast::FieldLayout::kNone;
        if (cursor_.at(K::kKwLayout)) {
            cursor_.bump();
            if (!cursor_.eat(K::kLParen)) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "(");
            }
            const Token arg = cursor_.peek();
            if (arg.kind == K::kIdent && arg.lexeme == "c") {
                enum_layout = ast::FieldLayout::kC;
                cursor_.bump();
            } else {
                diag_report(diag::Code::kUnexpectedToken, arg.span, "only layout(c) is supported");
                if (arg.kind != K::kRParen && arg.kind != K::kEof) cursor_.bump();
            }
            if (!cursor_.eat(K::kRParen)) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
                recover_to_delim(K::kRParen, K::kLBrace, K::kSemicolon);
                cursor_.eat(K::kRParen);
            }
        }

        std::string_view name{};
        const Token name_tok = cursor_.peek();
        if (name_tok.kind == K::kIdent) {
            name = name_tok.lexeme;
            cursor_.bump();
        } else {
            diag_report(diag::Code::kFieldNameExpected, name_tok.span);
        }

        uint32_t decl_generic_begin = 0;
        uint32_t decl_generic_count = 0;
        (void)parse_decl_generic_param_clause(decl_generic_begin, decl_generic_count);

        const uint32_t impl_begin = static_cast<uint32_t>(ast_.path_refs().size());
        uint32_t impl_count = 0;
        if (cursor_.eat(K::kColon)) {
            while (!cursor_.at(K::kLBrace) && !cursor_.at(K::kEof) && !is_aborted()) {
                ast::PathRef pr{};
                if (!parse_type_path_ref(pr, /*allow_leading_coloncolon=*/true) || pr.path_count == 0) {
                    recover_to_delim(K::kComma, K::kLBrace, K::kSemicolon);
                    if (cursor_.eat(K::kComma)) continue;
                    break;
                }
                ast_.add_path_ref(pr);
                ++impl_count;
                if (cursor_.eat(K::kComma)) continue;
                break;
            }
        }

        uint32_t decl_constraint_begin = 0;
        uint32_t decl_constraint_count = 0;
        (void)parse_decl_fn_constraint_clause(decl_constraint_begin, decl_constraint_count);

        if (!cursor_.eat(K::kLBrace)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "{");
            recover_to_delim(K::kLBrace, K::kSemicolon, K::kRBrace);
            cursor_.eat(K::kLBrace);
        }

        const uint32_t variant_begin = static_cast<uint32_t>(ast_.enum_variant_decls().size());
        uint32_t variant_count = 0;
        while (!cursor_.at(K::kRBrace) && !cursor_.at(K::kEof) && !is_aborted()) {
            if (cursor_.eat(K::kSemicolon)) continue;

            if (!cursor_.eat(K::kKwCase)) {
                diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span, "case");
                recover_to_delim(K::kComma, K::kRBrace, K::kSemicolon);
                if (cursor_.eat(K::kComma) || cursor_.eat(K::kSemicolon)) continue;
                break;
            }

            const Token vname_tok = cursor_.peek();
            std::string_view vname{};
            if (vname_tok.kind == K::kIdent) {
                vname = vname_tok.lexeme;
                cursor_.bump();
            } else {
                diag_report(diag::Code::kUnexpectedToken, vname_tok.span, "enum variant identifier");
            }

            const uint32_t payload_begin = static_cast<uint32_t>(ast_.field_members().size());
            uint32_t payload_count = 0;
            if (cursor_.eat(K::kLParen)) {
                while (!cursor_.at(K::kRParen) && !cursor_.at(K::kEof) && !is_aborted()) {
                    if (cursor_.eat(K::kComma)) {
                        if (cursor_.at(K::kRParen)) break;
                        continue;
                    }

                    const Token fname_tok = cursor_.peek();
                    std::string_view fname{};
                    if (fname_tok.kind == K::kIdent) {
                        fname = fname_tok.lexeme;
                        cursor_.bump();
                    } else {
                        diag_report(diag::Code::kFieldMemberNameExpected, fname_tok.span);
                        recover_to_delim(K::kComma, K::kRParen, K::kSemicolon);
                        if (cursor_.eat(K::kComma)) continue;
                        break;
                    }

                    if (!cursor_.eat(K::kColon)) {
                        diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ":");
                        recover_to_delim(K::kComma, K::kRParen, K::kSemicolon);
                        if (cursor_.eat(K::kComma)) continue;
                        break;
                    }

                    auto ty = parse_type();
                    ast::FieldMember fm{};
                    fm.name = fname;
                    fm.type = ty.id;
                    fm.type_node = ty.node;
                    fm.span = span_join(fname_tok.span, ty.span);
                    ast_.add_field_member(fm);
                    ++payload_count;

                    if (cursor_.eat(K::kComma)) {
                        if (cursor_.at(K::kRParen)) break;
                        continue;
                    }
                    break;
                }

                if (!cursor_.eat(K::kRParen)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
                    recover_to_delim(K::kRParen, K::kComma, K::kRBrace);
                    cursor_.eat(K::kRParen);
                }
            }

            bool has_discriminant = false;
            int64_t discriminant = 0;
            if (cursor_.eat(K::kAssign)) {
                has_discriminant = true;
                const Token lit = cursor_.peek();
                if (lit.kind == K::kIntLit) {
                    int64_t v = 0;
                    bool saw = false;
                    for (const char ch : lit.lexeme) {
                        if (ch == '_') continue;
                        if (ch < '0' || ch > '9') break;
                        saw = true;
                        v = (v * 10) + (ch - '0');
                    }
                    if (saw) discriminant = v;
                    cursor_.bump();
                } else {
                    diag_report(diag::Code::kUnexpectedToken, lit.span, "integer literal");
                }
            }

            ast::EnumVariantDecl ev{};
            ev.name = vname;
            ev.payload_begin = payload_begin;
            ev.payload_count = payload_count;
            ev.has_discriminant = has_discriminant;
            ev.discriminant = discriminant;
            ev.span = vname_tok.span;
            ast_.add_enum_variant_decl(ev);
            ++variant_count;

            if (cursor_.eat(K::kComma)) {
                if (cursor_.at(K::kRBrace)) break;
                continue;
            }

            if (!cursor_.at(K::kRBrace)) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ",");
                recover_to_delim(K::kComma, K::kRBrace, K::kSemicolon);
                if (cursor_.eat(K::kComma)) continue;
            }
        }

        if (!cursor_.eat(K::kRBrace)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "}");
            recover_to_delim(K::kRBrace, K::kSemicolon);
            cursor_.eat(K::kRBrace);
        }
        const Span end_sp = stmt_consume_optional_semicolon(cursor_.prev().span);

        ast::Stmt s{};
        s.kind = ast::StmtKind::kEnumDecl;
        s.span = span_join(start, end_sp);
        s.name = name;
        s.is_export = is_export;
        s.type = name.empty() ? ty::kInvalidType : types_.intern_ident(name);
        s.field_layout = enum_layout;
        s.enum_variant_begin = variant_begin;
        s.enum_variant_count = variant_count;
        s.decl_path_ref_begin = impl_begin;
        s.decl_path_ref_count = impl_count;
        s.decl_generic_param_begin = decl_generic_begin;
        s.decl_generic_param_count = decl_generic_count;
        s.decl_constraint_begin = decl_constraint_begin;
        s.decl_constraint_count = decl_constraint_count;
        return ast_.add_stmt(s);
    }

    /// @brief `proto Name [: BaseProto, ...] { require ...; provide ...; ... };`
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

        uint32_t decl_generic_begin = 0;
        uint32_t decl_generic_count = 0;
        (void)parse_decl_generic_param_clause(decl_generic_begin, decl_generic_count);

        const uint32_t inherit_begin = static_cast<uint32_t>(ast_.path_refs().size());
        uint32_t inherit_count = 0;
        if (cursor_.eat(K::kColon)) {
            while (!cursor_.at(K::kLBrace) && !cursor_.at(K::kEof) && !is_aborted()) {
                ast::PathRef pr{};
                if (!parse_type_path_ref(pr, /*allow_leading_coloncolon=*/true) || pr.path_count == 0) {
                    recover_to_delim(K::kComma, K::kLBrace, K::kSemicolon);
                    if (cursor_.eat(K::kComma)) continue;
                    break;
                }
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
        members.reserve(16);
        while (!cursor_.at(K::kRBrace) && !cursor_.at(K::kEof) && !is_aborted()) {
            if (cursor_.eat(K::kSemicolon)) continue;

            if (cursor_.peek().kind == K::kIdent && cursor_.peek().lexeme == "operator") {
                diag_report(diag::Code::kProtoOperatorNotAllowed, cursor_.peek().span);
                recover_to_delim(K::kSemicolon, K::kRBrace);
                cursor_.eat(K::kSemicolon);
                continue;
            }

            if (cursor_.at(K::kKwRequire)) {
                const Token req_tok = cursor_.bump(); // require

                const auto parse_require_kind_item = [&](ast::ProtoRequireKind req_kind) -> ast::StmtId {
                    if (!cursor_.eat(K::kLParen)) {
                        diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "(");
                        recover_to_delim(K::kLParen, K::kRParen, K::kSemicolon);
                        (void)cursor_.eat(K::kLParen);
                    }

                    ast::PathRef pr{};
                    bool path_ok = true;
                    if (cursor_.at(K::kRParen) || cursor_.at(K::kSemicolon) || cursor_.at(K::kEof)) {
                        path_ok = false;
                        diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span, "type path");
                    } else {
                        path_ok = parse_type_path_ref(pr, /*allow_leading_coloncolon=*/true);
                    }

                    if (!cursor_.eat(K::kRParen)) {
                        diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ")");
                        recover_to_delim(K::kRParen, K::kSemicolon, K::kRBrace);
                        (void)cursor_.eat(K::kRParen);
                    }

                    Span end_sp = req_tok.span;
                    if (path_ok) {
                        end_sp = pr.span;
                    }
                    end_sp = stmt_consume_semicolon_or_recover(end_sp);

                    ast::Stmt m{};
                    m.kind = ast::StmtKind::kRequire;
                    m.span = span_join(req_tok.span, end_sp);
                    m.proto_require_kind = req_kind;
                    if (path_ok) {
                        m.proto_req_path_begin = pr.path_begin;
                        m.proto_req_path_count = pr.path_count;
                    }
                    return ast_.add_stmt(m);
                };

                const Token next = cursor_.peek();
                if (next.kind == K::kKwField || next.kind == K::kKwEnum ||
                    next.kind == K::kKwClass || next.kind == K::kKwActor ||
                    next.kind == K::kKwActs) {
                    ast::ProtoRequireKind req_kind = ast::ProtoRequireKind::kNone;
                    switch (next.kind) {
                        case K::kKwField: req_kind = ast::ProtoRequireKind::kStruct; break;
                        case K::kKwEnum: req_kind = ast::ProtoRequireKind::kEnum; break;
                        case K::kKwClass: req_kind = ast::ProtoRequireKind::kClass; break;
                        case K::kKwActor: req_kind = ast::ProtoRequireKind::kActor; break;
                        case K::kKwActs: req_kind = ast::ProtoRequireKind::kActs; break;
                        default: break;
                    }
                    cursor_.bump(); // kind keyword
                    const ast::StmtId msid = parse_require_kind_item(req_kind);
                    if (msid != ast::k_invalid_stmt) members.push_back(msid);
                    continue;
                }

                if (next.kind == K::kKwFn) {
                    diag_report(diag::Code::kUnexpectedToken, next.span,
                                "require function signature must not use 'def'");
                }
                const ast::StmtId msid = parse_decl_proto_member_sig(
                    /*with_def_keyword=*/false,
                    /*require_body=*/false,
                    ast::ProtoFnRole::kRequire
                );
                if (msid != ast::k_invalid_stmt) members.push_back(msid);
                continue;
            }

            if (cursor_.at(K::kKwProvide)) {
                cursor_.bump(); // provide
                if (cursor_.at(K::kKwFn)) {
                    const ast::StmtId msid = parse_decl_proto_member_sig(
                        /*with_def_keyword=*/true,
                        /*require_body=*/true,
                        ast::ProtoFnRole::kProvide
                    );
                    if (msid != ast::k_invalid_stmt) members.push_back(msid);
                    continue;
                }

                if (cursor_.at(K::kKwConst)) {
                    const ast::StmtId msid = parse_stmt_var();
                    if (msid != ast::k_invalid_stmt && (size_t)msid < ast_.stmts().size()) {
                        auto& m = ast_.stmt_mut(msid);
                        if (m.kind == ast::StmtKind::kVar) {
                            m.var_is_proto_provide = true;
                            m.is_const = true;
                            m.is_static = true;
                            m.is_set = false;
                            m.is_mut = false;
                        }
                    }
                    if (msid != ast::k_invalid_stmt) members.push_back(msid);
                    continue;
                }

                diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span, "provide def/const");
                recover_to_delim(K::kSemicolon, K::kRBrace);
                (void)cursor_.eat(K::kSemicolon);
                continue;
            }

            diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                        "proto member (require/provide)");
            recover_to_delim(K::kSemicolon, K::kRBrace);
            cursor_.eat(K::kSemicolon);
        }

        if (!cursor_.eat(K::kRBrace)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "}");
            recover_to_delim(K::kRBrace, K::kSemicolon);
            cursor_.eat(K::kRBrace);
        }

        if (cursor_.at(K::kKwWith)) {
            diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                        "legacy 'with require(...)' syntax is removed; use proto body require/provide items");
            recover_to_delim(K::kSemicolon);
        }

        const Span end_sp = stmt_consume_optional_semicolon(cursor_.prev().span);

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
        s.decl_generic_param_begin = decl_generic_begin;
        s.decl_generic_param_count = decl_generic_count;
        s.decl_constraint_begin = 0;
        s.decl_constraint_count = 0;
        return ast_.add_stmt(s);
    }

    /// @brief class 전용 lifecycle 멤버:
    /// - init(...) { ... }
    /// - init() = default;
    /// - deinit() { ... }
    /// - deinit() = default;
    /// 일반 def와 달리 return type(`->`)을 쓰지 않는다.
