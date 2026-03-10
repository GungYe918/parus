// frontend/src/tyck/type_check_stmt.cpp
#include <parus/tyck/TypeCheck.hpp>
#include <parus/syntax/TokenKind.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/diag/DiagCode.hpp>
#include "../common/type_check_literals.hpp"

#include <algorithm>
#include <cerrno>
#include <cctype>
#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>
#include <unordered_set>


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

            case ast::StmtKind::kRequire:
                check_stmt_require_(s);
                return;

            case ast::StmtKind::kThrow:
                check_stmt_throw_(s);
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

            case ast::StmtKind::kTryCatch:
                check_stmt_try_catch_(s);
                return;

            case ast::StmtKind::kFnDecl:
                check_stmt_fn_decl_(sid, s);
                return;

            case ast::StmtKind::kFieldDecl:
                check_stmt_field_decl_(sid);
                return;

            case ast::StmtKind::kEnumDecl:
                check_stmt_enum_decl_(sid);
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
            (s.is_static || s.is_const || s.is_extern || s.is_export || (s.link_abi == ast::LinkAbi::kC));
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
        // const: immutable + compile-time evaluable initializer
        // ----------------------------------------
        if (s.is_const) {
            if (s.type == ty::kInvalidType) {
                diag_(diag::Code::kVarDeclTypeAnnotationRequired, s.span);
                err_(s.span, "const requires an explicit declared type");
            }
            if (s.init == ast::k_invalid_expr) {
                if (s.is_static) {
                    diag_(diag::Code::kStaticVarRequiresInitializer, s.span);
                } else {
                    diag_(diag::Code::kVarDeclInitializerExpected, s.span);
                }
                err_(s.span, "const declaration requires initializer");
            }

            ty::TypeId init_t = ty::kInvalidType;
            if (s.init != ast::k_invalid_expr) {
                const CoercionPlan init_plan = classify_assign_with_coercion_(
                    AssignSite::LetInit, s.type, s.init, s.span);
                init_t = init_plan.src_after;
                if (s.type != ty::kInvalidType && !init_plan.ok) {
                    diag_(diag::Code::kTypeLetInitMismatch, s.span,
                        s.name, types_.to_string(s.type), type_for_user_diag_(init_t, s.init));
                    err_(s.span, "const init mismatch");
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
                        err_(s.span, "duplicate symbol (const): " + decl_name);
                    }
                }
            }

            if (var_sym == sema::SymbolTable::kNoScope) {
                auto ins = sym_.insert(sema::SymbolKind::kVar, decl_name, vt, s.span);
                if (!ins.ok) {
                    if (ins.is_duplicate) {
                        diag_(diag::Code::kDuplicateDecl, s.span, decl_name);
                        err_(s.span, "duplicate symbol (const): " + decl_name);
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
                sym_is_mut_[var_sym] = false;
                const_symbol_decl_sid_[var_sym] = sid;
                if (s.init != ast::k_invalid_expr) {
                    ConstInitData init_value{};
                    if (eval_const_symbol_(var_sym, init_value, s.span)) {
                        result_.const_symbol_values[var_sym] = init_value;
                    }
                }
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
            if (var_sym != sema::SymbolTable::kNoScope && !is_global_decl && is_move_only_type_(vt)) {
                if (s.init != ast::k_invalid_expr) {
                    mark_expr_move_consumed_(s.init, vt, s.span);
                    mark_symbol_initialized_(var_sym);
                } else {
                    ownership_state_[var_sym] = OwnershipState::kMovedUninitialized;
                }
            }
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
        if (ins.ok && !is_global_decl && is_move_only_type_(inferred)) {
            mark_expr_move_consumed_(s.init, inferred, s.span);
            mark_symbol_initialized_(ins.symbol_id);
        }
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
        const OwnershipStateMap before = capture_ownership_state_();
        std::vector<OwnershipStateMap> branches{};
        if (s.a != ast::k_invalid_stmt) {
            restore_ownership_state_(before);
            check_stmt_(s.a);
            branches.push_back(capture_ownership_state_());
        }
        if (s.b != ast::k_invalid_stmt) {
            restore_ownership_state_(before);
            check_stmt_(s.b);
            branches.push_back(capture_ownership_state_());
        }
        restore_ownership_state_(before);
        merge_ownership_state_from_branches_(before, branches, /*include_before_as_fallthrough=*/s.b == ast::k_invalid_stmt);
    }

    void TypeChecker::check_stmt_while_(const ast::Stmt& s) {
        if (s.expr != ast::k_invalid_expr) {
            ty::TypeId ct = check_expr_(s.expr);
            if (ct != types_.builtin(ty::Builtin::kBool) && !is_error_(ct)) {
                diag_(diag::Code::kTypeCondMustBeBool, ast_.expr(s.expr).span, types_.to_string(ct));
                err_(s.span, "while condition must be bool");
            }
        }
        const OwnershipStateMap before = capture_ownership_state_();
        std::vector<OwnershipStateMap> branches{};
        if (s.a != ast::k_invalid_stmt) {
            restore_ownership_state_(before);
            ++stmt_loop_depth_;
            check_stmt_(s.a);
            if (stmt_loop_depth_ > 0) --stmt_loop_depth_;
            branches.push_back(capture_ownership_state_());
        }
        restore_ownership_state_(before);
        merge_ownership_state_from_branches_(before, branches, /*include_before_as_fallthrough=*/true);
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
        const OwnershipStateMap before = capture_ownership_state_();
        std::vector<OwnershipStateMap> branches{};
        if (s.a != ast::k_invalid_stmt) {
            restore_ownership_state_(before);
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
        branches.push_back(capture_ownership_state_());
        restore_ownership_state_(before);
        merge_ownership_state_from_branches_(before, branches, /*include_before_as_fallthrough=*/false);
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
            return;
        }
        mark_expr_move_consumed_(s.expr, rt, s.span);
    }

    void TypeChecker::check_stmt_require_(const ast::Stmt& s) {
        if (s.expr == ast::k_invalid_expr) {
            diag_(diag::Code::kRequireExprTypeNotBool, s.span);
            err_(s.span, "require(expr) statement requires a bool expression");
            return;
        }

        const ProtoRequireEvalResult status = eval_proto_require_const_bool_(s.expr);
        switch (status) {
            case ProtoRequireEvalResult::kTrue:
                return;

            case ProtoRequireEvalResult::kFalse:
                diag_(diag::Code::kRequireUnsatisfied, ast_.expr(s.expr).span);
                err_(ast_.expr(s.expr).span, "compile-time require(expr) evaluated to false");
                return;

            case ProtoRequireEvalResult::kTypeNotBool:
                diag_(diag::Code::kRequireExprTypeNotBool, ast_.expr(s.expr).span);
                err_(ast_.expr(s.expr).span, "require(expr) must evaluate to bool");
                return;

            case ProtoRequireEvalResult::kTooComplex:
                diag_(diag::Code::kRequireExprTooComplex, ast_.expr(s.expr).span);
                err_(ast_.expr(s.expr).span,
                     "require(expr) supports only true/false/not/and/or/==/!= in v0");
                return;
        }
    }

    void TypeChecker::check_stmt_throw_(const ast::Stmt& s) {
        if (!fn_ctx_.in_fn || !fn_ctx_.is_throwing) {
            diag_(diag::Code::kThrowOnlyInThrowingFn, s.span);
            err_(s.span, "throw is only allowed inside throwing ('?') functions");
        }

        if (s.expr == ast::k_invalid_expr) {
            diag_(diag::Code::kThrowPayloadTypeNotAllowed, s.span, "<missing>");
            err_(s.span, "throw payload expression is required");
            fn_ctx_.has_exception_construct = true;
            return;
        }

        ty::TypeId payload_t = check_expr_(s.expr, Slot::kValue);
        (void)ensure_generic_field_instance_from_type_(payload_t, s.span);
        (void)ensure_generic_enum_instance_from_type_(payload_t, s.span);

        // catch(e)에서 바인딩된 untyped payload는 opaque rethrow 토큰으로 허용한다.
        // v0에서는 `throw e` 패턴만 보장하고, 구조 분해/필드 접근은 지원하지 않는다.
        bool is_untyped_catch_rethrow = false;
        const auto& payload_expr = ast_.expr(s.expr);
        if (payload_expr.kind == ast::ExprKind::kIdent) {
            if (auto sid = lookup_symbol_(payload_expr.text)) {
                is_untyped_catch_rethrow = (untyped_catch_binder_symbols_.find(*sid) !=
                                            untyped_catch_binder_symbols_.end());
            }
        }
        if (is_untyped_catch_rethrow) {
            fn_ctx_.has_exception_construct = true;
            return;
        }

        const bool is_struct_payload = (field_abi_meta_by_type_.find(payload_t) != field_abi_meta_by_type_.end());
        const bool is_enum_payload = (enum_abi_meta_by_type_.find(payload_t) != enum_abi_meta_by_type_.end());
        if (!is_struct_payload && !is_enum_payload) {
            diag_(diag::Code::kThrowPayloadTypeNotAllowed, s.span, types_.to_string(payload_t));
            err_(s.span, "throw payload must be enum/struct value in v0");
            fn_ctx_.has_exception_construct = true;
            return;
        }

        auto resolve_recoverable_proto = [&]() -> std::optional<ast::StmtId> {
            std::string key = "Recoverable";
            if (auto rewritten = rewrite_imported_path_(key)) {
                key = *rewritten;
            }
            if (auto it = proto_decl_by_name_.find(key); it != proto_decl_by_name_.end()) {
                return it->second;
            }
            if (auto sid = lookup_symbol_(key)) {
                const auto& ss = sym_.symbol(*sid);
                if (auto pit = proto_decl_by_name_.find(ss.name); pit != proto_decl_by_name_.end()) {
                    return pit->second;
                }
            }
            return std::nullopt;
        };

        auto satisfies_recoverable = [&](ty::TypeId t, ast::StmtId recoverable_sid) -> bool {
            ast::StmtId owner_sid = ast::k_invalid_stmt;
            if (auto it = enum_decl_by_type_.find(t); it != enum_decl_by_type_.end()) {
                owner_sid = it->second;
            } else if (auto it = field_abi_meta_by_type_.find(t); it != field_abi_meta_by_type_.end()) {
                owner_sid = it->second.sid;
            }
            if (owner_sid == ast::k_invalid_stmt || (size_t)owner_sid >= ast_.stmts().size()) {
                return false;
            }
            const auto& owner = ast_.stmt(owner_sid);
            const auto& refs = ast_.path_refs();
            const uint64_t begin = owner.decl_path_ref_begin;
            const uint64_t end = begin + owner.decl_path_ref_count;
            if (begin > refs.size() || end > refs.size()) return false;
            for (uint32_t i = owner.decl_path_ref_begin; i < owner.decl_path_ref_begin + owner.decl_path_ref_count; ++i) {
                if (auto psid = resolve_proto_decl_from_path_ref_(refs[i], s.span)) {
                    if (*psid == recoverable_sid) {
                        return evaluate_proto_require_at_apply_(
                            recoverable_sid, t, s.span,
                            /*emit_unsatisfied_diag=*/false,
                            /*emit_shape_diag=*/false);
                    }
                }
            }
            return false;
        };

        const auto recoverable_sid = resolve_recoverable_proto();
        if (!recoverable_sid.has_value() || !satisfies_recoverable(payload_t, *recoverable_sid)) {
            diag_(diag::Code::kThrowPayloadMustBeRecoverable, s.span, types_.to_string(payload_t));
            err_(s.span, "throw payload must satisfy Recoverable proto");
        }

        fn_ctx_.has_exception_construct = true;
    }

    void TypeChecker::check_stmt_try_catch_(const ast::Stmt& s) {
        if (!fn_ctx_.in_fn || !fn_ctx_.is_throwing) {
            diag_(diag::Code::kTryCatchOnlyInThrowingFn, s.span);
            err_(s.span, "try-catch is only allowed inside throwing ('?') functions");
        }

        if (s.catch_clause_count == 0) {
            diag_(diag::Code::kTryCatchNeedsAtLeastOneCatch, s.span);
            err_(s.span, "try-catch requires at least one catch clause");
        }

        if (s.a != ast::k_invalid_stmt) {
            check_stmt_(s.a);
        }

        const auto& clauses = ast_.try_catch_clauses();
        const uint64_t begin = s.catch_clause_begin;
        const uint64_t end = begin + s.catch_clause_count;
        if (begin > clauses.size() || end > clauses.size()) {
            diag_(diag::Code::kTypeErrorGeneric, s.span, "invalid try-catch clause range");
            err_(s.span, "invalid try-catch clause range");
            fn_ctx_.has_exception_construct = true;
            return;
        }

        auto resolve_recoverable_proto = [&]() -> std::optional<ast::StmtId> {
            std::string key = "Recoverable";
            if (auto rewritten = rewrite_imported_path_(key)) {
                key = *rewritten;
            }
            if (auto it = proto_decl_by_name_.find(key); it != proto_decl_by_name_.end()) {
                return it->second;
            }
            if (auto sid = lookup_symbol_(key)) {
                const auto& ss = sym_.symbol(*sid);
                if (auto pit = proto_decl_by_name_.find(ss.name); pit != proto_decl_by_name_.end()) {
                    return pit->second;
                }
            }
            return std::nullopt;
        };

        auto satisfies_recoverable = [&](ty::TypeId t, ast::StmtId recoverable_sid, Span sp) -> bool {
            ast::StmtId owner_sid = ast::k_invalid_stmt;
            if (auto it = enum_decl_by_type_.find(t); it != enum_decl_by_type_.end()) {
                owner_sid = it->second;
            } else if (auto it = field_abi_meta_by_type_.find(t); it != field_abi_meta_by_type_.end()) {
                owner_sid = it->second.sid;
            }
            if (owner_sid == ast::k_invalid_stmt || (size_t)owner_sid >= ast_.stmts().size()) {
                return false;
            }
            const auto& owner = ast_.stmt(owner_sid);
            const auto& refs = ast_.path_refs();
            const uint64_t rb = owner.decl_path_ref_begin;
            const uint64_t re = rb + owner.decl_path_ref_count;
            if (rb > refs.size() || re > refs.size()) return false;
            for (uint32_t i = owner.decl_path_ref_begin; i < owner.decl_path_ref_begin + owner.decl_path_ref_count; ++i) {
                if (auto psid = resolve_proto_decl_from_path_ref_(refs[i], sp)) {
                    if (*psid == recoverable_sid) {
                        return evaluate_proto_require_at_apply_(
                            recoverable_sid, t, sp,
                            /*emit_unsatisfied_diag=*/false,
                            /*emit_shape_diag=*/false);
                    }
                }
            }
            return false;
        };

        const auto recoverable_sid = resolve_recoverable_proto();

        for (uint32_t i = 0; i < s.catch_clause_count; ++i) {
            const auto& cc = clauses[s.catch_clause_begin + i];
            sym_.push_scope();

            ty::TypeId bind_t = types_.error();
            if (cc.has_typed_bind) {
                bind_t = cc.bind_type;
                (void)ensure_generic_field_instance_from_type_(bind_t, cc.span);
                (void)ensure_generic_enum_instance_from_type_(bind_t, cc.span);
                const bool is_struct_payload = (field_abi_meta_by_type_.find(bind_t) != field_abi_meta_by_type_.end());
                const bool is_enum_payload = (enum_abi_meta_by_type_.find(bind_t) != enum_abi_meta_by_type_.end());
                if (!is_struct_payload && !is_enum_payload) {
                    diag_(diag::Code::kThrowPayloadTypeNotAllowed, cc.span, types_.to_string(bind_t));
                    err_(cc.span, "typed catch binder must be enum/struct value in v0");
                    bind_t = types_.error();
                } else if (!recoverable_sid.has_value() ||
                           !satisfies_recoverable(bind_t, *recoverable_sid, cc.span)) {
                    diag_(diag::Code::kThrowPayloadMustBeRecoverable, cc.span, types_.to_string(bind_t));
                    err_(cc.span, "typed catch binder must satisfy Recoverable proto");
                }
            }

            if (cc.bind_name.empty()) {
                diag_(diag::Code::kCatchBinderNameExpected, cc.span);
                err_(cc.span, "catch binder name is required");
            } else {
                auto ins = sym_.insert(sema::SymbolKind::kVar, cc.bind_name, bind_t, cc.span);
                if (!ins.ok && ins.is_duplicate) {
                    diag_(diag::Code::kDuplicateDecl, cc.span, cc.bind_name);
                    err_(cc.span, "duplicate catch binder name");
                } else if (ins.ok) {
                    sym_is_mut_[ins.symbol_id] = false;
                    if (!cc.has_typed_bind) {
                        untyped_catch_binder_symbols_.insert(ins.symbol_id);
                    }
                }
            }

            if (cc.body != ast::k_invalid_stmt) {
                check_stmt_(cc.body);
            }
            sym_.pop_scope();
        }

        fn_ctx_.has_exception_construct = true;
    }

    void TypeChecker::check_stmt_switch_(const ast::Stmt& s) {
        ty::TypeId scrut_t = ty::kInvalidType;
        if (s.expr != ast::k_invalid_expr) {
            scrut_t = check_expr_(s.expr);
        }

        // Switch pattern matching is value-based. If scrutinee is a borrow, match on its element.
        ty::TypeId scrut_match_t = scrut_t;
        if (scrut_match_t != ty::kInvalidType) {
            const auto& sm = types_.get(scrut_match_t);
            if (sm.kind == ty::Kind::kBorrow) {
                scrut_match_t = sm.elem;
            }
        }

        const bool scrut_is_optional = is_optional_(scrut_match_t);
        ty::TypeId scrut_enum_t = scrut_is_optional ? optional_elem_(scrut_match_t) : scrut_match_t;
        if (scrut_enum_t != ty::kInvalidType) {
            (void)ensure_generic_enum_instance_from_type_(scrut_enum_t, s.span);
        }

        for (uint32_t i = 0; i < s.case_count; ++i) {
            const uint32_t cidx = s.case_begin + i;
            if ((size_t)cidx >= ast_.switch_cases().size()) break;
            auto& c = ast_.switch_cases_mut()[cidx];

            if (c.pat_kind == ast::CasePatKind::kNull && !scrut_is_optional) {
                diag_(diag::Code::kTypeErrorGeneric, c.span, "case null is only valid for optional scrutinee");
                err_(c.span, "switch case null requires optional scrutinee type");
            }

            if (c.pat_kind == ast::CasePatKind::kEnumVariant) {
                ty::TypeId case_enum_t = c.enum_type;
                if (case_enum_t != ty::kInvalidType) {
                    (void)ensure_generic_enum_instance_from_type_(case_enum_t, c.span);
                }
                if (case_enum_t == ty::kInvalidType) {
                    std::string enum_path = "<enum>";
                    if (c.enum_type_node != ast::k_invalid_type_node &&
                        (size_t)c.enum_type_node < ast_.type_nodes().size()) {
                        enum_path = types_.to_string(ast_.type_node(c.enum_type_node).resolved_type);
                    }
                    diag_(diag::Code::kTypeErrorGeneric, c.span,
                          std::string("invalid enum pattern target: ") + enum_path);
                    err_(c.span, "invalid enum switch pattern target");
                } else if (scrut_enum_t != ty::kInvalidType && scrut_enum_t != case_enum_t) {
                    diag_(diag::Code::kTypeMismatch, c.span,
                          types_.to_string(scrut_enum_t), types_.to_string(case_enum_t));
                    err_(c.span, "enum case pattern type does not match switch scrutinee");
                }

                auto meta_it = enum_abi_meta_by_type_.find(case_enum_t);
                if (meta_it == enum_abi_meta_by_type_.end()) {
                    if (auto sid = ensure_generic_enum_instance_from_type_(case_enum_t, c.span)) {
                        check_stmt_enum_decl_(*sid);
                    }
                    meta_it = enum_abi_meta_by_type_.find(case_enum_t);
                }

                bool bind_scope_opened = false;
                if (meta_it == enum_abi_meta_by_type_.end()) {
                    diag_(diag::Code::kTypeErrorGeneric, c.span, "enum pattern requires a known enum type");
                    err_(c.span, "enum pattern requires known enum metadata");
                } else {
                    const auto& meta = meta_it->second;
                    auto vit = meta.variant_index_by_name.find(std::string(c.enum_variant_name));
                    if (vit == meta.variant_index_by_name.end()) {
                        diag_(diag::Code::kTypeErrorGeneric, c.span,
                              std::string("unknown enum variant '") + std::string(c.enum_variant_name) + "'");
                        err_(c.span, "unknown enum variant in switch case");
                    } else {
                        const auto& vm = meta.variants[vit->second];
                        c.enum_variant_index = vm.index;
                        c.enum_tag_value = vm.tag;

                        std::unordered_set<std::string> seen_fields;
                        std::unordered_set<std::string> seen_binds;

                        const uint64_t bb = c.enum_bind_begin;
                        const uint64_t be = bb + c.enum_bind_count;
                        auto& binds = ast_.switch_enum_binds_mut();
                        if (bb > binds.size() || be > binds.size()) {
                            diag_(diag::Code::kTypeErrorGeneric, c.span, "invalid enum pattern bind slice");
                            err_(c.span, "invalid enum pattern bind slice");
                        } else {
                            for (uint32_t bi = c.enum_bind_begin; bi < c.enum_bind_begin + c.enum_bind_count; ++bi) {
                                auto& b = binds[bi];
                                if (!seen_fields.insert(std::string(b.field_name)).second) {
                                    diag_(diag::Code::kDuplicateDecl, b.span, b.field_name);
                                    err_(b.span, "duplicate enum bind field label");
                                    continue;
                                }
                                if (!seen_binds.insert(std::string(b.bind_name)).second) {
                                    diag_(diag::Code::kDuplicateDecl, b.span, b.bind_name);
                                    err_(b.span, "duplicate enum bind variable name");
                                    continue;
                                }
                                auto fit = vm.field_index_by_name.find(std::string(b.field_name));
                                if (fit == vm.field_index_by_name.end()) {
                                    diag_(diag::Code::kTypeErrorGeneric, b.span,
                                          std::string("unknown enum payload field '") + std::string(b.field_name) + "'");
                                    err_(b.span, "unknown enum payload field in switch bind");
                                    continue;
                                }
                                const auto& fm = vm.fields[fit->second];
                                b.bind_type = fm.type;
                                b.storage_name = ast_.add_owned_string(fm.storage_name);
                            }

                            sym_.push_scope();
                            bind_scope_opened = true;
                            for (uint32_t bi = c.enum_bind_begin; bi < c.enum_bind_begin + c.enum_bind_count; ++bi) {
                                const auto& b = binds[bi];
                                if (b.bind_type == ty::kInvalidType) continue;
                                auto ins = sym_.insert(sema::SymbolKind::kVar, b.bind_name, b.bind_type, b.span);
                                if (!ins.ok && ins.is_duplicate) {
                                    diag_(diag::Code::kDuplicateDecl, b.span, b.bind_name);
                                    err_(b.span, "duplicate switch bind variable");
                                } else if (ins.ok) {
                                    sym_is_mut_[ins.symbol_id] = false;
                                }
                            }
                        }
                    }
                }

                if (c.body != ast::k_invalid_stmt) check_stmt_(c.body);
                if (bind_scope_opened) {
                    sym_.pop_scope();
                }
                continue;
            }

            // case body는 항상 block
            if (c.body != ast::k_invalid_stmt) check_stmt_(c.body);
        }
    }
