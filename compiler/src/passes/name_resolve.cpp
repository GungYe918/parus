// compiler/src/passes/name_resolve.cpp
#include <gaupel/passes/NameResolve.hpp>
#include <gaupel/diag/DiagCode.hpp>
#include <gaupel/diag/Diagnostic.hpp>


namespace gaupel::passes {

    static void report(diag::Bag& bag, diag::Severity sev, diag::Code code, Span span, std::string_view a0 = {}) {
        diag::Diagnostic d(sev, code, span);
        if (!a0.empty()) d.add_arg(a0);
        bag.add(std::move(d));
    }

    static void report_int(diag::Bag& bag, diag::Severity sev, diag::Code code, Span span, int v0) {
        diag::Diagnostic d(sev, code, span);
        d.add_arg_int(v0);
        bag.add(std::move(d));
    }

    // -----------------------
    // Expr walk (Ident 체크)
    // -----------------------
    static void walk_expr(const ast::AstArena& ast, ast::ExprId id, sema::SymbolTable& sym, diag::Bag& bag);

    static void walk_call_args(
        const ast::AstArena& ast,
        uint32_t arg_begin,
        uint32_t arg_count,
        sema::SymbolTable& sym,
        diag::Bag& bag
    ) {
        const auto& args = ast.args();
        for (uint32_t i = 0; i < arg_count; ++i) {
            const auto& a = args[arg_begin + i];

            if (a.kind == ast::ArgKind::kNamedGroup) {
                const auto& ng = ast.named_group_args();
                for (uint32_t j = 0; j < a.child_count; ++j) {
                    const auto& entry = ng[a.child_begin + j];
                    if (!entry.is_hole && entry.expr != ast::k_invalid_expr) {
                        walk_expr(ast, entry.expr, sym, bag);
                    }
                }
                continue;
            }

            if (!a.is_hole && a.expr != ast::k_invalid_expr) {
                walk_expr(ast, a.expr, sym, bag);
            }
        }
    }

    static void walk_expr(const ast::AstArena& ast, ast::ExprId id, sema::SymbolTable& sym, diag::Bag& bag) {
        if (id == ast::k_invalid_expr) return;
        const auto& e = ast.expr(id);

        switch (e.kind) {
            case ast::ExprKind::kIdent: {
                // 식별자 사용은 반드시 선언되어야 함
                if (!sym.lookup(e.text)) {
                    report(bag, diag::Severity::kError, diag::Code::kUndefinedName, e.span, e.text);
                }
                break;
            }

            case ast::ExprKind::kUnary:
            case ast::ExprKind::kPostfixUnary:
                walk_expr(ast, e.a, sym, bag);
                break;

            case ast::ExprKind::kBinary:
            case ast::ExprKind::kAssign:
                walk_expr(ast, e.a, sym, bag);
                walk_expr(ast, e.b, sym, bag);
                break;

            case ast::ExprKind::kTernary:
                walk_expr(ast, e.a, sym, bag);
                walk_expr(ast, e.b, sym, bag);
                walk_expr(ast, e.c, sym, bag);
                break;

            case ast::ExprKind::kCall:
                walk_expr(ast, e.a, sym, bag);
                walk_call_args(ast, e.arg_begin, e.arg_count, sym, bag);
                break;

            case ast::ExprKind::kIndex:
                walk_expr(ast, e.a, sym, bag);
                walk_expr(ast, e.b, sym, bag);
                break;

            case ast::ExprKind::kLoop:
                // loop header expr
                if (e.loop_iter != ast::k_invalid_expr) walk_expr(ast, e.loop_iter, sym, bag);
                // loop body stmt는 stmt walker가 처리
                break;

            case ast::ExprKind::kIfExpr:
            case ast::ExprKind::kBlockExpr:
                // v0: 연결이 확정되면 확장
                if (e.a != ast::k_invalid_expr) walk_expr(ast, e.a, sym, bag);
                if (e.b != ast::k_invalid_expr) walk_expr(ast, e.b, sym, bag);
                if (e.c != ast::k_invalid_expr) walk_expr(ast, e.c, sym, bag);
                break;

            default:
                break;
        }
    }

