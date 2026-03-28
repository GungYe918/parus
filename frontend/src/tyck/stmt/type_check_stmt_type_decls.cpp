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
            ? types_.intern_ident(
                  s.name.empty() ? std::string("Self") : qualify_decl_name_(std::string(s.name)))
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
        {
            std::unordered_set<std::string> generic_params;
            for (const auto& name : collect_decl_generic_param_names_(s)) {
                generic_params.insert(name);
            }
            (void)validate_constraint_clause_decl_(s.decl_constraint_begin, s.decl_constraint_count, generic_params, s.span);
        }
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

        ty::TypeId self_ty = s.type;
        if (self_ty == ty::kInvalidType && !s.name.empty()) {
            self_ty = types_.intern_ident(qualify_decl_name_(std::string(s.name)));
        }
        if (self_ty != ty::kInvalidType) {
            FieldAbiMeta meta{};
            meta.sid = sid;
            meta.layout = s.field_layout;
            meta.align = s.field_align;
            field_abi_meta_by_type_[self_ty] = meta;
        }

        if (is_generic_template) {
            return;
        }

        ensure_generic_acts_for_owner_(self_ty, s.span);

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
                    << "' must use a POD builtin value type, `~T`/`(~T)?`, or recursively-sized owner arrays in this round, got "
                    << types_.to_string(m.type);
                diag_(diag::Code::kTypeFieldMemberMustBePodBuiltin, m.span, m.name, types_.to_string(m.type));
            }
            err_(m.span, oss.str());
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
                    if (m.kind == ast::StmtKind::kFnDecl &&
                        m.proto_fn_role == ast::ProtoFnRole::kRequire) {
                        out.push_back(msid);
                    } else if (m.kind == ast::StmtKind::kAssocTypeDecl &&
                               m.assoc_type_role == ast::AssocTypeRole::kProtoRequire) {
                        out.push_back(msid);
                    }
                }
            }
        };
        auto collect_provided = [&](auto&& self,
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
                    if (m.kind == ast::StmtKind::kFnDecl &&
                        m.proto_fn_role == ast::ProtoFnRole::kProvide &&
                        m.a != ast::k_invalid_stmt) {
                        out.push_back(msid);
                    }
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
                if (is_builtin_family_proto_(*proto_sid)) {
                    diag_(diag::Code::kTypeErrorGeneric, pr.span,
                          "builtin proto '" + proto_path + "' is reserved for primitive family constraints");
                    err_(pr.span, "builtin proto is reserved for primitive family constraints");
                    continue;
                }
                if (!evaluate_proto_require_at_apply_(*proto_sid, self_ty, pr.span,
                                                      /*emit_unsatisfied_diag=*/true,
                                                      /*emit_shape_diag=*/true)) {
                    continue;
                }

                bool proto_impl_ok = true;
                std::vector<ast::StmtId> required;
                std::vector<ast::StmtId> provided;
                std::unordered_set<ast::StmtId> visiting;
                collect_required(collect_required, *proto_sid, required, visiting);
                visiting.clear();
                collect_provided(collect_provided, *proto_sid, provided, visiting);
                for (const ast::StmtId req_sid : required) {
                    if (req_sid == ast::k_invalid_stmt || (size_t)req_sid >= ast_.stmts().size()) continue;
                    const auto& req = ast_.stmt(req_sid);
                    if (req.kind == ast::StmtKind::kAssocTypeDecl) {
                        if (proto_assoc_requirement_satisfied_by_default_acts_(req, self_ty)) {
                            continue;
                        }
                        diag_(diag::Code::kProtoImplMissingMember, req.span, req.name);
                        err_(req.span, "field does not bind proto associated type: " + std::string(req.name));
                        proto_impl_ok = false;
                        continue;
                    }
                    bool satisfied_by_provide = false;
                    for (const ast::StmtId prov_sid : provided) {
                        if (prov_sid == ast::k_invalid_stmt || (size_t)prov_sid >= ast_.stmts().size()) continue;
                        const auto& prov = ast_.stmt(prov_sid);
                        if (req.name == prov.name && req.param_count == prov.param_count &&
                            req.positional_param_count == prov.positional_param_count &&
                            req.fn_ret == prov.fn_ret) {
                            bool same_params = true;
                            for (uint32_t pi = 0; pi < req.param_count; ++pi) {
                                const auto& rp = ast_.params()[req.param_begin + pi];
                                const auto& pp = ast_.params()[prov.param_begin + pi];
                                if (rp.type != pp.type || rp.is_self != pp.is_self || rp.self_kind != pp.self_kind) {
                                    same_params = false;
                                    break;
                                }
                            }
                            if (same_params) {
                                satisfied_by_provide = true;
                                break;
                            }
                        }
                    }
                    if (satisfied_by_provide) continue;
                    if (proto_requirement_satisfied_by_default_acts_(req_sid, self_ty)) {
                        continue;
                    }
                    diag_(diag::Code::kProtoImplMissingMember, req.span, req.name);
                    err_(req.span, "field does not provide proto member: " + std::string(req.name));
                    proto_impl_ok = false;
                }
                if (proto_impl_ok && self_ty != ty::kInvalidType) {
                    auto& impls = explicit_impl_proto_sids_by_type_[self_ty];
                    if (std::find(impls.begin(), impls.end(), *proto_sid) == impls.end()) {
                        impls.push_back(*proto_sid);
                    }
                }
            }
        }
    }

    void TypeChecker::check_stmt_enum_decl_(ast::StmtId sid) {
        const ast::Stmt& s = ast_.stmt(sid);
        {
            std::unordered_set<std::string> generic_params;
            for (const auto& name : collect_decl_generic_param_names_(s)) {
                generic_params.insert(name);
            }
            (void)validate_constraint_clause_decl_(s.decl_constraint_begin, s.decl_constraint_count, generic_params, s.span);
        }
        const bool is_generic_template =
            (s.decl_generic_param_count > 0) &&
            (generic_enum_template_sid_set_.find(sid) != generic_enum_template_sid_set_.end());

        const uint32_t vb = s.enum_variant_begin;
        const uint32_t ve = s.enum_variant_begin + s.enum_variant_count;
        if (vb > ast_.enum_variant_decls().size() || ve > ast_.enum_variant_decls().size() || vb > ve) {
            diag_(diag::Code::kTypeErrorGeneric, s.span, "invalid enum variant range");
            err_(s.span, "invalid enum variant range");
            return;
        }

        ty::TypeId self_ty = s.type;
        if (self_ty == ty::kInvalidType && !s.name.empty()) {
            self_ty = types_.intern_ident(qualify_decl_name_(std::string(s.name)));
        }
        if (self_ty == ty::kInvalidType) {
            diag_(diag::Code::kTypeErrorGeneric, s.span, "enum type id is invalid");
            err_(s.span, "enum type id is invalid");
            return;
        }

        if (is_generic_template) {
            return;
        }

        EnumAbiMeta meta{};
        meta.sid = sid;
        meta.layout = s.field_layout;
        meta.is_layout_c = (s.field_layout == ast::FieldLayout::kC);
        meta.variants.reserve(s.enum_variant_count);

        int64_t next_tag = 0;
        std::unordered_set<std::string> seen_variant_names;
        seen_variant_names.reserve(s.enum_variant_count);

        for (uint32_t i = 0; i < s.enum_variant_count; ++i) {
            const auto& v = ast_.enum_variant_decls()[s.enum_variant_begin + i];
            const std::string vname(v.name);
            if (!seen_variant_names.insert(vname).second) {
                diag_(diag::Code::kEnumVariantDuplicate, v.span, v.name);
                err_(v.span, "duplicate enum variant name: " + vname);
                continue;
            }

            if (s.field_layout == ast::FieldLayout::kC && v.payload_count > 0) {
                diag_(diag::Code::kEnumLayoutCPayloadNotAllowed, v.span);
                err_(v.span, "layout(c) enum variant must not contain payload");
            }
            if (s.field_layout != ast::FieldLayout::kC && v.has_discriminant) {
                diag_(diag::Code::kEnumDiscriminantNonCForbidden, v.span);
                err_(v.span, "enum discriminant assignment is only allowed on layout(c) tag-only enum");
            }

            EnumVariantMeta vm{};
            vm.name = vname;
            vm.index = i;
            vm.has_discriminant = v.has_discriminant;
            vm.tag = v.has_discriminant ? v.discriminant : next_tag;
            next_tag = vm.tag + 1;

            const uint64_t pb = v.payload_begin;
            const uint64_t pe = pb + v.payload_count;
            if (pb > ast_.field_members().size() || pe > ast_.field_members().size() || pb > pe) {
                diag_(diag::Code::kTypeFieldMemberRangeInvalid, v.span);
                err_(v.span, "invalid enum variant payload range");
            } else {
                std::unordered_set<std::string> seen_payload_fields;
                for (uint32_t mi = v.payload_begin; mi < v.payload_begin + v.payload_count; ++mi) {
                    const auto& m = ast_.field_members()[mi];
                    if (!seen_payload_fields.insert(std::string(m.name)).second) {
                        diag_(diag::Code::kDuplicateDecl, m.span, m.name);
                        err_(m.span, "duplicate enum payload field: " + std::string(m.name));
                        continue;
                    }

                    const auto& mt = types_.get(m.type);
                    if (mt.kind == ty::Kind::kBorrow || mt.kind == ty::Kind::kEscape || mt.kind == ty::Kind::kFn) {
                        diag_(diag::Code::kTypeErrorGeneric, m.span,
                              std::string("unsupported enum payload type for field '") + std::string(m.name) + "'");
                        err_(m.span, "unsupported enum payload type");
                    }

                    if (class_decl_by_type_.find(m.type) != class_decl_by_type_.end() ||
                        actor_decl_by_type_.find(m.type) != actor_decl_by_type_.end()) {
                        diag_(diag::Code::kTypeErrorGeneric, m.span,
                              std::string("enum payload field '") + std::string(m.name) +
                              "' cannot use class/actor type in v0");
                        err_(m.span, "enum payload class/actor type is not supported in v0");
                    }

                    EnumVariantFieldMeta fm{};
                    fm.name = std::string(m.name);
                    fm.storage_name = "__v" + std::to_string(vm.index) + "_" + std::string(m.name);
                    fm.type = m.type;
                    fm.span = m.span;
                    vm.field_index_by_name[fm.name] = static_cast<uint32_t>(vm.fields.size());
                    vm.fields.push_back(std::move(fm));
                }
            }

            meta.variant_index_by_name[vm.name] = static_cast<uint32_t>(meta.variants.size());
            meta.variants.push_back(std::move(vm));
        }

        enum_abi_meta_by_type_[self_ty] = std::move(meta);
        enum_decl_by_type_[self_ty] = sid;
        ensure_generic_acts_for_owner_(self_ty, s.span);

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
                    if (m.kind == ast::StmtKind::kFnDecl &&
                        m.proto_fn_role == ast::ProtoFnRole::kRequire) {
                        out.push_back(msid);
                    } else if (m.kind == ast::StmtKind::kAssocTypeDecl &&
                               m.assoc_type_role == ast::AssocTypeRole::kProtoRequire) {
                        out.push_back(msid);
                    }
                }
            }
        };
        auto collect_provided = [&](auto&& self,
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
                    if (m.kind == ast::StmtKind::kFnDecl &&
                        m.proto_fn_role == ast::ProtoFnRole::kProvide &&
                        m.a != ast::k_invalid_stmt) {
                        out.push_back(msid);
                    }
                }
            }
        };

        const auto& refs = ast_.path_refs();
        const uint32_t rb = s.decl_path_ref_begin;
        const uint32_t re = s.decl_path_ref_begin + s.decl_path_ref_count;
        if (rb <= refs.size() && re <= refs.size()) {
            for (uint32_t i = rb; i < re; ++i) {
                bool typed_path_failure = false;
                const auto proto_sid = resolve_proto_decl_from_path_ref_(refs[i], refs[i].span, &typed_path_failure);
                if (!proto_sid.has_value()) {
                    if (!typed_path_failure) {
                        const std::string p = path_ref_display_(refs[i]);
                        diag_(diag::Code::kProtoImplTargetNotSupported, refs[i].span, p);
                        err_(refs[i].span, "unknown proto target: " + p);
                    }
                    continue;
                }
                if (is_builtin_family_proto_(*proto_sid)) {
                    const std::string p = path_ref_display_(refs[i]);
                    diag_(diag::Code::kTypeErrorGeneric, refs[i].span,
                          "builtin proto '" + p + "' is reserved for primitive family constraints");
                    err_(refs[i].span, "builtin proto is reserved for primitive family constraints");
                    continue;
                }
                if (!evaluate_proto_require_at_apply_(*proto_sid, self_ty, refs[i].span,
                                                      /*emit_unsatisfied_diag=*/true,
                                                      /*emit_shape_diag=*/true)) {
                    continue;
                }

                bool proto_impl_ok = true;
                std::vector<ast::StmtId> required;
                std::vector<ast::StmtId> provided;
                std::unordered_set<ast::StmtId> visiting;
                collect_required(collect_required, *proto_sid, required, visiting);
                visiting.clear();
                collect_provided(collect_provided, *proto_sid, provided, visiting);
                for (const ast::StmtId req_sid : required) {
                    if (req_sid == ast::k_invalid_stmt || (size_t)req_sid >= ast_.stmts().size()) continue;
                    const auto& req = ast_.stmt(req_sid);
                    if (req.kind == ast::StmtKind::kAssocTypeDecl) {
                        if (proto_assoc_requirement_satisfied_by_default_acts_(req, self_ty)) {
                            continue;
                        }
                        diag_(diag::Code::kProtoImplMissingMember, req.span, req.name);
                        err_(req.span, "enum does not bind proto associated type: " + std::string(req.name));
                        proto_impl_ok = false;
                        continue;
                    }
                    bool satisfied_by_provide = false;
                    for (const ast::StmtId prov_sid : provided) {
                        if (prov_sid == ast::k_invalid_stmt || (size_t)prov_sid >= ast_.stmts().size()) continue;
                        const auto& prov = ast_.stmt(prov_sid);
                        if (req.name == prov.name && req.param_count == prov.param_count &&
                            req.positional_param_count == prov.positional_param_count &&
                            req.fn_ret == prov.fn_ret) {
                            bool same_params = true;
                            for (uint32_t pi = 0; pi < req.param_count; ++pi) {
                                const auto& rp = ast_.params()[req.param_begin + pi];
                                const auto& pp = ast_.params()[prov.param_begin + pi];
                                if (rp.type != pp.type || rp.is_self != pp.is_self || rp.self_kind != pp.self_kind) {
                                    same_params = false;
                                    break;
                                }
                            }
                            if (same_params) {
                                satisfied_by_provide = true;
                                break;
                            }
                        }
                    }
                    if (satisfied_by_provide) continue;
                    if (proto_requirement_satisfied_by_default_acts_(req_sid, self_ty)) {
                        continue;
                    }
                    diag_(diag::Code::kProtoImplMissingMember, req.span, req.name);
                    err_(req.span, "enum does not provide proto member: " + std::string(req.name));
                    proto_impl_ok = false;
                }
                if (proto_impl_ok && self_ty != ty::kInvalidType) {
                    auto& impls = explicit_impl_proto_sids_by_type_[self_ty];
                    if (std::find(impls.begin(), impls.end(), *proto_sid) == impls.end()) {
                        impls.push_back(*proto_sid);
                    }
                }
            }
        }
    }

    /// @brief acts 선언 내부의 함수 멤버를 타입 체크한다.
    void TypeChecker::check_stmt_acts_decl_(ast::StmtId sid, const ast::Stmt& s) {
        {
            std::unordered_set<std::string> generic_params;
            for (const auto& name : collect_decl_generic_param_names_(s)) {
                generic_params.insert(name);
            }
            if (s.acts_is_for && s.acts_target_type != ty::kInvalidType) {
                std::unordered_set<std::string> owner_generic_params{};
                collect_unresolved_generic_param_names_in_type_(s.acts_target_type, owner_generic_params);
                for (const auto& name : owner_generic_params) {
                    generic_params.insert(name);
                }
            }
            (void)validate_constraint_clause_decl_(s.decl_constraint_begin, s.decl_constraint_count, generic_params, s.span);
        }
        if (sid != ast::k_invalid_stmt &&
            generic_acts_template_sid_set_.find(sid) != generic_acts_template_sid_set_.end()) {
            return;
        }

        sym_.push_scope();

        ty::TypeId owner_type = ty::kInvalidType;
        if (s.acts_is_for) {
            bool owner_ok = false;
            owner_type = canonicalize_acts_owner_type_(s.acts_target_type);
            if (auto inst_sid = ensure_generic_class_instance_from_type_(owner_type, s.span)) {
                const auto& inst = ast_.stmt(*inst_sid);
                if (inst.kind == ast::StmtKind::kClassDecl && inst.type != ty::kInvalidType) {
                    owner_type = inst.type;
                }
            }
            (void)ensure_generic_field_instance_from_type_(owner_type, s.span);
            (void)ensure_generic_enum_instance_from_type_(owner_type, s.span);
            owner_type = canonicalize_acts_owner_type_(owner_type);
            if (owner_type != ty::kInvalidType) {
                ty::Builtin builtin_kind = ty::Builtin::kNull;
                if (is_builtin_owner_type_(owner_type, &builtin_kind)) {
                    owner_ok = true;
                }
                if (class_decl_by_type_.find(owner_type) != class_decl_by_type_.end() ||
                    field_abi_meta_by_type_.find(owner_type) != field_abi_meta_by_type_.end() ||
                    enum_abi_meta_by_type_.find(owner_type) != enum_abi_meta_by_type_.end()) {
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
                oss << "acts-for target must be a field/class/enum/builtin type in v0, got "
                    << types_.to_string(owner_type);
                diag_(diag::Code::kTypeErrorGeneric, s.span, oss.str());
                err_(s.span, oss.str());
            }
        }

        const auto& kids = ast_.stmt_children();
        const uint32_t begin = s.stmt_begin;
        const uint32_t end = s.stmt_begin + s.stmt_count;

        if (s.acts_assoc_witness_count > 0) {
            std::unordered_set<std::string> seen_assoc_type_names{};
            std::unordered_set<std::string> required_assoc_type_names{};
            auto resolve_owner_decl_sid = [&]() -> ast::StmtId {
                if (owner_type != ty::kInvalidType) {
                    if (auto it = field_abi_meta_by_type_.find(owner_type); it != field_abi_meta_by_type_.end()) {
                        return it->second.sid;
                    }
                    if (auto it = class_decl_by_type_.find(owner_type); it != class_decl_by_type_.end()) {
                        return it->second;
                    }
                    if (auto it = enum_abi_meta_by_type_.find(owner_type); it != enum_abi_meta_by_type_.end()) {
                        return it->second.sid;
                    }
                    if (auto it = enum_decl_by_type_.find(owner_type); it != enum_decl_by_type_.end()) {
                        return it->second;
                    }
                }

                if (s.acts_target_type_node == ast::k_invalid_type_node ||
                    static_cast<size_t>(s.acts_target_type_node) >= ast_.type_nodes().size()) {
                    return ast::k_invalid_stmt;
                }

                const auto& tn = ast_.type_node(s.acts_target_type_node);
                if (tn.kind != ast::TypeNodeKind::kNamedPath || tn.path_count == 0) {
                    return ast::k_invalid_stmt;
                }

                std::string owner_key = path_join_(tn.path_begin, tn.path_count);
                const size_t owner_split = owner_key.rfind("::");
                const std::string owner_leaf =
                    (owner_split == std::string::npos) ? owner_key : owner_key.substr(owner_split + 2);
                const bool owner_had_alias = rewrite_imported_path_(owner_key).has_value();
                const bool rewritten_lookup = apply_imported_path_rewrite_(owner_key);
                if (!owner_had_alias && qualified_path_requires_import_(owner_key)) {
                    return ast::k_invalid_stmt;
                }

                auto owner_sym = rewritten_lookup ? sym_.lookup(owner_key) : lookup_symbol_(owner_key);
                if (!owner_sym.has_value()) owner_sym = lookup_symbol_(owner_key);
                if (!owner_sym.has_value()) return ast::k_invalid_stmt;

                const auto& ss = sym_.symbol(*owner_sym);
                const ty::TypeId declared_t = canonicalize_acts_owner_type_(
                    canonicalize_transparent_external_typedef_(ss.declared_type));
                if (declared_t != ty::kInvalidType) {
                    if (auto it = field_abi_meta_by_type_.find(declared_t); it != field_abi_meta_by_type_.end()) {
                        return it->second.sid;
                    }
                    if (auto it = class_decl_by_type_.find(declared_t); it != class_decl_by_type_.end()) {
                        return it->second;
                    }
                    if (auto it = enum_abi_meta_by_type_.find(declared_t); it != enum_abi_meta_by_type_.end()) {
                        return it->second.sid;
                    }
                    if (auto it = enum_decl_by_type_.find(declared_t); it != enum_decl_by_type_.end()) {
                        return it->second;
                    }
                }

                for (ast::StmtId candidate_sid = 0;
                     static_cast<size_t>(candidate_sid) < ast_.stmts().size();
                     ++candidate_sid) {
                    const auto& candidate = ast_.stmt(candidate_sid);
                    if (candidate.span.file_id != s.span.file_id) continue;
                    if (candidate.kind != ast::StmtKind::kFieldDecl &&
                        candidate.kind != ast::StmtKind::kClassDecl &&
                        candidate.kind != ast::StmtKind::kEnumDecl) {
                        continue;
                    }
                    if (candidate.name != owner_leaf) continue;
                    return candidate_sid;
                }
                return ast::k_invalid_stmt;
            };
            auto collect_required_assoc_names_from_proto = [&](ast::StmtId proto_sid) {
                if (proto_sid == ast::k_invalid_stmt || static_cast<size_t>(proto_sid) >= ast_.stmts().size()) {
                    return;
                }
                const auto& proto = ast_.stmt(proto_sid);
                if (proto.kind != ast::StmtKind::kProtoDecl) return;
                const auto& proto_kids = ast_.stmt_children();
                const uint64_t pb = proto.stmt_begin;
                const uint64_t pe = pb + proto.stmt_count;
                if (!(pb <= proto_kids.size() && pe <= proto_kids.size())) return;
                for (uint32_t i = 0; i < proto.stmt_count; ++i) {
                    const auto msid = proto_kids[proto.stmt_begin + i];
                    if (msid == ast::k_invalid_stmt ||
                        static_cast<size_t>(msid) >= ast_.stmts().size()) {
                        continue;
                    }
                    const auto& member = ast_.stmt(msid);
                    if (member.kind != ast::StmtKind::kAssocTypeDecl ||
                        member.assoc_type_role != ast::AssocTypeRole::kProtoRequire ||
                        member.name.empty()) {
                        continue;
                    }
                    required_assoc_type_names.insert(std::string(member.name));
                }
            };
            if (owner_type != ty::kInvalidType) {
                if (auto it = explicit_impl_proto_sids_by_type_.find(owner_type);
                    it != explicit_impl_proto_sids_by_type_.end()) {
                    for (const auto proto_sid : it->second) {
                        collect_required_assoc_names_from_proto(proto_sid);
                    }
                }
                if (required_assoc_type_names.empty()) {
                    const ast::StmtId owner_decl_sid = resolve_owner_decl_sid();
                    if (owner_decl_sid != ast::k_invalid_stmt &&
                        static_cast<size_t>(owner_decl_sid) < ast_.stmts().size()) {
                        const auto& owner_decl = ast_.stmt(owner_decl_sid);
                        const auto& refs = ast_.path_refs();
                        const uint64_t pb = owner_decl.decl_path_ref_begin;
                        const uint64_t pe = pb + owner_decl.decl_path_ref_count;
                        if (pb <= refs.size() && pe <= refs.size()) {
                            for (uint32_t i = 0; i < owner_decl.decl_path_ref_count; ++i) {
                                const auto& pr = refs[owner_decl.decl_path_ref_begin + i];
                                if (auto proto_sid = resolve_proto_decl_from_path_ref_(pr, owner_decl.span)) {
                                    collect_required_assoc_names_from_proto(*proto_sid);
                                    continue;
                                }
                                if (auto proto_sid = resolve_proto_sid_for_constraint_(path_ref_display_(pr))) {
                                    collect_required_assoc_names_from_proto(*proto_sid);
                                }
                            }
                        }
                    }
                }
            }
            const auto& witnesses = ast_.acts_assoc_type_witness_decls();
            const uint64_t wbegin = s.acts_assoc_witness_begin;
            const uint64_t wend = wbegin + s.acts_assoc_witness_count;
            if (!(wbegin <= witnesses.size() && wend <= witnesses.size())) {
                diag_(diag::Code::kTypeErrorGeneric, s.span, "invalid acts associated witness slice");
                err_(s.span, "invalid acts associated witness slice");
            } else {
                for (uint32_t i = 0; i < s.acts_assoc_witness_count; ++i) {
                    const auto& witness = witnesses[s.acts_assoc_witness_begin + i];
                    if (!s.acts_is_for) {
                        diag_(diag::Code::kTypeErrorGeneric, witness.span,
                              "acts-associated type witnesses are only allowed in acts-for declarations");
                        err_(witness.span, "acts-associated type witnesses are only allowed in acts-for declarations");
                        continue;
                    }
                    if (witness.assoc_name.empty()) {
                        diag_(diag::Code::kTypeErrorGeneric, witness.span,
                              "acts-associated type witness requires an associated type name");
                        err_(witness.span, "acts-associated type witness requires an associated type name");
                        continue;
                    }
                    if (witness.rhs_type == ty::kInvalidType || is_error_(witness.rhs_type)) {
                        diag_(diag::Code::kTypeErrorGeneric, witness.span,
                              "acts-associated type witness requires a valid type");
                        err_(witness.span, "acts-associated type witness requires a valid type");
                        continue;
                    }
                    if (!seen_assoc_type_names.insert(std::string(witness.assoc_name)).second) {
                        diag_(diag::Code::kDuplicateDecl, witness.span, witness.assoc_name);
                        err_(witness.span, "duplicate acts associated type witness");
                        continue;
                    }
                    if (!required_assoc_type_names.empty() &&
                        required_assoc_type_names.find(std::string(witness.assoc_name)) == required_assoc_type_names.end()) {
                        std::ostringstream oss;
                        oss << "unknown acts-associated type witness '" << witness.assoc_name
                            << "' for acts owner " << types_.to_string(owner_type);
                        diag_(diag::Code::kTypeErrorGeneric, witness.span, oss.str());
                        err_(witness.span, "unknown acts-associated type witness");
                    }
                }
            }
        }

        // acts 멤버 함수의 상호 참조를 위해 먼저 시그니처를 predeclare한다.
        if (begin < kids.size() && end <= kids.size()) {
            for (uint32_t i = begin; i < end; ++i) {
                const auto sid = kids[i];
                if (sid == ast::k_invalid_stmt) continue;
                const auto& member = ast_.stmt(sid);
                if (member.kind == ast::StmtKind::kAssocTypeDecl) {
                    diag_(diag::Code::kTypeErrorGeneric, member.span,
                          "acts-associated type witness must be written in the acts header using '<... is Assoc>'");
                    err_(member.span, "legacy acts associated type binding syntax is removed");
                    continue;
                }
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
                            } else if (owner_type != ty::kInvalidType &&
                                       !type_matches_acts_owner_(types_, owner_type, p0.type)) {
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
                        } else if (s.acts_is_for && owner_type != ty::kInvalidType &&
                                   !type_matches_acts_owner_(types_, owner_type, p0.type)) {
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
