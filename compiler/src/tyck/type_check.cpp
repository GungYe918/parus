// compiler/src/tyck/type_check.cpp
#include <gaupel/tyck/TypeCheck.hpp>
#include <gaupel/syntax/TokenKind.hpp>

#include <sstream>


namespace gaupel::tyck {

    using K = gaupel::syntax::TokenKind;

    // --------------------
    // public entry
    // --------------------
    TyckResult TypeChecker::check_program(ast::StmtId program_stmt) {
        result_ = {};
        expr_type_cache_.clear();
        expr_type_cache_.resize(ast_.exprs().size(), ty::kInvalidType);

        // placeholder "string" type: user-type "string"
        if (string_type_ == ty::kInvalidType) {
            string_type_ = types_.intern_ident("string");
        }

        first_pass_collect_top_level_(program_stmt);
        second_pass_check_program_(program_stmt);

        result_.ok = result_.errors.empty();
        result_.expr_types = expr_type_cache_;
        return result_;
    }

    // --------------------
    // errors
    // --------------------
    void TypeChecker::err_(Span sp, std::string msg) {
        TyError e{};
        e.span = sp;
        e.message = std::move(msg);
        result_.errors.push_back(std::move(e));
    }

    // --------------------
    // pass 1: collect top-level decls
    // --------------------
    void TypeChecker::first_pass_collect_top_level_(ast::StmtId program_stmt) {
        const ast::Stmt& prog = ast_.stmt(program_stmt);
        if (prog.kind != ast::StmtKind::kBlock) {
            err_(prog.span, "program root is not a block stmt");
            return;
        }

        // global scope는 SymbolTable이 이미 push되어 있음.
        for (uint32_t i = 0; i < prog.stmt_count; ++i) {
            const ast::StmtId cid = ast_.stmt_children()[prog.stmt_begin + i];
            const ast::Stmt& s = ast_.stmt(cid);

            // ----------------------------
            // top-level fn decl
            // ----------------------------
            if (s.kind == ast::StmtKind::kFnDecl) {
                // 1) 우선 Stmt.type이 fn 시그니처로 이미 채워져 있으면 그대로 사용
                ty::TypeId sig = s.type;
                if (sig != ty::kInvalidType && types_.get(sig).kind == ty::Kind::kFn) {
                    // ok
                } else {
                    // 2) 아니면 tyck에서 시그니처를 직접 구성한다.
                    //    - ret: (가능하면 stmt에 저장된 ret type 사용)
                    //    - params: ast params slice에서 타입 수집
                    //
                    // NOTE: 현재 AST 출력에서 `ret=i32 <id 6>`이 보이는 걸로 봐서
                    //       "함수 반환 타입"은 어딘가에 이미 TypeId로 들어있을 가능성이 크다.
                    //       그런데 이 파일의 v0 코드는 그걸 s.type 하나로만 접근하고 있어 충돌이 난다.
                    //
                    // 여기서는 "s.type가 fn이 아니면 반환 타입이라고 가정"하는 fallback을 둔다.
                    ty::TypeId ret = ty::kInvalidType;

                    // (A) s.type가 builtin/user type 같은 "반환 타입"으로 들어온 케이스를 우선 처리
                    if (sig != ty::kInvalidType && types_.get(sig).kind != ty::Kind::kFn) {
                        ret = sig;
                    }

                    // (B) 그래도 없으면 error로 둔다(파서가 별도 필드에 ret를 저장한다면 여기서 이어붙이면 됨)
                    if (ret == ty::kInvalidType) ret = types_.error();

                    // params 수집
                    std::vector<ty::TypeId> params;
                    params.reserve(s.param_count);

                    bool bad_param = false;
                    for (uint32_t pi = 0; pi < s.param_count; ++pi) {
                        const auto& p = ast_.params()[s.param_begin + pi];
                        ty::TypeId pt = p.type;

                        if (pt == ty::kInvalidType) {
                            // 파라미터 타입이 비어있으면 일단 error로 채우고 계속
                            err_(p.span, "parameter requires an explicit type");
                            pt = types_.error();
                            bad_param = true;
                        }
                        params.push_back(pt);
                    }

                    sig = types_.make_fn(ret, params.data(), (uint32_t)params.size());

                    // (선택) 시그니처를 못 만들 정도의 치명상은 아니니 별도 에러를 굳이 추가하지 않음.
                    // bad_param이면 위에서 이미 파라미터별 에러를 넣었다.
                    (void)bad_param;
                }

                auto ins = sym_.insert(sema::SymbolKind::kFn, s.name, sig, s.span);
                if (!ins.ok && ins.is_duplicate) {
                    err_(s.span, "duplicate symbol (function): " + std::string(s.name));
                }
                continue;
            }

            // ----------------------------
            // top-level var decl
            // ----------------------------
            if (s.kind == ast::StmtKind::kVar) {
                if (!s.is_set) {
                    ty::TypeId vt = (s.type == ty::kInvalidType) ? types_.error() : s.type;
                    auto ins = sym_.insert(sema::SymbolKind::kVar, s.name, vt, s.span);
                    if (!ins.ok && ins.is_duplicate) {
                        err_(s.span, "duplicate symbol (var): " + std::string(s.name));
                    }
                }
                continue;
            }

            // use / unknown / other: pass1에서는 스킵
        }
    }