    // -----------------------
    // Stmt walk + scope
    // -----------------------
    static void walk_stmt(const ast::AstArena& ast, ast::StmtId id, sema::SymbolTable& sym, diag::Bag& bag, const NameResolveOptions& opt);

    static void walk_block_children(const ast::AstArena& ast, const ast::Stmt& s, sema::SymbolTable& sym, diag::Bag& bag, const NameResolveOptions& opt) {
        const auto& kids = ast.stmt_children();
        for (uint32_t i = 0; i < s.stmt_count; ++i) {
            walk_stmt(ast, kids[s.stmt_begin + i], sym, bag, opt);
        }
    }

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
    
    static void walk_fn_decl(const ast::AstArena& ast, const ast::Stmt& fn, sema::SymbolTable& sym, diag::Bag& bag, const NameResolveOptions& opt) {
        // 1) 함수 이름은 현재 스코프(보통 top-level)에 등록
        declare_var_like(sema::SymbolKind::kFn, fn.name, fn.type, fn.span, sym, bag, opt);

        // 2) 함수 바디 스코프 시작
        sym.push_scope();

        // 3) 파라미터 선언 등록
        const auto& ps = ast.params();
        for (uint32_t i = 0; i < fn.param_count; ++i) {
            const auto& p = ps[fn.param_begin + i];
            declare_var_like(sema::SymbolKind::kVar, p.name, p.type, p.span, sym, bag, opt);

            // default expr 내부 이름 사용 검사
            if (p.has_default && p.default_expr != ast::k_invalid_expr) {
                walk_expr(ast, p.default_expr, sym, bag);
            }
        }

        // 4) 바디
        walk_stmt(ast, fn.a, sym, bag, opt);

        sym.pop_scope();
    }

    static void walk_stmt(const ast::AstArena& ast, ast::StmtId id, sema::SymbolTable& sym, diag::Bag& bag, const NameResolveOptions& opt) {
        if (id == ast::k_invalid_stmt) return;
        const auto& s = ast.stmt(id);

        switch (s.kind) {
            case ast::StmtKind::kEmpty:
                return;

            case ast::StmtKind::kExprStmt:
                walk_expr(ast, s.expr, sym, bag);
                return;

            case ast::StmtKind::kVar: {
                // let/set 공통: init expr 먼저 검사(러스트는 선언 RHS에서 이전 스코프를 보게 됨)
                // "let x = x" 같은 케이스는 바깥 x를 참조(새 x는 아직 스코프에 없음) 형태가 자연스럽다.
                if (s.init != ast::k_invalid_expr) {
                    walk_expr(ast, s.init, sym, bag);
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
                walk_block_children(ast, s, sym, bag, opt);
                sym.pop_scope();
                return;

            case ast::StmtKind::kIf:
                walk_expr(ast, s.expr, sym, bag);
                // then/else는 블록 자체가 push_scope를 하므로 여기선 그대로
                walk_stmt(ast, s.a, sym, bag, opt);
                walk_stmt(ast, s.b, sym, bag, opt);
                return;

            case ast::StmtKind::kWhile:
                walk_expr(ast, s.expr, sym, bag);
                walk_stmt(ast, s.a, sym, bag, opt);
                return;

            case ast::StmtKind::kReturn:
                walk_expr(ast, s.expr, sym, bag);
                return;

            case ast::StmtKind::kBreak:
            case ast::StmtKind::kContinue:
                return;

            case ast::StmtKind::kFnDecl:
                walk_fn_decl(ast, s, sym, bag, opt);
                return;

            case ast::StmtKind::kSwitch: {
                // switch cond
                walk_expr(ast, s.expr, sym, bag);

                // 각 case body는 block이고, block walker가 스코프를 만들 것
                const auto& cs = ast.switch_cases();
                for (uint32_t i = 0; i < s.case_count; ++i) {
                    walk_stmt(ast, cs[s.case_begin + i].body, sym, bag, opt);
                }
                return;
            }

            case ast::StmtKind::kUse:
                // use 문에서 expr가 쓰일 수 있으니 검사
                walk_expr(ast, s.expr, sym, bag);

                // TODO:
                // use_type_alias / module alias / ffi struct 등의 “decl 성격”을
                // 여기서 SymbolKind::kType / kFn / kField로 등록하도록 확장 가능.
                return;

            default:
                return;
        }
    }

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