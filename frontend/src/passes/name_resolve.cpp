// frontend/src/passes/name_resolve.cpp
#include <parus/passes/NameResolve.hpp>
#include <parus/diag/DiagCode.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/syntax/TokenKind.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include <unordered_map>
#include <unordered_set>


namespace parus::passes {

    // -----------------------------------------------------------------------------
    // Diagnostics helpers
    // -----------------------------------------------------------------------------
    static void report(diag::Bag& bag, diag::Severity sev, diag::Code code, Span span, std::string_view a0 = {}) {
        diag::Diagnostic d(sev, code, span);
        if (!a0.empty()) d.add_arg(a0);
        bag.add(std::move(d));
    }

    // -----------------------------------------------------------------------------
    // Core invariants (v0 parser quirks)
    //
    // 1) BlockExpr stores a *StmtId* inside Expr::a (casted to ExprId slot).
    //    - parse_expr_block(): out.kind = kBlockExpr
    //      out.a = (ExprId)block_stmt_id
    //      out.b = tail_expr_id (or invalid)
    //
    // 2) Loop expr stores:
    //    - loop_iter : ExprId
    //    - loop_body : StmtId
    //
    // This pass MUST treat these fields with the correct id-space, otherwise
    // "UndefinedName: x" false positives can happen due to ExprId/StmtId index aliasing.
    // -----------------------------------------------------------------------------

    // -----------------------------------------------------------------------------
    // Id validation helpers
    // -----------------------------------------------------------------------------
    struct IdRanges {
        uint32_t expr_count = 0;
        uint32_t stmt_count = 0;
        uint32_t arg_count = 0;
        uint32_t ng_count = 0;
        uint32_t stmt_children_count = 0;
        uint32_t param_count = 0;
        uint32_t switch_case_count = 0;
    };

    static IdRanges ranges_of_(const ast::AstArena& ast) {
        IdRanges r{};
        r.expr_count = (uint32_t)ast.exprs().size();
        r.stmt_count = (uint32_t)ast.stmts().size();
        r.arg_count = (uint32_t)ast.args().size();
        r.ng_count = (uint32_t)ast.named_group_args().size();
        r.stmt_children_count = (uint32_t)ast.stmt_children().size();
        r.param_count = (uint32_t)ast.params().size();
        r.switch_case_count = (uint32_t)ast.switch_cases().size();
        return r;
    }

    static bool is_valid_expr_id_(const IdRanges& r, ast::ExprId id) {
        return id != ast::k_invalid_expr && (uint32_t)id < r.expr_count;
    }

    static bool is_valid_stmt_id_(const IdRanges& r, ast::StmtId id) {
        return id != ast::k_invalid_stmt && (uint32_t)id < r.stmt_count;
    }

    static bool is_valid_param_index_(const IdRanges& r, uint32_t pid) {
        return pid < r.param_count;
    }

    static void ensure_result_sized_(const IdRanges& r, NameResolveResult& out) {
        if (out.expr_to_resolved.size() != r.expr_count ||
            out.stmt_to_resolved.size() != r.stmt_count ||
            out.param_to_resolved.size() != r.param_count)
        {
            out.reset_sizes(r.expr_count, r.stmt_count, r.param_count);
        }
    }

    // -----------------------------------------------------------------------------
    // Scope guard
    // -----------------------------------------------------------------------------
    struct ScopeGuard {
        sema::SymbolTable& sym;
        bool active = false;
        explicit ScopeGuard(sema::SymbolTable& s) : sym(s), active(true) { sym.push_scope(); }
        ~ScopeGuard() { if (active) sym.pop_scope(); }
        ScopeGuard(const ScopeGuard&) = delete;
        ScopeGuard& operator=(const ScopeGuard&) = delete;
    };

    // -----------------------------------------------------------------------------
    // ResolvedSymbol table helpers
    // -----------------------------------------------------------------------------
    static NameResolveResult::ResolvedId add_resolved_(
        NameResolveResult& out,
        BindingKind bind,
        uint32_t sym_id,
        Span span
    ) {
        ResolvedSymbol rs{};
        rs.bind = bind;
        rs.sym = sym_id;
        rs.span = span;

        out.resolved.push_back(rs);
        return (NameResolveResult::ResolvedId)(out.resolved.size() - 1);
    }

