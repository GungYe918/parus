// compiler/src/tyck/type_check.cpp
#include <gaupel/tyck/TypeCheck.hpp>
#include <gaupel/syntax/TokenKind.hpp>
#include <gaupel/diag/Diagnostic.hpp>
#include <gaupel/diag/DiagCode.hpp>

#include <sstream>
#include <unordered_map>


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

    void TypeChecker::diag_(diag::Code code, Span sp) {
        if (!diag_bag_) return;
        diag::Diagnostic d(diag::Severity::kError, code, sp);
        diag_bag_->add(std::move(d));
    }

    void TypeChecker::diag_(diag::Code code, Span sp, std::string_view a0) {
        if (!diag_bag_) return;
        diag::Diagnostic d(diag::Severity::kError, code, sp);
        d.add_arg(a0);
        diag_bag_->add(std::move(d));
    }

    void TypeChecker::diag_(diag::Code code, Span sp, std::string_view a0, std::string_view a1) {
        if (!diag_bag_) return;
        diag::Diagnostic d(diag::Severity::kError, code, sp);
        d.add_arg(a0);
        d.add_arg(a1);
        diag_bag_->add(std::move(d));
    }

    void TypeChecker::diag_(diag::Code code, Span sp, std::string_view a0, std::string_view a1, std::string_view a2) {
        if (!diag_bag_) return;
        diag::Diagnostic d(diag::Severity::kError, code, sp);
        d.add_arg(a0);
        d.add_arg(a1);
        d.add_arg(a2);
        diag_bag_->add(std::move(d));
    }

    void TypeChecker::err_(Span sp, std::string msg) {
        // 1) TyckResult(errors)
        TyError e{};
        e.span = sp;
        e.message = msg;
        result_.errors.push_back(std::move(e));

        // NOTE:
        // - err_()는 저장용으로만 사용
        // - 사용자 출력은 항상 diag_(Code, args...)만 사용
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

    bool TypeChecker::fits_builtin_int_big_(const gaupel::num::BigInt& v, gaupel::ty::Builtin dst) {
        using B = gaupel::ty::Builtin;
        switch (dst) {
            case B::kI8:   return v.fits_i8();
            case B::kI16:  return v.fits_i16();
            case B::kI32:  return v.fits_i32();
            case B::kI64:  return v.fits_i64();
            case B::kI128: return v.fits_i128();

            case B::kU8:   return v.fits_u8();
            case B::kU16:  return v.fits_u16();
            case B::kU32:  return v.fits_u32();
            case B::kU64:  return v.fits_u64();
            case B::kU128: return v.fits_u128();

            // isize/usize는 타겟 포인터폭에 의존.
            // v0: 우선 64-bit로 가정하거나(네 프로젝트가 x86_64 우선이니까),
            // 추후 TargetConfig로 분리.
            case B::kISize: return v.fits_i64();
            case B::kUSize: return v.fits_u64();

            default: return false;
        }
    }

    bool TypeChecker::infer_int_value_of_expr_(ast::ExprId eid, num::BigInt& out) const {
        auto it = pending_int_expr_.find((uint32_t)eid);
        if (it != pending_int_expr_.end() && it->second.has_value) {
            out = it->second.value;
            return true;
        }

        const ast::Expr& e = ast_.expr(eid);
        if (e.kind == ast::ExprKind::kIntLit) {
            return num::BigInt::parse_dec(e.text, out);
        }

        // ident의 경우: sym pending에서 찾아온다
        if (e.kind == ast::ExprKind::kIdent) {
            auto sid = sym_.lookup(e.text);
            if (!sid) return false;
            auto it2 = pending_int_sym_.find(*sid);
            if (it2 != pending_int_sym_.end() && it2->second.has_value) {
                out = it2->second.value;
                return true;
            }
        }

        return false;
    }

    bool TypeChecker::resolve_infer_int_in_context_(ast::ExprId eid, ty::TypeId expected) {
        // expected는 builtin int여야 한다.
        const auto& et = types_.get(expected);
        if (et.kind != ty::Kind::kBuiltin) return false;

        // float 컨텍스트면 즉시 에러
        if (et.builtin == ty::Builtin::kF32 || et.builtin == ty::Builtin::kF64) {
            diag_(diag::Code::kIntToFloatNotAllowed, ast_.expr(eid).span, types_.to_string(expected));
            return false;
        }

        // int/uint/isize/usize만 허용
        auto is_int_builtin = [&](ty::Builtin b) -> bool {
            return b == ty::Builtin::kI8 || b == ty::Builtin::kI16 || b == ty::Builtin::kI32 ||
                b == ty::Builtin::kI64 || b == ty::Builtin::kI128 ||
                b == ty::Builtin::kU8 || b == ty::Builtin::kU16 || b == ty::Builtin::kU32 ||
                b == ty::Builtin::kU64 || b == ty::Builtin::kU128 ||
                b == ty::Builtin::kISize || b == ty::Builtin::kUSize;
        };
        if (!is_int_builtin(et.builtin)) return false;

        num::BigInt v;
        if (!infer_int_value_of_expr_(eid, v)) {
            // 값이 없으면(예: placeholder가 연산을 거쳐 값 추적이 불가) 컨텍스트만으로는 확정 불가
            diag_(diag::Code::kIntLiteralNeedsTypeContext, ast_.expr(eid).span);
            return false;
        }

        if (!fits_builtin_int_big_(v, et.builtin)) {
            diag_(diag::Code::kIntLiteralDoesNotFit, ast_.expr(eid).span,
                types_.to_string(expected), v.to_string(64));
            return false;
        }

        // 여기서 “expr 자체 타입”을 바꾸는 모델은 현재 TypeId 시스템 상 불가능(Expr에 TypeId 저장 X, 캐시만 있음)
        // 대신: ident라면 심볼 타입을 확정하고 pending_sym에도 기록한다.
        const ast::Expr& e = ast_.expr(eid);
        if (e.kind == ast::ExprKind::kIdent) {
            auto sid = sym_.lookup(e.text);
            if (sid) {
                // sym이 {integer}로 선언된 경우에만 확정 반영
                const auto& st = types_.get(sym_.symbol(*sid).declared_type);
                if (st.kind == ty::Kind::kBuiltin && st.builtin == ty::Builtin::kInferInteger) {
                    sym_.update_declared_type(*sid, expected);
                    auto it = pending_int_sym_.find(*sid);
                    if (it != pending_int_sym_.end()) {
                        it->second.resolved = true;
                        it->second.resolved_type = expected;
                    }
                }
            }
        }

        // expr pending에도 resolved 표시(진단/디버그 용)
        auto& pe = pending_int_expr_[(uint32_t)eid];
        pe.value = v;
        pe.has_value = true;
        pe.resolved = true;
        pe.resolved_type = expected;

        return true;
    }

    // --------------------
    // pass 2: check
    // --------------------
    void TypeChecker::second_pass_check_program_(ast::StmtId program_stmt) {
        check_stmt_(program_stmt);

        // ----------------------------------------
        // Finalize unresolved deferred integers:
        // - If an inferred integer "{integer}" is never consumed in a way that fixes the type,
        //   we pick the smallest signed integer type that fits (i8..i128).
        // - This keeps DX friendly and avoids leaving IR in an unresolved state.
        // ----------------------------------------
        for (auto& kv : pending_int_sym_) {
            const uint32_t sym_id = kv.first;
            PendingInt& pi = kv.second;

            if (!pi.has_value) continue;
            if (pi.resolved) continue;

            // pick smallest signed type
            ty::Builtin b = ty::Builtin::kI128;
            if      (pi.value.fits_i8())   b = ty::Builtin::kI8;
            else if (pi.value.fits_i16())  b = ty::Builtin::kI16;
            else if (pi.value.fits_i32())  b = ty::Builtin::kI32;
            else if (pi.value.fits_i64())  b = ty::Builtin::kI64;
            else                           b = ty::Builtin::kI128;

            pi.resolved = true;
            pi.resolved_type = types_.builtin(b);

            // NEW: SymbolTable에 확정 타입 반영
            sym_.update_declared_type(sym_id, pi.resolved_type);
        }
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
                check_stmt_var_(sid);
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

    void TypeChecker::check_stmt_var_(ast::StmtId sid) {
        ast::Stmt& s = ast_.stmt_mut(sid); // mutable access (AST에 타입 기록 위해)

        // ----------------------------------------
        // let: explicit type required (기존 로직 유지 + diag 강화)
        // ----------------------------------------
        if (!s.is_set) {
            if (s.type == ty::kInvalidType) {
                diag_(diag::Code::kVarDeclTypeAnnotationRequired, s.span);
                err_(s.span, "let requires an explicit declared type");
            }

            ty::TypeId init_t = ty::kInvalidType;
            if (s.init != ast::k_invalid_expr) {
                init_t = check_expr_(s.init);

                // 컨텍스트 해소: let x: i32 = 123; 같은 케이스에서 RHS가 {integer}면 여기서 확정
                if (s.type != ty::kInvalidType) {
                    const auto& st = types_.get(s.type);
                    const auto& it = types_.get(init_t);
                    if (it.kind == ty::Kind::kBuiltin && it.builtin == ty::Builtin::kInferInteger) {
                        (void)resolve_infer_int_in_context_(s.init, s.type);
                        init_t = check_expr_(s.init); // 캐시된 타입이 바뀌진 않지만, 논리적으로 재평가 의도
                    }
                }

                if (s.type != ty::kInvalidType && !can_assign_(s.type, init_t)) {
                    diag_(diag::Code::kTypeLetInitMismatch, s.span,
                        s.name, types_.to_string(s.type), types_.to_string(init_t));
                    err_(s.span, "let init mismatch");
                }
            }

            ty::TypeId vt = (s.type == ty::kInvalidType) ? types_.error() : s.type;

            auto ins = sym_.insert(sema::SymbolKind::kVar, s.name, vt, s.span);
            if (!ins.ok) {
                if (ins.is_duplicate) {
                    diag_(diag::Code::kDuplicateDecl, s.span, s.name);
                    err_(s.span, "duplicate symbol (var): " + std::string(s.name));
                } else if (ins.is_shadowing) {
                    // 정책은 나중에 옵션화 가능. 일단 경고/에러는 name-resolve 단계와 정렬하는 편이 좋다.
                    diag_(diag::Code::kShadowing, s.span, s.name);
                }
            }

            // (선택) let의 경우도 AST에 vt를 확정 기록 (이미 s.type이지만, invalid였으면 error로)
            s.type = vt;
            return;
        }

        // ----------------------------------------
        // set: type inference declaration
        //   - must have initializer
        //   - must NOT infer from null (set x = null; 금지)
        //   - infer = RHS type (v0)
        //   - record inferred type into AST (s.type)
        // ----------------------------------------
        if (s.init == ast::k_invalid_expr) {
            // 파서가 막았더라도 방어
            err_(s.span, "set requires initializer expression");
            s.type = types_.error();
            return;
        }

        // (A) RHS 타입 계산
        ty::TypeId rhs = check_expr_(s.init);

        // (B) set x = null; 금지 (정확히 null literal/Null 타입)
        //     - rhs 타입이 null인 것만으로도 걸 수 있지만,
        //       더 정확한 메시지/정책 위해 expr kind도 같이 확인.
        const ast::Expr& init_e = ast_.expr(s.init);
        const bool rhs_is_null_lit = (init_e.kind == ast::ExprKind::kNullLit);
        if (rhs_is_null_lit || rhs == types_.builtin(ty::Builtin::kNull)) {
            diag_(diag::Code::kSetCannotInferFromNull, s.span, s.name);
            err_(s.span, "set cannot infer type from null (use let with explicit optional type)");
            rhs = types_.error(); // 계속 진행용
        }

        // (C) 추론 타입 확정
        //     - 정수 리터럴은 Rust처럼 "{integer}" placeholder로 보류한다.
        //     - 실제 i8~i128 선택/확정은 "소비될 때" 수행한다.
        ty::TypeId inferred = rhs;

        // (D) 현재 스코프에 선언으로 삽입 (여기서 SymbolId를 얻어야 pending을 sym-id로 저장 가능)
        auto ins = sym_.insert(sema::SymbolKind::kVar, s.name, inferred, s.span);
        if (!ins.ok) {
            if (ins.is_duplicate) {
                diag_(diag::Code::kDuplicateDecl, s.span, s.name);
                err_(s.span, "duplicate symbol (var): " + std::string(s.name));
                s.type = types_.error();
                return;
            } else if (ins.is_shadowing) {
                // 섀도잉 정책은 옵션화 가능.
                // 지금은 일단 경고로 기록(또는 프로젝트 정책이 "불가"면 kShadowingNotAllowed로 바꿔)
                diag_(diag::Code::kShadowing, s.span, s.name);
            }
        }

        // (E) set x = <int literal> 이면: declared_type을 "{integer}"로 바꾸고 pending을 sym-id로 저장
        if (init_e.kind == ast::ExprKind::kIntLit) {
            num::BigInt v;
            if (!num::BigInt::parse_dec(init_e.text, v)) {
                diag_(diag::Code::kIntLiteralInvalid, init_e.span, init_e.text);
                err_(init_e.span, "invalid integer literal");
                inferred = types_.error();
                sym_.update_declared_type(ins.symbol_id, inferred);
            } else {
                inferred = types_.builtin(ty::Builtin::kInferInteger);
                sym_.update_declared_type(ins.symbol_id, inferred);

                PendingInt pi{};
                pi.value = v;
                pi.has_value = true;
                pi.resolved = false;
                pi.resolved_type = ty::kInvalidType;

                pending_int_sym_[ins.symbol_id] = pi;
            }
        }

        if (inferred == ty::kInvalidType) inferred = types_.error();

        // (F) AST에 “추론된 타입” 기록 (후속 패스/IR 친화)
        s.type = inferred;
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
            // 이제 unit 타입이 있으므로:
            // - fn ret == unit 이면 return; 허용
            // - 아니면 에러
            if (rt == types_.builtin(ty::Builtin::kUnit)) {
                return;
            }
            err_(s.span, "return expression is required (function does not return unit)");
            return;
        }

        ty::TypeId v = check_expr_(s.expr);
        if (!can_assign_(rt, v)) {
            diag_(diag::Code::kTypeMismatch, s.span, types_.to_string(rt), types_.to_string(v));

            err_(s.span, "return mismatch"); // 저장만 수행, 출력은 위 diag_ 하나로 종결
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

            case ast::ExprKind::kIntLit: {
                // v1: Rust-like unsuffixed integer -> internal placeholder "{integer}"
                // 실제 i8~i128 확정은 "소비될 때" 수행한다.
                t = types_.builtin(ty::Builtin::kInferInteger);

                // expr-level pending: 모든 int literal은 expr 단위로 값을 보존한다.
                num::BigInt v;
                if (!num::BigInt::parse_dec(e.text, v)) {
                    diag_(diag::Code::kIntLiteralInvalid, e.span, e.text);
                    err_(e.span, "invalid integer literal");
                } else {
                    PendingInt pi{};
                    pi.value = v;
                    pi.has_value = true;
                    pi.resolved = false;
                    pi.resolved_type = ty::kInvalidType;
                    pending_int_expr_[(uint32_t)eid] = pi;
                }
                break;
            }

            case ast::ExprKind::kFloatLit: {
                // v1: suffix-based float typing
                // Accept:
                //  - 1.0f32 / 1.0f64 / 1.0f128
                //  - default: f64
                std::string_view tx = e.text;

                auto ends_with = [](std::string_view s, std::string_view suf) -> bool {
                    return s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf;
                };

                if (ends_with(tx, "f32"))  t = types_.builtin(ty::Builtin::kF32);
                else if (ends_with(tx, "f64"))  t = types_.builtin(ty::Builtin::kF64);
                else if (ends_with(tx, "f128")) t = types_.builtin(ty::Builtin::kF128);
                else t = types_.builtin(ty::Builtin::kF64);

                break;
            }

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
                    diag_(diag::Code::kUndefinedName, e.span, e.text);
                    err_(e.span, "unknown identifier"); // 저장용
                    t = types_.error();
                } else {
                    t = sym_.symbol(*id).declared_type;
                    if (t == ty::kInvalidType) t = types_.error();

                    // expr-level pending 연결:
                    // ident의 타입이 {integer}이고 sym pending에 값이 있으면,
                    // 이 식 자체도 pending value를 "가진 것"으로 취급해 컨텍스트 해소를 가능하게 한다.
                    const auto& tt = types_.get(t);
                    if (tt.kind == ty::Kind::kBuiltin && tt.builtin == ty::Builtin::kInferInteger) {
                        auto it = pending_int_sym_.find(*id);
                        if (it != pending_int_sym_.end() && it->second.has_value) {
                            pending_int_expr_[(uint32_t)eid] = it->second;
                        }
                    }
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

        // never -> anything (bottom type)
        if (src == types_.builtin(ty::Builtin::kNever)) return true;
        // only never can be assigned into never
        if (dst == types_.builtin(ty::Builtin::kNever)) return src == dst;

        // null -> T? 허용
        if (is_null_(src) && is_optional_(dst)) return true;

        // -------------------------------------------------
        // "{integer}" placeholder rules (Rust-like)
        // - placeholder can be assigned ONLY into an integer type (signed/unsigned),
        //   and only if the literal value fits (checked at resolution site).
        // - placeholder -> float is NOT allowed (no implicit int->float).
        // -------------------------------------------------
        const auto& dt = types_.get(dst);
        const auto& st = types_.get(src);

        auto is_int_builtin = [&](ty::Builtin b) -> bool {
            return b == ty::Builtin::kI8 || b == ty::Builtin::kI16 || b == ty::Builtin::kI32 ||
                b == ty::Builtin::kI64 || b == ty::Builtin::kI128 ||
                b == ty::Builtin::kU8 || b == ty::Builtin::kU16 || b == ty::Builtin::kU32 ||
                b == ty::Builtin::kU64 || b == ty::Builtin::kU128 ||
                b == ty::Builtin::kISize || b == ty::Builtin::kUSize;
        };

        auto is_float_builtin = [&](ty::Builtin b) -> bool {
            return b == ty::Builtin::kF32 || b == ty::Builtin::kF64;
        };

        const bool dst_is_builtin = (dt.kind == ty::Kind::kBuiltin);
        const bool src_is_builtin = (st.kind == ty::Kind::kBuiltin);

        if (dst_is_builtin && src_is_builtin &&
            st.builtin == ty::Builtin::kInferInteger) {

            if (is_float_builtin(dt.builtin)) {
                return false;
            }

            if (!is_int_builtin(dt.builtin)) {
                return false;
            }

            // 값 fit 여부는 소비 지점에서 resolve할 때 체크한다.
            return true;
        }

        return false;
    }

    ty::TypeId TypeChecker::unify_(ty::TypeId a, ty::TypeId b) {
        if (is_error_(a) || is_error_(b)) return types_.error();
        if (a == b) return a;

        if (a == types_.builtin(ty::Builtin::kNever)) return b;
        if (b == types_.builtin(ty::Builtin::kNever)) return a;

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
                diag_(diag::Code::kBorrowOperandMustBePlace, e.span);
                err_(e.span, "borrow needs place");
                return types_.error();
            }
            if (fn_ctx_.is_pure || fn_ctx_.is_comptime) {
                diag_(diag::Code::kTypeBorrowNotAllowedInPureComptime, e.span);
                err_(e.span, "borrow not allowed in pure/comptime");
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

        auto is_builtin = [&](ty::TypeId t) -> bool {
            return t != ty::kInvalidType && types_.get(t).kind == ty::Kind::kBuiltin;
        };

        auto builtin_of = [&](ty::TypeId t) -> ty::Builtin {
            return types_.get(t).builtin;
        };

        auto is_infer_int = [&](ty::TypeId t) -> bool {
            return is_builtin(t) && builtin_of(t) == ty::Builtin::kInferInteger;
        };

        auto is_float = [&](ty::TypeId t) -> bool {
            if (!is_builtin(t)) return false;
            auto b = builtin_of(t);
            return b == ty::Builtin::kF32 || b == ty::Builtin::kF64 || b == ty::Builtin::kF128;
        };

        auto is_int = [&](ty::TypeId t) -> bool {
            if (!is_builtin(t)) return false;
            auto b = builtin_of(t);
            return b == ty::Builtin::kI8 || b == ty::Builtin::kI16 || b == ty::Builtin::kI32 ||
                b == ty::Builtin::kI64 || b == ty::Builtin::kI128 ||
                b == ty::Builtin::kU8 || b == ty::Builtin::kU16 || b == ty::Builtin::kU32 ||
                b == ty::Builtin::kU64 || b == ty::Builtin::kU128 ||
                b == ty::Builtin::kISize || b == ty::Builtin::kUSize;
        };

        // == / != : null 비교 제한 유지
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

        // 산술: + - * / %
        if (e.op == K::kPlus || e.op == K::kMinus || e.op == K::kStar || e.op == K::kSlash || e.op == K::kPercent) {
            // float 컨텍스트에 {integer}가 섞이면 즉시 에러 (암시적 int->float 금지)
            if ((is_float(lt) && is_infer_int(rt)) || (is_float(rt) && is_infer_int(lt))) {
                err_(e.span, "cannot use deferred integer '{integer}' in float arithmetic (no implicit int->float)");
                return types_.error();
            }

            // {integer} + concrete int => {integer}를 concrete 쪽 타입으로 해소
            if (is_infer_int(lt) && is_int(rt)) {
                if (!resolve_infer_int_in_context_(e.a, rt)) return types_.error();
                // 결과 타입은 rhs에 맞춘다
                return rt;
            }
            if (is_infer_int(rt) && is_int(lt)) {
                if (!resolve_infer_int_in_context_(e.b, lt)) return types_.error();
                return lt;
            }

            // {integer} + {integer} => 아직 결정 불가
            if (is_infer_int(lt) && is_infer_int(rt)) {
                return types_.builtin(ty::Builtin::kInferInteger);
            }

            // 이종 산술 금지 (no promotion)
            if (lt != rt && !is_error_(lt) && !is_error_(rt)) {
                err_(e.span, "binary arithmetic requires both operands to have the same type (no implicit promotion)");
                return types_.error();
            }

            return lt;
        }

        // 비교 < <= > >= : 동일 타입만
        if (e.op == K::kLt || e.op == K::kLtEq || e.op == K::kGt || e.op == K::kGtEq) {
            // {integer} 비교는 타입 컨텍스트 필요 (v1: 금지)
            if (is_infer_int(lt) || is_infer_int(rt)) {
                err_(e.span, "comparison with deferred integer '{integer}' needs an explicit integer type context");
                return types_.builtin(ty::Builtin::kBool);
            }

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

        // RHS가 {integer}면, LHS 타입 컨텍스트로 해소 시도
        {
            const auto& st = types_.get(rt);
            if (st.kind == ty::Kind::kBuiltin && st.builtin == ty::Builtin::kInferInteger) {
                (void)resolve_infer_int_in_context_(e.b, lt);
                rt = check_expr_(e.b);
            }
        }

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
            diag_(diag::Code::kTypeNotCallable, e.span, types_.to_string(callee_t));
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
            diag_(diag::Code::kTypeArgCountMismatch, e.span,
                std::to_string(ct.param_count), std::to_string(positional_count));
            err_(e.span, "argument count mismatch");
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

                // 컨텍스트 해소: arg가 {integer}면 param 타입으로 해소 시도
                const auto& st = types_.get(at);
                if (st.kind == ty::Kind::kBuiltin && st.builtin == ty::Builtin::kInferInteger) {
                    if (a.expr != ast::k_invalid_expr) {
                        (void)resolve_infer_int_in_context_(a.expr, expected);
                        at = check_expr_(a.expr);
                    }
                }

                if (!can_assign_(expected, at)) {
                    diag_(diag::Code::kTypeArgTypeMismatch, a.span,
                        std::to_string(pi), types_.to_string(expected), types_.to_string(at));
                    err_(a.span, "argument type mismatch");
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

        return types_.builtin(ty::Builtin::kNever);
    }

} // namespace gaupel::tyck