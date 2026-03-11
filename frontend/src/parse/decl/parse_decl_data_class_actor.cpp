    ast::StmtId Parser::parse_decl_class_lifecycle_member() {
        using K = syntax::TokenKind;

        const Token start_tok = cursor_.peek();
        const bool is_init = (start_tok.kind == K::kKwInit);
        const bool is_deinit = (start_tok.kind == K::kKwDeinit);
        if (!is_init && !is_deinit) {
            diag_report(diag::Code::kExpectedToken, start_tok.span, "init/deinit");
            ast::Stmt s{};
            s.kind = ast::StmtKind::kError;
            s.span = start_tok.span;
            return ast_.add_stmt(s);
        }
        cursor_.bump(); // init | deinit

        uint32_t param_begin = 0;
        uint32_t param_count = 0;
        uint32_t positional_count = 0;
        bool has_named_group = false;
        parse_decl_fn_params(param_begin, param_count, positional_count, has_named_group);

        const uint32_t user_param_count = param_count;
        if (has_named_group) {
            diag_report(diag::Code::kUnexpectedToken, start_tok.span,
                        "init()/deinit() does not support named-group parameters in v0");
        }

        for (uint32_t i = 0; i < user_param_count; ++i) {
            const auto& p = ast_.params()[param_begin + i];
            if (p.is_self) {
                diag_report(diag::Code::kClassLifecycleSelfNotAllowed, p.span);
            }
        }

        if (cursor_.at(K::kArrow)) {
            diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                        "init()/deinit() must not declare return type");
            cursor_.bump();
            (void)parse_type(); // recovery
        }

        if (is_deinit && user_param_count != 0) {
            diag_report(diag::Code::kUnexpectedToken, start_tok.span,
                        "deinit() must not declare parameters in v0");
        }

        // lifecycle ABI rule(v0):
        // - source syntax: init(...), deinit()
        // - internal ABI: hidden self receiver `self: &mut Self` is appended
        ast::Param hidden_self{};
        hidden_self.name = std::string_view("self");
        hidden_self.type = types_.make_borrow(types_.intern_ident("Self"), /*is_mut=*/true);
        hidden_self.type_node = ast::k_invalid_type_node;
        hidden_self.is_mut = false;
        hidden_self.is_self = true;
        hidden_self.self_kind = ast::SelfReceiverKind::kMut;
        hidden_self.is_named_group = false;
        hidden_self.has_default = false;
        hidden_self.default_expr = ast::k_invalid_expr;
        hidden_self.span = start_tok.span;
        ast_.add_param(hidden_self);
        ++param_count;

        // call-shape metadata는 일반 함수와 동일하게 유지한다.
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

        const ty::TypeId unit_ty = types_.builtin(ty::Builtin::kUnit);
        const ty::TypeId sig_id = types_.make_fn(
            unit_ty,
            pts.empty() ? nullptr : pts.data(),
            static_cast<uint32_t>(pts.size()),
            positional_count,
            labels.empty() ? nullptr : labels.data(),
            has_default_flags.empty() ? nullptr : has_default_flags.data()
        );

        ast::StmtId body = ast::k_invalid_stmt;
        Span end_sp = cursor_.prev().span;
        bool is_defaulted = false;
        if (cursor_.eat(K::kAssign)) {
            is_defaulted = true;

            if (!cursor_.eat(K::kKwDefault)) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "default");
                if (cursor_.peek().kind == K::kKwDefault) {
                    cursor_.bump();
                }
            }

            if (user_param_count != 0) {
                diag_report(diag::Code::kClassLifecycleDefaultParamNotAllowed, start_tok.span);
            }

            ast::Stmt empty{};
            empty.kind = ast::StmtKind::kBlock;
            empty.span = cursor_.prev().span;
            empty.stmt_begin = static_cast<uint32_t>(ast_.stmt_children().size());
            empty.stmt_count = 0;
            body = ast_.add_stmt(empty);

            end_sp = stmt_consume_semicolon_or_recover(cursor_.prev().span);
        } else {
            body = parse_stmt_required_block(is_init ? "init" : "deinit");
            end_sp = ast_.stmt(body).span;
            if (cursor_.at(K::kSemicolon)) {
                end_sp = cursor_.bump().span;
            }
        }

        if (is_defaulted && is_deinit && user_param_count != 0) {
            diag_report(diag::Code::kUnexpectedToken, start_tok.span,
                        "deinit() = default must not declare parameters");
        }

        ast::Stmt s{};
        s.kind = ast::StmtKind::kFnDecl;
        s.span = span_join(start_tok.span, end_sp);
        s.name = is_init ? std::string_view("init") : std::string_view("deinit");
        s.type = sig_id;
        s.fn_ret = unit_ty;
        s.fn_ret_type_node = ast::k_invalid_type_node;
        s.a = body;

        s.is_export = false;
        s.is_extern = false;
        s.link_abi = ast::LinkAbi::kNone;
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
        s.fn_is_proto_sig = false;
        s.fn_generic_param_begin = 0;
        s.fn_generic_param_count = 0;
        s.fn_constraint_begin = 0;
        s.fn_constraint_count = 0;
        return ast_.add_stmt(s);
    }

    /// @brief actor 전용 lifecycle 멤버:
    /// - init(...) { ... }
    /// - init() = default;
    /// 일반 def와 달리 return type(`->`)을 쓰지 않는다.
    ast::StmtId Parser::parse_decl_actor_init_member() {
        using K = syntax::TokenKind;

        const Token start_tok = cursor_.peek();
        if (start_tok.kind != K::kKwInit) {
            diag_report(diag::Code::kExpectedToken, start_tok.span, "init");
            ast::Stmt s{};
            s.kind = ast::StmtKind::kError;
            s.span = start_tok.span;
            return ast_.add_stmt(s);
        }
        cursor_.bump(); // init

        uint32_t param_begin = 0;
        uint32_t param_count = 0;
        uint32_t positional_count = 0;
        bool has_named_group = false;
        parse_decl_fn_params(param_begin, param_count, positional_count, has_named_group);

        auto strip_actor_self_params = [&](uint32_t& begin, uint32_t& count, uint32_t& positional) {
            bool found_self = false;
            for (uint32_t i = 0; i < count; ++i) {
                const auto param = ast_.params()[begin + i];
                if (!param.is_self) continue;
                diag_report(diag::Code::kActorSelfReceiverNotAllowed, param.span);
                found_self = true;
            }
            if (!found_self) return;

            const uint32_t filtered_begin = static_cast<uint32_t>(ast_.params().size());
            uint32_t filtered_count = 0;
            uint32_t filtered_positional = 0;
            for (uint32_t i = 0; i < count; ++i) {
                const auto param = ast_.params()[begin + i];
                if (param.is_self) continue;
                ast_.add_param(param);
                ++filtered_count;
                if (!param.is_named_group) ++filtered_positional;
            }
            begin = filtered_begin;
            count = filtered_count;
            positional = filtered_positional;
        };
        strip_actor_self_params(param_begin, param_count, positional_count);

        const uint32_t user_param_count = param_count;
        if (has_named_group) {
            diag_report(diag::Code::kUnexpectedToken, start_tok.span,
                        "actor init() does not support named-group parameters in v0");
        }

        if (cursor_.at(K::kArrow)) {
            diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                        "actor init() must not declare return type");
            cursor_.bump();
            (void)parse_type(); // recovery
        }

        // actor lifecycle hidden receiver:
        // - source name: draft
        // - internal type: &mut Self
        ast::Param hidden_self{};
        hidden_self.name = std::string_view("draft");
        hidden_self.type = types_.make_borrow(types_.intern_ident("Self"), /*is_mut=*/true);
        hidden_self.type_node = ast::k_invalid_type_node;
        hidden_self.is_mut = false;
        hidden_self.is_self = true;
        hidden_self.self_kind = ast::SelfReceiverKind::kMut;
        hidden_self.is_named_group = false;
        hidden_self.has_default = false;
        hidden_self.default_expr = ast::k_invalid_expr;
        hidden_self.span = start_tok.span;
        ast_.add_param(hidden_self);
        ++param_count;

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

        const ty::TypeId unit_ty = types_.builtin(ty::Builtin::kUnit);
        const ty::TypeId sig_id = types_.make_fn(
            unit_ty,
            pts.empty() ? nullptr : pts.data(),
            static_cast<uint32_t>(pts.size()),
            positional_count,
            labels.empty() ? nullptr : labels.data(),
            has_default_flags.empty() ? nullptr : has_default_flags.data()
        );

        ast::StmtId body = ast::k_invalid_stmt;
        Span end_sp = cursor_.prev().span;
        if (cursor_.eat(K::kAssign)) {
            if (!cursor_.eat(K::kKwDefault)) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "default");
                if (cursor_.peek().kind == K::kKwDefault) {
                    cursor_.bump();
                }
            }

            if (user_param_count != 0) {
                diag_report(diag::Code::kClassLifecycleDefaultParamNotAllowed, start_tok.span);
            }

            ast::Stmt empty{};
            empty.kind = ast::StmtKind::kBlock;
            empty.span = cursor_.prev().span;
            empty.stmt_begin = static_cast<uint32_t>(ast_.stmt_children().size());
            empty.stmt_count = 0;
            body = ast_.add_stmt(empty);

            end_sp = stmt_consume_semicolon_or_recover(cursor_.prev().span);
        } else {
            const bool prev_actor_ctx = in_actor_member_context_;
            in_actor_member_context_ = true;
            body = parse_stmt_required_block("actor init");
            in_actor_member_context_ = prev_actor_ctx;
            end_sp = ast_.stmt(body).span;
            if (cursor_.at(K::kSemicolon)) {
                end_sp = cursor_.bump().span;
            }
        }

        ast::Stmt s{};
        s.kind = ast::StmtKind::kFnDecl;
        s.span = span_join(start_tok.span, end_sp);
        s.name = std::string_view("init");
        s.type = sig_id;
        s.fn_ret = unit_ty;
        s.fn_ret_type_node = ast::k_invalid_type_node;
        s.a = body;

        s.is_export = false;
        s.is_extern = false;
        s.link_abi = ast::LinkAbi::kNone;
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
        s.fn_is_proto_sig = false;
        s.fn_generic_param_begin = 0;
        s.fn_generic_param_count = 0;
        s.fn_constraint_begin = 0;
        s.fn_constraint_count = 0;
        return ast_.add_stmt(s);
    }

    /// @brief `class Name [: ProtoA, ...] { x: T; static y: T = v; static def ...; ... }`
    ast::StmtId Parser::parse_decl_class() {
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
                        "'extern' is not allowed on class declarations");
            cursor_.bump();
        }

        if (!cursor_.eat(K::kKwClass)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "class");
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

        std::vector<ast::StmtId> members;
        members.reserve(16);
        const uint32_t class_field_member_begin = static_cast<uint32_t>(ast_.field_members().size());
        uint32_t class_field_member_count = 0;
        while (!cursor_.at(K::kRBrace) && !cursor_.at(K::kEof) && !is_aborted()) {
            if (cursor_.eat(K::kSemicolon)) continue;

            const auto k = cursor_.peek().kind;
            const bool lifecycle_start =
                cursor_.peek().kind == K::kKwInit ||
                cursor_.peek().kind == K::kKwDeinit;
            if (lifecycle_start) {
                members.push_back(parse_decl_class_lifecycle_member());
                continue;
            }

            if (k == K::kKwStatic) {
                const Token static_tok = cursor_.bump(); // static

                if (cursor_.at(K::kKwMut)) {
                    diag_report(diag::Code::kClassStaticMutNotAllowed, cursor_.peek().span);
                    cursor_.bump();
                }

                if (cursor_.at(K::kKwLet) || cursor_.at(K::kKwSet) || cursor_.at(K::kKwMut)) {
                    diag_report(diag::Code::kClassMemberLetSetRemoved, cursor_.peek().span);
                    recover_to_delim(K::kSemicolon, K::kRBrace);
                    cursor_.eat(K::kSemicolon);
                    continue;
                }

                const auto sk = cursor_.peek().kind;
                const bool static_fn_start =
                    (sk == K::kAt || sk == K::kKwFn || sk == K::kKwExport || sk == K::kKwExtern ||
                    (sk == K::kKwConst && cursor_.peek(1).kind == K::kKwFn));
                if (static_fn_start) {
                    ast::StmtId mid = parse_decl_fn();
                    auto& ms = ast_.stmt_mut(mid);
                    if (ms.kind == ast::StmtKind::kFnDecl) {
                        ms.is_static = true;
                        if (ms.is_export) {
                            diag_report(diag::Code::kUnexpectedToken, ms.span,
                                        "class member must not be declared with export");
                            ms.is_export = false;
                        }
                        if (ms.is_extern) {
                            diag_report(diag::Code::kUnexpectedToken, ms.span,
                                        "class member must not be declared with extern");
                            ms.is_extern = false;
                            ms.link_abi = ast::LinkAbi::kNone;
                        }
                    }
                    members.push_back(mid);
                    continue;
                }

                bool member_is_const = false;
                if (cursor_.at(K::kKwConst)) {
                    member_is_const = true;
                    cursor_.bump();
                }

                if (cursor_.at(K::kKwMut)) {
                    diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                                "const declaration cannot use 'mut'");
                    cursor_.bump();
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
                if (!cursor_.eat(K::kAssign)) {
                    diag_report(diag::Code::kClassStaticVarRequiresInitializer, static_tok.span);
                } else {
                    if (!(cursor_.at(K::kSemicolon) || cursor_.at(K::kRBrace) || cursor_.at(K::kEof))) {
                        init = parse_expr();
                    } else {
                        diag_report(diag::Code::kVarDeclInitializerExpected, cursor_.peek().span);
                    }
                }

                Span mend = stmt_consume_semicolon_or_recover(cursor_.prev().span);

                ast::Stmt ms{};
                ms.kind = ast::StmtKind::kVar;
                ms.span = span_join(static_tok.span, mend);
                ms.is_set = false;
                ms.is_mut = false;
                ms.is_static = true;
                ms.is_const = member_is_const;
                ms.name = member_name;
                ms.type = type_id;
                ms.type_node = type_node;
                ms.init = init;
                members.push_back(ast_.add_stmt(ms));
                continue;
            }

            if (k == K::kKwLet || k == K::kKwSet || k == K::kKwMut) {
                diag_report(diag::Code::kClassMemberLetSetRemoved, cursor_.peek().span);
                recover_to_delim(K::kSemicolon, K::kRBrace);
                cursor_.eat(K::kSemicolon);
                continue;
            }

            const bool fn_start =
                (k == K::kAt || k == K::kKwFn || k == K::kKwExport || k == K::kKwExtern ||
                (k == K::kKwConst && cursor_.peek(1).kind == K::kKwFn));
            if (fn_start) {
                ast::StmtId mid = parse_decl_fn();
                auto& ms = ast_.stmt_mut(mid);
                if (ms.kind == ast::StmtKind::kFnDecl && ms.is_export) {
                    diag_report(diag::Code::kUnexpectedToken, ms.span,
                                "class member must not be declared with export");
                    ms.is_export = false;
                }
                if (ms.kind == ast::StmtKind::kFnDecl && ms.is_extern) {
                    diag_report(diag::Code::kUnexpectedToken, ms.span,
                                "class member must not be declared with extern");
                    ms.is_extern = false;
                    ms.link_abi = ast::LinkAbi::kNone;
                }
                members.push_back(mid);
                continue;
            }

            if (k == K::kIdent && cursor_.peek(1).kind == K::kColon) {
                const Token name_tok2 = cursor_.peek();
                const std::string_view member_name = name_tok2.lexeme;
                cursor_.bump(); // ident
                cursor_.bump(); // :

                auto parsed_ty = parse_type();
                if (parsed_ty.id == ty::kInvalidType) {
                    recover_to_delim(K::kSemicolon, K::kRBrace);
                    cursor_.eat(K::kSemicolon);
                    continue;
                }

                if (cursor_.eat(K::kAssign)) {
                    diag_report(diag::Code::kClassMemberFieldInitNotAllowed, cursor_.prev().span);
                    if (!(cursor_.at(K::kSemicolon) || cursor_.at(K::kRBrace) || cursor_.at(K::kEof))) {
                        (void)parse_expr();
                    }
                }

                Span end_span = stmt_consume_semicolon_or_recover(cursor_.prev().span);

                ast::FieldMember fm{};
                fm.type = parsed_ty.id;
                fm.type_node = parsed_ty.node;
                fm.name = member_name;
                fm.span = span_join(name_tok2.span, end_span);
                ast_.add_field_member(fm);
                ++class_field_member_count;
                continue;
            }

            diag_report(diag::Code::kUnexpectedToken, cursor_.peek().span,
                        "class member declaration");
            recover_to_delim(K::kSemicolon, K::kRBrace);
            cursor_.eat(K::kSemicolon);
        }

        if (!cursor_.eat(K::kRBrace)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "}");
            recover_to_delim(K::kRBrace, K::kSemicolon);
            cursor_.eat(K::kRBrace);
        }
        const Span end_sp = stmt_consume_semicolon_or_recover(cursor_.prev().span);

        const uint32_t member_begin = static_cast<uint32_t>(ast_.stmt_children().size());
        for (const auto sid : members) {
            ast_.add_stmt_child(sid);
        }

        ast::Stmt s{};
        s.kind = ast::StmtKind::kClassDecl;
        s.span = span_join(start, end_sp);
        s.name = name;
        s.is_export = is_export;
        s.type = name.empty() ? ty::kInvalidType : types_.intern_ident(name);
        s.stmt_begin = member_begin;
        s.stmt_count = static_cast<uint32_t>(members.size());
        s.field_member_begin = class_field_member_begin;
        s.field_member_count = class_field_member_count;
        s.decl_path_ref_begin = impl_begin;
        s.decl_path_ref_count = impl_count;
        s.decl_generic_param_begin = decl_generic_begin;
        s.decl_generic_param_count = decl_generic_count;
        s.decl_constraint_begin = decl_constraint_begin;
        s.decl_constraint_count = decl_constraint_count;
        return ast_.add_stmt(s);
    }

    /// @brief `actor Name { draft { x: T; } init(...){...} def sub|pub ... }`
    ast::StmtId Parser::parse_decl_actor() {
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
                        "'extern' is not allowed on actor declarations");
            cursor_.bump();
        }

        if (!cursor_.eat(K::kKwActor)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "actor");
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

        uint32_t actor_decl_generic_begin = 0;
        uint32_t actor_decl_generic_count = 0;
        const Token actor_generic_tok = cursor_.peek();
        (void)parse_decl_generic_param_clause(actor_decl_generic_begin, actor_decl_generic_count);
        if (actor_decl_generic_count > 0) {
            diag_report(diag::Code::kGenericActorDeclNotSupportedV1, actor_generic_tok.span,
                        name.empty() ? std::string_view("<anonymous>") : name);
        }

        if (cursor_.eat(K::kColon)) {
            diag_report(diag::Code::kUnexpectedToken, cursor_.prev().span,
                        "actor does not support proto/acts inheritance in v0");
            while (!cursor_.at(K::kLBrace) && !cursor_.at(K::kEof) && !is_aborted()) {
                cursor_.bump();
            }
        }

        if (!cursor_.eat(K::kLBrace)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "{");
            recover_to_delim(K::kLBrace, K::kSemicolon, K::kRBrace);
            cursor_.eat(K::kLBrace);
        }

        std::vector<ast::StmtId> members;
        members.reserve(16);

        uint32_t draft_field_begin = static_cast<uint32_t>(ast_.field_members().size());
        uint32_t draft_field_count = 0;
        bool seen_draft = false;

        auto parse_actor_method_member = [&]() -> ast::StmtId {
            const Token def_tok = cursor_.peek();
            if (!cursor_.eat(K::kKwFn)) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "def");
            }

            ast::FnMode mode = ast::FnMode::kNone;
            const Token mode_tok = cursor_.peek();
            const bool is_sub = (mode_tok.kind == K::kKwSub);
            const bool is_pub = (mode_tok.kind == K::kKwPub);
            if (is_sub) {
                mode = ast::FnMode::kSub;
                cursor_.bump();
            } else if (is_pub) {
                mode = ast::FnMode::kPub;
                cursor_.bump();
            } else {
                diag_report(diag::Code::kActorMethodModeRequired, mode_tok.span);
            }

            std::string_view fn_name{};
            const Token fn_name_tok = cursor_.peek();
            if (fn_name_tok.kind == K::kIdent) {
                fn_name = fn_name_tok.lexeme;
                cursor_.bump();
            } else {
                diag_report(diag::Code::kFnNameExpected, fn_name_tok.span);
            }

            uint32_t generic_begin = 0;
            uint32_t generic_count = 0;
            (void)parse_decl_generic_param_clause(generic_begin, generic_count);

            uint32_t param_begin = 0;
            uint32_t param_count = 0;
            uint32_t positional_count = 0;
            bool has_named_group = false;
            parse_decl_fn_params(param_begin, param_count, positional_count, has_named_group);

            auto strip_actor_self_params = [&](uint32_t& begin, uint32_t& count, uint32_t& positional) {
                bool found_self = false;
                for (uint32_t i = 0; i < count; ++i) {
                    const auto param = ast_.params()[begin + i];
                    if (!param.is_self) continue;
                    diag_report(diag::Code::kActorSelfReceiverNotAllowed, param.span);
                    found_self = true;
                }
                if (!found_self) return;

                const uint32_t filtered_begin = static_cast<uint32_t>(ast_.params().size());
                uint32_t filtered_count = 0;
                uint32_t filtered_positional = 0;
                for (uint32_t i = 0; i < count; ++i) {
                    const auto param = ast_.params()[begin + i];
                    if (param.is_self) continue;
                    ast_.add_param(param);
                    ++filtered_count;
                    if (!param.is_named_group) ++filtered_positional;
                }
                begin = filtered_begin;
                count = filtered_count;
                positional = filtered_positional;
            };
            strip_actor_self_params(param_begin, param_count, positional_count);

            uint32_t constraint_begin = 0;
            uint32_t constraint_count = 0;
            (void)parse_decl_fn_constraint_clause(constraint_begin, constraint_count);

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

            const bool prev_actor_ctx = in_actor_member_context_;
            in_actor_member_context_ = true;
            ast::StmtId body = parse_stmt_required_block("actor def");
            in_actor_member_context_ = prev_actor_ctx;
            Span end_sp = ast_.stmt(body).span;
            if (cursor_.at(K::kSemicolon)) {
                end_sp = cursor_.bump().span;
            }

            ast::Param hidden_self{};
            hidden_self.name = std::string_view("draft");
            hidden_self.type = types_.make_borrow(types_.intern_ident("Self"), /*is_mut=*/true);
            hidden_self.type_node = ast::k_invalid_type_node;
            hidden_self.is_mut = false;
            hidden_self.is_self = true;
            hidden_self.self_kind = ast::SelfReceiverKind::kMut;
            hidden_self.is_named_group = false;
            hidden_self.has_default = false;
            hidden_self.default_expr = ast::k_invalid_expr;
            hidden_self.span = def_tok.span;
            ast_.add_param(hidden_self);
            ++param_count;

            std::vector<ty::TypeId> pts;
            std::vector<std::string_view> labels;
            std::vector<uint8_t> has_default_flags;
            pts.reserve(param_count);
            labels.reserve(param_count);
            has_default_flags.reserve(param_count);
            for (uint32_t i = 0; i < param_count; ++i) {
                const auto& p = ast_.params()[param_begin + i];
                pts.push_back(p.type == ty::kInvalidType ? types_.error() : p.type);
                labels.push_back(p.name);
                has_default_flags.push_back(p.has_default ? 1u : 0u);
            }

            const ty::TypeId sig_id = types_.make_fn(
                ret_ty.id,
                pts.empty() ? nullptr : pts.data(),
                static_cast<uint32_t>(pts.size()),
                positional_count,
                labels.empty() ? nullptr : labels.data(),
                has_default_flags.empty() ? nullptr : has_default_flags.data()
            );

            ast::Stmt fs{};
            fs.kind = ast::StmtKind::kFnDecl;
            fs.span = span_join(def_tok.span, end_sp);
            fs.name = fn_name;
            fs.type = sig_id;
            fs.fn_ret = ret_ty.id;
            fs.fn_ret_type_node = ret_ty.node;
            fs.a = body;
            fs.fn_mode = mode;
            fs.is_export = false;
            fs.is_extern = false;
            fs.link_abi = ast::LinkAbi::kNone;
            fs.is_throwing = false;
            fs.fn_is_const = false;
            fs.is_pure = false;
            fs.is_comptime = false;
            fs.is_commit = false;
            fs.is_recast = false;
            fs.attr_begin = 0;
            fs.attr_count = 0;
            fs.param_begin = param_begin;
            fs.param_count = param_count;
            fs.positional_param_count = positional_count;
            fs.has_named_group = has_named_group;
            fs.fn_is_proto_sig = false;
            fs.fn_generic_param_begin = generic_begin;
            fs.fn_generic_param_count = generic_count;
            fs.fn_constraint_begin = constraint_begin;
            fs.fn_constraint_count = constraint_count;
            return ast_.add_stmt(fs);
        };

        auto parse_draft_block = [&](bool accept_members) {
            const Token draft_tok = cursor_.peek();
            cursor_.bump(); // draft

            if (!cursor_.eat(K::kLBrace)) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "{");
                recover_to_delim(K::kLBrace, K::kSemicolon, K::kRBrace);
                cursor_.eat(K::kLBrace);
            }

            const uint32_t local_begin = static_cast<uint32_t>(ast_.field_members().size());
            uint32_t local_count = 0;
            while (!cursor_.at(K::kRBrace) && !cursor_.at(K::kEof) && !is_aborted()) {
                if (cursor_.eat(K::kSemicolon)) continue;

                const Token member_name_tok = cursor_.peek();
                if (member_name_tok.kind != K::kIdent) {
                    diag_report(diag::Code::kFieldMemberNameExpected, member_name_tok.span);
                    recover_to_delim(K::kSemicolon, K::kRBrace);
                    cursor_.eat(K::kSemicolon);
                    continue;
                }
                cursor_.bump(); // name

                if (!cursor_.eat(K::kColon)) {
                    diag_report(diag::Code::kExpectedToken, cursor_.peek().span, ":");
                    recover_to_delim(K::kSemicolon, K::kRBrace);
                    cursor_.eat(K::kSemicolon);
                    continue;
                }

                auto parsed_ty = parse_type();
                if (parsed_ty.id == ty::kInvalidType) {
                    recover_to_delim(K::kSemicolon, K::kRBrace);
                    cursor_.eat(K::kSemicolon);
                    continue;
                }

                Span end_span = stmt_consume_semicolon_or_recover(cursor_.prev().span);
                if (accept_members) {
                    ast::FieldMember fm{};
                    fm.type = parsed_ty.id;
                    fm.type_node = parsed_ty.node;
                    fm.name = member_name_tok.lexeme;
                    fm.span = span_join(draft_tok.span, end_span);
                    ast_.add_field_member(fm);
                    ++local_count;
                }
            }

            if (!cursor_.eat(K::kRBrace)) {
                diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "}");
                recover_to_delim(K::kRBrace, K::kSemicolon);
                cursor_.eat(K::kRBrace);
            }

            if (accept_members) {
                draft_field_begin = local_begin;
                draft_field_count = local_count;
            }
        };

        while (!cursor_.at(K::kRBrace) && !cursor_.at(K::kEof) && !is_aborted()) {
            if (cursor_.eat(K::kSemicolon)) continue;

            const Token t = cursor_.peek();
            if (t.kind == K::kKwDraft) {
                if (seen_draft) {
                    diag_report(diag::Code::kActorRequiresSingleDraft, t.span);
                    parse_draft_block(/*accept_members=*/false);
                } else {
                    seen_draft = true;
                    parse_draft_block(/*accept_members=*/true);
                }
                continue;
            }

            if (t.kind == K::kKwInit) {
                members.push_back(parse_decl_actor_init_member());
                continue;
            }

            if (t.kind == K::kKwDeinit) {
                diag_report(diag::Code::kActorDeinitNotAllowed, t.span);
                const ast::StmtId bad_sid = parse_decl_class_lifecycle_member();
                auto& bad = ast_.stmt_mut(bad_sid);
                bad.kind = ast::StmtKind::kError;
                continue;
            }

            if (t.kind == K::kKwConst && cursor_.peek(1).kind == K::kKwFn) {
                diag_report(diag::Code::kUnexpectedToken, t.span,
                            "const def is not allowed in actor methods");
                cursor_.bump(); // const
                members.push_back(parse_actor_method_member());
                continue;
            }

            if (t.kind == K::kKwStatic || t.kind == K::kKwLet || t.kind == K::kKwSet || t.kind == K::kKwMut) {
                diag_report(diag::Code::kActorMemberNotAllowed, t.span);
                recover_to_delim(K::kSemicolon, K::kRBrace);
                cursor_.eat(K::kSemicolon);
                continue;
            }

            if (t.kind == K::kKwFn) {
                members.push_back(parse_actor_method_member());
                continue;
            }

            if (t.kind == K::kAt || t.kind == K::kKwExport || t.kind == K::kKwExtern) {
                diag_report(diag::Code::kUnexpectedToken, t.span,
                            "actor methods use 'def sub|pub ...' without attribute/export/extern in v0");
                recover_to_delim(K::kSemicolon, K::kRBrace);
                cursor_.eat(K::kSemicolon);
                continue;
            }

            diag_report(diag::Code::kUnexpectedToken, t.span,
                        "actor member declaration");
            recover_to_delim(K::kSemicolon, K::kRBrace);
            cursor_.eat(K::kSemicolon);
        }

        if (!cursor_.eat(K::kRBrace)) {
            diag_report(diag::Code::kExpectedToken, cursor_.peek().span, "}");
            recover_to_delim(K::kRBrace, K::kSemicolon);
            cursor_.eat(K::kRBrace);
        }
        const Span end_sp = stmt_consume_semicolon_or_recover(cursor_.prev().span);

        if (!seen_draft) {
            diag_report(diag::Code::kActorRequiresSingleDraft, span_join(start, end_sp));
        }

        const uint32_t member_begin = static_cast<uint32_t>(ast_.stmt_children().size());
        for (const auto sid : members) {
            ast_.add_stmt_child(sid);
        }

        ast::Stmt s{};
        s.kind = ast::StmtKind::kActorDecl;
        s.span = span_join(start, end_sp);
        s.name = name;
        s.is_export = is_export;
        s.type = name.empty() ? ty::kInvalidType : types_.intern_ident(name);
        s.stmt_begin = member_begin;
        s.stmt_count = static_cast<uint32_t>(members.size());
        s.field_member_begin = draft_field_begin;
        s.field_member_count = seen_draft ? draft_field_count : 0;
        return ast_.add_stmt(s);
    }

    /// @brief `operator(KEY)(...) -> T { ... }` acts 멤버를 함수 선언 노드로 파싱한다.
