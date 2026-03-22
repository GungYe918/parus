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
                const bool struct_like_external_type =
                    sym.kind == sema::SymbolKind::kType &&
                    sym.is_external &&
                    (!sym.external_field_payload.empty() ||
                     sym.external_payload.starts_with("parus_field_decl"));
                if ((sym.kind == sema::SymbolKind::kField || struct_like_external_type) &&
                    sym.declared_type != ty::kInvalidType) {
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

        if (e.text == "text") {
            if (!has_manual_permission_(ast::kManualPermAbi)) {
                diag_(diag::Code::kManualAbiRequired, e.span);
                err_(e.span, "text view construction requires manual[abi]");
                return types_.error();
            }

            const ty::TypeId text_ty = types_.builtin(ty::Builtin::kText);
            const ty::TypeId usize_ty = types_.builtin(ty::Builtin::kUSize);
            const auto& inits = ast_.field_init_entries();

            auto is_byte_compatible_ptr = [&](ty::TypeId t) -> bool {
                if (t == ty::kInvalidType || is_error_(t)) return false;
                const auto& tt = types_.get(t);
                if (tt.kind != ty::Kind::kPtr || tt.elem == ty::kInvalidType) return false;
                ty::TypeId elem = canonicalize_transparent_external_typedef_(tt.elem);
                if (elem == ty::kInvalidType || elem >= types_.count()) return false;
                const auto& et = types_.get(elem);
                if (et.kind == ty::Kind::kBuiltin) {
                    switch (et.builtin) {
                        case ty::Builtin::kU8:
                        case ty::Builtin::kI8:
                        case ty::Builtin::kCChar:
                        case ty::Builtin::kCSChar:
                        case ty::Builtin::kCUChar:
                            return true;
                        default:
                            return false;
                    }
                }
                if (et.kind == ty::Kind::kNamedUser) {
                    std::vector<std::string_view> path{};
                    std::vector<ty::TypeId> args{};
                    if (!types_.decompose_named_user(elem, path, args)) return false;
                    if (!args.empty() || path.empty()) return false;
                    const std::string_view leaf = path.back();
                    if (!(leaf == "c_char" || leaf == "c_schar" || leaf == "c_uchar")) {
                        return false;
                    }
                    if (path.size() == 1) return true;
                    return path[path.size() - 2] == "ext";
                }
                return false;
            };

            bool saw_data = false;
            bool saw_len = false;
            std::unordered_set<std::string_view> seen_members;
            seen_members.reserve(e.field_init_count);

            for (uint32_t i = 0; i < e.field_init_count; ++i) {
                const auto& ent = inits[e.field_init_begin + i];
                const bool inserted = seen_members.insert(ent.name).second;
                if (!inserted) {
                    diag_(diag::Code::kFieldInitDuplicateMember, ent.span, ent.name);
                    err_(ent.span, "duplicate member in text view initializer: " + std::string(ent.name));
                    if (ent.expr != ast::k_invalid_expr) (void)check_expr_(ent.expr);
                    continue;
                }

                if (ent.name == "data") {
                    saw_data = true;
                    const ty::TypeId data_ty = check_expr_(ent.expr);
                    if (!is_byte_compatible_ptr(data_ty)) {
                        diag_(diag::Code::kTypeAssignMismatch, ent.span,
                              "byte-compatible raw pointer", type_for_user_diag_(data_ty, ent.expr));
                        err_(ent.span, "text.data requires a byte-compatible raw pointer");
                    }
                    continue;
                }

                if (ent.name == "len") {
                    saw_len = true;
                    const CoercionPlan plan = classify_assign_with_coercion_(
                        AssignSite::FieldInit, usize_ty, ent.expr, ent.span);
                    if (!plan.ok) {
                        diag_(diag::Code::kTypeAssignMismatch, ent.span,
                              types_.to_string(usize_ty), type_for_user_diag_(plan.src_after, ent.expr));
                        err_(ent.span, "text.len requires a usize-compatible expression");
                    }
                    continue;
                }

                diag_(diag::Code::kFieldInitUnknownMember, ent.span, "text", ent.name);
                err_(ent.span, "unknown member in text view initializer: " + std::string(ent.name));
                if (ent.expr != ast::k_invalid_expr) (void)check_expr_(ent.expr);
            }

            if (!saw_data) {
                diag_(diag::Code::kFieldInitMissingMember, e.span, "text", "data");
                err_(e.span, "missing member in text view initializer: data");
            }
            if (!saw_len) {
                diag_(diag::Code::kFieldInitMissingMember, e.span, "text", "len");
                err_(e.span, "missing member in text view initializer: len");
            }
            return text_ty;
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
        lc.break_expected_type = canonicalize_transparent_external_typedef_(e.target_type);
        if (is_optional_(lc.break_expected_type)) {
            lc.break_expected_type = optional_elem_(lc.break_expected_type);
        }
        if (lc.break_expected_type != ty::kInvalidType && is_error_(lc.break_expected_type)) {
            lc.break_expected_type = ty::kInvalidType;
        }

        parus::LoopSourceKind loop_source_kind = parus::LoopSourceKind::kNone;
        ty::TypeId loop_binder_type = ty::kInvalidType;
        ty::TypeId loop_iterator_type = ty::kInvalidType;
        ast::StmtId loop_iter_decl = ast::k_invalid_stmt;
        uint32_t loop_iter_external_symbol = sema::SymbolTable::kNoScope;
        ast::StmtId loop_next_decl = ast::k_invalid_stmt;
        uint32_t loop_next_external_symbol = sema::SymbolTable::kNoScope;

        auto cache_loop_meta = [&]() {
            if (current_expr_id_ == ast::k_invalid_expr) return;
            if ((size_t)current_expr_id_ < expr_loop_source_kind_cache_.size()) {
                expr_loop_source_kind_cache_[current_expr_id_] =
                    static_cast<uint8_t>(loop_source_kind);
            }
            if ((size_t)current_expr_id_ < expr_loop_binder_type_cache_.size()) {
                expr_loop_binder_type_cache_[current_expr_id_] = loop_binder_type;
            }
            if ((size_t)current_expr_id_ < expr_loop_iterator_type_cache_.size()) {
                expr_loop_iterator_type_cache_[current_expr_id_] = loop_iterator_type;
            }
            if ((size_t)current_expr_id_ < expr_loop_iter_decl_cache_.size()) {
                expr_loop_iter_decl_cache_[current_expr_id_] = loop_iter_decl;
            }
            if ((size_t)current_expr_id_ < expr_loop_iter_external_symbol_cache_.size()) {
                expr_loop_iter_external_symbol_cache_[current_expr_id_] = loop_iter_external_symbol;
            }
            if ((size_t)current_expr_id_ < expr_loop_next_decl_cache_.size()) {
                expr_loop_next_decl_cache_[current_expr_id_] = loop_next_decl;
            }
            if ((size_t)current_expr_id_ < expr_loop_next_external_symbol_cache_.size()) {
                expr_loop_next_external_symbol_cache_[current_expr_id_] = loop_next_external_symbol;
            }
        };

        auto set_loop_binder = [&](ty::TypeId t) {
            loop_binder_type = t;
            if (!e.loop_var.empty()) {
                auto ins = sym_.insert(sema::SymbolKind::kVar, e.loop_var, t, e.span);
                if (!ins.ok) {
                    diag_(diag::Code::kDuplicateDecl, e.span, e.loop_var);
                    err_(e.span, "failed to bind loop variable '" + std::string(e.loop_var) + "'");
                    loop_binder_type = types_.error();
                    return;
                }
                if (ins.is_shadowing) {
                    diag_(diag::Code::kShadowing, e.span, e.loop_var);
                }
            }
        };

        struct LoopMethodTarget {
            ty::TypeId fn_type = ty::kInvalidType;
            ast::StmtId decl_sid = ast::k_invalid_stmt;
            uint32_t external_sym = sema::SymbolTable::kNoScope;
            bool external_is_template = false;
            bool ok = false;
        };

        auto resolve_proto_sid_for_loop_ = [&](std::initializer_list<std::string_view> names)
            -> std::optional<ast::StmtId> {
            for (const auto name : names) {
                if (auto sid = resolve_proto_sid_for_constraint_(std::string(name))) return sid;
                if (auto it = proto_decl_by_name_.find(std::string(name)); it != proto_decl_by_name_.end()) {
                    return it->second;
                }
            }
            return std::nullopt;
        };

        auto owner_base_name_matches = [&](std::string_view lhs, std::string_view rhs) -> bool {
            if (lhs == rhs) return true;
            auto suffix_match = [](std::string_view full, std::string_view suffix) -> bool {
                if (full.size() <= suffix.size() + 2u) return false;
                if (!full.ends_with(suffix)) return false;
                const size_t split = full.size() - suffix.size();
                return full[split - 1] == ':' && full[split - 2] == ':';
            };
            return suffix_match(lhs, rhs) || suffix_match(rhs, lhs);
        };

        auto resolve_owner_type_in_map = [&](const auto& method_map, ty::TypeId t) -> ty::TypeId {
            if (t == ty::kInvalidType) return t;
            if (method_map.find(t) != method_map.end()) return t;
            const auto& tt = types_.get(t);
            if (tt.kind != ty::Kind::kNamedUser) return t;
            if (auto sid = lookup_symbol_(types_.to_string(t))) {
                const auto& ss = sym_.symbol(*sid);
                if (ss.kind == sema::SymbolKind::kType &&
                    method_map.find(ss.declared_type) != method_map.end()) {
                    return ss.declared_type;
                }
            }
            return t;
        };

        auto resolve_loop_method = [&](ty::TypeId owner_t,
                                       std::string_view member_name,
                                       bool require_mut_self,
                                       Span member_span) -> LoopMethodTarget {
            LoopMethodTarget out{};
            owner_t = canonicalize_acts_owner_type_(owner_t);
            if (owner_t == ty::kInvalidType || member_name.empty()) return out;

            auto local_receiver_matches = [&](const ast::Stmt& fn) -> bool {
                if (fn.kind != ast::StmtKind::kFnDecl || fn.param_count == 0) return false;
                const auto& p0 = ast_.params()[fn.param_begin];
                if (!p0.is_self) return false;
                if (require_mut_self) {
                    return p0.self_kind == ast::SelfReceiverKind::kMut;
                }
                return p0.self_kind == ast::SelfReceiverKind::kRead;
            };

            auto external_receiver_matches = [&](ty::TypeId fn_t) -> bool {
                if (fn_t == ty::kInvalidType || fn_t >= types_.count()) return false;
                const auto& ft = types_.get(fn_t);
                if (ft.kind != ty::Kind::kFn || ft.param_count == 0) return false;
                ty::TypeId self_t = canonicalize_transparent_external_typedef_(types_.fn_param_at(fn_t, 0));
                if (self_t == ty::kInvalidType || self_t >= types_.count()) return false;
                const auto& st = types_.get(self_t);
                if (st.kind != ty::Kind::kBorrow || st.elem == ty::kInvalidType) return false;
                const ty::TypeId elem_t = canonicalize_transparent_external_typedef_(st.elem);
                if (elem_t != owner_t) return false;
                return require_mut_self ? st.borrow_is_mut : !st.borrow_is_mut;
            };

            auto adopt_local = [&](ast::StmtId sid) -> bool {
                if (sid == ast::k_invalid_stmt || static_cast<size_t>(sid) >= ast_.stmts().size()) return false;
                const auto& fn = ast_.stmt(sid);
                if (!local_receiver_matches(fn)) return false;
                if (out.ok) {
                    diag_(diag::Code::kTypeErrorGeneric, member_span,
                          "ambiguous iteration method '" + std::string(member_name) + "'");
                    err_(member_span, "ambiguous iteration method");
                    out.fn_type = types_.error();
                    out.ok = true;
                    return true;
                }
                out.ok = true;
                out.decl_sid = sid;
                out.fn_type = fn.type;
                return true;
            };

            auto adopt_external = [&](uint32_t sym_sid, ty::TypeId fn_t, bool candidate_is_template) -> bool {
                if (!external_receiver_matches(fn_t)) return false;
                if (out.ok) {
                    const bool current_is_external =
                        out.decl_sid == ast::k_invalid_stmt &&
                        out.external_sym != sema::SymbolTable::kNoScope;
                    if (current_is_external) {
                        if (out.fn_type == fn_t) {
                            if (out.external_is_template && !candidate_is_template) {
                                out.external_sym = sym_sid;
                                out.external_is_template = false;
                            }
                            return true;
                        }
                        if (out.external_is_template && !candidate_is_template) {
                            out.external_sym = sym_sid;
                            out.fn_type = fn_t;
                            out.external_is_template = false;
                            return true;
                        }
                        if (!out.external_is_template && candidate_is_template) {
                            return true;
                        }
                    }
                    diag_(diag::Code::kTypeErrorGeneric, member_span,
                          "ambiguous external iteration method '" + std::string(member_name) + "'");
                    err_(member_span, "ambiguous external iteration method");
                    out.fn_type = types_.error();
                    out.ok = true;
                    return true;
                }
                out.ok = true;
                out.external_sym = sym_sid;
                out.fn_type = fn_t;
                out.external_is_template = candidate_is_template;
                return true;
            };

            ty::TypeId class_owner_t = resolve_owner_type_in_map(class_effective_method_map_, owner_t);
            if (class_owner_t != ty::kInvalidType &&
                class_effective_method_map_.find(class_owner_t) == class_effective_method_map_.end()) {
                if (auto cit = class_decl_by_type_.find(class_owner_t); cit != class_decl_by_type_.end()) {
                    const ast::StmtId csid = cit->second;
                    if (generic_decl_checked_instances_.find(csid) == generic_decl_checked_instances_.end() &&
                        generic_decl_checking_instances_.find(csid) == generic_decl_checking_instances_.end()) {
                        check_stmt_class_decl_(csid);
                        generic_decl_checked_instances_.insert(csid);
                    }
                }
            }
            class_owner_t = resolve_owner_type_in_map(class_effective_method_map_, owner_t);
            if (class_owner_t != ty::kInvalidType) {
                auto oit = class_effective_method_map_.find(class_owner_t);
                if (oit != class_effective_method_map_.end()) {
                    auto mit = oit->second.find(std::string(member_name));
                    if (mit != oit->second.end()) {
                        for (const auto sid : mit->second) {
                            (void)adopt_local(sid);
                        }
                    }
                }
            }

            for (const auto& md : lookup_acts_methods_for_call_(owner_t, member_name, nullptr)) {
                (void)adopt_local(md.fn_sid);
            }

            auto oit = external_acts_default_method_map_.find(owner_t);
            if (oit != external_acts_default_method_map_.end()) {
                auto mit = oit->second.find(std::string(member_name));
                if (mit != oit->second.end()) {
                    for (const auto& md : mit->second) {
                        if (md.fn_symbol == sema::SymbolTable::kNoScope ||
                            md.fn_symbol >= sym_.symbols().size()) {
                            continue;
                        }
                        (void)adopt_external(md.fn_symbol,
                                             sym_.symbol(md.fn_symbol).declared_type,
                                             /*candidate_is_template=*/false);
                    }
                }
            }

            std::string owner_base{};
            std::vector<ty::TypeId> owner_args{};
            if (decompose_named_user_type_(owner_t, owner_base, owner_args) && !owner_base.empty()) {
                for (const auto& [template_owner_base, member_map] : external_acts_template_method_map_) {
                    if (!owner_base_name_matches(owner_base, template_owner_base)) continue;
                    auto mit = member_map.find(std::string(member_name));
                    if (mit == member_map.end()) continue;
                    for (const auto& md : mit->second) {
                        if (md.fn_symbol == sema::SymbolTable::kNoScope ||
                            md.fn_symbol >= sym_.symbols().size()) {
                            continue;
                        }
                        const auto& ss = sym_.symbol(md.fn_symbol);
                        ty::TypeId fn_t = ss.declared_type;
                        if (md.owner_is_generic_template) {
                            struct ExternalGenericDeclMeta {
                                std::vector<std::string> params{};
                            };
                            auto payload_unescape_value_ = [](std::string_view raw) -> std::string {
                                auto hex_value = [](char ch) -> int {
                                    if (ch >= '0' && ch <= '9') return ch - '0';
                                    if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
                                    if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
                                    return -1;
                                };
                                std::string out{};
                                out.reserve(raw.size());
                                for (size_t i = 0; i < raw.size(); ++i) {
                                    if (raw[i] == '%' && i + 2 < raw.size()) {
                                        const int hi = hex_value(raw[i + 1]);
                                        const int lo = hex_value(raw[i + 2]);
                                        if (hi >= 0 && lo >= 0) {
                                            out.push_back(static_cast<char>((hi << 4) | lo));
                                            i += 2;
                                            continue;
                                        }
                                    }
                                    out.push_back(raw[i]);
                                }
                                return out;
                            };
                            auto parse_external_generic_decl_meta_ = [&](std::string_view payload) {
                                ExternalGenericDeclMeta meta{};
                                size_t pos = 0;
                                while (pos < payload.size()) {
                                    size_t next = payload.find('|', pos);
                                    if (next == std::string_view::npos) next = payload.size();
                                    const std::string_view part = payload.substr(pos, next - pos);
                                    if (part.starts_with("gparam=")) {
                                        meta.params.push_back(
                                            payload_unescape_value_(part.substr(std::string_view("gparam=").size()))
                                        );
                                    }
                                    if (next == payload.size()) break;
                                    pos = next + 1;
                                }
                                return meta;
                            };
                            const auto meta = parse_external_generic_decl_meta_(ss.external_payload);
                            if (owner_args.size() != md.owner_generic_arity ||
                                meta.params.size() < md.owner_generic_arity) {
                                continue;
                            }
                            std::unordered_map<std::string, ty::TypeId> bindings{};
                            for (size_t i = 0; i < owner_args.size(); ++i) {
                                bindings.emplace(meta.params[i], owner_args[i]);
                            }
                            fn_t = substitute_generic_type_(fn_t, bindings);
                        }
                        (void)adopt_external(md.fn_symbol, fn_t, /*candidate_is_template=*/true);
                    }
                }
            }

            if (!out.ok) {
                diag_(diag::Code::kLoopIterableUnsupported, member_span);
                err_(member_span,
                     "loop iterable is missing required '" + std::string(member_name) + "' method");
                return out;
            }
            return out;
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
                    const ty::TypeId default_range_int = types_.builtin(ty::Builtin::kI32);
                    if (!resolve_infer_int_in_context_(r.a, default_range_int)) {
                        lhs_t = types_.error();
                    } else {
                        lhs_t = check_expr_(r.a, Slot::kValue);
                    }
                    if (!resolve_infer_int_in_context_(r.b, default_range_int)) {
                        rhs_t = types_.error();
                    } else {
                        rhs_t = check_expr_(r.b, Slot::kValue);
                    }
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
                    const auto seq_proto_sid = resolve_proto_sid_for_loop_(
                        {"iter::Sequence", "core::iter::Sequence", "Sequence"}
                    );
                    const auto iter_proto_sid = resolve_proto_sid_for_loop_(
                        {"iter::Iterator", "core::iter::Iterator", "Iterator"}
                    );
                    if (!seq_proto_sid.has_value()) {
                        diag_(diag::Code::kLoopIterableUnsupported, ast_.expr(e.loop_iter).span);
                        err_(e.span, "loop iterable is missing iter::Sequence protocol metadata");
                        set_loop_binder(types_.error());
                    } else if (!type_satisfies_proto_constraint_(iter_t, *seq_proto_sid, e.span)) {
                        diag_(diag::Code::kLoopIterableUnsupported, ast_.expr(e.loop_iter).span);
                        err_(e.span, "loop iterable does not satisfy iter::Sequence");
                        set_loop_binder(types_.error());
                    } else {
                                const auto iter_method =
                            resolve_loop_method(iter_t, "iter", /*require_mut_self=*/false, ast_.expr(e.loop_iter).span);
                        if (!iter_method.ok || is_error_(iter_method.fn_type)) {
                            set_loop_binder(types_.error());
                        } else {
                            ty::TypeId iterator_t = canonicalize_transparent_external_typedef_(
                                types_.get(iter_method.fn_type).ret
                            );
                            if (iterator_t == ty::kInvalidType || is_error_(iterator_t)) {
                                diag_(diag::Code::kLoopIterableUnsupported, ast_.expr(e.loop_iter).span);
                                err_(e.span, "iter::Sequence.iter() must return a concrete iterator type");
                                set_loop_binder(types_.error());
                            } else if (iter_proto_sid.has_value() &&
                                       !type_satisfies_proto_constraint_(iterator_t, *iter_proto_sid, e.span)) {
                                diag_(diag::Code::kLoopIterableUnsupported, ast_.expr(e.loop_iter).span);
                                err_(e.span, "loop iterator does not satisfy iter::Iterator");
                                set_loop_binder(types_.error());
                            } else {
                                const auto next_method =
                                    resolve_loop_method(iterator_t, "next", /*require_mut_self=*/true, ast_.expr(e.loop_iter).span);
                                const ty::TypeId next_ret =
                                    next_method.ok ? canonicalize_transparent_external_typedef_(types_.get(next_method.fn_type).ret)
                                                   : ty::kInvalidType;
                                if (!next_method.ok || next_ret == ty::kInvalidType || !is_optional_(next_ret)) {
                                    diag_(diag::Code::kLoopIterableUnsupported, ast_.expr(e.loop_iter).span);
                                    err_(e.span, "iter::Iterator.next(self mut) must return Item?");
                                    set_loop_binder(types_.error());
                                } else {
                                    loop_source_kind = parus::LoopSourceKind::kSequence;
                                    loop_iterator_type = iterator_t;
                                    loop_iter_decl = iter_method.decl_sid;
                                    loop_iter_external_symbol = iter_method.external_sym;
                                    loop_next_decl = next_method.decl_sid;
                                    loop_next_external_symbol = next_method.external_sym;
                                    set_loop_binder(optional_elem_(next_ret));
                                }
                            }
                        }
                    }
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
        break_target_stack_.push_back(BreakTargetKind::kLoopExpr);

        // body is a block stmt
        if (e.loop_body != ast::k_invalid_stmt) {
            ++stmt_loop_depth_;
            check_stmt_(e.loop_body);
            if (stmt_loop_depth_ > 0) --stmt_loop_depth_;
        } else {
            err_(e.span, "loop has no body");
        }

        // pop loop ctx
        if (!break_target_stack_.empty()) break_target_stack_.pop_back();
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