    // --------------------
    // pass 2: check
    // --------------------
    void TypeChecker::second_pass_check_program_(ast::StmtId program_stmt) {
        check_stmt_(program_stmt);
    }

    // --------------------
    // stmt dispatch
    // --------------------
    void TypeChecker::check_stmt_(ast::StmtId sid) {
        const ast::Stmt& s = ast_.stmt(sid);

        switch (s.kind) {
            case ast::StmtKind::kEmpty:
                return;

            case ast::StmtKind::kExprStmt:
                if (s.expr != ast::k_invalid_expr) (void)check_expr_(s.expr);
                return;

            case ast::StmtKind::kBlock:
                check_stmt_block_(s);
                return;

            case ast::StmtKind::kVar:
                check_stmt_var_(s);
                return;

            case ast::StmtKind::kIf:
                check_stmt_if_(s);
                return;

            case ast::StmtKind::kWhile:
                check_stmt_while_(s);
                return;

            case ast::StmtKind::kReturn:
                check_stmt_return_(s);
                return;

            case ast::StmtKind::kBreak:
            case ast::StmtKind::kContinue:
                // loop context는 나중에 확장
                return;

            case ast::StmtKind::kSwitch:
                check_stmt_switch_(s);
                return;

            case ast::StmtKind::kFnDecl:
                check_stmt_fn_decl_(s);
                return;

            case ast::StmtKind::kUse:
                // use는 타입체커에서 보통 "심볼/타입/FFI 등록" 패스로 처리
                return;

            case ast::StmtKind::kError:
                return;
        }
    }

    void TypeChecker::check_stmt_block_(const ast::Stmt& s) {
        // 새 스코프
        sym_.push_scope();

        for (uint32_t i = 0; i < s.stmt_count; ++i) {
            const ast::StmtId cid = ast_.stmt_children()[s.stmt_begin + i];
            check_stmt_(cid);
        }

        sym_.pop_scope();
    }

