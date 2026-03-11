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
            case K::kKwCopy:
            case K::kKwClone:
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
        s.fn_is_const = false;
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

        // Removed syntax: acts for Vec<T> <T> { ... }
        if (cursor_.at(K::kLt)) {
            uint32_t removed_generic_begin = 0;
            uint32_t removed_generic_count = 0;
            (void)parse_decl_generic_param_clause(removed_generic_begin, removed_generic_count);
            diag_report(diag::Code::kActsGenericClauseRemoved, cursor_.prev().span);
        }

        uint32_t decl_constraint_begin = 0;
        uint32_t decl_constraint_count = 0;
        (void)parse_decl_fn_constraint_clause(decl_constraint_begin, decl_constraint_count);

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
            const bool is_fn_member_start =
                (k == K::kAt || k == K::kKwFn || k == K::kKwExport || k == K::kKwExtern ||
                (k == K::kKwConst && cursor_.peek(1).kind == K::kKwFn));
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
        const Span end_sp = stmt_consume_optional_semicolon(cursor_.prev().span);

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
        s.decl_generic_param_begin = 0;
        s.decl_generic_param_count = 0;
        s.decl_constraint_begin = decl_constraint_begin;
        s.decl_constraint_count = decl_constraint_count;
        return ast_.add_stmt(s);
    }

} // namespace parus
