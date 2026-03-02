// frontend/src/tyck/type_check_stmt.cpp
#include <parus/tyck/TypeCheck.hpp>
#include <parus/syntax/TokenKind.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/diag/DiagCode.hpp>
#include "../common/type_check_literals.hpp"

#include <algorithm>
#include <sstream>
#include <unordered_map>


namespace parus::tyck {

    using K = parus::syntax::TokenKind;
    using detail::ParsedFloatLiteral;
    using detail::ParsedIntLiteral;
    using detail::parse_float_literal_;
    using detail::parse_int_literal_;

    void TypeChecker::check_stmt_(ast::StmtId sid) {
        const ast::Stmt s = ast_.stmt(sid);

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
            case ast::StmtKind::kDoScope:
                check_stmt_do_scope_(s);
                return;
            case ast::StmtKind::kDoWhile:
                check_stmt_do_while_(s);
                return;
            case ast::StmtKind::kManual:
                check_stmt_manual_(s);
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

                // value-break는 "loop expression" 컨텍스트에서만 허용한다.
                // (while 같은 statement-loop에서는 break 값을 받을 곳이 없다.)
                if (loop_stack_.empty()) {
                    diag_(diag::Code::kTypeBreakValueOnlyInLoopExpr, s.span);
                    err_(s.span, "break value is not allowed in a statement loop");
                    (void)check_expr_(s.expr, Slot::kValue);
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

            case ast::StmtKind::kCommitStmt:
                if (!in_actor_method_ || !in_actor_pub_method_) {
                    diag_(diag::Code::kActorCommitOnlyInPub, s.span);
                    err_(s.span, "commit is only allowed in actor pub methods");
                }
                return;

            case ast::StmtKind::kRecastStmt:
                if (!in_actor_method_ || !in_actor_sub_method_) {
                    diag_(diag::Code::kActorRecastOnlyInSub, s.span);
                    err_(s.span, "recast is only allowed in actor sub methods");
                }
                return;

            case ast::StmtKind::kSwitch:
                check_stmt_switch_(s);
                return;

            case ast::StmtKind::kFnDecl:
                check_stmt_fn_decl_(sid, s);
                return;

            case ast::StmtKind::kFieldDecl:
                check_stmt_field_decl_(sid);
                return;

            case ast::StmtKind::kProtoDecl:
                check_stmt_proto_decl_(sid);
                return;

            case ast::StmtKind::kClassDecl:
                check_stmt_class_decl_(sid);
                return;

            case ast::StmtKind::kActorDecl:
                check_stmt_actor_decl_(sid);
                return;

            case ast::StmtKind::kActsDecl:
                check_stmt_acts_decl_(sid, s);
                return;

            case ast::StmtKind::kUse:
                if (s.use_kind == ast::UseKind::kImport && block_depth_ != 0) {
                    const std::string msg = "import is only allowed at file scope";
                    diag_(diag::Code::kTypeErrorGeneric, s.span, msg);
                    err_(s.span, msg);
                    return;
                }
                if ((s.use_kind == ast::UseKind::kImport ||
                     s.use_kind == ast::UseKind::kPathAlias ||
                     s.use_kind == ast::UseKind::kNestAlias) &&
                    s.use_path_count > 0) {
                    const std::string path = path_join_(s.use_path_begin, s.use_path_count);
                    std::string alias(s.use_rhs_ident);
                    if (alias.empty()) {
                        const auto& segs = ast_.path_segs();
                        if (s.use_path_begin + s.use_path_count <= segs.size()) {
                            alias = std::string(segs[s.use_path_begin + s.use_path_count - 1]);
                        }
                    }
                    if (!path.empty() && !alias.empty()) {
                        if (s.use_kind == ast::UseKind::kNestAlias) {
                            if (!is_known_namespace_path_(path)) {
                                diag_(diag::Code::kUseNestPathExpectedNamespace, s.span, path);
                                err_(s.span, "use nest target must be namespace path: " + path);
                            } else {
                                (void)define_alias_(alias, path, s.span, /*warn_use_nest_preferred=*/false);
                            }
                        } else if (s.use_kind == ast::UseKind::kPathAlias) {
                            (void)define_alias_(alias, path, s.span, /*warn_use_nest_preferred=*/true);
                        } else {
                            (void)define_alias_(alias, path, s.span, /*warn_use_nest_preferred=*/false);
                        }
                    }
                }
                if (s.use_kind == ast::UseKind::kActsEnable) {
                    (void)apply_use_acts_selection_(s);
                }
                return;

            case ast::StmtKind::kNestDecl:
                if (s.nest_is_file_directive) {
                    return;
                }
                if (s.a != ast::k_invalid_stmt) {
                    uint32_t pushed = 0;
                    const auto& segs = ast_.path_segs();
                    const uint64_t begin = s.nest_path_begin;
                    const uint64_t end = begin + s.nest_path_count;
                    if (begin <= segs.size() && end <= segs.size()) {
                        for (uint32_t i = 0; i < s.nest_path_count; ++i) {
                            namespace_stack_.push_back(std::string(segs[s.nest_path_begin + i]));
                            ++pushed;
                        }
                    }

                    push_alias_scope_();
                    const auto& body = ast_.stmt(s.a);
                    if (body.kind == ast::StmtKind::kBlock) {
                        const auto& children = ast_.stmt_children();
                        for (uint32_t i = 0; i < body.stmt_count; ++i) {
                            const ast::StmtId cid = children[body.stmt_begin + i];
                            check_stmt_(cid);
                        }
                    } else {
                        check_stmt_(s.a);
                    }
                    pop_alias_scope_();

                    while (pushed > 0) {
                        namespace_stack_.pop_back();
                        --pushed;
                    }
                }
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
        push_acts_selection_scope_();
        push_alias_scope_();
        ++block_depth_;

        // s.stmt_begin/count 는 ast_.stmt_children()의 slice
        const auto& children = ast_.stmt_children();

        for (uint32_t i = 0; i < s.stmt_count; ++i) {
            const ast::StmtId cid = children[s.stmt_begin + i];
            check_stmt_(cid);
            // 에러가 나도 계속 진행할지 정책(여기선 계속)
        }

        if (block_depth_ > 0) --block_depth_;
        pop_alias_scope_();
        pop_acts_selection_scope_();
        sym_.pop_scope();
    }

    void TypeChecker::check_stmt_var_(ast::StmtId sid) {
        const ast::Stmt s = ast_.stmt(sid);
        const bool is_global_decl =
            (block_depth_ == 0) &&
            (s.is_static || s.is_extern || s.is_export || (s.link_abi == ast::LinkAbi::kC));
        const std::string decl_name = is_global_decl
            ? qualify_decl_name_(s.name)
            : std::string(s.name);

        // ----------------------------------------
        // extern variable declaration:
        // - declaration only (no initializer)
        // - explicit type required
        // - set/let 표기 차이는 의미론에 영향 없음
        // ----------------------------------------
        if (s.is_extern) {
            if (s.type == ty::kInvalidType) {
                diag_(diag::Code::kVarDeclTypeAnnotationRequired, s.span);
                err_(s.span, "extern variable requires an explicit declared type");
            }
            if (s.init != ast::k_invalid_expr) {
                diag_(diag::Code::kTypeErrorGeneric, s.span,
                    "extern variable declaration must not have an initializer");
                err_(s.span, "extern variable declaration must not have an initializer");
            }

            ty::TypeId vt = (s.type == ty::kInvalidType) ? types_.error() : s.type;
            uint32_t var_sym = sema::SymbolTable::kNoScope;
            if (is_global_decl) {
                if (auto sid_existing = sym_.lookup(decl_name)) {
                    const auto& existing = sym_.symbol(*sid_existing);
                    if (existing.kind == sema::SymbolKind::kVar) {
                        var_sym = *sid_existing;
                    } else {
                        diag_(diag::Code::kDuplicateDecl, s.span, decl_name);
                        err_(s.span, "duplicate symbol (extern var): " + decl_name);
                    }
                }
            }

            if (var_sym == sema::SymbolTable::kNoScope) {
                auto ins = sym_.insert(sema::SymbolKind::kVar, decl_name, vt, s.span);
                if (!ins.ok) {
                    if (ins.is_duplicate) {
                        diag_(diag::Code::kDuplicateDecl, s.span, decl_name);
                        err_(s.span, "duplicate symbol (extern var): " + decl_name);
                    } else if (ins.is_shadowing) {
                        diag_(diag::Code::kShadowing, s.span, decl_name);
                    }
                } else {
                    var_sym = ins.symbol_id;
                }
            } else {
                (void)sym_.update_declared_type(var_sym, vt);
            }

            if (var_sym != sema::SymbolTable::kNoScope) {
                sym_is_mut_[var_sym] = s.is_mut;
            }

            ast_.stmt_mut(sid).type = vt;
            check_c_abi_global_decl_(s);
            return;
        }

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
                const CoercionPlan init_plan = classify_assign_with_coercion_(
                    AssignSite::LetInit, s.type, s.init, s.span);
                init_t = init_plan.src_after;

                if (s.type != ty::kInvalidType && !init_plan.ok) {
                    diag_(diag::Code::kTypeLetInitMismatch, s.span,
                        s.name, types_.to_string(s.type), type_for_user_diag_(init_t, s.init));
                    err_(s.span, "let init mismatch");
                }
            }

            ty::TypeId vt = (s.type == ty::kInvalidType) ? types_.error() : s.type;

            uint32_t var_sym = sema::SymbolTable::kNoScope;
            if (is_global_decl) {
                if (auto sid_existing = sym_.lookup(decl_name)) {
                    const auto& existing = sym_.symbol(*sid_existing);
                    if (existing.kind == sema::SymbolKind::kVar) {
                        var_sym = *sid_existing;
                    } else {
                        diag_(diag::Code::kDuplicateDecl, s.span, decl_name);
                        err_(s.span, "duplicate symbol (var): " + decl_name);
                    }
                }
            }

            if (var_sym == sema::SymbolTable::kNoScope) {
                auto ins = sym_.insert(sema::SymbolKind::kVar, decl_name, vt, s.span);
                if (!ins.ok) {
                    if (ins.is_duplicate) {
                        diag_(diag::Code::kDuplicateDecl, s.span, decl_name);
                        err_(s.span, "duplicate symbol (var): " + decl_name);
                    } else if (ins.is_shadowing) {
                        diag_(diag::Code::kShadowing, s.span, decl_name);
                    }
                } else {
                    var_sym = ins.symbol_id;
                }
            } else {
                (void)sym_.update_declared_type(var_sym, vt);
            }

            if (var_sym != sema::SymbolTable::kNoScope) {
                sym_is_mut_[var_sym] = s.is_mut;
            }

            // (선택) let의 경우도 AST에 vt를 확정 기록 (이미 s.type이지만, invalid였으면 error로)
            ast_.stmt_mut(sid).type = vt;
            if (var_sym != sema::SymbolTable::kNoScope && s.var_has_acts_binding) {
                (void)bind_symbol_acts_selection_(var_sym, vt, s, s.span);
            }
            check_c_abi_global_decl_(s);
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
            ast_.stmt_mut(sid).type = types_.error();
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
                ast_.stmt_mut(sid).type = types_.error();
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
            const ParsedIntLiteral lit = parse_int_literal_(init_e.text);
            num::BigInt v;
            if (!lit.ok || !num::BigInt::parse_dec(lit.digits_no_sep, v)) {
                diag_(diag::Code::kIntLiteralInvalid, init_e.span, init_e.text);
                err_(init_e.span, "invalid integer literal");
                inferred = types_.error();
                if (ins.ok) sym_.update_declared_type(ins.symbol_id, inferred);
            } else {
                if (lit.has_suffix) {
                    inferred = types_.builtin(lit.suffix);
                    if (!fits_builtin_int_big_(v, lit.suffix)) {
                        diag_(diag::Code::kIntLiteralOverflow, init_e.span, init_e.text, types_.to_string(inferred));
                        err_(init_e.span, "integer literal overflow");
                        inferred = types_.error();
                    }
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
        }

        if (inferred == ty::kInvalidType) inferred = types_.error();

        // (F) AST에 “추론된 타입” 기록
        ast_.stmt_mut(sid).type = inferred;
        if (ins.ok && s.var_has_acts_binding) {
            (void)bind_symbol_acts_selection_(ins.symbol_id, inferred, s, s.span);
        }
        check_c_abi_global_decl_(s);
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
        if (s.a != ast::k_invalid_stmt) {
            ++stmt_loop_depth_;
            check_stmt_(s.a);
            if (stmt_loop_depth_ > 0) --stmt_loop_depth_;
        }
    }

    /// @brief `do { ... }` 문장을 타입체크한다.
    void TypeChecker::check_stmt_do_scope_(const ast::Stmt& s) {
        if (s.a != ast::k_invalid_stmt) {
            check_stmt_(s.a);
        }
    }

    /// @brief `do { ... } while (cond);` 문장을 타입체크한다.
    void TypeChecker::check_stmt_do_while_(const ast::Stmt& s) {
        // do-while도 반복문이므로 body 내부의 break/continue 문맥을 허용한다.
        if (s.a != ast::k_invalid_stmt) {
            ++stmt_loop_depth_;
            check_stmt_(s.a);
            if (stmt_loop_depth_ > 0) --stmt_loop_depth_;
        }

        if (s.expr != ast::k_invalid_expr) {
            ty::TypeId ct = check_expr_(s.expr);
            if (ct != types_.builtin(ty::Builtin::kBool) && !is_error_(ct)) {
                diag_(diag::Code::kTypeCondMustBeBool, ast_.expr(s.expr).span, types_.to_string(ct));
                err_(s.span, "do-while condition must be bool");
            }
        }
    }

    /// @brief manual 블록은 타입 규칙 완화 없이 블록 본문만 검사한다.
    void TypeChecker::check_stmt_manual_(const ast::Stmt& s) {
        if (s.a != ast::k_invalid_stmt) {
            check_stmt_(s.a);
        }
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
            // 내부 표현상 Unit(=source "void") 타입이면 return; 허용
            // - 아니면 에러
            if (rt == types_.builtin(ty::Builtin::kUnit)) {
                return;
            }

            diag_(diag::Code::kTypeReturnExprRequired, s.span);
            err_(s.span, "return expression is required (function does not return void)");
            return;
        }

        ty::TypeId v = check_expr_(s.expr);
        const CoercionPlan ret_plan = classify_assign_with_coercion_(
            AssignSite::Return, rt, s.expr, s.span);
        v = ret_plan.src_after;
        if (!ret_plan.ok) {
            diag_(diag::Code::kTypeMismatch, s.span, types_.to_string(rt), type_for_user_diag_(v, s.expr));

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

    void TypeChecker::check_stmt_fn_decl_(ast::StmtId sid, const ast::Stmt& s) {
        // ----------------------------
        // 0) 시그니처 타입 확보
        // ----------------------------
        ty::TypeId sig = s.type;

        ty::TypeId ret = ty::kInvalidType;

        // (1) 파서가 Stmt.type에 def 시그니처를 넣어준 경우
        if (sig != ty::kInvalidType && types_.get(sig).kind == ty::Kind::kFn) {
            ret = types_.get(sig).ret;
        } else {
            // (2) 아니면 tyck에서 직접 만든다.
            //     - s.type가 def이 아니면 "반환 타입"으로 들어왔을 가능성이 높으므로 그걸 우선 ret로 사용
            if (sig != ty::kInvalidType && types_.get(sig).kind != ty::Kind::kFn) {
                ret = sig;
            }
            if (ret == ty::kInvalidType) {
                // 반환 타입을 얻을 길이 없으면 error로 둔다.
                // (나중에 AST에 fn_ret_type 필드를 확정하면 여기서 그 필드를 쓰면 됨)
                ret = types_.error();
                err_(s.span, "def decl is missing return type (cannot form signature)");
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

        if (s.link_abi == ast::LinkAbi::kC) {
            if (s.has_named_group || s.positional_param_count != s.param_count) {
                diag_(diag::Code::kAbiCNamedGroupNotAllowed, s.span, s.name);
                err_(s.span, "C ABI function must not use named-group parameters: " + std::string(s.name));
            }

            (void)ensure_generic_field_instance_from_type_(ret, s.span);
            if (!is_c_abi_safe_type_(ret, /*allow_void=*/true)) {
                diag_(diag::Code::kAbiCTypeNotFfiSafe, s.span,
                    std::string("return type of '") + std::string(s.name) + "'",
                    types_.to_string(ret));
                err_(s.span, "C ABI return type is not FFI-safe");
            }

            for (uint32_t i = 0; i < s.param_count; ++i) {
                const auto& p = ast_.params()[s.param_begin + i];
                (void)ensure_generic_field_instance_from_type_(p.type, p.span);
                if (!is_c_abi_safe_type_(p.type, /*allow_void=*/false)) {
                    diag_(diag::Code::kAbiCTypeNotFfiSafe, p.span,
                        std::string("parameter '") + std::string(p.name) + "'",
                        types_.to_string(p.type));
                    err_(p.span, "C ABI parameter type is not FFI-safe: " + std::string(p.name));
                }
            }
        }

        // ----------------------------
        // 0.5) generic proto constraints (declaration-time validation)
        // ----------------------------
        std::unordered_set<std::string> generic_params;
        for (uint32_t gi = 0; gi < s.fn_generic_param_count; ++gi) {
            const uint32_t idx = s.fn_generic_param_begin + gi;
            if (idx >= ast_.generic_param_decls().size()) break;
            generic_params.insert(std::string(ast_.generic_param_decls()[idx].name));
        }
        for (uint32_t ci = 0; ci < s.fn_constraint_count; ++ci) {
            const uint32_t idx = s.fn_constraint_begin + ci;
            if (idx >= ast_.fn_constraint_decls().size()) break;
            const auto& c = ast_.fn_constraint_decls()[idx];

            if (generic_params.find(std::string(c.type_param)) == generic_params.end()) {
                std::string msg = "constraint uses unknown type parameter: " + std::string(c.type_param);
                diag_(diag::Code::kProtoConstraintUnsatisfied, c.span, msg);
                err_(c.span, msg);
            }

            const std::string proto_path = path_join_(c.proto_path_begin, c.proto_path_count);
            bool proto_ok = false;
            if (!proto_path.empty()) {
                std::string key = proto_path;
                if (auto rewritten = rewrite_imported_path_(key)) {
                    key = *rewritten;
                }
                if (proto_decl_by_name_.find(key) != proto_decl_by_name_.end()) {
                    proto_ok = true;
                } else if (auto sid = lookup_symbol_(key)) {
                    const auto& ss = sym_.symbol(*sid);
                    proto_ok = (proto_decl_by_name_.find(ss.name) != proto_decl_by_name_.end());
                }
            }
            if (!proto_ok) {
                diag_(diag::Code::kProtoImplTargetNotSupported, c.span, proto_path);
                err_(c.span, "unknown proto in constraint: " + proto_path);
            }
        }

        // generic templates are declaration-only at this stage.
        // concrete instances are materialized and checked on-demand at call sites.
        if (sid != ast::k_invalid_stmt &&
            s.fn_generic_param_count > 0 &&
            generic_fn_template_sid_set_.find(sid) != generic_fn_template_sid_set_.end()) {
            return;
        }

        // ----------------------------
        // 1) 함수 스코프 진입 + def ctx 설정
        // ----------------------------
        sym_.push_scope();

        FnCtx saved = fn_ctx_;
        fn_ctx_.in_fn = true;
        fn_ctx_.is_pure = s.is_pure;
        fn_ctx_.is_comptime = s.is_comptime;
        fn_ctx_.ret = (ret == ty::kInvalidType) ? types_.error() : ret;
        fn_sid_stack_.push_back(sid);

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
            if (ins.ok) {
                if ((size_t)(s.param_begin + i) >= param_resolved_symbol_cache_.size()) {
                    param_resolved_symbol_cache_.resize((size_t)(s.param_begin + i) + 1, sema::SymbolTable::kNoScope);
                }
                param_resolved_symbol_cache_[s.param_begin + i] = ins.symbol_id;
                // receiver mutability follows `self mut`; regular params follow `mut name: T`.
                const bool param_is_mut = p.is_mut ||
                    (p.is_self && p.self_kind == ast::SelfReceiverKind::kMut);
                sym_is_mut_[ins.symbol_id] = param_is_mut;
            }

            // POLICY CHANGE:
            // positional 파라미터 기본값 금지 (named-group에서만 허용)
            if (!p.is_named_group && p.has_default) {
                Span sp = p.span;
                if (p.default_expr != ast::k_invalid_expr) {
                    sp = ast_.expr(p.default_expr).span;
                }
                diag_(diag::Code::kFnParamDefaultNotAllowedOutsideNamedGroup, sp);
                err_(sp, "default value is only allowed inside named-group '{ ... }'");

                // recovery/부수 오류 확인용으로 default expr는 체크는 하되,
                // 이 default를 유효 규칙으로 인정하지 않는다.
                if (p.default_expr != ast::k_invalid_expr) {
                    (void)check_expr_(p.default_expr);
                }
                continue;
            }

            // named-group default만 타입 검사
            if (p.is_named_group && p.has_default && p.default_expr != ast::k_invalid_expr) {
                const CoercionPlan dplan = classify_assign_with_coercion_(
                    AssignSite::DefaultArg, pt, p.default_expr, p.span);
                const ty::TypeId dt = dplan.src_after;
                if (!dplan.ok) {
                    std::ostringstream oss;
                    oss << "default value type mismatch for param '" << p.name
                        << "': expected " << types_.to_string(pt)
                        << ", got " << type_for_user_diag_(dt, p.default_expr);
                    diag_(diag::Code::kTypeParamDefaultMismatch, p.span,
                            p.name, types_.to_string(pt), type_for_user_diag_(dt, p.default_expr));
                    err_(p.span, oss.str());
                }
            }
        }

        // ----------------------------
        // 3) 본문 체크
        // ----------------------------
        if (s.is_extern) {
            if (s.a != ast::k_invalid_stmt) {
                diag_(diag::Code::kTypeErrorGeneric, s.span, "extern function declaration must not have a body");
                err_(s.span, "extern function declaration must not have a body");
            }
        } else if (s.a != ast::k_invalid_stmt) {
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

        // 반환 타입이 void(Unit)/never면 "끝까지 도달" 허용
        const ty::TypeId fn_ret = fn_ctx_.ret;

        if (!s.is_extern && !is_unit(fn_ret) && !is_never(fn_ret)) {
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

                    case ast::StmtKind::kDoScope:
                    case ast::StmtKind::kManual:
                        return (st.a != ast::k_invalid_stmt) ? self(self, st.a) : false;

                    // while/loop/switch 등은 v0에서 보수적으로 false
                    case ast::StmtKind::kWhile:
                    case ast::StmtKind::kDoWhile:
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
        if (!fn_sid_stack_.empty()) {
            fn_sid_stack_.pop_back();
        }
        sym_.pop_scope();
    }

    void TypeChecker::check_stmt_proto_decl_(ast::StmtId sid) {
        if (sid == ast::k_invalid_stmt || (size_t)sid >= ast_.stmts().size()) return;
        const ast::Stmt& s = ast_.stmt(sid);
        if (s.kind != ast::StmtKind::kProtoDecl) return;
        if (generic_proto_template_sid_set_.find(sid) != generic_proto_template_sid_set_.end()) {
            return;
        }

        const auto& kids = ast_.stmt_children();
        const uint32_t mb = s.stmt_begin;
        const uint32_t me = s.stmt_begin + s.stmt_count;
        uint32_t proto_member_with_body = 0;
        uint32_t proto_member_sig_only = 0;
        std::vector<ast::StmtId> proto_default_members;
        if (mb <= kids.size() && me <= kids.size()) {
            for (uint32_t i = 0; i < s.stmt_count; ++i) {
                const ast::StmtId msid = kids[s.stmt_begin + i];
                if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                const auto& m = ast_.stmt(msid);
                if (m.kind != ast::StmtKind::kFnDecl) {
                    diag_(diag::Code::kUnexpectedToken, m.span, "proto member signature");
                    err_(m.span, "proto body allows only function signatures");
                    continue;
                }
                if (m.fn_is_operator) {
                    diag_(diag::Code::kProtoOperatorNotAllowed, m.span);
                    err_(m.span, "operator declarations are not allowed in proto");
                }

                if (m.a != ast::k_invalid_stmt) {
                    ++proto_member_with_body;
                    proto_default_members.push_back(msid);
                } else {
                    ++proto_member_sig_only;
                }
            }
        }

        if (proto_member_with_body > 0 && proto_member_sig_only > 0) {
            diag_(diag::Code::kProtoMemberBodyMixNotAllowed, s.span);
            err_(s.span, "proto members must be all signature-only or all default-body");
        }

        for (const ast::StmtId msid : proto_default_members) {
            if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
            check_stmt_fn_decl_(msid, ast_.stmt(msid));
        }

        if (s.proto_has_require && s.proto_require_expr != ast::k_invalid_expr) {
            const ty::TypeId rt = check_expr_(s.proto_require_expr, Slot::kValue);
            if (rt != types_.builtin(ty::Builtin::kBool)) {
                diag_(diag::Code::kProtoRequireTypeNotBool, ast_.expr(s.proto_require_expr).span);
                err_(ast_.expr(s.proto_require_expr).span, "require(...) expression must be bool");
            } else {
                auto eval_simple_bool = [&](auto&& self, ast::ExprId eid) -> std::optional<bool> {
                    if (eid == ast::k_invalid_expr || (size_t)eid >= ast_.exprs().size()) return std::nullopt;
                    const auto& e = ast_.expr(eid);
                    switch (e.kind) {
                        case ast::ExprKind::kBoolLit:
                            return e.text == "true";
                        case ast::ExprKind::kUnary: {
                            if (e.op != K::kBang && e.op != K::kKwNot) return std::nullopt;
                            auto v = self(self, e.a);
                            if (!v.has_value()) return std::nullopt;
                            return !(*v);
                        }
                        case ast::ExprKind::kBinary: {
                            if (e.op != K::kKwAnd && e.op != K::kKwOr) return std::nullopt;
                            auto lv = self(self, e.a);
                            auto rv = self(self, e.b);
                            if (!lv.has_value() || !rv.has_value()) return std::nullopt;
                            if (e.op == K::kKwAnd) return (*lv) && (*rv);
                            return (*lv) || (*rv);
                        }
                        default:
                            return std::nullopt;
                    }
                };

                auto v = eval_simple_bool(eval_simple_bool, s.proto_require_expr);
                if (!v.has_value()) {
                    diag_(diag::Code::kProtoRequireExprTooComplex, ast_.expr(s.proto_require_expr).span);
                    err_(ast_.expr(s.proto_require_expr).span,
                         "require(...) supports only true/false/not/and/or in v1");
                } else if (!(*v)) {
                    diag_(diag::Code::kProtoConstraintUnsatisfied, ast_.expr(s.proto_require_expr).span,
                          std::string(s.name));
                    err_(ast_.expr(s.proto_require_expr).span, "proto require(...) evaluated to false");
                }
            }
        }

        const auto& refs = ast_.path_refs();
        const uint32_t ib = s.decl_path_ref_begin;
        const uint32_t ie = s.decl_path_ref_begin + s.decl_path_ref_count;
        if (ib <= refs.size() && ie <= refs.size()) {
            for (uint32_t i = ib; i < ie; ++i) {
                const auto& pr = refs[i];
                const std::string path = path_ref_display_(pr);
                if (path.empty()) continue;
                bool typed_path_failure = false;
                if (!resolve_proto_decl_from_path_ref_(pr, pr.span, &typed_path_failure).has_value()) {
                    if (typed_path_failure) continue;
                    diag_(diag::Code::kProtoImplTargetNotSupported, pr.span, path);
                    err_(pr.span, "unknown base proto: " + path);
                }
            }
        }
    }

    void TypeChecker::check_stmt_class_decl_(ast::StmtId sid) {
        if (sid == ast::k_invalid_stmt || (size_t)sid >= ast_.stmts().size()) return;
        const ast::Stmt& s = ast_.stmt(sid);
        if (s.kind != ast::StmtKind::kClassDecl) return;
        if (generic_class_template_sid_set_.find(sid) != generic_class_template_sid_set_.end()) {
            return;
        }

        const ty::TypeId self_ty = (s.type == ty::kInvalidType)
            ? types_.intern_ident(s.name.empty() ? std::string("Self") : std::string(s.name))
            : s.type;

        if (self_ty != ty::kInvalidType) {
            FieldAbiMeta meta{};
            meta.sid = sid;
            meta.layout = ast::FieldLayout::kNone;
            meta.align = 0;
            field_abi_meta_by_type_[self_ty] = meta;
        }

        auto normalize_self = [&](ty::TypeId t) -> ty::TypeId {
            if (t == ty::kInvalidType) return t;
            const auto& tt = types_.get(t);
            if (tt.kind == ty::Kind::kNamedUser && types_.to_string(t) == "Self") {
                return self_ty;
            }
            if (tt.kind == ty::Kind::kBorrow) {
                const auto& et = types_.get(tt.elem);
                if (et.kind == ty::Kind::kNamedUser && types_.to_string(tt.elem) == "Self") {
                    return types_.make_borrow(self_ty, tt.borrow_is_mut);
                }
            }
            return t;
        };

        auto fn_sig_matches = [&](const ast::Stmt& req, const ast::Stmt& impl) -> bool {
            if (req.kind != ast::StmtKind::kFnDecl || impl.kind != ast::StmtKind::kFnDecl) return false;
            if (req.name != impl.name) return false;
            if (req.param_count != impl.param_count) return false;
            if (req.positional_param_count != impl.positional_param_count) return false;
            if (normalize_self(req.fn_ret) != normalize_self(impl.fn_ret)) return false;
            for (uint32_t i = 0; i < req.param_count; ++i) {
                const auto& rp = ast_.params()[req.param_begin + i];
                const auto& ip = ast_.params()[impl.param_begin + i];
                if (normalize_self(rp.type) != normalize_self(ip.type)) return false;
                if (rp.is_self != ip.is_self) return false;
                if (rp.self_kind != ip.self_kind) return false;
            }
            return true;
        };

        auto collect_required = [&](auto&& self,
                                    ast::StmtId proto_sid,
                                    std::vector<ast::StmtId>& out,
                                    std::unordered_set<ast::StmtId>& visiting) -> void {
            if (proto_sid == ast::k_invalid_stmt || (size_t)proto_sid >= ast_.stmts().size()) return;
            if (!visiting.insert(proto_sid).second) return;
            const auto& ps = ast_.stmt(proto_sid);
            if (ps.kind != ast::StmtKind::kProtoDecl) return;

            const auto& refs = ast_.path_refs();
            const uint32_t ib = ps.decl_path_ref_begin;
            const uint32_t ie = ps.decl_path_ref_begin + ps.decl_path_ref_count;
            if (ib <= refs.size() && ie <= refs.size()) {
                for (uint32_t i = ib; i < ie; ++i) {
                    const auto& pr = refs[i];
                    if (auto base_sid = resolve_proto_decl_from_path_ref_(pr, pr.span)) {
                        self(self, *base_sid, out, visiting);
                    }
                }
            }

            const auto& kids = ast_.stmt_children();
            const uint32_t mb = ps.stmt_begin;
            const uint32_t me = ps.stmt_begin + ps.stmt_count;
            if (mb <= kids.size() && me <= kids.size()) {
                for (uint32_t i = mb; i < me; ++i) {
                    const ast::StmtId msid = kids[i];
                    if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                    const auto& m = ast_.stmt(msid);
                    if (m.kind == ast::StmtKind::kFnDecl && m.a == ast::k_invalid_stmt) out.push_back(msid);
                }
            }
        };

        auto collect_default_members = [&](auto&& self,
                                           ast::StmtId proto_sid,
                                           std::vector<ast::StmtId>& out,
                                           std::unordered_set<ast::StmtId>& visiting) -> void {
            if (proto_sid == ast::k_invalid_stmt || (size_t)proto_sid >= ast_.stmts().size()) return;
            if (!visiting.insert(proto_sid).second) return;
            const auto& ps = ast_.stmt(proto_sid);
            if (ps.kind != ast::StmtKind::kProtoDecl) return;

            const auto& refs = ast_.path_refs();
            const uint32_t ib = ps.decl_path_ref_begin;
            const uint32_t ie = ps.decl_path_ref_begin + ps.decl_path_ref_count;
            if (ib <= refs.size() && ie <= refs.size()) {
                for (uint32_t i = ib; i < ie; ++i) {
                    const auto& pr = refs[i];
                    if (auto base_sid = resolve_proto_decl_from_path_ref_(pr, pr.span)) {
                        self(self, *base_sid, out, visiting);
                    }
                }
            }

            const auto& kids = ast_.stmt_children();
            const uint32_t mb = ps.stmt_begin;
            const uint32_t me = ps.stmt_begin + ps.stmt_count;
            if (mb <= kids.size() && me <= kids.size()) {
                for (uint32_t i = mb; i < me; ++i) {
                    const ast::StmtId msid = kids[i];
                    if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                    const auto& m = ast_.stmt(msid);
                    if (m.kind == ast::StmtKind::kFnDecl && m.a != ast::k_invalid_stmt) out.push_back(msid);
                }
            }
        };

        std::unordered_map<std::string, std::vector<ast::StmtId>> impl_methods;
        std::unordered_map<std::string, std::vector<ast::StmtId>> local_overload_sets;
        std::vector<ast::StmtId> active_default_members;
        std::unordered_set<std::string> class_member_names;
        std::unordered_set<std::string> class_method_names;

        {
            const uint64_t fmb = s.field_member_begin;
            const uint64_t fme = fmb + s.field_member_count;
            if (fmb <= ast_.field_members().size() && fme <= ast_.field_members().size()) {
                for (uint32_t i = s.field_member_begin; i < s.field_member_begin + s.field_member_count; ++i) {
                    const auto& fm = ast_.field_members()[i];
                    const std::string key(fm.name);
                    if (!class_member_names.insert(key).second) {
                        diag_(diag::Code::kDuplicateDecl, fm.span, fm.name);
                        err_(fm.span, "duplicate class member name");
                        continue;
                    }

                }
            } else {
                diag_(diag::Code::kTypeFieldMemberRangeInvalid, s.span);
                err_(s.span, "invalid class field member range");
            }
        }

        const auto& kids = ast_.stmt_children();
        const uint32_t begin = s.stmt_begin;
        const uint32_t end = s.stmt_begin + s.stmt_count;
        if (begin <= kids.size() && end <= kids.size()) {
            for (uint32_t i = begin; i < end; ++i) {
                const ast::StmtId msid = kids[i];
                if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                const auto& m = ast_.stmt(msid);
                if (m.kind == ast::StmtKind::kFnDecl) {
                    const std::string key(m.name);
                    if (class_member_names.find(key) != class_member_names.end()) {
                        diag_(diag::Code::kDuplicateDecl, m.span, m.name);
                        err_(m.span, "duplicate class member name");
                    }
                    class_method_names.insert(key);
                    impl_methods[std::string(m.name)].push_back(msid);
                } else if (m.kind == ast::StmtKind::kVar && m.is_static) {
                    const std::string key(m.name);
                    if (class_member_names.find(key) != class_member_names.end() ||
                        class_method_names.find(key) != class_method_names.end()) {
                        diag_(diag::Code::kDuplicateDecl, m.span, m.name);
                        err_(m.span, "duplicate class member name");
                    } else {
                        class_member_names.insert(key);
                    }
                }
            }
        }

        local_overload_sets = impl_methods;
        const auto& refs = ast_.path_refs();
        const uint32_t pb = s.decl_path_ref_begin;
        const uint32_t pe = s.decl_path_ref_begin + s.decl_path_ref_count;
        std::vector<std::optional<ast::StmtId>> resolved_impl_proto_sids{};
        std::vector<uint8_t> resolved_impl_proto_typed_failure{};
        if (pb <= refs.size() && pe <= refs.size()) {
            const uint32_t count = pe - pb;
            resolved_impl_proto_sids.resize(count);
            resolved_impl_proto_typed_failure.resize(count, 0u);
            for (uint32_t i = pb; i < pe; ++i) {
                bool typed_path_failure = false;
                resolved_impl_proto_sids[i - pb] = resolve_proto_decl_from_path_ref_(refs[i], refs[i].span, &typed_path_failure);
                resolved_impl_proto_typed_failure[i - pb] = typed_path_failure ? 1u : 0u;
            }
        }
        {
            if (pb <= refs.size() && pe <= refs.size()) {
                for (uint32_t i = pb; i < pe; ++i) {
                    const auto& proto_sid = resolved_impl_proto_sids[i - pb];
                    if (!proto_sid.has_value()) continue;

                    std::vector<ast::StmtId> defaults;
                    std::unordered_set<ast::StmtId> visiting;
                    collect_default_members(collect_default_members, *proto_sid, defaults, visiting);
                    for (const ast::StmtId def_sid : defaults) {
                        if (def_sid == ast::k_invalid_stmt || (size_t)def_sid >= ast_.stmts().size()) continue;
                        const auto& def = ast_.stmt(def_sid);
                        if (def.kind != ast::StmtKind::kFnDecl) continue;

                        bool overridden = false;
                        auto mit = impl_methods.find(std::string(def.name));
                        if (mit != impl_methods.end()) {
                            for (const auto impl_sid : mit->second) {
                                if (impl_sid == ast::k_invalid_stmt || (size_t)impl_sid >= ast_.stmts().size()) continue;
                                if (fn_sig_matches(def, ast_.stmt(impl_sid))) {
                                    overridden = true;
                                    break;
                                }
                            }
                        }
                        if (overridden) continue;

                        auto& slot = local_overload_sets[std::string(def.name)];
                        bool dup_sig = false;
                        for (const auto cur_sid : slot) {
                            if (cur_sid == ast::k_invalid_stmt || (size_t)cur_sid >= ast_.stmts().size()) continue;
                            if (fn_sig_matches(def, ast_.stmt(cur_sid))) {
                                dup_sig = true;
                                break;
                            }
                        }
                        if (dup_sig) continue;

                        slot.push_back(def_sid);
                        active_default_members.push_back(def_sid);
                    }
                }
            }
        }

        if (self_ty != ty::kInvalidType) {
            class_effective_method_map_[self_ty] = local_overload_sets;
        }

        struct FnOverloadBackup {
            bool had_key = false;
            std::vector<ast::StmtId> prev;
        };
        std::unordered_map<std::string, FnOverloadBackup> overload_backups;
        overload_backups.reserve(local_overload_sets.size());
        for (const auto& ent : local_overload_sets) {
            FnOverloadBackup bk{};
            auto fit = fn_decl_by_name_.find(ent.first);
            if (fit != fn_decl_by_name_.end()) {
                bk.had_key = true;
                bk.prev = fit->second;
            }
            overload_backups.emplace(ent.first, std::move(bk));
            fn_decl_by_name_[ent.first] = ent.second;
        }

        struct FnTypeBackup {
            ast::StmtId sid = ast::k_invalid_stmt;
            ty::TypeId old_ret = ty::kInvalidType;
            ty::TypeId old_type = ty::kInvalidType;
            std::vector<ty::TypeId> old_param_types;
        };
        std::vector<FnTypeBackup> default_type_backups;
        {
            std::unordered_set<ast::StmtId> seen;
            seen.reserve(active_default_members.size());
            for (const ast::StmtId sid_def : active_default_members) {
                if (!seen.insert(sid_def).second) continue;
                if (sid_def == ast::k_invalid_stmt || (size_t)sid_def >= ast_.stmts().size()) continue;
                auto& def = ast_.stmt_mut(sid_def);
                if (def.kind != ast::StmtKind::kFnDecl) continue;

                FnTypeBackup bk{};
                bk.sid = sid_def;
                bk.old_ret = def.fn_ret;
                bk.old_type = def.type;
                bk.old_param_types.reserve(def.param_count);
                for (uint32_t pi = 0; pi < def.param_count; ++pi) {
                    auto& p = ast_.params_mut()[def.param_begin + pi];
                    bk.old_param_types.push_back(p.type);
                    p.type = normalize_self(p.type);
                }
                def.fn_ret = normalize_self(def.fn_ret);

                std::vector<ty::TypeId> params;
                std::vector<std::string_view> labels;
                std::vector<uint8_t> has_default_flags;
                params.reserve(def.param_count);
                labels.reserve(def.param_count);
                has_default_flags.reserve(def.param_count);
                for (uint32_t pi = 0; pi < def.param_count; ++pi) {
                    const auto& p = ast_.params()[def.param_begin + pi];
                    params.push_back(p.type == ty::kInvalidType ? types_.error() : p.type);
                    labels.push_back(p.name);
                    has_default_flags.push_back(p.has_default ? 1u : 0u);
                }
                ty::TypeId ret = def.fn_ret;
                if (ret == ty::kInvalidType) ret = types_.builtin(ty::Builtin::kUnit);
                def.type = types_.make_fn(
                    ret,
                    params.empty() ? nullptr : params.data(),
                    static_cast<uint32_t>(params.size()),
                    def.positional_param_count,
                    labels.empty() ? nullptr : labels.data(),
                    has_default_flags.empty() ? nullptr : has_default_flags.data()
                );
                default_type_backups.push_back(std::move(bk));
            }
        }

        sym_.push_scope();
        if (begin <= kids.size() && end <= kids.size()) {
            // predeclare class member symbols
            for (uint32_t i = begin; i < end; ++i) {
                const ast::StmtId msid = kids[i];
                if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                const auto& m = ast_.stmt(msid);
                if (m.kind == ast::StmtKind::kFnDecl) {
                    (void)sym_.insert(sema::SymbolKind::kFn, m.name, m.type, m.span);
                } else if (m.kind == ast::StmtKind::kVar && m.is_static) {
                    ty::TypeId vt = (m.type == ty::kInvalidType) ? types_.error() : m.type;
                    auto ins = sym_.insert(sema::SymbolKind::kVar, m.name, vt, m.span);
                    if (!ins.ok && ins.is_duplicate) {
                        diag_(diag::Code::kDuplicateDecl, m.span, m.name);
                        err_(m.span, "duplicate class member name");
                    }
                }
            }

            // predeclare proto default members (if not overridden by class members)
            for (const auto& ent : local_overload_sets) {
                if (ent.second.empty()) continue;
                if (sym_.lookup_in_current(ent.first).has_value()) continue;
                const ast::StmtId msid = ent.second.front();
                if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                const auto& m = ast_.stmt(msid);
                (void)sym_.insert(sema::SymbolKind::kFn, ent.first, m.type, m.span);
            }

            for (uint32_t i = begin; i < end; ++i) {
                const ast::StmtId msid = kids[i];
                if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                const auto& m = ast_.stmt(msid);
                if (m.kind == ast::StmtKind::kFnDecl) {
                    check_stmt_fn_decl_(msid, m);
                } else if (m.kind == ast::StmtKind::kVar && m.is_static) {
                    if (m.init == ast::k_invalid_expr) {
                        diag_(diag::Code::kClassStaticVarRequiresInitializer, m.span);
                        err_(m.span, "class static variable requires initializer");
                    } else {
                        const CoercionPlan init_plan = classify_assign_with_coercion_(
                            AssignSite::LetInit, m.type, m.init, m.span);
                        const ty::TypeId init_t = init_plan.src_after;
                        if (m.type != ty::kInvalidType && !init_plan.ok) {
                            diag_(diag::Code::kTypeLetInitMismatch, m.span,
                                  m.name, types_.to_string(m.type), type_for_user_diag_(init_t, m.init));
                            err_(m.span, "class static init mismatch");
                        }
                    }
                }
            }
        }
        sym_.pop_scope();

        for (auto it = default_type_backups.rbegin(); it != default_type_backups.rend(); ++it) {
            if (it->sid == ast::k_invalid_stmt || (size_t)it->sid >= ast_.stmts().size()) continue;
            auto& def = ast_.stmt_mut(it->sid);
            def.fn_ret = it->old_ret;
            def.type = it->old_type;
            const uint32_t n = std::min<uint32_t>(def.param_count, static_cast<uint32_t>(it->old_param_types.size()));
            for (uint32_t pi = 0; pi < n; ++pi) {
                ast_.params_mut()[def.param_begin + pi].type = it->old_param_types[pi];
            }
        }

        for (auto& ent : overload_backups) {
            if (ent.second.had_key) {
                fn_decl_by_name_[ent.first] = std::move(ent.second.prev);
            } else {
                fn_decl_by_name_.erase(ent.first);
            }
        }

        // Implements validation: class : ProtoA, ProtoB
        auto is_non_proto_base = [&](std::string_view raw) -> bool {
            if (raw.empty()) return false;
            std::string key(raw);
            if (auto rewritten = rewrite_imported_path_(key)) {
                key = *rewritten;
            }
            if (proto_decl_by_name_.find(key) != proto_decl_by_name_.end()) return false;
            if (auto sid2 = lookup_symbol_(key)) {
                const auto& ss = sym_.symbol(*sid2);
                if (ss.kind == sema::SymbolKind::kType &&
                    proto_decl_by_name_.find(ss.name) == proto_decl_by_name_.end()) {
                    return true;
                }
            }
            return false;
        };
        if (pb <= refs.size() && pe <= refs.size()) {
            for (uint32_t i = pb; i < pe; ++i) {
                const auto& pr = refs[i];
                const std::string proto_path = path_ref_display_(pr);
                const bool typed_path_failure = (i - pb < resolved_impl_proto_typed_failure.size())
                    ? (resolved_impl_proto_typed_failure[i - pb] != 0u)
                    : false;
                const auto proto_sid = (i - pb < resolved_impl_proto_sids.size())
                    ? resolved_impl_proto_sids[i - pb]
                    : std::optional<ast::StmtId>{};
                if (!proto_sid.has_value()) {
                    if (typed_path_failure) continue;
                    if (is_non_proto_base(proto_path)) {
                        diag_(diag::Code::kClassInheritanceNotAllowed, pr.span, proto_path);
                        err_(pr.span, "class inheritance is not allowed: " + proto_path);
                    } else {
                        diag_(diag::Code::kProtoImplTargetNotSupported, pr.span, proto_path);
                        err_(pr.span, "unknown proto target: " + proto_path);
                    }
                    continue;
                }

                std::vector<ast::StmtId> required;
                std::unordered_set<ast::StmtId> visiting;
                collect_required(collect_required, *proto_sid, required, visiting);
                for (const ast::StmtId req_sid : required) {
                    if (req_sid == ast::k_invalid_stmt || (size_t)req_sid >= ast_.stmts().size()) continue;
                    const auto& req = ast_.stmt(req_sid);
                    auto mit = impl_methods.find(std::string(req.name));
                    bool matched = false;
                    if (mit != impl_methods.end()) {
                        for (const auto cand_sid : mit->second) {
                            if (cand_sid == ast::k_invalid_stmt || (size_t)cand_sid >= ast_.stmts().size()) continue;
                            if (fn_sig_matches(req, ast_.stmt(cand_sid))) {
                                matched = true;
                                break;
                            }
                        }
                    }
                    if (!matched) {
                        diag_(diag::Code::kProtoImplMissingMember, req.span, req.name);
                        err_(req.span, "missing proto member implementation: " + std::string(req.name));
                    }
                }
            }
        }
    }

    void TypeChecker::check_stmt_actor_decl_(ast::StmtId sid) {
        if (sid == ast::k_invalid_stmt || (size_t)sid >= ast_.stmts().size()) return;
        const ast::Stmt& s = ast_.stmt(sid);
        if (s.kind != ast::StmtKind::kActorDecl) return;
        if (s.decl_generic_param_count > 0) {
            diag_(diag::Code::kGenericActorDeclNotSupportedV1, s.span, s.name);
            err_(s.span, "generic actor declaration is not supported in v1");
            return;
        }

        const ty::TypeId self_ty = (s.type == ty::kInvalidType)
            ? types_.intern_ident(s.name.empty() ? std::string("Self") : std::string(s.name))
            : s.type;

        if (self_ty != ty::kInvalidType) {
            FieldAbiMeta meta{};
            meta.sid = sid;
            meta.layout = ast::FieldLayout::kNone;
            meta.align = 0;
            field_abi_meta_by_type_[self_ty] = meta;
        }

        std::unordered_set<std::string> actor_member_names;
        std::unordered_map<std::string, std::vector<ast::StmtId>> impl_methods;
        std::unordered_set<std::string> actor_method_names;

        {
            const uint64_t fmb = s.field_member_begin;
            const uint64_t fme = fmb + s.field_member_count;
            if (fmb <= ast_.field_members().size() && fme <= ast_.field_members().size()) {
                for (uint32_t i = s.field_member_begin; i < s.field_member_begin + s.field_member_count; ++i) {
                    const auto& fm = ast_.field_members()[i];
                    const std::string key(fm.name);
                    if (!actor_member_names.insert(key).second) {
                        diag_(diag::Code::kDuplicateDecl, fm.span, fm.name);
                        err_(fm.span, "duplicate actor draft member name");
                    }
                }
            } else {
                diag_(diag::Code::kTypeFieldMemberRangeInvalid, s.span);
                err_(s.span, "invalid actor draft member range");
            }
        }

        const auto& kids = ast_.stmt_children();
        const uint32_t begin = s.stmt_begin;
        const uint32_t end = s.stmt_begin + s.stmt_count;
        if (begin <= kids.size() && end <= kids.size()) {
            for (uint32_t i = begin; i < end; ++i) {
                const ast::StmtId msid = kids[i];
                if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                const auto& m = ast_.stmt(msid);
                if (m.kind != ast::StmtKind::kFnDecl) continue;

                const std::string key(m.name);
                if (actor_member_names.find(key) != actor_member_names.end()) {
                    diag_(diag::Code::kDuplicateDecl, m.span, m.name);
                    err_(m.span, "duplicate actor member name");
                }
                actor_method_names.insert(key);
                impl_methods[std::string(m.name)].push_back(msid);

                if (m.name != "init" && m.fn_mode == ast::FnMode::kNone) {
                    diag_(diag::Code::kActorMethodModeRequired, m.span);
                    err_(m.span, "actor method requires mode sub/pub");
                }
            }
        }

        if (self_ty != ty::kInvalidType) {
            actor_method_map_[self_ty] = impl_methods;
        }

        sym_.push_scope();
        if (begin <= kids.size() && end <= kids.size()) {
            for (uint32_t i = begin; i < end; ++i) {
                const ast::StmtId msid = kids[i];
                if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                const auto& m = ast_.stmt(msid);
                if (m.kind != ast::StmtKind::kFnDecl) continue;
                (void)sym_.insert(sema::SymbolKind::kFn, m.name, m.type, m.span);
            }

            for (uint32_t i = begin; i < end; ++i) {
                const ast::StmtId msid = kids[i];
                if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                const auto& m = ast_.stmt(msid);
                if (m.kind != ast::StmtKind::kFnDecl) continue;

                const bool was_in_actor_method = in_actor_method_;
                const bool was_in_actor_pub = in_actor_pub_method_;
                const bool was_in_actor_sub = in_actor_sub_method_;

                in_actor_method_ = true;
                in_actor_pub_method_ = (m.fn_mode == ast::FnMode::kPub);
                in_actor_sub_method_ = (m.fn_mode == ast::FnMode::kSub);

                check_stmt_fn_decl_(msid, m);

                if (m.fn_mode == ast::FnMode::kPub) {
                    bool has_top_level_commit = false;
                    if (m.a != ast::k_invalid_stmt && (size_t)m.a < ast_.stmts().size()) {
                        const auto& body = ast_.stmt(m.a);
                        if (body.kind == ast::StmtKind::kBlock) {
                            const auto& body_kids = ast_.stmt_children();
                            const uint64_t bb = body.stmt_begin;
                            const uint64_t be = bb + body.stmt_count;
                            if (bb <= body_kids.size() && be <= body_kids.size()) {
                                for (uint32_t bi = body.stmt_begin; bi < body.stmt_begin + body.stmt_count; ++bi) {
                                    const ast::StmtId bcid = body_kids[bi];
                                    if (bcid == ast::k_invalid_stmt || (size_t)bcid >= ast_.stmts().size()) continue;
                                    if (ast_.stmt(bcid).kind == ast::StmtKind::kCommitStmt) {
                                        has_top_level_commit = true;
                                        break;
                                    }
                                }
                            }
                        }
                    }
                    if (!has_top_level_commit) {
                        diag_(diag::Code::kActorPubMissingTopLevelCommit, m.span);
                        err_(m.span, "actor pub method requires top-level commit");
                    }
                }

                in_actor_method_ = was_in_actor_method;
                in_actor_pub_method_ = was_in_actor_pub;
                in_actor_sub_method_ = was_in_actor_sub;
            }
        }
        sym_.pop_scope();
    }

    /// @brief field 선언의 멤버 타입 제약(POD 값 타입만 허용)을 검사한다.
    void TypeChecker::check_stmt_field_decl_(ast::StmtId sid) {
        const ast::Stmt& s = ast_.stmt(sid);
        const bool is_generic_template =
            (s.decl_generic_param_count > 0) &&
            (generic_field_template_sid_set_.find(sid) != generic_field_template_sid_set_.end());

        if (s.field_align != 0) {
            if ((s.field_align & (s.field_align - 1u)) != 0u) {
                const std::string msg = "field align(n) must be a power of two";
                diag_(diag::Code::kTypeErrorGeneric, s.span, msg);
                err_(s.span, msg);
            }
        }

        const uint32_t begin = s.field_member_begin;
        const uint32_t end = s.field_member_begin + s.field_member_count;
        if (begin > ast_.field_members().size() || end > ast_.field_members().size() || begin > end) {
            diag_(diag::Code::kTypeFieldMemberRangeInvalid, s.span);
            err_(s.span, "invalid field member range");
            return;
        }

        if (is_generic_template) {
            return;
        }

        for (uint32_t i = begin; i < end; ++i) {
            const auto& m = ast_.field_members()[i];
            const bool optional_member = is_optional_(m.type);

            if (s.field_layout == ast::FieldLayout::kC && optional_member) {
                std::ostringstream oss;
                oss << "layout(c) field member '" << m.name
                    << "' must not use optional type in v0";
                diag_(diag::Code::kAbiCTypeNotFfiSafe, m.span,
                      std::string("field member '") + std::string(m.name) + "'",
                      types_.to_string(m.type));
                err_(m.span, oss.str());
                continue;
            }

            const bool member_ok = (s.field_layout == ast::FieldLayout::kC)
                ? is_c_abi_safe_type_(m.type, /*allow_void=*/false)
                : is_field_pod_value_type_(types_, m.type);

            if (member_ok) {
                continue;
            }

            std::ostringstream oss;
            if (s.field_layout == ast::FieldLayout::kC) {
                oss << "layout(c) field member '" << m.name
                    << "' must use a C ABI FFI-safe type, got "
                    << types_.to_string(m.type);
                diag_(diag::Code::kAbiCTypeNotFfiSafe, m.span,
                      std::string("field member '") + std::string(m.name) + "'",
                      types_.to_string(m.type));
            } else {
                oss << "field member '" << m.name
                    << "' must use a POD value builtin type (e.g., i32/u32/f32/bool/char), got "
                    << types_.to_string(m.type);
                diag_(diag::Code::kTypeFieldMemberMustBePodBuiltin, m.span, m.name, types_.to_string(m.type));
            }
            err_(m.span, oss.str());
        }

        ty::TypeId self_ty = s.type;
        if (self_ty == ty::kInvalidType && !s.name.empty()) {
            self_ty = types_.intern_ident(s.name);
        }
        if (self_ty != ty::kInvalidType) {
            FieldAbiMeta meta{};
            meta.sid = sid;
            meta.layout = s.field_layout;
            meta.align = s.field_align;
            field_abi_meta_by_type_[self_ty] = meta;
        }

        // Implements validation for `field Name : ProtoA, ProtoB`
        auto collect_required = [&](auto&& self,
                                    ast::StmtId proto_sid,
                                    std::vector<ast::StmtId>& out,
                                    std::unordered_set<ast::StmtId>& visiting) -> void {
            if (proto_sid == ast::k_invalid_stmt || (size_t)proto_sid >= ast_.stmts().size()) return;
            if (!visiting.insert(proto_sid).second) return;
            const auto& ps = ast_.stmt(proto_sid);
            if (ps.kind != ast::StmtKind::kProtoDecl) return;

            const auto& refs = ast_.path_refs();
            const uint32_t ib = ps.decl_path_ref_begin;
            const uint32_t ie = ps.decl_path_ref_begin + ps.decl_path_ref_count;
            if (ib <= refs.size() && ie <= refs.size()) {
                for (uint32_t i = ib; i < ie; ++i) {
                    const auto& pr = refs[i];
                    if (auto base_sid = resolve_proto_decl_from_path_ref_(pr, pr.span)) {
                        self(self, *base_sid, out, visiting);
                    }
                }
            }

            const auto& kids = ast_.stmt_children();
            const uint32_t mb = ps.stmt_begin;
            const uint32_t me = ps.stmt_begin + ps.stmt_count;
            if (mb <= kids.size() && me <= kids.size()) {
                for (uint32_t i = mb; i < me; ++i) {
                    const ast::StmtId msid = kids[i];
                    if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                    const auto& m = ast_.stmt(msid);
                    if (m.kind == ast::StmtKind::kFnDecl && m.a == ast::k_invalid_stmt) out.push_back(msid);
                }
            }
        };

        const auto& refs = ast_.path_refs();
        const uint32_t pb = s.decl_path_ref_begin;
        const uint32_t pe = s.decl_path_ref_begin + s.decl_path_ref_count;
        if (pb <= refs.size() && pe <= refs.size()) {
            for (uint32_t i = pb; i < pe; ++i) {
                const auto& pr = refs[i];
                const std::string proto_path = path_ref_display_(pr);
                bool typed_path_failure = false;
                const auto proto_sid = resolve_proto_decl_from_path_ref_(pr, pr.span, &typed_path_failure);
                if (!proto_sid.has_value()) {
                    if (typed_path_failure) continue;
                    diag_(diag::Code::kProtoImplTargetNotSupported, pr.span, proto_path);
                    err_(pr.span, "unknown proto target: " + proto_path);
                    continue;
                }

                std::vector<ast::StmtId> required;
                std::unordered_set<ast::StmtId> visiting;
                collect_required(collect_required, *proto_sid, required, visiting);
                for (const ast::StmtId req_sid : required) {
                    if (req_sid == ast::k_invalid_stmt || (size_t)req_sid >= ast_.stmts().size()) continue;
                    const auto& req = ast_.stmt(req_sid);
                    diag_(diag::Code::kProtoImplMissingMember, req.span, req.name);
                    err_(req.span, "field does not provide proto member: " + std::string(req.name));
                }
            }
        }
    }

    /// @brief acts 선언 내부의 함수 멤버를 타입 체크한다.
    void TypeChecker::check_stmt_acts_decl_(ast::StmtId sid, const ast::Stmt& s) {
        if (sid != ast::k_invalid_stmt &&
            generic_acts_template_sid_set_.find(sid) != generic_acts_template_sid_set_.end()) {
            return;
        }

        sym_.push_scope();

        if (s.acts_is_for) {
            bool owner_ok = false;
            const ty::TypeId owner_type = canonicalize_acts_owner_type_(s.acts_target_type);
            if (owner_type != ty::kInvalidType) {
                if (class_decl_by_type_.find(owner_type) != class_decl_by_type_.end() ||
                    field_abi_meta_by_type_.find(owner_type) != field_abi_meta_by_type_.end()) {
                    owner_ok = true;
                } else {
                    const auto& owner_ty = types_.get(owner_type);
                    if (owner_ty.kind == ty::Kind::kNamedUser) {
                        const std::string owner_name = types_.to_string(owner_type);
                        if (auto owner_sym = lookup_symbol_(owner_name)) {
                            const auto& ss = sym_.symbol(*owner_sym);
                            owner_ok = (ss.kind == sema::SymbolKind::kField ||
                                        ss.kind == sema::SymbolKind::kType);
                        }
                    }
                }
            }

            if (!owner_ok) {
                std::ostringstream oss;
                oss << "acts-for target must be a field/class type in v0, got "
                    << types_.to_string(owner_type);
                diag_(diag::Code::kTypeErrorGeneric, s.span, oss.str());
                err_(s.span, oss.str());
            }
        }

        const auto& kids = ast_.stmt_children();
        const uint32_t begin = s.stmt_begin;
        const uint32_t end = s.stmt_begin + s.stmt_count;

        // acts 멤버 함수의 상호 참조를 위해 먼저 시그니처를 predeclare한다.
        if (begin < kids.size() && end <= kids.size()) {
            for (uint32_t i = begin; i < end; ++i) {
                const auto sid = kids[i];
                if (sid == ast::k_invalid_stmt) continue;
                const auto& member = ast_.stmt(sid);
                if (member.kind != ast::StmtKind::kFnDecl) continue;

                if (!member.fn_is_operator) {
                    if (s.acts_is_for) {
                        if (member.param_count == 0) {
                            diag_(diag::Code::kTypeErrorGeneric, member.span,
                                  "acts-for member requires a self receiver as the first parameter");
                            err_(member.span, "acts-for member requires a self receiver");
                        } else {
                            const auto& p0 = ast_.params()[member.param_begin];
                            if (!p0.is_self) {
                                diag_(diag::Code::kTypeErrorGeneric, p0.span,
                                      "acts-for member requires 'self' as first parameter");
                                err_(p0.span, "acts-for member requires 'self' as first parameter");
                            } else if (s.acts_target_type != ty::kInvalidType &&
                                       !type_matches_acts_owner_(types_, s.acts_target_type, p0.type)) {
                                const std::string msg = "self receiver type must match acts target type";
                                diag_(diag::Code::kTypeErrorGeneric, p0.span, msg);
                                err_(p0.span, msg);
                            }
                        }
                    } else {
                        if (member.param_count > 0) {
                            const auto& p0 = ast_.params()[member.param_begin];
                            if (p0.is_self) {
                                diag_(diag::Code::kTypeErrorGeneric, p0.span,
                                      "general acts namespace members must not declare a self receiver");
                                err_(p0.span, "general acts namespace members must not use self");
                            }
                        }
                    }
                }

                if (member.fn_is_operator) {
                    if (!s.acts_is_for) {
                        diag_(diag::Code::kOperatorDeclOnlyInActsFor, member.span);
                        err_(member.span, "operator declarations are only allowed in acts-for declarations");
                    }
                    if (member.param_count == 0) {
                        diag_(diag::Code::kOperatorSelfFirstParamRequired, member.span);
                        err_(member.span, "operator declaration requires a self receiver");
                    } else {
                        const auto& p0 = ast_.params()[member.param_begin];
                        if (!p0.is_self) {
                            diag_(diag::Code::kOperatorSelfFirstParamRequired, p0.span);
                            err_(p0.span, "operator first parameter must be marked with self");
                        } else if (s.acts_is_for && s.acts_target_type != ty::kInvalidType &&
                                   !type_matches_acts_owner_(types_, s.acts_target_type, p0.type)) {
                            std::string msg = "operator self type must match acts target type";
                            diag_(diag::Code::kTypeErrorGeneric, p0.span, msg);
                            err_(p0.span, msg);
                        }
                    }
                }

                auto ins = sym_.insert(sema::SymbolKind::kFn, member.name, member.type, member.span);
                if (!ins.ok && ins.is_duplicate) {
                    diag_(diag::Code::kDuplicateDecl, member.span, member.name);
                    err_(member.span, "duplicate acts member function name");
                }
            }

            for (uint32_t i = begin; i < end; ++i) {
                const auto sid = kids[i];
                if (sid == ast::k_invalid_stmt) continue;
                const auto& member = ast_.stmt(sid);
                if (member.kind != ast::StmtKind::kFnDecl) continue;
                check_stmt_fn_decl_(sid, member);
            }
        }

        sym_.pop_scope();
    }

    // --------------------
    // expr: memoized dispatcher
    // --------------------

} // namespace parus::tyck