    void TypeChecker::check_stmt_var_(const ast::Stmt& s) {
        // let/set
        if (!s.is_set) {
            // let name: Type (= init)?
            if (s.type == ty::kInvalidType) {
                err_(s.span, "let requires an explicit declared type");
            }

            // init 타입 검사(있으면)
            ty::TypeId init_t = ty::kInvalidType;
            if (s.init != ast::k_invalid_expr) {
                init_t = check_expr_(s.init);

                if (s.type != ty::kInvalidType && !can_assign_(s.type, init_t)) {
                    std::ostringstream oss;
                    oss << "cannot initialize let '" << s.name
                        << "': expected " << types_.to_string(s.type)
                        << ", got " << types_.to_string(init_t);
                    err_(s.span, oss.str());
                }
            }

            // 현재 스코프에 삽입 (top-level은 pass1에서 이미 넣었지만 block var는 여기서 넣음)
            ty::TypeId vt = (s.type == ty::kInvalidType) ? types_.error() : s.type;
            auto ins = sym_.insert(sema::SymbolKind::kVar, s.name, vt, s.span);
            if (!ins.ok && ins.is_duplicate) {
                err_(s.span, "duplicate symbol (var): " + std::string(s.name));
            }
            return;
        }

        // set name = init (type annotation 금지 정책은 파서 레벨에서 이미 처리한다고 가정)
        auto id = sym_.lookup(s.name);
        if (!id) {
            err_(s.span, "set refers to unknown variable: " + std::string(s.name));
            if (s.init != ast::k_invalid_expr) (void)check_expr_(s.init);
            return;
        }

        const auto& sym = sym_.symbol(*id);
        if (sym.kind != sema::SymbolKind::kVar) {
            err_(s.span, "set target is not a variable: " + std::string(s.name));
        }

        if (s.init == ast::k_invalid_expr) {
            err_(s.span, "set requires initializer expression");
            return;
        }

        ty::TypeId rhs = check_expr_(s.init);
        ty::TypeId dst = sym.declared_type;
        if (!can_assign_(dst, rhs)) {
            std::ostringstream oss;
            oss << "cannot assign to '" << s.name
                << "': expected " << types_.to_string(dst)
                << ", got " << types_.to_string(rhs);
            err_(s.span, oss.str());
        }
    }

    void TypeChecker::check_stmt_if_(const ast::Stmt& s) {
        // s.expr = cond, s.a = then block, s.b = else block
        if (s.expr != ast::k_invalid_expr) {
            ty::TypeId ct = check_expr_(s.expr);
            if (ct != types_.builtin(ty::Builtin::kBool) && !is_error_(ct)) {
                err_(s.span, "if condition must be bool");
            }
        }
        if (s.a != ast::k_invalid_stmt) check_stmt_(s.a);
        if (s.b != ast::k_invalid_stmt) check_stmt_(s.b);
    }

    void TypeChecker::check_stmt_while_(const ast::Stmt& s) {
        if (s.expr != ast::k_invalid_expr) {
            ty::TypeId ct = check_expr_(s.expr);
            if (ct != types_.builtin(ty::Builtin::kBool) && !is_error_(ct)) {
                err_(s.span, "while condition must be bool");
            }
        }
        if (s.a != ast::k_invalid_stmt) check_stmt_(s.a);
    }

    void TypeChecker::check_stmt_return_(const ast::Stmt& s) {
        if (!fn_ctx_.in_fn) {
            err_(s.span, "return outside of function");
            if (s.expr != ast::k_invalid_expr) (void)check_expr_(s.expr);
            return;
        }

        ty::TypeId rt = fn_ctx_.ret;
        if (rt == ty::kInvalidType) rt = types_.error();

        if (s.expr == ast::k_invalid_expr) {
            // void/empty return은 현재 타입 시스템에 void가 없으므로 정책적으로 막아둔다.
            // (향후 Builtin::kUnit 같은 걸 추가하거나, return;을 허용하려면 여기만 바꾸면 됨)
            err_(s.span, "return expression is required (no 'unit' type yet)");
            return;
        }

        ty::TypeId v = check_expr_(s.expr);
        if (!can_assign_(rt, v)) {
            std::ostringstream oss;
            oss << "return type mismatch: expected " << types_.to_string(rt)
                << ", got " << types_.to_string(v);
            err_(s.span, oss.str());
        }
    }

