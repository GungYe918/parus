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
    // call / array / index
    // --------------------
    ty::TypeId TypeChecker::check_expr_array_lit_(const ast::Expr& e) {
        // Array literal uses ast.args() slice: e.arg_begin..e.arg_begin+e.arg_count
        if (e.arg_count == 0) {
            diag_(diag::Code::kTypeArrayLiteralEmptyNeedsContext, e.span);
            err_(e.span, "empty array literal requires a contextual type (v0)");
            return types_.make_array(types_.error(), /*has_size=*/true, /*size=*/0);
        }

        const auto& args = ast_.args();
        const uint32_t end = e.arg_begin + e.arg_count;
        if (e.arg_begin >= args.size() || end > args.size()) {
            err_(e.span, "array literal element range is out of AST bounds");
            return types_.error();
        }

        ty::TypeId elem = ty::kInvalidType;
        bool has_error = false;

        for (uint32_t i = 0; i < e.arg_count; ++i) {
            const auto& a = args[e.arg_begin + i];
            if (a.expr == ast::k_invalid_expr) {
                has_error = true;
                continue;
            }

            ty::TypeId t = check_expr_(a.expr);
            if (is_error_(t)) {
                has_error = true;
                continue;
            }

            if (elem == ty::kInvalidType) {
                elem = t;
                continue;
            }

            if (elem == t) continue;

            const auto& et = types_.get(elem);
            const auto& tt = types_.get(t);
            const bool elem_is_infer = (et.kind == ty::Kind::kBuiltin && et.builtin == ty::Builtin::kInferInteger);
            const bool t_is_infer = (tt.kind == ty::Kind::kBuiltin && tt.builtin == ty::Builtin::kInferInteger);

            if (elem_is_infer && is_index_int_type_(t)) {
                (void)resolve_infer_int_in_context_(args[e.arg_begin].expr, t);
                elem = t;
                continue;
            }

            if (t_is_infer && is_index_int_type_(elem)) {
                (void)resolve_infer_int_in_context_(a.expr, elem);
                continue;
            }

            diag_(diag::Code::kTypeBinaryOperandsMustMatch, a.span, types_.to_string(elem), types_.to_string(t));
            err_(a.span, "array literal elements must have one unified type");
            has_error = true;
        }

        if (elem == ty::kInvalidType) elem = types_.error();
        if (has_error) elem = types_.error();

        return types_.make_array(elem, /*has_size=*/true, e.arg_count);
    }

    ty::TypeId TypeChecker::check_expr_field_init_(const ast::Expr& e) {
        std::string literal_head = e.text.empty()
            ? std::string("<field-init>")
            : std::string(e.text);
        ty::TypeId field_ty = ty::kInvalidType;
        if (e.field_init_type_node != ast::k_invalid_type_node &&
            (size_t)e.field_init_type_node < ast_.type_nodes().size()) {
            const auto& head = ast_.type_node(e.field_init_type_node);
            if (head.resolved_type != ty::kInvalidType) {
                field_ty = head.resolved_type;
                literal_head = types_.to_string(field_ty);
            }
        }

        if (field_ty == ty::kInvalidType && !e.text.empty()) {
            if (auto type_sym = lookup_symbol_(e.text)) {
                const auto& sym = sym_.symbol(*type_sym);
                if (sym.kind == sema::SymbolKind::kField && sym.declared_type != ty::kInvalidType) {
                    field_ty = sym.declared_type;
                }
            }
            if (field_ty == ty::kInvalidType) {
                const ty::TypeId literal_head_ty = types_.intern_ident(e.text);
                if (auto inst_sid = ensure_generic_field_instance_from_type_(literal_head_ty, e.span)) {
                    const auto& inst = ast_.stmt(*inst_sid);
                    if (inst.kind == ast::StmtKind::kFieldDecl && inst.type != ty::kInvalidType) {
                        field_ty = inst.type;
                    }
                }
            }
        }

        if (field_ty == ty::kInvalidType) {
            diag_(diag::Code::kFieldInitTypeExpected, e.span, literal_head);
            err_(e.span, "field initializer head must resolve to a struct type");
            return types_.error();
        }

        if (actor_decl_by_type_.find(field_ty) != actor_decl_by_type_.end()) {
            diag_(diag::Code::kActorBraceInitNotAllowed, e.span, literal_head);
            err_(e.span, "actor construction must use ctor-style call");
            return types_.error();
        }

        auto meta_it = field_abi_meta_by_type_.find(field_ty);
        if (meta_it == field_abi_meta_by_type_.end()) {
            (void)ensure_generic_field_instance_from_type_(field_ty, e.span);
            meta_it = field_abi_meta_by_type_.find(field_ty);
        }
        if (meta_it == field_abi_meta_by_type_.end()) {
            diag_(diag::Code::kFieldInitTypeExpected, e.span, literal_head);
            err_(e.span, "field initializer target has no field metadata");
            return types_.error();
        }

        const auto& meta = meta_it->second;
        if (meta.sid == ast::k_invalid_stmt || meta.sid >= ast_.stmts().size()) {
            diag_(diag::Code::kTypeFieldMemberRangeInvalid, e.span);
            err_(e.span, "invalid field metadata statement id");
            return types_.error();
        }

        const auto& fs = ast_.stmt(meta.sid);
        if (fs.decl_generic_param_count > 0) {
            diag_(diag::Code::kGenericTypeArgInferenceFailed, e.span, literal_head);
            err_(e.span, "generic struct literal requires explicit type arguments");
            return types_.error();
        }
        const uint64_t member_begin = fs.field_member_begin;
        const uint64_t member_end = member_begin + fs.field_member_count;
        if (member_begin > ast_.field_members().size() || member_end > ast_.field_members().size()) {
            diag_(diag::Code::kTypeFieldMemberRangeInvalid, e.span);
            err_(e.span, "invalid field member range");
            return types_.error();
        }

        if (e.field_init_count == 0 && fs.field_member_count != 0) {
            diag_(diag::Code::kFieldInitEmptyNotAllowed, e.span, types_.to_string(field_ty));
            err_(e.span, "empty field initializer is only allowed for zero-member field");
        }

        const auto& inits = ast_.field_init_entries();
        const uint64_t init_begin = e.field_init_begin;
        const uint64_t init_end = init_begin + e.field_init_count;
        if (init_begin > inits.size() || init_end > inits.size()) {
            diag_(diag::Code::kTypeFieldMemberRangeInvalid, e.span);
            err_(e.span, "field initializer entry range is out of AST bounds");
            return types_.error();
        }

        std::unordered_map<std::string_view, uint32_t> member_index_by_name;
        member_index_by_name.reserve(fs.field_member_count);
        for (uint32_t i = fs.field_member_begin; i < fs.field_member_begin + fs.field_member_count; ++i) {
            member_index_by_name[ast_.field_members()[i].name] = i;
        }

        std::unordered_set<std::string_view> seen_members;
        seen_members.reserve(e.field_init_count);

        for (uint32_t i = 0; i < e.field_init_count; ++i) {
            const auto& ent = inits[e.field_init_begin + i];

            const bool inserted = seen_members.insert(ent.name).second;
            if (!inserted) {
                diag_(diag::Code::kFieldInitDuplicateMember, ent.span, ent.name);
                err_(ent.span, "duplicate member in field initializer: " + std::string(ent.name));
                if (ent.expr != ast::k_invalid_expr) (void)check_expr_(ent.expr);
                continue;
            }

            auto mit = member_index_by_name.find(ent.name);
            if (mit == member_index_by_name.end()) {
                diag_(diag::Code::kFieldInitUnknownMember, ent.span, types_.to_string(field_ty), ent.name);
                err_(ent.span, "unknown member in field initializer: " + std::string(ent.name));
                if (ent.expr != ast::k_invalid_expr) (void)check_expr_(ent.expr);
                continue;
            }

            const auto& member = ast_.field_members()[mit->second];
            const CoercionPlan plan = classify_assign_with_coercion_(
                AssignSite::FieldInit, member.type, ent.expr, ent.span);
            ty::TypeId rhs_t = plan.src_after;

            if (is_null_(rhs_t) && !is_optional_(member.type)) {
                diag_(diag::Code::kFieldInitNonOptionalNull, ent.span, member.name, types_.to_string(member.type));
                err_(ent.span, "null is only allowed for optional field members");
                continue;
            }

            if (!plan.ok) {
                diag_(diag::Code::kTypeAssignMismatch, ent.span, types_.to_string(member.type), type_for_user_diag_(rhs_t, ent.expr));
                err_(ent.span, "field initializer member type mismatch");
            }
        }

        for (uint32_t i = fs.field_member_begin; i < fs.field_member_begin + fs.field_member_count; ++i) {
            const auto& member = ast_.field_members()[i];
            if (seen_members.find(member.name) != seen_members.end()) continue;
            diag_(diag::Code::kFieldInitMissingMember, e.span, types_.to_string(field_ty), member.name);
            err_(e.span, "missing member in field initializer: " + std::string(member.name));
        }

        return field_ty;
    }

    ty::TypeId TypeChecker::check_expr_index_(const ast::Expr& e) {
        // e.a = base, e.b = index expr
        ty::TypeId base_t = check_expr_(e.a);
        ty::TypeId arr_t = base_t;

        const auto& bt = types_.get(base_t);
        if (bt.kind == ty::Kind::kBorrow) {
            const auto& inner = types_.get(bt.elem);
            if (inner.kind == ty::Kind::kArray) {
                arr_t = bt.elem;
            }
        }

        const auto& t = types_.get(arr_t);
        if (t.kind != ty::Kind::kArray) {
            diag_(diag::Code::kTypeIndexNonArray, e.span, types_.to_string(base_t));
            err_(e.span, "indexing is only supported on array types (T[] / T[N]) in v0");
            return types_.error();
        }

        // slice range: x[a..b], x[a..:b]
        if (is_range_expr_(e.b)) {
            const auto& r = ast_.expr(e.b);
            auto check_bound = [&](ast::ExprId bid) {
                if (bid == ast::k_invalid_expr) return;
                ty::TypeId bt = check_expr_(bid);
                if (is_error_(bt)) return;

                const auto& btt = types_.get(bt);
                if (btt.kind == ty::Kind::kBuiltin && btt.builtin == ty::Builtin::kInferInteger) {
                    (void)resolve_infer_int_in_context_(bid, types_.builtin(ty::Builtin::kUSize));
                    bt = check_expr_(bid);
                }

                if (!is_index_int_type_(bt)) {
                    diag_(diag::Code::kTypeIndexMustBeUSize, ast_.expr(bid).span, types_.to_string(bt));
                    err_(e.span, "slice bounds must be integer type in v0");
                }
            };

            check_bound(r.a);
            check_bound(r.b);

            // Sized array + constant bounds: diagnose obvious OOB early.
            if (t.array_has_size) {
                auto const_i64 = [&](auto&& self, ast::ExprId bid) -> std::optional<int64_t> {
                    if (bid == ast::k_invalid_expr || bid >= ast_.exprs().size()) return std::nullopt;
                    const auto& be = ast_.expr(bid);

                    if (be.kind == ast::ExprKind::kIntLit) {
                        int64_t v = 0;
                        if (parse_i64_lit_(be.text, v)) return v;
                        return std::nullopt;
                    }

                    if (be.kind == ast::ExprKind::kUnary &&
                        (be.op == syntax::TokenKind::kMinus || be.op == syntax::TokenKind::kPlus) &&
                        be.a != ast::k_invalid_expr) {
                        auto inner = self(self, be.a);
                        if (!inner.has_value()) return std::nullopt;
                        if (be.op == syntax::TokenKind::kMinus) return -*inner;
                        return *inner;
                    }

                    return std::nullopt;
                };

                const auto lo = const_i64(const_i64, r.a);
                const auto hi = const_i64(const_i64, r.b);
                if (lo.has_value() && hi.has_value()) {
                    bool bad = false;
                    int64_t hi_exclusive = *hi;
                    if (r.op == syntax::TokenKind::kDotDotColon) {
                        if (hi_exclusive == std::numeric_limits<int64_t>::max()) {
                            bad = true;
                        } else {
                            hi_exclusive += 1;
                        }
                    }

                    if (!bad) {
                        if (*lo < 0 || hi_exclusive < 0 || *lo > hi_exclusive) {
                            diag_(
                                diag::Code::kTypeSliceConstRangeInvalid,
                                e.span,
                                std::to_string(*lo),
                                std::to_string(hi_exclusive)
                            );
                            err_(e.span, "invalid constant slice range");
                            bad = true;
                        }
                    }

                    if (!bad) {
                        const int64_t len = static_cast<int64_t>(t.array_size);
                        if (hi_exclusive > len) {
                            diag_(
                                diag::Code::kTypeSliceConstOutOfBounds,
                                e.span,
                                std::to_string(len),
                                std::to_string(hi_exclusive)
                            );
                            err_(e.span, "constant slice out of bounds");
                        }
                    }
                }
            }

            // slicing result is unsized element view (T[])
            return types_.make_array(t.elem);
        }

        ty::TypeId it = check_expr_(e.b);
        if (!is_error_(it)) {
            const auto& itt = types_.get(it);
            if (itt.kind == ty::Kind::kBuiltin && itt.builtin == ty::Builtin::kInferInteger) {
                (void)resolve_infer_int_in_context_(e.b, types_.builtin(ty::Builtin::kUSize));
                it = check_expr_(e.b);
            }
        }

        // index는 정수 타입만 허용(v0)
        if (!is_error_(it) && !is_index_int_type_(it)) {
            diag_(diag::Code::kTypeIndexMustBeUSize, ast_.expr(e.b).span, types_.to_string(it));
            err_(e.span, "index expression must be integer type in v0");
        }

        return t.elem;
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

        const OwnershipStateMap before = capture_ownership_state_();

        // branches are always value-checked as expressions
        restore_ownership_state_(before);
        ty::TypeId t_then = check_expr_(e.b, Slot::kValue);
        const OwnershipStateMap then_state = capture_ownership_state_();

        restore_ownership_state_(before);
        ty::TypeId t_else = check_expr_(e.c, Slot::kValue);
        const OwnershipStateMap else_state = capture_ownership_state_();

        (void)slot; // currently result type doesn't depend on slot
        restore_ownership_state_(before);
        merge_ownership_state_from_branches_(before, {then_state, else_state}, /*include_before_as_fallthrough=*/e.c == ast::k_invalid_expr);
        return unify_(t_then, t_else);
    }

    ty::TypeId TypeChecker::check_expr_block_(const ast::Expr& e) {
        return check_expr_block_(e, Slot::kValue);
    }

    ty::TypeId TypeChecker::check_expr_block_(const ast::Expr& e, Slot slot) {
        const ast::StmtId block_sid = e.block_stmt;
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
        if (e.block_tail != ast::k_invalid_expr) {
            out = check_expr_(e.block_tail, Slot::kValue);
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

        parus::LoopSourceKind loop_source_kind = parus::LoopSourceKind::kNone;
        ty::TypeId loop_binder_type = ty::kInvalidType;

        auto cache_loop_meta = [&]() {
            if (current_expr_id_ == ast::k_invalid_expr) return;
            if ((size_t)current_expr_id_ < expr_loop_source_kind_cache_.size()) {
                expr_loop_source_kind_cache_[current_expr_id_] =
                    static_cast<uint8_t>(loop_source_kind);
            }
            if ((size_t)current_expr_id_ < expr_loop_binder_type_cache_.size()) {
                expr_loop_binder_type_cache_[current_expr_id_] = loop_binder_type;
            }
        };

        auto set_loop_binder = [&](ty::TypeId t) {
            loop_binder_type = t;
            if (!e.loop_var.empty()) {
                sym_.insert(sema::SymbolKind::kVar, e.loop_var, t, e.span);
            }
        };

        // loop scope: variable binding + body scope
        const OwnershipStateMap before = capture_ownership_state_();
        sym_.push_scope();

        // header: loop (v in xs) { ... }
        if (e.loop_has_header) {
            if (e.loop_iter == ast::k_invalid_expr || (size_t)e.loop_iter >= ast_.exprs().size()) {
                diag_(diag::Code::kLoopIterableUnsupported, e.span);
                err_(e.span, "loop header is missing iterable expression");
                set_loop_binder(types_.error());
                loop_source_kind = parus::LoopSourceKind::kIteratorFutureUnsupported;
            } else if (is_range_expr_(e.loop_iter)) {
                const auto& r = ast_.expr(e.loop_iter);
                ty::TypeId lhs_t = check_expr_(r.a, Slot::kValue);
                ty::TypeId rhs_t = check_expr_(r.b, Slot::kValue);

                const bool lhs_infer =
                    !is_error_(lhs_t) &&
                    types_.get(lhs_t).kind == ty::Kind::kBuiltin &&
                    types_.get(lhs_t).builtin == ty::Builtin::kInferInteger;
                const bool rhs_infer =
                    !is_error_(rhs_t) &&
                    types_.get(rhs_t).kind == ty::Kind::kBuiltin &&
                    types_.get(rhs_t).builtin == ty::Builtin::kInferInteger;

                if (lhs_infer && rhs_infer) {
                    diag_(diag::Code::kLoopRangeNeedsTypedBound, e.loop_iter == ast::k_invalid_expr ? e.span : ast_.expr(e.loop_iter).span);
                    err_(e.span, "loop range requires at least one typed integer bound");
                    lhs_t = rhs_t = types_.error();
                } else if (lhs_infer && !is_error_(rhs_t)) {
                    if (!resolve_infer_int_in_context_(r.a, rhs_t)) {
                        lhs_t = types_.error();
                    } else {
                        lhs_t = check_expr_(r.a, Slot::kValue);
                    }
                } else if (rhs_infer && !is_error_(lhs_t)) {
                    if (!resolve_infer_int_in_context_(r.b, lhs_t)) {
                        rhs_t = types_.error();
                    } else {
                        rhs_t = check_expr_(r.b, Slot::kValue);
                    }
                }

                lhs_t = canonicalize_transparent_external_typedef_(lhs_t);
                rhs_t = canonicalize_transparent_external_typedef_(rhs_t);

                if (!is_error_(lhs_t) && !is_index_int_type_(lhs_t)) {
                    diag_(diag::Code::kLoopRangeBoundMustBeInteger, ast_.expr(r.a).span);
                    err_(ast_.expr(r.a).span, "loop range lower bound must be integer");
                    lhs_t = types_.error();
                }
                if (!is_error_(rhs_t) && !is_index_int_type_(rhs_t)) {
                    diag_(diag::Code::kLoopRangeBoundMustBeInteger, ast_.expr(r.b).span);
                    err_(ast_.expr(r.b).span, "loop range upper bound must be integer");
                    rhs_t = types_.error();
                }
                if (!is_error_(lhs_t) && !is_error_(rhs_t) && lhs_t != rhs_t) {
                    diag_(diag::Code::kLoopRangeBoundTypeMismatch, ast_.expr(e.loop_iter).span);
                    err_(e.span, "loop range bounds must have the same concrete integer type");
                    lhs_t = rhs_t = types_.error();
                }

                loop_source_kind =
                    (r.op == parus::syntax::TokenKind::kDotDotColon)
                        ? parus::LoopSourceKind::kRangeInclusive
                        : parus::LoopSourceKind::kRangeExclusive;
                set_loop_binder(lhs_t);
            } else {
                ty::TypeId iter_t = canonicalize_transparent_external_typedef_(check_expr_(e.loop_iter, Slot::kValue));
                ty::TypeId array_t = iter_t;

                if (!is_error_(iter_t)) {
                    const auto& it = types_.get(iter_t);
                    if (it.kind == ty::Kind::kBorrow && it.elem != ty::kInvalidType) {
                        const ty::TypeId inner_t = canonicalize_transparent_external_typedef_(it.elem);
                        if (types_.get(inner_t).kind == ty::Kind::kArray) {
                            array_t = inner_t;
                        }
                    }
                }

                if (!is_error_(array_t) &&
                    array_t != ty::kInvalidType &&
                    types_.get(array_t).kind == ty::Kind::kArray) {
                    const auto& at = types_.get(array_t);
                    loop_source_kind =
                        at.array_has_size
                            ? parus::LoopSourceKind::kSizedArray
                            : parus::LoopSourceKind::kSliceView;
                    set_loop_binder(at.elem);
                } else {
                    loop_source_kind = parus::LoopSourceKind::kIteratorFutureUnsupported;
                    diag_(diag::Code::kLoopIterableUnsupported, ast_.expr(e.loop_iter).span);
                    err_(e.span, "loop iterable is unsupported in v0 (iterator protocol is deferred)");
                    set_loop_binder(types_.error());
                }
            }
        }

        cache_loop_meta();

        if (e.loop_has_header && is_error_(loop_binder_type)) {
            sym_.pop_scope();
            restore_ownership_state_(before);
            return types_.error();
        }

        // push loop ctx
        loop_stack_.push_back(lc);

        // body is a block stmt
        if (e.loop_body != ast::k_invalid_stmt) {
            ++stmt_loop_depth_;
            check_stmt_(e.loop_body);
            if (stmt_loop_depth_ > 0) --stmt_loop_depth_;
        } else {
            err_(e.span, "loop has no body");
        }

        // pop loop ctx
        LoopCtx done = loop_stack_.back();
        loop_stack_.pop_back();

        const OwnershipStateMap after_body = capture_ownership_state_();
        sym_.pop_scope();
        restore_ownership_state_(before);
        merge_ownership_state_from_branches_(before, {after_body}, /*include_before_as_fallthrough=*/e.loop_has_header);

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

} // namespace parus::tyck
