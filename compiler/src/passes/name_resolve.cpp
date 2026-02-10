// compiler/src/passes/name_resolve.cpp
#include <gaupel/passes/NameResolve.hpp>
#include <gaupel/diag/DiagCode.hpp>
#include <gaupel/diag/Diagnostic.hpp>

#include <cstdint>
#include <string_view>
#include <vector>


namespace gaupel::passes {

    // -----------------------------------------------------------------------------
    // Diagnostics helpers
    // -----------------------------------------------------------------------------
    static void report(diag::Bag& bag, diag::Severity sev, diag::Code code, Span span, std::string_view a0 = {}) {
        diag::Diagnostic d(sev, code, span);
        if (!a0.empty()) d.add_arg(a0);
        bag.add(std::move(d));
    }

    [[maybe_unused]]
    static void report_int(diag::Bag& bag, diag::Severity sev, diag::Code code, Span span, int v0) {
        diag::Diagnostic d(sev, code, span);
        d.add_arg_int(v0);
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

    // -----------------------
    // Forward declarations
    // -----------------------
    static void walk_stmt(
        const ast::AstArena& ast,
        ast::StmtId id,
        sema::SymbolTable& sym,
        diag::Bag& bag,
        const NameResolveOptions& opt
    );

    // -----------------------
    // Id validation helpers
    // -----------------------
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

    // -----------------------------------------------------------------------------
    // Expr walk (Ident 체크)
    // -----------------------------------------------------------------------------
    static void walk_expr(
        const ast::AstArena& ast,
        ast::ExprId root,
        sema::SymbolTable& sym,
        diag::Bag& bag,
        const NameResolveOptions& opt
    ) {
        if (root == ast::k_invalid_expr) return;

        const IdRanges r = ranges_of_(ast);

        // visited: 사이클/중복 방문 차단 (v0에선 충분)
        std::vector<uint8_t> visited;
        visited.resize(r.expr_count, 0);

        // 명시적 스택으로 DFS
        std::vector<ast::ExprId> stack;
        stack.reserve(64);
        stack.push_back(root);

        while (!stack.empty()) {
            ast::ExprId id = stack.back();
            stack.pop_back();

            if (!is_valid_expr_id_(r, id)) continue;
            if (visited[(uint32_t)id]) continue;
            visited[(uint32_t)id] = 1;

            const auto& e = ast.expr(id);

            switch (e.kind) {
                case ast::ExprKind::kIdent: {
                    if (!sym.lookup(e.text)) {
                        report(bag, diag::Severity::kError, diag::Code::kUndefinedName, e.span, e.text);
                    }
                    break;
                }

                case ast::ExprKind::kUnary:
                case ast::ExprKind::kPostfixUnary: {
                    if (is_valid_expr_id_(r, e.a)) stack.push_back(e.a);
                    break;
                }

                case ast::ExprKind::kBinary:
                case ast::ExprKind::kAssign: {
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

                case ast::ExprKind::kIndex: {
                    if (is_valid_expr_id_(r, e.a)) stack.push_back(e.a);
                    if (is_valid_expr_id_(r, e.b)) stack.push_back(e.b);
                    break;
                }

                case ast::ExprKind::kCall: {
                    // callee
                    if (is_valid_expr_id_(r, e.a)) stack.push_back(e.a);

                    // args (positional + named-group)
                    const auto& args = ast.args();
                    const auto& ng   = ast.named_group_args();

                    // 방어: 범위 체크
                    const uint32_t arg_end = e.arg_begin + e.arg_count;
                    if (e.arg_begin < r.arg_count && arg_end <= r.arg_count) {
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

                case ast::ExprKind::kLoop: {
                    // loop header expr
                    if (is_valid_expr_id_(r, e.loop_iter)) stack.push_back(e.loop_iter);

                    // IMPORTANT:
                    // loop body is a StmtId (do NOT treat it as ExprId)
                    if (is_valid_stmt_id_(r, e.loop_body)) {
                        walk_stmt(ast, e.loop_body, sym, bag, opt);
                    }
                    break;
                }

                case ast::ExprKind::kIfExpr: {
                    // v0 parse_expr_if(): cond=a, then=b, else=c  (then/else are ExprId(BlockExpr))
                    // BUT for old/buggy trees, b/c might accidentally contain StmtId.
                    if (is_valid_expr_id_(r, e.a)) stack.push_back(e.a);

                    if (is_valid_expr_id_(r, e.b)) {
                        stack.push_back(e.b);
                    } else {
                        const ast::StmtId sb = (ast::StmtId)e.b;
                        if (is_valid_stmt_id_(r, sb)) walk_stmt(ast, sb, sym, bag, opt);
                    }

                    if (is_valid_expr_id_(r, e.c)) {
                        stack.push_back(e.c);
                    } else {
                        const ast::StmtId sc = (ast::StmtId)e.c;
                        if (is_valid_stmt_id_(r, sc)) walk_stmt(ast, sc, sym, bag, opt);
                    }
                    break;
                }

                case ast::ExprKind::kBlockExpr: {
                    // block expr 또한 v0에서 stmt/expr id 혼선 가능성이 큼.
                    // IMPORTANT (current parser):
                    // - e.a : StmtId (block stmt), stored in ExprId slot by convention
                    // - e.b : tail ExprId (or invalid)
                    const ast::StmtId blk = (ast::StmtId)e.a;
                    if (is_valid_stmt_id_(r, blk)) {
                        walk_stmt(ast, blk, sym, bag, opt);
                    }

                    // tail expr (value of block)
                    if (is_valid_expr_id_(r, e.b)) stack.push_back(e.b);

                    // legacy / future-proof: if someone used c as tail, keep it safe
                    if (is_valid_expr_id_(r, e.c)) stack.push_back(e.c);
                    break;
                }

                default:
                    // literals, null, hole, error 등: 자식 없음
                    break;
            }
        }
    }

    // -----------------------------------------------------------------------------
    // Stmt walk + scope
    // -----------------------------------------------------------------------------
    static void declare_var_like(
        sema::SymbolKind kind,
        std::string_view name,
        gaupel::ty::TypeId declared_type,
        Span span,
        sema::SymbolTable& sym,
        diag::Bag& bag,
        const NameResolveOptions& opt
    ) {
        auto ins = sym.insert(kind, name, declared_type, span);

        if (ins.is_duplicate) {
            // 같은 스코프 중복 선언은 에러
            report(bag, diag::Severity::kError, diag::Code::kDuplicateDecl, span, name);
            return;
        }

        if (ins.is_shadowing) {
            // shadowing은 기본 허용. 옵션에 따라 진단만 추가.
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
    }

    static void walk_block_children_(
        const ast::AstArena& ast,
        const ast::Stmt& s,
        sema::SymbolTable& sym,
        diag::Bag& bag,
        const NameResolveOptions& opt
    ) {
        const IdRanges r = ranges_of_(ast);
        const auto& kids = ast.stmt_children();

        // 방어: 범위 체크 (깨진 AST에서도 out-of-range 방지)
        const uint32_t begin = s.stmt_begin;
        const uint32_t end = s.stmt_begin + s.stmt_count;
        if (begin >= r.stmt_children_count) return;
        if (end > r.stmt_children_count) return;

        for (uint32_t i = begin; i < end; ++i) {
            walk_stmt(ast, kids[i], sym, bag, opt);
        }
    }

    static void walk_fn_decl_(
        const ast::AstArena& ast,
        const ast::Stmt& fn,
        sema::SymbolTable& sym,
        diag::Bag& bag,
        const NameResolveOptions& opt
    ) {
        const IdRanges r = ranges_of_(ast);

        // 1) 함수 이름은 현재 스코프(보통 top-level)에 등록
        declare_var_like(sema::SymbolKind::kFn, fn.name, fn.type, fn.span, sym, bag, opt);

        // 2) 함수 바디 스코프 시작
        sym.push_scope();

        // 3) 파라미터 선언 등록
        const auto& ps = ast.params();
        const uint32_t pb = fn.param_begin;
        const uint32_t pe = fn.param_begin + fn.param_count;

        if (pb < r.param_count && pe <= r.param_count) {
            for (uint32_t i = pb; i < pe; ++i) {
                const auto& p = ps[i];

                declare_var_like(sema::SymbolKind::kVar, p.name, p.type, p.span, sym, bag, opt);

                // default expr 내부 이름 사용 검사
                if (p.has_default && p.default_expr != ast::k_invalid_expr) {
                    walk_expr(ast, p.default_expr, sym, bag, opt);
                }
            }
        }

        // 4) 바디
        walk_stmt(ast, fn.a, sym, bag, opt);

        sym.pop_scope();
    }

    static void walk_stmt(
        const ast::AstArena& ast,
        ast::StmtId id,
        sema::SymbolTable& sym,
        diag::Bag& bag,
        const NameResolveOptions& opt
    ) {
        if (id == ast::k_invalid_stmt) return;

        const IdRanges r = ranges_of_(ast);
        if (!is_valid_stmt_id_(r, id)) return;

        const auto& s = ast.stmt(id);

        switch (s.kind) {
            case ast::StmtKind::kEmpty:
                return;

            case ast::StmtKind::kExprStmt:
                walk_expr(ast, s.expr, sym, bag, opt);
                return;

            case ast::StmtKind::kVar: {
                // let/set 공통: init expr 먼저 검사(러스트는 선언 RHS에서 이전 스코프를 보게 됨)
                // "let x = x" 같은 케이스는 바깥 x를 참조(새 x는 아직 스코프에 없음) 형태가 자연스럽다.
                if (s.init != ast::k_invalid_expr) {
                    walk_expr(ast, s.init, sym, bag, opt);
                }

                if (!s.is_set) {
                    // let: 새 선언
                    declare_var_like(sema::SymbolKind::kVar, s.name, s.type, s.span, sym, bag, opt);
                } else {
                    // set: v0+ 의미 = 타입추론 선언
                    // - 심볼을 새로 등록 (declared_type이 없으면 invalid로 넣고, tyck에서 채우거나 error 처리)
                    declare_var_like(sema::SymbolKind::kVar, s.name, s.type, s.span, sym, bag, opt);
                }
                return;
            }

            case ast::StmtKind::kBlock:
                sym.push_scope();
                walk_block_children_(ast, s, sym, bag, opt);
                sym.pop_scope();
                return;

            case ast::StmtKind::kIf:
                walk_expr(ast, s.expr, sym, bag, opt);
                // then/else는 블록 자체가 push_scope를 하므로 여기선 그대로
                walk_stmt(ast, s.a, sym, bag, opt);
                walk_stmt(ast, s.b, sym, bag, opt);
                return;

            case ast::StmtKind::kWhile:
                walk_expr(ast, s.expr, sym, bag, opt);
                walk_stmt(ast, s.a, sym, bag, opt);
                return;

            case ast::StmtKind::kReturn:
                walk_expr(ast, s.expr, sym, bag, opt);
                return;

            case ast::StmtKind::kBreak:
            case ast::StmtKind::kContinue:
                return;

            case ast::StmtKind::kFnDecl:
                walk_fn_decl_(ast, s, sym, bag, opt);
                return;

            case ast::StmtKind::kSwitch: {
                // switch cond
                walk_expr(ast, s.expr, sym, bag, opt);

                // 각 case body는 block이고, block walker가 스코프를 만들 것
                const auto& cs = ast.switch_cases();
                const uint32_t cb = s.case_begin;
                const uint32_t ce = s.case_begin + s.case_count;

                if (cb < r.switch_case_count && ce <= r.switch_case_count) {
                    for (uint32_t i = cb; i < ce; ++i) {
                        walk_stmt(ast, cs[i].body, sym, bag, opt);
                    }
                }
                return;
            }

            case ast::StmtKind::kUse:
                // use 문에서 expr가 쓰일 수 있으니 검사
                walk_expr(ast, s.expr, sym, bag, opt);

                // TODO:
                // use_type_alias / module alias / ffi struct 등의 “decl 성격”을
                // 여기서 SymbolKind::kType / kFn / kField로 등록하도록 확장 가능.
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
        const NameResolveOptions& opt
    ) {
        walk_stmt(ast, root, sym, bag, opt);
    }

} // namespace gaupel::passes