    void TypeChecker::check_stmt_switch_(const ast::Stmt& s) {
        // v0: switch(expr){ case ... {block} ... }
        if (s.expr != ast::k_invalid_expr) {
            (void)check_expr_(s.expr);
        }
        // case body는 항상 block
        for (uint32_t i = 0; i < s.case_count; ++i) {
            const auto& c = ast_.switch_cases()[s.case_begin + i];
            if (c.body != ast::k_invalid_stmt) check_stmt_(c.body);
        }
    }

    void TypeChecker::check_stmt_fn_decl_(const ast::Stmt& s) {
        // ----------------------------
        // 0) 시그니처 타입 확보
        // ----------------------------
        ty::TypeId sig = s.type;

        ty::TypeId ret = ty::kInvalidType;

        // (1) 파서가 Stmt.type에 fn 시그니처를 넣어준 경우
        if (sig != ty::kInvalidType && types_.get(sig).kind == ty::Kind::kFn) {
            ret = types_.get(sig).ret;
        } else {
            // (2) 아니면 tyck에서 직접 만든다.
            //     - s.type가 fn이 아니면 "반환 타입"으로 들어왔을 가능성이 높으므로 그걸 우선 ret로 사용
            if (sig != ty::kInvalidType && types_.get(sig).kind != ty::Kind::kFn) {
                ret = sig;
            }
            if (ret == ty::kInvalidType) {
                // 반환 타입을 얻을 길이 없으면 error로 둔다.
                // (나중에 AST에 fn_ret_type 필드를 확정하면 여기서 그 필드를 쓰면 됨)
                ret = types_.error();
                err_(s.span, "fn decl is missing return type (cannot form signature)");
            }

            std::vector<ty::TypeId> params;
            params.reserve(s.param_count);

            for (uint32_t i = 0; i < s.param_count; ++i) {
                const auto& p = ast_.params()[s.param_begin + i];
                ty::TypeId pt = p.type;
                if (pt == ty::kInvalidType) {
                    err_(p.span, "parameter requires an explicit type");
                    pt = types_.error();
                }
                params.push_back(pt);
            }

            sig = types_.make_fn(ret, params.data(), (uint32_t)params.size());
        }

        // ----------------------------
        // 1) 함수 스코프 진입 + fn ctx 설정
        // ----------------------------
        sym_.push_scope();

        FnCtx saved = fn_ctx_;
        fn_ctx_.in_fn = true;
        fn_ctx_.is_pure = s.is_pure;
        fn_ctx_.is_comptime = s.is_comptime;
        fn_ctx_.ret = (ret == ty::kInvalidType) ? types_.error() : ret;

        // ----------------------------
        // 2) 파라미터 심볼 삽입 + default expr 검사
        // ----------------------------
        for (uint32_t i = 0; i < s.param_count; ++i) {
            const auto& p = ast_.params()[s.param_begin + i];
            ty::TypeId pt = (p.type == ty::kInvalidType) ? types_.error() : p.type;

            auto ins = sym_.insert(sema::SymbolKind::kVar, p.name, pt, p.span);
            if (!ins.ok && ins.is_duplicate) {
                err_(p.span, "duplicate parameter name: " + std::string(p.name));
            }

            if (p.has_default && p.default_expr != ast::k_invalid_expr) {
                ty::TypeId dt = check_expr_(p.default_expr);
                if (!can_assign_(pt, dt)) {
                    std::ostringstream oss;
                    oss << "default value type mismatch for param '" << p.name
                        << "': expected " << types_.to_string(pt)
                        << ", got " << types_.to_string(dt);
                    err_(p.span, oss.str());
                }
            }
        }

        // ----------------------------
        // 3) 본문 체크
        // ----------------------------
        if (s.a != ast::k_invalid_stmt) {
            check_stmt_(s.a);
        }

        // ----------------------------
        // 4) 종료
        // ----------------------------
        fn_ctx_ = saved;
        sym_.pop_scope();
    }

