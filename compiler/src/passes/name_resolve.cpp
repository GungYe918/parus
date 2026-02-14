// compiler/src/passes/name_resolve.cpp
#include <gaupel/passes/NameResolve.hpp>
#include <gaupel/diag/DiagCode.hpp>
#include <gaupel/diag/Diagnostic.hpp>

#include <cstdint>
#include <string_view>
#include <vector>
#include <unordered_set>


namespace gaupel::passes {

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
        std::unordered_set<uint32_t>& param_symbol_ids
    );

    // -----------------------------------------------------------------------------
    // Shadowing/duplicate diagnostics + insert wrapper
    // -----------------------------------------------------------------------------
    static sema::SymbolTable::InsertResult declare_(
        sema::SymbolKind kind,
        std::string_view name,
        gaupel::ty::TypeId ty,
        Span span,
        sema::SymbolTable& sym,
        diag::Bag& bag,
        const NameResolveOptions& opt
    ) {
        auto ins = sym.insert(kind, name, ty, span);

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
        std::unordered_set<uint32_t>& param_symbol_ids
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
                    auto sid = sym.lookup(e.text);
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

                case ast::ExprKind::kLoop: {
                    // loop expression introduces its own scope for header var.
                    ScopeGuard g(sym);

                    // Iter expression should be resolved BEFORE loop variable declaration.
                    // (loop header variable is body-local in v0 policy.)
                    if (is_valid_expr_id_(r, e.loop_iter)) {
                        walk_expr(ast, r, e.loop_iter, sym, bag, opt, out, param_symbol_ids);
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
                        walk_stmt(ast, r, e.loop_body, sym, bag, opt, out, param_symbol_ids);
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
                        if (is_valid_stmt_id_(r, sb)) walk_stmt(ast, r, sb, sym, bag, opt, out, param_symbol_ids);
                    }

                    if (is_valid_expr_id_(r, e.c)) {
                        stack.push_back(e.c);
                    } else {
                        const ast::StmtId sc = (ast::StmtId)e.c;
                        if (is_valid_stmt_id_(r, sc)) walk_stmt(ast, r, sc, sym, bag, opt, out, param_symbol_ids);
                    }
                    break;
                }

                case ast::ExprKind::kBlockExpr: {
                    // IMPORTANT (current parser):
                    // - e.a : StmtId (block stmt), stored in ExprId slot by convention
                    // - e.b : tail ExprId (or invalid)
                    const ast::StmtId blk = (ast::StmtId)e.a;
                    if (is_valid_stmt_id_(r, blk)) {
                        walk_stmt(ast, r, blk, sym, bag, opt, out, param_symbol_ids);
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
        std::unordered_set<uint32_t>& param_symbol_ids
    ) {
        const auto& kids = ast.stmt_children();
        const uint32_t begin = s.stmt_begin;
        const uint32_t end   = s.stmt_begin + s.stmt_count;

        // 방어: 깨진 AST에서도 out-of-range 방지
        if (begin >= r.stmt_children_count) return;
        if (end > r.stmt_children_count) return;

        for (uint32_t i = begin; i < end; ++i) {
            walk_stmt(ast, r, kids[i], sym, bag, opt, out, param_symbol_ids);
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
        std::unordered_set<uint32_t>& param_symbol_ids
    ) {
        if (!is_valid_stmt_id_(r, id)) return;
        ensure_result_sized_(r, out);

        const auto& s = ast.stmt(id);

        switch (s.kind) {
            case ast::StmtKind::kEmpty:
                return;

            case ast::StmtKind::kExprStmt:
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids);
                return;

            case ast::StmtKind::kVar: {
                // init 먼저
                if (s.init != ast::k_invalid_expr) {
                    walk_expr(ast, r, s.init, sym, bag, opt, out, param_symbol_ids);
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
                walk_block_children_(ast, r, s, sym, bag, opt, out, param_symbol_ids);
                return;
            }

            case ast::StmtKind::kIf:
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids);
                walk_stmt(ast, r, s.a, sym, bag, opt, out, param_symbol_ids);
                walk_stmt(ast, r, s.b, sym, bag, opt, out, param_symbol_ids);
                return;

            case ast::StmtKind::kWhile:
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids);
                walk_stmt(ast, r, s.a, sym, bag, opt, out, param_symbol_ids);
                return;

            case ast::StmtKind::kReturn:
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids);
                return;

            case ast::StmtKind::kBreak:
            case ast::StmtKind::kContinue:
                return;

            case ast::StmtKind::kFnDecl: {
                // 1) 함수 이름을 현재 스코프에 등록 + stmt 기록
                auto fins = declare_(sema::SymbolKind::kFn, s.name, s.type, s.span, sym, bag, opt);
                if (!fins.is_duplicate) {
                    const auto rid = add_resolved_(out, BindingKind::kFn, fins.symbol_id, s.span);
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
                            walk_expr(ast, r, p.default_expr, sym, bag, opt, out, param_symbol_ids);
                        }
                    }
                }

                // 4) body
                walk_stmt(ast, r, s.a, sym, bag, opt, out, param_symbol_ids);
                return;
            }

            case ast::StmtKind::kFieldDecl: {
                auto ins = declare_(sema::SymbolKind::kField, s.name, ast::k_invalid_type, s.span, sym, bag, opt);
                if (!ins.is_duplicate) {
                    const auto rid = add_resolved_(out, BindingKind::kType, ins.symbol_id, s.span);
                    out.stmt_to_resolved[(uint32_t)id] = rid;
                }
                return;
            }

            case ast::StmtKind::kActsDecl: {
                auto ins = declare_(sema::SymbolKind::kAct, s.name, ast::k_invalid_type, s.span, sym, bag, opt);
                if (!ins.is_duplicate) {
                    const auto rid = add_resolved_(out, BindingKind::kType, ins.symbol_id, s.span);
                    out.stmt_to_resolved[(uint32_t)id] = rid;
                }

                ScopeGuard g(sym);
                const auto& kids = ast.stmt_children();
                const uint32_t begin = s.stmt_begin;
                const uint32_t end = s.stmt_begin + s.stmt_count;
                if (begin < r.stmt_children_count && end <= r.stmt_children_count) {
                    for (uint32_t i = begin; i < end; ++i) {
                        walk_stmt(ast, r, kids[i], sym, bag, opt, out, param_symbol_ids);
                    }
                }
                return;
            }

            case ast::StmtKind::kSwitch: {
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids);

                const uint32_t cb = s.case_begin;
                const uint32_t ce = s.case_begin + s.case_count;
                if (cb < r.switch_case_count && ce <= r.switch_case_count) {
                    const auto& cs = ast.switch_cases();
                    for (uint32_t i = cb; i < ce; ++i) {
                        walk_stmt(ast, r, cs[i].body, sym, bag, opt, out, param_symbol_ids);
                    }
                }
                return;
            }

            case ast::StmtKind::kUse:
                // NOTE: use의 선언성(별칭/타입별칭 등)을 심볼로 올릴지 여부는 스펙 결정 후 확장.
                // 지금은 expr만 검사한다.
                walk_expr(ast, r, s.expr, sym, bag, opt, out, param_symbol_ids);
                return;

            default:
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

        walk_stmt(ast, r, root, sym, bag, opt, out_result, param_symbol_ids);
    }

} // namespace gaupel::passes
