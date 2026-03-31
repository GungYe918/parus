    static std::vector<std::string> candidate_names_for_external_export_(
        const NameResolveOptions::ExternalExport& ex,
        std::string_view current_module_head
    ) {
        return parus::candidate_names_for_external_export(
            ex.path,
            ex.module_head,
            ex.decl_bundle_name,
            current_module_head
        );
    }

    static std::optional<uint32_t> lookup_external_export_fallback_(
        sema::SymbolTable& sym,
        std::string_view raw_name,
        const std::unordered_map<std::string, std::string>& import_aliases,
        const std::unordered_set<std::string>& known_namespace_paths,
        const NameResolveOptions& opt
    ) {
        if (raw_name.empty() || opt.external_exports.empty()) return std::nullopt;

        std::string name(raw_name);
        if (auto rewritten = rewrite_imported_path_(name, import_aliases)) {
            name = *rewritten;
        } else if (qualified_path_requires_import_(raw_name, opt.current_module_head, known_namespace_paths)) {
            return std::nullopt;
        }

        auto try_match = [&](std::string_view candidate) -> std::optional<uint32_t> {
            if (candidate.empty()) return std::nullopt;

            for (const auto& ex : opt.external_exports) {
                const auto names = candidate_names_for_external_export_(ex, opt.current_module_head);
                for (const auto& nm : names) {
                    if (nm != candidate) continue;

                    if (auto existing = sym.lookup(nm)) {
                        if (ex.kind == sema::SymbolKind::kFn && !ex.link_name.empty()) {
                            auto& cur = sym.symbol_mut(*existing);
                            if (cur.link_name.empty()) cur.link_name = ex.link_name;
                        }
                        return *existing;
                    }

                    auto ins = sym.insert(
                        ex.kind,
                        nm,
                        ex.declared_type,
                        ex.decl_span,
                        ex.decl_span.file_id,
                        ex.decl_bundle_name,
                        ex.is_export,
                        /*is_external=*/true,
                        ex.module_head,
                        ex.decl_source_dir_norm,
                        ex.link_name,
                        ex.inst_payload
                    );
                    if (ins.ok || ins.is_duplicate) {
                        return ins.symbol_id;
                    }
                }
            }
            return std::nullopt;
        };

        if (auto sid = try_match(name)) {
            return sid;
        }
        return std::nullopt;
    }

    static bool is_explicit_acts_path_expr_(std::string_view text) {
        constexpr std::string_view marker = "::acts(";
        const size_t marker_pos = text.find(marker);
        if (marker_pos == std::string_view::npos) return false;

        const size_t close_pos = text.find(')', marker_pos + marker.size());
        if (close_pos == std::string_view::npos) return false;
        if (close_pos + 2 >= text.size()) return false;
        if (text[close_pos + 1] != ':' || text[close_pos + 2] != ':') return false;
        if (close_pos + 3 >= text.size()) return false;
        return true;
    }

    // -----------------------------------------------------------------------------
    // Expr walk (Ident resolve)
    // -----------------------------------------------------------------------------
    static void walk_expr(
        const ast::AstArena& ast,
        const IdRanges& r,
        ast::ExprId root,
        sema::SymbolTable& sym,
        diag::Bag& bag,
        const NameResolveOptions& opt,
        NameResolveResult& out,
        std::unordered_set<uint32_t>& param_symbol_ids,
        std::vector<std::string>& namespace_stack,
        std::unordered_map<std::string, std::string>& import_aliases,
        const std::unordered_set<std::string>& known_namespace_paths
    ) {
        if (root == ast::k_invalid_expr) return;

        ensure_result_sized_(r, out);

        std::vector<uint8_t> visited(r.expr_count, 0);
        std::vector<ast::ExprId> stack;
        stack.reserve(64);
        stack.push_back(root);

        while (!stack.empty()) {
            ast::ExprId id = stack.back();
            stack.pop_back();

            if (!is_valid_expr_id_(r, id)) continue;
            const uint32_t idx = (uint32_t)id;
            if (visited[idx]) continue;
            visited[idx] = 1;

            const auto& e = ast.expr(id);

            switch (e.kind) {
                case ast::ExprKind::kIdent: {
                    // v0: explicit acts path call callee
                    //   T::acts(Set)::member(...)
                    // 는 tyck의 acts 해소 단계에서 검증한다.
                    // name_resolve 단계에서 일반 symbol lookup을 강제하면
                    // false positive UndefinedName이 발생하므로 여기서 제외한다.
                    if (is_explicit_acts_path_expr_(e.text)) {
                        break;
                    }

                    // v0: enum constructor candidate
                    //   Type::Variant(...)
                    // 는 tyck에서 enum ctor/path-call 정책으로 최종 판정한다.
                    // 여기서 강제 lookup하면 false positive UndefinedName이 생길 수 있다.
                    if (e.text.find("::") != std::string_view::npos) {
                        const std::string_view full = e.text;
                        const size_t split = full.rfind("::");
                        if (split != std::string_view::npos && split > 0 && split + 2 < full.size()) {
                            std::string owner(full.substr(0, split));
                            // Strip owner generic args for lookup: Token<i32> -> Token
                            const size_t lt = owner.find('<');
                            if (lt != std::string::npos) owner.resize(lt);

                            auto owner_sid = lookup_symbol_(sym, owner, namespace_stack, import_aliases,
                                                            known_namespace_paths, opt.current_module_head);
                            if (owner_sid.has_value()) {
                                const auto& owner_sym = sym.symbol(*owner_sid);
                                if (owner_sym.kind == sema::SymbolKind::kType) {
                                    break;
                                }
                            }
                        }
                    }

                    if (opt.warn_core_path_when_std) {
                        const std::string_view text = e.text;
                        if (text == "core" || text.starts_with("core::")) {
                            report(
                                bag,
                                diag::Severity::kWarning,
                                diag::Code::kTypeErrorGeneric,
                                e.span,
                                "std is enabled; prefer 'std::...' over direct 'core::...' path usage"
                            );
                        }
                    }

                    auto sid = lookup_symbol_(sym, e.text, namespace_stack, import_aliases,
                                              known_namespace_paths, opt.current_module_head);
                    if (!sid) {
                        sid = lookup_external_export_fallback_(sym, e.text, import_aliases, known_namespace_paths, opt);
                    }
                    if (!sid) {
                        report(bag, diag::Severity::kError, diag::Code::kUndefinedName, e.span, e.text);
                    } else {
                        const auto& symobj = sym.symbol(*sid);
                        if (!is_symbol_visible_from_use_site_(symobj, e.span.file_id, opt)) {
                            diag::Diagnostic d(
                                diag::Severity::kError,
                                (!opt.current_bundle_name.empty() &&
                                 !symobj.decl_bundle_name.empty() &&
                                 symobj.decl_bundle_name != opt.current_bundle_name)
                                    ? diag::Code::kSymbolNotExportedBundleScope
                                    : diag::Code::kSymbolNotExportedFileScope,
                                e.span
                            );
                            d.add_arg(e.text);
                            if (symobj.is_external && !symobj.is_export) {
                                d.add_note("closure-private helper declarations are only materialized inside imported generic bodies");
                                d.add_note("source-level import ergonomics does not make hidden helper symbols directly visible");
                                d.add_help("reference the exported root API instead, or export this helper through a public path");
                            }
                            if (!opt.current_bundle_name.empty() &&
                                !symobj.decl_bundle_name.empty() &&
                                symobj.decl_bundle_name != opt.current_bundle_name) {
                                bag.add(std::move(d));
                            } else {
                                bag.add(std::move(d));
                            }
                            break;
                        }

                        // BindingKind 결정:
                        // - v0에서는 "param인지"가 중요하므로, pass 내부에서 param symbol id set을 유지한다.
                        BindingKind bk = BindingKind::kLocalVar;
                        if (param_symbol_ids.find(*sid) != param_symbol_ids.end()) {
                            bk = BindingKind::kParam;
                        } else {
                            // fallback by SymbolKind (확장 대비)
                            if (symobj.kind == sema::SymbolKind::kFn) bk = BindingKind::kFn;
                            else if (symobj.kind == sema::SymbolKind::kType) bk = BindingKind::kType;
                            else if (symobj.kind == sema::SymbolKind::kInst) bk = BindingKind::kType;
                            else bk = BindingKind::kLocalVar;
                        }

                        const auto rid = add_resolved_(out, bk, (uint32_t)(*sid), e.span);
                        out.expr_to_resolved[idx] = rid;
                    }
                    break;
                }

                case ast::ExprKind::kCast:
                case ast::ExprKind::kUnary:
                case ast::ExprKind::kPostfixUnary: {
                    if (is_valid_expr_id_(r, e.a)) stack.push_back(e.a);
                    break;
                }

                case ast::ExprKind::kBinary:
                case ast::ExprKind::kAssign:
                case ast::ExprKind::kIndex: {
                    if (is_valid_expr_id_(r, e.a)) stack.push_back(e.a);
                    if (e.kind == ast::ExprKind::kBinary &&
                        (e.op == syntax::TokenKind::kDot || e.op == syntax::TokenKind::kArrow)) {
                        // member access(. / ->) rhs 식별자는 심볼 이름 해소 대상으로 보지 않는다.
                        break;
                    }
                    if (is_valid_expr_id_(r, e.b)) stack.push_back(e.b);
                    break;
                }

                case ast::ExprKind::kTernary: {
                    if (is_valid_expr_id_(r, e.a)) stack.push_back(e.a);
                    if (is_valid_expr_id_(r, e.b)) stack.push_back(e.b);
                    if (is_valid_expr_id_(r, e.c)) stack.push_back(e.c);
                    break;
                }

                case ast::ExprKind::kCall: {
                    if (is_valid_expr_id_(r, e.a)) stack.push_back(e.a);

                    const uint32_t arg_end = e.arg_begin + e.arg_count;
                    if (e.arg_begin < r.arg_count && arg_end <= r.arg_count) {
                        const auto& args = ast.args();

                        for (uint32_t i = 0; i < e.arg_count; ++i) {
                            const auto& a = args[e.arg_begin + i];

                            if (!a.is_hole && is_valid_expr_id_(r, a.expr)) {
                                stack.push_back(a.expr);
                            }
                        }
                    }
                    break;
                }

                case ast::ExprKind::kArrayLit: {
                    const uint32_t arg_end = e.arg_begin + e.arg_count;
                    if (e.arg_begin < r.arg_count && arg_end <= r.arg_count) {
                        const auto& args = ast.args();
                        for (uint32_t i = 0; i < e.arg_count; ++i) {
                            const auto& a = args[e.arg_begin + i];
                            if (!a.is_hole && is_valid_expr_id_(r, a.expr)) {
                                stack.push_back(a.expr);
                            }
                        }
                    }
                    break;
                }

                case ast::ExprKind::kFieldInit: {
                    const auto& inits = ast.field_init_entries();
                    const uint64_t begin = e.field_init_begin;
                    const uint64_t end = begin + e.field_init_count;
                    if (begin <= inits.size() && end <= inits.size()) {
                        for (uint32_t i = 0; i < e.field_init_count; ++i) {
                            const auto& ent = inits[e.field_init_begin + i];
                            if (is_valid_expr_id_(r, ent.expr)) {
                                stack.push_back(ent.expr);
                            }
                        }
                    }
                    break;
                }

                case ast::ExprKind::kLoop: {
                    // loop expression introduces its own scope for header var.
                    ScopeGuard g(sym);

                    // Iter expression should be resolved BEFORE loop variable declaration.
                    // (loop header variable is body-local in v0 policy.)
                    if (is_valid_expr_id_(r, e.loop_iter)) {
                        walk_expr(ast, r, e.loop_iter, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths);
                    }

                    if (e.loop_has_header && !e.loop_var.empty()) {
                        const auto ins = declare_(
                            sema::SymbolKind::kVar,
                            e.loop_var,
                            ast::k_invalid_type,
                            e.span,
                            sym, bag, opt
                        );
                        if (ins.ok && !ins.is_duplicate) {
                            const auto rid = add_resolved_(out, BindingKind::kLocalVar, ins.symbol_id, e.span);
                            out.expr_loop_var_to_resolved[(uint32_t)id] = rid;
                        }
                    }

                    // IMPORTANT: loop body is StmtId.
                    if (is_valid_stmt_id_(r, e.loop_body)) {
                        walk_stmt(ast, r, e.loop_body, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, /*file_scope=*/false);
                    }
                    break;
                }

                case ast::ExprKind::kIfExpr: {
                    if (is_valid_expr_id_(r, e.a)) stack.push_back(e.a);
                    if (is_valid_expr_id_(r, e.b)) stack.push_back(e.b);
                    if (is_valid_expr_id_(r, e.c)) stack.push_back(e.c);
                    break;
                }

                case ast::ExprKind::kBlockExpr: {
                    const ast::StmtId blk = e.block_stmt;
                    if (is_valid_stmt_id_(r, blk)) {
                        walk_stmt(ast, r, blk, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, /*file_scope=*/false);
                    }
                    if (is_valid_expr_id_(r, e.block_tail)) stack.push_back(e.block_tail);
                    break;
                }

                default:
                    // literals/null/hole/error 등
                    break;
            }
        }
    }

    // -----------------------------------------------------------------------------
    // block children
    // -----------------------------------------------------------------------------
    static void walk_block_children_(
        const ast::AstArena& ast,
        const IdRanges& r,
        const ast::Stmt& s,
        sema::SymbolTable& sym,
        diag::Bag& bag,
        const NameResolveOptions& opt,
        NameResolveResult& out,
        std::unordered_set<uint32_t>& param_symbol_ids,
        std::vector<std::string>& namespace_stack,
        std::unordered_map<std::string, std::string>& import_aliases,
        const std::unordered_set<std::string>& known_namespace_paths,
        bool file_scope
    ) {
        const auto& kids = ast.stmt_children();
        const uint32_t begin = s.stmt_begin;
        const uint32_t end   = s.stmt_begin + s.stmt_count;

        // 방어: 깨진 AST에서도 out-of-range 방지
        if (begin >= r.stmt_children_count) return;
        if (end > r.stmt_children_count) return;

        for (uint32_t i = begin; i < end; ++i) {
            walk_stmt(ast, r, kids[i], sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, file_scope);
        }
    }

    // -----------------------------------------------------------------------------
    // Stmt walk + scope
    // -----------------------------------------------------------------------------
    static void walk_stmt(
        const ast::AstArena& ast,
        const IdRanges& r,
        ast::StmtId id,
        sema::SymbolTable& sym,
        diag::Bag& bag,
        const NameResolveOptions& opt,
        NameResolveResult& out,
        std::unordered_set<uint32_t>& param_symbol_ids,
        std::vector<std::string>& namespace_stack,
        std::unordered_map<std::string, std::string>& import_aliases,
        const std::unordered_set<std::string>& known_namespace_paths,
        bool file_scope
    ) {
        if (!is_valid_stmt_id_(r, id)) return;
        ensure_result_sized_(r, out);

        const auto& s = ast.stmt(id);

        switch (s.kind) {
            case ast::StmtKind::kEmpty:
                return;

            case ast::StmtKind::kExprStmt:
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths);
                return;

            case ast::StmtKind::kVar: {
                // init 먼저
                if (s.init != ast::k_invalid_expr) {
                    walk_expr(ast, r, s.init, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths);
                }
                if (s.var_has_consume_else && s.b != ast::k_invalid_stmt) {
                    walk_stmt(ast, r, s.b, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, /*file_scope=*/false);
                }

                const bool is_global_decl =
                    s.is_static || s.is_const || s.is_extern || s.is_export || (s.link_abi == ast::LinkAbi::kC);
                const std::string decl_name = is_global_decl
                    ? qualify_name_(namespace_stack, s.name)
                    : std::string(s.name);

                uint32_t var_sym = sema::SymbolTable::kNoScope;
                if (auto sid_existing = sym.lookup(decl_name)) {
                    const auto& existing = sym.symbol(*sid_existing);
                    if (existing.kind == sema::SymbolKind::kVar) {
                        var_sym = *sid_existing;
                    } else {
                        report(bag, diag::Severity::kError, diag::Code::kDuplicateDecl, s.span, decl_name);
                    }
                } else {
                    auto ins = declare_(sema::SymbolKind::kVar, decl_name, s.type, s.span, sym, bag, opt);
                    if (ins.ok && !ins.is_duplicate) {
                        var_sym = ins.symbol_id;
                    }
                }

                if (var_sym != sema::SymbolTable::kNoScope) {
                    const auto rid = add_resolved_(out, BindingKind::kLocalVar, var_sym, s.span);
                    out.stmt_to_resolved[(uint32_t)id] = rid;
                }
                return;
            }

            case ast::StmtKind::kBlock: {
                ScopeGuard g(sym);
                AliasScopeGuard ag(import_aliases);
                walk_block_children_(ast, r, s, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, file_scope);
                return;
            }

            case ast::StmtKind::kIf:
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths);
                walk_stmt(ast, r, s.a, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, /*file_scope=*/false);
                walk_stmt(ast, r, s.b, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, /*file_scope=*/false);
                return;

            case ast::StmtKind::kFor: {
                ScopeGuard g(sym);
                AliasScopeGuard ag(import_aliases);
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths);
                if (!s.name.empty()) {
                    const auto ins = declare_(
                        sema::SymbolKind::kVar,
                        s.name,
                        ast::k_invalid_type,
                        s.span,
                        sym, bag, opt
                    );
                    if (ins.ok && !ins.is_duplicate) {
                        const auto rid = add_resolved_(out, BindingKind::kLocalVar, ins.symbol_id, s.span);
                        out.stmt_for_var_to_resolved[(uint32_t)id] = rid;
                    }
                }
                walk_stmt(ast, r, s.a, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, /*file_scope=*/false);
                return;
            }

            case ast::StmtKind::kWhile:
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths);
                walk_stmt(ast, r, s.a, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, /*file_scope=*/false);
                return;
            case ast::StmtKind::kDoScope:
                walk_stmt(ast, r, s.a, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, /*file_scope=*/false);
                return;
            case ast::StmtKind::kDoWhile:
                walk_stmt(ast, r, s.a, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, /*file_scope=*/false);
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths);
                return;
            case ast::StmtKind::kManual:
                walk_stmt(ast, r, s.a, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, /*file_scope=*/false);
                return;

            case ast::StmtKind::kReturn:
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths);
                return;

            case ast::StmtKind::kRequire:
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths);
                return;

            case ast::StmtKind::kThrow:
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths);
                return;

            case ast::StmtKind::kBreak:
            case ast::StmtKind::kContinue:
            case ast::StmtKind::kCommitStmt:
            case ast::StmtKind::kRecastStmt:
            case ast::StmtKind::kAssocTypeDecl:
                return;

            case ast::StmtKind::kTryCatch: {
                walk_stmt(ast, r, s.a, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, /*file_scope=*/false);

                auto& clauses = const_cast<ast::AstArena&>(ast).try_catch_clauses_mut();
                const uint64_t begin = s.catch_clause_begin;
                const uint64_t end = begin + s.catch_clause_count;
                if (begin > clauses.size() || end > clauses.size()) {
                    return;
                }

                for (uint32_t i = 0; i < s.catch_clause_count; ++i) {
                    auto& cc = clauses[s.catch_clause_begin + i];
                    ScopeGuard g(sym);
                    AliasScopeGuard ag(import_aliases);
                    cc.resolved_symbol = sema::SymbolTable::kNoScope;

                    if (cc.bind_name.empty()) {
                        report(bag, diag::Severity::kError, diag::Code::kCatchBinderNameExpected, cc.span);
                    } else {
                        auto ins = declare_(sema::SymbolKind::kVar, cc.bind_name, cc.bind_type, cc.span, sym, bag, opt);
                        if (ins.ok && !ins.is_duplicate) {
                            cc.resolved_symbol = ins.symbol_id;
                            (void)add_resolved_(out, BindingKind::kLocalVar, ins.symbol_id, cc.span);
                        }
                    }

                    walk_stmt(ast, r, cc.body, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, /*file_scope=*/false);
                }
                return;
            }

            case ast::StmtKind::kFnDecl: {
                const std::string qname = qualify_name_(namespace_stack, s.name);
                uint32_t fn_sym = sema::SymbolTable::kNoScope;
                if (auto sid = sym.lookup(qname)) {
                    fn_sym = *sid;
                } else {
                    auto fins = declare_(sema::SymbolKind::kFn, qname, s.type, s.span, sym, bag, opt);
                    fn_sym = fins.symbol_id;
                }

                if (fn_sym != sema::SymbolTable::kNoScope) {
                    const auto rid = add_resolved_(out, BindingKind::kFn, fn_sym, s.span);
                    out.stmt_to_resolved[(uint32_t)id] = rid;
                }

                // 2) 함수 바디 스코프
                ScopeGuard g(sym);
                AliasScopeGuard ag(import_aliases);

                // 3) 파라미터 등록
                const uint32_t pb = s.param_begin;
                const uint32_t pe = s.param_begin + s.param_count;
                if (pb < r.param_count && pe <= r.param_count) {
                    const auto& ps = ast.params();
                    for (uint32_t i = pb; i < pe; ++i) {
                        const auto& p = ps[i];

                        // param is var
                        auto pins = declare_(sema::SymbolKind::kVar, p.name, p.type, p.span, sym, bag, opt);

                        // param binding 기록 (SymbolId -> param set, param index -> resolved)
                        if (!pins.is_duplicate && is_valid_param_index_(r, i)) {
                            param_symbol_ids.insert(pins.symbol_id);
                            const auto prid = add_resolved_(out, BindingKind::kParam, pins.symbol_id, p.span);
                            out.param_to_resolved[i] = prid;
                        }

                        // default expr 내부 이름 사용 검사
                        if (p.has_default && p.default_expr != ast::k_invalid_expr) {
                            walk_expr(ast, r, p.default_expr, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths);
                        }
                    }
                }

                // 4) body
                walk_stmt(ast, r, s.a, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, /*file_scope=*/false);
                return;
            }

            case ast::StmtKind::kFieldDecl: {
                const std::string qname = qualify_name_(namespace_stack, s.name);
                uint32_t field_sym = sema::SymbolTable::kNoScope;
                if (auto sid = sym.lookup(qname)) {
                    field_sym = *sid;
                } else {
                    auto ins = declare_(sema::SymbolKind::kField, qname, ast::k_invalid_type, s.span, sym, bag, opt);
                    field_sym = ins.symbol_id;
                }

                if (field_sym != sema::SymbolTable::kNoScope) {
                    const auto rid = add_resolved_(out, BindingKind::kType, field_sym, s.span);
                    out.stmt_to_resolved[(uint32_t)id] = rid;
                }
                return;
            }

            case ast::StmtKind::kEnumDecl: {
                const std::string qname = qualify_name_(namespace_stack, s.name);
                uint32_t enum_sym = sema::SymbolTable::kNoScope;
                if (auto sid = sym.lookup(qname)) {
                    enum_sym = *sid;
                } else {
                    auto ins = declare_(sema::SymbolKind::kType, qname, ast::k_invalid_type, s.span, sym, bag, opt);
                    enum_sym = ins.symbol_id;
                }
                if (enum_sym != sema::SymbolTable::kNoScope) {
                    const auto rid = add_resolved_(out, BindingKind::kType, enum_sym, s.span);
                    out.stmt_to_resolved[(uint32_t)id] = rid;
                }
                return;
            }

            case ast::StmtKind::kProtoDecl: {
                const std::string qname = qualify_name_(namespace_stack, s.name);
                uint32_t proto_sym = sema::SymbolTable::kNoScope;
                if (auto sid = sym.lookup(qname)) {
                    proto_sym = *sid;
                } else {
                    auto ins = declare_(sema::SymbolKind::kType, qname, ast::k_invalid_type, s.span, sym, bag, opt);
                    proto_sym = ins.symbol_id;
                }
                if (proto_sym != sema::SymbolTable::kNoScope) {
                    const auto rid = add_resolved_(out, BindingKind::kType, proto_sym, s.span);
                    out.stmt_to_resolved[(uint32_t)id] = rid;
                }

                // proto member signatures: default args and constraint exprs are still expressions.
                const auto& kids = ast.stmt_children();
                const uint64_t begin = s.stmt_begin;
                const uint64_t end = begin + s.stmt_count;
                if (begin <= kids.size() && end <= kids.size()) {
                    ScopeGuard g(sym);
                    AliasScopeGuard ag(import_aliases);
                    for (uint32_t i = 0; i < s.stmt_count; ++i) {
                        walk_stmt(ast, r, kids[s.stmt_begin + i], sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, /*file_scope=*/false);
                    }
                }
                return;
            }

            case ast::StmtKind::kClassDecl: {
                const std::string qname = qualify_name_(namespace_stack, s.name);
                uint32_t class_sym = sema::SymbolTable::kNoScope;
                if (auto sid = sym.lookup(qname)) {
                    class_sym = *sid;
                } else {
                    auto ins = declare_(sema::SymbolKind::kType, qname, ast::k_invalid_type, s.span, sym, bag, opt);
                    class_sym = ins.symbol_id;
                }
                if (class_sym != sema::SymbolTable::kNoScope) {
                    const auto rid = add_resolved_(out, BindingKind::kType, class_sym, s.span);
                    out.stmt_to_resolved[(uint32_t)id] = rid;
                }

                ScopeGuard g(sym);
                AliasScopeGuard ag(import_aliases);
                const auto& kids = ast.stmt_children();
                const uint64_t begin = s.stmt_begin;
                const uint64_t end = begin + s.stmt_count;
                if (begin <= kids.size() && end <= kids.size()) {
                    auto find_proto_decl_by_symbol = [&](uint32_t proto_sym) -> std::optional<ast::StmtId> {
                        for (uint32_t psid = 0; psid < r.stmt_count; ++psid) {
                            const auto& ps = ast.stmt((ast::StmtId)psid);
                            if (ps.kind != ast::StmtKind::kProtoDecl) continue;
                            if (psid >= out.stmt_to_resolved.size()) continue;
                            const auto rid = out.stmt_to_resolved[psid];
                            if (rid == NameResolveResult::k_invalid_resolved) continue;
                            if (rid >= out.resolved.size()) continue;
                            if (out.resolved[rid].sym == proto_sym) {
                                return (ast::StmtId)psid;
                            }
                        }
                        return std::nullopt;
                    };

                    auto resolve_proto_sid = [&](std::string_view raw) -> std::optional<ast::StmtId> {
                        if (raw.empty()) return std::nullopt;

                        if (auto proto_sym = lookup_symbol_(sym, raw, namespace_stack, import_aliases,
                                                            known_namespace_paths, opt.current_module_head)) {
                            if (auto psid = find_proto_decl_by_symbol(*proto_sym)) {
                                return psid;
                            }
                        }

                        std::string tail(raw);
                        if (const size_t pos = tail.rfind("::"); pos != std::string::npos) {
                            tail = tail.substr(pos + 2);
                        }
                        if (tail.empty()) return std::nullopt;

                        ast::StmtId found = ast::k_invalid_stmt;
                        bool ambiguous = false;
                        for (uint32_t psid = 0; psid < r.stmt_count; ++psid) {
                            const auto& ps = ast.stmt((ast::StmtId)psid);
                            if (ps.kind != ast::StmtKind::kProtoDecl) continue;
                            if (ps.name != tail) continue;
                            if (found == ast::k_invalid_stmt) {
                                found = (ast::StmtId)psid;
                            } else {
                                ambiguous = true;
                                break;
                            }
                        }
                        if (!ambiguous && found != ast::k_invalid_stmt) return found;
                        return std::nullopt;
                    };

                    auto predeclare_proto_defaults = [&](auto&& self,
                                                         ast::StmtId proto_sid,
                                                         std::unordered_set<ast::StmtId>& visiting) -> void {
                        if (!is_valid_stmt_id_(r, proto_sid)) return;
                        if (!visiting.insert(proto_sid).second) return;
                        const auto& ps = ast.stmt(proto_sid);
                        if (ps.kind != ast::StmtKind::kProtoDecl) return;

                        const auto& p_refs = ast.path_refs();
                        const uint32_t ib = ps.decl_path_ref_begin;
                        const uint32_t ie = ps.decl_path_ref_begin + ps.decl_path_ref_count;
                        if (ib <= p_refs.size() && ie <= p_refs.size()) {
                            for (uint32_t i = ib; i < ie; ++i) {
                                const auto& pr = p_refs[i];
                                const std::string base_path = path_join_(ast, pr.path_begin, pr.path_count);
                                if (auto base_sid = resolve_proto_sid(base_path)) {
                                    self(self, *base_sid, visiting);
                                }
                            }
                        }

                        const auto& p_kids = ast.stmt_children();
                        const uint64_t mb = ps.stmt_begin;
                        const uint64_t me = mb + ps.stmt_count;
                        if (mb <= p_kids.size() && me <= p_kids.size()) {
                            for (uint32_t i = 0; i < ps.stmt_count; ++i) {
                                const ast::StmtId msid = p_kids[ps.stmt_begin + i];
                                if (!is_valid_stmt_id_(r, msid)) continue;
                                const auto& ms = ast.stmt(msid);
                                if (ms.kind != ast::StmtKind::kFnDecl) continue;
                                if (ms.a == ast::k_invalid_stmt) continue; // signature-only member
                                (void)declare_(sema::SymbolKind::kFn, ms.name, ms.type, ms.span, sym, bag, opt);
                            }
                        }
                    };

                    // predeclare class member functions for same-class forward references.
                    for (uint32_t i = 0; i < s.stmt_count; ++i) {
                        const ast::StmtId msid = kids[s.stmt_begin + i];
                        if (!is_valid_stmt_id_(r, msid)) continue;
                        const auto& ms = ast.stmt(msid);
                        if (ms.kind != ast::StmtKind::kFnDecl || ms.name.empty()) continue;
                        (void)declare_(sema::SymbolKind::kFn, ms.name, ms.type, ms.span, sym, bag, opt);
                    }

                    // predeclare class static members so static init expressions can reference
                    // sibling members with bare names (e.g., `static const B = A + 1;`).
                    for (uint32_t i = 0; i < s.stmt_count; ++i) {
                        const ast::StmtId msid = kids[s.stmt_begin + i];
                        if (!is_valid_stmt_id_(r, msid)) continue;
                        const auto& ms = ast.stmt(msid);
                        if (ms.kind != ast::StmtKind::kVar || !ms.is_static || ms.name.empty()) continue;
                        (void)declare_(sema::SymbolKind::kVar, ms.name, ms.type, ms.span, sym, bag, opt);
                    }

                    // predeclare proto default members so class member bodies can call them unqualified.
                    const auto& refs = ast.path_refs();
                    const uint32_t pb = s.decl_path_ref_begin;
                    const uint32_t pe = s.decl_path_ref_begin + s.decl_path_ref_count;
                    if (pb <= refs.size() && pe <= refs.size()) {
                        std::unordered_set<ast::StmtId> visiting;
                        for (uint32_t i = pb; i < pe; ++i) {
                            const auto& pr = refs[i];
                            const std::string proto_path = path_join_(ast, pr.path_begin, pr.path_count);
                            if (auto proto_sid = resolve_proto_sid(proto_path)) {
                                predeclare_proto_defaults(predeclare_proto_defaults, *proto_sid, visiting);
                            }
                        }
                    }

                    for (uint32_t i = 0; i < s.stmt_count; ++i) {
                        const ast::StmtId msid = kids[s.stmt_begin + i];
                        if (!is_valid_stmt_id_(r, msid)) continue;
                        const auto& ms = ast.stmt(msid);
                        if (ms.kind == ast::StmtKind::kVar && ms.is_static) {
                            if (ms.init != ast::k_invalid_expr) {
                                walk_expr(ast, r, ms.init, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths);
                            }

                            std::string qvar = qname;
                            if (!qvar.empty()) qvar += "::";
                            qvar += std::string(ms.name);
                            if (auto v_sid = sym.lookup(qvar)) {
                                const auto rid = add_resolved_(out, BindingKind::kLocalVar, *v_sid, ms.span);
                                out.stmt_to_resolved[(uint32_t)msid] = rid;
                            }
                            continue;
                        }

                        walk_stmt(ast, r, msid, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, /*file_scope=*/false);
                    }
                }
                return;
            }

            case ast::StmtKind::kActorDecl: {
                const std::string qname = qualify_name_(namespace_stack, s.name);
                uint32_t actor_sym = sema::SymbolTable::kNoScope;
                if (auto sid = sym.lookup(qname)) {
                    actor_sym = *sid;
                } else {
                    auto ins = declare_(sema::SymbolKind::kType, qname, ast::k_invalid_type, s.span, sym, bag, opt);
                    actor_sym = ins.symbol_id;
                }
                if (actor_sym != sema::SymbolTable::kNoScope) {
                    const auto rid = add_resolved_(out, BindingKind::kType, actor_sym, s.span);
                    out.stmt_to_resolved[(uint32_t)id] = rid;
                }

                ScopeGuard g(sym);
                AliasScopeGuard ag(import_aliases);

                const auto& kids = ast.stmt_children();
                const uint64_t begin = s.stmt_begin;
                const uint64_t end = begin + s.stmt_count;
                if (begin <= kids.size() && end <= kids.size()) {
                    // actor member function predeclare for forward references
                    for (uint32_t i = 0; i < s.stmt_count; ++i) {
                        const ast::StmtId msid = kids[s.stmt_begin + i];
                        if (!is_valid_stmt_id_(r, msid)) continue;
                        const auto& ms = ast.stmt(msid);
                        if (ms.kind != ast::StmtKind::kFnDecl || ms.name.empty()) continue;
                        (void)declare_(sema::SymbolKind::kFn, ms.name, ms.type, ms.span, sym, bag, opt);
                    }

                    for (uint32_t i = 0; i < s.stmt_count; ++i) {
                        const ast::StmtId msid = kids[s.stmt_begin + i];
                        if (!is_valid_stmt_id_(r, msid)) continue;
                        walk_stmt(
                            ast, r, msid, sym, bag, opt, out, param_symbol_ids,
                            namespace_stack, import_aliases, known_namespace_paths,
                            /*file_scope=*/false
                        );
                    }
                }
                return;
            }

            case ast::StmtKind::kActsDecl: {
                const std::string qname = qualify_name_(namespace_stack, s.name);
                uint32_t acts_sym = sema::SymbolTable::kNoScope;
                if (auto sid = sym.lookup(qname)) {
                    acts_sym = *sid;
                } else {
                    auto ins = declare_(sema::SymbolKind::kAct, qname, ast::k_invalid_type, s.span, sym, bag, opt);
                    acts_sym = ins.symbol_id;
                }

                if (acts_sym != sema::SymbolTable::kNoScope) {
                    const auto rid = add_resolved_(out, BindingKind::kType, acts_sym, s.span);
                    out.stmt_to_resolved[(uint32_t)id] = rid;
                }

                ScopeGuard g(sym);
                AliasScopeGuard ag(import_aliases);
                const auto& kids = ast.stmt_children();
                const uint32_t begin = s.stmt_begin;
                const uint32_t end = s.stmt_begin + s.stmt_count;
                if (begin < r.stmt_children_count && end <= r.stmt_children_count) {
                    for (uint32_t i = begin; i < end; ++i) {
                        walk_stmt(ast, r, kids[i], sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, /*file_scope=*/false);
                    }
                }
                return;
            }

            case ast::StmtKind::kSwitch: {
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths);

                const uint32_t cb = s.case_begin;
                const uint32_t ce = s.case_begin + s.case_count;
                if (cb < r.switch_case_count && ce <= r.switch_case_count) {
                    const auto& cs = ast.switch_cases();
                    for (uint32_t i = cb; i < ce; ++i) {
                        ScopeGuard case_scope(sym);
                        if (cs[i].pat_kind == ast::CasePatKind::kEnumVariant &&
                            cs[i].enum_bind_count > 0) {
                            auto& binds = const_cast<ast::AstArena&>(ast).switch_enum_binds_mut();
                            const uint64_t bb = cs[i].enum_bind_begin;
                            const uint64_t be = bb + cs[i].enum_bind_count;
                            if (bb <= binds.size() && be <= binds.size()) {
                                for (uint32_t bi = cs[i].enum_bind_begin; bi < cs[i].enum_bind_begin + cs[i].enum_bind_count; ++bi) {
                                    auto& b = binds[bi];
                                    auto ins = declare_(sema::SymbolKind::kVar, b.bind_name, ast::k_invalid_type, b.span, sym, bag, opt);
                                    b.resolved_symbol = ins.symbol_id;
                                }
                            }
                        }
                        walk_stmt(ast, r, cs[i].body, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, /*file_scope=*/false);
                    }
                }
                return;
            }

            case ast::StmtKind::kUse:
                if ((s.use_kind == ast::UseKind::kImport ||
                     s.use_kind == ast::UseKind::kImportCHeader) &&
                    file_scope &&
                    s.use_path_count > 0) {
                    std::string alias = std::string(s.use_rhs_ident);
                    if (alias.empty()) {
                        const auto& segs = ast.path_segs();
                        if (s.use_path_begin + s.use_path_count <= segs.size()) {
                            const std::string_view last = segs[s.use_path_begin + s.use_path_count - 1];
                            if (!last.empty() && last.front() == '.') {
                                size_t off = 0;
                                while (off < last.size() && last[off] == '.') ++off;
                                alias = std::string(last.substr(off));
                            } else {
                                alias = std::string(last);
                            }
                        }
                    }
                    if (!alias.empty()) {
                        if (s.use_kind == ast::UseKind::kImportCHeader) {
                            import_aliases[alias] = alias;
                        } else {
                            const std::string raw_path = path_join_(ast, s.use_path_begin, s.use_path_count);
                            const std::string path = resolve_import_path_for_alias_(raw_path, opt);
                            if (!path.empty()) {
                                validate_import_dep_(bag, opt, s.span, path);
                                import_aliases[alias] = path;
                            }
                        }
                    }
                } else if (s.use_kind == ast::UseKind::kNestAlias &&
                           s.use_path_count > 0 &&
                           !s.use_rhs_ident.empty()) {
                    const std::string path = path_join_(ast, s.use_path_begin, s.use_path_count);
                    if (!path.empty()) {
                        if (!is_known_namespace_path_(path, known_namespace_paths)) {
                            report(bag, diag::Severity::kError, diag::Code::kUseNestPathExpectedNamespace, s.span, path);
                        } else {
                            import_aliases[std::string(s.use_rhs_ident)] = path;
                        }
                    }
                } else if (s.use_kind == ast::UseKind::kPathAlias &&
                           s.use_path_count > 0 &&
                           !s.use_rhs_ident.empty()) {
                    const std::string path = path_join_(ast, s.use_path_begin, s.use_path_count);
                    if (!path.empty()) {
                        import_aliases[std::string(s.use_rhs_ident)] = path;
                    }
                }
                // NOTE: use의 선언성(별칭/타입별칭 등)을 심볼로 올릴지 여부는 스펙 결정 후 확장.
                // 지금은 expr만 검사한다.
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths);
                return;

            case ast::StmtKind::kCompilerDirective:
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths);
                walk_stmt(ast, r, s.a, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, file_scope);
                return;

            case ast::StmtKind::kNestDecl:
                if (!s.nest_is_file_directive) {
                    uint32_t pushed = 0;
                    const auto& segs = ast.path_segs();
                    const uint64_t begin = s.nest_path_begin;
                    const uint64_t end = begin + s.nest_path_count;
                    if (begin <= segs.size() && end <= segs.size()) {
                        for (uint32_t i = 0; i < s.nest_path_count; ++i) {
                            namespace_stack.push_back(std::string(segs[s.nest_path_begin + i]));
                            ++pushed;
                        }
                    }

                    if (is_valid_stmt_id_(r, s.a)) {
                        AliasScopeGuard ag(import_aliases);
                        const auto& body = ast.stmt(s.a);
                        if (body.kind == ast::StmtKind::kBlock) {
                            // nest 본문은 lexical scope가 아니라 namespace declaration 영역이다.
                            // 심볼을 pop하지 않고 전역 경로 심볼 테이블에 유지한다.
                            walk_block_children_(ast, r, body, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, /*file_scope=*/false);
                        } else {
                            walk_stmt(ast, r, s.a, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, /*file_scope=*/false);
                        }
                    }

                    while (pushed > 0) {
                        namespace_stack.pop_back();
                        --pushed;
                    }
                }
                return;

            case ast::StmtKind::kInstDecl: {
                const std::string qname = qualify_name_(namespace_stack, s.name);
                uint32_t inst_sym = sema::SymbolTable::kNoScope;
                if (auto sid = sym.lookup(qname)) {
                    inst_sym = *sid;
                } else {
                    auto ins = declare_(sema::SymbolKind::kInst, qname, ast::k_invalid_type, s.span, sym, bag, opt);
                    inst_sym = ins.symbol_id;
                }
                if (inst_sym != sema::SymbolTable::kNoScope) {
                    const auto rid = add_resolved_(out, BindingKind::kType, inst_sym, s.span);
                    out.stmt_to_resolved[(uint32_t)id] = rid;
                }
                return;
            }

            default:
                return;
        }
    }

    static void init_file_context_(
        const ast::AstArena& ast,
        const IdRanges& r,
        ast::StmtId root,
        std::vector<std::string>& namespace_stack,
        std::unordered_map<std::string, std::string>& import_aliases,
        diag::Bag& bag,
        const NameResolveOptions& opt
    ) {
        if (!is_valid_stmt_id_(r, root)) return;
        const auto& root_stmt = ast.stmt(root);
        if (root_stmt.kind != ast::StmtKind::kBlock) return;

        const auto& kids = ast.stmt_children();
        const uint32_t begin = root_stmt.stmt_begin;
        const uint32_t end = root_stmt.stmt_begin + root_stmt.stmt_count;
        if (begin >= r.stmt_children_count || end > r.stmt_children_count) return;

        const auto& segs = ast.path_segs();
        bool file_namespace_set = false;

        for (uint32_t i = begin; i < end; ++i) {
            const ast::StmtId sid = kids[i];
            if (!is_valid_stmt_id_(r, sid)) continue;
            const auto& s = ast.stmt(sid);

            if (!file_namespace_set &&
                s.kind == ast::StmtKind::kNestDecl &&
                s.nest_is_file_directive &&
                s.nest_path_count > 0)
            {
                const uint64_t pbegin = s.nest_path_begin;
                const uint64_t pend = pbegin + s.nest_path_count;
                if (pbegin <= segs.size() && pend <= segs.size()) {
                    const std::string file_head = path_join_(ast, s.nest_path_begin, s.nest_path_count);
                    const bool redundant_with_module_head =
                        !opt.current_module_head.empty() &&
                        file_head == opt.current_module_head;
                    if (!redundant_with_module_head) {
                        for (uint32_t j = 0; j < s.nest_path_count; ++j) {
                            namespace_stack.push_back(std::string(segs[s.nest_path_begin + j]));
                        }
                        report(bag,
                               diag::Severity::kWarning,
                               diag::Code::kNestNotUsedForModuleResolution,
                               s.span,
                               file_head);
                    }
                    file_namespace_set = true;
                }
                continue;
            }

            if (s.kind == ast::StmtKind::kUse &&
                (s.use_kind == ast::UseKind::kImport ||
                 s.use_kind == ast::UseKind::kImportCHeader) &&
                s.use_path_count > 0)
            {
                std::string alias = std::string(s.use_rhs_ident);
                if (alias.empty()) {
                    if (s.use_path_begin + s.use_path_count <= segs.size()) {
                        const std::string_view last = segs[s.use_path_begin + s.use_path_count - 1];
                        if (!last.empty() && last.front() == '.') {
                            size_t off = 0;
                            while (off < last.size() && last[off] == '.') ++off;
                            alias = std::string(last.substr(off));
                        } else {
                            alias = std::string(last);
                        }
                    }
                }
                if (alias.empty()) continue;

                if (s.use_kind == ast::UseKind::kImportCHeader) {
                    import_aliases[alias] = alias;
                } else {
                    const std::string raw_path = path_join_(ast, s.use_path_begin, s.use_path_count);
                    const std::string path = resolve_import_path_for_alias_(raw_path, opt);
                    if (path.empty()) continue;
                    validate_import_dep_(bag, opt, s.span, path);
                    import_aliases[alias] = path;
                }
            }
        }
    }

    static void predeclare_namespace_decls_(
        const ast::AstArena& ast,
        const IdRanges& r,
        ast::StmtId sid,
        sema::SymbolTable& sym,
        diag::Bag& bag,
        const NameResolveOptions& opt,
        std::vector<std::string>& namespace_stack
    ) {
        if (!is_valid_stmt_id_(r, sid)) return;
        const auto& s = ast.stmt(sid);

        auto reify_external_placeholder_as_local = [&](std::string_view qname,
                                                       sema::SymbolKind kind,
                                                       parus::ty::TypeId declared_type,
                                                       Span decl_span,
                                                       bool is_export) -> bool {
            auto existing = sym.lookup(qname);
            if (!existing.has_value()) return false;

            auto& se = sym.symbol_mut(*existing);
            if (!se.is_external || se.kind != kind) return false;

            se.declared_type = declared_type;
            se.decl_span = decl_span;
            se.decl_file_id = decl_span.file_id;
            se.decl_bundle_name = opt.current_bundle_name;
            se.decl_module_head = opt.current_module_head;
            se.decl_source_dir_norm = opt.current_source_dir_norm;
            se.link_name.clear();
            se.external_payload.clear();
            se.is_export = is_export;
            se.is_external = false;
            return true;
        };

        if (s.kind == ast::StmtKind::kBlock) {
            const auto& kids = ast.stmt_children();
            const uint64_t begin = s.stmt_begin;
            const uint64_t end = begin + s.stmt_count;
            if (begin <= kids.size() && end <= kids.size()) {
                for (uint32_t i = 0; i < s.stmt_count; ++i) {
                    predeclare_namespace_decls_(ast, r, kids[s.stmt_begin + i], sym, bag, opt, namespace_stack);
                }
            }
            return;
        }

        if (s.kind == ast::StmtKind::kNestDecl) {
            if (!s.nest_is_file_directive) {
                uint32_t pushed = 0;
                const auto& segs = ast.path_segs();
                const uint64_t begin = s.nest_path_begin;
                const uint64_t end = begin + s.nest_path_count;
                if (begin <= segs.size() && end <= segs.size()) {
                    for (uint32_t i = 0; i < s.nest_path_count; ++i) {
                        namespace_stack.push_back(std::string(segs[s.nest_path_begin + i]));
                        ++pushed;
                    }
                }
                predeclare_namespace_decls_(ast, r, s.a, sym, bag, opt, namespace_stack);
                while (pushed > 0) {
                    namespace_stack.pop_back();
                    --pushed;
                }
            }
            return;
        }

        if (s.kind == ast::StmtKind::kFnDecl) {
            const std::string qname = qualify_name_(namespace_stack, s.name);
            if (reify_external_placeholder_as_local(
                    qname,
                    sema::SymbolKind::kFn,
                    s.type,
                    s.span,
                    s.is_export)) {
                return;
            }
            if (!sym.lookup(qname)) {
                auto ins = declare_(sema::SymbolKind::kFn, qname, s.type, s.span, sym, bag, opt);
                if (ins.ok && !ins.is_duplicate) {
                    auto& se = sym.symbol_mut(ins.symbol_id);
                    se.decl_file_id = s.span.file_id;
                    se.decl_bundle_name = opt.current_bundle_name;
                    se.decl_module_head = opt.current_module_head;
                    se.decl_source_dir_norm = opt.current_source_dir_norm;
                    se.is_export = s.is_export;
                    se.is_external = false;
                }
            }
            return;
        }

        if (s.kind == ast::StmtKind::kFieldDecl) {
            const std::string qname = qualify_name_(namespace_stack, s.name);
            if (!reify_external_placeholder_as_local(
                    qname,
                    sema::SymbolKind::kField,
                    ast::k_invalid_type,
                    s.span,
                    s.is_export) &&
                !sym.lookup(qname)) {
                auto ins = declare_(sema::SymbolKind::kField, qname, ast::k_invalid_type, s.span, sym, bag, opt);
                if (ins.ok && !ins.is_duplicate) {
                    auto& se = sym.symbol_mut(ins.symbol_id);
                    se.decl_file_id = s.span.file_id;
                    se.decl_bundle_name = opt.current_bundle_name;
                    se.decl_module_head = opt.current_module_head;
                    se.decl_source_dir_norm = opt.current_source_dir_norm;
                    se.is_export = s.is_export;
                    se.is_external = false;
                }
            }
            return;
        }

        if (s.kind == ast::StmtKind::kEnumDecl) {
            const std::string qname = qualify_name_(namespace_stack, s.name);
            if (!reify_external_placeholder_as_local(
                    qname,
                    sema::SymbolKind::kType,
                    ast::k_invalid_type,
                    s.span,
                    s.is_export) &&
                !sym.lookup(qname)) {
                auto ins = declare_(sema::SymbolKind::kType, qname, ast::k_invalid_type, s.span, sym, bag, opt);
                if (ins.ok && !ins.is_duplicate) {
                    auto& se = sym.symbol_mut(ins.symbol_id);
                    se.decl_file_id = s.span.file_id;
                    se.decl_bundle_name = opt.current_bundle_name;
                    se.decl_module_head = opt.current_module_head;
                    se.decl_source_dir_norm = opt.current_source_dir_norm;
                    se.is_export = s.is_export;
                    se.is_external = false;
                }
            }
            return;
        }

        if (s.kind == ast::StmtKind::kProtoDecl) {
            const std::string qname = qualify_name_(namespace_stack, s.name);
            if (!reify_external_placeholder_as_local(
                    qname,
                    sema::SymbolKind::kType,
                    ast::k_invalid_type,
                    s.span,
                    s.is_export) &&
                !sym.lookup(qname)) {
                auto ins = declare_(sema::SymbolKind::kType, qname, ast::k_invalid_type, s.span, sym, bag, opt);
                if (ins.ok && !ins.is_duplicate) {
                    auto& se = sym.symbol_mut(ins.symbol_id);
                    se.decl_file_id = s.span.file_id;
                    se.decl_bundle_name = opt.current_bundle_name;
                    se.decl_module_head = opt.current_module_head;
                    se.decl_source_dir_norm = opt.current_source_dir_norm;
                    se.is_export = s.is_export;
                    se.is_external = false;
                }
            }

            const auto& kids = ast.stmt_children();
            const uint64_t begin = s.stmt_begin;
            const uint64_t end = begin + s.stmt_count;
            if (begin <= kids.size() && end <= kids.size()) {
                for (uint32_t i = 0; i < s.stmt_count; ++i) {
                    const ast::StmtId msid = kids[s.stmt_begin + i];
                    if (!is_valid_stmt_id_(r, msid)) continue;
                    const auto& ms = ast.stmt(msid);
                    if (ms.kind != ast::StmtKind::kFnDecl) continue;
                    if (ms.a == ast::k_invalid_stmt) continue; // signature-only proto member is not callable path symbol

                    std::string mqname = qname;
                    if (!mqname.empty()) mqname += "::";
                    mqname += std::string(ms.name);
                    if (!sym.lookup(mqname)) {
                        auto ins = declare_(sema::SymbolKind::kFn, mqname, ms.type, ms.span, sym, bag, opt);
                        if (ins.ok && !ins.is_duplicate) {
                            auto& se = sym.symbol_mut(ins.symbol_id);
                            se.decl_file_id = ms.span.file_id;
                            se.decl_bundle_name = opt.current_bundle_name;
                            se.decl_module_head = opt.current_module_head;
                            se.decl_source_dir_norm = opt.current_source_dir_norm;
                            se.is_export = s.is_export;
                            se.is_external = false;
                        }
                    }
                }
            }
            return;
        }

        if (s.kind == ast::StmtKind::kClassDecl) {
            const std::string qname = qualify_name_(namespace_stack, s.name);
            if (!reify_external_placeholder_as_local(
                    qname,
                    sema::SymbolKind::kType,
                    ast::k_invalid_type,
                    s.span,
                    s.is_export) &&
                !sym.lookup(qname)) {
                auto ins = declare_(sema::SymbolKind::kType, qname, ast::k_invalid_type, s.span, sym, bag, opt);
                if (ins.ok && !ins.is_duplicate) {
                    auto& se = sym.symbol_mut(ins.symbol_id);
                    se.decl_file_id = s.span.file_id;
                    se.decl_bundle_name = opt.current_bundle_name;
                    se.decl_module_head = opt.current_module_head;
                    se.decl_source_dir_norm = opt.current_source_dir_norm;
                    se.is_export = s.is_export;
                    se.is_external = false;
                }
            }

            const auto& kids = ast.stmt_children();
            const uint64_t begin = s.stmt_begin;
            const uint64_t end = begin + s.stmt_count;
            if (begin <= kids.size() && end <= kids.size()) {
                for (uint32_t i = 0; i < s.stmt_count; ++i) {
                    const ast::StmtId msid = kids[s.stmt_begin + i];
                    if (!is_valid_stmt_id_(r, msid)) continue;
                    const auto& ms = ast.stmt(msid);
                    if (ms.kind == ast::StmtKind::kFnDecl) {
                        std::string mqname = qname;
                        if (!mqname.empty()) mqname += "::";
                        mqname += std::string(ms.name);
                        if (!sym.lookup(mqname)) {
                            auto ins = declare_(sema::SymbolKind::kFn, mqname, ms.type, ms.span, sym, bag, opt);
                            if (ins.ok && !ins.is_duplicate) {
                                auto& se = sym.symbol_mut(ins.symbol_id);
                                se.decl_file_id = ms.span.file_id;
                                se.decl_bundle_name = opt.current_bundle_name;
                                se.decl_module_head = opt.current_module_head;
                                se.decl_source_dir_norm = opt.current_source_dir_norm;
                                se.is_export = s.is_export;
                                se.is_external = false;
                            }
                        }
                        continue;
                    }

                    if (ms.kind == ast::StmtKind::kVar && ms.is_static) {
                        std::string mqname = qname;
                        if (!mqname.empty()) mqname += "::";
                        mqname += std::string(ms.name);
                        if (!sym.lookup(mqname)) {
                            auto ins = declare_(sema::SymbolKind::kVar, mqname, ms.type, ms.span, sym, bag, opt);
                            if (ins.ok && !ins.is_duplicate) {
                                auto& se = sym.symbol_mut(ins.symbol_id);
                                se.decl_file_id = ms.span.file_id;
                                se.decl_bundle_name = opt.current_bundle_name;
                                se.decl_module_head = opt.current_module_head;
                                se.decl_source_dir_norm = opt.current_source_dir_norm;
                                se.is_export = s.is_export;
                                se.is_external = false;
                            }
                        }
                    }
                }
            }
            return;
        }

        if (s.kind == ast::StmtKind::kActorDecl) {
            const std::string qname = qualify_name_(namespace_stack, s.name);
            if (!reify_external_placeholder_as_local(
                    qname,
                    sema::SymbolKind::kType,
                    ast::k_invalid_type,
                    s.span,
                    s.is_export) &&
                !sym.lookup(qname)) {
                auto ins = declare_(sema::SymbolKind::kType, qname, ast::k_invalid_type, s.span, sym, bag, opt);
                if (ins.ok && !ins.is_duplicate) {
                    auto& se = sym.symbol_mut(ins.symbol_id);
                    se.decl_file_id = s.span.file_id;
                    se.decl_bundle_name = opt.current_bundle_name;
                    se.decl_module_head = opt.current_module_head;
                    se.decl_source_dir_norm = opt.current_source_dir_norm;
                    se.is_export = s.is_export;
                    se.is_external = false;
                }
            }

            const auto& kids = ast.stmt_children();
            const uint64_t begin = s.stmt_begin;
            const uint64_t end = begin + s.stmt_count;
            if (begin <= kids.size() && end <= kids.size()) {
                for (uint32_t i = 0; i < s.stmt_count; ++i) {
                    const ast::StmtId msid = kids[s.stmt_begin + i];
                    if (!is_valid_stmt_id_(r, msid)) continue;
                    const auto& ms = ast.stmt(msid);
                    if (ms.kind != ast::StmtKind::kFnDecl) continue;
                    std::string mqname = qname;
                    if (!mqname.empty()) mqname += "::";
                    mqname += std::string(ms.name);
                    if (!sym.lookup(mqname)) {
                        auto ins = declare_(sema::SymbolKind::kFn, mqname, ms.type, ms.span, sym, bag, opt);
                        if (ins.ok && !ins.is_duplicate) {
                            auto& se = sym.symbol_mut(ins.symbol_id);
                            se.decl_file_id = ms.span.file_id;
                            se.decl_bundle_name = opt.current_bundle_name;
                            se.decl_module_head = opt.current_module_head;
                            se.decl_source_dir_norm = opt.current_source_dir_norm;
                            se.is_export = s.is_export;
                            se.is_external = false;
                        }
                    }
                }
            }
            return;
        }

        if (s.kind == ast::StmtKind::kVar) {
            const bool is_global_decl =
                s.is_static || s.is_const || s.is_extern || s.is_export || (s.link_abi == ast::LinkAbi::kC);
            if (!is_global_decl) return;

            const std::string qname = qualify_name_(namespace_stack, s.name);
            if (!reify_external_placeholder_as_local(
                    qname,
                    sema::SymbolKind::kVar,
                    s.type,
                    s.span,
                    s.is_export) &&
                !sym.lookup(qname)) {
                auto ins = declare_(sema::SymbolKind::kVar, qname, s.type, s.span, sym, bag, opt);
                if (ins.ok && !ins.is_duplicate) {
                    auto& se = sym.symbol_mut(ins.symbol_id);
                    se.decl_file_id = s.span.file_id;
                    se.decl_bundle_name = opt.current_bundle_name;
                    se.decl_module_head = opt.current_module_head;
                    se.decl_source_dir_norm = opt.current_source_dir_norm;
                    se.is_export = s.is_export;
                    se.is_external = false;
                }
            }
            return;
        }

        if (s.kind == ast::StmtKind::kActsDecl) {
            const std::string qname = qualify_name_(namespace_stack, s.name);
            if (!reify_external_placeholder_as_local(
                    qname,
                    sema::SymbolKind::kAct,
                    ast::k_invalid_type,
                    s.span,
                    s.is_export) &&
                !sym.lookup(qname)) {
                auto ins = declare_(sema::SymbolKind::kAct, qname, ast::k_invalid_type, s.span, sym, bag, opt);
                if (ins.ok && !ins.is_duplicate) {
                    auto& se = sym.symbol_mut(ins.symbol_id);
                    se.decl_file_id = s.span.file_id;
                    se.decl_bundle_name = opt.current_bundle_name;
                    se.decl_module_head = opt.current_module_head;
                    se.decl_source_dir_norm = opt.current_source_dir_norm;
                    se.is_export = s.is_export;
                    se.is_external = false;
                }
            }

            const auto& kids = ast.stmt_children();
            const uint64_t begin = s.stmt_begin;
            const uint64_t end = begin + s.stmt_count;
            if (begin <= kids.size() && end <= kids.size()) {
                for (uint32_t i = 0; i < s.stmt_count; ++i) {
                    const ast::StmtId msid = kids[s.stmt_begin + i];
                    if (!is_valid_stmt_id_(r, msid)) continue;
                    const auto& ms = ast.stmt(msid);
                    if (ms.kind != ast::StmtKind::kFnDecl) continue;

                    // 2-lane acts model:
                    // - acts Name {}: namespace function set -> expose Name::member
                    // - acts for T / acts Name for T: method/operator lane -> not exposed as plain path symbol
                    if (s.acts_is_for) continue;

                    std::string mqname = qname;
                    if (!mqname.empty()) mqname += "::";
                    mqname += std::string(ms.name);
                    if (!sym.lookup(mqname)) {
                        auto ins = declare_(sema::SymbolKind::kFn, mqname, ms.type, ms.span, sym, bag, opt);
                        if (ins.ok && !ins.is_duplicate) {
                            auto& se = sym.symbol_mut(ins.symbol_id);
                            se.decl_file_id = ms.span.file_id;
                            se.decl_bundle_name = opt.current_bundle_name;
                            se.decl_module_head = opt.current_module_head;
                            se.decl_source_dir_norm = opt.current_source_dir_norm;
                            se.is_export = s.is_export;
                            se.is_external = false;
                        }
                    }
                }
            }
            return;
        }

        if (s.kind == ast::StmtKind::kInstDecl) {
            const std::string qname = qualify_name_(namespace_stack, s.name);
            if (!sym.lookup(qname)) {
                auto ins = declare_(sema::SymbolKind::kInst, qname, ast::k_invalid_type, s.span, sym, bag, opt);
                if (ins.ok && !ins.is_duplicate) {
                    auto& se = sym.symbol_mut(ins.symbol_id);
                    se.decl_file_id = s.span.file_id;
                    se.decl_bundle_name = opt.current_bundle_name;
                    se.decl_module_head = opt.current_module_head;
                    se.decl_source_dir_norm = opt.current_source_dir_norm;
                    se.is_export = s.is_export;
                    se.is_external = false;
                }
            }
            return;
        }
    }

    static void register_external_exports_(
        sema::SymbolTable& sym,
        diag::Bag& bag,
        const NameResolveOptions& opt
    ) {
        auto is_c_abi_external_payload = [](std::string_view payload) -> bool {
            return payload.starts_with("parus_c_import|") ||
                   payload.starts_with("parus_c_abi_decl|");
        };
        auto has_throwing_metadata = [](std::string_view payload) -> bool {
            size_t pos = 0;
            while (pos < payload.size()) {
                size_t next = payload.find('|', pos);
                if (next == std::string_view::npos) next = payload.size();
                const std::string_view part = payload.substr(pos, next - pos);
                if (part == "throwing=1") return true;
                if (next == payload.size()) break;
                pos = next + 1;
            }
            return false;
        };
        auto strip_throwing_metadata = [](std::string_view payload) -> std::string {
            std::string out{};
            size_t pos = 0;
            bool first = true;
            while (pos < payload.size()) {
                size_t next = payload.find('|', pos);
                if (next == std::string_view::npos) next = payload.size();
                const std::string_view part = payload.substr(pos, next - pos);
                if (!part.empty() &&
                    !(part == "throwing=1" || part.starts_with("throwing="))) {
                    if (!first) out.push_back('|');
                    out.append(part.data(), part.size());
                    first = false;
                }
                if (next == payload.size()) break;
                pos = next + 1;
            }
            return out;
        };

        auto hidden_external_overload_name = [&](std::string_view visible_name) -> std::string {
            std::string hidden(visible_name);
            hidden += "@@extovl$";
            hidden += std::to_string(sym.symbols().size());
            return hidden;
        };

        auto handle_external_export = [&](const NameResolveOptions::ExternalExport& ex) {
            if (ex.path.empty()) return;

            const auto names = candidate_names_for_external_export_(ex, opt.current_module_head);
            for (const auto& nm : names) {
                if (nm.empty()) continue;
                std::string inst_payload = ex.inst_payload;
                if (is_c_abi_external_payload(inst_payload) &&
                    has_throwing_metadata(inst_payload)) {
                    diag::Diagnostic d(
                        diag::Severity::kError,
                        diag::Code::kAbiCExternalThrowingMetadataInvalid,
                        ex.decl_span
                    );
                    d.add_arg(nm);
                    d.add_note("C ABI and cimport symbols are always non-throwing in the core exception model");
                    d.add_help("remove 'throwing=1' from the imported metadata, or bridge through a Parus wrapper");
                    bag.add(std::move(d));
                    inst_payload = strip_throwing_metadata(inst_payload);
                }

                if (auto existing = sym.lookup(nm)) {
                    const auto& old = sym.symbol(*existing);
                    if (ex.kind == sema::SymbolKind::kField &&
                        old.kind == sema::SymbolKind::kType &&
                        old.is_external) {
                        auto& cur = sym.symbol_mut(*existing);
                        if (cur.external_field_payload.empty() && !ex.inst_payload.empty()) {
                            cur.external_field_payload = ex.inst_payload;
                        }
                        continue;
                    }
                    if (old.kind != ex.kind) {
                        report(bag, diag::Severity::kError, diag::Code::kDuplicateDecl, ex.decl_span, nm);
                    } else if (ex.kind == sema::SymbolKind::kFn &&
                               old.is_external &&
                               !is_c_abi_external_payload(old.external_payload) &&
                               !is_c_abi_external_payload(inst_payload)) {
                        const std::string hidden_name = hidden_external_overload_name(nm);
                        auto ins = declare_(
                            ex.kind,
                            hidden_name,
                            ex.declared_type,
                            ex.decl_span,
                            sym,
                            bag,
                            opt
                        );
                        if (!ins.ok || ins.is_duplicate) {
                            continue;
                        }

                        auto& se = sym.symbol_mut(ins.symbol_id);
                        se.decl_file_id = ex.decl_span.file_id;
                        se.decl_bundle_name = ex.decl_bundle_name;
                        se.decl_module_head = ex.module_head;
                        se.decl_source_dir_norm = ex.decl_source_dir_norm;
                        se.link_name = ex.link_name;
                        se.external_payload = inst_payload;
                        se.is_export = ex.is_export;
                        se.is_external = true;
                    } else {
                        auto& cur = sym.symbol_mut(*existing);
                        if (ex.kind == sema::SymbolKind::kFn && !ex.link_name.empty() && cur.link_name.empty()) {
                            cur.link_name = ex.link_name;
                        }
                        if (cur.external_payload.empty() && !inst_payload.empty()) {
                            cur.external_payload = inst_payload;
                        }
                    }
                    continue;
                }

                auto ins = declare_(
                    ex.kind,
                    nm,
                    ex.declared_type,
                    ex.decl_span,
                    sym,
                    bag,
                    opt
                );
                if (!ins.ok || ins.is_duplicate) continue;

                auto& se = sym.symbol_mut(ins.symbol_id);
                se.decl_file_id = ex.decl_span.file_id;
                se.decl_bundle_name = ex.decl_bundle_name;
                se.decl_module_head = ex.module_head;
                se.decl_source_dir_norm = ex.decl_source_dir_norm;
                se.link_name = ex.link_name;
                se.external_payload = inst_payload;
                se.is_export = ex.is_export;
                se.is_external = true;
            }
        };

        for (const auto& ex : opt.external_exports) {
            if (ex.kind == sema::SymbolKind::kField) continue;
            handle_external_export(ex);
        }
        for (const auto& ex : opt.external_exports) {
            if (ex.kind != sema::SymbolKind::kField) continue;
            handle_external_export(ex);
        }
    }

    // -----------------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------------