    // --------------------
    // expr: memoized dispatcher
    // --------------------
    ty::TypeId TypeChecker::check_expr_(ast::ExprId eid) {
        if (eid == ast::k_invalid_expr) return types_.error();
        if (eid >= expr_type_cache_.size()) return types_.error();

        if (expr_type_cache_[eid] != ty::kInvalidType) {
            return expr_type_cache_[eid];
        }

        const ast::Expr& e = ast_.expr(eid);
        ty::TypeId t = types_.error();

        switch (e.kind) {
            case ast::ExprKind::kError:
                t = types_.error();
                break;

            case ast::ExprKind::kIntLit:
                // v0: default i64
                t = types_.builtin(ty::Builtin::kI64);
                break;

            case ast::ExprKind::kFloatLit:
                // v0: default f64
                t = types_.builtin(ty::Builtin::kF64);
                break;

            case ast::ExprKind::kStringLit:
                t = string_type_;
                break;

            case ast::ExprKind::kCharLit:
                t = types_.builtin(ty::Builtin::kChar);
                break;

            case ast::ExprKind::kBoolLit:
                t = types_.builtin(ty::Builtin::kBool);
                break;

            case ast::ExprKind::kNullLit:
                t = types_.builtin(ty::Builtin::kNull);
                break;

            case ast::ExprKind::kIdent: {
                auto id = sym_.lookup(e.text);
                if (!id) {
                    err_(e.span, "unknown identifier: " + std::string(e.text));
                    t = types_.error();
                } else {
                    t = sym_.symbol(*id).declared_type;
                    if (t == ty::kInvalidType) t = types_.error();
                }
                break;
            }

            case ast::ExprKind::kHole:
                err_(e.span, "unresolved hole '_' in expression");
                t = types_.error();
                break;

            case ast::ExprKind::kUnary:
                t = check_expr_unary_(e);
                break;

            case ast::ExprKind::kPostfixUnary:
                t = check_expr_postfix_unary_(e);
                break;

            case ast::ExprKind::kBinary:
                t = check_expr_binary_(e);
                break;

            case ast::ExprKind::kAssign:
                t = check_expr_assign_(e);
                break;

            case ast::ExprKind::kTernary:
                t = check_expr_ternary_(e);
                break;

            case ast::ExprKind::kCall:
                t = check_expr_call_(e);
                break;

            case ast::ExprKind::kIndex:
                t = check_expr_index_(e);
                break;

            case ast::ExprKind::kIfExpr:
                t = check_expr_if_(e);
                break;

            case ast::ExprKind::kBlockExpr:
                t = check_expr_block_(e);
                break;

            case ast::ExprKind::kLoop:
                t = check_expr_loop_(e);
                break;
        }

        expr_type_cache_[eid] = t;
        return t;
    }

    // --------------------
    // helpers: type predicates
    // --------------------
    bool TypeChecker::is_optional_(ty::TypeId t) const {
        if (t == ty::kInvalidType) return false;
        return types_.get(t).kind == ty::Kind::kOptional;
    }

    ty::TypeId TypeChecker::optional_elem_(ty::TypeId opt) const {
        if (!is_optional_(opt)) return ty::kInvalidType;
        return types_.get(opt).elem;
    }

    bool TypeChecker::is_null_(ty::TypeId t) const {
        return t == types_.builtin(ty::Builtin::kNull);
    }

    bool TypeChecker::is_error_(ty::TypeId t) const {
        return t == types_.error() || types_.get(t).kind == ty::Kind::kError;
    }

    bool TypeChecker::can_assign_(ty::TypeId dst, ty::TypeId src) const {
        if (is_error_(dst) || is_error_(src)) return true;
        if (dst == src) return true;

        // null -> T? 허용
        if (is_null_(src) && is_optional_(dst)) return true;

        return false;
    }

    ty::TypeId TypeChecker::unify_(ty::TypeId a, ty::TypeId b) {
        if (is_error_(a) || is_error_(b)) return types_.error();
        if (a == b) return a;

        // null + T? => T?
        if (is_null_(a) && is_optional_(b)) return b;
        if (is_null_(b) && is_optional_(a)) return a;

        // null + T => T? (정책: 삼항 등에서 null 섞이면 옵셔널로 승격)
        if (is_null_(a)) return types_.make_optional(b);
        if (is_null_(b)) return types_.make_optional(a);

        // v0: 나머지는 error
        return types_.error();
    }

