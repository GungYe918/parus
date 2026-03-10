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
            if (s.is_throwing) {
                diag_(diag::Code::kAbiCThrowingFnNotAllowed, s.span, s.name);
                err_(s.span, "C ABI function must not be throwing ('?'); convert exception channel at boundary");
            }

            auto check_enum_direct_c_abi = [&](ty::TypeId t, Span sp, std::string_view what) {
                (void)ensure_generic_enum_instance_from_type_(t, sp);
                if (enum_abi_meta_by_type_.find(t) != enum_abi_meta_by_type_.end()) {
                    diag_(diag::Code::kEnumCAbiDirectSignatureForbidden, sp, what);
                    err_(sp, "enum direct type in C ABI signature is forbidden in v0; cast at boundary (as i32)");
                }
            };

            if (s.has_named_group || s.positional_param_count != s.param_count) {
                diag_(diag::Code::kAbiCNamedGroupNotAllowed, s.span, s.name);
                err_(s.span, "C ABI function must not use named-group parameters: " + std::string(s.name));
            }

            check_enum_direct_c_abi(ret, s.span, std::string("return type of '") + std::string(s.name) + "'");
            (void)ensure_generic_field_instance_from_type_(ret, s.span);
            if (!is_c_abi_safe_type_(ret, /*allow_void=*/true)) {
                diag_(diag::Code::kAbiCTypeNotFfiSafe, s.span,
                    std::string("return type of '") + std::string(s.name) + "'",
                    types_.to_string(ret));
                err_(s.span, "C ABI return type is not FFI-safe");
            }

            for (uint32_t i = 0; i < s.param_count; ++i) {
                const auto& p = ast_.params()[s.param_begin + i];
                check_enum_direct_c_abi(p.type, p.span, std::string("parameter '") + std::string(p.name) + "'");
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
        const OwnershipStateMap saved_ownership = capture_ownership_state_();
        sym_.push_scope();

        FnCtx saved = fn_ctx_;
        fn_ctx_.in_fn = true;
        fn_ctx_.is_pure = s.is_pure;
        fn_ctx_.is_comptime = s.is_comptime;
        fn_ctx_.is_throwing = s.is_throwing;
        fn_ctx_.has_exception_construct = false;
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
                    case ast::StmtKind::kThrow:
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

                    case ast::StmtKind::kTryCatch: {
                        // v0 정책: try body와 모든 catch body가 return하면 always-return으로 본다.
                        if (st.a == ast::k_invalid_stmt) return false;
                        if (!self(self, st.a)) return false;
                        if (st.catch_clause_count == 0) return false;
                        const auto& catches = ast_.try_catch_clauses();
                        const uint32_t cb = st.catch_clause_begin;
                        const uint32_t ce = st.catch_clause_begin + st.catch_clause_count;
                        if (ce > catches.size()) return false;
                        for (uint32_t i = cb; i < ce; ++i) {
                            const auto& cc = catches[i];
                            if (cc.body == ast::k_invalid_stmt) return false;
                            if (!self(self, cc.body)) return false;
                        }
                        return true;
                    }

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
        restore_ownership_state_(saved_ownership);
    }

    namespace {

        struct ConstFoldValue {
            enum class Kind : uint8_t {
                kInvalid = 0,
                kInt,
                kFloat,
                kBool,
                kChar,
            };

            Kind kind = Kind::kInvalid;
            int64_t i64 = 0;
            double f64 = 0.0;
            bool b = false;
            uint32_t ch = 0;
        };

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

        static bool const_init_to_fold_(const ConstInitData& in, ConstFoldValue& out) {
            switch (in.kind) {
                case ConstInitKind::kInt: {
                    errno = 0;
                    char* end = nullptr;
                    const long long v = std::strtoll(in.text.c_str(), &end, 10);
                    if (errno == ERANGE || end == nullptr || *end != '\0') return false;
                    out.kind = ConstFoldValue::Kind::kInt;
                    out.i64 = static_cast<int64_t>(v);
                    return true;
                }
                case ConstInitKind::kFloat: {
                    errno = 0;
                    char* end = nullptr;
                    const double v = std::strtod(in.text.c_str(), &end);
                    if (errno == ERANGE || end == nullptr || *end != '\0') return false;
                    out.kind = ConstFoldValue::Kind::kFloat;
                    out.f64 = v;
                    return true;
                }
                case ConstInitKind::kBool:
                    out.kind = ConstFoldValue::Kind::kBool;
                    out.b = (in.text == "true");
                    return true;
                case ConstInitKind::kChar: {
                    errno = 0;
                    char* end = nullptr;
                    const unsigned long v = std::strtoul(in.text.c_str(), &end, 10);
                    if (errno == ERANGE || end == nullptr || *end != '\0') return false;
                    out.kind = ConstFoldValue::Kind::kChar;
                    out.ch = static_cast<uint32_t>(v);
                    return true;
                }
                case ConstInitKind::kNone:
                default:
                    return false;
            }
        }

        static bool fold_to_const_init_(const ConstFoldValue& in, ConstInitData& out) {
            switch (in.kind) {
                case ConstFoldValue::Kind::kInt:
                    out.kind = ConstInitKind::kInt;
                    out.text = std::to_string(in.i64);
                    return true;
                case ConstFoldValue::Kind::kFloat:
                    out.kind = ConstInitKind::kFloat;
                    out.text = format_const_float_(in.f64);
                    return true;
                case ConstFoldValue::Kind::kBool:
                    out.kind = ConstInitKind::kBool;
                    out.text = in.b ? "true" : "false";
                    return true;
                case ConstFoldValue::Kind::kChar:
                    out.kind = ConstInitKind::kChar;
                    out.text = std::to_string(in.ch);
                    return true;
                case ConstFoldValue::Kind::kInvalid:
                default:
                    return false;
            }
        }

    } // namespace

    bool TypeChecker::eval_const_expr_(ast::ExprId expr_id, ConstInitData& out, Span diag_span) {
        auto fail_not_evaluable = [&](Span sp, std::string_view reason) -> bool {
            diag_(diag::Code::kConstExprNotEvaluable, sp, reason);
            err_(sp, std::string("const expression is not evaluable: ") + std::string(reason));
            return false;
        };
        auto fail_call_not_supported = [&](Span sp) -> bool {
            diag_(diag::Code::kConstExprCallNotSupported, sp);
            err_(sp, "function call is not supported in const expression");
            return false;
        };

        auto eval = [&](auto&& self, ast::ExprId eid, ConstFoldValue& outv) -> bool {
            if (eid == ast::k_invalid_expr || (size_t)eid >= ast_.exprs().size()) {
                return fail_not_evaluable(diag_span, "invalid expression");
            }

            const auto& e = ast_.expr(eid);
            switch (e.kind) {
                case ast::ExprKind::kIntLit: {
                    int64_t v = 0;
                    std::string canonical;
                    if (!parse_int_literal_i64_(e.text, v, canonical)) {
                        return fail_not_evaluable(e.span, "invalid integer literal for const expression");
                    }
                    outv.kind = ConstFoldValue::Kind::kInt;
                    outv.i64 = v;
                    return true;
                }
                case ast::ExprKind::kFloatLit: {
                    double v = 0.0;
                    std::string canonical;
                    if (!parse_float_literal_f64_(e.text, v, canonical)) {
                        return fail_not_evaluable(e.span, "invalid float literal for const expression");
                    }
                    outv.kind = ConstFoldValue::Kind::kFloat;
                    outv.f64 = v;
                    return true;
                }
                case ast::ExprKind::kBoolLit:
                    outv.kind = ConstFoldValue::Kind::kBool;
                    outv.b = (e.text == "true");
                    return true;
                case ast::ExprKind::kCharLit: {
                    uint32_t ch = 0;
                    if (!parse_char_literal_scalar_(e.text, ch)) {
                        return fail_not_evaluable(e.span, "invalid char literal for const expression");
                    }
                    outv.kind = ConstFoldValue::Kind::kChar;
                    outv.ch = ch;
                    return true;
                }
                case ast::ExprKind::kIdent: {
                    auto sid = lookup_symbol_(e.text);
                    if (!sid.has_value()) {
                        return fail_not_evaluable(e.span, "unresolved identifier in const expression");
                    }
                    if (auto it = result_.const_symbol_values.find(*sid); it != result_.const_symbol_values.end()) {
                        if (!const_init_to_fold_(it->second, outv)) {
                            return fail_not_evaluable(e.span, "failed to decode referenced const value");
                        }
                        return true;
                    }
                    if (const_symbol_decl_sid_.find(*sid) != const_symbol_decl_sid_.end()) {
                        ConstInitData cv{};
                        if (!eval_const_symbol_(*sid, cv, e.span)) return false;
                        if (!const_init_to_fold_(cv, outv)) {
                            return fail_not_evaluable(e.span, "failed to decode const symbol value");
                        }
                        return true;
                    }
                    return fail_not_evaluable(e.span, "reference to non-const symbol in const expression");
                }
                case ast::ExprKind::kUnary: {
                    ConstFoldValue in{};
                    if (!self(self, e.a, in)) return false;
                    if (e.op == K::kPlus) {
                        if (in.kind == ConstFoldValue::Kind::kInt ||
                            in.kind == ConstFoldValue::Kind::kFloat ||
                            in.kind == ConstFoldValue::Kind::kChar) {
                            outv = in;
                            return true;
                        }
                        return fail_not_evaluable(e.span, "unary '+' requires numeric const operand");
                    }
                    if (e.op == K::kMinus) {
                        if (in.kind == ConstFoldValue::Kind::kInt) {
                            outv.kind = ConstFoldValue::Kind::kInt;
                            outv.i64 = -in.i64;
                            return true;
                        }
                        if (in.kind == ConstFoldValue::Kind::kFloat) {
                            outv.kind = ConstFoldValue::Kind::kFloat;
                            outv.f64 = -in.f64;
                            return true;
                        }
                        if (in.kind == ConstFoldValue::Kind::kChar) {
                            outv.kind = ConstFoldValue::Kind::kInt;
                            outv.i64 = -static_cast<int64_t>(in.ch);
                            return true;
                        }
                        return fail_not_evaluable(e.span, "unary '-' requires numeric const operand");
                    }
                    if (e.op == K::kKwNot) {
                        if (in.kind != ConstFoldValue::Kind::kBool) {
                            return fail_not_evaluable(e.span, "logical not requires bool const operand");
                        }
                        outv.kind = ConstFoldValue::Kind::kBool;
                        outv.b = !in.b;
                        return true;
                    }
                    if (e.op == K::kBang) {
                        if (in.kind != ConstFoldValue::Kind::kInt) {
                            return fail_not_evaluable(e.span, "bitwise not requires integer const operand");
                        }
                        outv.kind = ConstFoldValue::Kind::kInt;
                        outv.i64 = ~in.i64;
                        return true;
                    }
                    return fail_not_evaluable(e.span, "unsupported unary operator in const expression");
                }
                case ast::ExprKind::kBinary: {
                    ConstFoldValue lhs{};
                    ConstFoldValue rhs{};
                    if (!self(self, e.a, lhs)) return false;
                    if (!self(self, e.b, rhs)) return false;

                    auto is_numeric = [](ConstFoldValue::Kind k) {
                        return k == ConstFoldValue::Kind::kInt ||
                               k == ConstFoldValue::Kind::kFloat ||
                               k == ConstFoldValue::Kind::kChar;
                    };
                    auto as_i64 = [](const ConstFoldValue& v, int64_t& out_i) -> bool {
                        if (v.kind == ConstFoldValue::Kind::kInt) {
                            out_i = v.i64;
                            return true;
                        }
                        if (v.kind == ConstFoldValue::Kind::kChar) {
                            out_i = static_cast<int64_t>(v.ch);
                            return true;
                        }
                        return false;
                    };
                    auto as_f64 = [&](const ConstFoldValue& v, double& out_f) -> bool {
                        if (v.kind == ConstFoldValue::Kind::kFloat) {
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
                        if (lhs.kind == ConstFoldValue::Kind::kFloat || rhs.kind == ConstFoldValue::Kind::kFloat) {
                            double lf = 0.0;
                            double rf = 0.0;
                            if (!as_f64(lhs, lf) || !as_f64(rhs, rf)) {
                                return fail_not_evaluable(e.span, "failed numeric conversion in const comparison");
                            }
                            outv.kind = ConstFoldValue::Kind::kBool;
                            outv.b = cmp(lf, rf);
                            return true;
                        }
                        int64_t li = 0;
                        int64_t ri = 0;
                        if (!as_i64(lhs, li) || !as_i64(rhs, ri)) {
                            return fail_not_evaluable(e.span, "failed integer conversion in const comparison");
                        }
                        outv.kind = ConstFoldValue::Kind::kBool;
                        outv.b = cmp(li, ri);
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
                            if (lhs.kind == ConstFoldValue::Kind::kFloat || rhs.kind == ConstFoldValue::Kind::kFloat) {
                                double lf = 0.0;
                                double rf = 0.0;
                                if (!as_f64(lhs, lf) || !as_f64(rhs, rf)) {
                                    return fail_not_evaluable(e.span, "failed numeric conversion in const arithmetic");
                                }
                                if (e.op == K::kSlash && rf == 0.0) {
                                    return fail_not_evaluable(e.span, "division by zero in const expression");
                                }
                                outv.kind = ConstFoldValue::Kind::kFloat;
                                if (e.op == K::kPlus) outv.f64 = lf + rf;
                                else if (e.op == K::kMinus) outv.f64 = lf - rf;
                                else if (e.op == K::kStar) outv.f64 = lf * rf;
                                else outv.f64 = lf / rf;
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
                            outv.kind = ConstFoldValue::Kind::kInt;
                            if (e.op == K::kPlus) outv.i64 = li + ri;
                            else if (e.op == K::kMinus) outv.i64 = li - ri;
                            else if (e.op == K::kStar) outv.i64 = li * ri;
                            else outv.i64 = li / ri;
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
                            outv.kind = ConstFoldValue::Kind::kInt;
                            outv.i64 = li % ri;
                            return true;
                        }
                        case K::kEqEq:
                        case K::kBangEq: {
                            bool eq = false;
                            if (lhs.kind == ConstFoldValue::Kind::kBool && rhs.kind == ConstFoldValue::Kind::kBool) {
                                eq = (lhs.b == rhs.b);
                            } else if (is_numeric(lhs.kind) && is_numeric(rhs.kind)) {
                                if (lhs.kind == ConstFoldValue::Kind::kFloat || rhs.kind == ConstFoldValue::Kind::kFloat) {
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
                            outv.kind = ConstFoldValue::Kind::kBool;
                            outv.b = (e.op == K::kEqEq) ? eq : !eq;
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
                            if (lhs.kind != ConstFoldValue::Kind::kBool || rhs.kind != ConstFoldValue::Kind::kBool) {
                                return fail_not_evaluable(e.span, "logical operator requires bool const operands");
                            }
                            outv.kind = ConstFoldValue::Kind::kBool;
                            outv.b = (e.op == K::kKwAnd) ? (lhs.b && rhs.b) : (lhs.b || rhs.b);
                            return true;
                        }
                        default:
                            return fail_not_evaluable(e.span, "unsupported binary operator in const expression");
                    }
                }
                case ast::ExprKind::kCall:
                    return fail_call_not_supported(e.span);
                default:
                    return fail_not_evaluable(e.span, "unsupported expression kind in const expression");
            }
        };

        ConstFoldValue value{};
        if (!eval(eval, expr_id, value)) return false;
        if (!fold_to_const_init_(value, out)) {
            return fail_not_evaluable(diag_span, "failed to materialize const value");
        }
        return true;
    }

    bool TypeChecker::eval_const_symbol_(uint32_t symbol_id, ConstInitData& out, Span diag_span) {
        if (auto it = result_.const_symbol_values.find(symbol_id); it != result_.const_symbol_values.end()) {
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
            if (auto it = result_.const_symbol_values.find(symbol_id); it != result_.const_symbol_values.end()) {
                out = it->second;
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
        ConstInitData value{};
        const bool ok = eval_const_expr_(s.init, value, s.span);
        if (!ok) {
            const_symbol_eval_state_[symbol_id] = 3u;
            return false;
        }

        result_.const_symbol_values[symbol_id] = value;
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
        (void)owner_type; // v1: reserved for Self-aware require evaluation.
        if (proto_sid == ast::k_invalid_stmt || (size_t)proto_sid >= ast_.stmts().size()) return true;
        const auto& ps = ast_.stmt(proto_sid);
        if (ps.kind != ast::StmtKind::kProtoDecl) return true;
        if (!ps.proto_has_require || ps.proto_require_expr == ast::k_invalid_expr) return true;

        const ast::ExprId req_eid = ps.proto_require_expr;
        const Span req_span = ((size_t)req_eid < ast_.exprs().size()) ? ast_.expr(req_eid).span : apply_span;
        const ProtoRequireEvalResult status = eval_proto_require_const_bool_(req_eid);
        switch (status) {
            case ProtoRequireEvalResult::kTrue:
                return true;
            case ProtoRequireEvalResult::kFalse:
                if (emit_unsatisfied_diag) {
                    diag_(diag::Code::kProtoConstraintUnsatisfied, apply_span, std::string(ps.name));
                    err_(apply_span, "proto require(...) evaluated to false at apply-site");
                }
                return false;
            case ProtoRequireEvalResult::kTypeNotBool:
                if (emit_shape_diag && proto_require_type_diag_emitted_.insert(req_eid).second) {
                    diag_(diag::Code::kProtoRequireTypeNotBool, req_span);
                    err_(req_span, "require(...) expression must be bool");
                }
                return false;
            case ProtoRequireEvalResult::kTooComplex:
                if (emit_shape_diag && proto_require_complex_diag_emitted_.insert(req_eid).second) {
                    diag_(diag::Code::kProtoRequireExprTooComplex, req_span);
                    err_(req_span,
                         "require(...) supports only true/false/not/and/or/==/!= in v1");
                }
                return false;
        }
        return false;
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
            // Declaration-time check validates require expression shape only.
            // Truth value is enforced at proto apply-site (class/struct/enum/generic concrete).
            (void)evaluate_proto_require_at_apply_(
                sid, ty::kInvalidType, s.span,
                /*emit_unsatisfied_diag=*/false,
                /*emit_shape_diag=*/true);
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
                                ConstInitData init_value{};
                                if (eval_const_symbol_(global_sym, init_value, m.span)) {
                                    result_.const_symbol_values[global_sym] = init_value;
                                }
                            }
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
                if (!evaluate_proto_require_at_apply_(*proto_sid, self_ty, pr.span,
                                                      /*emit_unsatisfied_diag=*/true,
                                                      /*emit_shape_diag=*/true)) {
                    // When require(...) itself is not satisfied, suppress derived
                    // member-missing diagnostics for this proto.
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