    // forward
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
        bool file_scope
    );

    // -----------------------------------------------------------------------------
    // Shadowing/duplicate diagnostics + insert wrapper
    // -----------------------------------------------------------------------------
    static sema::SymbolTable::InsertResult declare_(
        sema::SymbolKind kind,
        std::string_view name,
        parus::ty::TypeId ty,
        Span span,
        sema::SymbolTable& sym,
        diag::Bag& bag,
        const NameResolveOptions& opt
    ) {
        auto ins = sym.insert(kind, name, ty, span);

        // 함수는 오버로딩을 허용한다.
        // SymbolTable은 이름당 1개 엔트리만 보관하므로,
        // 같은 스코프에 같은 이름의 함수가 이미 있으면 기존 심볼을 재사용한다.
        if (ins.is_duplicate && kind == sema::SymbolKind::kFn) {
            const auto& dup = sym.symbol(ins.symbol_id);
            if (dup.kind == sema::SymbolKind::kFn) {
                sema::SymbolTable::InsertResult ok{};
                ok.ok = true;
                ok.symbol_id = ins.symbol_id;
                return ok;
            }
        }

        if (ins.is_duplicate) {
            report(bag, diag::Severity::kError, diag::Code::kDuplicateDecl, span, name);
            return ins;
        }

        if (ins.is_shadowing) {
            switch (opt.shadowing) {
                case ShadowingMode::kAllow:
                    break;
                case ShadowingMode::kWarn:
                    report(bag, diag::Severity::kWarning, diag::Code::kShadowing, span, name);
                    break;
                case ShadowingMode::kError:
                    report(bag, diag::Severity::kError, diag::Code::kShadowingNotAllowed, span, name);
                    break;
            }
        }
        return ins;
    }

    static std::string path_join_(const ast::AstArena& ast, uint32_t begin, uint32_t count) {
        if (count == 0) return {};
        const auto& segs = ast.path_segs();
        if (begin >= segs.size() || begin + count > segs.size()) return {};

        std::string out;
        for (uint32_t i = 0; i < count; ++i) {
            if (i) out += "::";
            out += std::string(segs[begin + i]);
        }
        return out;
    }

    static std::string qualify_name_(const std::vector<std::string>& namespace_stack, std::string_view base) {
        if (base.empty()) return {};
        if (namespace_stack.empty()) return std::string(base);

        std::string out;
        for (size_t i = 0; i < namespace_stack.size(); ++i) {
            if (i) out += "::";
            out += namespace_stack[i];
        }
        out += "::";
        out += std::string(base);
        return out;
    }

    static std::optional<std::string> rewrite_imported_path_(
        std::string_view name,
        const std::unordered_map<std::string, std::string>& import_aliases
    ) {
        if (name.empty()) return std::nullopt;

        const size_t pos = name.find("::");
        if (pos == std::string_view::npos) {
            auto it = import_aliases.find(std::string(name));
            if (it == import_aliases.end()) return std::nullopt;
            return it->second;
        }

        std::string head(name.substr(0, pos));
        auto it = import_aliases.find(head);
        if (it == import_aliases.end()) return std::nullopt;

        std::string out = it->second;
        out += name.substr(pos);
        return out;
    }

    static std::optional<uint32_t> lookup_symbol_(
        sema::SymbolTable& sym,
        std::string_view raw_name,
        const std::vector<std::string>& namespace_stack,
        const std::unordered_map<std::string, std::string>& import_aliases
    ) {
        if (raw_name.empty()) return std::nullopt;

        std::string name(raw_name);
        if (auto rewritten = rewrite_imported_path_(name, import_aliases)) {
            name = *rewritten;
        }

        if (auto sid = sym.lookup(name)) {
            return sid;
        }

        if (name.find("::") != std::string::npos) {
            return std::nullopt;
        }

        for (size_t depth = namespace_stack.size(); depth > 0; --depth) {
            std::string q;
            for (size_t i = 0; i < depth; ++i) {
                if (i) q += "::";
                q += namespace_stack[i];
            }
            q += "::";
            q += name;
            if (auto sid = sym.lookup(q)) {
                return sid;
            }
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
        std::unordered_map<std::string, std::string>& import_aliases
    ) {
        (void)opt;
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

                    auto sid = lookup_symbol_(sym, e.text, namespace_stack, import_aliases);
                    if (!sid) {
                        report(bag, diag::Severity::kError, diag::Code::kUndefinedName, e.span, e.text);
                    } else {
                        // BindingKind 결정:
                        // - v0에서는 "param인지"가 중요하므로, pass 내부에서 param symbol id set을 유지한다.
                        BindingKind bk = BindingKind::kLocalVar;
                        if (param_symbol_ids.find(*sid) != param_symbol_ids.end()) {
                            bk = BindingKind::kParam;
                        } else {
                            // fallback by SymbolKind (확장 대비)
                            const auto& symobj = sym.symbol(*sid);
                            if (symobj.kind == sema::SymbolKind::kFn) bk = BindingKind::kFn;
                            else if (symobj.kind == sema::SymbolKind::kType) bk = BindingKind::kType;
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
                    if (e.kind == ast::ExprKind::kBinary && e.op == syntax::TokenKind::kDot) {
                        // value member access는 rhs 식별자를 "심볼 이름 해소" 대상으로 보지 않는다.
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
                        const auto& ng   = ast.named_group_args();

                        for (uint32_t i = 0; i < e.arg_count; ++i) {
                            const auto& a = args[e.arg_begin + i];

                            if (a.kind == ast::ArgKind::kNamedGroup) {
                                const uint32_t ng_end = a.child_begin + a.child_count;
                                if (a.child_begin < r.ng_count && ng_end <= r.ng_count) {
                                    for (uint32_t j = 0; j < a.child_count; ++j) {
                                        const auto& entry = ng[a.child_begin + j];
                                        if (!entry.is_hole && is_valid_expr_id_(r, entry.expr)) {
                                            stack.push_back(entry.expr);
                                        }
                                    }
                                }
                                continue;
                            }

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
                        walk_expr(ast, r, e.loop_iter, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases);
                    }

                    if (e.loop_has_header && !e.loop_var.empty()) {
                        (void)declare_(
                            sema::SymbolKind::kVar,
                            e.loop_var,
                            ast::k_invalid_type,
                            e.span,
                            sym, bag, opt
                        );
                    }

                    // IMPORTANT: loop body is StmtId.
                    if (is_valid_stmt_id_(r, e.loop_body)) {
                        walk_stmt(ast, r, e.loop_body, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, /*file_scope=*/false);
                    }
                    break;
                }

                case ast::ExprKind::kIfExpr: {
                    if (is_valid_expr_id_(r, e.a)) stack.push_back(e.a);

                    // then/else: normally ExprId, but old trees might contain StmtId.
                    if (is_valid_expr_id_(r, e.b)) {
                        stack.push_back(e.b);
                    } else {
                        const ast::StmtId sb = (ast::StmtId)e.b;
                        if (is_valid_stmt_id_(r, sb)) walk_stmt(ast, r, sb, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, /*file_scope=*/false);
                    }

                    if (is_valid_expr_id_(r, e.c)) {
                        stack.push_back(e.c);
                    } else {
                        const ast::StmtId sc = (ast::StmtId)e.c;
                        if (is_valid_stmt_id_(r, sc)) walk_stmt(ast, r, sc, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, /*file_scope=*/false);
                    }
                    break;
                }

                case ast::ExprKind::kBlockExpr: {
                    // IMPORTANT (current parser):
                    // - e.a : StmtId (block stmt), stored in ExprId slot by convention
                    // - e.b : tail ExprId (or invalid)
                    const ast::StmtId blk = (ast::StmtId)e.a;
                    if (is_valid_stmt_id_(r, blk)) {
                        walk_stmt(ast, r, blk, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, /*file_scope=*/false);
                    }
                    if (is_valid_expr_id_(r, e.b)) stack.push_back(e.b);
                    if (is_valid_expr_id_(r, e.c)) stack.push_back(e.c);
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
        bool file_scope
    ) {
        const auto& kids = ast.stmt_children();
        const uint32_t begin = s.stmt_begin;
        const uint32_t end   = s.stmt_begin + s.stmt_count;

        // 방어: 깨진 AST에서도 out-of-range 방지
        if (begin >= r.stmt_children_count) return;
        if (end > r.stmt_children_count) return;

        for (uint32_t i = begin; i < end; ++i) {
            walk_stmt(ast, r, kids[i], sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, file_scope);
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
        bool file_scope
    ) {
        if (!is_valid_stmt_id_(r, id)) return;
        ensure_result_sized_(r, out);

        const auto& s = ast.stmt(id);

        switch (s.kind) {
            case ast::StmtKind::kEmpty:
                return;

            case ast::StmtKind::kExprStmt:
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases);
                return;

            case ast::StmtKind::kVar: {
                // init 먼저
                if (s.init != ast::k_invalid_expr) {
                    walk_expr(ast, r, s.init, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases);
                }

                auto ins = declare_(sema::SymbolKind::kVar, s.name, s.type, s.span, sym, bag, opt);

                // decl stmt 기록(중복이어도 id는 남기지 않는 게 안전)
                if (!ins.is_duplicate) {
                    const auto rid = add_resolved_(out, BindingKind::kLocalVar, ins.symbol_id, s.span);
                    out.stmt_to_resolved[(uint32_t)id] = rid;
                }
                return;
            }

            case ast::StmtKind::kBlock: {
                ScopeGuard g(sym);
                walk_block_children_(ast, r, s, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, file_scope);
                return;
            }

            case ast::StmtKind::kIf:
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases);
                walk_stmt(ast, r, s.a, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, /*file_scope=*/false);
                walk_stmt(ast, r, s.b, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, /*file_scope=*/false);
                return;

            case ast::StmtKind::kWhile:
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases);
                walk_stmt(ast, r, s.a, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, /*file_scope=*/false);
                return;
            case ast::StmtKind::kDoScope:
                walk_stmt(ast, r, s.a, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, /*file_scope=*/false);
                return;
            case ast::StmtKind::kDoWhile:
                walk_stmt(ast, r, s.a, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, /*file_scope=*/false);
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases);
                return;
            case ast::StmtKind::kManual:
                walk_stmt(ast, r, s.a, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, /*file_scope=*/false);
                return;

            case ast::StmtKind::kReturn:
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases);
                return;

            case ast::StmtKind::kBreak:
            case ast::StmtKind::kContinue:
                return;

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
                            walk_expr(ast, r, p.default_expr, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases);
                        }
                    }
                }

                // 4) body
                walk_stmt(ast, r, s.a, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, /*file_scope=*/false);
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
                const auto& kids = ast.stmt_children();
                const uint32_t begin = s.stmt_begin;
                const uint32_t end = s.stmt_begin + s.stmt_count;
                if (begin < r.stmt_children_count && end <= r.stmt_children_count) {
                    for (uint32_t i = begin; i < end; ++i) {
                        walk_stmt(ast, r, kids[i], sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, /*file_scope=*/false);
                    }
                }
                return;
            }

            case ast::StmtKind::kSwitch: {
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases);

                const uint32_t cb = s.case_begin;
                const uint32_t ce = s.case_begin + s.case_count;
                if (cb < r.switch_case_count && ce <= r.switch_case_count) {
                    const auto& cs = ast.switch_cases();
                    for (uint32_t i = cb; i < ce; ++i) {
                        walk_stmt(ast, r, cs[i].body, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, /*file_scope=*/false);
                    }
                }
                return;
            }

            case ast::StmtKind::kUse:
                if (s.use_kind == ast::UseKind::kImport && file_scope && s.use_path_count > 0) {
                    const std::string path = path_join_(ast, s.use_path_begin, s.use_path_count);
                    if (!path.empty()) {
                        std::string alias = std::string(s.use_rhs_ident);
                        if (alias.empty()) {
                            const auto& segs = ast.path_segs();
                            if (s.use_path_begin + s.use_path_count <= segs.size()) {
                                alias = std::string(segs[s.use_path_begin + s.use_path_count - 1]);
                            }
                        }
                        if (!alias.empty()) {
                            import_aliases[alias] = path;
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
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases);
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
                        const auto& body = ast.stmt(s.a);
                        if (body.kind == ast::StmtKind::kBlock) {
                            // nest 본문은 lexical scope가 아니라 namespace declaration 영역이다.
                            // 심볼을 pop하지 않고 전역 경로 심볼 테이블에 유지한다.
                            walk_block_children_(ast, r, body, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, /*file_scope=*/false);
                        } else {
                            walk_stmt(ast, r, s.a, sym, bag, opt, out, param_symbol_ids, namespace_stack, import_aliases, /*file_scope=*/false);
                        }
                    }

                    while (pushed > 0) {
                        namespace_stack.pop_back();
                        --pushed;
                    }
                }
                return;

            default:
                return;
        }
    }

    static void init_file_context_(
        const ast::AstArena& ast,
        const IdRanges& r,
        ast::StmtId root,
        std::vector<std::string>& namespace_stack,
        std::unordered_map<std::string, std::string>& import_aliases
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
                    for (uint32_t j = 0; j < s.nest_path_count; ++j) {
                        namespace_stack.push_back(std::string(segs[s.nest_path_begin + j]));
                    }
                    file_namespace_set = true;
                }
                continue;
            }

            if (s.kind == ast::StmtKind::kUse &&
                s.use_kind == ast::UseKind::kImport &&
                s.use_path_count > 0)
            {
                const std::string path = path_join_(ast, s.use_path_begin, s.use_path_count);
                if (path.empty()) continue;

                std::string alias = std::string(s.use_rhs_ident);
                if (alias.empty()) {
                    if (s.use_path_begin + s.use_path_count <= segs.size()) {
                        alias = std::string(segs[s.use_path_begin + s.use_path_count - 1]);
                    }
                }
                if (!alias.empty()) {
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
            if (!sym.lookup(qname)) {
                (void)declare_(sema::SymbolKind::kFn, qname, s.type, s.span, sym, bag, opt);
            }
            return;
        }

        if (s.kind == ast::StmtKind::kFieldDecl) {
            const std::string qname = qualify_name_(namespace_stack, s.name);
            if (!sym.lookup(qname)) {
                (void)declare_(sema::SymbolKind::kField, qname, ast::k_invalid_type, s.span, sym, bag, opt);
            }
            return;
        }

        if (s.kind == ast::StmtKind::kActsDecl) {
            const std::string qname = qualify_name_(namespace_stack, s.name);
            if (!sym.lookup(qname)) {
                (void)declare_(sema::SymbolKind::kAct, qname, ast::k_invalid_type, s.span, sym, bag, opt);
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
                        (void)declare_(sema::SymbolKind::kFn, mqname, ms.type, ms.span, sym, bag, opt);
                    }
                }
            }
            return;
        }
    }

    // -----------------------------------------------------------------------------
    // Public API
    // -----------------------------------------------------------------------------
    void name_resolve_stmt_tree(
        const ast::AstArena& ast,
        ast::StmtId root,
        sema::SymbolTable& sym,
        diag::Bag& bag,
        const NameResolveOptions& opt,
        NameResolveResult& out_result
    ) {
        const IdRanges r = ranges_of_(ast);
        out_result.reset_sizes(r.expr_count, r.stmt_count, r.param_count);

        // v0: param id 분류를 위해 pass 내부 상태로 "param symbol ids"를 유지한다.
        std::unordered_set<uint32_t> param_symbol_ids;
        param_symbol_ids.reserve(64);

        std::vector<std::string> namespace_stack;
        namespace_stack.reserve(8);
        std::unordered_map<std::string, std::string> import_aliases;
        import_aliases.reserve(32);
        init_file_context_(ast, r, root, namespace_stack, import_aliases);

        std::vector<std::string> predeclare_ns = namespace_stack;
        predeclare_namespace_decls_(ast, r, root, sym, bag, opt, predeclare_ns);

        walk_stmt(
            ast, r, root, sym, bag, opt, out_result,
            param_symbol_ids, namespace_stack, import_aliases, /*file_scope=*/true
        );
    }

} // namespace parus::passes