    // place expr (v0: Ident, Index만 place로 인정)
    bool TypeChecker::is_place_expr_(ast::ExprId eid) const {
        if (eid == ast::k_invalid_expr) return false;
        const auto& e = ast_.expr(eid);
        return e.kind == ast::ExprKind::kIdent || e.kind == ast::ExprKind::kIndex;
    }

    // --------------------
    // unary / postfix unary
    // --------------------
    ty::TypeId TypeChecker::check_expr_unary_(const ast::Expr& e) {
        // e.op, e.a
        ty::TypeId at = check_expr_(e.a);

        // & / && 는 place 필요 + pure/comptime 금지 (너의 정책 반영)
        if (e.op == K::kAmp) {
            if (!is_place_expr_(e.a)) {
                err_(e.span, "borrow '&' requires a place expression (ident/index)");
                return types_.error();
            }
            if (fn_ctx_.is_pure || fn_ctx_.is_comptime) {
                err_(e.span, "borrow '&' is not allowed in pure/comptime functions (recommended rule)");
                return types_.error();
            }
            // mut은 unary op에 저장되는 구조가 아직 없으니 v0에서는 &만 지원.
            // (&mut)은 이후 AST/Parser 확장 시 여기에서 반영 가능.
            return types_.make_borrow(at, /*is_mut=*/false);
        }

        if (e.op == K::kAmpAmp) {
            if (!is_place_expr_(e.a)) {
                err_(e.span, "escape '&&' requires a place expression (ident/index)");
                return types_.error();
            }
            if (fn_ctx_.is_pure || fn_ctx_.is_comptime) {
                err_(e.span, "escape '&&' is not allowed in pure/comptime functions (recommended rule)");
                return types_.error();
            }
            return types_.make_escape(at);
        }

        // 기타 unary: v0에서는 최소만
        if (e.op == K::kBang) {
            if (at != types_.builtin(ty::Builtin::kBool) && !is_error_(at)) {
                err_(e.span, "operator '!' requires bool");
            }
            return types_.builtin(ty::Builtin::kBool);
        }

        if (e.op == K::kMinus || e.op == K::kPlus) {
            // 숫자만(간단히 i*/u*/f*를 모두 “numeric”으로 취급)
            return at;
        }

        return types_.error();
    }

    ty::TypeId TypeChecker::check_expr_postfix_unary_(const ast::Expr& e) {
        // v0: postfix ++만 있다고 가정
        if (!is_place_expr_(e.a)) {
            err_(e.span, "postfix operator requires a place expression");
            return types_.error();
        }
        ty::TypeId at = check_expr_(e.a);
        return at;
    }

    // --------------------
    // binary / assign / ternary
    // --------------------
    ty::TypeId TypeChecker::check_expr_binary_(const ast::Expr& e) {
        ty::TypeId lt = check_expr_(e.a);
        ty::TypeId rt = check_expr_(e.b);

        // == / != 에서 null 비교 제한:
        // - (T? == null) OK
        // - (T  == null) ERROR
        // - (null == null) OK
        if (e.op == K::kEqEq || e.op == K::kBangEq) {
            if (is_null_(lt) && is_null_(rt)) {
                return types_.builtin(ty::Builtin::kBool);
            }
            if (is_null_(lt) && !is_optional_(rt)) {
                err_(e.span, "null comparison is only allowed with optional types (rhs is not optional)");
            }
            if (is_null_(rt) && !is_optional_(lt)) {
                err_(e.span, "null comparison is only allowed with optional types (lhs is not optional)");
            }
            return types_.builtin(ty::Builtin::kBool);
        }

        // 논리 and/or는 키워드로 처리한다고 했으니 여기선 &&/|| 안 다룸
        // 간단 산술: + - * / % 는 "좌우 타입 동일"만 허용
        if (e.op == K::kPlus || e.op == K::kMinus || e.op == K::kStar || e.op == K::kSlash || e.op == K::kPercent) {
            if (lt != rt && !is_error_(lt) && !is_error_(rt)) {
                err_(e.span, "binary arithmetic requires both operands to have the same type (v0 rule)");
                return types_.error();
            }
            return lt;
        }

        // 비교 < <= > >= : 동일 타입만
        if (e.op == K::kLt || e.op == K::kLtEq || e.op == K::kGt || e.op == K::kGtEq) {
            if (lt != rt && !is_error_(lt) && !is_error_(rt)) {
                err_(e.span, "comparison requires both operands to have the same type (v0 rule)");
            }
            return types_.builtin(ty::Builtin::kBool);
        }

        return types_.error();
    }

