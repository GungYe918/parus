    ty::TypeId TypeChecker::check_expr_unary_(const ast::Expr& e) {
        if (current_expr_id_ != ast::k_invalid_expr &&
            current_expr_id_ < expr_overload_target_cache_.size()) {
            expr_overload_target_cache_[current_expr_id_] = ast::k_invalid_stmt;
        }

        auto is_actor_handle_type = [&](ty::TypeId t) -> bool {
            while (t != ty::kInvalidType && !is_error_(t)) {
                const auto& tt = types_.get(t);
                if (tt.kind == ty::Kind::kBorrow) {
                    t = tt.elem;
                    continue;
                }
                return tt.kind == ty::Kind::kNamedUser &&
                       actor_decl_by_type_.find(t) != actor_decl_by_type_.end();
            }
            return false;
        };

        // NOTE:
        // - '&' / '&mut' / '~' 의 의미 규칙(place, escape, conflict 등)은
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

            if (is_trivial_copy_clone_type_(at)) {
                return at;
            }

            if (is_actor_handle_type(at)) {
                if (e.op == K::kKwCopy) {
                    diag_(diag::Code::kCopyNotSupportedForType, e.span, types_.to_string(at));
                    err_(e.span, "actor handles do not support 'copy'; use 'clone'");
                    return types_.error();
                }
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

        if (e.op == K::kTilde) {
            if (in_actor_method_ && e.a != ast::k_invalid_expr) {
                const auto& opnd = ast_.expr(e.a);
                if (opnd.kind == ast::ExprKind::kIdent && opnd.text == "draft") {
                    diag_(diag::Code::kActorEscapeDraftMoveNotAllowed, e.span);
                    err_(e.span, "actor draft cannot be moved with '~'");
                    return types_.error();
                }
            }
            ty::TypeId at = check_expr_(e.a);
            if (!is_error_(at)) {
                mark_expr_move_consumed_(e.a, at, e.span);
            }
            return types_.make_escape(at);
        }

        // e.op, e.a
        ty::TypeId at = check_expr_(e.a);
        at = read_decay_borrow_(types_, at);

        // 기타 unary: v0에서는 최소만
        if (e.op == K::kKwNot) {
            if (at != types_.builtin(ty::Builtin::kBool) && !is_error_(at)) {
                diag_(diag::Code::kTypeUnaryBangMustBeBool, e.span, types_.to_string(at));
                err_(e.span, "operator 'not' requires bool");
            }
            return types_.builtin(ty::Builtin::kBool);
        }

        if (e.op == K::kBang) {
            if (at == types_.builtin(ty::Builtin::kBool) && !is_error_(at)) {
                diag_(diag::Code::kTypeBoolNegationUseNot, e.span, types_.to_string(at));
                err_(e.span, "boolean negation must use 'not', not '!'");
                return types_.error();
            }

            auto is_bitwise_int = [&](ty::TypeId t) -> bool {
                if (t == ty::kInvalidType || is_error_(t)) return false;
                const auto& tv = types_.get(t);
                if (tv.kind != ty::Kind::kBuiltin) return false;
                switch (tv.builtin) {
                    case ty::Builtin::kI8:
                    case ty::Builtin::kI16:
                    case ty::Builtin::kI32:
                    case ty::Builtin::kI64:
                    case ty::Builtin::kU8:
                    case ty::Builtin::kU16:
                    case ty::Builtin::kU32:
                    case ty::Builtin::kU64:
                    case ty::Builtin::kISize:
                    case ty::Builtin::kUSize:
                        return true;
                    default:
                        return false;
                }
            };

            if (!is_bitwise_int(at) && !is_error_(at)) {
                diag_(diag::Code::kTypeUnaryBitNotMustBeInteger, e.span, types_.to_string(at));
                err_(e.span, "prefix '!' is bitwise not and requires a builtin integer type");
                return types_.error();
            }
            return at;
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

        if (e.op == K::kBang) {
            ty::TypeId at = check_expr_(e.a);
            at = read_decay_borrow_(types_, at);
            if (is_error_(at)) return types_.error();
            if (!is_optional_(at)) {
                diag_(diag::Code::kTypeErrorGeneric, e.span,
                    std::string("postfix '!' requires an optional operand (got ") + types_.to_string(at) + ")");
                err_(e.span, "postfix '!' requires an optional operand");
                return types_.error();
            }
            const ty::TypeId elem = optional_elem_(at);
            if (elem == ty::kInvalidType) {
                err_(e.span, "optional elem type is invalid");
                return types_.error();
            }
            return elem;
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

        auto resolve_member_owner_type = [&](ast::ExprId recv_eid, Span member_span) -> ty::TypeId {
            if (recv_eid == ast::k_invalid_expr || (size_t)recv_eid >= ast_.exprs().size()) {
                return ty::kInvalidType;
            }
            const ast::Expr& recv = ast_.expr(recv_eid);
            if (recv.kind == ast::ExprKind::kIdent) {
                std::string recv_lookup = std::string(recv.text);
                if (auto rewritten = rewrite_imported_path_(recv_lookup)) {
                    recv_lookup = *rewritten;
                }
                if (auto recv_sid = lookup_symbol_(recv_lookup)) {
                    const auto& recv_sym = sym_.symbol(*recv_sid);
                    if (recv_sym.kind == sema::SymbolKind::kType) {
                        diag_(diag::Code::kDotReceiverMustBeValue, recv.span, recv.text);
                        err_(recv.span, "member access receiver must be a value, not a type name");
                        return types_.error();
                    }
                }
            }

            ty::TypeId owner_t = check_expr_(recv_eid);
            owner_t = read_decay_borrow_(types_, owner_t);
            (void)ensure_generic_field_instance_from_type_(owner_t, member_span);
            return owner_t;
        };

        auto resolve_owner_decl_sid_for_proto = [&](ty::TypeId owner_t) -> ast::StmtId {
            if (owner_t == ty::kInvalidType) return ast::k_invalid_stmt;
            if (auto it = class_decl_by_type_.find(owner_t); it != class_decl_by_type_.end()) {
                return it->second;
            }
            if (auto it = actor_decl_by_type_.find(owner_t); it != actor_decl_by_type_.end()) {
                return it->second;
            }
            if (auto it = enum_decl_by_type_.find(owner_t); it != enum_decl_by_type_.end()) {
                return it->second;
            }
            if (auto it = field_abi_meta_by_type_.find(owner_t); it != field_abi_meta_by_type_.end()) {
                return it->second.sid;
            }
            return ast::k_invalid_stmt;
        };

        auto collect_proto_closure = [&](auto&& self,
                                         ast::StmtId proto_sid,
                                         std::unordered_set<ast::StmtId>& out) -> void {
            if (proto_sid == ast::k_invalid_stmt || (size_t)proto_sid >= ast_.stmts().size()) return;
            if (!out.insert(proto_sid).second) return;
            const auto& ps = ast_.stmt(proto_sid);
            if (ps.kind != ast::StmtKind::kProtoDecl) return;

            const auto& refs = ast_.path_refs();
            const uint64_t ib = ps.decl_path_ref_begin;
            const uint64_t ie = ib + ps.decl_path_ref_count;
            if (ib <= refs.size() && ie <= refs.size()) {
                for (uint32_t i = ps.decl_path_ref_begin; i < ps.decl_path_ref_begin + ps.decl_path_ref_count; ++i) {
                    if (auto base_sid = resolve_proto_decl_from_path_ref_(refs[i], ps.span)) {
                        self(self, *base_sid, out);
                    }
                }
            }
        };

        auto proto_name_matches = [&](ast::StmtId proto_sid, std::string_view q) -> bool {
            if (proto_sid == ast::k_invalid_stmt || (size_t)proto_sid >= ast_.stmts().size()) return false;
            const auto& ps = ast_.stmt(proto_sid);
            if (ps.kind != ast::StmtKind::kProtoDecl) return false;
            if (ps.name == q) return true;
            if (auto it = proto_qualified_name_by_stmt_.find(proto_sid); it != proto_qualified_name_by_stmt_.end()) {
                if (it->second == q) return true;
            }
            return false;
        };

        auto check_proto_arrow_const_access = [&](ast::ExprId recv_eid,
                                                  std::optional<std::string_view> qualifier,
                                                  const ast::Expr& member_expr) -> ty::TypeId {
            if (member_expr.kind != ast::ExprKind::kIdent) {
                diag_(diag::Code::kTypeErrorGeneric, member_expr.span, "arrow member access requires identifier rhs");
                err_(member_expr.span, "arrow member access requires identifier rhs");
                return types_.error();
            }

            const ty::TypeId owner_t = resolve_member_owner_type(recv_eid, member_expr.span);
            if (is_error_(owner_t)) return types_.error();

            const ast::StmtId owner_sid = resolve_owner_decl_sid_for_proto(owner_t);
            if (owner_sid == ast::k_invalid_stmt || (size_t)owner_sid >= ast_.stmts().size()) {
                diag_(diag::Code::kProtoArrowMemberNotFound, member_expr.span, std::string(member_expr.text));
                err_(member_expr.span, "proto arrow member is unavailable for this receiver type");
                return types_.error();
            }

            std::unordered_set<ast::StmtId> proto_sids{};
            const auto& owner = ast_.stmt(owner_sid);
            const auto& refs = ast_.path_refs();
            const uint64_t rb = owner.decl_path_ref_begin;
            const uint64_t re = rb + owner.decl_path_ref_count;
            if (rb <= refs.size() && re <= refs.size()) {
                for (uint32_t i = owner.decl_path_ref_begin; i < owner.decl_path_ref_begin + owner.decl_path_ref_count; ++i) {
                    if (auto psid = resolve_proto_decl_from_path_ref_(refs[i], member_expr.span)) {
                        collect_proto_closure(collect_proto_closure, *psid, proto_sids);
                    }
                }
            }

            std::unordered_set<ast::StmtId> filtered{};
            for (const ast::StmtId psid : proto_sids) {
                if (psid == ast::k_invalid_stmt || (size_t)psid >= ast_.stmts().size()) continue;
                if (evaluate_proto_require_at_apply_(psid, owner_t, member_expr.span,
                                                     /*emit_unsatisfied_diag=*/false,
                                                     /*emit_shape_diag=*/false)) {
                    filtered.insert(psid);
                }
            }

            if (qualifier.has_value()) {
                std::unordered_set<ast::StmtId> narrowed{};
                for (const ast::StmtId psid : filtered) {
                    if (proto_name_matches(psid, *qualifier)) {
                        collect_proto_closure(collect_proto_closure, psid, narrowed);
                    }
                }
                if (narrowed.empty()) {
                    diag_(diag::Code::kProtoArrowMemberNotFound, member_expr.span, std::string(member_expr.text));
                    err_(member_expr.span, "unknown proto qualifier on arrow access");
                    return types_.error();
                }
                filtered = std::move(narrowed);
            }

            struct ConstCandidate {
                ast::StmtId proto_sid = ast::k_invalid_stmt;
                ast::StmtId var_sid = ast::k_invalid_stmt;
            };
            std::vector<ConstCandidate> const_candidates{};
            std::unordered_set<ast::StmtId> const_provider_protos{};
            bool has_provided_fn_with_same_name = false;

            const auto& kids = ast_.stmt_children();
            for (const ast::StmtId psid : filtered) {
                if (psid == ast::k_invalid_stmt || (size_t)psid >= ast_.stmts().size()) continue;
                const auto& ps = ast_.stmt(psid);
                const uint64_t mb = ps.stmt_begin;
                const uint64_t me = mb + ps.stmt_count;
                if (mb > kids.size() || me > kids.size()) continue;

                bool provided_const_here = false;
                for (uint32_t i = ps.stmt_begin; i < ps.stmt_begin + ps.stmt_count; ++i) {
                    const ast::StmtId msid = kids[i];
                    if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                    const auto& m = ast_.stmt(msid);

                    if (m.kind == ast::StmtKind::kFnDecl &&
                        m.proto_fn_role == ast::ProtoFnRole::kProvide &&
                        m.a != ast::k_invalid_stmt &&
                        m.name == member_expr.text) {
                        has_provided_fn_with_same_name = true;
                    }

                    if (m.kind != ast::StmtKind::kVar) continue;
                    if (!m.var_is_proto_provide || !m.is_const) continue;
                    if (m.name != member_expr.text) continue;
                    const_candidates.push_back({psid, msid});
                    provided_const_here = true;
                }
                if (provided_const_here) const_provider_protos.insert(psid);
            }

            if (const_candidates.empty()) {
                diag_(diag::Code::kProtoArrowMemberNotFound, member_expr.span, std::string(member_expr.text));
                if (has_provided_fn_with_same_name) {
                    err_(member_expr.span, "arrow member is a function; call it with (...)");
                } else {
                    err_(member_expr.span, "proto arrow const member is not found");
                }
                return types_.error();
            }

            if (!qualifier.has_value() && const_provider_protos.size() > 1) {
                diag_(diag::Code::kProtoArrowQualifierRequired, member_expr.span, std::string(member_expr.text));
                err_(member_expr.span, "arrow const member is provided by multiple protos; use receiver->Proto.member");
                return types_.error();
            }

            if (qualifier.has_value() && const_provider_protos.size() > 1) {
                diag_(diag::Code::kProtoArrowMemberAmbiguous, member_expr.span, std::string(member_expr.text));
                err_(member_expr.span, "arrow const member remains ambiguous in qualified proto closure");
                return types_.error();
            }

            const auto& chosen = const_candidates.front();
            if (chosen.var_sid == ast::k_invalid_stmt || (size_t)chosen.var_sid >= ast_.stmts().size()) {
                return types_.error();
            }

            const auto& var_decl = ast_.stmt(chosen.var_sid);
            if (current_expr_id_ != ast::k_invalid_expr &&
                current_expr_id_ < expr_proto_const_decl_cache_.size()) {
                expr_proto_const_decl_cache_[current_expr_id_] = chosen.var_sid;
            }

            return var_decl.type;
        };

        // value member access (v0): obj.field
        if (e.op == K::kArrow) {
            if (e.b == ast::k_invalid_expr) {
                diag_(diag::Code::kTypeErrorGeneric, e.span, "missing member on '->' access");
                err_(e.span, "missing member on '->' access");
                return types_.error();
            }
            const ast::Expr& rhs = ast_.expr(e.b);
            return check_proto_arrow_const_access(e.a, std::nullopt, rhs);
        }

        if (e.op == K::kDot) {
            if (e.a != ast::k_invalid_expr) {
                const auto& lhs = ast_.expr(e.a);
                if (lhs.kind == ast::ExprKind::kBinary &&
                    lhs.op == K::kArrow &&
                    lhs.a != ast::k_invalid_expr &&
                    lhs.b != ast::k_invalid_expr) {
                    const auto& qualifier_expr = ast_.expr(lhs.b);
                    if (qualifier_expr.kind != ast::ExprKind::kIdent) {
                        diag_(diag::Code::kTypeErrorGeneric, qualifier_expr.span,
                              "proto qualifier after '->' must be identifier");
                        err_(qualifier_expr.span, "invalid proto qualifier in arrow member access");
                        return types_.error();
                    }
                    if (e.b == ast::k_invalid_expr) {
                        diag_(diag::Code::kTypeErrorGeneric, e.span, "missing member after proto qualifier");
                        err_(e.span, "missing member after proto qualifier");
                        return types_.error();
                    }
                    const auto& rhs = ast_.expr(e.b);
                    return check_proto_arrow_const_access(lhs.a, qualifier_expr.text, rhs);
                }
            }

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

            if (in_actor_method_ && e.a != ast::k_invalid_expr) {
                const ast::Expr& lhs = ast_.expr(e.a);
                if (lhs.kind == ast::ExprKind::kIdent && lhs.text == "self") {
                    diag_(diag::Code::kActorSelfFieldAccessUseDraft, rhs.span, rhs.text);
                    err_(rhs.span, "actor state access must use 'draft." + std::string(rhs.text) + "'");
                    return types_.error();
                }
            }

            ty::TypeId base_t = check_expr_(e.a);
            base_t = read_decay_borrow_(types_, base_t);
            (void)ensure_generic_enum_instance_from_type_(base_t, e.span);

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

        // Pipe operators are canonicalized before tyck.
        // - '|>' valid form should be rewritten to Call Expr.
        // - '<|' is reserved but intentionally unsupported in v1.
        if (e.op == K::kPipeRev) {
            diag_(diag::Code::kPipeRevNotSupportedYet, e.span);
            err_(e.span, "pipe operator '<|' is not supported in v1");
            return types_.error();
        }
        if (e.op == K::kPipeFwd) {
            err_(e.span, "pipe expression was not canonicalized");
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

            if (auto sid = root_place_symbol_(e.a)) {
                const auto& lhs = ast_.expr(e.a);
                if (lhs.kind == ast::ExprKind::kIdent) {
                    mark_symbol_initialized_(*sid);
                }
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

        ty::TypeId lt = (e.op == K::kAssign) ? check_expr_place_no_read_(e.a) : check_expr_(e.a);
        ty::TypeId lhs_target = lt;
        {
            ty::TypeId elem = ty::kInvalidType;
            bool is_mut_borrow = false;
            if (borrow_info_(types_, lt, elem, is_mut_borrow) && is_mut_borrow) {
                lhs_target = elem; // &mut T place에는 T를 대입한다.
            }
        }

        if (e.op == K::kAssign) {
            if (auto sid = root_place_symbol_(e.a)) {
                const auto& lhs = ast_.expr(e.a);
                if (lhs.kind != ast::ExprKind::kIdent) {
                    (void)ensure_symbol_readable_(*sid, lhs.span);
                }
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
        if (assign_plan.ok) {
            mark_expr_move_consumed_(e.b, lhs_target, e.span);
            if (auto sid = root_place_symbol_(e.a)) {
                const auto& lhs = ast_.expr(e.a);
                if (lhs.kind == ast::ExprKind::kIdent) {
                    mark_symbol_initialized_(*sid);
                }
            }
        }
        return lhs_target;
    }
