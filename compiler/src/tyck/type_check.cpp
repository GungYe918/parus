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

    static bool fits_builtin_int_(__int128 v, ty::Builtin dst) {
        const __int128 I8_MIN = -128, I8_MAX = 127;
        const __int128 I16_MIN = -32768, I16_MAX = 32767;
        const __int128 I32_MIN = -((__int128)1 << 31), I32_MAX = (((__int128)1 << 31) - 1);
        const __int128 I64_MIN = -((__int128)1 << 63), I64_MAX = (((__int128)1 << 63) - 1);
        const __int128 I128_MIN = -((__int128)1 << 127);
        const __int128 I128_MAX = (((__int128)1 << 127) - 1);

        switch (dst) {
            case ty::Builtin::kI8:   return v >= I8_MIN && v <= I8_MAX;
            case ty::Builtin::kI16:  return v >= I16_MIN && v <= I16_MAX;
            case ty::Builtin::kI32:  return v >= I32_MIN && v <= I32_MAX;
            case ty::Builtin::kI64:  return v >= I64_MIN && v <= I64_MAX;
            case ty::Builtin::kI128: return v >= I128_MIN && v <= I128_MAX;

            // u*는 음수 금지. v가 음수면 false.
            case ty::Builtin::kU8:   return v >= 0 && v <= 255;
            case ty::Builtin::kU16:  return v >= 0 && v <= 65535;
            case ty::Builtin::kU32:  return v >= 0 && v <= (((__int128)1 << 32) - 1);
            case ty::Builtin::kU64:  return v >= 0 && v <= (((__int128)1 << 64) - 1);
            case ty::Builtin::kU128: return v >= 0; // __int128로는 상한(2^128-1) 표현 불가 -> v0에서 양수면 "일단 가능" 처리

            default: return false;
        }
    }

    static ty::Builtin pick_smallest_signed_int_(__int128 v) {
        if (fits_builtin_int_(v, ty::Builtin::kI8))   return ty::Builtin::kI8;
        if (fits_builtin_int_(v, ty::Builtin::kI16))  return ty::Builtin::kI16;
        if (fits_builtin_int_(v, ty::Builtin::kI32))  return ty::Builtin::kI32;
        if (fits_builtin_int_(v, ty::Builtin::kI64))  return ty::Builtin::kI64;
        return ty::Builtin::kI128;
    }

    // --------------------
    // pass 2: check
    // --------------------
    void TypeChecker::second_pass_check_program_(ast::StmtId program_stmt) {
        check_stmt_(program_stmt);

        // ----------------------------------------
        // Finalize unresolved deferred integers:
        // - If a set-var inferred as "{integer}" is never consumed in a way that fixes the type,
        //   we pick the smallest signed integer type that fits (i8..i128).
        // - This keeps DX friendly and avoids leaving IR in an unresolved state.
        // ----------------------------------------
        for (auto& kv : pending_int_) {
            PendingInt& pi = kv.second;
            if (!pi.has_value) continue;
            if (pi.resolved) continue;

            // pick smallest signed type
            ty::Builtin b = pick_smallest_signed_int_(pi.value);
            pi.resolved = true;
            pi.resolved_type = types_.builtin(b);

            // NOTE:
            // v0에서는 SymbolTable에 직접 type mutation API가 없으므로,
            // 실제 소비 지점에서는 pending_int_를 우선 확인하는 방식으로 동작하게 된다.
            // (다음 단계에서 SymbolTable이 SymbolId 기반으로 타입 갱신을 지원하면 여기서 반영)
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

    static bool parse_i128_(std::string_view text, __int128& out) {
        // text는 AST에 저장된 리터럴 원문 (예: "4", "-1")
        // 10진 정수만 가정(v0). 필요하면 0x/0b는 Lexer 단계에서 별도 토큰화 후 확장.
        if (text.size() == 0) return false;

        bool neg = false;
        size_t i = 0;
        if (text[0] == '-') { neg = true; i = 1; }
        if (i >= text.size()) return false;

        __int128 v = 0;

        // i128 범위 체크를 위해 절대값을 unsigned로 받지 않고,
        // "v = v*10 + d"에서 overflow를 직접 감지한다.
        for (; i < text.size(); ++i) {
            char c = text[i];
            if (c < '0' || c > '9') return false;
            int d = c - '0';

            // overflow 체크: v*10 + d 가 i128 절대범위를 넘는지
            // signed i128 최대:  2^127 - 1
            // signed i128 최소: -2^127
            const __int128 I128_MAX = (((__int128)1 << 126) - 1) * 2 + 1; // 2^127 - 1
            const __int128 I128_MIN = -((__int128)1 << 127);

            if (!neg) {
                if (v > (I128_MAX - d) / 10) return false;
                v = v * 10 + d;
            } else {
                // 음수는 최소 -2^127 까지 허용
                // out = -(abs). abs는 2^127 까지 가능
                const __int128 ABS_MIN = ((__int128)1 << 127); // 2^127
                // 여기서는 v를 abs로 누적한다.
                if (v > (ABS_MIN - d) / 10) return false;
                v = v * 10 + d;
            }
        }

        if (!neg) {
            out = v;
            return true;
        } else {
            const __int128 ABS_MIN = ((__int128)1 << 127);
            if (v == ABS_MIN) { out = -ABS_MIN; return true; } // exactly i128 min
            out = -v;
            return true;
        }
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
        if (init_e.kind == ast::ExprKind::kIntLit) {
            __int128 v = 0;
            if (!parse_i128_(init_e.text, v)) {
                err_(init_e.span, "integer literal is out of range for i128 (overflow)");
                inferred = types_.error();
            } else {
                inferred = types_.builtin(ty::Builtin::kInferInteger);

                PendingInt pi{};
                pi.value = v;
                pi.has_value = true;
                pi.resolved = false;
                pi.resolved_type = ty::kInvalidType;

                pending_int_[std::string(s.name)] = pi;
            }
        }

        if (inferred == ty::kInvalidType) inferred = types_.error();

        // (D) 현재 스코프에 선언으로 삽입
        auto ins = sym_.insert(sema::SymbolKind::kVar, s.name, inferred, s.span);
        if (!ins.ok) {
            if (ins.is_duplicate) {
                diag_(diag::Code::kDuplicateDecl, s.span, s.name);
                err_(s.span, "duplicate symbol (var): " + std::string(s.name));
            } else if (ins.is_shadowing) {
                // 섀도잉 정책은 옵션화 가능.
                // 지금은 일단 경고로 기록(또는 프로젝트 정책이 "불가"면 kShadowingNotAllowed로 바꿔)
                diag_(diag::Code::kShadowing, s.span, s.name);
            }
        }

        // (E) AST에 “추론된 타입” 기록 (후속 패스/IR 친화)
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
            // void/empty return은 현재 타입 시스템에 void가 없으므로 정책적으로 막아둔다.
            // (향후 Builtin::kUnit 같은 걸 추가하거나, return;을 허용하려면 여기만 바꾸면 됨)
            err_(s.span, "return expression is required (no 'unit' type yet)");
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

            case ast::ExprKind::kIntLit:
                // v1: Rust-like unsuffixed integer -> internal placeholder "{integer}"
                // 실제 i8~i128 확정은 소비될 때 수행한다.
                t = types_.builtin(ty::Builtin::kInferInteger);
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
                    diag_(diag::Code::kUndefinedName, e.span, e.text);
                    err_(e.span, "unknown identifier"); // 저장용
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

        // -------------------------------------------------
        // "{integer}" placeholder rules (Rust-like)
        // - placeholder can be assigned ONLY into an integer type (signed/unsigned),
        //   and only if the literal value fits.
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
                // set x = 5; then let c: f64 = x;  => ERROR
                return false;
            }

            if (!is_int_builtin(dt.builtin)) {
                return false;
            }

            // literal-backed placeholder만 v1에서 허용
            // (이 함수는 const이므로 실제 해소는 다른 곳에서 수행; 여기서는 "가능성"만 판단)
            return true;
        }

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
            return b == ty::Builtin::kF32 || b == ty::Builtin::kF64;
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

        auto resolve_placeholder_to_ = [&](ty::TypeId expected) -> bool {
            // v1: placeholder는 "set var = int literal"로부터만 생긴다고 가정하고,
            // 이름 기반으로 pending_int_에서 값 얻어온다.
            //
            // - 좌/우 expr이 ident가 아닌 경우(직접 리터럴)는 아직 pending에 없을 수 있음.
            //   그 경우는 e.a/e.b에서 text를 직접 파싱한다.
            auto resolve_expr = [&](ast::ExprId eid) -> bool {
                const ast::Expr& ex = ast_.expr(eid);

                __int128 v = 0;
                bool ok = false;

                if (ex.kind == ast::ExprKind::kIntLit) {
                    ok = parse_i128_(ex.text, v);
                } else if (ex.kind == ast::ExprKind::kIdent) {
                    auto it = pending_int_.find(std::string(ex.text));
                    if (it != pending_int_.end() && it->second.has_value) {
                        v = it->second.value;
                        ok = true;

                        // 이미 다른 타입으로 확정된 상태인데, 여기 expected와 다르면 에러
                        if (it->second.resolved && it->second.resolved_type != expected) {
                            err_(ex.span, "deferred integer already resolved to a different type");
                            return false;
                        }
                    }
                }

                if (!ok) return false;

                // expected 범위 체크
                const auto& et = types_.get(expected);
                if (et.kind != ty::Kind::kBuiltin) return false;

                if (!fits_builtin_int_(v, et.builtin)) {
                    err_(ex.span, "integer literal does not fit the required type");
                    return false;
                }

                // ident면 resolve 기록
                if (ex.kind == ast::ExprKind::kIdent) {
                    auto& pi = pending_int_[std::string(ex.text)];
                    pi.resolved = true;
                    pi.resolved_type = expected;
                }
                return true;
            };

            bool okA = true, okB = true;
            if (is_infer_int(lt)) okA = resolve_expr(e.a);
            if (is_infer_int(rt)) okB = resolve_expr(e.b);
            return okA && okB;
        };

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
            // float 컨텍스트에 placeholder가 끼면 즉시 에러 (no implicit int->float)
            if ((is_float(lt) && is_infer_int(rt)) || (is_float(rt) && is_infer_int(lt))) {
                err_(e.span, "cannot use deferred integer '{integer}' in float arithmetic (no implicit int->float)");
                return types_.error();
            }

            // placeholder + concrete int => placeholder를 concrete로 확정
            if (is_infer_int(lt) && is_int(rt)) {
                if (!resolve_placeholder_to_(rt)) return types_.error();
                return rt;
            }
            if (is_infer_int(rt) && is_int(lt)) {
                if (!resolve_placeholder_to_(lt)) return types_.error();
                return lt;
            }

            // placeholder + placeholder => 아직 타입 결정 불가 (v1: 그대로 placeholder 유지)
            if (is_infer_int(lt) && is_infer_int(rt)) {
                return types_.builtin(ty::Builtin::kInferInteger);
            }

            // 핵심: i32 + i64 같은 이종 연산은 에러 (Rust처럼 강제)
            if (lt != rt && !is_error_(lt) && !is_error_(rt)) {
                err_(e.span, "binary arithmetic requires both operands to have the same type (no implicit promotion)");
                return types_.error();
            }
            return lt;
        }

        // 비교 < <= > >= : 동일 타입만
        if (e.op == K::kLt || e.op == K::kLtEq || e.op == K::kGt || e.op == K::kGtEq) {
            // placeholder 비교는 컨텍스트가 부족하니 v1에서 금지(원하면 여기서도 해소 규칙 추가 가능)
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