    ty::TypeId TypeChecker::check_expr_assign_(const ast::Expr& e) {
        // e.a = lhs, e.b = rhs
        if (!is_place_expr_(e.a)) {
            err_(e.span, "assignment lhs must be a place expression (ident/index)");
        }
        ty::TypeId lt = check_expr_(e.a);
        ty::TypeId rt = check_expr_(e.b);

        if (!can_assign_(lt, rt)) {
            std::ostringstream oss;
            oss << "cannot assign: expected " << types_.to_string(lt)
                << ", got " << types_.to_string(rt);
            err_(e.span, oss.str());
        }
        return lt;
    }

    ty::TypeId TypeChecker::check_expr_ternary_(const ast::Expr& e) {
        // a ? b : c
        ty::TypeId ct = check_expr_(e.a);
        if (ct != types_.builtin(ty::Builtin::kBool) && !is_error_(ct)) {
            err_(e.span, "ternary condition must be bool");
        }
        ty::TypeId t1 = check_expr_(e.b);
        ty::TypeId t2 = check_expr_(e.c);
        return unify_(t1, t2);
    }

    // --------------------
    // call / index
    // --------------------
    ty::TypeId TypeChecker::check_expr_call_(const ast::Expr& e) {
        // e.a = callee, args slice in e.arg_begin/e.arg_count
        ty::TypeId callee_t = check_expr_(e.a);
        const auto& ct = types_.get(callee_t);

        if (ct.kind != ty::Kind::kFn) {
            err_(e.span, "call target is not a function");
            // 그래도 args는 검사해서 에러 누락 방지
            for (uint32_t i = 0; i < e.arg_count; ++i) {
                const auto& a = ast_.args()[e.arg_begin + i];
                if (a.expr != ast::k_invalid_expr) (void)check_expr_(a.expr);
            }
            return types_.error();
        }

        // v0: positional only 검사 (named-group/label은 후속 단계에서 확장)
        // ArgKind::kNamedGroup도 일단 child들을 검사해준다.
        uint32_t positional_count = 0;
        for (uint32_t i = 0; i < e.arg_count; ++i) {
            const auto& a = ast_.args()[e.arg_begin + i];
            if (a.kind == ast::ArgKind::kNamedGroup) continue;
            positional_count++;
        }

        if (positional_count != ct.param_count) {
            std::ostringstream oss;
            oss << "argument count mismatch: expected " << ct.param_count
                << ", got " << positional_count;
            err_(e.span, oss.str());
        }

        // 실제 타입 검사
        uint32_t pi = 0;
        for (uint32_t i = 0; i < e.arg_count; ++i) {
            const auto& a = ast_.args()[e.arg_begin + i];

            if (a.kind == ast::ArgKind::kNamedGroup) {
                // 그룹 자체: 내부 child 검사
                for (uint32_t k = 0; k < a.child_count; ++k) {
                    const auto& ca = ast_.named_group_args()[a.child_begin + k];
                    if (ca.expr != ast::k_invalid_expr) (void)check_expr_(ca.expr);
                }
                continue;
            }

            ty::TypeId at = (a.expr != ast::k_invalid_expr) ? check_expr_(a.expr) : types_.error();

            if (pi < ct.param_count) {
                ty::TypeId expected = types_.fn_param_at(callee_t, pi);
                if (!can_assign_(expected, at)) {
                    std::ostringstream oss;
                    oss << "argument type mismatch at #" << pi
                        << ": expected " << types_.to_string(expected)
                        << ", got " << types_.to_string(at);
                    err_(a.span, oss.str());
                }
            }

            pi++;
        }

        return ct.ret;
    }

