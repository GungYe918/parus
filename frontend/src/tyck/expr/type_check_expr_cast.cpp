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
                case ty::Builtin::kCChar: case ty::Builtin::kCSChar: case ty::Builtin::kCUChar:
                case ty::Builtin::kCShort: case ty::Builtin::kCUShort:
                case ty::Builtin::kCInt: case ty::Builtin::kCUInt:
                case ty::Builtin::kCLong: case ty::Builtin::kCULong:
                case ty::Builtin::kCLongLong: case ty::Builtin::kCULongLong:
                case ty::Builtin::kCFloat: case ty::Builtin::kCDouble:
                case ty::Builtin::kCSize: case ty::Builtin::kCSSize: case ty::Builtin::kCPtrDiff:
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
                    b == ty::Builtin::kISize || b == ty::Builtin::kUSize ||
                    b == ty::Builtin::kCChar || b == ty::Builtin::kCSChar || b == ty::Builtin::kCUChar ||
                    b == ty::Builtin::kCShort || b == ty::Builtin::kCUShort ||
                    b == ty::Builtin::kCInt || b == ty::Builtin::kCUInt ||
                    b == ty::Builtin::kCLong || b == ty::Builtin::kCULong ||
                    b == ty::Builtin::kCLongLong || b == ty::Builtin::kCULongLong ||
                    b == ty::Builtin::kCSize || b == ty::Builtin::kCSSize ||
                    b == ty::Builtin::kCPtrDiff;
            };

            auto is_float_builtin = [&](ty::Builtin b) -> bool {
                return b == ty::Builtin::kF32 || b == ty::Builtin::kF64 || b == ty::Builtin::kF128 ||
                       b == ty::Builtin::kCFloat || b == ty::Builtin::kCDouble;
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

} // namespace parus::tyck
