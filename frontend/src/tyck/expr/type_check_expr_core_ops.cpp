    ty::TypeId TypeChecker::check_expr_unary_(const ast::Expr& e) {
        if (current_expr_id_ != ast::k_invalid_expr &&
            current_expr_id_ < expr_overload_target_cache_.size()) {
            expr_overload_target_cache_[current_expr_id_] = ast::k_invalid_stmt;
        }

        auto is_copy_clone_trivial_fast_path = [&](ty::TypeId t) -> bool {
            if (t == ty::kInvalidType || is_error_(t)) return false;
            const auto& tv = types_.get(t);
            if (tv.kind == ty::Kind::kBorrow) return true;
            if (tv.kind != ty::Kind::kBuiltin) return false;
            switch (tv.builtin) {
                case ty::Builtin::kBool:
                case ty::Builtin::kChar:
                case ty::Builtin::kI8:
                case ty::Builtin::kI16:
                case ty::Builtin::kI32:
                case ty::Builtin::kI64:
                case ty::Builtin::kI128:
                case ty::Builtin::kU8:
                case ty::Builtin::kU16:
                case ty::Builtin::kU32:
                case ty::Builtin::kU64:
                case ty::Builtin::kU128:
                case ty::Builtin::kISize:
                case ty::Builtin::kUSize:
                case ty::Builtin::kF32:
                case ty::Builtin::kF64:
                case ty::Builtin::kF128:
                    return true;
                default:
                    return false;
            }
        };

        // NOTE:
        // - '&' / '&mut' / '^&' 의 의미 규칙(place, escape, conflict 등)은
        //   capability 단계에서 독립적으로 검사한다.
        // - tyck는 여기서 "결과 타입 계산"만 수행한다.
        if (e.op == K::kKwCopy || e.op == K::kKwClone) {
            if (!is_place_expr_(e.a)) {
                diag_(diag::Code::kCopyCloneOperandMustBePlace, e.span);
                err_(e.span, "copy/clone operand must be a place expression");
                return types_.error();
            }

            ty::TypeId at = check_expr_(e.a);
            const ActiveActsSelection* forced_selection = nullptr;
            if (auto sid = root_place_symbol_(e.a)) {
                forced_selection = lookup_symbol_acts_selection_(*sid);
            }

            const ast::StmtId op_sid = resolve_prefix_operator_overload_(e.op, at, forced_selection);
            if (op_sid != ast::k_invalid_stmt) {
                if (current_expr_id_ != ast::k_invalid_expr &&
                    current_expr_id_ < expr_overload_target_cache_.size()) {
                    expr_overload_target_cache_[current_expr_id_] = op_sid;
                }
                return ast_.stmt(op_sid).fn_ret;
            }

            if (is_copy_clone_trivial_fast_path(at)) {
                return at;
            }

            if (e.op == K::kKwCopy) {
                diag_(diag::Code::kCopyNotSupportedForType, e.span, types_.to_string(at));
                err_(e.span, "copy is not supported for this type without operator(copy)");
            } else {
                diag_(diag::Code::kCloneNotSupportedForType, e.span, types_.to_string(at));
                err_(e.span, "clone is not supported for this type without operator(clone)");
            }
            return types_.error();
        }

        if (e.op == K::kKwTry) {
            if (e.a == ast::k_invalid_expr || (size_t)e.a >= ast_.exprs().size()) {
                diag_(diag::Code::kTryExprOperandMustBeThrowingCall, e.span);
                err_(e.span, "try operand must be a throwing function call expression");
                fn_ctx_.has_exception_construct = true;
                return types_.error();
            }

            const auto& operand = ast_.expr(e.a);
            if (operand.kind != ast::ExprKind::kCall) {
                (void)check_expr_(e.a);
                diag_(diag::Code::kTryExprOperandMustBeThrowingCall, e.span);
                err_(e.span, "try operand must be a throwing function call expression");
                fn_ctx_.has_exception_construct = true;
                return types_.error();
            }

            const bool saved_try_ctx = in_try_expr_context_;
            in_try_expr_context_ = true;
            ty::TypeId at = check_expr_(e.a);
            in_try_expr_context_ = saved_try_ctx;

            bool is_throwing_call = false;
            if ((size_t)e.a < expr_overload_target_cache_.size()) {
                const ast::StmtId target_sid = expr_overload_target_cache_[e.a];
                if (target_sid != ast::k_invalid_stmt && (size_t)target_sid < ast_.stmts().size()) {
                    const auto& target = ast_.stmt(target_sid);
                    if (target.kind == ast::StmtKind::kFnDecl && target.is_throwing) {
                        is_throwing_call = true;
                    }
                }
            }
            if (!is_throwing_call) {
                diag_(diag::Code::kTryExprOperandMustBeThrowingCall, e.span);
                err_(e.span, "try operand must be a throwing ('?') function call");
                fn_ctx_.has_exception_construct = true;
                return types_.error();
            }

            fn_ctx_.has_exception_construct = true;
            if (is_error_(at)) return types_.error();
            if (is_optional_(at)) return at;
            return types_.make_optional(at);
        }

        if (e.op == K::kAmp) {
            // slice borrow: &x[a..b], &mut x[a..:b]
            if (e.a != ast::k_invalid_expr) {
                const auto& opnd = ast_.expr(e.a);
                if (opnd.kind == ast::ExprKind::kIndex && is_range_expr_(opnd.b)) {
                    // index expression 쪽에서 base/경계 타입 검사를 모두 수행한다.
                    ty::TypeId view_t = check_expr_(e.a);
                    const auto& vt = types_.get(view_t);
                    if (vt.kind != ty::Kind::kArray) {
                        diag_(diag::Code::kTypeIndexNonArray, e.span, types_.to_string(view_t));
                        err_(e.span, "slicing is only supported on array types (T[] / T[N]) in v0");
                        return types_.error();
                    }
                    return types_.make_borrow(view_t, /*is_mut=*/e.unary_is_mut);
                }
            }

            ty::TypeId at = check_expr_(e.a);
            if (!is_error_(at)) {
                const auto& atv = types_.get(at);
                if (atv.kind == ty::Kind::kBorrow || atv.kind == ty::Kind::kEscape) {
                    diag_(diag::Code::kBorrowOperandMustBeOwnedPlace, e.span);
                    err_(e.span, "borrow '&' can only be created from owned place values");
                    return types_.error();
                }
            }
            return types_.make_borrow(at, /*is_mut=*/e.unary_is_mut);
        }

        if (e.op == K::kCaretAmp) {
            if (in_actor_method_ && e.a != ast::k_invalid_expr) {
                const auto& opnd = ast_.expr(e.a);
                if (opnd.kind == ast::ExprKind::kIdent && opnd.text == "draft") {
                    diag_(diag::Code::kActorEscapeDraftMoveNotAllowed, e.span);
                    err_(e.span, "actor draft cannot be moved with '^&'");
                    return types_.error();
                }
            }
            ty::TypeId at = check_expr_(e.a);
            return types_.make_escape(at);
        }

        // e.op, e.a
        ty::TypeId at = check_expr_(e.a);
        at = read_decay_borrow_(types_, at);

        // 기타 unary: v0에서는 최소만
        if (e.op == K::kBang || e.op == K::kKwNot) {
            if (at != types_.builtin(ty::Builtin::kBool) && !is_error_(at)) {
                diag_(diag::Code::kTypeUnaryBangMustBeBool, e.span, types_.to_string(at));
                err_(e.span, "operator 'not' requires bool");
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
        if (current_expr_id_ != ast::k_invalid_expr &&
            current_expr_id_ < expr_overload_target_cache_.size()) {
            expr_overload_target_cache_[current_expr_id_] = ast::k_invalid_stmt;
        }

        if (!is_place_expr_(e.a)) {
            diag_(diag::Code::kPostfixOperandMustBePlace, e.span);
            err_(e.span, "postfix operator requires a place expression");
            return types_.error();
        }

        ty::TypeId at = check_expr_(e.a);
        ty::TypeId elem = ty::kInvalidType;
        bool is_mut_borrow = false;
        const bool write_through_borrow = borrow_info_(types_, at, elem, is_mut_borrow) && is_mut_borrow;

        // mut check (x++ is a write)
        // - place가 가리키는 심볼이 mut가 아니면 무조건 에러
        if (!write_through_borrow) {
            if (auto sid = root_place_symbol_(e.a)) {
                if (!is_mutable_symbol_(*sid)) {
                    diag_(diag::Code::kWriteToImmutable, e.span);
                    err_(e.span, "cannot apply postfix ++ to an immutable variable (declare it with `mut`)");
                }
            }
        }

        const ty::TypeId receiver_ty = write_through_borrow ? elem : at;
        const ActiveActsSelection* forced_selection = nullptr;
        if (auto sid = root_place_symbol_(e.a)) {
            forced_selection = lookup_symbol_acts_selection_(*sid);
        }
        const ast::StmtId op_sid = resolve_postfix_operator_overload_(e.op, receiver_ty, forced_selection);
        if (op_sid != ast::k_invalid_stmt) {
            if (current_expr_id_ != ast::k_invalid_expr &&
                current_expr_id_ < expr_overload_target_cache_.size()) {
                expr_overload_target_cache_[current_expr_id_] = op_sid;
            }
            return ast_.stmt(op_sid).fn_ret;
        }

        return receiver_ty;
    }

    // --------------------
    // binary / assign / ternary
    // --------------------
    ty::TypeId TypeChecker::check_expr_binary_(const ast::Expr& e) {
        if (current_expr_id_ != ast::k_invalid_expr &&
            current_expr_id_ < expr_overload_target_cache_.size()) {
            expr_overload_target_cache_[current_expr_id_] = ast::k_invalid_stmt;
        }

        // value member access (v0): obj.field
        if (e.op == K::kDot) {
            ty::TypeId base_t = check_expr_(e.a);
            base_t = read_decay_borrow_(types_, base_t);
            (void)ensure_generic_enum_instance_from_type_(base_t, e.span);

            if (e.b == ast::k_invalid_expr) {
                diag_(diag::Code::kTypeErrorGeneric, e.span, "missing member on '.' access");
                err_(e.span, "missing member on '.' access");
                return types_.error();
            }

            const ast::Expr& rhs = ast_.expr(e.b);
            if (rhs.kind != ast::ExprKind::kIdent) {
                diag_(diag::Code::kTypeErrorGeneric, rhs.span, "member access requires identifier rhs");
                err_(rhs.span, "member access requires identifier rhs");
                return types_.error();
            }

            if (enum_abi_meta_by_type_.find(base_t) != enum_abi_meta_by_type_.end()) {
                diag_(diag::Code::kEnumDotFieldAccessForbidden, rhs.span);
                err_(rhs.span, "enum payload field access is only allowed via switch pattern binding");
                return types_.error();
            }

            auto meta_it = field_abi_meta_by_type_.find(base_t);
            if (meta_it == field_abi_meta_by_type_.end()) {
                (void)ensure_generic_field_instance_from_type_(base_t, e.span);
                meta_it = field_abi_meta_by_type_.find(base_t);
            }
            if (meta_it == field_abi_meta_by_type_.end()) {
                const auto& bt = types_.get(base_t);
                if (bt.kind == ty::Kind::kNamedUser) {
                    if (auto sid = lookup_symbol_(types_.to_string(base_t))) {
                        const auto& ss = sym_.symbol(*sid);
                        if (ss.kind == sema::SymbolKind::kField && ss.declared_type != ty::kInvalidType) {
                            (void)ensure_generic_field_instance_from_type_(ss.declared_type, e.span);
                            meta_it = field_abi_meta_by_type_.find(ss.declared_type);
                        }
                    }
                }
            }

            if (meta_it == field_abi_meta_by_type_.end()) {
                diag_(diag::Code::kTypeErrorGeneric, e.span,
                    std::string("member access is only available on field/class/actor values in v0, got ") + types_.to_string(base_t));
                err_(e.span, "member access on non field/class/actor value");
                return types_.error();
            }

            const ast::StmtId fsid = meta_it->second.sid;
            if (fsid == ast::k_invalid_stmt || (size_t)fsid >= ast_.stmts().size()) {
                diag_(diag::Code::kTypeErrorGeneric, e.span, "invalid field metadata while resolving member");
                err_(e.span, "invalid field metadata");
                return types_.error();
            }

            const auto& fs = ast_.stmt(fsid);
            const uint64_t begin = fs.field_member_begin;
            const uint64_t end = begin + fs.field_member_count;
            if (begin > ast_.field_members().size() || end > ast_.field_members().size()) {
                diag_(diag::Code::kTypeErrorGeneric, e.span, "invalid field member range");
                err_(e.span, "invalid field member range");
                return types_.error();
            }

            for (uint32_t i = fs.field_member_begin; i < fs.field_member_begin + fs.field_member_count; ++i) {
                const auto& m = ast_.field_members()[i];
                if (m.name == rhs.text) {
                    return m.type;
                }
            }

            diag_(diag::Code::kTypeErrorGeneric, rhs.span,
                std::string("unknown field member '") + std::string(rhs.text) + "'");
            err_(rhs.span, "unknown field member");
            return types_.error();
        }

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
                err_(e.span, "operator '?" "?' requires optional lhs");
                return types_.error();
            }

            ty::TypeId elem = optional_elem_(lt);
            if (elem == ty::kInvalidType) {
                // 방어: Optional인데 elem이 invalid인 경우
                err_(e.span, "optional elem type is invalid");
                return types_.error();
            }

            const CoercionPlan rhs_plan = classify_assign_with_coercion_(
                AssignSite::Assign, elem, e.b, e.span);
            rt = rhs_plan.src_after;
            if (!rhs_plan.ok) {
                diag_(diag::Code::kTypeNullCoalesceRhsMismatch, e.span,
                    types_.to_string(elem), type_for_user_diag_(rt, e.b));
                err_(e.span, "operator '?" "?' rhs mismatch");
                return types_.error();
            }

            // 결과는 non-optional elem
            return elem;
        }

        ty::TypeId lt = check_expr_(e.a);
        ty::TypeId rt = check_expr_(e.b);
        lt = read_decay_borrow_(types_, lt);
        rt = read_decay_borrow_(types_, rt);
        const ActiveActsSelection* forced_selection = nullptr;
        if (auto sid = root_place_symbol_(e.a)) {
            forced_selection = lookup_symbol_acts_selection_(*sid);
        }

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
        // Logical: and / or
        // ------------------------------------------------------------
        if (e.op == K::kKwAnd || e.op == K::kKwOr) {
            const ty::TypeId bool_ty = types_.builtin(ty::Builtin::kBool);
            if (lt != bool_ty && !is_error_(lt)) {
                diag_(diag::Code::kTypeErrorGeneric, e.span,
                      "logical operator requires bool lhs, got '" + types_.to_string(lt) + "'");
                err_(e.span, "logical operator lhs must be bool");
            }
            if (rt != bool_ty && !is_error_(rt)) {
                diag_(diag::Code::kTypeErrorGeneric, e.span,
                      "logical operator requires bool rhs, got '" + types_.to_string(rt) + "'");
                err_(e.span, "logical operator rhs must be bool");
            }
            return bool_ty;
        }

        // ------------------------------------------------------------
        // Equality: == / !=
        // ------------------------------------------------------------
        if (e.op == K::kEqEq || e.op == K::kBangEq) {
            // acts overload 우선 규칙: 오버로드가 존재하면 builtin보다 먼저 채택한다.
            if (!acts_default_operator_map_.empty()) {
                const ast::StmtId op_sid = resolve_binary_operator_overload_(e.op, lt, rt, forced_selection);
                if (op_sid != ast::k_invalid_stmt) {
                    if (current_expr_id_ != ast::k_invalid_expr &&
                        current_expr_id_ < expr_overload_target_cache_.size()) {
                        expr_overload_target_cache_[current_expr_id_] = op_sid;
                    }
                    return ast_.stmt(op_sid).fn_ret;
                }
            }

            const bool both_builtin = is_builtin(lt) && is_builtin(rt);
            if (!both_builtin && !is_null_(lt) && !is_null_(rt)) {
                diag_(diag::Code::kTypeErrorGeneric, e.span, "no matching operator overload for equality");
                err_(e.span, "no matching operator overload for equality");
                return types_.error();
            }

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
            // acts overload 우선 규칙: 오버로드가 존재하면 builtin보다 먼저 채택한다.
            if (!acts_default_operator_map_.empty()) {
                const ast::StmtId op_sid = resolve_binary_operator_overload_(e.op, lt, rt, forced_selection);
                if (op_sid != ast::k_invalid_stmt) {
                    if (current_expr_id_ != ast::k_invalid_expr &&
                        current_expr_id_ < expr_overload_target_cache_.size()) {
                        expr_overload_target_cache_[current_expr_id_] = op_sid;
                    }
                    return ast_.stmt(op_sid).fn_ret;
                }
            }

            const bool both_builtin = is_builtin(lt) && is_builtin(rt);
            if (!both_builtin) {
                diag_(diag::Code::kTypeErrorGeneric, e.span, "no matching operator overload for arithmetic");
                err_(e.span, "no matching operator overload for arithmetic");
                return types_.error();
            }

            // float + unsuffixed integer literal is forbidden (no implicit int->float)
            if ((is_float(lt) && is_infer_int(rt)) || (is_float(rt) && is_infer_int(lt))) {
                diag_(diag::Code::kIntToFloatNotAllowed, e.span, "float-arithmetic");
                err_(e.span, "cannot use an unsuffixed integer literal in float arithmetic (no implicit int->float)");
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
            // acts overload 우선 규칙: 오버로드가 존재하면 builtin보다 먼저 채택한다.
            if (!acts_default_operator_map_.empty()) {
                const ast::StmtId op_sid = resolve_binary_operator_overload_(e.op, lt, rt, forced_selection);
                if (op_sid != ast::k_invalid_stmt) {
                    if (current_expr_id_ != ast::k_invalid_expr &&
                        current_expr_id_ < expr_overload_target_cache_.size()) {
                        expr_overload_target_cache_[current_expr_id_] = op_sid;
                    }
                    return ast_.stmt(op_sid).fn_ret;
                }
            }

            const bool both_builtin = is_builtin(lt) && is_builtin(rt);
            if (!both_builtin) {
                diag_(diag::Code::kTypeErrorGeneric, e.span, "no matching operator overload for comparison");
                err_(e.span, "no matching operator overload for comparison");
                return types_.error();
            }

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
                // infer-int vs infer-int (or vs non-int) => needs explicit context
                diag_(diag::Code::kIntLiteralNeedsTypeContext, e.span);
                err_(e.span, "comparison with unsuffixed integer literals needs an explicit integer type context");
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
        // Remaining operators: try overload first, then fail explicitly.
        // (No silent fallback for parser-accepted-but-unsupported operators.)
        // ------------------------------------------------------------
        {
            const ast::StmtId op_sid = resolve_binary_operator_overload_(e.op, lt, rt, forced_selection);
            if (op_sid != ast::k_invalid_stmt) {
                if (current_expr_id_ != ast::k_invalid_expr &&
                    current_expr_id_ < expr_overload_target_cache_.size()) {
                    expr_overload_target_cache_[current_expr_id_] = op_sid;
                }
                return ast_.stmt(op_sid).fn_ret;
            }
        }

        diag_(diag::Code::kTypeErrorGeneric, e.span,
              "unsupported binary operator '" +
                  std::string(parus::syntax::token_kind_name(e.op)) +
                  "' for operand types '" + types_.to_string(lt) +
                  "' and '" + types_.to_string(rt) + "'");
        err_(e.span, "unsupported binary operator");
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

            ty::TypeId lt = check_expr_(e.a);
            ty::TypeId lhs_target = lt;
            {
                ty::TypeId elem = ty::kInvalidType;
                bool is_mut_borrow = false;
                if (borrow_info_(types_, lt, elem, is_mut_borrow) && is_mut_borrow) {
                    lhs_target = elem; // &mut T place에는 T를 대입한다.
                }
            }

            // mut check
            {
                ty::TypeId elem = ty::kInvalidType;
                bool is_mut_borrow = false;
                const bool write_through_borrow = borrow_info_(types_, lt, elem, is_mut_borrow) && is_mut_borrow;
                if (!write_through_borrow) {
                    if (auto sid = root_place_symbol_(e.a)) {
                        if (!is_mutable_symbol_(*sid)) {
                            diag_(diag::Code::kWriteToImmutable, e.span, "assignment");
                            err_(e.span, "cannot assign to an immutable variable (declare it with `mut`)");
                        }
                    }
                }
            }

            ty::TypeId rt = check_expr_(e.b);

            if (is_error_(lt) || is_error_(rt)) return types_.error();

            if (!is_optional_(lhs_target)) {
                diag_(diag::Code::kTypeNullCoalesceAssignLhsMustBeOptional, e.span, types_.to_string(lhs_target));
                err_(e.span, "operator '?" "?=' requires optional lhs");
                return types_.error();
            }

            ty::TypeId elem = optional_elem_(lhs_target);
            if (elem == ty::kInvalidType) {
                err_(e.span, "optional elem type is invalid");
                return types_.error();
            }

            const CoercionPlan rhs_plan = classify_assign_with_coercion_(
                AssignSite::NullCoalesceAssign, elem, e.b, e.span);
            rt = rhs_plan.src_after;
            if (!rhs_plan.ok) {
                diag_(diag::Code::kTypeNullCoalesceAssignRhsMismatch, e.span,
                    types_.to_string(elem), type_for_user_diag_(rt, e.b));
                err_(e.span, "operator '?" "?=' rhs mismatch");
                return types_.error();
            }

            return lhs_target;
        }

        // ------------------------------------------------------------
        // 기존 '=' / 기타 대입류 (현 로직 유지)
        // ------------------------------------------------------------
        // e.a = lhs, e.b = rhs
        if (!is_place_expr_(e.a)) {
            diag_(diag::Code::kAssignLhsMustBePlace, e.span);
            err_(e.span, "assignment lhs must be a place expression (ident/index)");
        }

        ty::TypeId lt = check_expr_(e.a);
        ty::TypeId lhs_target = lt;
        {
            ty::TypeId elem = ty::kInvalidType;
            bool is_mut_borrow = false;
            if (borrow_info_(types_, lt, elem, is_mut_borrow) && is_mut_borrow) {
                lhs_target = elem; // &mut T place에는 T를 대입한다.
            }
        }

        if (is_place_expr_(e.a)) {
            ty::TypeId elem = ty::kInvalidType;
            bool is_mut_borrow = false;
            const bool write_through_borrow = borrow_info_(types_, lt, elem, is_mut_borrow) && is_mut_borrow;
            if (!write_through_borrow) {
                if (auto sid = root_place_symbol_(e.a)) {
                    if (!is_mutable_symbol_(*sid)) {
                        diag_(diag::Code::kWriteToImmutable, e.span, "assignment");
                        err_(e.span, "cannot assign to an immutable variable (declare it with `mut`)");
                    }
                }
            }
        }

        ty::TypeId rt = check_expr_(e.b);

        const CoercionPlan assign_plan = classify_assign_with_coercion_(
            AssignSite::Assign, lhs_target, e.b, e.span);
        rt = assign_plan.src_after;
        if (!assign_plan.ok) {
            diag_(
                diag::Code::kTypeAssignMismatch, e.span,
                types_.to_string(lhs_target), type_for_user_diag_(rt, e.b)
            );
            err_(e.span, "assign mismatch");
        }
        return lhs_target;
    }

