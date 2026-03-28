    void TypeChecker::first_pass_collect_top_level_(ast::StmtId program_stmt) {
        const ast::Stmt& prog = ast_.stmt(program_stmt);
        if (prog.kind != ast::StmtKind::kBlock) {
            err_(prog.span, "program root is not a block stmt");
            diag_(diag::Code::kTopLevelMustBeBlock, prog.span);
            return;
        }
        fn_decl_by_name_.clear();
        fn_qualified_name_by_stmt_.clear();
        field_qualified_name_by_stmt_.clear();
        enum_qualified_name_by_stmt_.clear();
        class_qualified_name_by_stmt_.clear();
        acts_qualified_name_by_stmt_.clear();
        class_decl_by_name_.clear();
        class_decl_by_type_.clear();
        class_effective_method_map_.clear();
        class_member_fn_sid_set_.clear();
        class_member_owner_by_stmt_.clear();
        private_class_member_qname_owner_.clear();
        actor_decl_by_name_.clear();
        actor_decl_by_type_.clear();
        actor_method_map_.clear();
        actor_member_fn_sid_set_.clear();
        proto_member_fn_sid_set_.clear();
        import_alias_to_path_.clear();
        acts_named_decl_by_owner_and_name_.clear();
        acts_default_decls_by_owner_.clear();
        acts_default_assoc_type_map_.clear();
        explicit_impl_proto_sids_by_type_.clear();
        collect_core_impl_marker_file_ids_(program_stmt);

        if (!core_impl_marker_file_ids_.empty()) {
            const auto& kids = ast_.stmt_children();
            const uint64_t begin = prog.stmt_begin;
            const uint64_t end = begin + prog.stmt_count;
            if (begin <= kids.size() && end <= kids.size()) {
                for (uint32_t i = 0; i < prog.stmt_count; ++i) {
                    const auto sid = kids[prog.stmt_begin + i];
                    if (sid == ast::k_invalid_stmt || static_cast<size_t>(sid) >= ast_.stmts().size()) continue;
                    const auto& s = ast_.stmt(sid);
                    if (!is_core_impl_marker_stmt_(s)) continue;
                    const std::string file_bundle = bundle_name_for_file_(s.span.file_id);
                    if (file_bundle == "core") continue;

                    std::ostringstream oss;
                    oss << "$![Impl::Core]; is allowed only when bundle-name is 'core' (current bundle: '"
                        << (file_bundle.empty() ? "<unknown>" : file_bundle)
                        << "'); core file requires bundle context (use parus check sysroot/core/config.lei)";
                    const std::string msg = oss.str();
                    diag_(diag::Code::kTypeErrorGeneric, s.span, msg);
                    err_(s.span, msg);
                    core_context_invalid_ = true;
                    return;
                }
            }
        }

        auto build_fn_sig = [&](const ast::Stmt& s) -> ty::TypeId {
            ty::TypeId sig = s.type;
            if (sig != ty::kInvalidType && types_.get(sig).kind == ty::Kind::kFn) {
                return sig;
            }

            ty::TypeId ret = ty::kInvalidType;
            if (sig != ty::kInvalidType && types_.get(sig).kind != ty::Kind::kFn) {
                ret = sig;
            }
            if (ret == ty::kInvalidType) ret = types_.error();

            std::vector<ty::TypeId> params;
            std::vector<std::string_view> labels;
            std::vector<uint8_t> has_default_flags;
            params.reserve(s.param_count);
            labels.reserve(s.param_count);
            has_default_flags.reserve(s.param_count);

            for (uint32_t pi = 0; pi < s.param_count; ++pi) {
                const auto& p = ast_.params()[s.param_begin + pi];
                ty::TypeId pt = p.type;
                if (pt == ty::kInvalidType) {
                    err_(p.span, "parameter requires an explicit type");
                    diag_(diag::Code::kTypeParamTypeRequired, p.span, p.name);
                    pt = types_.error();
                }
                params.push_back(pt);
                labels.push_back(p.name);
                has_default_flags.push_back(p.has_default ? 1u : 0u);
            }

            return types_.make_fn(
                ret,
                params.empty() ? nullptr : params.data(),
                (uint32_t)params.size(),
                s.positional_param_count,
                labels.empty() ? nullptr : labels.data(),
                has_default_flags.empty() ? nullptr : has_default_flags.data(),
                s.link_abi == ast::LinkAbi::kC,
                s.fn_is_c_variadic,
                ty::CCallConv::kDefault
            );
        };

        auto core_builtin_use_type = [&](std::string_view name,
                                         bool& out_is_transparent,
                                         ty::TypeId& out_ty) -> bool {
            out_is_transparent = true;
            out_ty = ty::kInvalidType;
            using B = ty::Builtin;
            if (name == "c_void")      { out_ty = types_.builtin(B::kCVoid); return true; }
            if (name == "c_char")      { out_ty = types_.builtin(B::kCChar); return true; }
            if (name == "c_schar")     { out_ty = types_.builtin(B::kCSChar); return true; }
            if (name == "c_uchar")     { out_ty = types_.builtin(B::kCUChar); return true; }
            if (name == "c_short")     { out_ty = types_.builtin(B::kCShort); return true; }
            if (name == "c_ushort")    { out_ty = types_.builtin(B::kCUShort); return true; }
            if (name == "c_int")       { out_ty = types_.builtin(B::kCInt); return true; }
            if (name == "c_uint")      { out_ty = types_.builtin(B::kCUInt); return true; }
            if (name == "c_long")      { out_ty = types_.builtin(B::kCLong); return true; }
            if (name == "c_ulong")     { out_ty = types_.builtin(B::kCULong); return true; }
            if (name == "c_longlong")  { out_ty = types_.builtin(B::kCLongLong); return true; }
            if (name == "c_ulonglong") { out_ty = types_.builtin(B::kCULongLong); return true; }
            if (name == "c_float")     { out_ty = types_.builtin(B::kCFloat); return true; }
            if (name == "c_double")    { out_ty = types_.builtin(B::kCDouble); return true; }
            if (name == "c_size")      { out_ty = types_.builtin(B::kCSize); return true; }
            if (name == "c_ssize")     { out_ty = types_.builtin(B::kCSSize); return true; }
            if (name == "c_ptrdiff")   { out_ty = types_.builtin(B::kCPtrDiff); return true; }
            if (name == "vaList") {
                out_is_transparent = false;
                out_ty = types_.builtin(B::kVaList);
                return true;
            }
            return false;
        };
        auto core_builtin_use_proto = [&](std::string_view name) -> bool {
            return name == "Comparable" ||
                   name == "BinaryInteger" ||
                   name == "SignedInteger" ||
                   name == "UnsignedInteger" ||
                   name == "BinaryFloatingPoint" ||
                   name == "Step";
        };
        auto module_head_for_file = [&](uint32_t file_id) -> std::string {
            if (file_id == 0) return {};
            for (const auto& sym : sym_.symbols()) {
                if (sym.is_external) continue;
                if (sym.decl_file_id != file_id) continue;
                if (!sym.decl_module_head.empty()) return sym.decl_module_head;
            }
            return {};
        };

        std::unordered_map<std::string, ast::StmtId> c_abi_symbol_owner;
        const auto make_c_abi_decl_payload = [](const ast::Stmt& s) {
            std::string payload = "parus_c_abi_decl|is_c_abi=1|callconv=default";
            payload += s.fn_is_c_variadic ? "|variadic=1" : "|variadic=0";
            return payload;
        };

        auto collect_stmt = [&](auto&& self, ast::StmtId sid) -> void {
            if (sid == ast::k_invalid_stmt || (size_t)sid >= ast_.stmts().size()) return;
            const ast::Stmt& s = ast_.stmt(sid);
            if (s.span.file_id != 0 &&
                explicit_file_bundle_overrides_.find(s.span.file_id) != explicit_file_bundle_overrides_.end()) {
                return;
            }
            std::string stmt_impl_key{};
            ImplBindingKind stmt_impl_binding = ImplBindingKind::kNone;
            const bool has_any_impl_binding = stmt_impl_binding_key_(s, stmt_impl_key);
            if (stmt_impl_binding_kind_(s, stmt_impl_binding) && s.kind != ast::StmtKind::kFnDecl) {
                const std::string msg = "recognized $![Impl::*] binding is only allowed on function declarations";
                diag_(diag::Code::kTypeErrorGeneric, s.span, msg);
                err_(s.span, msg);
            } else if (has_any_impl_binding && s.kind != ast::StmtKind::kFnDecl) {
                const std::string msg = "attached $![Impl::*] binding is only allowed on function declarations";
                diag_(diag::Code::kTypeErrorGeneric, s.span, msg);
                err_(s.span, msg);
            }

            if (s.kind == ast::StmtKind::kNestDecl) {
                if (s.nest_is_file_directive) return;

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

                if (s.a != ast::k_invalid_stmt && (size_t)s.a < ast_.stmts().size()) {
                    const auto& body = ast_.stmt(s.a);
                    if (body.kind == ast::StmtKind::kBlock) {
                        const auto& kids = ast_.stmt_children();
                        const uint64_t bb = body.stmt_begin;
                        const uint64_t be = bb + body.stmt_count;
                        if (bb <= kids.size() && be <= kids.size()) {
                            for (uint32_t i = 0; i < body.stmt_count; ++i) {
                                self(self, kids[body.stmt_begin + i]);
                            }
                        }
                    }
                }

                while (pushed > 0) {
                    namespace_stack_.pop_back();
                    --pushed;
                }
                return;
            }

            if (s.kind == ast::StmtKind::kUse &&
                (s.use_kind == ast::UseKind::kImport ||
                 s.use_kind == ast::UseKind::kImportCHeader ||
                 s.use_kind == ast::UseKind::kPathAlias ||
                 s.use_kind == ast::UseKind::kNestAlias)) {
                // v0: alias는 second-pass lexical 처리만 사용한다.
                // first-pass에서는 별칭을 전역 pre-collect하지 않는다.
                return;
            }

            if (s.kind == ast::StmtKind::kUse &&
                s.use_kind == ast::UseKind::kCoreBuiltinUse) {
                const std::string qname = qualify_decl_name_(s.use_name);
                const std::string file_bundle = bundle_name_for_file_(s.span.file_id);
                if (file_bundle != "core") {
                    const std::string msg = "core builtin use is allowed only in bundle-name 'core'";
                    diag_(diag::Code::kTypeErrorGeneric, s.span, msg);
                    err_(s.span, msg);
                    return;
                }
                if (core_impl_marker_file_ids_.find(s.span.file_id) == core_impl_marker_file_ids_.end()) {
                    const std::string msg = "core builtin use requires $![Impl::Core]; in the same file";
                    diag_(diag::Code::kTypeErrorGeneric, s.span, msg);
                    err_(s.span, msg);
                    return;
                }
                bool is_transparent = false;
                ty::TypeId builtin_ty = ty::kInvalidType;
                const bool is_type_target =
                    core_builtin_use_type(s.use_name, is_transparent, builtin_ty) &&
                    builtin_ty != ty::kInvalidType;
                const bool is_proto_target = core_builtin_use_proto(s.use_name);
                if (!is_type_target && !is_proto_target) {
                    const std::string msg = "unknown core builtin use target: " + std::string(s.use_name);
                    diag_(diag::Code::kTypeErrorGeneric, s.span, msg);
                    err_(s.span, msg);
                    return;
                }

                const std::string module_head = module_head_for_file(s.span.file_id);
                if (!module_head.empty()) {
                    const bool is_ext_module =
                        module_head == "ext" ||
                        module_head.ends_with("::ext");
                    const bool is_constraints_module =
                        module_head == "constraints" ||
                        module_head.ends_with("::constraints");
                    if ((is_type_target && !is_ext_module) ||
                        (is_proto_target && !is_constraints_module)) {
                        const std::string msg =
                            "core builtin use is allowed only in module 'ext' or 'constraints' (got: " + qname + ")";
                        diag_(diag::Code::kTypeErrorGeneric, s.span, msg);
                        err_(s.span, msg);
                        return;
                    }
                }

                if (is_type_target) {
                    (void)is_transparent;

                    if (auto existing = sym_.lookup_in_current(qname)) {
                        const auto& existing_sym = sym_.symbol(*existing);
                        if (existing_sym.kind != sema::SymbolKind::kType) {
                            err_(s.span, "duplicate symbol (type): " + qname);
                            diag_(diag::Code::kDuplicateDecl, s.span, qname);
                        } else {
                            (void)sym_.update_declared_type(*existing, builtin_ty);
                        }
                    } else {
                        auto ins = sym_.insert(sema::SymbolKind::kType, qname, builtin_ty, s.span);
                        if (!ins.ok && ins.is_duplicate) {
                            err_(s.span, "duplicate symbol (type): " + qname);
                            diag_(diag::Code::kDuplicateDecl, s.span, qname);
                        }
                    }
                    return;
                }

                if (!core_builtin_use_proto(s.use_name)) {
                    const std::string msg = "unknown core builtin proto target: " + std::string(s.use_name);
                    diag_(diag::Code::kTypeErrorGeneric, s.span, msg);
                    err_(s.span, msg);
                    return;
                }

                ty::TypeId proto_ty = types_.intern_ident(qname);
                std::optional<ast::StmtId> proto_sid{};
                if (auto it = proto_decl_by_name_.find(qname); it != proto_decl_by_name_.end()) {
                    proto_sid = it->second;
                } else {
                    ast::Stmt stub{};
                    stub.kind = ast::StmtKind::kProtoDecl;
                    stub.span = s.span;
                    stub.name = s.use_name;
                    stub.type = proto_ty;
                    stub.is_export = s.is_export;
                    const auto stub_sid = ast_.add_stmt(stub);
                    proto_decl_by_name_[qname] = stub_sid;
                    proto_qualified_name_by_stmt_[stub_sid] = "core::constraints::" + std::string(s.use_name);
                    proto_decl_by_type_[proto_ty] = stub_sid;
                    proto_sid = stub_sid;
                }

                if (proto_sid.has_value()) {
                    auto& proto_stmt = ast_.stmt_mut(*proto_sid);
                    if (proto_stmt.type == ty::kInvalidType) {
                        proto_stmt.type = proto_ty;
                    }
                    proto_decl_by_type_[proto_stmt.type] = *proto_sid;
                }

                if (auto existing = sym_.lookup_in_current(qname)) {
                    const auto& existing_sym = sym_.symbol(*existing);
                    if (existing_sym.kind != sema::SymbolKind::kType) {
                        err_(s.span, "duplicate symbol (proto): " + qname);
                        diag_(diag::Code::kDuplicateDecl, s.span, qname);
                    } else {
                        (void)sym_.update_declared_type(*existing, proto_ty);
                    }
                } else {
                    auto ins = sym_.insert(sema::SymbolKind::kType, qname, proto_ty, s.span);
                    if (!ins.ok && ins.is_duplicate) {
                        err_(s.span, "duplicate symbol (proto): " + qname);
                        diag_(diag::Code::kDuplicateDecl, s.span, qname);
                    }
                }
                return;
            }

            if (s.kind == ast::StmtKind::kFnDecl) {
                if (s.fn_generic_param_count > 0) {
                    generic_fn_template_sid_set_.insert(sid);
                }
                const std::string qname = qualify_decl_name_(s.name);
                fn_qualified_name_by_stmt_[sid] = qname;
                fn_decl_by_name_[qname].push_back(sid);

                const ty::TypeId sig = build_fn_sig(s);
                ImplBindingKind impl_binding = ImplBindingKind::kNone;
                (void)stmt_impl_binding_kind_(s, impl_binding);
                std::string impl_key{};
                const bool has_impl_binding = stmt_impl_binding_key_(s, impl_key);
                const bool compiler_owned_impl =
                    has_impl_binding &&
                    impl_binding != ImplBindingKind::kNone &&
                    s.a == ast::k_invalid_stmt;
                if (compiler_owned_impl) {
                    const std::string file_bundle = bundle_name_for_file_(s.span.file_id);
                    if (file_bundle != "core" ||
                        core_impl_marker_file_ids_.find(s.span.file_id) == core_impl_marker_file_ids_.end()) {
                        const std::string msg =
                            "bodyless recognized $![Impl::*] binding requires bundle 'core' and file marker '$![Impl::Core];'";
                        diag_(diag::Code::kTypeErrorGeneric, s.span, msg);
                        err_(s.span, msg);
                    }
                }
                const std::string impl_payload =
                    has_impl_binding ? make_impl_binding_payload_(impl_key, compiler_owned_impl) : std::string{};

                if (auto existing = sym_.lookup_in_current(qname)) {
                    const auto& existing_sym = sym_.symbol(*existing);
                    if (existing_sym.kind != sema::SymbolKind::kFn) {
                        err_(s.span, "duplicate symbol (function): " + qname);
                        diag_(diag::Code::kDuplicateDecl, s.span, qname);
                    }
                } else {
                    auto ins = sym_.insert(sema::SymbolKind::kFn, qname, sig, s.span,
                                           /*decl_file_id=*/0,
                                           /*decl_bundle_name=*/{},
                                           /*is_export=*/false,
                                           /*is_external=*/false,
                                           /*decl_module_head=*/{},
                                           /*decl_source_dir_norm=*/{},
                                           /*link_name=*/{},
                                           impl_payload);
                    if (!ins.ok && ins.is_duplicate) {
                        err_(s.span, "duplicate symbol (function): " + qname);
                        diag_(diag::Code::kDuplicateDecl, s.span, qname);
                    } else if (ins.ok && s.link_abi == ast::LinkAbi::kC) {
                        auto& sym = sym_.symbol_mut(ins.symbol_id);
                        sym.external_payload = make_c_abi_decl_payload(s);
                    } else if (ins.ok && !impl_payload.empty()) {
                        auto& sym = sym_.symbol_mut(ins.symbol_id);
                        sym.external_payload = impl_payload;
                    }
                }

                if (s.link_abi == ast::LinkAbi::kC) {
                    const std::string c_sym = std::string(s.name);
                    auto cins = c_abi_symbol_owner.emplace(c_sym, sid);
                    if (!cins.second && cins.first->second != sid) {
                        diag_(diag::Code::kDuplicateDecl, s.span, c_sym);
                        err_(s.span, "duplicate C ABI symbol: " + c_sym);
                    }

                    if (s.has_named_group || s.positional_param_count != s.param_count) {
                        diag_(diag::Code::kAbiCNamedGroupNotAllowed, s.span, s.name);
                        err_(s.span, "C ABI function must not use named-group parameters: " + std::string(s.name));
                    }

                    ty::TypeId ret_ty = s.fn_ret;
                    if (ret_ty == ty::kInvalidType && sig != ty::kInvalidType && types_.get(sig).kind == ty::Kind::kFn) {
                        ret_ty = types_.get(sig).ret;
                    }
                    (void)ensure_generic_field_instance_from_type_(ret_ty, s.span);
                    if (!is_c_abi_safe_type_(ret_ty, /*allow_void=*/true)) {
                        const ty::TypeId canon = canonicalize_transparent_external_typedef_(ret_ty);
                        const bool is_text =
                            canon != ty::kInvalidType &&
                            types_.get(canon).kind == ty::Kind::kBuiltin &&
                            types_.get(canon).builtin == ty::Builtin::kText;
                        diag_(diag::Code::kAbiCTypeNotFfiSafe, s.span, std::string("return type of '") + std::string(s.name) + "'", types_.to_string(ret_ty));
                        if (is_text) {
                            diag_(diag::Code::kTypeErrorGeneric, s.span,
                                  "text is not C ABI-safe; use *const core::ext::c_char and explicit boundary conversion");
                        }
                        std::string msg = "C ABI return type is not FFI-safe: " + types_.to_string(ret_ty);
                        if (is_text) msg += " (text is not C ABI-safe; use *const core::ext::c_char)";
                        err_(s.span, msg);
                    }

                    for (uint32_t pi = 0; pi < s.param_count; ++pi) {
                        const auto& p = ast_.params()[s.param_begin + pi];
                        (void)ensure_generic_field_instance_from_type_(p.type, p.span);
                        if (!is_c_abi_safe_type_(p.type, /*allow_void=*/false)) {
                            const ty::TypeId canon = canonicalize_transparent_external_typedef_(p.type);
                            const bool is_text =
                                canon != ty::kInvalidType &&
                                types_.get(canon).kind == ty::Kind::kBuiltin &&
                                types_.get(canon).builtin == ty::Builtin::kText;
                            diag_(diag::Code::kAbiCTypeNotFfiSafe, p.span, std::string("parameter '") + std::string(p.name) + "'", types_.to_string(p.type));
                            if (is_text) {
                                diag_(diag::Code::kTypeErrorGeneric, p.span,
                                      "text is not C ABI-safe; use *const core::ext::c_char and explicit boundary conversion");
                            }
                            std::string msg = "C ABI parameter type is not FFI-safe: " + std::string(p.name);
                            if (is_text) msg += " (text is not C ABI-safe; use *const core::ext::c_char)";
                            err_(p.span, msg);
                        }
                    }
                }
                return;
            }

            if (s.kind == ast::StmtKind::kVar) {
                const bool is_global_decl =
                    s.is_static || s.is_const || s.is_extern || s.is_export || (s.link_abi == ast::LinkAbi::kC);
                if (is_global_decl) {
                    const std::string qname = qualify_decl_name_(s.name);
                    ty::TypeId decl_ty = s.type;
                    if (decl_ty == ty::kInvalidType) {
                        decl_ty = types_.error();
                    }
                    uint32_t var_sym = sema::SymbolTable::kNoScope;

                    if (auto existing = sym_.lookup_in_current(qname)) {
                        const auto& existing_sym = sym_.symbol(*existing);
                        if (existing_sym.kind != sema::SymbolKind::kVar) {
                            err_(s.span, "duplicate symbol (global var): " + qname);
                            diag_(diag::Code::kDuplicateDecl, s.span, qname);
                        } else {
                            var_sym = *existing;
                            (void)sym_.update_declared_type(*existing, decl_ty);
                        }
                    } else {
                        auto ins = sym_.insert(sema::SymbolKind::kVar, qname, decl_ty, s.span);
                        if (!ins.ok && ins.is_duplicate) {
                            err_(s.span, "duplicate symbol (global var): " + qname);
                            diag_(diag::Code::kDuplicateDecl, s.span, qname);
                        } else if (ins.ok) {
                            var_sym = ins.symbol_id;
                        }
                    }

                    if (s.is_const && var_sym != sema::SymbolTable::kNoScope) {
                        const_symbol_decl_sid_[var_sym] = sid;
                    }
                }

                if (s.link_abi == ast::LinkAbi::kC && !s.is_static) {
                    diag_(diag::Code::kAbiCGlobalMustBeStatic, s.span, s.name);
                    err_(s.span, "C ABI global must be static: " + std::string(s.name));
                }
                return;
            }

            if (s.kind == ast::StmtKind::kFieldDecl) {
                if (s.decl_generic_param_count > 0) {
                    generic_field_template_sid_set_.insert(sid);
                }
                const std::string qname = qualify_decl_name_(s.name);
                field_qualified_name_by_stmt_[sid] = qname;
                ty::TypeId field_ty = s.type;
                if (field_ty == ty::kInvalidType && !qname.empty()) {
                    field_ty = types_.intern_ident(qname);
                    ast_.stmt_mut(sid).type = field_ty;
                }

                if (auto existing = sym_.lookup_in_current(qname)) {
                    const auto& existing_sym = sym_.symbol(*existing);
                    if (existing_sym.kind != sema::SymbolKind::kField) {
                        err_(s.span, "duplicate symbol (field): " + qname);
                        diag_(diag::Code::kDuplicateDecl, s.span, qname);
                    } else if (field_ty != ty::kInvalidType) {
                        (void)sym_.update_declared_type(*existing, field_ty);
                    }
                } else {
                    auto ins = sym_.insert(sema::SymbolKind::kField, qname, field_ty, s.span);
                    if (!ins.ok && ins.is_duplicate) {
                        err_(s.span, "duplicate symbol (field): " + qname);
                        diag_(diag::Code::kDuplicateDecl, s.span, qname);
                    }
                }

                if (field_ty != ty::kInvalidType) {
                    FieldAbiMeta meta{};
                    meta.sid = sid;
                    meta.layout = s.field_layout;
                    meta.align = s.field_align;
                    field_abi_meta_by_type_[field_ty] = meta;
                }
                return;
            }

            if (s.kind == ast::StmtKind::kEnumDecl) {
                if (s.decl_generic_param_count > 0) {
                    generic_enum_template_sid_set_.insert(sid);
                }

                const std::string qname = qualify_decl_name_(s.name);
                enum_qualified_name_by_stmt_[sid] = qname;
                ty::TypeId enum_ty = s.type;
                if (enum_ty == ty::kInvalidType && !qname.empty()) {
                    enum_ty = types_.intern_ident(qname);
                    ast_.stmt_mut(sid).type = enum_ty;
                }

                enum_decl_by_name_[qname] = sid;
                if (enum_ty != ty::kInvalidType) {
                    enum_decl_by_type_[enum_ty] = sid;
                }

                if (auto existing = sym_.lookup_in_current(qname)) {
                    const auto& existing_sym = sym_.symbol(*existing);
                    if (existing_sym.kind != sema::SymbolKind::kType) {
                        err_(s.span, "duplicate symbol (enum): " + qname);
                        diag_(diag::Code::kDuplicateDecl, s.span, qname);
                    } else if (enum_ty != ty::kInvalidType) {
                        (void)sym_.update_declared_type(*existing, enum_ty);
                    }
                } else {
                    auto ins = sym_.insert(sema::SymbolKind::kType, qname, enum_ty, s.span);
                    if (!ins.ok && ins.is_duplicate) {
                        err_(s.span, "duplicate symbol (enum): " + qname);
                        diag_(diag::Code::kDuplicateDecl, s.span, qname);
                    }
                }
                return;
            }

            if (s.kind == ast::StmtKind::kProtoDecl) {
                const std::string qname = qualify_decl_name_(s.name);
                if (s.decl_generic_param_count > 0) {
                    generic_proto_template_sid_set_.insert(sid);
                }
                proto_qualified_name_by_stmt_[sid] = qname;
                proto_decl_by_name_[qname] = sid;

                ty::TypeId proto_ty = s.type;
                if (proto_ty == ty::kInvalidType && !qname.empty()) {
                    proto_ty = types_.intern_ident(qname);
                    ast_.stmt_mut(sid).type = proto_ty;
                }
                if (proto_ty != ty::kInvalidType) {
                    proto_decl_by_type_[proto_ty] = sid;
                }

                if (auto existing = sym_.lookup_in_current(qname)) {
                    const auto& existing_sym = sym_.symbol(*existing);
                    if (existing_sym.kind != sema::SymbolKind::kType) {
                        err_(s.span, "duplicate symbol (proto): " + qname);
                        diag_(diag::Code::kDuplicateDecl, s.span, qname);
                    } else if (proto_ty != ty::kInvalidType) {
                        (void)sym_.update_declared_type(*existing, proto_ty);
                    }
                } else {
                    auto ins = sym_.insert(sema::SymbolKind::kType, qname, proto_ty, s.span);
                    if (!ins.ok && ins.is_duplicate) {
                        err_(s.span, "duplicate symbol (proto): " + qname);
                        diag_(diag::Code::kDuplicateDecl, s.span, qname);
                    }
                }

                const auto& kids = ast_.stmt_children();
                const uint64_t begin = s.stmt_begin;
                const uint64_t end = begin + s.stmt_count;
                if (begin <= kids.size() && end <= kids.size()) {
                    for (uint32_t i = 0; i < s.stmt_count; ++i) {
                        const ast::StmtId msid = kids[s.stmt_begin + i];
                        if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                        const auto& ms = ast_.stmt(msid);
                        if (ms.kind == ast::StmtKind::kFnDecl) {
                            if (ms.fn_generic_param_count > 0) {
                                generic_fn_template_sid_set_.insert(msid);
                            }
                            proto_member_fn_sid_set_.insert(msid);

                            std::string mqname = qname;
                            if (!mqname.empty()) mqname += "::";
                            mqname += std::string(ms.name);
                            fn_qualified_name_by_stmt_[msid] = std::move(mqname);

                            if (ms.a != ast::k_invalid_stmt) {
                                const std::string qfn = fn_qualified_name_by_stmt_[msid];
                                if (auto existing = sym_.lookup_in_current(qfn)) {
                                    const auto& existing_sym = sym_.symbol(*existing);
                                    if (existing_sym.kind != sema::SymbolKind::kFn) {
                                        err_(ms.span, "duplicate symbol (proto default function): " + qfn);
                                        diag_(diag::Code::kDuplicateDecl, ms.span, qfn);
                                    } else {
                                        (void)sym_.update_declared_type(*existing, ms.type);
                                        fn_decl_by_name_[qfn].push_back(msid);
                                    }
                                } else {
                                    auto fins = sym_.insert(sema::SymbolKind::kFn, qfn, ms.type, ms.span);
                                    if (!fins.ok && fins.is_duplicate) {
                                        err_(ms.span, "duplicate symbol (proto default function): " + qfn);
                                        diag_(diag::Code::kDuplicateDecl, ms.span, qfn);
                                    } else {
                                        fn_decl_by_name_[qfn].push_back(msid);
                                    }
                                }
                            }
                            continue;
                        }

                        if (ms.kind == ast::StmtKind::kVar && ms.var_is_proto_provide && ms.is_const) {
                            std::string qvar = qname;
                            if (!qvar.empty()) qvar += "::";
                            qvar += std::string(ms.name);

                            const ty::TypeId vt = (ms.type == ty::kInvalidType) ? types_.error() : ms.type;
                            uint32_t var_sym = sema::SymbolTable::kNoScope;
                            if (auto existing = sym_.lookup_in_current(qvar)) {
                                const auto& existing_sym = sym_.symbol(*existing);
                                if (existing_sym.kind != sema::SymbolKind::kVar) {
                                    err_(ms.span, "duplicate symbol (proto provide const): " + qvar);
                                    diag_(diag::Code::kDuplicateDecl, ms.span, qvar);
                                } else {
                                    var_sym = *existing;
                                    (void)sym_.update_declared_type(*existing, vt);
                                }
                            } else {
                                auto vins = sym_.insert(sema::SymbolKind::kVar, qvar, vt, ms.span);
                                if (!vins.ok && vins.is_duplicate) {
                                    err_(ms.span, "duplicate symbol (proto provide const): " + qvar);
                                    diag_(diag::Code::kDuplicateDecl, ms.span, qvar);
                                } else if (vins.ok) {
                                    var_sym = vins.symbol_id;
                                }
                            }
                            if (var_sym != sema::SymbolTable::kNoScope) {
                                const_symbol_decl_sid_[var_sym] = msid;
                            }
                        }
                    }
                }
                return;
            }

            if (s.kind == ast::StmtKind::kClassDecl) {
                const std::string qname = qualify_decl_name_(s.name);
                if (s.decl_generic_param_count > 0) {
                    generic_class_template_sid_set_.insert(sid);
                }
                class_qualified_name_by_stmt_[sid] = qname;
                ty::TypeId class_ty = s.type;
                if (class_ty == ty::kInvalidType && !qname.empty()) {
                    class_ty = types_.intern_ident(qname);
                    ast_.stmt_mut(sid).type = class_ty;
                }
                class_decl_by_name_[qname] = sid;
                if (class_ty != ty::kInvalidType) {
                    class_decl_by_type_[class_ty] = sid;

                    FieldAbiMeta meta{};
                    meta.sid = sid;
                    meta.layout = ast::FieldLayout::kNone;
                    meta.align = 0;
                    field_abi_meta_by_type_[class_ty] = meta;
                }

                if (auto existing = sym_.lookup_in_current(qname)) {
                    const auto& existing_sym = sym_.symbol(*existing);
                    if (existing_sym.kind != sema::SymbolKind::kType) {
                        err_(s.span, "duplicate symbol (class): " + qname);
                        diag_(diag::Code::kDuplicateDecl, s.span, qname);
                    } else if (class_ty != ty::kInvalidType) {
                        (void)sym_.update_declared_type(*existing, class_ty);
                    }
                } else {
                    auto ins = sym_.insert(sema::SymbolKind::kType, qname, class_ty, s.span);
                    if (!ins.ok && ins.is_duplicate) {
                        err_(s.span, "duplicate symbol (class): " + qname);
                        diag_(diag::Code::kDuplicateDecl, s.span, qname);
                    }
                }

                auto normalize_self_type = [&](ty::TypeId t) -> ty::TypeId {
                    if (t == ty::kInvalidType || class_ty == ty::kInvalidType) return t;
                    const auto& tt = types_.get(t);
                    if (tt.kind == ty::Kind::kNamedUser && is_self_named_type_(t)) {
                        return class_ty;
                    }
                    if (tt.kind == ty::Kind::kBorrow) {
                        const auto& et = types_.get(tt.elem);
                        if (et.kind == ty::Kind::kNamedUser && is_self_named_type_(tt.elem)) {
                            return types_.make_borrow(class_ty, tt.borrow_is_mut);
                        }
                    }
                    return t;
                };

                const auto& kids = ast_.stmt_children();
                const uint64_t begin = s.stmt_begin;
                const uint64_t end = begin + s.stmt_count;
                if (begin <= kids.size() && end <= kids.size()) {
                    for (uint32_t i = 0; i < s.stmt_count; ++i) {
                        const ast::StmtId msid = kids[s.stmt_begin + i];
                        if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                        const auto& ms = ast_.stmt(msid);
                        class_member_owner_by_stmt_[msid] = sid;
                        if (ms.kind == ast::StmtKind::kVar) {
                            if (!ms.is_static) continue;
                            std::string vqname = qname;
                            if (!vqname.empty()) vqname += "::";
                            vqname += std::string(ms.name);
                            if (ms.member_visibility == ast::FieldMember::Visibility::kPrivate) {
                                private_class_member_qname_owner_[vqname] = sid;
                            }

                            ty::TypeId vt = ms.type;
                            if (vt == ty::kInvalidType) vt = types_.error();
                            uint32_t var_sym = sema::SymbolTable::kNoScope;

                            if (auto existing = sym_.lookup_in_current(vqname)) {
                                const auto& existing_sym = sym_.symbol(*existing);
                                if (existing_sym.kind != sema::SymbolKind::kVar) {
                                    err_(ms.span, "duplicate symbol (class static variable): " + vqname);
                                    diag_(diag::Code::kDuplicateDecl, ms.span, vqname);
                                } else {
                                    var_sym = *existing;
                                    (void)sym_.update_declared_type(*existing, vt);
                                }
                            } else {
                                auto vins = sym_.insert(sema::SymbolKind::kVar, vqname, vt, ms.span);
                                if (!vins.ok && vins.is_duplicate) {
                                    err_(ms.span, "duplicate symbol (class static variable): " + vqname);
                                    diag_(diag::Code::kDuplicateDecl, ms.span, vqname);
                                } else if (vins.ok) {
                                    var_sym = vins.symbol_id;
                                }
                            }
                            if (ms.is_const && var_sym != sema::SymbolTable::kNoScope) {
                                const_symbol_decl_sid_[var_sym] = msid;
                            }
                            continue;
                        }

                        if (ms.kind != ast::StmtKind::kFnDecl) continue;
                        if (ms.fn_generic_param_count > 0) {
                            generic_fn_template_sid_set_.insert(msid);
                        }
                        class_member_fn_sid_set_.insert(msid);

                        for (uint32_t pi = 0; pi < ms.param_count; ++pi) {
                            auto& p = ast_.params_mut()[ms.param_begin + pi];
                            p.type = normalize_self_type(p.type);
                        }

                        auto& mm = ast_.stmt_mut(msid);
                        mm.fn_ret = normalize_self_type(mm.fn_ret);

                        std::vector<ty::TypeId> params;
                        std::vector<std::string_view> labels;
                        std::vector<uint8_t> has_default_flags;
                        params.reserve(mm.param_count);
                        labels.reserve(mm.param_count);
                        has_default_flags.reserve(mm.param_count);
                        for (uint32_t pi = 0; pi < mm.param_count; ++pi) {
                            const auto& p = ast_.params()[mm.param_begin + pi];
                            params.push_back(p.type == ty::kInvalidType ? types_.error() : p.type);
                            labels.push_back(p.name);
                            has_default_flags.push_back(p.has_default ? 1u : 0u);
                        }
                        ty::TypeId ret = mm.fn_ret;
                        if (ret == ty::kInvalidType) ret = types_.builtin(ty::Builtin::kUnit);
                        mm.type = types_.make_fn(
                            ret,
                            params.empty() ? nullptr : params.data(),
                            static_cast<uint32_t>(params.size()),
                            mm.positional_param_count,
                            labels.empty() ? nullptr : labels.data(),
                            has_default_flags.empty() ? nullptr : has_default_flags.data(),
                            mm.link_abi == ast::LinkAbi::kC,
                            mm.fn_is_c_variadic,
                            ty::CCallConv::kDefault
                        );

                        std::string mqname = qname;
                        if (!mqname.empty()) mqname += "::";
                        mqname += std::string(mm.name);
                        fn_qualified_name_by_stmt_[msid] = std::move(mqname);

                        const std::string qfn = fn_qualified_name_by_stmt_[msid];
                        if (mm.member_visibility == ast::FieldMember::Visibility::kPrivate) {
                            private_class_member_qname_owner_[qfn] = sid;
                        }
                        if (auto existing = sym_.lookup_in_current(qfn)) {
                            const auto& existing_sym = sym_.symbol(*existing);
                            if (existing_sym.kind != sema::SymbolKind::kFn) {
                                err_(mm.span, "duplicate symbol (class member function): " + qfn);
                                diag_(diag::Code::kDuplicateDecl, mm.span, qfn);
                            } else {
                                (void)sym_.update_declared_type(*existing, mm.type);
                                fn_decl_by_name_[qfn].push_back(msid);
                            }
                        } else {
                            auto fins = sym_.insert(sema::SymbolKind::kFn, qfn, mm.type, mm.span);
                            if (!fins.ok && fins.is_duplicate) {
                                err_(mm.span, "duplicate symbol (class member function): " + qfn);
                                diag_(diag::Code::kDuplicateDecl, mm.span, qfn);
                            } else {
                                fn_decl_by_name_[qfn].push_back(msid);
                            }
                        }

                        if (class_ty != ty::kInvalidType) {
                            class_effective_method_map_[class_ty][std::string(mm.name)].push_back(msid);
                        }
                    }
                }
                return;
            }

            if (s.kind == ast::StmtKind::kActorDecl) {
                const std::string qname = qualify_decl_name_(s.name);
                ty::TypeId actor_ty = s.type;
                if (actor_ty == ty::kInvalidType && !qname.empty()) {
                    actor_ty = types_.intern_ident(qname);
                    ast_.stmt_mut(sid).type = actor_ty;
                }
                actor_decl_by_name_[qname] = sid;
                if (actor_ty != ty::kInvalidType) {
                    actor_decl_by_type_[actor_ty] = sid;

                    FieldAbiMeta meta{};
                    meta.sid = sid;
                    meta.layout = ast::FieldLayout::kNone;
                    meta.align = 0;
                    field_abi_meta_by_type_[actor_ty] = meta;
                }

                if (auto existing = sym_.lookup_in_current(qname)) {
                    const auto& existing_sym = sym_.symbol(*existing);
                    if (existing_sym.kind != sema::SymbolKind::kType) {
                        err_(s.span, "duplicate symbol (actor): " + qname);
                        diag_(diag::Code::kDuplicateDecl, s.span, qname);
                    } else if (actor_ty != ty::kInvalidType) {
                        (void)sym_.update_declared_type(*existing, actor_ty);
                    }
                } else {
                    auto ins = sym_.insert(sema::SymbolKind::kType, qname, actor_ty, s.span);
                    if (!ins.ok && ins.is_duplicate) {
                        err_(s.span, "duplicate symbol (actor): " + qname);
                        diag_(diag::Code::kDuplicateDecl, s.span, qname);
                    }
                }

                auto normalize_self_type = [&](ty::TypeId t) -> ty::TypeId {
                    if (t == ty::kInvalidType || actor_ty == ty::kInvalidType) return t;
                    const auto& tt = types_.get(t);
                    if (tt.kind == ty::Kind::kNamedUser && is_self_named_type_(t)) {
                        return actor_ty;
                    }
                    if (tt.kind == ty::Kind::kBorrow) {
                        const auto& et = types_.get(tt.elem);
                        if (et.kind == ty::Kind::kNamedUser && is_self_named_type_(tt.elem)) {
                            return types_.make_borrow(actor_ty, tt.borrow_is_mut);
                        }
                    }
                    return t;
                };

                const auto& kids = ast_.stmt_children();
                const uint64_t begin = s.stmt_begin;
                const uint64_t end = begin + s.stmt_count;
                if (begin <= kids.size() && end <= kids.size()) {
                    for (uint32_t i = 0; i < s.stmt_count; ++i) {
                        const ast::StmtId msid = kids[s.stmt_begin + i];
                        if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                        const auto& ms = ast_.stmt(msid);
                        if (ms.kind != ast::StmtKind::kFnDecl) continue;
                        if (ms.fn_generic_param_count > 0) {
                            generic_fn_template_sid_set_.insert(msid);
                        }
                        actor_member_fn_sid_set_.insert(msid);

                        for (uint32_t pi = 0; pi < ms.param_count; ++pi) {
                            auto& p = ast_.params_mut()[ms.param_begin + pi];
                            p.type = normalize_self_type(p.type);
                        }

                        auto& mm = ast_.stmt_mut(msid);
                        mm.fn_ret = normalize_self_type(mm.fn_ret);

                        std::vector<ty::TypeId> params;
                        std::vector<std::string_view> labels;
                        std::vector<uint8_t> has_default_flags;
                        params.reserve(mm.param_count);
                        labels.reserve(mm.param_count);
                        has_default_flags.reserve(mm.param_count);
                        for (uint32_t pi = 0; pi < mm.param_count; ++pi) {
                            const auto& p = ast_.params()[mm.param_begin + pi];
                            params.push_back(p.type == ty::kInvalidType ? types_.error() : p.type);
                            labels.push_back(p.name);
                            has_default_flags.push_back(p.has_default ? 1u : 0u);
                        }
                        ty::TypeId ret = mm.fn_ret;
                        if (ret == ty::kInvalidType) ret = types_.builtin(ty::Builtin::kUnit);
                        mm.type = types_.make_fn(
                            ret,
                            params.empty() ? nullptr : params.data(),
                            static_cast<uint32_t>(params.size()),
                            mm.positional_param_count,
                            labels.empty() ? nullptr : labels.data(),
                            has_default_flags.empty() ? nullptr : has_default_flags.data(),
                            mm.link_abi == ast::LinkAbi::kC,
                            mm.fn_is_c_variadic,
                            ty::CCallConv::kDefault
                        );

                        std::string mqname = qname;
                        if (!mqname.empty()) mqname += "::";
                        mqname += std::string(mm.name);
                        fn_qualified_name_by_stmt_[msid] = std::move(mqname);

                        const std::string qfn = fn_qualified_name_by_stmt_[msid];
                        if (auto existing = sym_.lookup_in_current(qfn)) {
                            const auto& existing_sym = sym_.symbol(*existing);
                            if (existing_sym.kind != sema::SymbolKind::kFn) {
                                err_(mm.span, "duplicate symbol (actor member function): " + qfn);
                                diag_(diag::Code::kDuplicateDecl, mm.span, qfn);
                            } else {
                                (void)sym_.update_declared_type(*existing, mm.type);
                                fn_decl_by_name_[qfn].push_back(msid);
                            }
                        } else {
                            auto fins = sym_.insert(sema::SymbolKind::kFn, qfn, mm.type, mm.span);
                            if (!fins.ok && fins.is_duplicate) {
                                err_(mm.span, "duplicate symbol (actor member function): " + qfn);
                                diag_(diag::Code::kDuplicateDecl, mm.span, qfn);
                            } else {
                                fn_decl_by_name_[qfn].push_back(msid);
                            }
                        }

                        if (actor_ty != ty::kInvalidType) {
                            actor_method_map_[actor_ty][std::string(mm.name)].push_back(msid);
                        }
                    }
                }
                return;
            }

            if (s.kind == ast::StmtKind::kActsDecl) {
                const std::string qname = qualify_decl_name_(s.name);
                acts_qualified_name_by_stmt_[sid] = qname;
                if (s.acts_is_for) {
                    bool generic_owner = type_contains_unresolved_generic_param_(s.acts_target_type);
                    if (!generic_owner && s.acts_assoc_witness_count > 0) {
                        const auto& witnesses = ast_.acts_assoc_type_witness_decls();
                        const uint64_t begin = s.acts_assoc_witness_begin;
                        const uint64_t end = begin + s.acts_assoc_witness_count;
                        if (begin <= witnesses.size() && end <= witnesses.size()) {
                            for (uint32_t i = 0; i < s.acts_assoc_witness_count; ++i) {
                                const auto& witness = witnesses[s.acts_assoc_witness_begin + i];
                                if (type_contains_unresolved_generic_param_(witness.rhs_type)) {
                                    generic_owner = true;
                                    break;
                                }
                            }
                        }
                    }
                    if (generic_owner || s.decl_generic_param_count > 0) {
                        generic_acts_template_sid_set_.insert(sid);
                    }
                }
                if (auto existing = sym_.lookup_in_current(qname)) {
                    const auto& existing_sym = sym_.symbol(*existing);
                    if (existing_sym.kind != sema::SymbolKind::kAct) {
                        err_(s.span, "duplicate symbol (acts): " + qname);
                        diag_(diag::Code::kDuplicateDecl, s.span, qname);
                    }
                } else {
                    auto ins = sym_.insert(sema::SymbolKind::kAct, qname, ty::kInvalidType, s.span);
                    if (!ins.ok && ins.is_duplicate) {
                        err_(s.span, "duplicate symbol (acts): " + qname);
                        diag_(diag::Code::kDuplicateDecl, s.span, qname);
                    }
                }

                ty::TypeId owner_type = s.acts_target_type;
                if (s.acts_is_for) {
                    owner_type = canonicalize_acts_owner_type_(owner_type);
                    if (owner_type != s.acts_target_type) {
                        ast_.stmt_mut(sid).acts_target_type = owner_type;
                    }
                }

                if (s.acts_is_for && s.acts_has_set_name && owner_type != ty::kInvalidType) {
                    const std::string key = acts_named_decl_key_(owner_type, qname);
                    auto it = acts_named_decl_by_owner_and_name_.find(key);
                    if (it != acts_named_decl_by_owner_and_name_.end() && it->second != sid) {
                        std::ostringstream oss;
                        oss << "duplicate named acts declaration for type "
                            << types_.to_string(owner_type)
                            << " and set '" << qname << "'";
                        diag_(diag::Code::kTypeErrorGeneric, s.span, oss.str());
                        err_(s.span, oss.str());
                    } else {
                        acts_named_decl_by_owner_and_name_[key] = sid;
                    }
                }

                if (s.acts_is_for && !s.acts_has_set_name && owner_type != ty::kInvalidType) {
                    const bool generic_owner = type_contains_unresolved_generic_param_(owner_type);
                    if (!generic_owner) {
                        acts_default_decls_by_owner_[owner_type].push_back(sid);
                    }
                }

                if (s.acts_is_for && owner_type != ty::kInvalidType) {
                    (void)enforce_builtin_acts_policy_(s, owner_type);
                }

                const auto& kids = ast_.stmt_children();
                const uint64_t begin = s.stmt_begin;
                const uint64_t end = begin + s.stmt_count;
                if (begin <= kids.size() && end <= kids.size()) {
                    auto materialize_self_type = [&](ast::Param& p) {
                        if (!p.is_self || owner_type == ty::kInvalidType) return;
                        switch (p.self_kind) {
                            case ast::SelfReceiverKind::kRead:
                                p.type = types_.make_borrow(owner_type, /*is_mut=*/false);
                                break;
                            case ast::SelfReceiverKind::kMut:
                                p.type = types_.make_borrow(owner_type, /*is_mut=*/true);
                                break;
                            case ast::SelfReceiverKind::kMove:
                                p.type = owner_type;
                                break;
                            case ast::SelfReceiverKind::kNone:
                                // parser/self-rewrite 경로의 방어 fallback
                                p.type = types_.make_borrow(owner_type, /*is_mut=*/false);
                                break;
                        }
                    };

                    for (uint32_t i = 0; i < s.stmt_count; ++i) {
                        const ast::StmtId msid = kids[s.stmt_begin + i];
                        if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                        const auto& ms = ast_.stmt(msid);
                        if (ms.kind != ast::StmtKind::kFnDecl) continue;
                        if (ms.fn_generic_param_count > 0) {
                            generic_fn_template_sid_set_.insert(msid);
                        }

                        if (ms.param_count > 0) {
                            auto& p0 = ast_.params_mut()[ms.param_begin];
                            materialize_self_type(p0);
                        }

                        std::string mqname = qname;
                        if (!mqname.empty()) mqname += "::";
                        mqname += std::string(ms.name);
                        fn_qualified_name_by_stmt_[msid] = std::move(mqname);

                        // general acts namespace members are callable via path:
                        //   acts(Foo)::bar(...)
                        //   use acts(Foo) as f; f::bar(...)
                        if (!s.acts_is_for) {
                            ty::TypeId ret = ms.fn_ret;
                            if (ret == ty::kInvalidType) {
                                ret = types_.builtin(ty::Builtin::kUnit);
                            }

                            std::vector<ty::TypeId> params;
                            params.reserve(ms.param_count);
                            for (uint32_t pi = 0; pi < ms.param_count; ++pi) {
                                const auto& p = ast_.params()[ms.param_begin + pi];
                                ty::TypeId pt = p.type;
                                if (pt == ty::kInvalidType) pt = types_.error();
                                params.push_back(pt);
                            }

                            ty::TypeId sig = types_.make_fn(
                                ret,
                                params.data(),
                                (uint32_t)params.size(),
                                (uint32_t)params.size(),
                                nullptr,
                                nullptr,
                                ms.link_abi == ast::LinkAbi::kC,
                                ms.fn_is_c_variadic,
                                ty::CCallConv::kDefault
                            );
                            ast_.stmt_mut(msid).type = sig;

                            const std::string qfn = fn_qualified_name_by_stmt_[msid];
                            if (auto existing = sym_.lookup_in_current(qfn)) {
                                const auto& existing_sym = sym_.symbol(*existing);
                                if (existing_sym.kind != sema::SymbolKind::kFn) {
                                    err_(ms.span, "duplicate symbol (acts function): " + qfn);
                                    diag_(diag::Code::kDuplicateDecl, ms.span, qfn);
                                } else {
                                    (void)sym_.update_declared_type(*existing, sig);
                                    fn_decl_by_name_[qfn].push_back(msid);
                                }
                            } else {
                                auto fins = sym_.insert(sema::SymbolKind::kFn, qfn, sig, ms.span);
                                if (!fins.ok && fins.is_duplicate) {
                                    err_(ms.span, "duplicate symbol (acts function): " + qfn);
                                    diag_(diag::Code::kDuplicateDecl, ms.span, qfn);
                                } else {
                                    fn_decl_by_name_[qfn].push_back(msid);
                                }
                            }
                        }
                    }
                }
                collect_acts_operator_decl_(sid, s, /*allow_named_set=*/true);
                collect_acts_method_decl_(sid, s, /*allow_named_set=*/true);
                collect_acts_assoc_type_decl_(sid, s, /*allow_named_set=*/true);
                return;
            }
        };

        const auto& kids = ast_.stmt_children();
        const uint64_t begin = prog.stmt_begin;
        const uint64_t end = begin + prog.stmt_count;
        if (begin <= kids.size() && end <= kids.size()) {
            for (uint32_t i = 0; i < prog.stmt_count; ++i) {
                const ast::StmtId sid = kids[prog.stmt_begin + i];
                if (sid == ast::k_invalid_stmt || static_cast<size_t>(sid) >= ast_.stmts().size()) continue;
                const auto& s = ast_.stmt(sid);
                const auto saved_ns = namespace_stack_;
                if (auto it = explicit_file_bundle_overrides_.find(s.span.file_id);
                    it != explicit_file_bundle_overrides_.end() &&
                    it->second != current_bundle_name_()) {
                    namespace_stack_.clear();
                }
                collect_stmt(collect_stmt, sid);
                namespace_stack_ = saved_ns;
            }
        }

        ensure_builtin_family_proto_aliases_();

        auto make_member_sig_key = [&](ast::StmtId fn_sid) -> std::string {
            if (fn_sid == ast::k_invalid_stmt || (size_t)fn_sid >= ast_.stmts().size()) return {};
            const auto& def = ast_.stmt(fn_sid);
            if (def.kind != ast::StmtKind::kFnDecl) return {};

            std::ostringstream oss;
            oss << (def.fn_is_operator ? "op:" : "fn:");
            oss << std::string(def.name) << "|pc=" << def.positional_param_count
                << "|tc=" << def.param_count << "|ng=" << (def.has_named_group ? "1" : "0");
            for (uint32_t i = 0; i < def.param_count; ++i) {
                const auto& p = ast_.params()[def.param_begin + i];
                oss << "|p" << i << ":" << std::string(p.name) << ":" << p.type
                    << ":" << (p.is_named_group ? "N" : "P")
                    << ":" << (p.has_default ? "D" : "R")
                    << ":" << (p.is_self ? "S" : "_")
                    << ":sk=" << (uint32_t)p.self_kind;
            }
            oss << "|ret=" << def.fn_ret;
            return oss.str();
        };

        auto report_acts_overlap = [&](Span sp, ty::TypeId owner, std::string_view member_name) {
            std::ostringstream oss;
            oss << "acts signature overlap for type " << types_.to_string(owner)
                << " member '" << member_name
                << "' between default and named acts (or duplicate default)";
            diag_(diag::Code::kGenericActsOverlap, sp, types_.to_string(owner), std::string(member_name));
            err_(sp, oss.str());
        };

        for (const auto& owner_entry : acts_default_method_map_) {
            const ty::TypeId owner_type = owner_entry.first;
            for (const auto& name_entry : owner_entry.second) {
                std::unordered_map<std::string, ActsMethodDecl> seen_default;
                std::unordered_map<std::string, ActsMethodDecl> seen_named;
                for (const auto& decl : name_entry.second) {
                    const std::string key = make_member_sig_key(decl.fn_sid);
                    if (key.empty()) continue;

                    if (decl.from_named_set) {
                        auto dit = seen_default.find(key);
                        if (dit != seen_default.end()) {
                            report_acts_overlap(ast_.stmt(decl.fn_sid).span, owner_type, name_entry.first);
                            report_acts_overlap(ast_.stmt(dit->second.fn_sid).span, owner_type, name_entry.first);
                        } else {
                            seen_named.emplace(key, decl);
                        }
                    } else {
                        auto nit = seen_named.find(key);
                        if (nit != seen_named.end()) {
                            report_acts_overlap(ast_.stmt(decl.fn_sid).span, owner_type, name_entry.first);
                            report_acts_overlap(ast_.stmt(nit->second.fn_sid).span, owner_type, name_entry.first);
                        }
                        auto dit = seen_default.find(key);
                        if (dit != seen_default.end()) {
                            report_acts_overlap(ast_.stmt(decl.fn_sid).span, owner_type, name_entry.first);
                            report_acts_overlap(ast_.stmt(dit->second.fn_sid).span, owner_type, name_entry.first);
                        } else {
                            seen_default.emplace(key, decl);
                        }
                    }
                }
            }
        }

        for (const auto& op_entry : acts_default_operator_map_) {
            std::unordered_map<std::string, ActsOperatorDecl> seen_default;
            std::unordered_map<std::string, ActsOperatorDecl> seen_named;
            for (const auto& decl : op_entry.second) {
                const std::string key = make_member_sig_key(decl.fn_sid);
                if (key.empty()) continue;

                if (decl.from_named_set) {
                    auto dit = seen_default.find(key);
                    if (dit != seen_default.end()) {
                        report_acts_overlap(ast_.stmt(decl.fn_sid).span, decl.owner_type, ast_.stmt(decl.fn_sid).name);
                        report_acts_overlap(ast_.stmt(dit->second.fn_sid).span, decl.owner_type, ast_.stmt(dit->second.fn_sid).name);
                    } else {
                        seen_named.emplace(key, decl);
                    }
                } else {
                    auto nit = seen_named.find(key);
                    if (nit != seen_named.end()) {
                        report_acts_overlap(ast_.stmt(decl.fn_sid).span, decl.owner_type, ast_.stmt(decl.fn_sid).name);
                        report_acts_overlap(ast_.stmt(nit->second.fn_sid).span, decl.owner_type, ast_.stmt(nit->second.fn_sid).name);
                    }
                    auto dit = seen_default.find(key);
                    if (dit != seen_default.end()) {
                        report_acts_overlap(ast_.stmt(decl.fn_sid).span, decl.owner_type, ast_.stmt(decl.fn_sid).name);
                        report_acts_overlap(ast_.stmt(dit->second.fn_sid).span, decl.owner_type, ast_.stmt(dit->second.fn_sid).name);
                    } else {
                        seen_default.emplace(key, decl);
                    }
                }
            }
        }

        struct ParamShape {
            std::string label;
            ty::TypeId type = ty::kInvalidType;
            bool has_default = false;
            Span span{};
        };
        struct DeclShape {
            ast::StmtId sid = ast::k_invalid_stmt;
            std::string name{};
            Span span{};
            ty::TypeId ret = ty::kInvalidType;
            bool is_c_abi = false;
            std::vector<ParamShape> positional;
            std::vector<ParamShape> named;
        };

        const auto make_decl_key = [](const DeclShape& d) -> std::string {
            std::ostringstream oss;
            oss << "P" << d.positional.size();
            for (const auto& p : d.positional) {
                oss << "|" << p.label << ":" << p.type;
            }
            oss << "|N" << d.named.size();
            for (const auto& p : d.named) {
                oss << "|" << p.label << ":" << p.type << ":" << (p.has_default ? "opt" : "req");
            }
            return oss.str();
        };

        const auto make_positional_type_key = [](const DeclShape& d) -> std::string {
            std::ostringstream oss;
            oss << "P" << d.positional.size();
            for (const auto& p : d.positional) {
                oss << "|" << p.type;
            }
            return oss.str();
        };

        const auto make_labeled_set_key = [](const DeclShape& d) -> std::string {
            std::vector<std::pair<std::string, ty::TypeId>> elems;
            elems.reserve(d.positional.size());
            for (const auto& p : d.positional) {
                elems.emplace_back(p.label, p.type);
            }
            std::sort(elems.begin(), elems.end(), [](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first < b.first;
                return a.second < b.second;
            });

            std::ostringstream oss;
            oss << "L" << elems.size();
            for (const auto& e : elems) {
                oss << "|" << e.first << ":" << e.second;
            }
            return oss.str();
        };

        const auto make_human_sig = [&](const DeclShape& d) -> std::string {
            std::ostringstream oss;
            oss << d.name << "(";
            bool first = true;
            for (const auto& p : d.positional) {
                if (!first) oss << ", ";
                first = false;
                oss << p.label << ": " << types_.to_string(p.type);
            }
            if (!d.named.empty()) {
                if (!first) oss << ", ";
                oss << "{";
                for (size_t i = 0; i < d.named.size(); ++i) {
                    if (i) oss << ", ";
                    oss << d.named[i].label << ": " << types_.to_string(d.named[i].type);
                    if (d.named[i].has_default) oss << "=?";
                }
                oss << "}";
            }
            oss << ") -> " << types_.to_string(d.ret);
            return oss.str();
        };

        const auto project_mangled_name = [&](const DeclShape& d) -> std::string {
            std::ostringstream sig;
            sig << "def(";
            bool first = true;
            for (const auto& p : d.positional) {
                if (!first) sig << ", ";
                first = false;
                sig << types_.to_string(p.type);
            }
            if (!d.named.empty()) {
                if (!first) sig << ", ";
                sig << "{";
                for (size_t i = 0; i < d.named.size(); ++i) {
                    if (i) sig << ", ";
                    sig << d.named[i].label << ":" << types_.to_string(d.named[i].type);
                    if (d.named[i].has_default) sig << "=?";
                }
                sig << "}";
            }
            sig << ") -> " << types_.to_string(d.ret);

            std::string out = d.name + "$" + sig.str();
            for (char& ch : out) {
                if (!std::isalnum(static_cast<unsigned char>(ch)) && ch != '_') ch = '_';
            }
            return out;
        };

        for (const auto& it : fn_decl_by_name_) {
            const std::string& fn_name = it.first;
            const std::vector<ast::StmtId>& decl_ids = it.second;
            if (decl_ids.empty()) continue;

            std::vector<DeclShape> decls;
            decls.reserve(decl_ids.size());

            for (const ast::StmtId sid : decl_ids) {
                const ast::Stmt& s = ast_.stmt(sid);
                if (s.kind != ast::StmtKind::kFnDecl) continue;

                DeclShape d{};
                d.sid = sid;
                auto qit = fn_qualified_name_by_stmt_.find(sid);
                d.name = (qit != fn_qualified_name_by_stmt_.end()) ? qit->second : std::string(s.name);
                d.span = s.span;
                d.is_c_abi = (s.link_abi == ast::LinkAbi::kC);
                d.ret = (s.fn_ret != ty::kInvalidType)
                    ? s.fn_ret
                    : ((s.type != ty::kInvalidType && types_.get(s.type).kind == ty::Kind::kFn)
                        ? types_.get(s.type).ret
                        : types_.error());

                const uint32_t total = s.param_count;
                uint32_t pos_cnt = s.positional_param_count;
                if (pos_cnt > total) pos_cnt = total;

                std::unordered_set<std::string> seen_labels;
                seen_labels.reserve(total);

                for (uint32_t i = 0; i < total; ++i) {
                    const auto& p = ast_.params()[s.param_begin + i];
                    ParamShape ps{};
                    ps.label = std::string(p.name);
                    ps.type = p.type;
                    ps.has_default = p.has_default;
                    ps.span = p.span;

                    if (!seen_labels.insert(ps.label).second) {
                        std::string msg = "duplicate parameter label '" + ps.label +
                                        "' in overload declaration of '" + fn_name + "'";
                        diag_(diag::Code::kTypeErrorGeneric, ps.span, msg);
                        err_(ps.span, msg);
                    }

                    const bool is_named = (i >= pos_cnt) || p.is_named_group;
                    if (is_named) d.named.push_back(std::move(ps));
                    else d.positional.push_back(std::move(ps));
                }

                decls.push_back(std::move(d));
            }

            if (decls.size() <= 1) continue;

            bool has_c_abi = false;
            for (const auto& d : decls) {
                if (d.is_c_abi) {
                    has_c_abi = true;
                    break;
                }
            }
            if (has_c_abi) {
                for (const auto& d : decls) {
                    if (!d.is_c_abi) continue;
                    diag_(diag::Code::kAbiCOverloadNotAllowed, d.span, fn_name);
                    err_(d.span, "C ABI function must not be overloaded: " + fn_name);
                }
            }

            std::unordered_map<std::string, size_t> decl_key_owner;
            decl_key_owner.reserve(decls.size());
            for (size_t i = 0; i < decls.size(); ++i) {
                const std::string key = make_decl_key(decls[i]);
                auto ins = decl_key_owner.emplace(key, i);
                if (!ins.second) {
                    const auto& prev = decls[ins.first->second];
                    std::string msg;
                    if (prev.ret != decls[i].ret) {
                        msg = "overload conflict in '" + fn_name +
                            "': return-type-only overloading is not allowed";
                    } else {
                        msg = "overload conflict in '" + fn_name +
                            "': declaration key collision";
                    }
                    diag_(diag::Code::kOverloadDeclConflict, decls[i].span, fn_name, msg);
                    err_(decls[i].span, msg);
                }
            }

            std::unordered_map<std::string, size_t> pos_view_owner;
            pos_view_owner.reserve(decls.size());
            for (size_t i = 0; i < decls.size(); ++i) {
                if (!decls[i].named.empty()) continue;
                const std::string key = make_positional_type_key(decls[i]);
                auto ins = pos_view_owner.emplace(key, i);
                if (!ins.second) {
                    std::string msg = "overload conflict in '" + fn_name +
                        "': positional-call view is indistinguishable";
                    diag_(diag::Code::kOverloadDeclConflict, decls[i].span, fn_name, msg);
                    err_(decls[i].span, msg);
                }
            }

            std::unordered_map<std::string, size_t> labeled_view_owner;
            labeled_view_owner.reserve(decls.size());
            for (size_t i = 0; i < decls.size(); ++i) {
                if (!decls[i].named.empty()) continue;
                const std::string key = make_labeled_set_key(decls[i]);
                auto ins = labeled_view_owner.emplace(key, i);
                if (!ins.second) {
                    std::string msg = "overload conflict in '" + fn_name +
                        "': labeled-call view is indistinguishable";
                    diag_(diag::Code::kOverloadDeclConflict, decls[i].span, fn_name, msg);
                    err_(decls[i].span, msg);
                }
            }

            std::unordered_map<std::string, std::pair<ast::StmtId, std::string>> mangled_owner;
            mangled_owner.reserve(decls.size());
            for (const auto& d : decls) {
                const std::string mangled = project_mangled_name(d);
                const std::string human = make_human_sig(d);
                auto ins = mangled_owner.emplace(mangled, std::make_pair(d.sid, human));
                if (!ins.second && ins.first->second.first != d.sid) {
                    diag_(diag::Code::kMangleSymbolCollision, d.span, mangled, ins.first->second.second, human);
                    err_(d.span, "mangle symbol collision: " + mangled);
                }
            }
        }
    }

    /// @brief acts operator 조회용 키를 생성한다.
    uint64_t TypeChecker::acts_operator_key_(ty::TypeId owner_type, syntax::TokenKind op_token, bool is_postfix) {
        const uint64_t owner = static_cast<uint64_t>(owner_type);
        const uint64_t op = static_cast<uint64_t>(op_token);
        const uint64_t pf = is_postfix ? 1ull : 0ull;
        return (owner << 32) | (op << 1) | pf;
    }

    std::string TypeChecker::acts_named_decl_key_(ty::TypeId owner_type, std::string_view set_qname) {
        std::string out = std::to_string(static_cast<uint64_t>(owner_type));
        out += "::";
        out += std::string(set_qname);
        return out;
    }

    bool TypeChecker::is_intlike_builtin_(ty::Builtin b) {
        using B = ty::Builtin;
        switch (b) {
            case B::kI8:
            case B::kI16:
            case B::kI32:
            case B::kI64:
            case B::kI128:
            case B::kU8:
            case B::kU16:
            case B::kU32:
            case B::kU64:
            case B::kU128:
            case B::kISize:
            case B::kUSize:
                return true;
            default:
                return false;
        }
    }

    bool TypeChecker::is_float_builtin_(ty::Builtin b) {
        return b == ty::Builtin::kF32 ||
               b == ty::Builtin::kF64 ||
               b == ty::Builtin::kF128;
    }

    bool TypeChecker::is_char_builtin_(ty::Builtin b) {
        return b == ty::Builtin::kChar;
    }

    bool TypeChecker::is_text_builtin_(ty::Builtin b) {
        return b == ty::Builtin::kText;
    }

    bool TypeChecker::is_bool_builtin_(ty::Builtin b) {
        return b == ty::Builtin::kBool;
    }

    TypeChecker::BuiltinActsPolicy TypeChecker::builtin_acts_policy_(ty::Builtin b) {
        BuiltinActsPolicy p{};
        p.reserved_bundle = "core";

        if (is_intlike_builtin_(b)) {
            p.allow_default_acts = true;
            p.allow_named_acts = false;
            p.api_group = BuiltinActsApiGroup::IntLike;
            return p;
        }
        if (is_float_builtin_(b)) {
            p.allow_default_acts = true;
            p.allow_named_acts = false;
            p.api_group = BuiltinActsApiGroup::FloatLike;
            return p;
        }
        if (is_bool_builtin_(b)) {
            p.allow_default_acts = true;
            p.allow_named_acts = false;
            p.api_group = BuiltinActsApiGroup::BoolLike;
            return p;
        }
        if (is_char_builtin_(b)) {
            p.allow_default_acts = true;
            p.allow_named_acts = false;
            p.api_group = BuiltinActsApiGroup::CharLike;
            return p;
        }
        if (is_text_builtin_(b)) {
            p.allow_default_acts = true;
            p.allow_named_acts = false;
            p.api_group = BuiltinActsApiGroup::TextLike;
            return p;
        }

        p.allow_default_acts = false;
        p.allow_named_acts = false;
        p.api_group = BuiltinActsApiGroup::Unsupported;
        return p;
    }

    bool TypeChecker::is_builtin_owner_type_(ty::TypeId t, ty::Builtin* out_builtin) const {
        if (t == ty::kInvalidType) return false;
        const auto& tt = types_.get(t);
        if (tt.kind != ty::Kind::kBuiltin) return false;
        if (out_builtin != nullptr) *out_builtin = tt.builtin;
        return true;
    }

    std::optional<ty::TypeId> TypeChecker::parse_builtin_owner_type_from_text_(std::string_view s) const {
        using B = ty::Builtin;
        if (s == "bool") return types_.builtin(B::kBool);
        if (s == "char") return types_.builtin(B::kChar);
        if (s == "text") return types_.builtin(B::kText);
        if (s == "i8") return types_.builtin(B::kI8);
        if (s == "i16") return types_.builtin(B::kI16);
        if (s == "i32") return types_.builtin(B::kI32);
        if (s == "i64") return types_.builtin(B::kI64);
        if (s == "i128") return types_.builtin(B::kI128);
        if (s == "u8") return types_.builtin(B::kU8);
        if (s == "u16") return types_.builtin(B::kU16);
        if (s == "u32") return types_.builtin(B::kU32);
        if (s == "u64") return types_.builtin(B::kU64);
        if (s == "u128") return types_.builtin(B::kU128);
        if (s == "isize") return types_.builtin(B::kISize);
        if (s == "usize") return types_.builtin(B::kUSize);
        if (s == "f32") return types_.builtin(B::kF32);
        if (s == "f64") return types_.builtin(B::kF64);
        if (s == "f128") return types_.builtin(B::kF128);
        return std::nullopt;
    }

    bool TypeChecker::parse_external_builtin_acts_payload_(
        std::string_view payload,
        ty::TypeId& out_owner_type,
        std::string& out_member_name,
        bool& out_receiver_is_self
    ) const {
        out_owner_type = ty::kInvalidType;
        out_member_name.clear();
        out_receiver_is_self = false;

        if (!payload.starts_with("parus_builtin_acts|")) return false;

        std::string owner{};
        std::string member{};
        std::string self_flag{};

        size_t pos = 0;
        while (pos < payload.size()) {
            size_t next = payload.find('|', pos);
            if (next == std::string_view::npos) next = payload.size();
            const std::string_view part = payload.substr(pos, next - pos);
            const size_t eq = part.find('=');
            if (eq != std::string_view::npos && eq + 1 < part.size()) {
                const std::string_view k = part.substr(0, eq);
                const std::string_view v = part.substr(eq + 1);
                if (k == "owner") owner.assign(v);
                else if (k == "member") member.assign(v);
                else if (k == "self") self_flag.assign(v);
            }
            if (next == payload.size()) break;
            pos = next + 1;
        }

        if (owner.empty() || member.empty()) return false;
        const auto owner_ty = parse_builtin_owner_type_from_text_(owner);
        if (!owner_ty.has_value()) return false;

        out_owner_type = *owner_ty;
        out_member_name = std::move(member);
        out_receiver_is_self = (self_flag == "1" || self_flag == "true");
        return true;
    }

    bool TypeChecker::parse_external_acts_assoc_type_payload_(
        std::string_view payload,
        ty::TypeId& out_owner_type,
        std::string& out_assoc_name,
        ty::TypeId& out_bound_type
    ) const {
        out_owner_type = ty::kInvalidType;
        out_assoc_name.clear();
        out_bound_type = ty::kInvalidType;

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

        auto parse_one_assoc = [&](std::string_view raw_body) -> bool {
            const std::string body = payload_unescape_value_(raw_body);
            const size_t first = body.find(',');
            if (first == std::string::npos) return false;
            const std::string name = body.substr(0, first);
            const std::string repr_and_sem = body.substr(first + 1);
            if (name.empty() || repr_and_sem.empty()) return false;
            const size_t split = repr_and_sem.find('@');
            const std::string repr = (split == std::string::npos) ? repr_and_sem : repr_and_sem.substr(0, split);
            const std::string sem = (split == std::string::npos) ? std::string{} : repr_and_sem.substr(split + 1);
            const ty::TypeId parsed =
                parus::cimport::parse_external_type_repr(repr, sem, std::string(payload), types_);
            if (parsed == ty::kInvalidType) return false;
            out_assoc_name = name;
            out_bound_type = parsed;
            return true;
        };

        size_t pos = 0;
        while (pos < payload.size()) {
            size_t next = payload.find('|', pos);
            if (next == std::string_view::npos) next = payload.size();
            const std::string_view part = payload.substr(pos, next - pos);
            const size_t eq = part.find('=');
            if (eq != std::string_view::npos && eq + 1 < part.size()) {
                const std::string_view k = part.substr(0, eq);
                const std::string_view v = part.substr(eq + 1);
                if (k == "owner") {
                    const std::string owner_text = payload_unescape_value_(v);
                    if (auto owner = parse_builtin_owner_type_from_text_(owner_text)) {
                        out_owner_type = *owner;
                    }
                } else if (k == "assoc_type" && parse_one_assoc(v)) {
                    return true;
                }
            }
            if (next == payload.size()) break;
            pos = next + 1;
        }
        return false;
    }

    void TypeChecker::collect_external_builtin_acts_methods_() {
        external_acts_default_method_map_.clear();
        external_acts_template_method_map_.clear();
        external_acts_default_assoc_type_map_.clear();
        external_acts_template_assoc_type_map_.clear();
        auto parse_external_generic_decl_meta_ = [](std::string_view payload) {
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
        auto type_contains_meta_generic_ =
            [&](auto&& self, ty::TypeId t, const std::unordered_set<std::string>& generic_names) -> bool {
                if (t == ty::kInvalidType || t >= types_.count()) return false;
                const auto& tt = types_.get(t);
                switch (tt.kind) {
                    case ty::Kind::kNamedUser: {
                        std::vector<std::string_view> path{};
                        std::vector<ty::TypeId> args{};
                        if (!types_.decompose_named_user(t, path, args) || path.empty()) return false;
                        if (args.empty() && path.size() == 1 &&
                            generic_names.find(std::string(path.front())) != generic_names.end()) {
                            return true;
                        }
                        for (const auto arg : args) {
                            if (self(self, arg, generic_names)) return true;
                        }
                        return false;
                    }
                    case ty::Kind::kOptional:
                    case ty::Kind::kArray:
                    case ty::Kind::kBorrow:
                    case ty::Kind::kEscape:
                    case ty::Kind::kPtr:
                        return self(self, tt.elem, generic_names);
                    case ty::Kind::kFn:
                        if (self(self, tt.ret, generic_names)) return true;
                        for (uint32_t i = 0; i < tt.param_count; ++i) {
                            if (self(self, types_.fn_param_at(t, i), generic_names)) return true;
                        }
                        return false;
                    default:
                        return false;
                }
            };
        for (uint32_t sid = 0; sid < sym_.symbols().size(); ++sid) {
            const auto& sym = sym_.symbol(sid);
            if (!sym.is_external) continue;
            if (sym.kind != sema::SymbolKind::kFn) continue;
            if (sym.declared_type == ty::kInvalidType) continue;

            ty::TypeId owner_t = ty::kInvalidType;
            std::string member_name{};
            bool receiver_is_self = false;
            if (parse_external_builtin_acts_payload_(sym.external_payload, owner_t, member_name, receiver_is_self)) {
                owner_t = canonicalize_acts_owner_type_(owner_t);
                ExternalActsMethodDecl md{};
                md.fn_symbol = sid;
                md.owner_type = owner_t;
                md.receiver_is_self = receiver_is_self;
                external_acts_default_method_map_[owner_t][member_name].push_back(md);
                continue;
            }

            const std::string_view full_name = sym.name;
            const size_t split = full_name.rfind("::");
            if (split == std::string_view::npos) continue;

            const std::string_view owner_set = full_name.substr(0, split);
            std::string_view acts_set = owner_set;
            if (const size_t owner_split = owner_set.rfind("::");
                owner_split != std::string_view::npos) {
                acts_set = owner_set.substr(owner_split + 2);
            }
            if (!acts_set.starts_with("__acts_for$")) continue;

            if (sym.declared_type >= types_.count()) continue;
            const auto& fn_t = types_.get(sym.declared_type);
            if (fn_t.kind != ty::Kind::kFn || fn_t.param_count == 0) continue;

            owner_t = types_.fn_param_at(sym.declared_type, 0);
            if (owner_t == ty::kInvalidType || owner_t >= types_.count()) continue;
            const auto& owner_tt = types_.get(owner_t);
            if (owner_tt.kind == ty::Kind::kBorrow) {
                owner_t = owner_tt.elem;
            }
            owner_t = canonicalize_acts_owner_type_(owner_t);
            if (owner_t == ty::kInvalidType) continue;

            member_name.assign(full_name.substr(split + 2));
            receiver_is_self = true;

            ExternalActsMethodDecl md{};
            md.fn_symbol = sid;
            md.owner_type = owner_t;
            md.receiver_is_self = receiver_is_self;
            md.external_payload = sym.external_payload;

            std::string owner_base{};
            std::vector<ty::TypeId> owner_args{};
            const auto meta = parse_external_generic_decl_meta_(sym.external_payload);
            std::unordered_set<std::string> meta_generic_names(meta.params.begin(), meta.params.end());
            const bool owner_contains_named_meta_generic =
                !meta_generic_names.empty() &&
                type_contains_meta_generic_(type_contains_meta_generic_, owner_t, meta_generic_names);
            const bool owner_has_template_shape =
                decompose_named_user_type_(owner_t, owner_base, owner_args) &&
                !owner_base.empty() &&
                !owner_args.empty() &&
                meta.params.size() >= owner_args.size();
            const bool owner_uses_meta_generics =
                owner_contains_named_meta_generic || owner_has_template_shape;
            if (owner_uses_meta_generics && !owner_base.empty()) {
                md.owner_is_generic_template = true;
                md.owner_generic_arity = static_cast<uint32_t>(owner_args.size());
                md.owner_base = owner_base;
                external_acts_template_method_map_[owner_base][member_name].push_back(md);
                continue;
            }
            external_acts_default_method_map_[owner_t][member_name].push_back(md);
        }

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

        for (uint32_t sid = 0; sid < sym_.symbols().size(); ++sid) {
            const auto& sym = sym_.symbol(sid);
            if (!sym.is_external) continue;
            const bool struct_like_external_type =
                (sym.kind == sema::SymbolKind::kField) ||
                (sym.kind == sema::SymbolKind::kType &&
                 (!sym.external_field_payload.empty() ||
                  sym.external_payload.starts_with("parus_field_decl")));
            if (sym.kind != sema::SymbolKind::kType && !struct_like_external_type) continue;
            if (sym.declared_type == ty::kInvalidType) continue;
            const std::string_view assoc_payload =
                !sym.external_field_payload.empty()
                    ? std::string_view(sym.external_field_payload)
                    : std::string_view(sym.external_payload);
            if (assoc_payload.find("assoc_type=") == std::string_view::npos) continue;

            const ty::TypeId owner_t = canonicalize_acts_owner_type_(sym.declared_type);
            std::string owner_base{};
            std::vector<ty::TypeId> owner_args{};
            (void)decompose_named_user_type_(owner_t, owner_base, owner_args);
            std::vector<std::string> payload_params{};
            size_t meta_pos = 0;
            while (meta_pos < assoc_payload.size()) {
                size_t meta_next = assoc_payload.find('|', meta_pos);
                if (meta_next == std::string_view::npos) meta_next = assoc_payload.size();
                const std::string_view meta_part = assoc_payload.substr(meta_pos, meta_next - meta_pos);
                if (meta_part.starts_with("gparam=")) {
                    payload_params.push_back(payload_unescape_value_(
                        meta_part.substr(std::string_view("gparam=").size())
                    ));
                }
                if (meta_next == assoc_payload.size()) break;
                meta_pos = meta_next + 1;
            }
            const bool owner_is_template =
                !payload_params.empty() &&
                !owner_base.empty();

            size_t pos = 0;
            while (pos < assoc_payload.size()) {
                size_t next = assoc_payload.find('|', pos);
                if (next == std::string_view::npos) next = assoc_payload.size();
                const std::string_view part = assoc_payload.substr(pos, next - pos);
                if (part.starts_with("assoc_type=")) {
                    const std::string body = payload_unescape_value_(
                        part.substr(std::string_view("assoc_type=").size())
                    );
                    const size_t first = body.find(',');
                    if (first != std::string::npos) {
                        const std::string assoc_name = body.substr(0, first);
                        const std::string repr_and_sem = body.substr(first + 1);
                        const size_t split = repr_and_sem.find('@');
                        const std::string repr =
                            (split == std::string::npos) ? repr_and_sem : repr_and_sem.substr(0, split);
                        const std::string sem =
                            (split == std::string::npos) ? std::string{} : repr_and_sem.substr(split + 1);
                        const ty::TypeId bound =
                            parus::cimport::parse_external_type_repr(repr, sem, std::string(sym.external_payload), types_);
                        if (!assoc_name.empty() && bound != ty::kInvalidType) {
                            ExternalActsAssocTypeDecl decl{};
                            decl.owner_type = owner_t;
                            decl.bound_type = bound;
                            decl.external_payload = std::string(assoc_payload);
                            if (owner_is_template) {
                                decl.owner_is_generic_template = true;
                                decl.owner_generic_arity = static_cast<uint32_t>(payload_params.size());
                                decl.owner_base = owner_base;
                                external_acts_template_assoc_type_map_[owner_base][assoc_name].push_back(decl);
                            } else {
                                external_acts_default_assoc_type_map_[owner_t][assoc_name].push_back(decl);
                            }
                        }
                    }
                }
                if (next == assoc_payload.size()) break;
                pos = next + 1;
            }
        }
    }

    void TypeChecker::collect_external_proto_stubs_() {
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

        for (uint32_t sid = 0; sid < sym_.symbols().size(); ++sid) {
            const auto& sym = sym_.symbol(sid);
            if (!sym.is_external) continue;
            if (!sym.is_export) continue;
            if (sym.kind != sema::SymbolKind::kType) continue;
            if (!sym.external_payload.starts_with("parus_decl_kind=proto")) continue;
            if (sym.name.empty()) continue;
            if (proto_decl_by_name_.find(sym.name) != proto_decl_by_name_.end()) continue;

            ast::Stmt stub{};
            stub.kind = ast::StmtKind::kProtoDecl;
            stub.span = sym.decl_span;
            stub.name = sym.name;
            stub.type = sym.declared_type;
            stub.is_export = sym.is_export;

            size_t pos = 0;
            while (pos < sym.external_payload.size()) {
                size_t next = sym.external_payload.find('|', pos);
                if (next == std::string_view::npos) next = sym.external_payload.size();
                const std::string_view part = std::string_view(sym.external_payload).substr(pos, next - pos);
                if (part.starts_with("gparam=")) {
                    ast::GenericParamDecl gp{};
                    gp.name = ast_.add_owned_string(
                        payload_unescape_value_(part.substr(std::string_view("gparam=").size()))
                    );
                    gp.span = sym.decl_span;
                    if (stub.decl_generic_param_count == 0) {
                        stub.decl_generic_param_begin = static_cast<uint32_t>(ast_.generic_param_decls().size());
                    }
                    ast_.add_generic_param_decl(gp);
                    ++stub.decl_generic_param_count;
                }
                if (next == sym.external_payload.size()) break;
                pos = next + 1;
            }

            const auto stub_sid = ast_.add_stmt(stub);
            proto_decl_by_name_[sym.name] = stub_sid;
            proto_qualified_name_by_stmt_[stub_sid] = sym.name;
            if (sym.declared_type != ty::kInvalidType) {
                proto_decl_by_type_[sym.declared_type] = stub_sid;
            }
            if (stub.decl_generic_param_count > 0) {
                generic_proto_template_sid_set_.insert(stub_sid);
            }
        }
    }

    void TypeChecker::ensure_builtin_family_proto_aliases_() {
        constexpr std::string_view kBuiltinProtoLeaves[] = {
            "Comparable",
            "BinaryInteger",
            "SignedInteger",
            "UnsignedInteger",
            "BinaryFloatingPoint",
            "Step",
        };

        for (const auto leaf : kBuiltinProtoLeaves) {
            std::optional<ast::StmtId> sid{};

            auto bind_alias = [&](std::string key, ast::StmtId target_sid) {
                if (key.empty()) return;
                proto_decl_by_name_[std::move(key)] = target_sid;
            };

            auto try_key = [&](std::string_view key) {
                if (key.empty() || sid.has_value()) return;
                if (auto it = proto_decl_by_name_.find(std::string(key)); it != proto_decl_by_name_.end()) {
                    sid = it->second;
                }
            };

            try_key(leaf);
            try_key(std::string("constraints::") + std::string(leaf));
            try_key(std::string("core::constraints::") + std::string(leaf));
            if (!sid.has_value()) {
                for (const auto& [name, existing_sid] : proto_decl_by_name_) {
                    const size_t pos = name.rfind("::");
                    const std::string_view tail =
                        (pos == std::string::npos) ? std::string_view(name)
                                                   : std::string_view(name).substr(pos + 2);
                    if (tail == leaf) {
                        sid = existing_sid;
                        break;
                    }
                }
            }

            if (!sid.has_value()) continue;

            bind_alias(std::string(leaf), *sid);
            bind_alias(std::string("constraints::") + std::string(leaf), *sid);
            bind_alias(std::string("core::constraints::") + std::string(leaf), *sid);
            if (proto_qualified_name_by_stmt_.find(*sid) == proto_qualified_name_by_stmt_.end()) {
                proto_qualified_name_by_stmt_[*sid] = "core::constraints::" + std::string(leaf);
            }
        }
    }

    void TypeChecker::collect_external_fn_overloads_() {
        external_fn_overload_map_.clear();

        auto visible_name = [](std::string_view name) -> std::string {
            constexpr std::string_view marker = "@@extovl$";
            const size_t pos = name.find(marker);
            if (pos == std::string_view::npos) return std::string(name);
            return std::string(name.substr(0, pos));
        };
        auto is_c_abi_external_payload = [](std::string_view payload) -> bool {
            return payload.starts_with("parus_c_import|") ||
                   payload.starts_with("parus_c_abi_decl|");
        };

        for (uint32_t sid = 0; sid < sym_.symbols().size(); ++sid) {
            const auto& sym = sym_.symbol(sid);
            if (!sym.is_external) continue;
            if (sym.kind != sema::SymbolKind::kFn) continue;
            if (sym.declared_type == ty::kInvalidType) continue;
            if (is_c_abi_external_payload(sym.external_payload)) continue;
            external_fn_overload_map_[visible_name(sym.name)].push_back(sid);
        }
    }

    bool TypeChecker::parse_external_enum_decl_payload_(
        std::string_view payload,
        EnumAbiMeta& out_meta
    ) const {
        out_meta = {};
        if (!payload.starts_with("parus_decl_kind=enum")) return false;

        auto parse_i64_ = [](std::string_view text, int64_t& out) -> bool {
            if (text.empty()) return false;
            bool neg = false;
            size_t pos = 0;
            if (text[0] == '-') {
                neg = true;
                pos = 1;
                if (pos >= text.size()) return false;
            }
            int64_t value = 0;
            for (; pos < text.size(); ++pos) {
                const char ch = text[pos];
                if (ch < '0' || ch > '9') return false;
                value = value * 10 + static_cast<int64_t>(ch - '0');
            }
            out = neg ? -value : value;
            return true;
        };

        out_meta.layout = ast::FieldLayout::kNone;
        out_meta.is_layout_c = false;

        size_t pos = 0;
        while (pos < payload.size()) {
            size_t next = payload.find('|', pos);
            if (next == std::string_view::npos) next = payload.size();
            const std::string_view part = payload.substr(pos, next - pos);
            const size_t eq = part.find('=');
            if (eq != std::string_view::npos && eq + 1 < part.size()) {
                const std::string_view key = part.substr(0, eq);
                const std::string_view val = part.substr(eq + 1);
                if (key == "layout") {
                    if (val == "c") {
                        out_meta.layout = ast::FieldLayout::kC;
                        out_meta.is_layout_c = true;
                    } else {
                        out_meta.layout = ast::FieldLayout::kNone;
                        out_meta.is_layout_c = false;
                    }
                } else if (key == "variant") {
                    const size_t comma = val.rfind(',');
                    if (comma == std::string_view::npos || comma == 0 || comma + 1 >= val.size()) {
                        return false;
                    }
                    int64_t tag = 0;
                    if (!parse_i64_(val.substr(comma + 1), tag)) return false;

                    EnumVariantMeta vm{};
                    vm.name = std::string(val.substr(0, comma));
                    vm.index = static_cast<uint32_t>(out_meta.variants.size());
                    vm.tag = tag;
                    vm.has_discriminant = true;
                    out_meta.variant_index_by_name[vm.name] = vm.index;
                    out_meta.variants.push_back(std::move(vm));
                }
            }
            if (next == payload.size()) break;
            pos = next + 1;
        }

        return !out_meta.variants.empty();
    }

    void TypeChecker::collect_external_enum_metadata_() {
        for (uint32_t sid = 0; sid < sym_.symbols().size(); ++sid) {
            const auto& sym = sym_.symbol(sid);
            if (!sym.is_external) continue;
            if (sym.kind != sema::SymbolKind::kType) continue;
            if (sym.external_payload.empty()) continue;
            if (sym.declared_type == ty::kInvalidType) continue;
            if (enum_abi_meta_by_type_.find(sym.declared_type) != enum_abi_meta_by_type_.end()) continue;

            EnumAbiMeta meta{};
            if (!parse_external_enum_decl_payload_(sym.external_payload, meta)) continue;
            meta.sid = ast::k_invalid_stmt;
            enum_abi_meta_by_type_[sym.declared_type] = std::move(meta);
        }
    }

    bool TypeChecker::is_core_impl_marker_stmt_(const ast::Stmt& s) const {
        if (s.kind != ast::StmtKind::kCompilerIntrinsicDirective) return false;
        if (s.directive_target_path_count != 0) return false; // tag form only: $![Impl::Core];
        if (s.directive_key_path_count != 2) return false;

        const auto& segs = ast_.path_segs();
        const uint64_t begin = s.directive_key_path_begin;
        const uint64_t end = begin + s.directive_key_path_count;
        if (begin > segs.size() || end > segs.size()) return false;
        return segs[s.directive_key_path_begin] == "Impl" &&
               segs[s.directive_key_path_begin + 1] == "Core";
    }

    TypeChecker::ImplBindingKind TypeChecker::parse_impl_binding_payload_(std::string_view payload) const {
        if (!payload.starts_with("parus_impl_binding|key=")) return ImplBindingKind::kNone;
        std::string_view rest = payload.substr(std::string_view("parus_impl_binding|key=").size());
        std::string_view key = rest;
        std::string_view mode{};
        if (const size_t split = rest.find('|'); split != std::string_view::npos) {
            key = rest.substr(0, split);
            rest = rest.substr(split + 1);
            while (!rest.empty()) {
                const size_t next = rest.find('|');
                const std::string_view part =
                    (next == std::string_view::npos) ? rest : rest.substr(0, next);
                if (part.starts_with("mode=")) {
                    mode = part.substr(std::string_view("mode=").size());
                }
                if (next == std::string_view::npos) break;
                rest = rest.substr(next + 1);
            }
        }
        if (!mode.empty() && mode != "compiler") return ImplBindingKind::kNone;
        if (key == "Impl::SpinLoop") return ImplBindingKind::kSpinLoop;
        if (key == "Impl::StepNext") return ImplBindingKind::kStepNext;
        if (key == "Impl::SizeOf") return ImplBindingKind::kSizeOf;
        if (key == "Impl::AlignOf") return ImplBindingKind::kAlignOf;
        return ImplBindingKind::kNone;
    }

    bool TypeChecker::stmt_impl_binding_key_(const ast::Stmt& s, std::string& out_key) const {
        out_key.clear();
        if (s.directive_target_path_count != 0) return false;
        if (s.directive_key_path_count != 2) return false;

        const auto& segs = ast_.path_segs();
        const uint64_t begin = s.directive_key_path_begin;
        const uint64_t end = begin + s.directive_key_path_count;
        if (begin > segs.size() || end > segs.size()) return false;
        if (segs[s.directive_key_path_begin] != "Impl") return false;
        if (segs[s.directive_key_path_begin + 1] == "Core") return false;
        out_key = "Impl::";
        out_key += segs[s.directive_key_path_begin + 1];
        return true;
    }

    bool TypeChecker::stmt_impl_binding_kind_(const ast::Stmt& s, ImplBindingKind& out_kind) const {
        out_kind = ImplBindingKind::kNone;
        std::string key{};
        if (!stmt_impl_binding_key_(s, key)) return false;
        const std::string_view leaf = std::string_view(key).substr(std::string_view("Impl::").size());
        if (leaf == "SpinLoop") out_kind = ImplBindingKind::kSpinLoop;
        else if (leaf == "StepNext") out_kind = ImplBindingKind::kStepNext;
        else if (leaf == "SizeOf") out_kind = ImplBindingKind::kSizeOf;
        else if (leaf == "AlignOf") out_kind = ImplBindingKind::kAlignOf;
        else return false;
        return true;
    }

    std::string TypeChecker::make_impl_binding_payload_(ImplBindingKind kind) const {
        switch (kind) {
            case ImplBindingKind::kSpinLoop: return make_impl_binding_payload_("Impl::SpinLoop", /*compiler_owned=*/true);
            case ImplBindingKind::kStepNext: return make_impl_binding_payload_("Impl::StepNext", /*compiler_owned=*/true);
            case ImplBindingKind::kSizeOf: return make_impl_binding_payload_("Impl::SizeOf", /*compiler_owned=*/true);
            case ImplBindingKind::kAlignOf: return make_impl_binding_payload_("Impl::AlignOf", /*compiler_owned=*/true);
            case ImplBindingKind::kNone: break;
        }
        return {};
    }

    std::string TypeChecker::make_impl_binding_payload_(std::string_view key, bool compiler_owned) const {
        std::string payload = "parus_impl_binding|key=";
        payload += key;
        payload += compiler_owned ? "|mode=compiler" : "|mode=library";
        return payload;
    }

    void TypeChecker::collect_unresolved_generic_param_names_in_type_(
        ty::TypeId t,
        std::unordered_set<std::string>& out
    ) const {
        if (t == ty::kInvalidType) return;
        const auto& tt = types_.get(t);
        switch (tt.kind) {
            case ty::Kind::kNamedUser: {
                std::vector<std::string_view> path{};
                std::vector<ty::TypeId> args{};
                if (!types_.decompose_named_user(t, path, args) || path.empty()) return;

                for (const auto arg : args) {
                    collect_unresolved_generic_param_names_in_type_(arg, out);
                }

                if (!args.empty() || path.size() != 1) return;

                ty::Builtin builtin{};
                if (ty::TypePool::builtin_from_name(path.front(), builtin) ||
                    ty::TypePool::c_builtin_from_name(path.front(), builtin)) {
                    return;
                }
                if (!lookup_symbol_(path.front()).has_value()) {
                    out.emplace(path.front());
                }
                return;
            }
            case ty::Kind::kOptional:
            case ty::Kind::kArray:
            case ty::Kind::kBorrow:
            case ty::Kind::kEscape:
            case ty::Kind::kPtr:
                collect_unresolved_generic_param_names_in_type_(tt.elem, out);
                return;
            case ty::Kind::kFn: {
                collect_unresolved_generic_param_names_in_type_(tt.ret, out);
                for (uint32_t i = 0; i < tt.param_count; ++i) {
                    collect_unresolved_generic_param_names_in_type_(types_.fn_param_at(t, i), out);
                }
                return;
            }
            default:
                return;
        }
    }

    void TypeChecker::collect_unresolved_generic_param_names_in_proto_target_(
        ty::TypeId t,
        std::unordered_set<std::string>& out
    ) const {
        if (t == ty::kInvalidType) return;
        const auto& tt = types_.get(t);
        switch (tt.kind) {
            case ty::Kind::kNamedUser: {
                std::vector<std::string_view> path{};
                std::vector<ty::TypeId> args{};
                if (!types_.decompose_named_user(t, path, args) || path.empty()) return;
                for (const auto arg : args) {
                    collect_unresolved_generic_param_names_in_type_(arg, out);
                }
                return;
            }
            case ty::Kind::kOptional:
            case ty::Kind::kArray:
            case ty::Kind::kBorrow:
            case ty::Kind::kEscape:
            case ty::Kind::kPtr:
                collect_unresolved_generic_param_names_in_type_(tt.elem, out);
                return;
            case ty::Kind::kFn: {
                collect_unresolved_generic_param_names_in_type_(tt.ret, out);
                for (uint32_t i = 0; i < tt.param_count; ++i) {
                    collect_unresolved_generic_param_names_in_type_(types_.fn_param_at(t, i), out);
                }
                return;
            }
            default:
                return;
        }
    }

    std::optional<ast::StmtId> TypeChecker::resolve_proto_sid_for_constraint_(std::string_view raw) const {
        if (raw.empty()) return std::nullopt;
        const bool qualified_raw = raw.find("::") != std::string_view::npos;
        auto resolve_from_symbol = [&](uint32_t sym_sid) -> std::optional<ast::StmtId> {
            const auto& ss = sym_.symbol(sym_sid);
            if (auto pit = proto_decl_by_name_.find(ss.name); pit != proto_decl_by_name_.end()) {
                return pit->second;
            }
            return std::nullopt;
        };
        auto try_resolve = [&](std::string key) -> std::optional<ast::StmtId> {
            const bool key_had_alias = rewrite_imported_path_(key).has_value();
            const bool key_rewritten = apply_imported_path_rewrite_(key);
            std::optional<uint32_t> sym_sid{};
            if (!key_had_alias && qualified_path_requires_import_(key)) {
                sym_sid = lookup_public_proto_target_symbol_(key);
                if (!sym_sid.has_value()) {
                    return std::nullopt;
                }
            }
            if (auto it = proto_decl_by_name_.find(key); it != proto_decl_by_name_.end()) {
                return it->second;
            }
            if (!sym_sid.has_value()) {
                sym_sid = key_rewritten ? sym_.lookup(key) : lookup_symbol_(key);
            }
            if (sym_sid.has_value()) {
                if (auto sid = resolve_from_symbol(*sym_sid)) {
                    return sid;
                }
            }
            return std::nullopt;
        };

        if (auto resolved = try_resolve(std::string(raw))) {
            return resolved;
        }
        if (!raw.starts_with("core::") && qualified_raw) {
            if (auto resolved = try_resolve("core::" + std::string(raw))) {
                return resolved;
            }
        }
        if (!qualified_raw) {
            if (auto it = proto_decl_by_name_.find(std::string(raw)); it != proto_decl_by_name_.end()) {
                return it->second;
            }
        }

        const size_t sep = raw.rfind("::");
        const std::string_view leaf =
            (sep != std::string_view::npos && sep + 2 < raw.size()) ? raw.substr(sep + 2) : raw;
        if (!leaf.empty()) {
            std::optional<ast::StmtId> unique{};
            bool ambiguous = false;
            for (const auto& [name, sid] : proto_decl_by_name_) {
                const size_t name_sep = name.rfind("::");
                const std::string_view candidate_leaf =
                    (name_sep == std::string::npos) ? std::string_view(name)
                                                    : std::string_view(name).substr(name_sep + 2);
                if (candidate_leaf != leaf) continue;
                if (unique.has_value() && *unique != sid) {
                    ambiguous = true;
                    break;
                }
                unique = sid;
            }
            if (!ambiguous && unique.has_value()) {
                return unique;
            }
        }
        return std::nullopt;
    }

    bool TypeChecker::is_builtin_family_proto_(ast::StmtId proto_sid) const {
        if (proto_sid == ast::k_invalid_stmt || static_cast<size_t>(proto_sid) >= ast_.stmts().size()) {
            return false;
        }

        std::string name = ast_.stmt(proto_sid).name.empty()
            ? std::string{}
            : std::string(ast_.stmt(proto_sid).name);
        if (auto it = proto_qualified_name_by_stmt_.find(proto_sid); it != proto_qualified_name_by_stmt_.end()) {
            name = it->second;
        }

        const auto tail = [&](std::string_view qname) -> std::string_view {
            const size_t pos = qname.rfind("::");
            return (pos == std::string_view::npos) ? qname : qname.substr(pos + 2);
        };

        const std::string_view leaf = tail(name);
        return leaf == "Comparable" ||
               leaf == "BinaryInteger" ||
               leaf == "SignedInteger" ||
               leaf == "UnsignedInteger" ||
               leaf == "BinaryFloatingPoint" ||
               leaf == "Step";
    }

    bool TypeChecker::builtin_family_proto_satisfied_by_primitive_name_(
        ty::TypeId concrete_t,
        std::string_view proto_name
    ) const {
        concrete_t = canonicalize_transparent_external_typedef_(concrete_t);
        if (concrete_t == ty::kInvalidType || concrete_t >= types_.count()) return false;
        const auto& tt = types_.get(concrete_t);
        if (tt.kind != ty::Kind::kBuiltin) return false;

        const size_t pos = proto_name.rfind("::");
        const std::string_view leaf =
            (pos == std::string_view::npos) ? proto_name : proto_name.substr(pos + 2);

        auto is_signed = [&](ty::Builtin b) {
            using B = ty::Builtin;
            switch (b) {
                case B::kI8:
                case B::kI16:
                case B::kI32:
                case B::kI64:
                case B::kI128:
                case B::kISize:
                    return true;
                default:
                    return false;
            }
        };
        auto is_unsigned = [&](ty::Builtin b) {
            using B = ty::Builtin;
            switch (b) {
                case B::kU8:
                case B::kU16:
                case B::kU32:
                case B::kU64:
                case B::kU128:
                case B::kUSize:
                    return true;
                default:
                    return false;
            }
        };
        auto is_char = [&](ty::Builtin b) {
            return b == ty::Builtin::kChar;
        };

        if (leaf == "SignedInteger") return is_signed(tt.builtin);
        if (leaf == "UnsignedInteger") return is_unsigned(tt.builtin);
        if (leaf == "BinaryInteger") return is_signed(tt.builtin) || is_unsigned(tt.builtin);
        if (leaf == "Comparable") return is_signed(tt.builtin) || is_unsigned(tt.builtin) || is_char(tt.builtin);
        if (leaf == "BinaryFloatingPoint") return is_float_builtin_(tt.builtin);
        if (leaf == "Step") return is_signed(tt.builtin) || is_unsigned(tt.builtin) || is_char(tt.builtin);
        return false;
    }

    bool TypeChecker::builtin_family_proto_satisfied_by_primitive_(ty::TypeId concrete_t, ast::StmtId proto_sid) const {
        if (!is_builtin_family_proto_(proto_sid)) return false;

        std::string name = ast_.stmt(proto_sid).name.empty()
            ? std::string{}
            : std::string(ast_.stmt(proto_sid).name);
        if (auto it = proto_qualified_name_by_stmt_.find(proto_sid); it != proto_qualified_name_by_stmt_.end()) {
            name = it->second;
        }
        return builtin_family_proto_satisfied_by_primitive_name_(concrete_t, name);
    }

    bool TypeChecker::proto_decl_matches_constraint_sid_(ast::StmtId candidate_sid, ast::StmtId expected_sid) const {
        if (candidate_sid == ast::k_invalid_stmt || expected_sid == ast::k_invalid_stmt) return false;
        if (candidate_sid == expected_sid) return true;
        if (static_cast<size_t>(candidate_sid) >= ast_.stmts().size() ||
            static_cast<size_t>(expected_sid) >= ast_.stmts().size()) {
            return false;
        }

        const auto& candidate_stmt = ast_.stmt(candidate_sid);
        const auto& expected_stmt = ast_.stmt(expected_sid);
        if (candidate_stmt.type != ty::kInvalidType &&
            expected_stmt.type != ty::kInvalidType &&
            candidate_stmt.type == expected_stmt.type) {
            return true;
        }

        auto proto_qname_base = [&](ast::StmtId sid) -> std::string_view {
            std::string_view qname = ast_.stmt(sid).name;
            if (auto it = proto_qualified_name_by_stmt_.find(sid); it != proto_qualified_name_by_stmt_.end()) {
                qname = it->second;
            }
            return qname;
        };

        const auto normalize_core_prefix = [](std::string_view qname) -> std::string_view {
            if (qname.starts_with("core::")) {
                return qname.substr(std::string_view("core::").size());
            }
            return qname;
        };

        const std::string_view candidate_qname = proto_qname_base(candidate_sid);
        const std::string_view expected_qname = proto_qname_base(expected_sid);
        if (candidate_qname.empty() || expected_qname.empty()) return false;
        if (candidate_qname == expected_qname) return true;
        if (normalize_core_prefix(candidate_qname) == normalize_core_prefix(expected_qname)) {
            return true;
        }

        const bool candidate_is_applied = candidate_qname.find('<') != std::string_view::npos;
        const bool expected_is_applied = expected_qname.find('<') != std::string_view::npos;
        if (candidate_is_applied || expected_is_applied) {
            return false;
        }

        const size_t csplit = candidate_qname.rfind("::");
        const size_t esplit = expected_qname.rfind("::");
        const std::string_view cleaf =
            (csplit == std::string_view::npos) ? candidate_qname : candidate_qname.substr(csplit + 2);
        const std::string_view eleaf =
            (esplit == std::string_view::npos) ? expected_qname : expected_qname.substr(esplit + 2);
        return cleaf == eleaf;
    }

    bool TypeChecker::type_satisfies_proto_constraint_(ty::TypeId concrete_t, ast::StmtId proto_sid, Span use_span) {
        if (proto_sid == ast::k_invalid_stmt) return false;
        concrete_t = canonicalize_transparent_external_typedef_(concrete_t);
        concrete_t = canonicalize_acts_owner_type_(concrete_t);
        if (is_builtin_family_proto_(proto_sid)) {
            return builtin_family_proto_satisfied_by_primitive_(concrete_t, proto_sid);
        }
        if (!evaluate_proto_require_at_apply_(proto_sid, concrete_t, use_span,
                                              /*emit_unsatisfied_diag=*/false,
                                              /*emit_shape_diag=*/false)) {
            return false;
        }
        if (auto it = explicit_impl_proto_sids_by_type_.find(concrete_t);
            it != explicit_impl_proto_sids_by_type_.end()) {
            const auto& impls = it->second;
            for (const auto impl_proto_sid : impls) {
                if (proto_decl_matches_constraint_sid_(impl_proto_sid, proto_sid)) {
                    return true;
                }
            }
        }

        std::vector<ast::StmtId> reqs;
        std::vector<ast::StmtId> provs;
        std::unordered_set<ast::StmtId> visiting;
        auto fn_sig_same = [&](const ast::Stmt& a, const ast::Stmt& b) -> bool {
            if (a.kind != ast::StmtKind::kFnDecl || b.kind != ast::StmtKind::kFnDecl) return false;
            if (a.name != b.name) return false;
            if (a.param_count != b.param_count) return false;
            if (a.positional_param_count != b.positional_param_count) return false;
            if (a.fn_ret != b.fn_ret) return false;
            for (uint32_t i = 0; i < a.param_count; ++i) {
                const auto& ap = ast_.params()[a.param_begin + i];
                const auto& bp = ast_.params()[b.param_begin + i];
                if (ap.type != bp.type || ap.is_self != bp.is_self || ap.self_kind != bp.self_kind) return false;
            }
            return true;
        };
        auto collect = [&](auto&& self, ast::StmtId cur_sid) -> void {
            if (cur_sid == ast::k_invalid_stmt || static_cast<size_t>(cur_sid) >= ast_.stmts().size()) return;
            if (!visiting.insert(cur_sid).second) return;
            const auto& cur = ast_.stmt(cur_sid);
            if (cur.kind != ast::StmtKind::kProtoDecl) return;
            const auto& refs = ast_.path_refs();
            const uint64_t ib = cur.decl_path_ref_begin;
            const uint64_t ie = ib + cur.decl_path_ref_count;
            if (ib <= refs.size() && ie <= refs.size()) {
                for (uint32_t i = cur.decl_path_ref_begin; i < cur.decl_path_ref_begin + cur.decl_path_ref_count; ++i) {
                    if (auto base_sid = resolve_proto_decl_from_path_ref_(refs[i], use_span)) {
                        self(self, *base_sid);
                    }
                }
            }
            const auto& kids = ast_.stmt_children();
            const uint64_t mb = cur.stmt_begin;
            const uint64_t me = mb + cur.stmt_count;
            if (mb <= kids.size() && me <= kids.size()) {
                for (uint32_t i = cur.stmt_begin; i < cur.stmt_begin + cur.stmt_count; ++i) {
                    const auto msid = kids[i];
                    if (msid == ast::k_invalid_stmt || static_cast<size_t>(msid) >= ast_.stmts().size()) continue;
                    const auto& m = ast_.stmt(msid);
                    if (m.kind != ast::StmtKind::kFnDecl) continue;
                    if (m.proto_fn_role == ast::ProtoFnRole::kRequire) reqs.push_back(msid);
                    if (m.proto_fn_role == ast::ProtoFnRole::kProvide && m.a != ast::k_invalid_stmt) provs.push_back(msid);
                }
            }
        };
        collect(collect, proto_sid);

        bool effective_required_empty = true;
        for (const auto req_sid : reqs) {
            if (req_sid == ast::k_invalid_stmt || static_cast<size_t>(req_sid) >= ast_.stmts().size()) continue;
            const auto& req = ast_.stmt(req_sid);
            bool satisfied = false;
            for (const auto prov_sid : provs) {
                if (prov_sid == ast::k_invalid_stmt || static_cast<size_t>(prov_sid) >= ast_.stmts().size()) continue;
                if (fn_sig_same(req, ast_.stmt(prov_sid))) {
                    satisfied = true;
                    break;
                }
            }
            if (!satisfied) {
                effective_required_empty = false;
                break;
            }
        }
        if (effective_required_empty) return true;

        ast::StmtId owner_sid = ast::k_invalid_stmt;
        if (auto cit = class_decl_by_type_.find(concrete_t); cit != class_decl_by_type_.end()) {
            owner_sid = cit->second;
        } else if (auto fit = field_abi_meta_by_type_.find(concrete_t); fit != field_abi_meta_by_type_.end()) {
            owner_sid = fit->second.sid;
        } else if (auto eit = enum_abi_meta_by_type_.find(concrete_t); eit != enum_abi_meta_by_type_.end()) {
            owner_sid = eit->second.sid;
        } else {
            auto adopt_owner_sid_from_symbol_ = [&](std::string key) -> bool {
                if (key.empty()) return false;
                auto sym_sid = sym_.lookup(key);
                if (!sym_sid.has_value()) sym_sid = lookup_symbol_(key);
                if (!sym_sid.has_value()) return false;
                const auto& ss = sym_.symbol(*sym_sid);
                const ty::TypeId declared_t = canonicalize_transparent_external_typedef_(ss.declared_type);
                if (auto cit = class_decl_by_type_.find(declared_t); cit != class_decl_by_type_.end()) {
                    owner_sid = cit->second;
                    return true;
                }
                if (auto fit = field_abi_meta_by_type_.find(declared_t); fit != field_abi_meta_by_type_.end()) {
                    owner_sid = fit->second.sid;
                    return true;
                }
                if (auto eit = enum_abi_meta_by_type_.find(declared_t); eit != enum_abi_meta_by_type_.end()) {
                    owner_sid = eit->second.sid;
                    return true;
                }
                return false;
            };

            const std::string concrete_name = types_.to_string(concrete_t);
            (void)adopt_owner_sid_from_symbol_(concrete_name);
        }
        auto external_type_declares_proto_ = [&](ty::TypeId type_id) -> bool {
            std::string owner_base{};
            std::vector<ty::TypeId> owner_args{};
            if (!decompose_named_user_type_(type_id, owner_base, owner_args) || owner_base.empty()) {
                return false;
            }

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
                    out.push_back(static_cast<char>(raw[i]));
                }
                return out;
            };

            struct ExternalGenericDeclMeta {
                std::vector<std::string> params{};
                std::vector<std::pair<std::string, std::string>> impl_protos{};
            };
            auto parse_external_generic_decl_meta_ = [&](std::string_view payload) {
                ExternalGenericDeclMeta out{};
                size_t pos = 0;
                while (pos < payload.size()) {
                    size_t next = payload.find('|', pos);
                    if (next == std::string_view::npos) next = payload.size();
                    const std::string_view part = payload.substr(pos, next - pos);
                    if (part.starts_with("gparam=")) {
                        out.params.push_back(
                            payload_unescape_value_(part.substr(std::string_view("gparam=").size()))
                        );
                    } else if (part.starts_with("impl_proto=")) {
                        const std::string body = payload_unescape_value_(
                            part.substr(std::string_view("impl_proto=").size())
                        );
                        const size_t split = body.find('@');
                        if (split == std::string::npos) {
                            out.impl_protos.emplace_back(body, std::string{});
                        } else {
                            out.impl_protos.emplace_back(body.substr(0, split), body.substr(split + 1));
                        }
                    }
                    if (next == payload.size()) break;
                    pos = next + 1;
                }
                return out;
            };

            auto substitute_external_generic_type_ =
                [&](auto&& self,
                    ty::TypeId cur,
                    const std::unordered_map<std::string, ty::TypeId>& subst,
                    ty::TypePool& pool) -> ty::TypeId {
                    if (cur == ty::kInvalidType || cur >= pool.count()) return cur;
                    const auto& tt = pool.get(cur);
                    switch (tt.kind) {
                        case ty::Kind::kNamedUser: {
                            std::vector<std::string_view> path{};
                            std::vector<ty::TypeId> args{};
                            if (!pool.decompose_named_user(cur, path, args)) return cur;
                            if (path.size() == 1 && args.empty()) {
                                auto it = subst.find(std::string(path.front()));
                                if (it != subst.end()) return it->second;
                            }
                            bool changed = false;
                            for (auto& arg : args) {
                                const auto next = self(self, arg, subst, pool);
                                if (next != arg) {
                                    arg = next;
                                    changed = true;
                                }
                            }
                            if (!changed) return cur;
                            return pool.intern_named_path_with_args(
                                path.data(),
                                static_cast<uint32_t>(path.size()),
                                args.empty() ? nullptr : args.data(),
                                static_cast<uint32_t>(args.size())
                            );
                        }
                        case ty::Kind::kOptional: {
                            const auto elem = self(self, tt.elem, subst, pool);
                            return elem == tt.elem ? cur : pool.make_optional(elem);
                        }
                        case ty::Kind::kBorrow: {
                            const auto elem = self(self, tt.elem, subst, pool);
                            return elem == tt.elem ? cur : pool.make_borrow(elem, tt.borrow_is_mut);
                        }
                        case ty::Kind::kEscape: {
                            const auto elem = self(self, tt.elem, subst, pool);
                            return elem == tt.elem ? cur : pool.make_escape(elem);
                        }
                        case ty::Kind::kPtr: {
                            const auto elem = self(self, tt.elem, subst, pool);
                            return elem == tt.elem ? cur : pool.make_ptr(elem, tt.ptr_is_mut);
                        }
                        case ty::Kind::kArray: {
                            const auto elem = self(self, tt.elem, subst, pool);
                            return elem == tt.elem ? cur : pool.make_array(elem, tt.array_has_size, tt.array_size);
                        }
                        case ty::Kind::kFn: {
                            std::vector<ty::TypeId> params{};
                            std::vector<std::string_view> labels{};
                            std::vector<uint8_t> defaults{};
                            bool changed = false;
                            params.reserve(tt.param_count);
                            labels.reserve(tt.param_count);
                            defaults.reserve(tt.param_count);
                            for (uint32_t i = 0; i < tt.param_count; ++i) {
                                const auto p = pool.fn_param_at(cur, i);
                                const auto np = self(self, p, subst, pool);
                                if (np != p) changed = true;
                                params.push_back(np);
                                labels.push_back(pool.fn_param_label_at(cur, i));
                                defaults.push_back(pool.fn_param_has_default_at(cur, i) ? 1u : 0u);
                            }
                            const auto ret = self(self, tt.ret, subst, pool);
                            if (ret != tt.ret) changed = true;
                            if (!changed) return cur;
                            return pool.make_fn(
                                ret,
                                params.empty() ? nullptr : params.data(),
                                tt.param_count,
                                tt.positional_param_count,
                                labels.empty() ? nullptr : labels.data(),
                                defaults.empty() ? nullptr : defaults.data(),
                                tt.fn_is_c_abi,
                                tt.fn_is_c_variadic,
                                tt.fn_callconv
                            );
                        }
                        default:
                            return cur;
                    }
                };

            const auto expected_proto_type = ast_.stmt(proto_sid).type;
            auto matches_external_symbol = [&](const sema::Symbol& sym) -> bool {
                if (!sym.is_external) return false;
                const bool struct_like_external_type =
                    (sym.kind == sema::SymbolKind::kField) ||
                    (sym.kind == sema::SymbolKind::kType &&
                     (!sym.external_field_payload.empty() ||
                      sym.external_payload.starts_with("parus_field_decl")));
                if (sym.kind != sema::SymbolKind::kType && !struct_like_external_type) return false;
                const std::string_view payload =
                    !sym.external_field_payload.empty()
                        ? std::string_view(sym.external_field_payload)
                        : std::string_view(sym.external_payload);
                if (payload.empty()) return false;
                const auto meta = parse_external_generic_decl_meta_(payload);
                if (meta.impl_protos.empty()) return false;

                std::unordered_map<std::string, ty::TypeId> subst{};
                const size_t bind_n = std::min(meta.params.size(), owner_args.size());
                for (size_t i = 0; i < bind_n; ++i) {
                    subst.emplace(meta.params[i], owner_args[i]);
                }

                for (const auto& [repr, semantic] : meta.impl_protos) {
                    if (repr.empty() && semantic.empty()) continue;
                    auto parsed = parus::cimport::parse_external_type_repr(repr, semantic, payload, types_);
                    if (parsed == ty::kInvalidType) continue;
                    parsed = substitute_external_generic_type_(substitute_external_generic_type_, parsed, subst, types_);
                    if (expected_proto_type != ty::kInvalidType && parsed == expected_proto_type) {
                        return true;
                    }
                    if (auto parsed_proto_sid = resolve_proto_decl_from_type_(parsed, use_span)) {
                        if (proto_decl_matches_constraint_sid_(*parsed_proto_sid, proto_sid)) {
                            return true;
                        }
                    }
                    if (auto expected_proto_sid = resolve_proto_decl_from_type_(expected_proto_type, use_span)) {
                        if (auto parsed_proto_sid = resolve_proto_decl_from_type_(parsed, use_span)) {
                            if (proto_decl_matches_constraint_sid_(*parsed_proto_sid, *expected_proto_sid)) {
                                return true;
                            }
                        }
                    }
                }
                return false;
            };

            std::vector<std::string> lookup_keys{};
            lookup_keys.push_back(owner_base);
            if (auto rewritten = rewrite_imported_path_(lookup_keys.back())) {
                lookup_keys.push_back(*rewritten);
            }
            const auto concrete_name = types_.to_string(type_id);
            if (!concrete_name.empty()) {
                lookup_keys.push_back(concrete_name);
                if (auto rewritten = rewrite_imported_path_(lookup_keys.back())) {
                    lookup_keys.push_back(*rewritten);
                }
            }
            std::sort(lookup_keys.begin(), lookup_keys.end());
            lookup_keys.erase(std::unique(lookup_keys.begin(), lookup_keys.end()), lookup_keys.end());

            for (const auto& key : lookup_keys) {
                if (key.empty()) continue;
                auto sid = sym_.lookup(key);
                if (!sid.has_value()) sid = lookup_symbol_(key);
                if (!sid.has_value()) continue;
                if (matches_external_symbol(sym_.symbol(*sid))) return true;
            }
            return false;
        };
        if (owner_sid == ast::k_invalid_stmt || static_cast<size_t>(owner_sid) >= ast_.stmts().size()) {
            return external_type_declares_proto_(concrete_t);
        }
        const auto& owner = ast_.stmt(owner_sid);
        const auto& refs = ast_.path_refs();
        const uint64_t begin = owner.decl_path_ref_begin;
        const uint64_t end = begin + owner.decl_path_ref_count;
        if (begin > refs.size() || end > refs.size()) return false;
        for (uint32_t i = owner.decl_path_ref_begin; i < owner.decl_path_ref_begin + owner.decl_path_ref_count; ++i) {
            if (auto psid = resolve_proto_decl_from_path_ref_(refs[i], use_span)) {
                if (proto_decl_matches_constraint_sid_(*psid, proto_sid)) return true;
            }
        }
        return false;
    }

    bool TypeChecker::evaluate_generic_constraint_(
        const ast::FnConstraintDecl& cc,
        const std::unordered_map<std::string, ty::TypeId>& bindings,
        Span use_span,
        GenericConstraintFailure& out
    ) {
        out = GenericConstraintFailure{};
        const auto lhs_it = bindings.find(std::string(cc.type_param));
        if (lhs_it == bindings.end()) {
            out.kind = GenericConstraintFailure::Kind::kUnknownTypeParam;
            out.lhs_type_param = std::string(cc.type_param);
            return false;
        }

        if (cc.kind == ast::FnConstraintKind::kProto) {
            ty::TypeId rhs = cc.rhs_type;
            if (rhs == ty::kInvalidType) {
                out.kind = GenericConstraintFailure::Kind::kProtoNotFound;
                out.rhs_proto = "<invalid>";
                return false;
            }
            rhs = substitute_generic_type_(rhs, bindings);
            std::unordered_set<std::string> unresolved{};
            collect_unresolved_generic_param_names_in_proto_target_(rhs, unresolved);
            if (!unresolved.empty()) {
                out.kind = GenericConstraintFailure::Kind::kUnknownTypeParam;
                out.lhs_type_param = *unresolved.begin();
                return false;
            }

            const std::string proto_repr = types_.to_string(rhs);
            const bool proto_is_leaf = proto_repr.find("::") == std::string::npos;
            bool typed_path_failure = false;
            auto proto_sid = resolve_proto_decl_from_type_(rhs, use_span, &typed_path_failure, /*emit_diag=*/false);
            if (!proto_sid.has_value()) {
                if (proto_is_leaf &&
                    builtin_family_proto_satisfied_by_primitive_name_(lhs_it->second, proto_repr)) {
                    return true;
                }
                if (typed_path_failure) {
                    out.kind = GenericConstraintFailure::Kind::kProtoNotFound;
                    out.rhs_proto = proto_repr;
                    return false;
                }
                const size_t pos = proto_repr.rfind("::");
                const std::string_view leaf =
                    (pos == std::string::npos) ? std::string_view(proto_repr)
                                               : std::string_view(proto_repr).substr(pos + 2);
                if (proto_is_leaf &&
                    (leaf == "Comparable" || leaf == "BinaryInteger" || leaf == "SignedInteger" ||
                     leaf == "UnsignedInteger" || leaf == "BinaryFloatingPoint" ||
                     leaf == "Step")) {
                    out.kind = GenericConstraintFailure::Kind::kProtoUnsatisfied;
                    out.lhs_type_param = std::string(cc.type_param);
                    out.rhs_proto = proto_repr;
                    out.concrete_lhs = types_.to_string(lhs_it->second);
                    return false;
                }
                out.kind = GenericConstraintFailure::Kind::kProtoNotFound;
                out.rhs_proto = proto_repr;
                return false;
            }
            if (!type_satisfies_proto_constraint_(lhs_it->second, *proto_sid, use_span)) {
                out.kind = GenericConstraintFailure::Kind::kProtoUnsatisfied;
                out.lhs_type_param = std::string(cc.type_param);
                out.rhs_proto = proto_repr;
                out.concrete_lhs = types_.to_string(lhs_it->second);
                return false;
            }
            return true;
        }

        if (cc.kind == ast::FnConstraintKind::kTypeEq) {
            ty::TypeId rhs = cc.rhs_type;
            if (rhs == ty::kInvalidType) {
                out.kind = GenericConstraintFailure::Kind::kUnknownTypeParam;
                out.lhs_type_param = std::string(cc.type_param);
                out.rhs_type_repr = "<invalid>";
                return false;
            }
            rhs = substitute_generic_type_(rhs, bindings);
            std::unordered_set<std::string> unresolved{};
            collect_unresolved_generic_param_names_in_type_(rhs, unresolved);
            if (!unresolved.empty()) {
                out.kind = GenericConstraintFailure::Kind::kUnknownTypeParam;
                out.lhs_type_param = *unresolved.begin();
                return false;
            }

            const ty::TypeId lhs = canonicalize_transparent_external_typedef_(lhs_it->second);
            rhs = canonicalize_transparent_external_typedef_(rhs);
            if (lhs != rhs) {
                out.kind = GenericConstraintFailure::Kind::kTypeMismatch;
                out.lhs_type_param = std::string(cc.type_param);
                out.rhs_type_repr =
                    (cc.rhs_type != ty::kInvalidType) ? types_.to_string(cc.rhs_type) : std::string("<invalid>");
                out.concrete_lhs = types_.to_string(lhs_it->second);
                out.concrete_rhs = types_.to_string(rhs);
                return false;
            }
            return true;
        }

        return true;
    }

    bool TypeChecker::validate_constraint_clause_decl_(
        uint32_t begin,
        uint32_t count,
        const std::unordered_set<std::string>& generic_params,
        Span owner_span
    ) {
        bool ok = true;
        for (uint32_t ci = 0; ci < count; ++ci) {
            const uint32_t idx = begin + ci;
            if (idx >= ast_.fn_constraint_decls().size()) break;
            auto& c = ast_.fn_constraint_decls_mut()[idx];

            if (generic_params.find(std::string(c.type_param)) == generic_params.end()) {
                diag_(diag::Code::kGenericUnknownTypeParamInConstraint, c.span, c.type_param);
                err_(c.span, "constraint references unknown generic type parameter");
                ok = false;
            }

            if (c.kind == ast::FnConstraintKind::kProto) {
                std::unordered_set<std::string> unresolved{};
                collect_unresolved_generic_param_names_in_proto_target_(c.rhs_type, unresolved);
                for (const auto& name : unresolved) {
                    if (generic_params.find(name) != generic_params.end()) continue;
                    diag_(diag::Code::kGenericUnknownTypeParamInConstraint, c.span, name);
                    err_(c.span, "constraint references unknown generic type parameter");
                    ok = false;
                }

                const std::string proto_repr =
                    (c.rhs_type != ty::kInvalidType) ? types_.to_string(c.rhs_type) : std::string("<invalid>");
                const bool proto_is_leaf = proto_repr.find("::") == std::string::npos;
                bool typed_path_failure = false;
                if (auto proto_sid =
                        resolve_proto_decl_from_type_(c.rhs_type, c.span, &typed_path_failure, /*emit_diag=*/false);
                    proto_sid.has_value()) {
                    const auto& proto_decl = ast_.stmt(*proto_sid);
                    if (proto_decl.type != ty::kInvalidType) {
                        c.rhs_type = proto_decl.type;
                    }
                } else {
                    const size_t pos = proto_repr.rfind("::");
                    const std::string_view leaf =
                        (pos == std::string::npos) ? std::string_view(proto_repr)
                                                   : std::string_view(proto_repr).substr(pos + 2);
                    if (!(proto_is_leaf &&
                          (leaf == "Comparable" || leaf == "BinaryInteger" || leaf == "SignedInteger" ||
                           leaf == "UnsignedInteger" || leaf == "BinaryFloatingPoint" ||
                           leaf == "Step"))) {
                        if (!typed_path_failure) {
                            diag::Diagnostic d(
                                diag::Severity::kError,
                                diag::Code::kGenericConstraintProtoNotFound,
                                c.span
                            );
                            d.add_arg(proto_repr);
                            d.add_note("proto-target import ergonomics only applies to public exported proto targets");
                            d.add_note("this acts constraint could not resolve the referenced proto target");
                            d.add_help("add an explicit import for the proto, or export the proto through a public path");
                            if (diag_bag_) {
                                diag_bag_->add(std::move(d));
                            }
                        }
                        err_(c.span, "unknown proto in generic constraint");
                        ok = false;
                    }
                }
                continue;
            }

            if (c.kind == ast::FnConstraintKind::kTypeEq) {
                std::unordered_set<std::string> unresolved{};
                collect_unresolved_generic_param_names_in_type_(c.rhs_type, unresolved);
                for (const auto& name : unresolved) {
                    if (generic_params.find(name) != generic_params.end()) continue;
                    diag_(diag::Code::kGenericUnknownTypeParamInConstraint, c.span, name);
                    err_(c.span, "constraint references unknown generic type parameter");
                    ok = false;
                }
                continue;
            }
        }
        (void)owner_span;
        return ok;
    }

    void TypeChecker::collect_core_impl_marker_file_ids_(ast::StmtId program_stmt) {
        core_impl_marker_file_ids_ = explicit_core_impl_marker_file_ids_;
        if (program_stmt == ast::k_invalid_stmt || static_cast<size_t>(program_stmt) >= ast_.stmts().size()) {
            return;
        }
        const auto& root = ast_.stmt(program_stmt);
        if (root.kind != ast::StmtKind::kBlock) return;
        const auto& kids = ast_.stmt_children();
        const uint64_t begin = root.stmt_begin;
        const uint64_t end = begin + root.stmt_count;
        if (begin > kids.size() || end > kids.size()) return;

        for (uint32_t i = 0; i < root.stmt_count; ++i) {
            const auto sid = kids[root.stmt_begin + i];
            if (sid == ast::k_invalid_stmt || static_cast<size_t>(sid) >= ast_.stmts().size()) continue;
            const auto& s = ast_.stmt(sid);
            if (!is_core_impl_marker_stmt_(s)) continue;
            core_impl_marker_file_ids_.insert(s.span.file_id);
        }
    }

    std::string TypeChecker::current_bundle_name_() const {
        if (!explicit_current_bundle_name_.empty()) return explicit_current_bundle_name_;
        const auto& syms = sym_.symbols();
        for (const auto& s : syms) {
            if (s.is_external) continue;
            if (!s.decl_bundle_name.empty()) return s.decl_bundle_name;
        }
        return {};
    }

    std::string TypeChecker::bundle_name_for_file_(uint32_t file_id) const {
        if (auto it = explicit_file_bundle_overrides_.find(file_id); it != explicit_file_bundle_overrides_.end()) {
            return it->second;
        }
        return current_bundle_name_();
    }

    bool TypeChecker::enforce_builtin_acts_policy_(const ast::Stmt& acts_decl, ty::TypeId owner_type) {
        if (!acts_decl.acts_is_for) return true;

        ty::Builtin b = ty::Builtin::kNull;
        if (!is_builtin_owner_type_(owner_type, &b)) return true;

        const BuiltinActsPolicy policy = builtin_acts_policy_(b);
        if (!policy.allow_default_acts) {
            std::ostringstream oss;
            oss << "acts for builtin type '" << types_.to_string(owner_type)
                << "' is not supported";
            diag_(diag::Code::kTypeErrorGeneric, acts_decl.span, oss.str());
            err_(acts_decl.span, oss.str());
            return false;
        }

        if (acts_decl.acts_has_set_name && !policy.allow_named_acts) {
            std::ostringstream oss;
            oss << "named acts set is not allowed for builtin type '"
                << types_.to_string(owner_type) << "'";
            diag_(diag::Code::kTypeErrorGeneric, acts_decl.span, oss.str());
            err_(acts_decl.span, oss.str());
            return false;
        }

        if (!acts_decl.acts_has_set_name) {
            auto it = acts_default_decls_by_owner_.find(owner_type);
            if (it != acts_default_decls_by_owner_.end() && it->second.size() > 1) {
                std::ostringstream oss;
                oss << "duplicate default acts declaration for type "
                    << types_.to_string(owner_type);
                diag_(diag::Code::kTypeErrorGeneric, acts_decl.span, oss.str());
                err_(acts_decl.span, oss.str());
                return false;
            }
        }

        const std::string bundle = bundle_name_for_file_(acts_decl.span.file_id);
        const bool bundle_ok =
            policy.reserved_bundle.empty() ||
            bundle == policy.reserved_bundle;
        const bool marker_ok =
            core_impl_marker_file_ids_.find(acts_decl.span.file_id) != core_impl_marker_file_ids_.end();

        if (!bundle_ok || !marker_ok) {
            std::ostringstream oss;
            oss << "acts for builtin type '" << types_.to_string(owner_type)
                << "' requires bundle '" << policy.reserved_bundle
                << "' and file marker '$![Impl::Core];' (current bundle: '"
                << (bundle.empty() ? "<unknown>" : bundle) << "')";
            diag_(diag::Code::kTypeErrorGeneric, acts_decl.span, oss.str());
            err_(acts_decl.span, oss.str());
            return false;
        }

        return true;
    }

    void TypeChecker::push_acts_selection_scope_() {
        acts_selection_scope_stack_.emplace_back();
    }

    void TypeChecker::pop_acts_selection_scope_() {
        if (acts_selection_scope_stack_.empty()) return;
        acts_selection_scope_stack_.pop_back();
    }

    const TypeChecker::ActiveActsSelection*
    TypeChecker::lookup_active_acts_selection_(ty::TypeId owner_type) const {
        if (owner_type == ty::kInvalidType) return nullptr;
        for (auto it = acts_selection_scope_stack_.rbegin(); it != acts_selection_scope_stack_.rend(); ++it) {
            auto hit = it->find(owner_type);
            if (hit != it->end()) return &hit->second;
        }
        return nullptr;
    }

    const TypeChecker::ActiveActsSelection*
    TypeChecker::lookup_symbol_acts_selection_(uint32_t symbol_id) const {
        auto it = acts_selection_by_symbol_.find(symbol_id);
        if (it == acts_selection_by_symbol_.end()) return nullptr;
        return &it->second;
    }

    bool TypeChecker::bind_symbol_acts_selection_(
        uint32_t symbol_id,
        ty::TypeId owner_type,
        const ast::Stmt& var_stmt,
        Span diag_span
    ) {
        if (!var_stmt.var_has_acts_binding) return true;
        owner_type = canonicalize_acts_owner_type_(owner_type);
        if (owner_type == ty::kInvalidType) {
            diag_(diag::Code::kTypeErrorGeneric, diag_span, "binding acts target type is invalid");
            err_(diag_span, "binding acts target type is invalid");
            return false;
        }

        ActiveActsSelection selection{};
        selection.span = diag_span;
        if (var_stmt.var_acts_is_default || var_stmt.var_acts_set_name == "default") {
            selection.kind = ActiveActsSelectionKind::kDefaultOnly;
            selection.named_decl_sid = ast::k_invalid_stmt;
            selection.set_name = "default";
            acts_selection_by_symbol_[symbol_id] = std::move(selection);
            return true;
        }

        std::string raw_set_path;
        if (var_stmt.var_acts_set_path_count > 0) {
            raw_set_path = path_join_(var_stmt.var_acts_set_path_begin, var_stmt.var_acts_set_path_count);
        }
        if (raw_set_path.empty() && !var_stmt.var_acts_set_name.empty()) {
            raw_set_path = std::string(var_stmt.var_acts_set_name);
        }
        if (raw_set_path.empty()) {
            diag_(diag::Code::kTypeErrorGeneric, diag_span, "acts set name is required for binding");
            err_(diag_span, "acts set name is required for binding");
            return false;
        }

        const auto named_sid = resolve_named_acts_decl_sid_(owner_type, raw_set_path);
        if (!named_sid.has_value()) {
            std::ostringstream oss;
            oss << "unknown acts set '" << raw_set_path
                << "' for type " << types_.to_string(owner_type);
            diag_(diag::Code::kTypeErrorGeneric, diag_span, oss.str());
            err_(diag_span, oss.str());
            return false;
        }

        selection.kind = ActiveActsSelectionKind::kNamed;
        selection.named_decl_sid = *named_sid;
        selection.set_name = raw_set_path;
        acts_selection_by_symbol_[symbol_id] = std::move(selection);
        return true;
    }

    std::optional<ast::StmtId>
    TypeChecker::resolve_named_acts_decl_sid_(ty::TypeId owner_type, std::string_view raw_set_path) const {
        owner_type = canonicalize_acts_owner_type_(owner_type);
        if (owner_type == ty::kInvalidType || raw_set_path.empty()) return std::nullopt;

        std::vector<std::string> candidates;
        candidates.reserve(4);
        auto add_candidate = [&](std::string candidate) {
            if (candidate.empty()) return;
            if (std::find(candidates.begin(), candidates.end(), candidate) == candidates.end()) {
                candidates.push_back(std::move(candidate));
            }
        };

        const std::string raw(raw_set_path);
        add_candidate(raw);

        if (auto rewritten = rewrite_imported_path_(raw)) {
            add_candidate(*rewritten);
        }

        if (raw.find("::") == std::string::npos) {
            const std::string qualified = qualify_decl_name_(raw);
            add_candidate(qualified);
            if (auto rewritten_qualified = rewrite_imported_path_(qualified)) {
                add_candidate(*rewritten_qualified);
            }
        }

        for (const auto& candidate : candidates) {
            const auto it = acts_named_decl_by_owner_and_name_.find(
                acts_named_decl_key_(owner_type, candidate));
            if (it != acts_named_decl_by_owner_and_name_.end()) {
                return it->second;
            }
        }
        return std::nullopt;
    }

    bool TypeChecker::apply_use_acts_selection_(const ast::Stmt& use_stmt) {
        if (use_stmt.use_kind != ast::UseKind::kActsEnable) return true;

        ty::TypeId owner_type = canonicalize_acts_owner_type_(use_stmt.acts_target_type);
        if (owner_type == ty::kInvalidType) {
            diag_(diag::Code::kTypeErrorGeneric, use_stmt.span, "acts selection target type is invalid");
            err_(use_stmt.span, "acts selection target type is invalid");
            return false;
        }

        if (acts_selection_scope_stack_.empty()) {
            push_acts_selection_scope_();
        }

        ActiveActsSelection selection{};
        selection.span = use_stmt.span;

        const bool is_default = (use_stmt.use_name == "default");
        if (is_default) {
            selection.kind = ActiveActsSelectionKind::kDefaultOnly;
            selection.named_decl_sid = ast::k_invalid_stmt;
            selection.set_name = "default";
        } else {
            std::string raw_set_path;
            if (use_stmt.use_path_count > 0) {
                raw_set_path = path_join_(use_stmt.use_path_begin, use_stmt.use_path_count);
            }
            if (raw_set_path.empty() && !use_stmt.use_name.empty()) {
                raw_set_path = std::string(use_stmt.use_name);
            }

            if (raw_set_path.empty()) {
                diag_(diag::Code::kTypeErrorGeneric, use_stmt.span, "acts set name is required");
                err_(use_stmt.span, "acts set name is required");
                return false;
            }

            const auto named_sid = resolve_named_acts_decl_sid_(owner_type, raw_set_path);
            if (!named_sid.has_value()) {
                std::ostringstream oss;
                oss << "unknown acts set '" << raw_set_path
                    << "' for type " << types_.to_string(owner_type)
                    << " (declare 'acts " << raw_set_path << " for " << types_.to_string(owner_type)
                    << "' and enable with 'use " << types_.to_string(owner_type)
                    << " with acts(" << raw_set_path << ");')";
                diag_(diag::Code::kTypeErrorGeneric, use_stmt.span, oss.str());
                err_(use_stmt.span, oss.str());
                return false;
            }

            selection.kind = ActiveActsSelectionKind::kNamed;
            selection.named_decl_sid = *named_sid;
            selection.set_name = raw_set_path;
        }

        auto& current_scope = acts_selection_scope_stack_.back();
        auto it = current_scope.find(owner_type);
        if (it != current_scope.end()) {
            const bool same_selection =
                (it->second.kind == selection.kind) &&
                (it->second.named_decl_sid == selection.named_decl_sid);
            if (!same_selection) {
                std::ostringstream oss;
                oss << "conflicting acts selection in same scope for type "
                    << types_.to_string(owner_type);
                diag_(diag::Code::kTypeErrorGeneric, use_stmt.span, oss.str());
                err_(use_stmt.span, oss.str());
                return false;
            }
            return true;
        }

        current_scope.emplace(owner_type, std::move(selection));
        return true;
    }

    /// @brief 실제 파라미터 타입이 acts owner 타입과 호환되는지 판정한다.
    bool TypeChecker::type_matches_acts_owner_(const ty::TypePool& types, ty::TypeId owner, ty::TypeId actual) {
        if (owner == ty::kInvalidType || actual == ty::kInvalidType) return false;
        if (owner == actual) return true;
        const auto& at = types.get(actual);
        auto is_self_named = [&](ty::TypeId t) -> bool {
            std::vector<std::string_view> path{};
            std::vector<ty::TypeId> args{};
            if (!types.decompose_named_user(t, path, args)) return false;
            return path.size() == 1 && path[0] == "Self" && args.empty();
        };
        if (at.kind == ty::Kind::kNamedUser && is_self_named(actual)) {
            return true;
        }
        if (at.kind == ty::Kind::kBorrow) {
            const auto& elem = types.get(at.elem);
            if (elem.kind == ty::Kind::kNamedUser && is_self_named(at.elem)) {
                return true;
            }
            return at.elem == owner;
        }
        return false;
    }

    /// @brief acts decl 하나에서 기본 acts(`acts for T`) operator 멤버를 인덱싱한다.
    void TypeChecker::collect_acts_operator_decl_(ast::StmtId acts_decl_sid, const ast::Stmt& acts_decl, bool allow_named_set) {
        if (!acts_decl.acts_is_for) return;
        if (acts_decl.acts_has_set_name && !allow_named_set) return;
        const ty::TypeId owner_type = canonicalize_acts_owner_type_(acts_decl.acts_target_type);
        if (owner_type == ty::kInvalidType) return;

        const auto& kids = ast_.stmt_children();
        const uint32_t begin = acts_decl.stmt_begin;
        const uint32_t end = acts_decl.stmt_begin + acts_decl.stmt_count;
        if (begin >= kids.size() || end > kids.size()) return;

        for (uint32_t i = begin; i < end; ++i) {
            const ast::StmtId sid = kids[i];
            if (sid == ast::k_invalid_stmt) continue;
            const auto& member = ast_.stmt(sid);
            if (member.kind != ast::StmtKind::kFnDecl || !member.fn_is_operator) continue;

            // 규칙 검증: operator의 첫 파라미터는 self 리시버여야 한다.
            if (member.param_count == 0) {
                diag_(diag::Code::kOperatorSelfFirstParamRequired, member.span);
                err_(member.span, "operator declaration requires a self receiver parameter");
                continue;
            }
            const auto& first = ast_.params()[member.param_begin];
            if (!first.is_self) {
                diag_(diag::Code::kOperatorSelfFirstParamRequired, first.span);
                err_(first.span, "operator declaration requires 'self' on first parameter");
                continue;
            }
            if (!type_matches_acts_owner_(types_, owner_type, first.type)) {
                std::string msg = "operator self type must match acts target type";
                diag_(diag::Code::kTypeErrorGeneric, first.span, msg);
                err_(first.span, msg);
                continue;
            }

            const uint64_t key = acts_operator_key_(
                owner_type,
                member.fn_operator_token,
                member.fn_operator_is_postfix
            );
            acts_default_operator_map_[key].push_back(ActsOperatorDecl{
                .fn_sid = sid,
                .acts_decl_sid = acts_decl_sid,
                .owner_type = owner_type,
                .op_token = member.fn_operator_token,
                .is_postfix = member.fn_operator_is_postfix,
                .from_named_set = acts_decl.acts_has_set_name,
            });
        }
    }

    /// @brief acts decl 하나에서 기본 acts(`acts for T`) 일반 메서드 멤버를 인덱싱한다.
    void TypeChecker::collect_acts_method_decl_(ast::StmtId acts_decl_sid, const ast::Stmt& acts_decl, bool allow_named_set) {
        if (!acts_decl.acts_is_for) return;
        if (acts_decl.acts_has_set_name && !allow_named_set) return;
        const ty::TypeId owner_type = canonicalize_acts_owner_type_(acts_decl.acts_target_type);
        if (owner_type == ty::kInvalidType) return;

        const auto& kids = ast_.stmt_children();
        const uint32_t begin = acts_decl.stmt_begin;
        const uint32_t end = acts_decl.stmt_begin + acts_decl.stmt_count;
        if (begin >= kids.size() || end > kids.size()) return;

        for (uint32_t i = begin; i < end; ++i) {
            const ast::StmtId sid = kids[i];
            if (sid == ast::k_invalid_stmt) continue;
            const auto& member = ast_.stmt(sid);
            if (member.kind != ast::StmtKind::kFnDecl) continue;
            if (member.fn_is_operator) continue;

            bool recv_self = false;
            if (member.param_count > 0) {
                const auto& p0 = ast_.params()[member.param_begin];
                recv_self = p0.is_self;
            }

            acts_default_method_map_[owner_type]
                [std::string(member.name)]
                .push_back(ActsMethodDecl{
                    .fn_sid = sid,
                    .acts_decl_sid = acts_decl_sid,
                    .owner_type = owner_type,
                    .receiver_is_self = recv_self,
                    .from_named_set = acts_decl.acts_has_set_name,
                });
        }
    }

    void TypeChecker::collect_acts_assoc_type_decl_(ast::StmtId acts_decl_sid, const ast::Stmt& acts_decl, bool allow_named_set) {
        if (!acts_decl.acts_is_for) return;
        if (acts_decl.acts_has_set_name && !allow_named_set) return;
        const ty::TypeId owner_type = canonicalize_acts_owner_type_(acts_decl.acts_target_type);
        if (owner_type == ty::kInvalidType) return;

        const auto& witnesses = ast_.acts_assoc_type_witness_decls();
        const uint64_t begin = acts_decl.acts_assoc_witness_begin;
        const uint64_t end = begin + acts_decl.acts_assoc_witness_count;
        if (begin > witnesses.size() || end > witnesses.size()) return;

        for (uint32_t i = 0; i < acts_decl.acts_assoc_witness_count; ++i) {
            const auto& witness = witnesses[acts_decl.acts_assoc_witness_begin + i];
            if (witness.assoc_name.empty() || witness.rhs_type == ty::kInvalidType) continue;

            acts_default_assoc_type_map_[owner_type]
                [std::string(witness.assoc_name)]
                .push_back(ActsAssocTypeDecl{
                    .assoc_sid = ast::k_invalid_stmt,
                    .acts_decl_sid = acts_decl_sid,
                    .owner_type = owner_type,
                    .bound_type = witness.rhs_type,
                    .from_named_set = acts_decl.acts_has_set_name,
                });
        }
    }

    std::vector<TypeChecker::ActsMethodDecl>
    TypeChecker::lookup_acts_methods_for_call_(
        ty::TypeId owner_type,
        std::string_view name,
        const ActiveActsSelection* forced_selection
    ) const {
        std::vector<ActsMethodDecl> out;
        owner_type = canonicalize_acts_owner_type_(owner_type);
        if (owner_type == ty::kInvalidType || name.empty()) return out;

        auto owner_base_name_matches = [](std::string_view lhs, std::string_view rhs) -> bool {
            if (lhs == rhs) return true;
            auto suffix_match = [](std::string_view full, std::string_view suffix) -> bool {
                if (full.size() <= suffix.size() + 2u) return false;
                if (!full.ends_with(suffix)) return false;
                const size_t split = full.size() - suffix.size();
                return full[split - 1] == ':' && full[split - 2] == ':';
            };
            return suffix_match(lhs, rhs) || suffix_match(rhs, lhs);
        };

        auto owner_types_match = [&](ty::TypeId lhs, ty::TypeId rhs) -> bool {
            lhs = canonicalize_acts_owner_type_(lhs);
            rhs = canonicalize_acts_owner_type_(rhs);
            if (lhs == rhs) return true;
            if (canonicalize_transparent_external_typedef_(lhs) ==
                canonicalize_transparent_external_typedef_(rhs)) {
                return true;
            }

            std::string lhs_base{};
            std::string rhs_base{};
            std::vector<ty::TypeId> lhs_args{};
            std::vector<ty::TypeId> rhs_args{};
            if (decompose_named_user_type_(lhs, lhs_base, lhs_args) &&
                decompose_named_user_type_(rhs, rhs_base, rhs_args) &&
                owner_base_name_matches(lhs_base, rhs_base) &&
                lhs_args.size() == rhs_args.size()) {
                bool all_same = true;
                for (size_t i = 0; i < lhs_args.size(); ++i) {
                    if (canonicalize_transparent_external_typedef_(lhs_args[i]) !=
                        canonicalize_transparent_external_typedef_(rhs_args[i])) {
                        all_same = false;
                        break;
                    }
                }
                if (all_same) return true;
            }

            return types_.to_string(lhs) == types_.to_string(rhs);
        };

        std::vector<ActsMethodDecl> matching_decls{};
        if (auto oit = acts_default_method_map_.find(owner_type);
            oit != acts_default_method_map_.end()) {
            if (auto mit = oit->second.find(std::string(name)); mit != oit->second.end()) {
                matching_decls.insert(matching_decls.end(), mit->second.begin(), mit->second.end());
            }
        }
        if (matching_decls.empty()) {
            for (const auto& [candidate_owner, member_map] : acts_default_method_map_) {
                if (!owner_types_match(candidate_owner, owner_type)) continue;
                auto mit = member_map.find(std::string(name));
                if (mit == member_map.end()) continue;
                matching_decls.insert(matching_decls.end(), mit->second.begin(), mit->second.end());
            }
        }
        if (matching_decls.empty()) return out;

        const auto* active = forced_selection ? forced_selection : lookup_active_acts_selection_(owner_type);
        if (active == nullptr || active->kind == ActiveActsSelectionKind::kDefaultOnly) {
            for (const auto& d : matching_decls) {
                if (!d.from_named_set) out.push_back(d);
            }
            return out;
        }

        for (const auto& d : matching_decls) {
            if (d.from_named_set && d.acts_decl_sid == active->named_decl_sid) {
                out.push_back(d);
            }
        }
        for (const auto& d : matching_decls) {
            if (!d.from_named_set) out.push_back(d);
        }
        return out;
    }

    std::vector<TypeChecker::ExternalActsMethodDecl>
    TypeChecker::lookup_external_acts_methods_for_call_(
        ty::TypeId concrete_owner_type,
        std::string_view member_name
    ) const {
        std::vector<ExternalActsMethodDecl> out;
        concrete_owner_type = canonicalize_acts_owner_type_(concrete_owner_type);
        if (concrete_owner_type == ty::kInvalidType || member_name.empty()) return out;
        const std::string concrete_owner_export = types_.to_export_string(concrete_owner_type);

        auto owner_base_name_matches = [](std::string_view lhs, std::string_view rhs) -> bool {
            if (lhs == rhs) return true;
            auto suffix_match = [](std::string_view full, std::string_view suffix) -> bool {
                if (full.size() <= suffix.size() + 2u) return false;
                if (!full.ends_with(suffix)) return false;
                const size_t split = full.size() - suffix.size();
                return full[split - 1] == ':' && full[split - 2] == ':';
            };
            return suffix_match(lhs, rhs) || suffix_match(rhs, lhs);
        };

        std::string concrete_owner_base{};
        std::vector<ty::TypeId> concrete_owner_args{};
        const bool have_concrete_owner_base =
            decompose_named_user_type_(concrete_owner_type, concrete_owner_base, concrete_owner_args) &&
            !concrete_owner_base.empty();
        auto parse_external_generic_decl_meta_ = [](std::string_view payload) {
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
        auto type_contains_meta_generic_ =
            [&](auto&& self, ty::TypeId t, const std::unordered_set<std::string>& generic_names) -> bool {
                if (t == ty::kInvalidType || t >= types_.count()) return false;
                const auto& tt = types_.get(t);
                switch (tt.kind) {
                    case ty::Kind::kNamedUser: {
                        std::vector<std::string_view> path{};
                        std::vector<ty::TypeId> args{};
                        if (!types_.decompose_named_user(t, path, args) || path.empty()) return false;
                        if (args.empty() && path.size() == 1 &&
                            generic_names.find(std::string(path.front())) != generic_names.end()) {
                            return true;
                        }
                        for (const auto arg : args) {
                            if (self(self, arg, generic_names)) return true;
                        }
                        return false;
                    }
                    case ty::Kind::kOptional:
                    case ty::Kind::kArray:
                    case ty::Kind::kBorrow:
                    case ty::Kind::kEscape:
                    case ty::Kind::kPtr:
                        return self(self, tt.elem, generic_names);
                    case ty::Kind::kFn:
                        if (self(self, tt.ret, generic_names)) return true;
                        for (uint32_t i = 0; i < tt.param_count; ++i) {
                            if (self(self, types_.fn_param_at(t, i), generic_names)) return true;
                        }
                        return false;
                    default:
                        return false;
                }
            };

        std::unordered_set<uint32_t> seen{};
        for (uint32_t sid = 0; sid < sym_.symbols().size(); ++sid) {
            const auto& sym = sym_.symbol(sid);
            if (!sym.is_external || sym.kind != sema::SymbolKind::kFn) continue;
            if (sym.declared_type == ty::kInvalidType || sym.declared_type >= types_.count()) continue;
            const auto& fn_t = types_.get(sym.declared_type);
            if (fn_t.kind != ty::Kind::kFn || fn_t.param_count == 0) continue;

            ty::TypeId owner_t = ty::kInvalidType;
            std::string parsed_member_name{};
            bool receiver_is_self = false;
            if (parse_external_builtin_acts_payload_(sym.external_payload, owner_t, parsed_member_name, receiver_is_self)) {
                if (parsed_member_name != member_name) continue;
                if (owner_t == concrete_owner_type && seen.insert(sid).second) {
                    ExternalActsMethodDecl md{};
                    md.fn_symbol = sid;
                    md.owner_type = owner_t;
                    md.receiver_is_self = receiver_is_self;
                    md.external_payload = sym.external_payload;
                    out.push_back(std::move(md));
                }
                continue;
            }

            const std::string_view full_name = sym.name;
            const size_t split = full_name.rfind("::");
            if (split == std::string_view::npos) continue;
            const std::string_view parsed_name = full_name.substr(split + 2);
            if (parsed_name != member_name) continue;

            owner_t = types_.fn_param_at(sym.declared_type, 0);
            if (owner_t == ty::kInvalidType || owner_t >= types_.count()) continue;
            const auto& owner_tt = types_.get(owner_t);
            if (owner_tt.kind == ty::Kind::kBorrow) owner_t = owner_tt.elem;
            owner_t = canonicalize_acts_owner_type_(owner_t);
            if (owner_t == ty::kInvalidType) continue;

            ExternalActsMethodDecl md{};
            md.fn_symbol = sid;
            md.owner_type = owner_t;
            md.receiver_is_self = true;
            md.external_payload = sym.external_payload;

            const auto meta = parse_external_generic_decl_meta_(sym.external_payload);
            std::unordered_set<std::string> meta_generic_names(meta.params.begin(), meta.params.end());
            std::string template_owner_base{};
            std::vector<ty::TypeId> template_owner_args{};
            const bool owner_contains_named_meta_generic =
                !meta_generic_names.empty() &&
                type_contains_meta_generic_(type_contains_meta_generic_, owner_t, meta_generic_names);
            const bool owner_has_template_shape =
                decompose_named_user_type_(owner_t, template_owner_base, template_owner_args) &&
                !template_owner_base.empty() &&
                !template_owner_args.empty() &&
                meta.params.size() >= template_owner_args.size();
            const bool owner_uses_meta_generics =
                owner_contains_named_meta_generic || owner_has_template_shape;
            if (owner_uses_meta_generics) {
                if (!have_concrete_owner_base) continue;
                if (!owner_has_template_shape) {
                    continue;
                }
                if (!owner_base_name_matches(concrete_owner_base, template_owner_base) ||
                    template_owner_args.size() != concrete_owner_args.size()) {
                    continue;
                }
                md.owner_is_generic_template = true;
                md.owner_generic_arity = static_cast<uint32_t>(template_owner_args.size());
                md.owner_base = template_owner_base;
                if (seen.insert(sid).second) {
                    out.push_back(std::move(md));
                }
                continue;
            }

            const std::string owner_export = types_.to_export_string(owner_t);
            const bool owner_matches =
                owner_t == concrete_owner_type ||
                owner_export == concrete_owner_export ||
                owner_base_name_matches(owner_export, concrete_owner_export);
            if (owner_matches && seen.insert(sid).second) {
                out.push_back(std::move(md));
            }
        }

        return out;
    }

    std::optional<ty::TypeId>
    TypeChecker::lookup_acts_assoc_type_binding_(
        ty::TypeId owner_type,
        std::string_view name,
        const ActiveActsSelection* forced_selection
    ) const {
        owner_type = canonicalize_acts_owner_type_(owner_type);
        if (owner_type == ty::kInvalidType || name.empty()) return std::nullopt;

        auto lookup_local = [&](ty::TypeId concrete_owner) -> std::optional<ty::TypeId> {
            auto oit = acts_default_assoc_type_map_.find(concrete_owner);
            if (oit == acts_default_assoc_type_map_.end()) return std::nullopt;
            auto ait = oit->second.find(std::string(name));
            if (ait == oit->second.end()) return std::nullopt;

            const auto* active = forced_selection ? forced_selection : lookup_active_acts_selection_(concrete_owner);
            if (active != nullptr && active->kind == ActiveActsSelectionKind::kNamed) {
                for (const auto& d : ait->second) {
                    if (d.from_named_set && d.acts_decl_sid == active->named_decl_sid) {
                        return d.bound_type;
                    }
                }
            }
            for (const auto& d : ait->second) {
                if (!d.from_named_set) return d.bound_type;
            }
            return std::nullopt;
        };

        if (auto local = lookup_local(owner_type)) return local;

        auto lookup_external = [&](ty::TypeId concrete_owner) -> std::optional<ty::TypeId> {
            auto oit = external_acts_default_assoc_type_map_.find(concrete_owner);
            if (oit == external_acts_default_assoc_type_map_.end()) return std::nullopt;
            auto ait = oit->second.find(std::string(name));
            if (ait == oit->second.end() || ait->second.empty()) return std::nullopt;
            return ait->second.front().bound_type;
        };

        if (auto external = lookup_external(owner_type)) return external;

        std::string owner_base{};
        std::vector<ty::TypeId> owner_args{};
        if (!decompose_named_user_type_(owner_type, owner_base, owner_args) || owner_base.empty()) {
            return std::nullopt;
        }

        auto substitute_external_generic_type_ =
            [&](auto&& self,
                ty::TypeId cur,
                const std::unordered_map<std::string, ty::TypeId>& subst,
                ty::TypePool& pool) -> ty::TypeId {
                if (cur == ty::kInvalidType || cur >= pool.count()) return cur;
                const auto& tt = pool.get(cur);
                switch (tt.kind) {
                    case ty::Kind::kNamedUser: {
                        std::vector<std::string_view> path{};
                        std::vector<ty::TypeId> args{};
                        if (!pool.decompose_named_user(cur, path, args)) return cur;
                        if (path.size() == 1 && args.empty()) {
                            auto it = subst.find(std::string(path.front()));
                            if (it != subst.end()) return it->second;
                        }
                        bool changed = false;
                        for (auto& arg : args) {
                            const auto next = self(self, arg, subst, pool);
                            if (next != arg) {
                                arg = next;
                                changed = true;
                            }
                        }
                        if (!changed) return cur;
                        return pool.intern_named_path_with_args(
                            path.data(),
                            static_cast<uint32_t>(path.size()),
                            args.empty() ? nullptr : args.data(),
                            static_cast<uint32_t>(args.size())
                        );
                    }
                    case ty::Kind::kOptional: {
                        const auto elem = self(self, tt.elem, subst, pool);
                        return elem == tt.elem ? cur : pool.make_optional(elem);
                    }
                    case ty::Kind::kBorrow: {
                        const auto elem = self(self, tt.elem, subst, pool);
                        return elem == tt.elem ? cur : pool.make_borrow(elem, tt.borrow_is_mut);
                    }
                    case ty::Kind::kEscape: {
                        const auto elem = self(self, tt.elem, subst, pool);
                        return elem == tt.elem ? cur : pool.make_escape(elem);
                    }
                    case ty::Kind::kPtr: {
                        const auto elem = self(self, tt.elem, subst, pool);
                        return elem == tt.elem ? cur : pool.make_ptr(elem, tt.ptr_is_mut);
                    }
                    case ty::Kind::kArray: {
                        const auto elem = self(self, tt.elem, subst, pool);
                        return elem == tt.elem ? cur : pool.make_array(elem, tt.array_has_size, tt.array_size);
                    }
                    case ty::Kind::kFn: {
                        std::vector<ty::TypeId> params{};
                        std::vector<std::string_view> labels{};
                        std::vector<uint8_t> defaults{};
                        bool changed = false;
                        params.reserve(tt.param_count);
                        labels.reserve(tt.param_count);
                        defaults.reserve(tt.param_count);
                        for (uint32_t i = 0; i < tt.param_count; ++i) {
                            const auto p = pool.fn_param_at(cur, i);
                            const auto np = self(self, p, subst, pool);
                            if (np != p) changed = true;
                            params.push_back(np);
                            labels.push_back(pool.fn_param_label_at(cur, i));
                            defaults.push_back(pool.fn_param_has_default_at(cur, i) ? 1u : 0u);
                        }
                        const auto ret = self(self, tt.ret, subst, pool);
                        if (ret != tt.ret) changed = true;
                        if (!changed) return cur;
                        return pool.make_fn(
                            ret,
                            params.empty() ? nullptr : params.data(),
                            tt.param_count,
                            tt.positional_param_count,
                            labels.empty() ? nullptr : labels.data(),
                            defaults.empty() ? nullptr : defaults.data(),
                            tt.fn_is_c_abi,
                            tt.fn_is_c_variadic,
                            tt.fn_callconv
                        );
                    }
                    default:
                        return cur;
                }
            };

        auto owner_base_name_matches = [](std::string_view lhs, std::string_view rhs) -> bool {
            if (lhs == rhs) return true;
            auto suffix_match = [](std::string_view full, std::string_view suffix) -> bool {
                if (full.size() <= suffix.size() + 2u) return false;
                if (!full.ends_with(suffix)) return false;
                const size_t split = full.size() - suffix.size();
                return full[split - 1] == ':' && full[split - 2] == ':';
            };
            return suffix_match(lhs, rhs) || suffix_match(rhs, lhs);
        };

        auto parse_external_template_params_ = [&](std::string_view payload) {
            std::vector<std::string> params{};
            size_t pos = 0;
            while (pos < payload.size()) {
                size_t next = payload.find('|', pos);
                if (next == std::string_view::npos) next = payload.size();
                const std::string_view part = payload.substr(pos, next - pos);
                if (part.starts_with("gparam=")) {
                    params.push_back(payload_unescape_value_(
                        part.substr(std::string_view("gparam=").size())
                    ));
                }
                if (next == payload.size()) break;
                pos = next + 1;
            }
            return params;
        };

        std::vector<std::string> lookup_keys{};
        lookup_keys.push_back(owner_base);
        if (auto rewritten = rewrite_imported_path_(owner_base)) {
            lookup_keys.push_back(*rewritten);
        }
        std::sort(lookup_keys.begin(), lookup_keys.end());
        lookup_keys.erase(std::unique(lookup_keys.begin(), lookup_keys.end()), lookup_keys.end());

        for (const auto& key : lookup_keys) {
            for (const auto& [template_owner_base, assoc_map] : external_acts_template_assoc_type_map_) {
                if (!owner_base_name_matches(key, template_owner_base)) continue;
                auto ait = assoc_map.find(std::string(name));
                if (ait == assoc_map.end()) continue;

                for (const auto& d : ait->second) {
                    if (!d.owner_is_generic_template) continue;
                    if (d.owner_generic_arity != owner_args.size()) continue;
                    std::unordered_map<std::string, ty::TypeId> subst{};
                    std::vector<std::string_view> templ_path{};
                    std::vector<ty::TypeId> templ_args{};
                    if (types_.decompose_named_user(d.owner_type, templ_path, templ_args) &&
                        templ_args.size() == owner_args.size()) {
                        for (size_t i = 0; i < templ_args.size(); ++i) {
                            std::vector<std::string_view> arg_path{};
                            std::vector<ty::TypeId> arg_args{};
                            if (!types_.decompose_named_user(templ_args[i], arg_path, arg_args) ||
                                arg_path.size() != 1 ||
                                !arg_args.empty()) {
                                subst.clear();
                                break;
                            }
                            subst.emplace(std::string(arg_path.front()), owner_args[i]);
                        }
                    } else {
                        const auto params = parse_external_template_params_(d.external_payload);
                        if (params.size() != owner_args.size()) continue;
                        for (size_t i = 0; i < params.size(); ++i) {
                            subst.emplace(params[i], owner_args[i]);
                        }
                    }
                    if (subst.size() != owner_args.size()) continue;
                    const ty::TypeId substituted = substitute_external_generic_type_(
                        substitute_external_generic_type_,
                        d.bound_type,
                        subst,
                        types_
                    );
                    return substituted;
                }
            }
        }
        return std::nullopt;
    }

    bool TypeChecker::is_self_assoc_named_type_(ty::TypeId t, std::string* out_name) const {
        std::vector<std::string_view> path{};
        std::vector<ty::TypeId> args{};
        if (!types_.decompose_named_user(t, path, args)) return false;
        if (path.size() != 2 || path[0] != "Self" || !args.empty()) return false;
        if (out_name != nullptr) *out_name = std::string(path[1]);
        return true;
    }

    ty::TypeId TypeChecker::substitute_self_and_assoc_type_(
        ty::TypeId t,
        ty::TypeId owner_type,
        const std::unordered_map<std::string, ty::TypeId>* assoc_bindings
    ) const {
        auto subst_assoc = [&](std::string_view name) -> ty::TypeId {
            if (assoc_bindings != nullptr) {
                auto it = assoc_bindings->find(std::string(name));
                if (it != assoc_bindings->end()) return it->second;
            }
            if (auto bound = lookup_acts_assoc_type_binding_(owner_type, name)) {
                return *bound;
            }
            return ty::kInvalidType;
        };

        auto self = [&](auto&& self, ty::TypeId cur) -> ty::TypeId {
            if (cur == ty::kInvalidType) return cur;
            const auto& tt = types_.get(cur);
            switch (tt.kind) {
                case ty::Kind::kNamedUser: {
                    if (is_self_named_type_(cur)) return owner_type;
                    std::string assoc_name{};
                    if (is_self_assoc_named_type_(cur, &assoc_name)) {
                        const ty::TypeId bound = subst_assoc(assoc_name);
                        return bound == ty::kInvalidType ? cur : bound;
                    }
                    std::vector<std::string_view> path{};
                    std::vector<ty::TypeId> args{};
                    if (!types_.decompose_named_user(cur, path, args) || args.empty()) return cur;
                    bool changed = false;
                    for (auto& arg : args) {
                        const auto next = self(self, arg);
                        if (next != arg) {
                            arg = next;
                            changed = true;
                        }
                    }
                    if (!changed) return cur;
                    return types_.intern_named_path_with_args(
                        path.data(),
                        static_cast<uint32_t>(path.size()),
                        args.empty() ? nullptr : args.data(),
                        static_cast<uint32_t>(args.size())
                    );
                }
                case ty::Kind::kOptional: {
                    const auto elem = self(self, tt.elem);
                    return elem == tt.elem ? cur : types_.make_optional(elem);
                }
                case ty::Kind::kBorrow: {
                    const auto elem = self(self, tt.elem);
                    return elem == tt.elem ? cur : types_.make_borrow(elem, tt.borrow_is_mut);
                }
                case ty::Kind::kEscape: {
                    const auto elem = self(self, tt.elem);
                    return elem == tt.elem ? cur : types_.make_escape(elem);
                }
                case ty::Kind::kPtr: {
                    const auto elem = self(self, tt.elem);
                    return elem == tt.elem ? cur : types_.make_ptr(elem, tt.ptr_is_mut);
                }
                case ty::Kind::kArray: {
                    const auto elem = self(self, tt.elem);
                    return elem == tt.elem ? cur : types_.make_array(elem, tt.array_has_size, tt.array_size);
                }
                case ty::Kind::kFn: {
                    std::vector<ty::TypeId> params{};
                    std::vector<std::string_view> labels{};
                    std::vector<uint8_t> defaults{};
                    bool changed = false;
                    params.reserve(tt.param_count);
                    labels.reserve(tt.param_count);
                    defaults.reserve(tt.param_count);
                    for (uint32_t i = 0; i < tt.param_count; ++i) {
                        const auto p = types_.fn_param_at(cur, i);
                        const auto np = self(self, p);
                        if (np != p) changed = true;
                        params.push_back(np);
                        labels.push_back(types_.fn_param_label_at(cur, i));
                        defaults.push_back(types_.fn_param_has_default_at(cur, i) ? 1u : 0u);
                    }
                    const auto ret = self(self, tt.ret);
                    if (ret != tt.ret) changed = true;
                    if (!changed) return cur;
                    return types_.make_fn(
                        ret,
                        params.empty() ? nullptr : params.data(),
                        tt.param_count,
                        tt.positional_param_count,
                        labels.empty() ? nullptr : labels.data(),
                        defaults.empty() ? nullptr : defaults.data(),
                        tt.fn_is_c_abi,
                        tt.fn_is_c_variadic,
                        tt.fn_callconv
                    );
                }
                default:
                    return cur;
            }
        };

        return self(self, t);
    }

    bool TypeChecker::collect_assoc_type_bindings_for_owner_(
        ty::TypeId owner_type,
        std::unordered_map<std::string, ty::TypeId>& out,
        const ActiveActsSelection* forced_selection
    ) const {
        out.clear();
        owner_type = canonicalize_acts_owner_type_(owner_type);
        if (owner_type == ty::kInvalidType) return false;

        auto oit = acts_default_assoc_type_map_.find(owner_type);
        if (oit != acts_default_assoc_type_map_.end()) {
            const auto* active = forced_selection ? forced_selection : lookup_active_acts_selection_(owner_type);
            for (const auto& [name, decls] : oit->second) {
                if (active != nullptr && active->kind == ActiveActsSelectionKind::kNamed) {
                    for (const auto& d : decls) {
                        if (d.from_named_set && d.acts_decl_sid == active->named_decl_sid) {
                            out[name] = d.bound_type;
                            break;
                        }
                    }
                }
                if (out.find(name) != out.end()) continue;
                for (const auto& d : decls) {
                    if (!d.from_named_set) {
                        out[name] = d.bound_type;
                        break;
                    }
                }
            }
        }

        auto eout = external_acts_default_assoc_type_map_.find(owner_type);
        if (eout != external_acts_default_assoc_type_map_.end()) {
            for (const auto& [name, decls] : eout->second) {
                if (out.find(name) != out.end() || decls.empty()) continue;
                out[name] = decls.front().bound_type;
            }
        }

        for (const auto& [_, assoc_map] : external_acts_template_assoc_type_map_) {
            for (const auto& [name, decls] : assoc_map) {
                (void)decls;
                if (out.find(name) != out.end()) continue;
                if (auto bound = lookup_acts_assoc_type_binding_(owner_type, name, forced_selection)) {
                    out.emplace(name, *bound);
                }
            }
        }

        return !out.empty();
    }

    /// @brief binary operator에 대응되는 기본 acts overload를 찾는다.
    ast::StmtId TypeChecker::resolve_binary_operator_overload_(
        syntax::TokenKind op,
        ty::TypeId lhs,
        ty::TypeId rhs,
        const ActiveActsSelection* forced_selection
    ) const {
        lhs = canonicalize_acts_owner_type_(lhs);
        const uint64_t key = acts_operator_key_(lhs, op, /*is_postfix=*/false);
        auto it = acts_default_operator_map_.find(key);
        if (it == acts_default_operator_map_.end()) return ast::k_invalid_stmt;

        auto match_one = [&](const ActsOperatorDecl& decl) -> bool {
            const auto& def = ast_.stmt(decl.fn_sid);
            if (def.kind != ast::StmtKind::kFnDecl) return false;
            if (def.param_count < 2) return false;
            const auto& p0 = ast_.params()[def.param_begin + 0];
            const auto& p1 = ast_.params()[def.param_begin + 1];
            return can_assign_(p0.type, lhs) && can_assign_(p1.type, rhs);
        };

        auto select_from = [&](bool named_stage, ast::StmtId named_sid, bool& ambiguous) -> ast::StmtId {
            ambiguous = false;
            ast::StmtId selected = ast::k_invalid_stmt;
            for (const auto& decl : it->second) {
                if (named_stage) {
                    if (!decl.from_named_set || decl.acts_decl_sid != named_sid) continue;
                } else {
                    if (decl.from_named_set) continue;
                }
                if (!match_one(decl)) continue;
                if (selected != ast::k_invalid_stmt) {
                    ambiguous = true;
                    return ast::k_invalid_stmt;
                }
                selected = decl.fn_sid;
            }
            return selected;
        };

        const auto* active = forced_selection ? forced_selection : lookup_active_acts_selection_(lhs);
        if (active != nullptr && active->kind == ActiveActsSelectionKind::kNamed) {
            bool amb_named = false;
            const ast::StmtId named = select_from(/*named_stage=*/true, active->named_decl_sid, amb_named);
            if (amb_named) return ast::k_invalid_stmt;
            if (named != ast::k_invalid_stmt) return named;
        }

        bool amb_default = false;
        const ast::StmtId def = select_from(/*named_stage=*/false, ast::k_invalid_stmt, amb_default);
        if (amb_default) return ast::k_invalid_stmt;
        return def;
    }

    /// @brief prefix operator(copy/clone 등)에 대응되는 기본 acts overload를 찾는다.
    ast::StmtId TypeChecker::resolve_prefix_operator_overload_(
        syntax::TokenKind op,
        ty::TypeId lhs,
        const ActiveActsSelection* forced_selection
    ) const {
        lhs = canonicalize_acts_owner_type_(lhs);
        const uint64_t key = acts_operator_key_(lhs, op, /*is_postfix=*/false);
        auto it = acts_default_operator_map_.find(key);
        if (it == acts_default_operator_map_.end()) return ast::k_invalid_stmt;

        auto match_one = [&](const ActsOperatorDecl& decl) -> bool {
            const auto& def = ast_.stmt(decl.fn_sid);
            if (def.kind != ast::StmtKind::kFnDecl) return false;
            if (def.param_count != 1) return false;
            const auto& p0 = ast_.params()[def.param_begin + 0];
            return type_matches_acts_owner_(types_, lhs, p0.type);
        };

        auto select_from = [&](bool named_stage, ast::StmtId named_sid, bool& ambiguous) -> ast::StmtId {
            ambiguous = false;
            ast::StmtId selected = ast::k_invalid_stmt;
            for (const auto& decl : it->second) {
                if (named_stage) {
                    if (!decl.from_named_set || decl.acts_decl_sid != named_sid) continue;
                } else {
                    if (decl.from_named_set) continue;
                }
                if (!match_one(decl)) continue;
                if (selected != ast::k_invalid_stmt) {
                    ambiguous = true;
                    return ast::k_invalid_stmt;
                }
                selected = decl.fn_sid;
            }
            return selected;
        };

        const auto* active = forced_selection ? forced_selection : lookup_active_acts_selection_(lhs);
        if (active != nullptr && active->kind == ActiveActsSelectionKind::kNamed) {
            bool amb_named = false;
            const ast::StmtId named = select_from(/*named_stage=*/true, active->named_decl_sid, amb_named);
            if (amb_named) return ast::k_invalid_stmt;
            if (named != ast::k_invalid_stmt) return named;
        }

        bool amb_default = false;
        const ast::StmtId def = select_from(/*named_stage=*/false, ast::k_invalid_stmt, amb_default);
        if (amb_default) return ast::k_invalid_stmt;
        return def;
    }

    /// @brief postfix operator(++ 등)에 대응되는 기본 acts overload를 찾는다.
    ast::StmtId TypeChecker::resolve_postfix_operator_overload_(
        syntax::TokenKind op,
        ty::TypeId lhs,
        const ActiveActsSelection* forced_selection
    ) const {
        lhs = canonicalize_acts_owner_type_(lhs);
        const uint64_t key = acts_operator_key_(lhs, op, /*is_postfix=*/true);
        auto it = acts_default_operator_map_.find(key);
        if (it == acts_default_operator_map_.end()) return ast::k_invalid_stmt;

        auto match_one = [&](const ActsOperatorDecl& decl) -> bool {
            const auto& def = ast_.stmt(decl.fn_sid);
            if (def.kind != ast::StmtKind::kFnDecl) return false;
            if (def.param_count < 1) return false;
            const auto& p0 = ast_.params()[def.param_begin + 0];
            return can_assign_(p0.type, lhs);
        };

        auto select_from = [&](bool named_stage, ast::StmtId named_sid, bool& ambiguous) -> ast::StmtId {
            ambiguous = false;
            ast::StmtId selected = ast::k_invalid_stmt;
            for (const auto& decl : it->second) {
                if (named_stage) {
                    if (!decl.from_named_set || decl.acts_decl_sid != named_sid) continue;
                } else {
                    if (decl.from_named_set) continue;
                }
                if (!match_one(decl)) continue;
                if (selected != ast::k_invalid_stmt) {
                    ambiguous = true;
                    return ast::k_invalid_stmt;
                }
                selected = decl.fn_sid;
            }
            return selected;
        };

        const auto* active = forced_selection ? forced_selection : lookup_active_acts_selection_(lhs);
        if (active != nullptr && active->kind == ActiveActsSelectionKind::kNamed) {
            bool amb_named = false;
            const ast::StmtId named = select_from(/*named_stage=*/true, active->named_decl_sid, amb_named);
            if (amb_named) return ast::k_invalid_stmt;
            if (named != ast::k_invalid_stmt) return named;
        }

        bool amb_default = false;
        const ast::StmtId def = select_from(/*named_stage=*/false, ast::k_invalid_stmt, amb_default);
        if (amb_default) return ast::k_invalid_stmt;
        return def;
    }

    bool TypeChecker::fits_builtin_int_big_(const parus::num::BigInt& v, parus::ty::Builtin dst) {
        using B = parus::ty::Builtin;
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
            case B::kCChar:
            case B::kCSChar:
                return v.fits_i8();
            case B::kCUChar:
                return v.fits_u8();
            case B::kCShort:
                return v.fits_i16();
            case B::kCUShort:
                return v.fits_u16();
            case B::kCInt:
                return v.fits_i32();
            case B::kCUInt:
                return v.fits_u32();
            case B::kCLong:
            case B::kCLongLong:
                return v.fits_i64();
            case B::kCULong:
            case B::kCULongLong:
                return v.fits_u64();
            case B::kCSize:
                return v.fits_u64();
            case B::kCSSize:
            case B::kCPtrDiff:
                return v.fits_i64();

            default: return false;
        }
    }

    /// @brief field 멤버로 허용할 POD 값 내장 타입인지 판정한다.
    bool TypeChecker::is_field_pod_value_type_(const ty::TypePool& types, ty::TypeId id) {
        if (id == ty::kInvalidType) return false;
        const auto& t = types.get(id);
        if (t.kind == ty::Kind::kOptional) {
            // non-layout(c) field policy: Optional<POD> and Optional<~T> are allowed.
            return is_field_pod_value_type_(types, t.elem);
        }
        if (t.kind == ty::Kind::kArray) {
            if (!t.array_has_size) return false;
            return is_field_pod_value_type_(types, t.elem);
        }
        if (t.kind == ty::Kind::kPtr) {
            return t.elem != ty::kInvalidType;
        }
        if (t.kind == ty::Kind::kEscape) {
            return t.elem != ty::kInvalidType;
        }
        if (t.kind != ty::Kind::kBuiltin) return false;

        using B = ty::Builtin;
        switch (t.builtin) {
            case B::kBool:
            case B::kChar:
            case B::kI8:
            case B::kI16:
            case B::kI32:
            case B::kI64:
            case B::kI128:
            case B::kU8:
            case B::kU16:
            case B::kU32:
            case B::kU64:
            case B::kU128:
            case B::kISize:
            case B::kUSize:
            case B::kF32:
            case B::kF64:
            case B::kF128:
            case B::kCVoid:
            case B::kCChar:
            case B::kCSChar:
            case B::kCUChar:
            case B::kCShort:
            case B::kCUShort:
            case B::kCInt:
            case B::kCUInt:
            case B::kCLong:
            case B::kCULong:
            case B::kCLongLong:
            case B::kCULongLong:
            case B::kCFloat:
            case B::kCDouble:
            case B::kCSize:
            case B::kCSSize:
            case B::kCPtrDiff:
            case B::kVaList:
                return true;

            case B::kNull:
            case B::kUnit:
            case B::kNever:
            case B::kText:
            case B::kInferInteger:
                return false;
        }

        return false;
    }

    bool TypeChecker::is_storage_safe_owner_container_type_(ty::TypeId id) const {
        std::unordered_set<ty::TypeId> visiting{};
        auto rec = [&](auto&& self, ty::TypeId cur) -> bool {
            cur = canonicalize_transparent_external_typedef_(cur);
            if (cur == ty::kInvalidType || is_error_(cur)) return false;

            const auto& t = types_.get(cur);
            switch (t.kind) {
                case ty::Kind::kOptional:
                    return t.elem != ty::kInvalidType && self(self, t.elem);
                case ty::Kind::kArray:
                    return t.array_has_size &&
                           t.elem != ty::kInvalidType &&
                           self(self, t.elem);
                case ty::Kind::kPtr:
                    return t.elem != ty::kInvalidType;
                case ty::Kind::kEscape:
                    return t.elem != ty::kInvalidType;
                case ty::Kind::kBuiltin:
                    return is_field_pod_value_type_(types_, cur);
                case ty::Kind::kNamedUser:
                    break;
                default:
                    return false;
            }
            if (!visiting.insert(cur).second) {
                // Recursive by-value storage is not storage-safe in this round.
                return false;
            }

            if (actor_decl_by_type_.find(cur) != actor_decl_by_type_.end()) {
                return false;
            }

            auto validate_field_members = [&](ast::StmtId sid) -> bool {
                if (sid == ast::k_invalid_stmt || static_cast<size_t>(sid) >= ast_.stmts().size()) return false;
                const auto& s = ast_.stmt(sid);
                const uint64_t begin = s.field_member_begin;
                const uint64_t end = begin + s.field_member_count;
                if (begin > ast_.field_members().size() || end > ast_.field_members().size() || begin > end) {
                    return false;
                }
                for (uint32_t i = s.field_member_begin; i < s.field_member_begin + s.field_member_count; ++i) {
                    if (!self(self, ast_.field_members()[i].type)) return false;
                }
                return true;
            };

            if (auto it = class_decl_by_type_.find(cur); it != class_decl_by_type_.end()) {
                return validate_field_members(it->second);
            }
            if (auto it = field_abi_meta_by_type_.find(cur); it != field_abi_meta_by_type_.end()) {
                return validate_field_members(it->second.sid);
            }
            if (auto it = enum_abi_meta_by_type_.find(cur); it != enum_abi_meta_by_type_.end()) {
                for (const auto& variant : it->second.variants) {
                    for (const auto& field : variant.fields) {
                        if (!self(self, field.type)) return false;
                    }
                }
                return true;
            }

            return false;
        };
        return rec(rec, id);
    }

    bool TypeChecker::type_contains_infer_int_(ty::TypeId tid) const {
        tid = canonicalize_transparent_external_typedef_(tid);
        if (tid == ty::kInvalidType) return false;

        const auto& tt = types_.get(tid);
        switch (tt.kind) {
            case ty::Kind::kBuiltin:
                return tt.builtin == ty::Builtin::kInferInteger;
            case ty::Kind::kOptional:
            case ty::Kind::kArray:
            case ty::Kind::kBorrow:
            case ty::Kind::kEscape:
                return tt.elem != ty::kInvalidType && type_contains_infer_int_(tt.elem);
            default:
                return false;
        }
    }

    ty::TypeId TypeChecker::choose_smallest_signed_type_(const num::BigInt& v) const {
        ty::Builtin b = ty::Builtin::kI128;
        if      (v.fits_i8())  b = ty::Builtin::kI8;
        else if (v.fits_i16()) b = ty::Builtin::kI16;
        else if (v.fits_i32()) b = ty::Builtin::kI32;
        else if (v.fits_i64()) b = ty::Builtin::kI64;
        return types_.builtin(b);
    }

    bool TypeChecker::collect_infer_int_leaf_values_(ast::ExprId eid, std::vector<num::BigInt>& out) const {
        if (eid == ast::k_invalid_expr || static_cast<size_t>(eid) >= ast_.exprs().size()) return false;

        num::BigInt one{};
        if (infer_int_value_of_expr_(eid, one)) {
            out.push_back(one);
            return true;
        }

        const ast::Expr& e = ast_.expr(eid);
        auto collect_from_stmt = [&](auto&& self, ast::StmtId sid) -> bool {
            if (sid == ast::k_invalid_stmt || static_cast<size_t>(sid) >= ast_.stmts().size()) return false;
            const ast::Stmt& s = ast_.stmt(sid);
            bool any = false;

            if (s.kind == ast::StmtKind::kBlock) {
                const auto& items = ast_.stmt_children();
                const uint32_t end = s.stmt_begin + s.stmt_count;
                if (s.stmt_begin < items.size() && end <= items.size()) {
                    for (uint32_t i = s.stmt_begin; i < end; ++i) {
                        const ast::StmtId child = items[i];
                        if (child == ast::k_invalid_stmt) continue;
                        any = self(self, child) || any;
                    }
                }
                return any;
            }

            if (s.kind == ast::StmtKind::kBreak) {
                if (s.expr != ast::k_invalid_expr) {
                    any = collect_infer_int_leaf_values_(s.expr, out) || any;
                }
                return any;
            }

            if (s.a != ast::k_invalid_stmt) any = self(self, s.a) || any;
            if (s.b != ast::k_invalid_stmt) any = self(self, s.b) || any;
            return any;
        };

        switch (e.kind) {
            case ast::ExprKind::kArrayLit: {
                bool any = false;
                const auto& args = ast_.args();
                const uint32_t end = e.arg_begin + e.arg_count;
                if (e.arg_begin >= args.size() || end > args.size()) return false;
                for (uint32_t i = e.arg_begin; i < end; ++i) {
                    if (args[i].expr == ast::k_invalid_expr) continue;
                    any = collect_infer_int_leaf_values_(args[i].expr, out) || any;
                }
                return any;
            }
            case ast::ExprKind::kIfExpr:
                return (e.b != ast::k_invalid_expr && collect_infer_int_leaf_values_(e.b, out)) ||
                       (e.c != ast::k_invalid_expr && collect_infer_int_leaf_values_(e.c, out));
            case ast::ExprKind::kTernary:
                return (e.b != ast::k_invalid_expr && collect_infer_int_leaf_values_(e.b, out)) ||
                       (e.c != ast::k_invalid_expr && collect_infer_int_leaf_values_(e.c, out));
            case ast::ExprKind::kBlockExpr:
                return e.block_tail != ast::k_invalid_expr && collect_infer_int_leaf_values_(e.block_tail, out);
            case ast::ExprKind::kBinary:
                if (e.op == parus::syntax::TokenKind::kQuestionQuestion) {
                    bool any = false;
                    if (e.a != ast::k_invalid_expr) any = collect_infer_int_leaf_values_(e.a, out) || any;
                    if (e.b != ast::k_invalid_expr) any = collect_infer_int_leaf_values_(e.b, out) || any;
                    return any;
                }
                return false;
            case ast::ExprKind::kLoop:
                return collect_from_stmt(collect_from_stmt, e.loop_body);
            case ast::ExprKind::kIdent: {
                auto sid = lookup_symbol_(e.text);
                if (!sid) return false;
                auto origin_it = pending_int_sym_origin_.find(*sid);
                if (origin_it == pending_int_sym_origin_.end()) return false;
                if (origin_it->second == eid) return false;
                return collect_infer_int_leaf_values_(origin_it->second, out);
            }
            default:
                return false;
        }
    }

    bool TypeChecker::finalize_infer_int_shape_(ast::ExprId origin_eid, ty::TypeId current, ty::TypeId& out) const {
        out = canonicalize_transparent_external_typedef_(current);
        if (out == ty::kInvalidType) return false;
        if (!type_contains_infer_int_(out)) return true;

        std::vector<num::BigInt> leaves{};
        if (!collect_infer_int_leaf_values_(origin_eid, leaves) || leaves.empty()) {
            return false;
        }

        ty::TypeId chosen = choose_smallest_signed_type_(leaves.front());
        for (size_t i = 1; i < leaves.size(); ++i) {
            const ty::TypeId leaf_ty = choose_smallest_signed_type_(leaves[i]);
            if (leaf_ty == chosen) continue;
            const auto& cur_t = types_.get(chosen);
            const auto& next_t = types_.get(leaf_ty);
            if (cur_t.kind != ty::Kind::kBuiltin || next_t.kind != ty::Kind::kBuiltin) {
                return false;
            }
            auto rank = [](ty::Builtin b) -> int {
                switch (b) {
                    case ty::Builtin::kI8: return 1;
                    case ty::Builtin::kI16: return 2;
                    case ty::Builtin::kI32: return 3;
                    case ty::Builtin::kI64: return 4;
                    case ty::Builtin::kI128: return 5;
                    default: return 5;
                }
            };
            if (rank(next_t.builtin) > rank(cur_t.builtin)) {
                chosen = leaf_ty;
            }
        }

        const auto rewrite = [&](ty::TypeId src, const auto& self) -> ty::TypeId {
            src = canonicalize_transparent_external_typedef_(src);
            if (src == ty::kInvalidType) return ty::kInvalidType;
            const auto& st = types_.get(src);
            switch (st.kind) {
                case ty::Kind::kBuiltin:
                    if (st.builtin == ty::Builtin::kInferInteger) return chosen;
                    return src;
                case ty::Kind::kOptional: {
                    const ty::TypeId elem = self(st.elem, self);
                    if (elem == ty::kInvalidType) return ty::kInvalidType;
                    return types_.make_optional(elem);
                }
                case ty::Kind::kArray: {
                    const ty::TypeId elem = self(st.elem, self);
                    if (elem == ty::kInvalidType) return ty::kInvalidType;
                    return types_.make_array(elem, st.array_has_size, st.array_size);
                }
                case ty::Kind::kBorrow: {
                    const ty::TypeId elem = self(st.elem, self);
                    if (elem == ty::kInvalidType) return ty::kInvalidType;
                    return types_.make_borrow(elem, st.borrow_is_mut);
                }
                case ty::Kind::kEscape: {
                    const ty::TypeId elem = self(st.elem, self);
                    if (elem == ty::kInvalidType) return ty::kInvalidType;
                    return types_.make_escape(elem);
                }
                default:
                    return src;
            }
        };

        out = rewrite(out, rewrite);
        return out != ty::kInvalidType && !type_contains_infer_int_(out);
    }

    bool TypeChecker::infer_int_value_of_expr_(ast::ExprId eid, num::BigInt& out) const {
        auto it = pending_int_expr_.find((uint32_t)eid);
        if (it != pending_int_expr_.end() && it->second.has_value) {
            out = it->second.value;
            return true;
        }

        const ast::Expr& e = ast_.expr(eid);
        if (e.kind == ast::ExprKind::kIntLit) {
            const ParsedIntLiteral lit = parse_int_literal_(e.text);
            if (!lit.ok) return false;
            return num::BigInt::parse_dec(lit.digits_no_sep, out);
        }

        // ident의 경우: sym pending에서 찾아온다
        if (e.kind == ast::ExprKind::kIdent) {
            auto sid = lookup_symbol_(e.text);
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

        expected = canonicalize_transparent_external_typedef_(expected);
        if (expected == ty::kInvalidType) return false;

        const auto& et = types_.get(expected);
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
        const auto rewrite_infer_shape = [&](ty::TypeId src, ty::TypeId dst, const auto& self) -> ty::TypeId {
            src = canonicalize_transparent_external_typedef_(src);
            dst = canonicalize_transparent_external_typedef_(dst);
            if (src == ty::kInvalidType || dst == ty::kInvalidType) return ty::kInvalidType;
            if (src == dst) return dst;

            const auto& st = types_.get(src);
            const auto& dt = types_.get(dst);
            if (st.kind == ty::Kind::kBuiltin && dt.kind == ty::Kind::kBuiltin &&
                st.builtin == ty::Builtin::kInferInteger && is_int_builtin(dt.builtin)) {
                return dst;
            }

            if (st.kind != dt.kind) return ty::kInvalidType;
            switch (st.kind) {
                case ty::Kind::kOptional: {
                    const ty::TypeId elem = self(st.elem, dt.elem, self);
                    if (elem == ty::kInvalidType) return ty::kInvalidType;
                    return types_.make_optional(elem);
                }
                case ty::Kind::kArray: {
                    if (st.array_has_size != dt.array_has_size) return ty::kInvalidType;
                    if (st.array_has_size && st.array_size != dt.array_size) return ty::kInvalidType;
                    const ty::TypeId elem = self(st.elem, dt.elem, self);
                    if (elem == ty::kInvalidType) return ty::kInvalidType;
                    return types_.make_array(elem, st.array_has_size, st.array_size);
                }
                case ty::Kind::kBorrow: {
                    if (st.borrow_is_mut != dt.borrow_is_mut) return ty::kInvalidType;
                    const ty::TypeId elem = self(st.elem, dt.elem, self);
                    if (elem == ty::kInvalidType) return ty::kInvalidType;
                    return types_.make_borrow(elem, st.borrow_is_mut);
                }
                case ty::Kind::kEscape: {
                    const ty::TypeId elem = self(st.elem, dt.elem, self);
                    if (elem == ty::kInvalidType) return ty::kInvalidType;
                    return types_.make_escape(elem);
                }
                default:
                    return ty::kInvalidType;
            }
        };
        const auto resolve_ident_in_context = [&](ty::TypeId ctx) -> bool {
            if ((size_t)eid >= ast_.exprs().size()) return false;
            const ast::Expr& ex = ast_.expr(eid);
            if (ex.kind != ast::ExprKind::kIdent) return false;

            auto sid = lookup_symbol_(ex.text);
            if (!sid) return false;

            const ty::TypeId current = canonicalize_transparent_external_typedef_(sym_.symbol(*sid).declared_type);
            ty::TypeId rewritten = rewrite_infer_shape(current, ctx, rewrite_infer_shape);
            if (rewritten == ty::kInvalidType) {
                auto origin_it = pending_int_sym_origin_.find(*sid);
                if (origin_it != pending_int_sym_origin_.end() &&
                    origin_it->second != ast::k_invalid_expr &&
                    origin_it->second != eid) {
                    if (!resolve_infer_int_in_context_(origin_it->second, ctx)) {
                        ty::TypeId finalized = ty::kInvalidType;
                        if (!finalize_infer_int_shape_(origin_it->second, current, finalized)) {
                            return false;
                        }
                        rewritten = finalized;
                    } else {
                        const ty::TypeId origin_ty = check_expr_(origin_it->second);
                        rewritten = rewrite_infer_shape(origin_ty, ctx, rewrite_infer_shape);
                    }
                }
            }
            if (rewritten == ty::kInvalidType) return false;

            sym_.update_declared_type(*sid, rewritten);
            if ((size_t)eid < expr_type_cache_.size()) {
                expr_type_cache_[eid] = rewritten;
            }

            auto pit = pending_int_sym_.find(*sid);
            if (pit != pending_int_sym_.end()) {
                pit->second.resolved = true;
                pit->second.resolved_type = rewritten;
            }

            auto& pe = pending_int_expr_[(uint32_t)eid];
            pe.has_value = false;
            pe.resolved = true;
            pe.resolved_type = rewritten;
            return true;
        };
        if (resolve_ident_in_context(expected)) return true;

        // ------------------------------------------------------------
        // (0) aggregate context: array
        // - expected가 array인 경우, array literal의 각 원소로 컨텍스트를 내려준다.
        // - 원소 타입에 {integer}가 포함된 경우에만 재귀 해소를 시도한다.
        // ------------------------------------------------------------
        if (et.kind == ty::Kind::kArray) {
            const ast::Expr& e = ast_.expr(eid);
            if (e.kind != ast::ExprKind::kArrayLit) return false;
            if (et.array_has_size && e.arg_count != et.array_size) return false;

            ty::TypeId value_expected = expected;
            if (!et.array_has_size) {
                value_expected = types_.make_array(et.elem, /*has_size=*/true, e.arg_count);
            }

            bool ok_all = true;
            const auto& args = ast_.args();
            const uint32_t end = e.arg_begin + e.arg_count;
            if (e.arg_begin >= args.size() || end > args.size()) return false;

            for (uint32_t i = 0; i < e.arg_count; ++i) {
                const auto& a = args[e.arg_begin + i];
                if (a.expr == ast::k_invalid_expr) continue;

                ty::TypeId elem_t = check_expr_(a.expr);
                if (!type_contains_infer_int_(elem_t)) continue;

                if (!resolve_infer_int_in_context_(a.expr, et.elem)) {
                    ok_all = false;
                }
            }

            if (ok_all) {
                if ((size_t)eid < expr_type_cache_.size()) {
                    expr_type_cache_[eid] = value_expected;
                }
                return true;
            }
            return false;
        }

        if (et.kind == ty::Kind::kOptional) {
            if (et.elem != ty::kInvalidType) {
                const ast::Expr& e = ast_.expr(eid);
                if (e.kind == ast::ExprKind::kLoop ||
                    e.kind == ast::ExprKind::kIfExpr ||
                    e.kind == ast::ExprKind::kTernary ||
                    e.kind == ast::ExprKind::kBlockExpr ||
                    (e.kind == ast::ExprKind::kBinary &&
                     e.op == parus::syntax::TokenKind::kQuestionQuestion)) {
                    return resolve_infer_int_in_context_(eid, et.elem);
                }
            }
            return false;
        }

        // expected는 builtin int여야 한다.
        if (et.kind != ty::Kind::kBuiltin) return false;

        // float 컨텍스트면 즉시 에러 (암시적 int->float 금지)
        if (et.builtin == ty::Builtin::kF32 || et.builtin == ty::Builtin::kF64 || et.builtin == ty::Builtin::kF128 ||
            et.builtin == ty::Builtin::kCFloat || et.builtin == ty::Builtin::kCDouble) {
            diag_(diag::Code::kIntToFloatNotAllowed, ast_.expr(eid).span, types_.to_string(expected));
            return false;
        }

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
            if ((size_t)eid < expr_type_cache_.size()) {
                expr_type_cache_[eid] = expected;
            }
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
                if (e.block_tail != ast::k_invalid_expr) {
                    bool ok_tail = resolve_infer_int_in_context_(e.block_tail, expected);
                    if (ok_tail) {
                        mark_resolved_here(/*has_value=*/false, num::BigInt{});
                        return true;
                    }
                    return false;
                }
                // tail이 없으면 null로 수렴하므로 integer expected로는 해소 불가
                return false;
            }

            case ast::ExprKind::kLoop: {
                auto& me = ast_.expr_mut(eid);
                me.target_type = expected;
                if ((size_t)eid < expr_type_cache_.size()) {
                    expr_type_cache_[eid] = ty::kInvalidType;
                }

                const ty::TypeId loop_t = check_expr_(eid, Slot::kValue);
                if (is_error_(loop_t) || type_contains_infer_int_(loop_t)) {
                    return false;
                }

                auto& pe = pending_int_expr_[(uint32_t)eid];
                pe.has_value = false;
                pe.resolved = true;
                pe.resolved_type = loop_t;
                return true;
            }

            case ast::ExprKind::kBinary: {
                if (e.op != K::kQuestionQuestion) break;

                ty::TypeId lhs_t = (e.a != ast::k_invalid_expr) ? check_expr_(e.a, Slot::kValue) : ty::kInvalidType;
                lhs_t = canonicalize_transparent_external_typedef_(lhs_t);

                bool ok_lhs = true;
                if (!is_error_(lhs_t) && type_contains_infer_int_(lhs_t)) {
                    if (is_optional_(lhs_t)) {
                        ok_lhs = resolve_infer_int_in_context_(e.a, types_.make_optional(expected));
                    } else if (!is_null_(lhs_t)) {
                        ok_lhs = false;
                    }
                }

                bool ok_rhs = true;
                if (e.b != ast::k_invalid_expr) {
                    ty::TypeId rhs_t = check_expr_(e.b, Slot::kValue);
                    if (!is_error_(rhs_t) && type_contains_infer_int_(rhs_t)) {
                        ok_rhs = resolve_infer_int_in_context_(e.b, expected);
                    }
                }

                if (ok_lhs && ok_rhs) {
                    mark_resolved_here(/*has_value=*/false, num::BigInt{});
                    return true;
                }
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
            auto sid = lookup_symbol_(e.text);
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

        if ((size_t)eid < expr_type_cache_.size()) {
            expr_type_cache_[eid] = expected;
        }

        return true;
    }

    // --------------------
    // pass 2: check
    // --------------------
