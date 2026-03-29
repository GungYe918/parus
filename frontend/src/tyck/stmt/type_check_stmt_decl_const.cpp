    bool TypeChecker::stmt_diverges_(ast::StmtId sid, bool loop_control_counts) const {
        if (sid == ast::k_invalid_stmt) return false;
        const ast::Stmt& st = ast_.stmt(sid);

        switch (st.kind) {
            case ast::StmtKind::kReturn:
            case ast::StmtKind::kThrow:
                return true;

            case ast::StmtKind::kBreak:
            case ast::StmtKind::kContinue:
                return loop_control_counts;

            case ast::StmtKind::kExprStmt:
                if (st.expr == ast::k_invalid_expr ||
                    static_cast<size_t>(st.expr) >= expr_type_cache_.size()) {
                    return false;
                }
                return expr_type_cache_[st.expr] == types_.builtin(ty::Builtin::kNever);

            case ast::StmtKind::kBlock: {
                if (st.stmt_count == 0) return false;
                const auto& children = ast_.stmt_children();
                const ast::StmtId last = children[st.stmt_begin + (st.stmt_count - 1)];
                return stmt_diverges_(last, loop_control_counts);
            }

            case ast::StmtKind::kIf:
                if (st.a == ast::k_invalid_stmt || st.b == ast::k_invalid_stmt) return false;
                return stmt_diverges_(st.a, loop_control_counts) &&
                    stmt_diverges_(st.b, loop_control_counts);

            case ast::StmtKind::kDoScope:
            case ast::StmtKind::kManual:
                return (st.a != ast::k_invalid_stmt) ? stmt_diverges_(st.a, loop_control_counts) : false;

            case ast::StmtKind::kTryCatch: {
                if (st.a == ast::k_invalid_stmt) return false;
                if (!stmt_diverges_(st.a, loop_control_counts)) return false;
                if (st.catch_clause_count == 0) return false;
                const auto& catches = ast_.try_catch_clauses();
                const uint32_t cb = st.catch_clause_begin;
                const uint32_t ce = st.catch_clause_begin + st.catch_clause_count;
                if (ce > catches.size()) return false;
                for (uint32_t i = cb; i < ce; ++i) {
                    const auto& cc = catches[i];
                    if (cc.body == ast::k_invalid_stmt) return false;
                    if (!stmt_diverges_(cc.body, loop_control_counts)) return false;
                }
                return true;
            }

            case ast::StmtKind::kWhile:
            case ast::StmtKind::kDoWhile:
            case ast::StmtKind::kSwitch:
                return false;

            default:
                return false;
        }
    }

    void TypeChecker::check_stmt_fn_decl_(ast::StmtId sid, const ast::Stmt& s) {
        const ast::Stmt fn = s;
        const ast::StmtId saved_fn_decl_id = current_fn_decl_id_;
        current_fn_decl_id_ = sid;
        struct RestoreFnDeclId {
            ast::StmtId& slot;
            ast::StmtId saved;
            ~RestoreFnDeclId() { slot = saved; }
        } restore_fn_decl_id{current_fn_decl_id_, saved_fn_decl_id};
        // ----------------------------
        // 0) 시그니처 타입 확보
        // ----------------------------
        ty::TypeId sig = fn.type;

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

        if (is_va_list_type_(ret)) {
            const std::string msg = "vaList return type is not supported; use C ABI parameter pass-through only";
            diag_(diag::Code::kTypeErrorGeneric, s.span, msg);
            err_(s.span, msg);
        }
        (void)ensure_generic_field_instance_from_type_(ret, s.span);
        (void)ensure_generic_enum_instance_from_type_(ret, s.span);
        for (uint32_t i = 0; i < s.param_count; ++i) {
            const auto& p = ast_.params()[s.param_begin + i];
            (void)ensure_generic_field_instance_from_type_(p.type, p.span);
            (void)ensure_generic_enum_instance_from_type_(p.type, p.span);
            if (!is_va_list_type_(p.type)) continue;
            if (s.link_abi != ast::LinkAbi::kC) {
                const std::string msg =
                    "vaList parameter is allowed only on C ABI function signatures";
                diag_(diag::Code::kTypeErrorGeneric, p.span, msg);
                err_(p.span, msg);
            }
        }

        if (fn.link_abi == ast::LinkAbi::kC) {
            if (fn.fn_is_c_variadic && !fn.is_extern) {
                const std::string msg = "C variadic declaration is allowed only on extern \"C\" declarations";
                diag_(diag::Code::kTypeErrorGeneric, fn.span, msg);
                err_(fn.span, msg);
            }
            if (fn.is_throwing) {
                diag_(diag::Code::kAbiCThrowingFnNotAllowed, fn.span, fn.name);
                err_(fn.span, "C ABI function must not be throwing ('?'); convert exception channel at boundary");
            }

            auto check_enum_direct_c_abi = [&](ty::TypeId t, Span sp, std::string_view what) {
                (void)ensure_generic_enum_instance_from_type_(t, sp);
                if (enum_abi_meta_by_type_.find(t) != enum_abi_meta_by_type_.end()) {
                    diag_(diag::Code::kEnumCAbiDirectSignatureForbidden, sp, what);
                    err_(sp, "enum direct type in C ABI signature is forbidden in v0; cast at boundary (as i32)");
                }
            };

            if (fn.has_named_group || fn.positional_param_count != fn.param_count) {
                diag_(diag::Code::kAbiCNamedGroupNotAllowed, fn.span, fn.name);
                err_(fn.span, "C ABI function must not use named-group parameters: " + std::string(fn.name));
            }

            check_enum_direct_c_abi(ret, fn.span, std::string("return type of '") + std::string(fn.name) + "'");
            (void)ensure_generic_field_instance_from_type_(ret, s.span);
            if (!is_c_abi_safe_type_(ret, /*allow_void=*/true)) {
                const ty::TypeId canon = canonicalize_transparent_external_typedef_(ret);
                const bool is_text =
                    canon != ty::kInvalidType &&
                    types_.get(canon).kind == ty::Kind::kBuiltin &&
                    types_.get(canon).builtin == ty::Builtin::kText;
                diag_(diag::Code::kAbiCTypeNotFfiSafe, fn.span,
                    std::string("return type of '") + std::string(fn.name) + "'",
                    types_.to_string(ret));
                if (is_text) {
                    diag_(diag::Code::kTypeErrorGeneric, fn.span,
                          "text is not C ABI-safe; use *const core::ext::c_char and explicit boundary conversion");
                }
                std::string msg = "C ABI return type is not FFI-safe";
                if (is_text) msg += " (text is not C ABI-safe; use *const core::ext::c_char)";
                err_(fn.span, msg);
            }

            for (uint32_t i = 0; i < fn.param_count; ++i) {
                const auto& p = ast_.params()[fn.param_begin + i];
                check_enum_direct_c_abi(p.type, p.span, std::string("parameter '") + std::string(p.name) + "'");
                (void)ensure_generic_field_instance_from_type_(p.type, p.span);
                if (!is_c_abi_safe_type_(p.type, /*allow_void=*/false)) {
                    const ty::TypeId canon = canonicalize_transparent_external_typedef_(p.type);
                    const bool is_text =
                        canon != ty::kInvalidType &&
                        types_.get(canon).kind == ty::Kind::kBuiltin &&
                        types_.get(canon).builtin == ty::Builtin::kText;
                    diag_(diag::Code::kAbiCTypeNotFfiSafe, p.span,
                        std::string("parameter '") + std::string(p.name) + "'",
                        types_.to_string(p.type));
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

        if (fn.has_named_group && fn.positional_param_count > 0) {
            diag_(diag::Code::kFnNamedGroupMixedWithPositional, fn.span);
            err_(fn.span, "function declaration must be either positional-only or named-group-only");
        }

        // ----------------------------
        // 0.5) generic constraints (declaration-time validation)
        // ----------------------------
        std::unordered_set<std::string> generic_params;
        for (uint32_t gi = 0; gi < fn.fn_generic_param_count; ++gi) {
            const uint32_t idx = fn.fn_generic_param_begin + gi;
            if (idx >= ast_.generic_param_decls().size()) break;
            generic_params.insert(std::string(ast_.generic_param_decls()[idx].name));
        }
        if (!validate_constraint_clause_decl_(fn.fn_constraint_begin, fn.fn_constraint_count, generic_params, fn.span)) {
            if (generic_params.empty()) {
                err_(fn.span, "generic constraint declaration validation failed");
            }
        }

        ImplBindingKind impl_binding = ImplBindingKind::kNone;
        const bool has_impl_binding = stmt_impl_binding_kind_(fn, impl_binding);
        if (has_impl_binding) {
            const bool compiler_owned_impl = (fn.a == ast::k_invalid_stmt);
            if (compiler_owned_impl) {
                const std::string file_bundle = bundle_name_for_file_(fn.span.file_id);
                if (file_bundle != "core" ||
                    core_impl_marker_file_ids_.find(fn.span.file_id) == core_impl_marker_file_ids_.end()) {
                    const std::string msg =
                        "bodyless recognized $![Impl::*] binding requires bundle 'core' and file marker '$![Impl::Core];'";
                    diag_(diag::Code::kTypeErrorGeneric, fn.span, msg);
                    err_(fn.span, msg);
                }
            }

            if (fn.is_extern) {
                const std::string msg = "recognized $![Impl::*] binding must not be extern";
                diag_(diag::Code::kTypeErrorGeneric, fn.span, msg);
                err_(fn.span, msg);
            }

            const ty::TypeId usize_ty = types_.builtin(ty::Builtin::kUSize);
            const ty::TypeId unit_ty = types_.builtin(ty::Builtin::kUnit);
            switch (impl_binding) {
                case ImplBindingKind::kSpinLoop:
                    if (fn.param_count != 0 || fn.fn_generic_param_count != 0 || ret != unit_ty ||
                        fn.name != "spin_loop") {
                        const std::string msg =
                            "$![Impl::SpinLoop] requires signature 'def spin_loop() -> void'";
                        diag_(diag::Code::kTypeErrorGeneric, fn.span, msg);
                        err_(fn.span, msg);
                    }
                    break;
                case ImplBindingKind::kStepNext:
                    break;
                case ImplBindingKind::kSizeOf:
                    if (fn.param_count != 0 || fn.fn_generic_param_count != 1 || ret != usize_ty ||
                        fn.name != "size_of") {
                        const std::string msg =
                            "$![Impl::SizeOf] requires signature 'def size_of<T>() -> usize'";
                        diag_(diag::Code::kTypeErrorGeneric, fn.span, msg);
                        err_(fn.span, msg);
                    }
                    break;
                case ImplBindingKind::kAlignOf:
                    if (fn.param_count != 0 || fn.fn_generic_param_count != 1 || ret != usize_ty ||
                        fn.name != "align_of") {
                        const std::string msg =
                            "$![Impl::AlignOf] requires signature 'def align_of<T>() -> usize'";
                        diag_(diag::Code::kTypeErrorGeneric, fn.span, msg);
                        err_(fn.span, msg);
                    }
                    break;
                case ImplBindingKind::kNone:
                    break;
            }
        }

        // generic templates are declaration-only at this stage.
        // concrete instances are materialized and checked on-demand at call sites.
        if (sid != ast::k_invalid_stmt &&
            fn.fn_generic_param_count > 0 &&
            generic_fn_template_sid_set_.find(sid) != generic_fn_template_sid_set_.end()) {
            return;
        }
        if (has_impl_binding && fn.a == ast::k_invalid_stmt) {
            return;
        }

        // ----------------------------
        // 1) 함수 스코프 진입 + def ctx 설정
        // ----------------------------
        const OwnershipStateMap saved_ownership = capture_ownership_state_();
        sym_.push_scope();

        FnCtx saved = fn_ctx_;
        fn_ctx_.in_fn = true;
        fn_ctx_.is_pure = fn.is_pure;
        fn_ctx_.is_comptime = fn.is_comptime;
        fn_ctx_.is_throwing = fn.is_throwing;
        fn_ctx_.has_exception_construct = false;
        fn_ctx_.ret = (ret == ty::kInvalidType) ? types_.error() : ret;
        fn_sid_stack_.push_back(sid);

        std::vector<std::string> saved_namespace_stack = namespace_stack_;
        auto restore_namespace_stack = [&]() {
            namespace_stack_ = std::move(saved_namespace_stack);
        };
        auto strip_inst_suffix = [](std::string_view qname) -> std::string_view {
            const size_t last_colons = qname.rfind("::");
            const size_t last_lt = qname.rfind('<');
            const size_t last_gt = qname.rfind('>');
            if (last_lt == std::string_view::npos || last_gt == std::string_view::npos) return qname;
            if (last_gt + 1 != qname.size()) return qname;
            if (last_colons != std::string_view::npos && last_lt < last_colons + 2) return qname;
            return qname.substr(0, last_lt);
        };
        auto apply_function_namespace = [&](std::string_view qname) {
            qname = strip_inst_suffix(qname);
            const size_t split = qname.rfind("::");
            if (split == std::string_view::npos) return;
            const std::string_view ns = qname.substr(0, split);
            if (ns.empty()) return;
            std::vector<std::string> replacement{};
            size_t pos = 0;
            while (pos < ns.size()) {
                const size_t next = ns.find("::", pos);
                if (next == std::string_view::npos) {
                    replacement.push_back(std::string(ns.substr(pos)));
                    break;
                }
                replacement.push_back(std::string(ns.substr(pos, next - pos)));
                pos = next + 2;
            }
            namespace_stack_ = std::move(replacement);
        };

        if (sid != ast::k_invalid_stmt) {
            if (auto it = fn_qualified_name_by_stmt_.find(sid); it != fn_qualified_name_by_stmt_.end()) {
                apply_function_namespace(it->second);
            } else if (sid < stmt_resolved_symbol_cache_.size()) {
                const uint32_t sym_sid = stmt_resolved_symbol_cache_[sid];
                if (sym_sid != sema::SymbolTable::kNoScope && sym_sid < sym_.symbols().size()) {
                    apply_function_namespace(sym_.symbol(sym_sid).name);
                }
            }
        }

        // ----------------------------
        // 2) 파라미터 심볼 삽입 + default expr 검사
        // ----------------------------
        for (uint32_t i = 0; i < fn.param_count; ++i) {
            const auto& p = ast_.params()[fn.param_begin + i];
            ty::TypeId pt = (p.type == ty::kInvalidType) ? types_.error() : p.type;

            auto ins = sym_.insert(sema::SymbolKind::kVar, p.name, pt, p.span);
            if (!ins.ok && ins.is_duplicate) {
                err_(p.span, "duplicate parameter name: " + std::string(p.name));
                diag_(diag::Code::kTypeDuplicateParam, p.span, p.name);
            }
            if (ins.ok) {
                if ((size_t)(fn.param_begin + i) >= param_resolved_symbol_cache_.size()) {
                    param_resolved_symbol_cache_.resize((size_t)(fn.param_begin + i) + 1, sema::SymbolTable::kNoScope);
                }
                param_resolved_symbol_cache_[fn.param_begin + i] = ins.symbol_id;
                // receiver mutability follows `mut self`; regular params follow `mut name: T`.
                const bool param_is_mut = p.is_mut ||
                    (p.is_self && p.self_kind == ast::SelfReceiverKind::kMut);
                sym_is_mut_[ins.symbol_id] = param_is_mut;
                mark_symbol_initialized_(ins.symbol_id);
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
        if (fn.is_extern) {
            if (fn.a != ast::k_invalid_stmt) {
                diag_(diag::Code::kTypeErrorGeneric, fn.span, "extern function declaration must not have a body");
                err_(fn.span, "extern function declaration must not have a body");
            }
        } else if (fn.a != ast::k_invalid_stmt) {
            check_stmt_(fn.a);
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

        if (!fn.is_extern && !is_unit(fn_ret) && !is_never(fn_ret)) {
            const bool ok_all_paths = stmt_diverges_(fn.a, /*loop_control_counts=*/false);
            if (!ok_all_paths) {
                // 여기서 “return 누락” 진단
                // (diag code는 새로 만드는 게 정석: kMissingReturn)
                diag_(diag::Code::kMissingReturn, fn.span, fn.name);
                err_(fn.span, "missing return on some control path");
            }
        }

        // ----------------------------
        // 4) 종료
        // ----------------------------
        fn_ctx_ = saved;
        if (!fn_sid_stack_.empty()) {
            fn_sid_stack_.pop_back();
        }
        restore_namespace_stack();
        sym_.pop_scope();
        restore_ownership_state_(saved_ownership);
    }

    namespace {
        static bool parse_char_literal_scalar_(std::string_view lit, uint32_t& out) {
            if (lit.size() < 3 || lit.front() != '\'' || lit.back() != '\'') return false;
            std::string_view body = lit.substr(1, lit.size() - 2);
            if (body.empty()) return false;
            if (body.size() == 1) {
                out = static_cast<uint32_t>(static_cast<unsigned char>(body[0]));
                return true;
            }
            if (body[0] != '\\' || body.size() != 2) return false;
            switch (body[1]) {
                case 'n': out = static_cast<uint32_t>('\n'); return true;
                case 'r': out = static_cast<uint32_t>('\r'); return true;
                case 't': out = static_cast<uint32_t>('\t'); return true;
                case '\\': out = static_cast<uint32_t>('\\'); return true;
                case '\'': out = static_cast<uint32_t>('\''); return true;
                case '0': out = static_cast<uint32_t>('\0'); return true;
                default: return false;
            }
        }

        static bool parse_int_literal_i64_(std::string_view lit, int64_t& out, std::string& canonical) {
            const ParsedIntLiteral p = parse_int_literal_(lit);
            if (!p.ok || p.digits_no_sep.empty()) return false;

            errno = 0;
            char* end = nullptr;
            const long long v = std::strtoll(p.digits_no_sep.c_str(), &end, 10);
            if (errno == ERANGE || end == nullptr || *end != '\0') return false;
            out = static_cast<int64_t>(v);
            canonical = p.digits_no_sep;
            return true;
        }

        static bool parse_float_literal_f64_(std::string_view lit, double& out, std::string& canonical) {
            const ParsedFloatLiteral pf = parse_float_literal_(lit);
            if (!pf.ok) return false;

            size_t i = 0;
            while (i < lit.size()) {
                const unsigned char u = static_cast<unsigned char>(lit[i]);
                if (std::isdigit(u) || lit[i] == '_') {
                    ++i;
                    continue;
                }
                break;
            }
            if (i < lit.size() && lit[i] == '.') {
                ++i;
                while (i < lit.size()) {
                    const unsigned char u = static_cast<unsigned char>(lit[i]);
                    if (std::isdigit(u) || lit[i] == '_') {
                        ++i;
                        continue;
                    }
                    break;
                }
            }
            std::string body;
            body.reserve(i);
            for (size_t j = 0; j < i; ++j) {
                if (lit[j] == '_') continue;
                body.push_back(lit[j]);
            }
            if (body.empty()) return false;

            errno = 0;
            char* end = nullptr;
            const double v = std::strtod(body.c_str(), &end);
            if (errno == ERANGE || end == nullptr || *end != '\0') return false;
            out = v;
            canonical = body;
            return true;
        }

        static std::string format_const_float_(double v) {
            std::ostringstream oss;
            oss.setf(std::ios::fmtflags(0), std::ios::floatfield);
            oss << std::setprecision(17) << v;
            return oss.str();
        }

    } // namespace

    bool TypeChecker::const_value_to_scalar_init_(const ConstValue& in, ConstInitData& out) const {
        switch (in.kind) {
            case ConstValue::Kind::kInt:
                out.kind = ConstInitKind::kInt;
                out.text = std::to_string(in.i64);
                return true;
            case ConstValue::Kind::kFloat:
                out.kind = ConstInitKind::kFloat;
                out.text = format_const_float_(in.f64);
                return true;
            case ConstValue::Kind::kBool:
                out.kind = ConstInitKind::kBool;
                out.text = in.b ? "true" : "false";
                return true;
            case ConstValue::Kind::kChar:
                out.kind = ConstInitKind::kChar;
                out.text = std::to_string(in.ch);
                return true;
            case ConstValue::Kind::kInvalid:
            case ConstValue::Kind::kStruct:
            default:
                return false;
        }
    }

    bool TypeChecker::const_value_type_matches_(const ConstValue& v, ty::TypeId expected) const {
        if (expected == ty::kInvalidType) return true;
        if (v.type == ty::kInvalidType) return true;
        return v.type == expected;
    }

    bool TypeChecker::const_value_is_composite_(const ConstValue& v) const {
        return v.kind == ConstValue::Kind::kStruct;
    }

    bool TypeChecker::eval_const_expr_value_(ast::ExprId expr_id, ConstValue& out, Span diag_span) {
        ConstEvalContext ctx{};
        return eval_const_expr_value_impl_(expr_id, out, diag_span, ctx, nullptr);
    }

    bool TypeChecker::eval_const_symbol_value_(uint32_t symbol_id, ConstValue& out, Span diag_span) {
        ConstEvalContext ctx{};
        return eval_const_symbol_value_impl_(symbol_id, out, diag_span, ctx);
    }

    bool TypeChecker::eval_const_expr_(ast::ExprId expr_id, ConstInitData& out, Span diag_span) {
        ConstValue value{};
        if (!eval_const_expr_value_(expr_id, value, diag_span)) return false;
        if (!const_value_to_scalar_init_(value, out)) {
            diag_(diag::Code::kConstExprNotEvaluable, diag_span, "composite const value is not scalar-projectable");
            err_(diag_span, "const expression evaluated to a composite value where a scalar const was required");
            return false;
        }
        return true;
    }

    bool TypeChecker::eval_const_symbol_(uint32_t symbol_id, ConstInitData& out, Span diag_span) {
        if (auto it = result_.const_symbol_values.find(symbol_id); it != result_.const_symbol_values.end()) {
            out = it->second;
            return true;
        }
        ConstValue value{};
        if (!eval_const_symbol_value_(symbol_id, value, diag_span)) return false;
        if (!const_value_to_scalar_init_(value, out)) {
            diag_(diag::Code::kConstExprNotEvaluable, diag_span, "const symbol is composite and cannot be scalar-lowered");
            err_(diag_span, "const symbol evaluated to a composite value where a scalar const was required");
            return false;
        }
        result_.const_symbol_values[symbol_id] = out;
        return true;
    }

    bool TypeChecker::eval_const_expr_value_impl_(
        ast::ExprId expr_id,
        ConstValue& out,
        Span diag_span,
        ConstEvalContext& ctx,
        std::unordered_map<std::string, ConstBinding>* local_env
    ) {
        auto fail_not_evaluable = [&](Span sp, std::string_view reason) -> bool {
            diag_(diag::Code::kConstExprNotEvaluable, sp, reason);
            err_(sp, std::string("const expression is not evaluable: ") + std::string(reason));
            return false;
        };
        auto step_guard = [&](Span sp) -> bool {
            ++ctx.step_count;
            if (ctx.step_count <= ctx.step_budget) return true;
            diag_(diag::Code::kConstEvalStepLimitExceeded, sp, std::to_string(ctx.step_budget));
            err_(sp, "const evaluation step budget exceeded");
            return false;
        };
        auto expr_type_of = [&](ast::ExprId eid) -> ty::TypeId {
            if (eid != ast::k_invalid_expr && static_cast<size_t>(eid) < expr_type_cache_.size()) {
                return expr_type_cache_[eid];
            }
            return ty::kInvalidType;
        };

        auto eval = [&](auto&& self, ast::ExprId eid, ConstValue& outv) -> bool {
            if (!step_guard(diag_span)) return false;
            if (eid == ast::k_invalid_expr || (size_t)eid >= ast_.exprs().size()) {
                return fail_not_evaluable(diag_span, "invalid expression");
            }

            const auto& e = ast_.expr(eid);
            const ty::TypeId inferred_ty = expr_type_of(eid);
            switch (e.kind) {
                case ast::ExprKind::kIntLit: {
                    int64_t v = 0;
                    std::string canonical;
                    if (!parse_int_literal_i64_(e.text, v, canonical)) {
                        return fail_not_evaluable(e.span, "invalid integer literal for const expression");
                    }
                    outv.kind = ConstValue::Kind::kInt;
                    outv.i64 = v;
                    outv.type = inferred_ty;
                    return true;
                }
                case ast::ExprKind::kFloatLit: {
                    double v = 0.0;
                    std::string canonical;
                    if (!parse_float_literal_f64_(e.text, v, canonical)) {
                        return fail_not_evaluable(e.span, "invalid float literal for const expression");
                    }
                    outv.kind = ConstValue::Kind::kFloat;
                    outv.f64 = v;
                    outv.type = inferred_ty;
                    return true;
                }
                case ast::ExprKind::kBoolLit:
                    outv.kind = ConstValue::Kind::kBool;
                    outv.b = (e.text == "true");
                    outv.type = inferred_ty;
                    return true;
                case ast::ExprKind::kCharLit: {
                    uint32_t ch = 0;
                    if (!parse_char_literal_scalar_(e.text, ch)) {
                        return fail_not_evaluable(e.span, "invalid char literal for const expression");
                    }
                    outv.kind = ConstValue::Kind::kChar;
                    outv.ch = ch;
                    outv.type = inferred_ty;
                    return true;
                }
                case ast::ExprKind::kIdent: {
                    if (local_env != nullptr) {
                        if (auto it = local_env->find(std::string(e.text)); it != local_env->end()) {
                            outv = it->second.value;
                            return true;
                        }
                    }

                    auto sid = lookup_symbol_(e.text);
                    if (!sid.has_value()) {
                        return fail_not_evaluable(e.span, "unresolved identifier in const expression");
                    }
                    if (!eval_const_symbol_value_impl_(*sid, outv, e.span, ctx)) {
                        return false;
                    }
                    return true;
                }
                case ast::ExprKind::kFieldInit: {
                    if (inferred_ty == ty::kInvalidType) {
                        return fail_not_evaluable(e.span, "field initializer has no resolved type");
                    }
                    auto meta_it = field_abi_meta_by_type_.find(inferred_ty);
                    if (meta_it == field_abi_meta_by_type_.end()) {
                        return fail_not_evaluable(e.span, "field initializer type is not a concrete struct/class field layout");
                    }
                    const ast::StmtId owner_sid = meta_it->second.sid;
                    if (owner_sid == ast::k_invalid_stmt || (size_t)owner_sid >= ast_.stmts().size()) {
                        return fail_not_evaluable(e.span, "field initializer owner declaration is invalid");
                    }
                    const auto& owner = ast_.stmt(owner_sid);
                    const uint64_t mb = owner.field_member_begin;
                    const uint64_t me = mb + owner.field_member_count;
                    const auto& members = ast_.field_members();
                    if (mb > members.size() || me > members.size()) {
                        return fail_not_evaluable(e.span, "field initializer member range is invalid");
                    }

                    std::unordered_map<std::string, uint32_t> index_by_name{};
                    index_by_name.reserve(owner.field_member_count);
                    for (uint32_t i = 0; i < owner.field_member_count; ++i) {
                        const auto& fm = members[owner.field_member_begin + i];
                        index_by_name.emplace(std::string(fm.name), i);
                    }

                    std::vector<uint8_t> seen(owner.field_member_count, 0u);
                    std::vector<ConstValue> ordered_values(owner.field_member_count);
                    const auto& inits = ast_.field_init_entries();
                    const uint64_t ib = e.field_init_begin;
                    const uint64_t ie = ib + e.field_init_count;
                    if (ib > inits.size() || ie > inits.size()) {
                        return fail_not_evaluable(e.span, "field initializer entry range is invalid");
                    }
                    for (uint32_t i = 0; i < e.field_init_count; ++i) {
                        const auto& ent = inits[e.field_init_begin + i];
                        auto fit = index_by_name.find(std::string(ent.name));
                        if (fit == index_by_name.end()) {
                            return fail_not_evaluable(ent.span, "field initializer references unknown member");
                        }
                        const uint32_t idx = fit->second;
                        if (seen[idx] != 0u) {
                            return fail_not_evaluable(ent.span, "field initializer has duplicate member");
                        }
                        ConstValue fv{};
                        if (!self(self, ent.expr, fv)) return false;
                        const auto& fm = members[owner.field_member_begin + idx];
                        if (!const_value_type_matches_(fv, fm.type)) {
                            return fail_not_evaluable(ent.span, "field initializer member type mismatch");
                        }
                        ordered_values[idx] = fv;
                        seen[idx] = 1u;
                    }
                    for (uint32_t i = 0; i < owner.field_member_count; ++i) {
                        if (seen[i] != 0u) continue;
                        return fail_not_evaluable(e.span, "field initializer is missing member");
                    }

                    auto obj = std::make_shared<ConstObject>();
                    obj->type = inferred_ty;
                    obj->field_names.reserve(owner.field_member_count);
                    obj->field_values.reserve(owner.field_member_count);
                    for (uint32_t i = 0; i < owner.field_member_count; ++i) {
                        const auto& fm = members[owner.field_member_begin + i];
                        obj->field_names.push_back(std::string(fm.name));
                        obj->field_values.push_back(ordered_values[i]);
                    }

                    outv.kind = ConstValue::Kind::kStruct;
                    outv.type = inferred_ty;
                    outv.object = std::move(obj);
                    return true;
                }
                case ast::ExprKind::kUnary: {
                    ConstValue in{};
                    if (!self(self, e.a, in)) return false;
                    if (e.op == K::kPlus) {
                        if (in.kind == ConstValue::Kind::kInt ||
                            in.kind == ConstValue::Kind::kFloat ||
                            in.kind == ConstValue::Kind::kChar) {
                            outv = in;
                            outv.type = inferred_ty;
                            return true;
                        }
                        return fail_not_evaluable(e.span, "unary '+' requires numeric const operand");
                    }
                    if (e.op == K::kMinus) {
                        if (in.kind == ConstValue::Kind::kInt) {
                            outv.kind = ConstValue::Kind::kInt;
                            outv.i64 = -in.i64;
                            outv.type = inferred_ty;
                            return true;
                        }
                        if (in.kind == ConstValue::Kind::kFloat) {
                            outv.kind = ConstValue::Kind::kFloat;
                            outv.f64 = -in.f64;
                            outv.type = inferred_ty;
                            return true;
                        }
                        if (in.kind == ConstValue::Kind::kChar) {
                            outv.kind = ConstValue::Kind::kInt;
                            outv.i64 = -static_cast<int64_t>(in.ch);
                            outv.type = inferred_ty;
                            return true;
                        }
                        return fail_not_evaluable(e.span, "unary '-' requires numeric const operand");
                    }
                    if (e.op == K::kKwNot) {
                        if (in.kind != ConstValue::Kind::kBool) {
                            return fail_not_evaluable(e.span, "logical not requires bool const operand");
                        }
                        outv.kind = ConstValue::Kind::kBool;
                        outv.b = !in.b;
                        outv.type = inferred_ty;
                        return true;
                    }
                    if (e.op == K::kBang) {
                        if (in.kind != ConstValue::Kind::kInt) {
                            return fail_not_evaluable(e.span, "bitwise not requires integer const operand");
                        }
                        outv.kind = ConstValue::Kind::kInt;
                        outv.i64 = ~in.i64;
                        outv.type = inferred_ty;
                        return true;
                    }
                    return fail_not_evaluable(e.span, "unsupported unary operator in const expression");
                }
                case ast::ExprKind::kBinary: {
                    ConstValue lhs{};
                    ConstValue rhs{};
                    if (!self(self, e.a, lhs)) return false;
                    if (!self(self, e.b, rhs)) return false;

                    auto is_numeric = [](ConstValue::Kind k) {
                        return k == ConstValue::Kind::kInt ||
                               k == ConstValue::Kind::kFloat ||
                               k == ConstValue::Kind::kChar;
                    };
                    auto as_i64 = [](const ConstValue& v, int64_t& out_i) -> bool {
                        if (v.kind == ConstValue::Kind::kInt) {
                            out_i = v.i64;
                            return true;
                        }
                        if (v.kind == ConstValue::Kind::kChar) {
                            out_i = static_cast<int64_t>(v.ch);
                            return true;
                        }
                        return false;
                    };
                    auto as_f64 = [&](const ConstValue& v, double& out_f) -> bool {
                        if (v.kind == ConstValue::Kind::kFloat) {
                            out_f = v.f64;
                            return true;
                        }
                        int64_t iv = 0;
                        if (as_i64(v, iv)) {
                            out_f = static_cast<double>(iv);
                            return true;
                        }
                        return false;
                    };

                    auto eval_rel = [&](auto cmp) -> bool {
                        if (!is_numeric(lhs.kind) || !is_numeric(rhs.kind)) {
                            return fail_not_evaluable(e.span, "relational operator requires numeric const operands");
                        }
                        if (lhs.kind == ConstValue::Kind::kFloat || rhs.kind == ConstValue::Kind::kFloat) {
                            double lf = 0.0;
                            double rf = 0.0;
                            if (!as_f64(lhs, lf) || !as_f64(rhs, rf)) {
                                return fail_not_evaluable(e.span, "failed numeric conversion in const comparison");
                            }
                            outv.kind = ConstValue::Kind::kBool;
                            outv.b = cmp(lf, rf);
                            outv.type = inferred_ty;
                            return true;
                        }
                        int64_t li = 0;
                        int64_t ri = 0;
                        if (!as_i64(lhs, li) || !as_i64(rhs, ri)) {
                            return fail_not_evaluable(e.span, "failed integer conversion in const comparison");
                        }
                        outv.kind = ConstValue::Kind::kBool;
                        outv.b = cmp(li, ri);
                        outv.type = inferred_ty;
                        return true;
                    };

                    switch (e.op) {
                        case K::kPlus:
                        case K::kMinus:
                        case K::kStar:
                        case K::kSlash: {
                            if (!is_numeric(lhs.kind) || !is_numeric(rhs.kind)) {
                                return fail_not_evaluable(e.span, "arithmetic operator requires numeric const operands");
                            }
                            if (lhs.kind == ConstValue::Kind::kFloat || rhs.kind == ConstValue::Kind::kFloat) {
                                double lf = 0.0;
                                double rf = 0.0;
                                if (!as_f64(lhs, lf) || !as_f64(rhs, rf)) {
                                    return fail_not_evaluable(e.span, "failed numeric conversion in const arithmetic");
                                }
                                if (e.op == K::kSlash && rf == 0.0) {
                                    return fail_not_evaluable(e.span, "division by zero in const expression");
                                }
                                outv.kind = ConstValue::Kind::kFloat;
                                if (e.op == K::kPlus) outv.f64 = lf + rf;
                                else if (e.op == K::kMinus) outv.f64 = lf - rf;
                                else if (e.op == K::kStar) outv.f64 = lf * rf;
                                else outv.f64 = lf / rf;
                                outv.type = inferred_ty;
                                return true;
                            }
                            int64_t li = 0;
                            int64_t ri = 0;
                            if (!as_i64(lhs, li) || !as_i64(rhs, ri)) {
                                return fail_not_evaluable(e.span, "failed integer conversion in const arithmetic");
                            }
                            if (e.op == K::kSlash && ri == 0) {
                                return fail_not_evaluable(e.span, "division by zero in const expression");
                            }
                            outv.kind = ConstValue::Kind::kInt;
                            if (e.op == K::kPlus) outv.i64 = li + ri;
                            else if (e.op == K::kMinus) outv.i64 = li - ri;
                            else if (e.op == K::kStar) outv.i64 = li * ri;
                            else outv.i64 = li / ri;
                            outv.type = inferred_ty;
                            return true;
                        }
                        case K::kPercent: {
                            int64_t li = 0;
                            int64_t ri = 0;
                            if (!as_i64(lhs, li) || !as_i64(rhs, ri)) {
                                return fail_not_evaluable(e.span, "modulo requires integer const operands");
                            }
                            if (ri == 0) {
                                return fail_not_evaluable(e.span, "modulo by zero in const expression");
                            }
                            outv.kind = ConstValue::Kind::kInt;
                            outv.i64 = li % ri;
                            outv.type = inferred_ty;
                            return true;
                        }
                        case K::kEqEq:
                        case K::kBangEq: {
                            bool eq = false;
                            if (lhs.kind == ConstValue::Kind::kBool && rhs.kind == ConstValue::Kind::kBool) {
                                eq = (lhs.b == rhs.b);
                            } else if (is_numeric(lhs.kind) && is_numeric(rhs.kind)) {
                                if (lhs.kind == ConstValue::Kind::kFloat || rhs.kind == ConstValue::Kind::kFloat) {
                                    double lf = 0.0;
                                    double rf = 0.0;
                                    if (!as_f64(lhs, lf) || !as_f64(rhs, rf)) {
                                        return fail_not_evaluable(e.span, "failed numeric conversion in const equality");
                                    }
                                    eq = (lf == rf);
                                } else {
                                    int64_t li = 0;
                                    int64_t ri = 0;
                                    if (!as_i64(lhs, li) || !as_i64(rhs, ri)) {
                                        return fail_not_evaluable(e.span, "failed integer conversion in const equality");
                                    }
                                    eq = (li == ri);
                                }
                            } else {
                                return fail_not_evaluable(e.span, "equality requires compatible const operands");
                            }
                            outv.kind = ConstValue::Kind::kBool;
                            outv.b = (e.op == K::kEqEq) ? eq : !eq;
                            outv.type = inferred_ty;
                            return true;
                        }
                        case K::kLt:
                            return eval_rel([](auto l, auto r) { return l < r; });
                        case K::kLtEq:
                            return eval_rel([](auto l, auto r) { return l <= r; });
                        case K::kGt:
                            return eval_rel([](auto l, auto r) { return l > r; });
                        case K::kGtEq:
                            return eval_rel([](auto l, auto r) { return l >= r; });
                        case K::kKwAnd:
                        case K::kKwOr: {
                            if (lhs.kind != ConstValue::Kind::kBool || rhs.kind != ConstValue::Kind::kBool) {
                                return fail_not_evaluable(e.span, "logical operator requires bool const operands");
                            }
                            outv.kind = ConstValue::Kind::kBool;
                            outv.b = (e.op == K::kKwAnd) ? (lhs.b && rhs.b) : (lhs.b || rhs.b);
                            outv.type = inferred_ty;
                            return true;
                        }
                        default:
                            return fail_not_evaluable(e.span, "unsupported binary operator in const expression");
                    }
                }
                case ast::ExprKind::kAssign: {
                    if (e.op != K::kAssign) {
                        return fail_not_evaluable(e.span, "only plain '=' assignment is supported in const evaluation");
                    }
                    if (local_env == nullptr) {
                        return fail_not_evaluable(e.span, "assignment in const expression requires local const-fn context");
                    }
                    if (e.a == ast::k_invalid_expr || (size_t)e.a >= ast_.exprs().size()) {
                        return fail_not_evaluable(e.span, "assignment lhs is invalid");
                    }
                    const auto& lhs = ast_.expr(e.a);
                    if (lhs.kind != ast::ExprKind::kIdent) {
                        return fail_not_evaluable(lhs.span, "assignment lhs must be an identifier in const evaluation");
                    }
                    auto it = local_env->find(std::string(lhs.text));
                    if (it == local_env->end()) {
                        return fail_not_evaluable(lhs.span, "assignment lhs is not a local const-fn variable");
                    }
                    if (!it->second.is_mut) {
                        diag_(diag::Code::kWriteToImmutable, lhs.span, "assignment");
                        err_(lhs.span, "cannot assign to immutable const-fn local");
                        return false;
                    }

                    ConstValue rhs{};
                    if (!self(self, e.b, rhs)) return false;
                    if (!const_value_type_matches_(rhs, it->second.value.type)) {
                        return fail_not_evaluable(e.span, "assignment rhs type mismatch in const evaluation");
                    }
                    it->second.value = rhs;
                    outv = rhs;
                    outv.type = inferred_ty;
                    return true;
                }
                case ast::ExprKind::kTernary: {
                    ConstValue cond{};
                    if (!self(self, e.a, cond)) return false;
                    if (cond.kind != ConstValue::Kind::kBool) {
                        return fail_not_evaluable(e.span, "ternary condition must be bool in const evaluation");
                    }
                    if (cond.b) {
                        if (!self(self, e.b, outv)) return false;
                    } else {
                        if (!self(self, e.c, outv)) return false;
                    }
                    outv.type = inferred_ty;
                    return true;
                }
                case ast::ExprKind::kIfExpr: {
                    ConstValue cond{};
                    if (!self(self, e.a, cond)) return false;
                    if (cond.kind != ConstValue::Kind::kBool) {
                        return fail_not_evaluable(e.span, "if-expression condition must be bool in const evaluation");
                    }
                    if (cond.b) {
                        if (!self(self, e.b, outv)) return false;
                    } else {
                        if (!self(self, e.c, outv)) return false;
                    }
                    outv.type = inferred_ty;
                    return true;
                }
                case ast::ExprKind::kLoop:
                    diag_(diag::Code::kConstLoopExprNotSupported, e.span);
                    err_(e.span, "loop expression is not supported in const evaluation; use while statement inside const def");
                    return false;
                case ast::ExprKind::kCall: {
                    ast::StmtId callee_sid = ast::k_invalid_stmt;
                    if ((size_t)eid < expr_overload_target_cache_.size()) {
                        callee_sid = expr_overload_target_cache_[eid];
                    }

                    if (callee_sid == ast::k_invalid_stmt) {
                        if (e.a == ast::k_invalid_expr || (size_t)e.a >= ast_.exprs().size()) {
                            return fail_not_evaluable(e.span, "call callee expression is invalid");
                        }
                        const auto& ce = ast_.expr(e.a);
                        if (ce.kind != ast::ExprKind::kIdent) {
                            return fail_not_evaluable(ce.span, "const evaluation supports direct function identifier calls only");
                        }
                        auto fit = fn_decl_by_name_.find(std::string(ce.text));
                        if (fit != fn_decl_by_name_.end() && fit->second.size() == 1) {
                            callee_sid = fit->second.front();
                        }
                    }
                    if (callee_sid == ast::k_invalid_stmt ||
                        (size_t)callee_sid >= ast_.stmts().size() ||
                        ast_.stmt(callee_sid).kind != ast::StmtKind::kFnDecl) {
                        return fail_not_evaluable(e.span, "call target is not a resolved function declaration");
                    }

                    const auto& args = ast_.args();
                    const uint64_t begin = e.arg_begin;
                    const uint64_t end = begin + e.arg_count;
                    if (begin > args.size() || end > args.size()) {
                        return fail_not_evaluable(e.span, "call argument range is invalid");
                    }

                    std::vector<ConstValue> call_args{};
                    call_args.reserve(e.arg_count);
                    for (uint32_t i = 0; i < e.arg_count; ++i) {
                        const auto& a = args[e.arg_begin + i];
                        if (a.has_label || a.kind == ast::ArgKind::kLabeled || a.is_hole) {
                            return fail_not_evaluable(a.span, "labeled/hole arguments are not supported in const evaluation");
                        }
                        ConstValue av{};
                        if (!self(self, a.expr, av)) return false;
                        call_args.push_back(std::move(av));
                    }

                    if (!eval_const_fn_call_impl_(callee_sid, call_args, outv, e.span, ctx)) {
                        return false;
                    }
                    outv.type = inferred_ty == ty::kInvalidType ? outv.type : inferred_ty;
                    return true;
                }
                default:
                    return fail_not_evaluable(e.span, "unsupported expression kind in const expression");
            }
        };

        return eval(eval, expr_id, out);
    }

    bool TypeChecker::eval_const_fn_call_impl_(
        ast::StmtId fn_sid,
        const std::vector<ConstValue>& args,
        ConstValue& out,
        Span diag_span,
        ConstEvalContext& ctx
    ) {
        if (fn_sid == ast::k_invalid_stmt || (size_t)fn_sid >= ast_.stmts().size()) {
            diag_(diag::Code::kConstExprNotEvaluable, diag_span, "const call target statement is invalid");
            err_(diag_span, "const call target statement is invalid");
            return false;
        }
        const auto& fn = ast_.stmt(fn_sid);
        if (fn.kind != ast::StmtKind::kFnDecl) {
            diag_(diag::Code::kConstExprNotEvaluable, diag_span, "const call target is not a function declaration");
            err_(diag_span, "const call target is not a function declaration");
            return false;
        }
        if (!fn.fn_is_const) {
            diag_(diag::Code::kConstFnCallsNonConstFn, diag_span, fn.name);
            err_(diag_span, "const evaluation can only call const def");
            return false;
        }
        if (fn.a == ast::k_invalid_stmt) {
            diag_(diag::Code::kConstExprNotEvaluable, diag_span, "const def requires body for const evaluation");
            err_(diag_span, "const def requires body for const evaluation");
            return false;
        }
        if (ctx.call_stack.size() >= ctx.call_depth_budget) {
            diag_(diag::Code::kConstEvalCallDepthExceeded, diag_span, fn.name);
            err_(diag_span, "const evaluation call depth limit exceeded");
            return false;
        }

        std::unordered_map<std::string, ConstBinding> env{};
        env.reserve(fn.param_count + 8u);

        const auto bind_param = [&](const ast::Param& p, const ConstValue& v, Span use_span) -> bool {
            if (!const_value_type_matches_(v, p.type)) {
                diag_(diag::Code::kConstExprNotEvaluable, use_span, "const call argument type mismatch");
                err_(use_span, "const call argument type mismatch");
                return false;
            }
            ConstBinding b{};
            b.value = v;
            b.value.type = (b.value.type == ty::kInvalidType) ? p.type : b.value.type;
            b.is_mut = p.is_mut;
            if (!env.insert({std::string(p.name), std::move(b)}).second) {
                diag_(diag::Code::kConstFnBodyUnsupportedStmt, p.span, "duplicate parameter name in const evaluator");
                err_(p.span, "duplicate parameter in const evaluator");
                return false;
            }
            return true;
        };

        if (args.size() > fn.param_count) {
            diag_(diag::Code::kConstExprNotEvaluable, diag_span, "const call argument count mismatch");
            err_(diag_span, "const call argument count mismatch");
            return false;
        }
        for (uint32_t i = 0; i < static_cast<uint32_t>(args.size()); ++i) {
            const auto& p = ast_.params()[fn.param_begin + i];
            if (!bind_param(p, args[i], p.span)) return false;
        }
        for (uint32_t i = static_cast<uint32_t>(args.size()); i < fn.param_count; ++i) {
            const auto& p = ast_.params()[fn.param_begin + i];
            if (!p.has_default || p.default_expr == ast::k_invalid_expr) {
                diag_(diag::Code::kConstExprNotEvaluable, p.span, "const call argument count mismatch");
                err_(p.span, "missing non-default argument in const call");
                return false;
            }
            ConstValue dv{};
            if (!eval_const_expr_value_impl_(p.default_expr, dv, p.span, ctx, &env)) return false;
            if (!bind_param(p, dv, p.span)) return false;
        }

        struct ExecResult {
            bool ok = true;
            bool returned = false;
            bool broke = false;
            bool continued = false;
            ConstValue value{};
        };

        auto fail_stmt = [&](Span sp, std::string_view reason) -> ExecResult {
            diag_(diag::Code::kConstFnBodyUnsupportedStmt, sp, reason);
            err_(sp, std::string("unsupported const def statement: ") + std::string(reason));
            ExecResult r{};
            r.ok = false;
            return r;
        };
        auto eval_expr = [&](ast::ExprId eid, ConstValue& v, Span sp) -> bool {
            return eval_const_expr_value_impl_(eid, v, sp, ctx, &env);
        };
        auto restore_scope = [&](std::unordered_map<std::string, ConstBinding>& saved) {
            for (auto& [name, binding] : saved) {
                auto it_now = env.find(name);
                if (it_now != env.end()) binding = it_now->second;
            }
            env = std::move(saved);
        };
        auto stmt_kind_name = [](ast::StmtKind k) -> const char* {
            switch (k) {
                case ast::StmtKind::kBlock: return "block";
                case ast::StmtKind::kExprStmt: return "expr";
                case ast::StmtKind::kVar: return "var";
                case ast::StmtKind::kIf: return "if";
                case ast::StmtKind::kWhile: return "while";
                case ast::StmtKind::kBreak: return "break";
                case ast::StmtKind::kContinue: return "continue";
                case ast::StmtKind::kReturn: return "return";
                default: return "unsupported";
            }
        };

        auto exec_stmt = [&](auto&& self, ast::StmtId sid) -> ExecResult {
            if (sid == ast::k_invalid_stmt || (size_t)sid >= ast_.stmts().size()) {
                return fail_stmt(diag_span, "invalid statement");
            }
            const auto& s = ast_.stmt(sid);
            switch (s.kind) {
                case ast::StmtKind::kEmpty:
                    return {};
                case ast::StmtKind::kBlock: {
                    std::unordered_map<std::string, ConstBinding> saved = env;
                    const auto& kids = ast_.stmt_children();
                    const uint64_t begin = s.stmt_begin;
                    const uint64_t end = begin + s.stmt_count;
                    if (begin > kids.size() || end > kids.size()) {
                        auto r = fail_stmt(s.span, "invalid block child range");
                        restore_scope(saved);
                        return r;
                    }
                    for (uint32_t i = 0; i < s.stmt_count; ++i) {
                        ExecResult r = self(self, kids[s.stmt_begin + i]);
                        if (!r.ok || r.returned || r.broke || r.continued) {
                            restore_scope(saved);
                            return r;
                        }
                    }
                    restore_scope(saved);
                    return {};
                }
                case ast::StmtKind::kExprStmt: {
                    if (s.expr != ast::k_invalid_expr) {
                        ConstValue ignored{};
                        if (!eval_expr(s.expr, ignored, s.span)) {
                            return fail_stmt(s.span, "expression statement const evaluation failed");
                        }
                    }
                    return {};
                }
                case ast::StmtKind::kVar: {
                    if (s.is_static || s.is_extern || s.is_export || s.name.empty()) {
                        return fail_stmt(s.span, "only local let/set/const declarations are allowed in const def");
                    }
                    if (s.init == ast::k_invalid_expr) {
                        return fail_stmt(s.span, "local const-def variable requires initializer");
                    }
                    if (env.find(std::string(s.name)) != env.end()) {
                        return fail_stmt(s.span, "shadowing is not supported in const evaluator v1");
                    }
                    ConstValue iv{};
                    if (!eval_expr(s.init, iv, s.span)) {
                        return fail_stmt(s.span, "local initializer is not const-evaluable");
                    }
                    if (s.type != ty::kInvalidType && !const_value_type_matches_(iv, s.type)) {
                        return fail_stmt(s.span, "local initializer type mismatch");
                    }
                    ConstBinding b{};
                    b.value = iv;
                    if (b.value.type == ty::kInvalidType) b.value.type = s.type;
                    b.is_mut = (!s.is_const) && (s.is_set || s.is_mut);
                    env.emplace(std::string(s.name), std::move(b));
                    return {};
                }
                case ast::StmtKind::kIf: {
                    ConstValue cond{};
                    if (!eval_expr(s.expr, cond, s.span) || cond.kind != ConstValue::Kind::kBool) {
                        return fail_stmt(s.span, "if condition must be bool in const evaluator");
                    }
                    const ast::StmtId target = cond.b ? s.a : s.b;
                    if (target == ast::k_invalid_stmt) return {};
                    return self(self, target);
                }
                case ast::StmtKind::kWhile: {
                    for (;;) {
                        ConstValue cond{};
                        if (!eval_expr(s.expr, cond, s.span) || cond.kind != ConstValue::Kind::kBool) {
                            return fail_stmt(s.span, "while condition must be bool in const evaluator");
                        }
                        if (!cond.b) break;
                        ExecResult body = self(self, s.a);
                        if (!body.ok) return body;
                        if (body.returned) return body;
                        if (body.broke) break;
                        if (body.continued) continue;
                    }
                    return {};
                }
                case ast::StmtKind::kBreak: {
                    if (s.expr != ast::k_invalid_expr) {
                        return fail_stmt(s.span, "break value is not supported in const evaluator while loop");
                    }
                    ExecResult r{};
                    r.broke = true;
                    return r;
                }
                case ast::StmtKind::kContinue: {
                    ExecResult r{};
                    r.continued = true;
                    return r;
                }
                case ast::StmtKind::kReturn: {
                    ExecResult r{};
                    r.returned = true;
                    if (s.expr == ast::k_invalid_expr) {
                        r.value.kind = ConstValue::Kind::kInvalid;
                        r.value.type = types_.builtin(ty::Builtin::kUnit);
                        return r;
                    }
                    if (!eval_expr(s.expr, r.value, s.span)) {
                        r.ok = false;
                        return r;
                    }
                    return r;
                }
                default:
                    return fail_stmt(s.span, stmt_kind_name(s.kind));
            }
        };

        ctx.call_stack.push_back(fn_sid);
        const ExecResult r = exec_stmt(exec_stmt, fn.a);
        ctx.call_stack.pop_back();
        if (!r.ok) return false;
        if (!r.returned) {
            diag_(diag::Code::kConstFnBodyUnsupportedStmt, fn.span, "missing return");
            err_(fn.span, "const def evaluation reached end without return");
            return false;
        }
        if (!const_value_type_matches_(r.value, fn.fn_ret)) {
            diag_(diag::Code::kConstExprNotEvaluable, fn.span, "const def return type mismatch during evaluation");
            err_(fn.span, "const def return type mismatch during evaluation");
            return false;
        }
        out = r.value;
        if (out.type == ty::kInvalidType) out.type = fn.fn_ret;
        return true;
    }

    bool TypeChecker::eval_const_symbol_value_impl_(
        uint32_t symbol_id,
        ConstValue& out,
        Span diag_span,
        ConstEvalContext& ctx
    ) {
        if (auto it = const_symbol_runtime_values_.find(symbol_id); it != const_symbol_runtime_values_.end()) {
            out = it->second;
            return true;
        }

        const uint8_t state = const_symbol_eval_state_[symbol_id];
        if (state == 1u) {
            if (const_cycle_diag_emitted_.insert(symbol_id).second) {
                diag_(diag::Code::kConstExprCycle, diag_span);
                err_(diag_span, "cyclic const reference detected");
            }
            return false;
        }
        if (state == 2u) {
            if (auto it = const_symbol_runtime_values_.find(symbol_id); it != const_symbol_runtime_values_.end()) {
                out = it->second;
                return true;
            }
            if (auto it = result_.const_symbol_values.find(symbol_id); it != result_.const_symbol_values.end()) {
                ConstValue sv{};
                switch (it->second.kind) {
                    case ConstInitKind::kInt: {
                        errno = 0;
                        char* end = nullptr;
                        const long long v = std::strtoll(it->second.text.c_str(), &end, 10);
                        if (errno == ERANGE || end == nullptr || *end != '\0') return false;
                        sv.kind = ConstValue::Kind::kInt;
                        sv.i64 = static_cast<int64_t>(v);
                        break;
                    }
                    case ConstInitKind::kFloat: {
                        errno = 0;
                        char* end = nullptr;
                        const double v = std::strtod(it->second.text.c_str(), &end);
                        if (errno == ERANGE || end == nullptr || *end != '\0') return false;
                        sv.kind = ConstValue::Kind::kFloat;
                        sv.f64 = v;
                        break;
                    }
                    case ConstInitKind::kBool:
                        sv.kind = ConstValue::Kind::kBool;
                        sv.b = (it->second.text == "true");
                        break;
                    case ConstInitKind::kChar: {
                        errno = 0;
                        char* end = nullptr;
                        const unsigned long v = std::strtoul(it->second.text.c_str(), &end, 10);
                        if (errno == ERANGE || end == nullptr || *end != '\0') return false;
                        sv.kind = ConstValue::Kind::kChar;
                        sv.ch = static_cast<uint32_t>(v);
                        break;
                    }
                    case ConstInitKind::kNone:
                    default:
                        return false;
                }
                if (symbol_id < sym_.symbols().size()) {
                    sv.type = sym_.symbol(symbol_id).declared_type;
                }
                out = sv;
                return true;
            }
            return false;
        }
        if (state == 3u) {
            return false;
        }

        auto dit = const_symbol_decl_sid_.find(symbol_id);
        if (dit == const_symbol_decl_sid_.end()) {
            diag_(diag::Code::kConstExprNotEvaluable, diag_span,
                  "symbol has no const declaration for compile-time evaluation");
            err_(diag_span, "const symbol declaration is missing for evaluation");
            const_symbol_eval_state_[symbol_id] = 3u;
            return false;
        }

        const ast::StmtId sid = dit->second;
        if (sid == ast::k_invalid_stmt || (size_t)sid >= ast_.stmts().size()) {
            diag_(diag::Code::kConstExprNotEvaluable, diag_span, "invalid const declaration statement");
            err_(diag_span, "invalid const declaration statement");
            const_symbol_eval_state_[symbol_id] = 3u;
            return false;
        }

        const auto& s = ast_.stmt(sid);
        if (s.kind != ast::StmtKind::kVar || s.init == ast::k_invalid_expr) {
            diag_(diag::Code::kConstExprNotEvaluable, s.span, "const declaration requires initializer");
            err_(s.span, "const declaration requires initializer");
            const_symbol_eval_state_[symbol_id] = 3u;
            return false;
        }

        const_symbol_eval_state_[symbol_id] = 1u;
        ConstValue value{};
        const bool ok = eval_const_expr_value_impl_(s.init, value, s.span, ctx, nullptr);
        if (!ok) {
            const_symbol_eval_state_[symbol_id] = 3u;
            return false;
        }

        const_symbol_runtime_values_[symbol_id] = value;
        if (ConstInitData scalar{}; const_value_to_scalar_init_(value, scalar)) {
            result_.const_symbol_values[symbol_id] = std::move(scalar);
        }
        out = value;
        const_symbol_eval_state_[symbol_id] = 2u;
        return true;
    }

    TypeChecker::ProtoRequireEvalResult TypeChecker::eval_proto_require_const_bool_(ast::ExprId expr_id) const {
        if (expr_id == ast::k_invalid_expr || (size_t)expr_id >= ast_.exprs().size()) {
            return ProtoRequireEvalResult::kTooComplex;
        }

        struct Fold {
            enum class Kind : uint8_t {
                kBoolConst = 0,
                kNonBool,
                kTooComplex,
            };
            Kind kind = Kind::kTooComplex;
            bool value = false;
        };

        auto eval = [&](auto&& self, ast::ExprId eid) -> Fold {
            if (eid == ast::k_invalid_expr || (size_t)eid >= ast_.exprs().size()) {
                return {Fold::Kind::kTooComplex, false};
            }
            const auto& e = ast_.expr(eid);
            switch (e.kind) {
                case ast::ExprKind::kBoolLit:
                    return {Fold::Kind::kBoolConst, e.text == "true"};

                case ast::ExprKind::kUnary: {
                    if (e.op != K::kKwNot) {
                        return {Fold::Kind::kTooComplex, false};
                    }
                    const Fold inner = self(self, e.a);
                    if (inner.kind == Fold::Kind::kBoolConst) {
                        return {Fold::Kind::kBoolConst, !inner.value};
                    }
                    return inner;
                }

                case ast::ExprKind::kBinary: {
                    const Fold lhs = self(self, e.a);
                    const Fold rhs = self(self, e.b);
                    if (lhs.kind == Fold::Kind::kTooComplex || rhs.kind == Fold::Kind::kTooComplex) {
                        return {Fold::Kind::kTooComplex, false};
                    }
                    if (lhs.kind != Fold::Kind::kBoolConst || rhs.kind != Fold::Kind::kBoolConst) {
                        return {Fold::Kind::kNonBool, false};
                    }
                    if (e.op == K::kKwAnd) {
                        return {Fold::Kind::kBoolConst, lhs.value && rhs.value};
                    }
                    if (e.op == K::kKwOr) {
                        return {Fold::Kind::kBoolConst, lhs.value || rhs.value};
                    }
                    if (e.op == K::kEqEq) {
                        return {Fold::Kind::kBoolConst, lhs.value == rhs.value};
                    }
                    if (e.op == K::kBangEq) {
                        return {Fold::Kind::kBoolConst, lhs.value != rhs.value};
                    }
                    return {Fold::Kind::kTooComplex, false};
                }

                case ast::ExprKind::kIntLit:
                case ast::ExprKind::kFloatLit:
                case ast::ExprKind::kStringLit:
                case ast::ExprKind::kCharLit:
                case ast::ExprKind::kNullLit:
                case ast::ExprKind::kArrayLit:
                case ast::ExprKind::kFieldInit:
                    return {Fold::Kind::kNonBool, false};

                default:
                    return {Fold::Kind::kTooComplex, false};
            }
        };

        const Fold folded = eval(eval, expr_id);
        if (folded.kind == Fold::Kind::kBoolConst) {
            return folded.value ? ProtoRequireEvalResult::kTrue : ProtoRequireEvalResult::kFalse;
        }
        if (folded.kind == Fold::Kind::kNonBool) {
            return ProtoRequireEvalResult::kTypeNotBool;
        }
        return ProtoRequireEvalResult::kTooComplex;
    }

    bool TypeChecker::evaluate_proto_require_at_apply_(
        ast::StmtId proto_sid,
        ty::TypeId owner_type,
        Span apply_span,
        bool emit_unsatisfied_diag,
        bool emit_shape_diag
    ) {
        (void)owner_type;
        (void)emit_unsatisfied_diag;
        (void)emit_shape_diag;
        if (proto_sid == ast::k_invalid_stmt || (size_t)proto_sid >= ast_.stmts().size()) return true;
        const auto& ps = ast_.stmt(proto_sid);
        if (ps.kind != ast::StmtKind::kProtoDecl) return true;

        auto visit_proto = [&](auto&& self, ast::StmtId cur_sid, std::unordered_set<ast::StmtId>& visiting) -> bool {
            if (cur_sid == ast::k_invalid_stmt || (size_t)cur_sid >= ast_.stmts().size()) return true;
            if (!visiting.insert(cur_sid).second) return true;
            const auto& cur = ast_.stmt(cur_sid);
            if (cur.kind != ast::StmtKind::kProtoDecl) return true;

            const auto& refs = ast_.path_refs();
            const uint32_t ib = cur.decl_path_ref_begin;
            const uint32_t ie = cur.decl_path_ref_begin + cur.decl_path_ref_count;
            if (ib <= refs.size() && ie <= refs.size()) {
                for (uint32_t i = ib; i < ie; ++i) {
                    if (auto base_sid = resolve_proto_decl_from_path_ref_(refs[i], apply_span)) {
                        if (!self(self, *base_sid, visiting)) return false;
                    }
                }
            }
            return true;
        };

        std::unordered_set<ast::StmtId> visiting;
        return visit_proto(visit_proto, proto_sid, visiting);
    }

    bool TypeChecker::proto_member_fn_sig_matches_impl_(
        const ast::Stmt& req,
        const ast::Stmt& impl,
        ty::TypeId owner_type
    ) const {
        if (req.kind != ast::StmtKind::kFnDecl || impl.kind != ast::StmtKind::kFnDecl) return false;
        if (req.name != impl.name) return false;
        if (req.param_count != impl.param_count) return false;
        if (req.positional_param_count != impl.positional_param_count) return false;

        std::unordered_map<std::string, ty::TypeId> assoc_bindings{};
        (void)collect_assoc_type_bindings_for_owner_(owner_type, assoc_bindings);

        auto normalize_self = [&](ty::TypeId t) -> ty::TypeId {
            return substitute_self_and_assoc_type_(t, owner_type, &assoc_bindings);
        };

        auto is_receiver_semantic_param = [&](const ast::Param& p) -> bool {
            if (p.is_self) return true;
            if (p.type == ty::kInvalidType) return false;
            const auto& pt = types_.get(p.type);
            if (pt.kind == ty::Kind::kNamedUser && is_self_named_type_(p.type)) return true;
            if ((pt.kind == ty::Kind::kBorrow || pt.kind == ty::Kind::kEscape) &&
                pt.elem != ty::kInvalidType) {
                const auto& et = types_.get(pt.elem);
                return et.kind == ty::Kind::kNamedUser && is_self_named_type_(pt.elem);
            }
            return false;
        };

        if (normalize_self(req.fn_ret) != normalize_self(impl.fn_ret)) return false;
        for (uint32_t i = 0; i < req.param_count; ++i) {
            const auto& rp = ast_.params()[req.param_begin + i];
            const auto& ip = ast_.params()[impl.param_begin + i];
            if (normalize_self(rp.type) != normalize_self(ip.type)) return false;
            if (is_receiver_semantic_param(rp) != is_receiver_semantic_param(ip)) return false;
        }
        return true;
    }

    bool TypeChecker::proto_requirement_satisfied_by_default_acts_(
        ast::StmtId req_sid,
        ty::TypeId owner_type
    ) const {
        if (req_sid == ast::k_invalid_stmt || static_cast<size_t>(req_sid) >= ast_.stmts().size()) {
            return false;
        }
        if (owner_type == ty::kInvalidType) return false;
        owner_type = canonicalize_acts_owner_type_(owner_type);
        auto oit = acts_default_method_map_.find(owner_type);
        if (oit == acts_default_method_map_.end()) return false;
        const auto& req = ast_.stmt(req_sid);
        auto mit = oit->second.find(std::string(req.name));
        if (mit == oit->second.end()) return false;
        for (const auto& cand : mit->second) {
            if (cand.fn_sid == ast::k_invalid_stmt) continue;
            if (cand.from_named_set) continue;
            if (static_cast<size_t>(cand.fn_sid) >= ast_.stmts().size()) continue;
            if (proto_member_fn_sig_matches_impl_(req, ast_.stmt(cand.fn_sid), owner_type)) {
                return true;
            }
        }
        return false;
    }

    bool TypeChecker::proto_assoc_requirement_satisfied_by_default_acts_(
        const ast::Stmt& req,
        ty::TypeId owner_type
    ) const {
        if (req.kind != ast::StmtKind::kAssocTypeDecl) return false;
        if (req.assoc_type_role != ast::AssocTypeRole::kProtoRequire) return false;
        if (req.name.empty()) return false;
        return lookup_acts_assoc_type_binding_(owner_type, req.name).has_value();
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
        std::vector<ast::StmtId> proto_provide_fn_members;
        if (mb <= kids.size() && me <= kids.size()) {
            for (uint32_t i = 0; i < s.stmt_count; ++i) {
                const ast::StmtId msid = kids[s.stmt_begin + i];
                if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                const auto& m = ast_.stmt(msid);

                if (m.kind == ast::StmtKind::kFnDecl) {
                    if (m.fn_is_operator) {
                        diag_(diag::Code::kProtoOperatorNotAllowed, m.span);
                        err_(m.span, "operator declarations are not allowed in proto");
                    }

                    if (m.proto_fn_role == ast::ProtoFnRole::kRequire) {
                        if (m.a != ast::k_invalid_stmt) {
                            diag_(diag::Code::kProtoMemberBodyNotAllowed, m.span);
                            err_(m.span, "proto require function must not define a body");
                        }
                    } else if (m.proto_fn_role == ast::ProtoFnRole::kProvide) {
                        if (m.a == ast::k_invalid_stmt) {
                            diag_(diag::Code::kUnexpectedToken, m.span,
                                  "provide def member must define a body");
                            err_(m.span, "proto provide function requires a body");
                        } else {
                            proto_provide_fn_members.push_back(msid);
                        }
                    } else {
                        diag_(diag::Code::kUnexpectedToken, m.span,
                              "proto function member must begin with require/provide");
                        err_(m.span, "proto function member must begin with require/provide");
                    }
                    continue;
                }

                if (m.kind == ast::StmtKind::kRequire) {
                    diag_(diag::Code::kUnexpectedToken, m.span,
                          "proto kind-require is removed; use import or 'require type'");
                    err_(m.span, "proto kind-require is removed");
                    continue;
                }

                if (m.kind == ast::StmtKind::kAssocTypeDecl) {
                    if (m.assoc_type_role != ast::AssocTypeRole::kProtoRequire) {
                        diag_(diag::Code::kUnexpectedToken, m.span, "require type Item;");
                        err_(m.span, "proto associated type member must begin with 'require type'");
                    } else if (m.name.empty()) {
                        diag_(diag::Code::kTypeNameExpected, m.span);
                        err_(m.span, "proto associated type requires a name");
                    }
                    continue;
                }

                if (m.kind == ast::StmtKind::kVar) {
                    if (!m.var_is_proto_provide || !m.is_const) {
                        diag_(diag::Code::kUnexpectedToken, m.span,
                              "only provide const declarations are allowed as proto variables");
                        err_(m.span, "proto variable member must be provide const");
                        continue;
                    }
                    if (!m.is_static) {
                        diag_(diag::Code::kUnexpectedToken, m.span,
                              "proto provide const must be static");
                        err_(m.span, "proto provide const must be static");
                    }
                    if (m.type == ty::kInvalidType) {
                        diag_(diag::Code::kVarDeclTypeAnnotationRequired, m.span);
                        err_(m.span, "proto provide const requires explicit type");
                    }
                    if (m.init == ast::k_invalid_expr) {
                        diag_(diag::Code::kVarDeclInitializerExpected, m.span);
                        err_(m.span, "proto provide const requires initializer");
                    } else {
                        const CoercionPlan init_plan = classify_assign_with_coercion_(
                            AssignSite::LetInit, m.type, m.init, m.span);
                        const ty::TypeId init_t = init_plan.src_after;
                        if (m.type != ty::kInvalidType && !init_plan.ok) {
                            diag_(diag::Code::kTypeLetInitMismatch, m.span,
                                  m.name, types_.to_string(m.type), type_for_user_diag_(init_t, m.init));
                            err_(m.span, "proto provide const init mismatch");
                        }
                        ConstValue folded{};
                        if (!eval_const_expr_value_(m.init, folded, m.span)) {
                            err_(m.span, "proto provide const initializer must be compile-time evaluable");
                        }
                    }
                    continue;
                }

                diag_(diag::Code::kUnexpectedToken, m.span, "proto member");
                err_(m.span, "unsupported proto member");
            }
        }

        for (const ast::StmtId msid : proto_provide_fn_members) {
            if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
            check_stmt_fn_decl_(msid, ast_.stmt(msid));
        }

        // Declaration-time validation for proto body require items.
        (void)evaluate_proto_require_at_apply_(
            sid, ty::kInvalidType, s.span,
            /*emit_unsatisfied_diag=*/false,
            /*emit_shape_diag=*/true);

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
        {
            std::unordered_set<std::string> generic_params;
            for (const auto& name : collect_decl_generic_param_names_(s)) {
                generic_params.insert(name);
            }
            (void)validate_constraint_clause_decl_(s.decl_constraint_begin, s.decl_constraint_count, generic_params, s.span);
        }
        if (generic_class_template_sid_set_.find(sid) != generic_class_template_sid_set_.end()) {
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

        auto normalize_self = [&](ty::TypeId t) -> ty::TypeId {
            if (t == ty::kInvalidType) return t;
            const auto& tt = types_.get(t);
            if (tt.kind == ty::Kind::kNamedUser && is_self_named_type_(t)) {
                return self_ty;
            }
            if (tt.kind == ty::Kind::kBorrow) {
                const auto& et = types_.get(tt.elem);
                if (et.kind == ty::Kind::kNamedUser && is_self_named_type_(tt.elem)) {
                    return types_.make_borrow(self_ty, tt.borrow_is_mut);
                }
            }
            return t;
        };

        auto fn_sig_matches = [&](const ast::Stmt& req, const ast::Stmt& impl) -> bool {
            return proto_member_fn_sig_matches_impl_(req, impl, self_ty);
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
                    if (m.kind == ast::StmtKind::kFnDecl &&
                        m.proto_fn_role == ast::ProtoFnRole::kProvide &&
                        m.a != ast::k_invalid_stmt) {
                        out.push_back(msid);
                    }
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

                    if (!is_storage_safe_owner_container_type_(fm.type)) {
                        std::ostringstream oss;
                        oss << "class field member '" << fm.name
                            << "' must use a POD builtin value type, `~T`/`(~T)?`, a recursively-sized owner array, or a storage-safe named aggregate in this round, got "
                            << types_.to_string(fm.type);
                        diag_(diag::Code::kTypeFieldMemberMustBePodBuiltin, fm.span, fm.name, types_.to_string(fm.type));
                        err_(fm.span, oss.str());
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
        ensure_generic_acts_for_owner_(self_ty, s.span);

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
                    if (ins.ok && m.is_const) {
                        // class body const-eval에서 bare member 식별자(`A + 1`)가
                        // 현재 스코프의 사전등록 심볼로 먼저 해석되므로,
                        // 로컬 predecl 심볼도 const decl 맵에 연결해야 한다.
                        const_symbol_decl_sid_[ins.symbol_id] = msid;
                    }
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

            const std::string class_qname = [&]() -> std::string {
                if (auto it = class_qualified_name_by_stmt_.find(sid);
                    it != class_qualified_name_by_stmt_.end()) {
                    return it->second;
                }
                return std::string(s.name);
            }();

            for (uint32_t i = begin; i < end; ++i) {
                const ast::StmtId msid = kids[i];
                if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                const auto& m = ast_.stmt(msid);
                class_visibility_owner_stack_.push_back(sid);
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

                    if (m.is_const) {
                        std::string vqname = class_qname;
                        if (!vqname.empty()) vqname += "::";
                        vqname += std::string(m.name);
                        uint32_t global_sym = sema::SymbolTable::kNoScope;
                        if (auto sid_existing = sym_.lookup(vqname)) {
                            const auto& existing = sym_.symbol(*sid_existing);
                            if (existing.kind == sema::SymbolKind::kVar) {
                                global_sym = *sid_existing;
                            }
                        }
                        if (global_sym != sema::SymbolTable::kNoScope) {
                            const_symbol_decl_sid_[global_sym] = msid;
                            if (m.init != ast::k_invalid_expr) {
                                ConstValue init_value{};
                                if (eval_const_symbol_value_(global_sym, init_value, m.span)) {
                                    ConstInitData scalar_init{};
                                    if (const_value_to_scalar_init_(init_value, scalar_init)) {
                                        result_.const_symbol_values[global_sym] = scalar_init;
                                    } else {
                                        diag_(diag::Code::kConstGlobalCompositeNotSupported, m.span, vqname);
                                        err_(m.span, "class static const composite initializer is not supported in v1");
                                    }
                                }
                            }
                        }
                    }
                }
                class_visibility_owner_stack_.pop_back();
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
            const bool key_had_alias = rewrite_imported_path_(key).has_value();
            const bool key_rewritten = apply_imported_path_rewrite_(key);
            if (!key_had_alias && qualified_path_requires_import_(key)) {
                return false;
            }
            if (proto_decl_by_name_.find(key) != proto_decl_by_name_.end()) return false;
            if (auto sid2 = key_rewritten ? sym_.lookup(key) : lookup_symbol_(key)) {
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
                if (is_builtin_family_proto_(*proto_sid)) {
                    diag_(diag::Code::kTypeErrorGeneric, pr.span,
                          "builtin proto '" + proto_path + "' is reserved for primitive family constraints");
                    err_(pr.span, "builtin proto is reserved for primitive family constraints");
                    continue;
                }
                if (!evaluate_proto_require_at_apply_(*proto_sid, self_ty, pr.span,
                                                      /*emit_unsatisfied_diag=*/true,
                                                      /*emit_shape_diag=*/true)) {
                    // When require(...) itself is not satisfied, suppress derived
                    // member-missing diagnostics for this proto.
                    continue;
                }

                bool proto_impl_ok = true;
                std::vector<ast::StmtId> required;
                std::vector<ast::StmtId> provided;
                std::unordered_set<ast::StmtId> visiting;
                collect_required(collect_required, *proto_sid, required, visiting);
                visiting.clear();
                collect_default_members(collect_default_members, *proto_sid, provided, visiting);
                for (const ast::StmtId req_sid : required) {
                    if (req_sid == ast::k_invalid_stmt || (size_t)req_sid >= ast_.stmts().size()) continue;
                    const auto& req = ast_.stmt(req_sid);

                    if (req.kind == ast::StmtKind::kAssocTypeDecl) {
                        if (proto_assoc_requirement_satisfied_by_default_acts_(req, self_ty)) {
                            continue;
                        }
                        diag_(diag::Code::kProtoImplMissingMember, req.span, req.name);
                        err_(req.span, "missing proto associated type binding: " + std::string(req.name));
                        proto_impl_ok = false;
                        continue;
                    }

                    bool satisfied_by_provide = false;
                    for (const ast::StmtId prov_sid : provided) {
                        if (prov_sid == ast::k_invalid_stmt || (size_t)prov_sid >= ast_.stmts().size()) continue;
                        const auto& prov = ast_.stmt(prov_sid);
                        if (fn_sig_matches(req, prov)) {
                            satisfied_by_provide = true;
                            break;
                        }
                    }
                    if (satisfied_by_provide) continue;

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
                    if (!matched &&
                        proto_requirement_satisfied_by_default_acts_(req_sid, self_ty)) {
                        matched = true;
                    }
                    if (!matched) {
                        diag_(diag::Code::kProtoImplMissingMember, req.span, req.name);
                        err_(req.span, "missing proto member implementation: " + std::string(req.name));
                        proto_impl_ok = false;
                    }
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