    ty::TypeId TypeChecker::check_expr_index_(const ast::Expr& e) {
        // e.a = base, e.b = index expr
        ty::TypeId bt = check_expr_(e.a);
        ty::TypeId it = check_expr_(e.b);

        // index는 usize 권장(일단 usize만 허용)
        if (it != types_.builtin(ty::Builtin::kUSize) && !is_error_(it)) {
            err_(e.span, "index expression must be usize (v0 rule)");
        }

        const auto& t = types_.get(bt);
        if (t.kind == ty::Kind::kArray) {
            return t.elem;
        }

        err_(e.span, "indexing is only supported on array types (T[]) in v0");
        return types_.error();
    }

    // --------------------
    // if-expr / block-expr / loop-expr
    // --------------------
    ty::TypeId TypeChecker::check_expr_if_(const ast::Expr& e) {
        // v0: ExprKind::kIfExpr 의 slot 배치는 프로젝트 규칙에 따라 다를 수 있다.
        // 여기서는 a=cond, b=then expr(or block-expr id), c=else expr(or block-expr id) 로 가정한다.
        // (네 파서 구현에 맞춰 slot 매핑만 맞추면 됨)
        ty::TypeId ct = check_expr_(e.a);
        if (ct != types_.builtin(ty::Builtin::kBool) && !is_error_(ct)) {
            err_(e.span, "if-expr condition must be bool");
        }

        ty::TypeId t_then = check_expr_(e.b);
        ty::TypeId t_else = check_expr_(e.c);
        return unify_(t_then, t_else);
    }

    ty::TypeId TypeChecker::check_expr_block_(const ast::Expr& e) {
        // v0: block expr은 내부적으로 StmtId(블록) 를 참조하거나,
        // Expr 슬롯에 stmt id를 넣는 식으로 구현했을 수 있다.
        // 현재 Nodes.hpp에서는 Expr에 loop_body(StmtId)가 있고, block-expr용 stmt 슬롯은 없다.
        //
        // 따라서: "block-expr"은 일단 error로 두되,
        // 네 파서가 block-expr을 "StmtKind::kBlock"을 만들고 tail expr을 별도 노드에 두는 구조라면
        // 그 구조에 맞춰 여기만 채우면 된다.
        err_(e.span, "block-expr typing is not wired yet (need parser slot mapping to a block stmt)");
        return types_.error();
    }

    ty::TypeId TypeChecker::check_expr_loop_(const ast::Expr& e) {
        // loop expr은 보통 unit/never를 반환하거나,
        // body tail을 값으로 쓰는 언어라면 값이 될 수도 있음.
        // 현재는 타입 시스템에 unit이 없으니 error로 둔다.
        //
        // loop 헤더/이터레이터 타입 시스템은 다음 단계에서 확장:
        // - range
        // - iter protocol
        // - loop var binding type
        if (e.loop_body != ast::k_invalid_stmt) {
            sym_.push_scope();
            // loop var는 v0에서 unknown -> error로 등록
            if (e.loop_var.size() > 0) {
                sym_.insert(sema::SymbolKind::kVar, e.loop_var, types_.error(), e.span);
            }
            if (e.loop_iter != ast::k_invalid_expr) (void)check_expr_(e.loop_iter);
            check_stmt_(e.loop_body);
            sym_.pop_scope();
        }
        return types_.error();
    }

} // namespace gaupel::tyck