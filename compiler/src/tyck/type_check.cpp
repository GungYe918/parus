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
        // -----------------------------
        // HARD RESET (매 호출 독립 보장)
        // -----------------------------
        result_ = TyckResult{};
        loop_stack_.clear();
        fn_ctx_ = FnCtx{};
        pending_int_sym_.clear();
        pending_int_expr_.clear();
        sym_is_mut_.clear();

        // sym_ “완전 초기화”
        // (기존에 clear() API가 없으니 재생성하는 게 가장 안전)
        sym_ = sema::SymbolTable{};

        // expr type cache: AST exprs 크기에 맞춰 리셋
        expr_type_cache_.assign(ast_.exprs().size(), ty::kInvalidType);
        result_.expr_types = expr_type_cache_; // 결과 벡터도 동일 크기로 시작

        // string literal 타입(필요 시)
        if (string_type_ == ty::kInvalidType) {
            // 지금은 string builtin이 없으니 placeholder로 error() 사용
            string_type_ = types_.error();
        }

        // ------------------------------------------
        // Sanity: program은 Block이어야 한다 (정책)
        // ------------------------------------------
        if (program_stmt == ast::k_invalid_stmt) {
            result_.ok = false;
            return result_;
        }

        const ast::Stmt& root = ast_.stmt(program_stmt);
        if (root.kind != ast::StmtKind::kBlock) {
            if (diag_bag_) diag_(diag::Code::kTopLevelMustBeBlock, root.span);
            result_.ok = false;
            return result_;
        }

        // ---------------------------------------------------------
        // PASS 1: Top-level decl precollect (mutual recursion 지원)
        // - 전역 스코프에 "함수 시그니처 타입(ty::Kind::kFn)"을 먼저 등록한다.
        // - 기존 check_program() 내부의 "invalid 타입으로 insert"하던 루프가
        //   거의 모든 TypeNotCallable 증상의 원인이었으므로 제거하고,
        //   이미 구현된 first_pass_collect_top_level_()를 정식으로 사용한다.
        // ---------------------------------------------------------
        first_pass_collect_top_level_(program_stmt);

        // ---------------------------------------------------------
        // PASS 2: 실제 타입체크
        // - top-level block은 "scope 생성 없이" 자식만 순회한다.
        //   (중요: check_stmt_block_은 scope를 push하므로,
        //    root를 check_stmt_로 보내면 top-level이 새 스코프가 되어
        //    PASS1에서 등록한 전역 심볼이 가려질 수 있다.)
        // ---------------------------------------------------------
        for (uint32_t i = 0; i < root.stmt_count; ++i) {
            const ast::StmtId child_id = ast_.stmt_children()[root.stmt_begin + i];
            check_stmt_(child_id);
            // 에러가 나도 계속 진행(정책)
        }

        // 결과 반영
        result_.expr_types = expr_type_cache_;
        return result_;
    }

    // --------------------
    // errors
    // --------------------

    void TypeChecker::diag_(diag::Code code, Span sp) {
        if (!diag_bag_) return;
        result_.ok = false;
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
            diag_(diag::Code::kTopLevelMustBeBlock, prog.span);
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
                            diag_(diag::Code::kTypeParamTypeRequired, p.span, p.name);
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
                    diag_(diag::Code::kDuplicateDecl, s.span, s.name);
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
        if (eid == ast::k_invalid_expr) return false;

        // expected는 builtin int여야 한다.
        const auto& et = types_.get(expected);
        if (et.kind != ty::Kind::kBuiltin) return false;

        // float 컨텍스트면 즉시 에러 (암시적 int->float 금지)
        if (et.builtin == ty::Builtin::kF32 || et.builtin == ty::Builtin::kF64 || et.builtin == ty::Builtin::kF128) {
            diag_(diag::Code::kIntToFloatNotAllowed, ast_.expr(eid).span, types_.to_string(expected));
            return false;
        }

        auto is_int_builtin = [&](ty::Builtin b) -> bool {
            return b == ty::Builtin::kI8 || b == ty::Builtin::kI16 || b == ty::Builtin::kI32 ||
                b == ty::Builtin::kI64 || b == ty::Builtin::kI128 ||
                b == ty::Builtin::kU8 || b == ty::Builtin::kU16 || b == ty::Builtin::kU32 ||
                b == ty::Builtin::kU64 || b == ty::Builtin::kU128 ||
                b == ty::Builtin::kISize || b == ty::Builtin::kUSize;
        };
        if (!is_int_builtin(et.builtin)) return false;

        const ast::Expr& e = ast_.expr(eid);

        // ------------------------------------------------------------
        // (1) 합성 표현식: expected를 "아래로" 전파해서 내부 {integer}를 확정한다.
        //     - if-expr: then/else로 전파
        //     - ternary: b/c로 전파
        //     - block-expr: tail로 전파
        //
        // 여기서 중요한 점:
        // - 이 expr 자체에서 "정수 literal 값"을 뽑으려고 하면 안 된다.
        // - 내부 리터럴들이 fit+resolve만 되면 상위 expr은 자연히 expected 타입으로 수렴한다.
        // ------------------------------------------------------------
        auto mark_resolved_here = [&](bool has_value, const num::BigInt& v) {
            auto& pe = pending_int_expr_[(uint32_t)eid];
            if (has_value) {
                pe.value = v;
                pe.has_value = true;
            }
            pe.resolved = true;
            pe.resolved_type = expected;
        };

        switch (e.kind) {
            case ast::ExprKind::kIfExpr: {
                bool ok_then = (e.b != ast::k_invalid_expr) ? resolve_infer_int_in_context_(e.b, expected) : true;
                bool ok_else = (e.c != ast::k_invalid_expr) ? resolve_infer_int_in_context_(e.c, expected) : true;

                if (ok_then && ok_else) {
                    // if-expr 자체는 "값"을 직접 가지지 않으므로 value는 기록하지 않는다.
                    mark_resolved_here(/*has_value=*/false, num::BigInt{});
                    return true;
                }
                // branch 중 하나라도 해소 실패하면: 여기서 "컨텍스트 없음" 진단은 내지 말고 그냥 실패 리턴.
                // (실제 원인은 내부에서 fit 실패/unknown 등의 진단으로 이미 찍힌다.)
                return false;
            }

            case ast::ExprKind::kTernary: {
                bool ok_b = (e.b != ast::k_invalid_expr) ? resolve_infer_int_in_context_(e.b, expected) : true;
                bool ok_c = (e.c != ast::k_invalid_expr) ? resolve_infer_int_in_context_(e.c, expected) : true;

                if (ok_b && ok_c) {
                    mark_resolved_here(/*has_value=*/false, num::BigInt{});
                    return true;
                }
                return false;
            }

            case ast::ExprKind::kBlockExpr: {
                // mapping assumption in your code:
                // - e.a: StmtId of block
                // - e.b: tail ExprId (optional)
                if (e.b != ast::k_invalid_expr) {
                    bool ok_tail = resolve_infer_int_in_context_(e.b, expected);
                    if (ok_tail) {
                        mark_resolved_here(/*has_value=*/false, num::BigInt{});
                        return true;
                    }
                    return false;
                }
                // tail이 없으면 null로 수렴하므로 integer expected로는 해소 불가
                return false;
            }

            default:
                break;
        }

        // ------------------------------------------------------------
        // (2) 리프/값 추적 가능한 케이스: IntLit / Ident({integer})
        // ------------------------------------------------------------
        num::BigInt v;
        if (!infer_int_value_of_expr_(eid, v)) {
            // 값이 없으면(예: 연산을 거쳐 값 추적이 불가) 컨텍스트만으로는 확정 불가
            // 단, 위의 합성 expr들은 여기로 오지 않게 했으니, 이 진단은 "진짜 리프 해소 실패"에만 뜬다.
            diag_(diag::Code::kIntLiteralNeedsTypeContext, e.span);
            return false;
        }

        if (!fits_builtin_int_big_(v, et.builtin)) {
            diag_(diag::Code::kIntLiteralDoesNotFit, e.span,
                types_.to_string(expected), v.to_string(64));
            return false;
        }

        // ident라면 심볼 타입 확정 반영
        if (e.kind == ast::ExprKind::kIdent) {
            auto sid = sym_.lookup(e.text);
            if (sid) {
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

        // expr pending resolved 표시
        {
            auto& pe = pending_int_expr_[(uint32_t)eid];
            pe.value = v;
            pe.has_value = true;
            pe.resolved = true;
            pe.resolved_type = expected;
        }

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
                if (s.expr != ast::k_invalid_expr) (void)check_expr_(s.expr, Slot::kDiscard);
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

            case ast::StmtKind::kBreak: {
                // break expr? 는 loop 결과 타입을 만든다.
                if (!in_loop_()) {
                    diag_(diag::Code::kBreakOutsideLoop, s.span);
                    err_(s.span, "break outside loop");
                    if (s.expr != ast::k_invalid_expr) (void)check_expr_(s.expr, Slot::kValue);
                    return;
                }

                if (s.expr == ast::k_invalid_expr) {
                    // break;
                    note_break_(types_.builtin(ty::Builtin::kNull), /*is_value_break=*/false);
                    return;
                }

                // break expr;
                ty::TypeId bt = check_expr_(s.expr, Slot::kValue);
                note_break_(bt, /*is_value_break=*/true);
                return;
            }

            case ast::StmtKind::kContinue: {
                if (!in_loop_()) {
                    diag_(diag::Code::kContinueOutsideLoop, s.span);
                    err_(s.span, "continue outside loop");
                }
                return;
            }

            case ast::StmtKind::kSwitch:
                check_stmt_switch_(s);
                return;

            case ast::StmtKind::kFnDecl:
                check_stmt_fn_decl_(s);
                return;

            case ast::StmtKind::kUse:
                return;

            case ast::StmtKind::kError:
                return;
        }
    }

    
    void TypeChecker::check_stmt_block_(const ast::Stmt& s) {
        // s.kind == kBlock 가정
        // 블록 진입 시 새 스코프 생성
        const uint32_t scope_id = sym_.push_scope();
        (void)scope_id; // 디버그용이면 남겨두기

        // s.stmt_begin/count 는 ast_.stmt_children()의 slice
        const auto& children = ast_.stmt_children();

        for (uint32_t i = 0; i < s.stmt_count; ++i) {
            const ast::StmtId cid = children[s.stmt_begin + i];
            check_stmt_(cid);
            // 에러가 나도 계속 진행할지 정책(여기선 계속)
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
                    const auto& it = types_.get(init_t);
                    if (it.kind == ty::Kind::kBuiltin && it.builtin == ty::Builtin::kInferInteger) {
                        (void)resolve_infer_int_in_context_(s.init, s.type);
                        init_t = check_expr_(s.init); // 논리적 재평가 의도
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
                    diag_(diag::Code::kShadowing, s.span, s.name);
                }
            }

            // NEW: mut tracking (let mut / let)
            if (ins.ok) {
                sym_is_mut_[ins.symbol_id] = s.is_mut;
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

        // (B) set x = null; 금지
        const ast::Expr& init_e = ast_.expr(s.init);
        const bool rhs_is_null_lit = (init_e.kind == ast::ExprKind::kNullLit);
        if (rhs_is_null_lit || rhs == types_.builtin(ty::Builtin::kNull)) {
            diag_(diag::Code::kSetCannotInferFromNull, s.span, s.name);
            err_(s.span, "set cannot infer type from null (use let with explicit optional type)");
            rhs = types_.error(); // 계속 진행용
        }

        // (C) 추론 타입 확정
        ty::TypeId inferred = rhs;

        // (D) 현재 스코프에 선언으로 삽입
        auto ins = sym_.insert(sema::SymbolKind::kVar, s.name, inferred, s.span);
        if (!ins.ok) {
            if (ins.is_duplicate) {
                diag_(diag::Code::kDuplicateDecl, s.span, s.name);
                err_(s.span, "duplicate symbol (var): " + std::string(s.name));
                s.type = types_.error();
                return;
            } else if (ins.is_shadowing) {
                diag_(diag::Code::kShadowing, s.span, s.name);
            }
        }

        // NEW: mut tracking (set mut / set)
        if (ins.ok) {
            sym_is_mut_[ins.symbol_id] = s.is_mut;
        }

        // (E) set x = <int literal> 이면: declared_type을 "{integer}"로 바꾸고 pending을 sym-id로 저장
        if (init_e.kind == ast::ExprKind::kIntLit) {
            num::BigInt v;
            if (!num::BigInt::parse_dec(init_e.text, v)) {
                diag_(diag::Code::kIntLiteralInvalid, init_e.span, init_e.text);
                err_(init_e.span, "invalid integer literal");
                inferred = types_.error();
                if (ins.ok) sym_.update_declared_type(ins.symbol_id, inferred);
            } else {
                inferred = types_.builtin(ty::Builtin::kInferInteger);
                if (ins.ok) sym_.update_declared_type(ins.symbol_id, inferred);

                PendingInt pi{};
                pi.value = v;
                pi.has_value = true;
                pi.resolved = false;
                pi.resolved_type = ty::kInvalidType;

                if (ins.ok) pending_int_sym_[ins.symbol_id] = pi;
            }
        }

        if (inferred == ty::kInvalidType) inferred = types_.error();

        // (F) AST에 “추론된 타입” 기록
        s.type = inferred;
    }

    void TypeChecker::check_stmt_if_(const ast::Stmt& s) {
        // s.expr = cond, s.a = then block, s.b = else block
        if (s.expr != ast::k_invalid_expr) {
            ty::TypeId ct = check_expr_(s.expr);
            if (ct != types_.builtin(ty::Builtin::kBool) && !is_error_(ct)) {
                diag_(diag::Code::kTypeCondMustBeBool, ast_.expr(s.expr).span, types_.to_string(ct));
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
                diag_(diag::Code::kTypeCondMustBeBool, ast_.expr(s.expr).span, types_.to_string(ct));
                err_(s.span, "while condition must be bool");
            }
        }
        if (s.a != ast::k_invalid_stmt) check_stmt_(s.a);
    }

    void TypeChecker::check_stmt_return_(const ast::Stmt& s) {
        if (!fn_ctx_.in_fn) {
            diag_(diag::Code::kTypeReturnOutsideFn, s.span);
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

            diag_(diag::Code::kTypeReturnExprRequired, s.span);
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
                diag_(diag::Code::kTypeDuplicateParam, p.span, p.name);
            }

            if (p.has_default && p.default_expr != ast::k_invalid_expr) {
                ty::TypeId dt = check_expr_(p.default_expr);
                if (!can_assign_(pt, dt)) {
                    std::ostringstream oss;
                    oss << "default value type mismatch for param '" << p.name
                        << "': expected " << types_.to_string(pt)
                        << ", got " << types_.to_string(dt);
                    diag_(diag::Code::kTypeParamDefaultMismatch, p.span,
                            p.name, types_.to_string(pt), types_.to_string(dt));
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
        // 3.5) return 누락 검사 (v0: 구조 기반 보수 분석)
        // ----------------------------
        auto is_unit = [&](ty::TypeId t) -> bool {
            return t == types_.builtin(ty::Builtin::kUnit);
        };
        auto is_never = [&](ty::TypeId t) -> bool {
            return t == types_.builtin(ty::Builtin::kNever);
        };

        // 반환 타입이 unit/never면 "끝까지 도달" 허용
        const ty::TypeId fn_ret = fn_ctx_.ret;

        if (!is_unit(fn_ret) && !is_never(fn_ret)) {
            // body가 항상 return 하는지 검사
            auto stmt_always_returns = [&](auto&& self, ast::StmtId sid) -> bool {
                if (sid == ast::k_invalid_stmt) return false;
                const ast::Stmt& st = ast_.stmt(sid);

                switch (st.kind) {
                    case ast::StmtKind::kReturn:
                        return true;

                    case ast::StmtKind::kBlock: {
                        // v0 정책: block의 마지막 stmt가 항상 return이면 block이 항상 return
                        if (st.stmt_count == 0) return false;
                        const auto& children = ast_.stmt_children();
                        const ast::StmtId last = children[st.stmt_begin + (st.stmt_count - 1)];
                        return self(self, last);
                    }

                    case ast::StmtKind::kIf: {
                        // then/else 둘 다 return해야 if가 return
                        if (st.a == ast::k_invalid_stmt) return false;
                        if (st.b == ast::k_invalid_stmt) return false;
                        return self(self, st.a) && self(self, st.b);
                    }

                    // while/loop/switch 등은 v0에서 보수적으로 false
                    case ast::StmtKind::kWhile:
                    case ast::StmtKind::kSwitch:
                        return false;

                    default:
                        return false;
                }
            };

            const bool ok_all_paths = stmt_always_returns(stmt_always_returns, s.a);
            if (!ok_all_paths) {
                // 여기서 “return 누락” 진단
                // (diag code는 새로 만드는 게 정석: kMissingReturn)
                diag_(diag::Code::kMissingReturn, s.span, s.name);
                err_(s.span, "missing return on some control path");
            }
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
    ty::TypeId TypeChecker::check_expr_(ast::ExprId eid, Slot slot) {
        if (eid == ast::k_invalid_expr) return types_.error();
        if (eid >= expr_type_cache_.size()) return types_.error();

        const ast::Expr& e = ast_.expr(eid);

        // NOTE(slot-sensitive caching)
        // - 일부 expr는 "Value vs Discard" 컨텍스트에 따라 진단/타입 규칙이 달라질 수 있다.
        // - 특히 block-expr는 Slot::kValue에서 tail이 없으면 에러를 내야 한다.
        //   그런데 Discard 컨텍스트에서 먼저 체크되며 캐시에 타입이 박히면,
        //   나중에 Value 컨텍스트에서 재방문해도 진단이 발생하지 않는 버그가 생긴다.
        //
        // 정책:
        // - Slot에 의존하는 expr(kind)가 있다면, 그 expr는 "Value에서만 캐시"하거나
        //   아예 캐시를 우회해서 컨텍스트별로 재검사한다.
        const bool slot_sensitive =
            (e.kind == ast::ExprKind::kBlockExpr); // v0: currently only block-expr depends on slot

        // memoized
        // - slot-sensitive expr는 Value에서만 캐시를 신뢰한다.
        if (!slot_sensitive || slot == Slot::kValue) {
            if (expr_type_cache_[eid] != ty::kInvalidType) {
                return expr_type_cache_[eid];
            }
        }

        ty::TypeId t = types_.error();

        switch (e.kind) {
            case ast::ExprKind::kError:
                t = types_.error();
                break;

            case ast::ExprKind::kIntLit: {
                t = types_.builtin(ty::Builtin::kInferInteger);

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
                std::string_view tx = e.text;

                auto ends_with = [](std::string_view s, std::string_view suf) -> bool {
                    return s.size() >= suf.size() && s.substr(s.size() - suf.size()) == suf;
                };

                if (ends_with(tx, "f32"))       t = types_.builtin(ty::Builtin::kF32);
                else if (ends_with(tx, "f64"))  t = types_.builtin(ty::Builtin::kF64);
                else if (ends_with(tx, "f128")) t = types_.builtin(ty::Builtin::kF128);
                else                            t = types_.builtin(ty::Builtin::kF64);
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
                    err_(e.span, "unknown identifier");
                    t = types_.error();
                } else {
                    t = sym_.symbol(*id).declared_type;
                    if (t == ty::kInvalidType) t = types_.error();

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
                diag_(diag::Code::kTypeUnresolvedHole, e.span);
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
                t = check_expr_if_(e, slot);
                break;

            case ast::ExprKind::kBlockExpr:
                t = check_expr_block_(e, slot);
                break;

            case ast::ExprKind::kLoop:
                t = check_expr_loop_(e, slot);
                break;

            case ast::ExprKind::kCast:
                t = check_expr_cast_(e);
                break;
        }

        // caching policy:
        // - slot-sensitive expr는 Value 컨텍스트에서만 캐시한다.
        //   (Discard에서의 결과를 캐시하면, 나중에 Value에서 필요한 진단이 누락될 수 있다.)
        if (!slot_sensitive || slot == Slot::kValue) {
            expr_type_cache_[eid] = t;
        }

        return t;
    }

    ty::TypeId TypeChecker::check_expr_(ast::ExprId eid) {
        return check_expr_(eid, Slot::kValue);
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

    void TypeChecker::note_break_(ty::TypeId t, bool is_value_break) {
        if (loop_stack_.empty()) return;

        LoopCtx& lc = loop_stack_.back();
        lc.has_any_break = true;

        if (!is_value_break) {
            lc.has_null_break = true;
            return;
        }

        lc.has_value_break = true;

        if (lc.joined_value == ty::kInvalidType) {
            lc.joined_value = t;
        } else {
            lc.joined_value = unify_(lc.joined_value, t);
        }
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

    // v0: place(ident/index)에서 "근원 로컬 심볼"을 최대한 뽑는다.
    // - ident      => sym id
    // - index(a,i) => a가 ident면 그 sym id (v0 보수 규칙)
    std::optional<uint32_t> TypeChecker::root_place_symbol_(ast::ExprId place) const {
        if (place == ast::k_invalid_expr) return std::nullopt;
        const ast::Expr& e = ast_.expr(place);

        if (e.kind == ast::ExprKind::kIdent) {
            auto sid = sym_.lookup(e.text);
            if (!sid) return std::nullopt;
            return *sid;
        }

        if (e.kind == ast::ExprKind::kIndex) {
            // 가정: e.a = base, e.b = index
            const ast::Expr& base = ast_.expr(e.a);
            if (base.kind == ast::ExprKind::kIdent) {
                auto sid = sym_.lookup(base.text);
                if (!sid) return std::nullopt;
                return *sid;
            }
        }

        return std::nullopt;
    }

    bool TypeChecker::is_mutable_symbol_(uint32_t sym_id) const {
        auto it = sym_is_mut_.find(sym_id);
        if (it == sym_is_mut_.end()) return false;
        return it->second;
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
                diag_(diag::Code::kEscapeOperandMustBePlace, e.span);
                err_(e.span, "escape '&&' requires a place expression (ident/index)");
                return types_.error();
            }
            if (fn_ctx_.is_pure || fn_ctx_.is_comptime) {
                diag_(diag::Code::kTypeEscapeNotAllowedInPureComptime, e.span);
                err_(e.span, "escape '&&' is not allowed in pure/comptime functions (recommended rule)");
                return types_.error();
            }
            return types_.make_escape(at);
        }

        // 기타 unary: v0에서는 최소만
        if (e.op == K::kBang) {
            if (at != types_.builtin(ty::Builtin::kBool) && !is_error_(at)) {
                diag_(diag::Code::kTypeUnaryBangMustBeBool, e.span, types_.to_string(at));
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
        if (!is_place_expr_(e.a)) {
            diag_(diag::Code::kPostfixOperandMustBePlace, e.span);
            err_(e.span, "postfix operator requires a place expression");
            return types_.error();
        }

        // mut check (x++ is a write)
        // - place가 가리키는 심볼이 mut가 아니면 무조건 에러
        if (auto sid = root_place_symbol_(e.a)) {
            if (!is_mutable_symbol_(*sid)) {
                diag_(diag::Code::kWriteToImmutable, e.span);
                err_(e.span, "cannot apply postfix ++ to an immutable variable (declare it with `mut`)");
            }
        }

        ty::TypeId at = check_expr_(e.a);
        return at;
    }

    // --------------------
    // binary / assign / ternary
    // --------------------
    ty::TypeId TypeChecker::check_expr_binary_(const ast::Expr& e) {
        // NOTE:
        // - v0 정책: binary는 기본적으로 "builtin fast-path"만 처리한다.
        // - 추후 operator overloading을 넣을 때도,
        //   여기 구조를 (A) builtin (B) overload fallback 으로 유지하면 된다.

        // ------------------------------------------------------------
        // Null-Coalescing: ??  (Swift/C# 스타일 축약)
        //
        //  a ?? b
        //   - a: Optional(T?) or null
        //   - if a is T? then b must be assignable to T
        //   - result type: T (non-optional)
        //
        // v0 추가 정책:
        //   - lhs가 null literal인 경우 "null ?? x"를 금지하지 않고,
        //     그냥 결과를 rhs 타입으로 둔다. (원하면 경고/에러로 강화 가능)
        // ------------------------------------------------------------
        if (e.op == K::kQuestionQuestion) {
            ty::TypeId lt = check_expr_(e.a);
            ty::TypeId rt = check_expr_(e.b);

            // error short-circuit
            if (is_error_(lt) || is_error_(rt)) return types_.error();

            // lhs가 null이면 rhs로 수렴(정책)
            if (is_null_(lt)) {
                return rt;
            }

            // lhs는 optional 이어야 한다
            if (!is_optional_(lt)) {
                diag_(diag::Code::kTypeNullCoalesceLhsMustBeOptional, e.span, types_.to_string(lt));
                err_(e.span, "operator '??' requires optional lhs");
                return types_.error();
            }

            ty::TypeId elem = optional_elem_(lt);
            if (elem == ty::kInvalidType) {
                // 방어: Optional인데 elem이 invalid인 경우
                err_(e.span, "optional elem type is invalid");
                return types_.error();
            }

            // rhs가 {integer}면 elem 컨텍스트로 해소 시도
            {
                const auto& st = types_.get(rt);
                if (st.kind == ty::Kind::kBuiltin && st.builtin == ty::Builtin::kInferInteger) {
                    (void)resolve_infer_int_in_context_(e.b, elem);
                    rt = check_expr_(e.b);
                }
            }

            // rhs는 elem에 대입 가능해야 함
            if (!can_assign_(elem, rt)) {
                diag_(diag::Code::kTypeNullCoalesceRhsMismatch, e.span,
                    types_.to_string(elem), types_.to_string(rt));
                err_(e.span, "operator '??' rhs mismatch");
                return types_.error();
            }

            // 결과는 non-optional elem
            return elem;
        }

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

        // ------------------------------------------------------------
        // Equality: == / !=
        // ------------------------------------------------------------
        if (e.op == K::kEqEq || e.op == K::kBangEq) {
            // null == null : ok
            if (is_null_(lt) && is_null_(rt)) {
                return types_.builtin(ty::Builtin::kBool);
            }

            // null comparison rule: null is only comparable with optional
            if (is_null_(lt) && !is_optional_(rt)) {
                diag_(diag::Code::kTypeCompareOperandsMustMatch, e.span,
                    types_.to_string(lt), types_.to_string(rt));
                err_(e.span, "null comparison is only allowed with optional types (rhs is not optional)");
                return types_.builtin(ty::Builtin::kBool);
            }
            if (is_null_(rt) && !is_optional_(lt)) {
                diag_(diag::Code::kTypeCompareOperandsMustMatch, e.span,
                    types_.to_string(lt), types_.to_string(rt));
                err_(e.span, "null comparison is only allowed with optional types (lhs is not optional)");
                return types_.builtin(ty::Builtin::kBool);
            }

            // v0: other equality just returns bool (strict typing could be enforced later)
            return types_.builtin(ty::Builtin::kBool);
        }

        // ------------------------------------------------------------
        // Arithmetic: + - * / %
        // ------------------------------------------------------------
        if (e.op == K::kPlus || e.op == K::kMinus || e.op == K::kStar || e.op == K::kSlash || e.op == K::kPercent) {
            // float + {integer} is forbidden (no implicit int->float)
            if ((is_float(lt) && is_infer_int(rt)) || (is_float(rt) && is_infer_int(lt))) {
                diag_(diag::Code::kIntToFloatNotAllowed, e.span, "float-arithmetic");
                err_(e.span, "cannot use deferred integer '{integer}' in float arithmetic (no implicit int->float)");
                return types_.error();
            }

            // {integer} + concrete int => resolve {integer} to concrete int
            if (is_infer_int(lt) && is_int(rt)) {
                if (!resolve_infer_int_in_context_(e.a, rt)) return types_.error();
                lt = rt;
                return rt;
            }
            if (is_infer_int(rt) && is_int(lt)) {
                if (!resolve_infer_int_in_context_(e.b, lt)) return types_.error();
                rt = lt;
                return lt;
            }

            // {integer} + {integer} => still {integer}
            if (is_infer_int(lt) && is_infer_int(rt)) {
                return types_.builtin(ty::Builtin::kInferInteger);
            }

            // no implicit promotion: operands must match
            if (lt != rt && !is_error_(lt) && !is_error_(rt)) {
                diag_(diag::Code::kTypeBinaryOperandsMustMatch, e.span,
                    types_.to_string(lt), types_.to_string(rt));
                err_(e.span, "binary arithmetic requires both operands to have the same type (no implicit promotion)");
                return types_.error();
            }

            return lt;
        }

        // ------------------------------------------------------------
        // Comparison: < <= > >=
        // ------------------------------------------------------------
        if (e.op == K::kLt || e.op == K::kLtEq || e.op == K::kGt || e.op == K::kGtEq) {
            // If one side is concrete int and the other is {integer}, resolve it like arithmetic.
            if (is_infer_int(lt) && is_int(rt)) {
                if (!resolve_infer_int_in_context_(e.a, rt)) {
                    // resolve function should have emitted diag if needed, but keep safety:
                    diag_(diag::Code::kIntLiteralNeedsTypeContext, ast_.expr(e.a).span);
                    err_(e.span, "failed to resolve deferred integer on lhs in comparison");
                    return types_.builtin(ty::Builtin::kBool);
                }
                lt = rt;
            } else if (is_infer_int(rt) && is_int(lt)) {
                if (!resolve_infer_int_in_context_(e.b, lt)) {
                    diag_(diag::Code::kIntLiteralNeedsTypeContext, ast_.expr(e.b).span);
                    err_(e.span, "failed to resolve deferred integer on rhs in comparison");
                    return types_.builtin(ty::Builtin::kBool);
                }
                rt = lt;
            } else if (is_infer_int(lt) || is_infer_int(rt)) {
                // {integer} vs {integer} (or vs non-int) => needs explicit context
                diag_(diag::Code::kIntLiteralNeedsTypeContext, e.span);
                err_(e.span, "comparison with deferred integer '{integer}' needs an explicit integer type context");
                return types_.builtin(ty::Builtin::kBool);
            }

            // v0 strict rule: types must match
            if (lt != rt && !is_error_(lt) && !is_error_(rt)) {
                diag_(diag::Code::kTypeCompareOperandsMustMatch, e.span,
                    types_.to_string(lt), types_.to_string(rt));
                err_(e.span, "comparison requires both operands to have the same type (v0 rule)");
            }

            return types_.builtin(ty::Builtin::kBool);
        }

        // ------------------------------------------------------------
        // TODO: logical ops, bitwise ops, pipe, etc.
        // ------------------------------------------------------------
        return types_.error();
    }

    ty::TypeId TypeChecker::check_expr_assign_(const ast::Expr& e) {
        // NOTE:
        // - v0: assign expr는 (1) place 체크, (2) rhs 체크, (3) can_assign 검사로 끝낸다.
        // - compound-assign(+= 등)도 현재는 "단순 대입 호환"만 보는 형태.
        // - NEW: ??= 는 제어흐름 의미가 있으므로 별도 규칙을 강제한다.

        // ------------------------------------------------------------
        // Null-Coalescing Assign: ??=
        //
        //  x ??= y
        //   - lhs must be place
        //   - lhs type must be Optional(T?)
        //   - rhs must be assignable to T
        //   - expression result type: lhs type (T?)  (IR lowering/일관성에 유리)
        //
        // 이 연산도 "write" 이므로 mut 검사 대상이다.
        // ------------------------------------------------------------
        if (e.op == K::kQuestionQuestionAssign) {
            // e.a = lhs, e.b = rhs
            if (!is_place_expr_(e.a)) {
                diag_(diag::Code::kAssignLhsMustBePlace, e.span);
                err_(e.span, "assignment lhs must be a place expression (ident/index)");
                (void)check_expr_(e.b);
                return types_.error();
            }

            // NEW: mut check
            if (auto sid = root_place_symbol_(e.a)) {
                if (!is_mutable_symbol_(*sid)) {
                    // NOTE: 새 diag code 필요
                    diag_(diag::Code::kWriteToImmutable, e.span, "assignment");
                    err_(e.span, "cannot assign to an immutable variable (declare it with `mut`)");
                }
            }

            ty::TypeId lt = check_expr_(e.a);
            ty::TypeId rt = check_expr_(e.b);

            if (is_error_(lt) || is_error_(rt)) return types_.error();

            if (!is_optional_(lt)) {
                diag_(diag::Code::kTypeNullCoalesceAssignLhsMustBeOptional, e.span, types_.to_string(lt));
                err_(e.span, "operator '??=' requires optional lhs");
                return types_.error();
            }

            ty::TypeId elem = optional_elem_(lt);
            if (elem == ty::kInvalidType) {
                err_(e.span, "optional elem type is invalid");
                return types_.error();
            }

            // RHS가 {integer}면, elem 타입 컨텍스트로 해소 시도
            {
                const auto& st = types_.get(rt);
                if (st.kind == ty::Kind::kBuiltin && st.builtin == ty::Builtin::kInferInteger) {
                    (void)resolve_infer_int_in_context_(e.b, elem);
                    rt = check_expr_(e.b);
                }
            }

            if (!can_assign_(elem, rt)) {
                diag_(diag::Code::kTypeNullCoalesceAssignRhsMismatch, e.span,
                    types_.to_string(elem), types_.to_string(rt));
                err_(e.span, "operator '??=' rhs mismatch");
                return types_.error();
            }

            return lt;
        }

        // ------------------------------------------------------------
        // 기존 '=' / 기타 대입류 (현 로직 유지)
        // ------------------------------------------------------------
        // e.a = lhs, e.b = rhs
        if (!is_place_expr_(e.a)) {
            diag_(diag::Code::kAssignLhsMustBePlace, e.span);
            err_(e.span, "assignment lhs must be a place expression (ident/index)");
        } else {
            // NEW: mut check
            if (auto sid = root_place_symbol_(e.a)) {
                if (!is_mutable_symbol_(*sid)) {
                    // NOTE: 새 diag code 필요
                    diag_(diag::Code::kWriteToImmutable, e.span, "assignment");
                    err_(e.span, "cannot assign to an immutable variable (declare it with `mut`)");
                }
            }
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
            diag_(
                diag::Code::kTypeAssignMismatch, e.span,
                types_.to_string(lt), types_.to_string(rt)
            );
            err_(e.span, "assign mismatch");
        }
        return lt;
    }

    ty::TypeId TypeChecker::check_expr_ternary_(const ast::Expr& e) {
        // a ? b : c
        ty::TypeId ct = check_expr_(e.a);
        if (ct != types_.builtin(ty::Builtin::kBool) && !is_error_(ct)) {
            diag_(diag::Code::kTypeTernaryCondMustBeBool, e.span, types_.to_string(ct));
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
            diag_(diag::Code::kTypeIndexMustBeUSize, ast_.expr(e.b).span, types_.to_string(it));
            err_(e.span, "index expression must be usize (v0 rule)");
        }

        const auto& t = types_.get(bt);
        if (t.kind == ty::Kind::kArray) {
            return t.elem;
        }

        diag_(diag::Code::kTypeIndexNonArray, e.span, types_.to_string(bt));
        err_(e.span, "indexing is only supported on array types (T[]) in v0");
        return types_.error();
    }

    // --------------------
    // if-expr / block-expr / loop-expr
    // --------------------
    ty::TypeId TypeChecker::check_expr_if_(const ast::Expr& e) {
        return check_expr_if_(e, Slot::kValue);
    }

    ty::TypeId TypeChecker::check_expr_if_(const ast::Expr& e, Slot slot) {
        ty::TypeId ct = check_expr_(e.a, Slot::kValue);
        if (ct != types_.builtin(ty::Builtin::kBool) && !is_error_(ct)) {
            diag_(diag::Code::kTypeCondMustBeBool, ast_.expr(e.a).span, types_.to_string(ct));
            err_(e.span, "if-expr condition must be bool");
        }

        // branches are always value-checked as expressions
        ty::TypeId t_then = check_expr_(e.b, Slot::kValue);
        ty::TypeId t_else = check_expr_(e.c, Slot::kValue);

        (void)slot; // currently result type doesn't depend on slot
        return unify_(t_then, t_else);
    }

    ty::TypeId TypeChecker::check_expr_block_(const ast::Expr& e) {
        return check_expr_block_(e, Slot::kValue);
    }

    ty::TypeId TypeChecker::check_expr_block_(const ast::Expr& e, Slot slot) {
        // Mapping assumption:
        //  - e.a: StmtId of block stmt
        //  - e.b: tail ExprId (optional)
        const ast::StmtId block_sid = (ast::StmtId)e.a;
        if (block_sid == ast::k_invalid_stmt) {
            err_(e.span, "block-expr has no block stmt id");
            return types_.error();
        }

        const ast::Stmt& bs = ast_.stmt(block_sid);
        if (bs.kind != ast::StmtKind::kBlock) {
            err_(e.span, "block-expr target is not a block stmt");
            return types_.error();
        }

        // block expr introduces a scope (like block stmt)
        sym_.push_scope();

        // all child statements are checked in statement context
        for (uint32_t i = 0; i < bs.stmt_count; ++i) {
            const ast::StmtId cid = ast_.stmt_children()[bs.stmt_begin + i];
            check_stmt_(cid);
        }

        // tail
        ty::TypeId out = types_.builtin(ty::Builtin::kNull);
        if (e.b != ast::k_invalid_expr) {
            out = check_expr_(e.b, Slot::kValue);
        } else {
            // tail absent => null
            out = types_.builtin(ty::Builtin::kNull);

            // Slot::Value에서는 tail 요구 (v0 안전 정책)
            if (slot == Slot::kValue) {
                diag_(diag::Code::kBlockExprValueExpected, e.span);
                err_(e.span, "value expected: block-expr in value context must have a tail expression");
            }
        }

        sym_.pop_scope();
        return out;
    }

    ty::TypeId TypeChecker::check_expr_loop_(const ast::Expr& e) {
        return check_expr_loop_(e, Slot::kValue);
    }

    ty::TypeId TypeChecker::check_expr_loop_(const ast::Expr& e, Slot /*slot*/) {
        // loop result type comes ONLY from breaks, plus optional null if:
        // - break; exists, or
        // - iter-loop can naturally end

        LoopCtx lc{};
        lc.may_natural_end = e.loop_has_header; // iter loop => natural end => null
        lc.joined_value = ty::kInvalidType;

        // loop scope: variable binding + body scope
        sym_.push_scope();

        // header: loop (v in xs) { ... }
        if (e.loop_has_header) {
            // v0: loop var type unknown => error (until iter protocol exists)
            if (!e.loop_var.empty()) {
                sym_.insert(sema::SymbolKind::kVar, e.loop_var, types_.error(), e.span);
            }
            if (e.loop_iter != ast::k_invalid_expr) {
                (void)check_expr_(e.loop_iter, Slot::kValue);
            }
        }

        // push loop ctx
        loop_stack_.push_back(lc);

        // body is a block stmt
        if (e.loop_body != ast::k_invalid_stmt) {
            check_stmt_(e.loop_body);
        } else {
            err_(e.span, "loop has no body");
        }

        // pop loop ctx
        LoopCtx done = loop_stack_.back();
        loop_stack_.pop_back();

        sym_.pop_scope();

        // Decide loop type:
        // 1) no breaks:
        //   - iter loop: natural end => null
        //   - infinite loop: never
        if (!done.has_any_break) {
            if (done.may_natural_end) {
                return types_.builtin(ty::Builtin::kNull);
            }
            return types_.builtin(ty::Builtin::kNever);
        }

        // 2) breaks exist:
        // 2-a) no value breaks => only break; (and/or natural end) => null
        if (!done.has_value_break) {
            return types_.builtin(ty::Builtin::kNull);
        }

        // 2-b) value breaks exist => base type = joined_value
        ty::TypeId base = done.joined_value;
        if (base == ty::kInvalidType) base = types_.error();

        // If null is mixed in (break; or natural end), result becomes optional
        const bool has_null = done.has_null_break || done.may_natural_end;

        if (!has_null) {
            return base;
        }

        // base already optional? keep it. if base is null, keep null.
        if (is_null_(base)) return base;
        if (is_optional_(base)) return base;

        return types_.make_optional(base);
    }

    // --------------------
    // cast
    // --------------------
    ty::TypeId TypeChecker::check_expr_cast_(const ast::Expr& e) {
        // AST contract:
        // - e.a: operand
        // - e.cast_type: target type
        // - e.cast_kind: as / as? / as!
        const ast::ExprId operand_eid = e.a;

        if (operand_eid == ast::k_invalid_expr) {
            diag_(diag::Code::kTyckCastMissingOperand, e.span);
            err_(e.span, "cast missing operand");
            return types_.error();
        }

        ty::TypeId operand_t = check_expr_(operand_eid, Slot::kValue);

        ty::TypeId target_t = e.cast_type;
        if (target_t == ty::kInvalidType) {
            diag_(diag::Code::kTyckCastMissingTargetType, e.span);
            err_(e.span, "cast missing target type");
            return types_.error();
        }

        // ------------------------------------------------------------
        // 7.6.3 semantics (Swift/C#-like):
        //
        //   expr as  T   -> T     (no optional auto-unwrapping)
        //   expr as? T   -> T?    (always optional-normalized, no T??)
        //   expr as! T   -> T     (runtime trap on failure)
        //
        // Optional rules:
        // - `as`  : optional 값을 자동 해소하지 않는다.
        // - `as?` : 입력이 null이면 결과 null, 성공하면 T 값을 T?로 반환
        // - `as!` : 입력이 null이거나 변환 실패 시 trap(런타임)
        //
        // v0 scope:
        // - numeric scalar casts only (int<->int, int<->float, float<->float)
        // - future: runtime-checked downcast / ref casts in v1+
        // ------------------------------------------------------------

        auto make_optional_if_needed = [&](ty::TypeId t) -> ty::TypeId {
            if (t == ty::kInvalidType) return types_.error();
            if (is_optional_(t)) return t;
            return types_.make_optional(t);
        };

        // (A) 결과 타입 계산: as?만 항상 optional-normalize
        ty::TypeId result_t = target_t;
        if (e.cast_kind == ast::CastKind::kAsOptional) {
            result_t = make_optional_if_needed(target_t);
        }

        // (B) operand가 null인 경우
        const bool operand_is_null = is_null_(operand_t);
        if (operand_is_null) {
            // null as? T  -> null (T?)
            if (e.cast_kind == ast::CastKind::kAsOptional) {
                return result_t; // T?
            }

            // null as! T  -> runtime trap, but type is T
            if (e.cast_kind == ast::CastKind::kAsForce) {
                return result_t; // T
            }

            // null as T:
            // - only allowed when T is optional (null -> T?)
            // - otherwise error (no implicit unwrap / no null-to-nonopt)
            if (is_optional_(result_t)) {
                return result_t;
            }

            diag_(diag::Code::kTyckCastNullToNonOptional, e.span, types_.to_string(result_t));
            err_(e.span, "cannot cast null to non-optional type");
            return types_.error();
        }

        // (C) operand가 optional(U?)인 경우:
        // - as   : optional 자동 해소 없음 -> U? 를 그대로 검사해야 함 (대부분 비허용이 정상)
        // - as?  : null-safe/실패가능 -> 검사 단계에서는 U로 unwrap해서 "값이 있을 때 변환 가능?"만 본다
        // - as!  : 강제 -> 검사 단계에서는 U로 unwrap해서 변환 가능성만 보고, null은 런타임 trap로 처리
        const bool operand_is_opt = is_optional_(operand_t);

        ty::TypeId check_operand_t = operand_t;
        if ((e.cast_kind == ast::CastKind::kAsOptional || e.cast_kind == ast::CastKind::kAsForce) && operand_is_opt) {
            ty::TypeId elem = optional_elem_(operand_t);
            check_operand_t = (elem == ty::kInvalidType) ? types_.error() : elem;
        }

        // (D) 변환 가능성 체크는 "target_t" 기준으로 한다.
        //     - as?의 결과가 T?인 것과 별개로, 변환 자체는 T로 되는지 확인해야 한다.
        ty::TypeId check_target_t = target_t;

        // ------------------------------------------------------------
        // helper: builtin predicates
        // ------------------------------------------------------------
        auto is_builtin = [&](ty::TypeId t) -> bool {
            return t != ty::kInvalidType && types_.get(t).kind == ty::Kind::kBuiltin;
        };

        auto is_numeric_builtin = [&](ty::TypeId t) -> bool {
            if (!is_builtin(t)) return false;
            auto b = types_.get(t).builtin;
            switch (b) {
                case ty::Builtin::kI8: case ty::Builtin::kI16: case ty::Builtin::kI32:
                case ty::Builtin::kI64: case ty::Builtin::kI128:
                case ty::Builtin::kU8: case ty::Builtin::kU16: case ty::Builtin::kU32:
                case ty::Builtin::kU64: case ty::Builtin::kU128:
                case ty::Builtin::kISize: case ty::Builtin::kUSize:
                case ty::Builtin::kF32: case ty::Builtin::kF64: case ty::Builtin::kF128:
                    return true;
                default:
                    return false;
            }
        };

        // ------------------------------------------------------------
        // (E) "{integer}" placeholder 처리:
        // - 명시적 cast는 강력한 "컨텍스트"이므로,
        //   정수 타겟으로는 여기서 resolve 시도 가능.
        // - float 타겟으로 cast하는 경우는 resolve_infer_int_in_context_를 호출하면
        //   (네 정책상) IntToFloatNotAllowed 진단이 날 수 있으므로 호출하지 않는다.
        //   (float로 가고 싶다면 literal에 f32/f64 suffix를 붙이거나,
        //    향후 별도 정책을 도입하면 된다.)
        // ------------------------------------------------------------
        auto try_resolve_infer_int_by_cast_target = [&]() {
            const auto& st = types_.get(check_operand_t);
            if (!(st.kind == ty::Kind::kBuiltin && st.builtin == ty::Builtin::kInferInteger)) return;

            const auto& dt = types_.get(check_target_t);
            if (dt.kind != ty::Kind::kBuiltin) return;

            auto is_int_builtin = [&](ty::Builtin b) -> bool {
                return b == ty::Builtin::kI8 || b == ty::Builtin::kI16 || b == ty::Builtin::kI32 ||
                    b == ty::Builtin::kI64 || b == ty::Builtin::kI128 ||
                    b == ty::Builtin::kU8 || b == ty::Builtin::kU16 || b == ty::Builtin::kU32 ||
                    b == ty::Builtin::kU64 || b == ty::Builtin::kU128 ||
                    b == ty::Builtin::kISize || b == ty::Builtin::kUSize;
            };

            auto is_float_builtin = [&](ty::Builtin b) -> bool {
                return b == ty::Builtin::kF32 || b == ty::Builtin::kF64 || b == ty::Builtin::kF128;
            };

            if (is_int_builtin(dt.builtin)) {
                (void)resolve_infer_int_in_context_(operand_eid, check_target_t);
                // NOTE: expr cache가 이미 {integer}로 박혀있을 수 있으니
                // check_operand_t는 논리적으로만 업데이트한다고 가정(필요하면 재-read).
            } else if (is_float_builtin(dt.builtin)) {
                // explicit cast to float is allowed conceptually,
                // but we do NOT resolve infer-int here to avoid "implicit int->float" diag.
                // (policy v0; can be revisited later)
            }
        };

        // ------------------------------------------------------------
        // (F) 허용 규칙(v0):
        // 1) 같은 타입 (T -> T)
        // 2) builtin numeric <-> builtin numeric (explicit cast only)
        // 3) (향후) 다운캐스트/런타임 타입 검사: v1+
        //
        // IMPORTANT:
        // - `as`는 optional 자동 해소가 없으므로,
        //   U? as T 같은 것은 일반적으로 허용되지 않는다.
        // - `as?`/`as!`는 검사 시 unwrap(U? -> U) 후 변환 가능성만 체크한다.
        // ------------------------------------------------------------

        // 1) identical
        if (check_operand_t == check_target_t) {
            return result_t;
        }

        // 2) resolve "{integer}" with cast target when possible
        try_resolve_infer_int_by_cast_target();

        // 3) numeric explicit casts
        if (is_numeric_builtin(check_target_t) && is_numeric_builtin(check_operand_t)) {
            return result_t;
        }

        // Otherwise: not allowed (future: runtime checked downcast)
        diag_(diag::Code::kTyckCastNotAllowed, e.span,
            types_.to_string(operand_t), types_.to_string(result_t));
        err_(e.span, "cast not allowed");
        return types_.error();
    }

} // namespace gaupel::tyck