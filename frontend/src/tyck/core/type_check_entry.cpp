// frontend/src/tyck/type_check_entry.cpp
#include <parus/tyck/TypeCheck.hpp>
#include <parus/syntax/TokenKind.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/diag/DiagCode.hpp>
#include "../common/type_check_literals.hpp"

#include <sstream>
#include <cctype>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>


namespace parus::tyck {

    using K = parus::syntax::TokenKind;
    using detail::ParsedFloatLiteral;
    using detail::ParsedIntLiteral;
    using detail::parse_float_literal_;
    using detail::parse_int_literal_;

    TyckResult TypeChecker::check_program(ast::StmtId program_stmt) {
        // -----------------------------
        // HARD RESET (매 호출 독립 보장)
        // -----------------------------
        result_ = TyckResult{};
        loop_stack_.clear();
        stmt_loop_depth_ = 0;
        fn_ctx_ = FnCtx{};
        pending_int_sym_.clear();
        pending_int_expr_.clear();
        sym_is_mut_.clear();
        acts_default_operator_map_.clear();
        acts_default_method_map_.clear();
        acts_named_decl_by_owner_and_name_.clear();
        acts_selection_scope_stack_.clear();
        acts_selection_by_symbol_.clear();
        field_abi_meta_by_type_.clear();
        fn_qualified_name_by_stmt_.clear();
        class_effective_method_map_.clear();
        namespace_stack_.clear();
        import_alias_to_path_.clear();
        known_namespace_paths_.clear();
        import_alias_scope_stack_.clear();
        class_decl_by_name_.clear();
        class_decl_by_type_.clear();
        class_deinit_target_types_.clear();
        class_member_fn_sid_set_.clear();
        proto_member_fn_sid_set_.clear();
        block_depth_ = 0;

        // sym_ 초기화:
        // - bundle/module prepass 심볼이 있으면 시드 심볼테이블을 그대로 사용한다.
        // - 없으면 빈 심볼테이블로 시작한다.
        if (seed_sym_ != nullptr) {
            sym_ = *seed_sym_;
        } else {
            sym_ = sema::SymbolTable{};
        }

        // expr type cache: AST exprs 크기에 맞춰 리셋
        expr_type_cache_.assign(ast_.exprs().size(), ty::kInvalidType);
        expr_overload_target_cache_.assign(ast_.exprs().size(), ast::k_invalid_stmt);
        expr_ctor_owner_type_cache_.assign(ast_.exprs().size(), ty::kInvalidType);
        result_.expr_types = expr_type_cache_; // 결과 벡터도 동일 크기로 시작
        result_.expr_overload_target = expr_overload_target_cache_;
        result_.expr_ctor_owner_type = expr_ctor_owner_type_cache_;

        // string literal은 builtin text 타입으로 고정한다.
        if (string_type_ == ty::kInvalidType) {
            string_type_ = types_.builtin(ty::Builtin::kText);
        }

        // ------------------------------------------
        // Sanity: program은 Block이어야 한다 (정책)
        // ------------------------------------------
        if (program_stmt == ast::k_invalid_stmt) {
            result_.ok = false;
            return result_;
        }

        const ast::Stmt& root = ast_.stmt(program_stmt);
        if (root.kind != ast::StmtKind::kBlock) {
            if (diag_bag_) diag_(diag::Code::kTopLevelMustBeBlock, root.span);
            result_.ok = false;
            return result_;
        }

        // 파일 기본 nest 지시어를 먼저 반영한다.
        init_file_namespace_(program_stmt);
        collect_known_namespace_paths_(program_stmt);

        // ---------------------------------------------------------
        // PASS 1: Top-level decl precollect (mutual recursion 지원)
        // - 전역 스코프에 "함수 시그니처 타입(ty::Kind::kFn)"을 먼저 등록한다.
        // - 기존 check_program() 내부의 "invalid 타입으로 insert"하던 루프가
        //   거의 모든 TypeNotCallable 증상의 원인이었으므로 제거하고,
        //   이미 구현된 first_pass_collect_top_level_()를 정식으로 사용한다.
        // ---------------------------------------------------------
        first_pass_collect_top_level_(program_stmt);

        // ---------------------------------------------------------
        // PASS 2: 실제 타입체크
        // - top-level block은 "scope 생성 없이" 자식만 순회한다.
        //   (중요: check_stmt_block_은 scope를 push하므로,
        //    root를 check_stmt_로 보내면 top-level이 새 스코프가 되어
        //    PASS1에서 등록한 전역 심볼이 가려질 수 있다.)
        // ---------------------------------------------------------
        push_acts_selection_scope_();
        push_alias_scope_();
        for (uint32_t i = 0; i < root.stmt_count; ++i) {
            const ast::StmtId child_id = ast_.stmt_children()[root.stmt_begin + i];
            check_stmt_(child_id);
            // 에러가 나도 계속 진행(정책)
        }
        pop_alias_scope_();
        pop_acts_selection_scope_();

        // ----------------------------------------
        // Finalize unresolved deferred integers:
        // - If an inferred integer "{integer}" is never consumed in a way that fixes the type,
        //   pick the smallest signed integer type that fits.
        // - Finalization applies to both symbol-backed and expression-backed pending integers.
        // ----------------------------------------
        auto choose_smallest_signed = [&](const num::BigInt& v) -> ty::TypeId {
            ty::Builtin b = ty::Builtin::kI128;
            if      (v.fits_i8())  b = ty::Builtin::kI8;
            else if (v.fits_i16()) b = ty::Builtin::kI16;
            else if (v.fits_i32()) b = ty::Builtin::kI32;
            else if (v.fits_i64()) b = ty::Builtin::kI64;
            return types_.builtin(b);
        };

        for (auto& kv : pending_int_sym_) {
            const uint32_t sym_id = kv.first;
            PendingInt& pi = kv.second;

            if (!pi.has_value || pi.resolved) continue;
            pi.resolved = true;
            pi.resolved_type = choose_smallest_signed(pi.value);
            sym_.update_declared_type(sym_id, pi.resolved_type);
        }

        for (auto& kv : pending_int_expr_) {
            const uint32_t eid = kv.first;
            PendingInt& pi = kv.second;

            if (!pi.has_value || pi.resolved) continue;
            pi.resolved = true;
            pi.resolved_type = choose_smallest_signed(pi.value);

            if (eid < expr_type_cache_.size()) {
                expr_type_cache_[eid] = pi.resolved_type;
            }
        }

        // 결과 반영
        result_.expr_types = expr_type_cache_;
        result_.expr_overload_target = expr_overload_target_cache_;
        result_.expr_ctor_owner_type = expr_ctor_owner_type_cache_;
        result_.fn_qualified_names = fn_qualified_name_by_stmt_;
        return result_;
    }

    // --------------------
    // errors
    // --------------------

    void TypeChecker::diag_(diag::Code code, Span sp) {
        if (!diag_bag_) return;
        result_.ok = false;
        diag::Diagnostic d(diag::Severity::kError, code, sp);
        diag_bag_->add(std::move(d));
    }

    void TypeChecker::diag_(diag::Code code, Span sp, std::string_view a0) {
        if (!diag_bag_) return;
        result_.ok = false;
        diag::Diagnostic d(diag::Severity::kError, code, sp);
        d.add_arg(a0);
        diag_bag_->add(std::move(d));
    }

    void TypeChecker::diag_(diag::Code code, Span sp, std::string_view a0, std::string_view a1) {
        if (!diag_bag_) return;
        result_.ok = false;
        diag::Diagnostic d(diag::Severity::kError, code, sp);
        d.add_arg(a0);
        d.add_arg(a1);
        diag_bag_->add(std::move(d));
    }

    void TypeChecker::diag_(diag::Code code, Span sp, std::string_view a0, std::string_view a1, std::string_view a2) {
        if (!diag_bag_) return;
        result_.ok = false;
        diag::Diagnostic d(diag::Severity::kError, code, sp);
        d.add_arg(a0);
        d.add_arg(a1);
        d.add_arg(a2);
        diag_bag_->add(std::move(d));
    }

    void TypeChecker::warn_(diag::Code code, Span sp, std::string_view a0) {
        if (!diag_bag_) return;
        diag::Diagnostic d(diag::Severity::kWarning, code, sp);
        if (!a0.empty()) d.add_arg(a0);
        diag_bag_->add(std::move(d));
    }

    void TypeChecker::warn_(diag::Code code, Span sp, std::string_view a0, std::string_view a1) {
        if (!diag_bag_) return;
        diag::Diagnostic d(diag::Severity::kWarning, code, sp);
        if (!a0.empty()) d.add_arg(a0);
        if (!a1.empty()) d.add_arg(a1);
        diag_bag_->add(std::move(d));
    }

    void TypeChecker::err_(Span sp, std::string msg) {
        // 1) TyckResult(errors)
        TyError e{};
        e.span = sp;
        e.message = msg;
        result_.errors.push_back(std::move(e));

        // NOTE:
        // - err_()는 저장용으로만 사용
        // - 사용자 출력은 항상 diag_(Code, args...)만 사용
    }

    bool TypeChecker::is_c_abi_safe_type_(ty::TypeId t, bool allow_void) const {
        std::unordered_set<ty::TypeId> visiting;
        return is_c_abi_safe_type_impl_(t, allow_void, visiting);
    }

    bool TypeChecker::is_c_abi_safe_type_impl_(
        ty::TypeId t,
        bool allow_void,
        std::unordered_set<ty::TypeId>& visiting
    ) const {
        if (t == ty::kInvalidType) return false;
        const auto& tt = types_.get(t);

        switch (tt.kind) {
            case ty::Kind::kError:
                return false;

            case ty::Kind::kBuiltin: {
                using B = ty::Builtin;
                switch (tt.builtin) {
                    case B::kI8:
                    case B::kI16:
                    case B::kI32:
                    case B::kI64:
                    case B::kU8:
                    case B::kU16:
                    case B::kU32:
                    case B::kU64:
                    case B::kISize:
                    case B::kUSize:
                    case B::kF32:
                    case B::kF64:
                    case B::kText:
                        return true;
                    case B::kUnit:
                        return allow_void;
                    default:
                        return false;
                }
            }

            case ty::Kind::kPtr:
                // ptr T / ptr mut T: pointee도 FFI-safe여야 한다.
                return is_c_abi_safe_type_impl_(tt.elem, /*allow_void=*/false, visiting);

            case ty::Kind::kBorrow:
            case ty::Kind::kEscape:
            case ty::Kind::kOptional:
            case ty::Kind::kArray:
            case ty::Kind::kFn:
                // v0.0.1 규칙:
                // - borrow/escape/optional/direct function type 금지
                return false;

            case ty::Kind::kNamedUser: {
                const auto it = field_abi_meta_by_type_.find(t);
                if (it == field_abi_meta_by_type_.end()) return false;
                if (it->second.layout != ast::FieldLayout::kC) return false;
                if (it->second.sid == ast::k_invalid_stmt) return false;
                if ((size_t)it->second.sid >= ast_.stmts().size()) return false;

                // 순환 타입이 있더라도 무한 재귀를 피한다.
                if (!visiting.insert(t).second) return true;

                const auto& fs = ast_.stmt(it->second.sid);
                const uint64_t begin = fs.field_member_begin;
                const uint64_t end = begin + fs.field_member_count;
                if (end > ast_.field_members().size()) {
                    visiting.erase(t);
                    return false;
                }

                for (uint32_t i = fs.field_member_begin; i < fs.field_member_begin + fs.field_member_count; ++i) {
                    const auto& m = ast_.field_members()[i];
                    if (!is_c_abi_safe_type_impl_(m.type, /*allow_void=*/false, visiting)) {
                        visiting.erase(t);
                        return false;
                    }
                }

                visiting.erase(t);
                return true;
            }
        }

        return false;
    }

    void TypeChecker::check_c_abi_global_decl_(const ast::Stmt& s) {
        if (s.link_abi != ast::LinkAbi::kC) return;

        if (!s.is_static) {
            diag_(diag::Code::kAbiCGlobalMustBeStatic, s.span, s.name);
            err_(s.span, "C ABI global must be static: " + std::string(s.name));
        }

        if (!is_c_abi_safe_type_(s.type, /*allow_void=*/false)) {
            diag_(diag::Code::kAbiCTypeNotFfiSafe, s.span, std::string("global '") + std::string(s.name) + "'", types_.to_string(s.type));
            err_(s.span, "C ABI global type is not FFI-safe: " + types_.to_string(s.type));
        }
    }

    std::string TypeChecker::path_join_(uint32_t begin, uint32_t count) const {
        if (count == 0) return {};
        const auto& segs = ast_.path_segs();
        if (begin >= segs.size() || begin + count > segs.size()) return {};

        std::string out;
        for (uint32_t i = 0; i < count; ++i) {
            if (i) out += "::";
            out += std::string(segs[begin + i]);
        }
        return out;
    }

    std::string TypeChecker::current_namespace_prefix_() const {
        if (namespace_stack_.empty()) return {};
        std::string out;
        for (size_t i = 0; i < namespace_stack_.size(); ++i) {
            if (i) out += "::";
            out += namespace_stack_[i];
        }
        return out;
    }

    std::string TypeChecker::qualify_decl_name_(std::string_view base_name) const {
        const std::string ns = current_namespace_prefix_();
        if (ns.empty()) return std::string(base_name);
        if (base_name.empty()) return ns;
        return ns + "::" + std::string(base_name);
    }

    void TypeChecker::init_file_namespace_(ast::StmtId program_stmt) {
        namespace_stack_.clear();
        if (program_stmt == ast::k_invalid_stmt) return;
        const auto& prog = ast_.stmt(program_stmt);
        if (prog.kind != ast::StmtKind::kBlock) return;

        for (uint32_t i = 0; i < prog.stmt_count; ++i) {
            const ast::StmtId cid = ast_.stmt_children()[prog.stmt_begin + i];
            if (cid == ast::k_invalid_stmt || (size_t)cid >= ast_.stmts().size()) continue;
            const auto& s = ast_.stmt(cid);
            if (s.kind != ast::StmtKind::kNestDecl || !s.nest_is_file_directive) continue;

            const std::string file_ns = path_join_(s.nest_path_begin, s.nest_path_count);
            if (file_ns.empty()) return;

            size_t pos = 0;
            while (pos < file_ns.size()) {
                size_t next = file_ns.find("::", pos);
                if (next == std::string::npos) {
                    namespace_stack_.push_back(file_ns.substr(pos));
                    break;
                }
                namespace_stack_.push_back(file_ns.substr(pos, next - pos));
                pos = next + 2;
            }
            return;
        }
    }

    std::optional<std::string> TypeChecker::rewrite_imported_path_(std::string_view path) const {
        if (path.empty()) return std::nullopt;

        auto lookup_alias = [&](std::string_view head) -> std::optional<std::string> {
            if (!import_alias_scope_stack_.empty()) {
                const auto& scope = import_alias_scope_stack_.back();
                auto it = scope.find(std::string(head));
                if (it != scope.end()) return it->second;
                return std::nullopt;
            }
            auto it = import_alias_to_path_.find(std::string(head));
            if (it == import_alias_to_path_.end()) return std::nullopt;
            return it->second;
        };

        const size_t pos = path.find("::");
        if (pos == std::string_view::npos) {
            return lookup_alias(path);
        }

        const std::string first(path.substr(0, pos));
        auto resolved = lookup_alias(first);
        if (!resolved.has_value()) return std::nullopt;

        std::string out = *resolved;
        out += path.substr(pos);
        return out;
    }

    std::optional<uint32_t> TypeChecker::lookup_symbol_(std::string_view name) const {
        if (name.empty()) return std::nullopt;

        std::string key(name);
        if (auto rewritten = rewrite_imported_path_(key)) {
            key = *rewritten;
        }

        if (auto sid = sym_.lookup(key)) {
            return sid;
        }

        // nest 경로 상대 해소:
        // - 식별자(`name`)와 경로(`a::b`) 모두 현재 namespace 기준 상대 해소를 시도한다.
        for (size_t depth = namespace_stack_.size(); depth > 0; --depth) {
            std::string qname;
            for (size_t i = 0; i < depth; ++i) {
                if (i) qname += "::";
                qname += namespace_stack_[i];
            }
            qname += "::";
            qname += key;
            if (auto sid = sym_.lookup(qname)) {
                return sid;
            }
        }

        return std::nullopt;
    }

    ty::TypeId TypeChecker::canonicalize_acts_owner_type_(ty::TypeId owner_type) const {
        if (owner_type == ty::kInvalidType) return owner_type;
        const auto& tt = types_.get(owner_type);
        if (tt.kind != ty::Kind::kNamedUser) return owner_type;

        if (auto sid = lookup_symbol_(types_.to_string(owner_type))) {
            const auto& ss = sym_.symbol(*sid);
            if ((ss.kind == sema::SymbolKind::kField || ss.kind == sema::SymbolKind::kType) &&
                ss.declared_type != ty::kInvalidType) {
                return ss.declared_type;
            }
        }
        return owner_type;
    }

    void TypeChecker::push_alias_scope_() {
        if (import_alias_scope_stack_.empty()) {
            import_alias_scope_stack_.push_back(import_alias_to_path_);
            return;
        }
        import_alias_scope_stack_.push_back(import_alias_scope_stack_.back());
    }

    void TypeChecker::pop_alias_scope_() {
        if (!import_alias_scope_stack_.empty()) {
            import_alias_scope_stack_.pop_back();
        }
    }

    bool TypeChecker::define_alias_(
        std::string_view alias,
        std::string_view path,
        Span diag_span,
        bool warn_use_nest_preferred
    ) {
        if (alias.empty() || path.empty()) return false;
        if (import_alias_scope_stack_.empty()) {
            push_alias_scope_();
        }

        auto& scope = import_alias_scope_stack_.back();
        const std::string alias_s(alias);
        const std::string path_s(path);
        auto it = scope.find(alias_s);
        if (it != scope.end() && it->second != path_s) {
            diag_(diag::Code::kTypeErrorGeneric, diag_span,
                  "conflicting use alias in same lexical scope: " + alias_s);
            err_(diag_span, "conflicting use alias: " + alias_s);
            return false;
        }

        scope[alias_s] = path_s;
        if (import_alias_scope_stack_.size() == 1) {
            import_alias_to_path_[alias_s] = path_s;
        }

        if (warn_use_nest_preferred && is_known_namespace_path_(path_s)) {
            warn_(diag::Code::kUseNestAliasPreferred, diag_span, path_s, alias_s);
        }
        return true;
    }

    bool TypeChecker::is_known_namespace_path_(std::string_view path) const {
        if (path.empty()) return false;
        return known_namespace_paths_.find(std::string(path)) != known_namespace_paths_.end();
    }

    void TypeChecker::collect_known_namespace_paths_(ast::StmtId program_stmt) {
        known_namespace_paths_.clear();
        if (program_stmt == ast::k_invalid_stmt) return;
        if ((size_t)program_stmt >= ast_.stmts().size()) return;

        std::vector<std::string> ns_stack = namespace_stack_;

        auto add_ns_prefixes_of_symbol = [&](std::string_view symbol_path) {
            if (symbol_path.empty()) return;
            size_t pos = symbol_path.find("::");
            while (pos != std::string_view::npos) {
                known_namespace_paths_.insert(std::string(symbol_path.substr(0, pos)));
                pos = symbol_path.find("::", pos + 2);
            }
        };

        auto qualify_with_ns_stack = [&](std::string_view base_name) -> std::string {
            if (base_name.empty()) return {};
            if (ns_stack.empty()) return std::string(base_name);
            std::string out;
            for (size_t i = 0; i < ns_stack.size(); ++i) {
                if (i) out += "::";
                out += ns_stack[i];
            }
            out += "::";
            out += std::string(base_name);
            return out;
        };

        auto collect_stmt = [&](auto&& self, ast::StmtId sid) -> void {
            if (sid == ast::k_invalid_stmt || (size_t)sid >= ast_.stmts().size()) return;
            const auto& s = ast_.stmt(sid);

            if (s.kind == ast::StmtKind::kBlock) {
                const auto& kids = ast_.stmt_children();
                const uint64_t begin = s.stmt_begin;
                const uint64_t end = begin + s.stmt_count;
                if (begin <= kids.size() && end <= kids.size()) {
                    for (uint32_t i = 0; i < s.stmt_count; ++i) {
                        self(self, kids[s.stmt_begin + i]);
                    }
                }
                return;
            }

            if (s.kind == ast::StmtKind::kNestDecl) {
                uint32_t pushed = 0;
                const auto& segs = ast_.path_segs();
                const uint64_t begin = s.nest_path_begin;
                const uint64_t end = begin + s.nest_path_count;
                if (begin <= segs.size() && end <= segs.size()) {
                    for (uint32_t i = 0; i < s.nest_path_count; ++i) {
                        ns_stack.push_back(std::string(segs[s.nest_path_begin + i]));
                        ++pushed;
                    }
                }
                if (!ns_stack.empty()) {
                    std::string ns;
                    for (size_t i = 0; i < ns_stack.size(); ++i) {
                        if (i) ns += "::";
                        ns += ns_stack[i];
                        known_namespace_paths_.insert(ns);
                    }
                }
                if (!s.nest_is_file_directive) {
                    self(self, s.a);
                }
                while (pushed > 0) {
                    ns_stack.pop_back();
                    --pushed;
                }
                return;
            }

            if ((s.kind == ast::StmtKind::kFnDecl ||
                 s.kind == ast::StmtKind::kFieldDecl ||
                 s.kind == ast::StmtKind::kProtoDecl ||
                 s.kind == ast::StmtKind::kClassDecl ||
                 s.kind == ast::StmtKind::kActsDecl) &&
                !s.name.empty()) {
                const std::string qname = qualify_with_ns_stack(s.name);
                add_ns_prefixes_of_symbol(qname);
                return;
            }

            if (s.kind == ast::StmtKind::kVar) {
                const bool is_global_decl =
                    s.is_static || s.is_extern || s.is_export || (s.link_abi == ast::LinkAbi::kC);
                if (!is_global_decl || s.name.empty()) return;
                const std::string qname = qualify_with_ns_stack(s.name);
                add_ns_prefixes_of_symbol(qname);
                return;
            }
        };

        collect_stmt(collect_stmt, program_stmt);
    }

    // --------------------
    // pass 1: collect top-level decls
    // --------------------
    void TypeChecker::first_pass_collect_top_level_(ast::StmtId program_stmt) {
        const ast::Stmt& prog = ast_.stmt(program_stmt);
        if (prog.kind != ast::StmtKind::kBlock) {
            err_(prog.span, "program root is not a block stmt");
            diag_(diag::Code::kTopLevelMustBeBlock, prog.span);
            return;
        }

        fn_decl_by_name_.clear();
        fn_qualified_name_by_stmt_.clear();
        proto_decl_by_name_.clear();
        proto_qualified_name_by_stmt_.clear();
        class_decl_by_name_.clear();
        class_decl_by_type_.clear();
        class_deinit_target_types_.clear();
        class_effective_method_map_.clear();
        class_member_fn_sid_set_.clear();
        proto_member_fn_sid_set_.clear();
        import_alias_to_path_.clear();
        acts_named_decl_by_owner_and_name_.clear();

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
                has_default_flags.empty() ? nullptr : has_default_flags.data()
            );
        };

        std::unordered_map<std::string, ast::StmtId> c_abi_symbol_owner;

        auto collect_stmt = [&](auto&& self, ast::StmtId sid) -> void {
            if (sid == ast::k_invalid_stmt || (size_t)sid >= ast_.stmts().size()) return;
            const ast::Stmt& s = ast_.stmt(sid);

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
                 s.use_kind == ast::UseKind::kPathAlias ||
                 s.use_kind == ast::UseKind::kNestAlias)) {
                // v0: alias는 second-pass lexical 처리만 사용한다.
                // first-pass에서는 별칭을 전역 pre-collect하지 않는다.
                return;
            }

            if (s.kind == ast::StmtKind::kFnDecl) {
                const std::string qname = qualify_decl_name_(s.name);
                fn_qualified_name_by_stmt_[sid] = qname;
                fn_decl_by_name_[qname].push_back(sid);

                const ty::TypeId sig = build_fn_sig(s);

                if (auto existing = sym_.lookup_in_current(qname)) {
                    const auto& existing_sym = sym_.symbol(*existing);
                    if (existing_sym.kind != sema::SymbolKind::kFn) {
                        err_(s.span, "duplicate symbol (function): " + qname);
                        diag_(diag::Code::kDuplicateDecl, s.span, qname);
                    }
                } else {
                    auto ins = sym_.insert(sema::SymbolKind::kFn, qname, sig, s.span);
                    if (!ins.ok && ins.is_duplicate) {
                        err_(s.span, "duplicate symbol (function): " + qname);
                        diag_(diag::Code::kDuplicateDecl, s.span, qname);
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
                    if (!is_c_abi_safe_type_(ret_ty, /*allow_void=*/true)) {
                        diag_(diag::Code::kAbiCTypeNotFfiSafe, s.span, std::string("return type of '") + std::string(s.name) + "'", types_.to_string(ret_ty));
                        err_(s.span, "C ABI return type is not FFI-safe: " + types_.to_string(ret_ty));
                    }

                    for (uint32_t pi = 0; pi < s.param_count; ++pi) {
                        const auto& p = ast_.params()[s.param_begin + pi];
                        if (!is_c_abi_safe_type_(p.type, /*allow_void=*/false)) {
                            diag_(diag::Code::kAbiCTypeNotFfiSafe, p.span, std::string("parameter '") + std::string(p.name) + "'", types_.to_string(p.type));
                            err_(p.span, "C ABI parameter type is not FFI-safe: " + std::string(p.name));
                        }
                    }
                }
                return;
            }

            if (s.kind == ast::StmtKind::kVar) {
                const bool is_global_decl =
                    s.is_static || s.is_extern || s.is_export || (s.link_abi == ast::LinkAbi::kC);
                if (is_global_decl) {
                    const std::string qname = qualify_decl_name_(s.name);
                    ty::TypeId decl_ty = s.type;
                    if (decl_ty == ty::kInvalidType) {
                        decl_ty = types_.error();
                    }

                    if (auto existing = sym_.lookup_in_current(qname)) {
                        const auto& existing_sym = sym_.symbol(*existing);
                        if (existing_sym.kind != sema::SymbolKind::kVar) {
                            err_(s.span, "duplicate symbol (global var): " + qname);
                            diag_(diag::Code::kDuplicateDecl, s.span, qname);
                        }
                    } else {
                        auto ins = sym_.insert(sema::SymbolKind::kVar, qname, decl_ty, s.span);
                        if (!ins.ok && ins.is_duplicate) {
                            err_(s.span, "duplicate symbol (global var): " + qname);
                            diag_(diag::Code::kDuplicateDecl, s.span, qname);
                        }
                    }
                }

                if (s.link_abi == ast::LinkAbi::kC && !s.is_static) {
                    diag_(diag::Code::kAbiCGlobalMustBeStatic, s.span, s.name);
                    err_(s.span, "C ABI global must be static: " + std::string(s.name));
                }
                return;
            }

            if (s.kind == ast::StmtKind::kFieldDecl) {
                const std::string qname = qualify_decl_name_(s.name);
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

            if (s.kind == ast::StmtKind::kProtoDecl) {
                const std::string qname = qualify_decl_name_(s.name);
                proto_qualified_name_by_stmt_[sid] = qname;
                proto_decl_by_name_[qname] = sid;

                ty::TypeId proto_ty = s.type;
                if (proto_ty == ty::kInvalidType && !qname.empty()) {
                    proto_ty = types_.intern_ident(qname);
                    ast_.stmt_mut(sid).type = proto_ty;
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
                        if (ms.kind != ast::StmtKind::kFnDecl) continue;
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
                    }
                }
                return;
            }

            if (s.kind == ast::StmtKind::kClassDecl) {
                const std::string qname = qualify_decl_name_(s.name);
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
                    if (tt.kind == ty::Kind::kNamedUser && types_.to_string(t) == "Self") {
                        return class_ty;
                    }
                    if (tt.kind == ty::Kind::kBorrow) {
                        const auto& et = types_.get(tt.elem);
                        if (et.kind == ty::Kind::kNamedUser && types_.to_string(tt.elem) == "Self") {
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
                        if (ms.kind == ast::StmtKind::kVar) {
                            if (!ms.is_static) continue;
                            std::string vqname = qname;
                            if (!vqname.empty()) vqname += "::";
                            vqname += std::string(ms.name);

                            ty::TypeId vt = ms.type;
                            if (vt == ty::kInvalidType) vt = types_.error();

                            if (auto existing = sym_.lookup_in_current(vqname)) {
                                const auto& existing_sym = sym_.symbol(*existing);
                                if (existing_sym.kind != sema::SymbolKind::kVar) {
                                    err_(ms.span, "duplicate symbol (class static variable): " + vqname);
                                    diag_(diag::Code::kDuplicateDecl, ms.span, vqname);
                                } else {
                                    (void)sym_.update_declared_type(*existing, vt);
                                }
                            } else {
                                auto vins = sym_.insert(sema::SymbolKind::kVar, vqname, vt, ms.span);
                                if (!vins.ok && vins.is_duplicate) {
                                    err_(ms.span, "duplicate symbol (class static variable): " + vqname);
                                    diag_(diag::Code::kDuplicateDecl, ms.span, vqname);
                                }
                            }
                            continue;
                        }

                        if (ms.kind != ast::StmtKind::kFnDecl) continue;
                        class_member_fn_sid_set_.insert(msid);
                        if (class_ty != ty::kInvalidType && ms.name == "deinit") {
                            class_deinit_target_types_.insert(class_ty);
                        }

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
                            has_default_flags.empty() ? nullptr : has_default_flags.data()
                        );

                        std::string mqname = qname;
                        if (!mqname.empty()) mqname += "::";
                        mqname += std::string(mm.name);
                        fn_qualified_name_by_stmt_[msid] = std::move(mqname);

                        const std::string qfn = fn_qualified_name_by_stmt_[msid];
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

            if (s.kind == ast::StmtKind::kActsDecl) {
                const std::string qname = qualify_decl_name_(s.name);
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
                    acts_named_decl_by_owner_and_name_[acts_named_decl_key_(owner_type, qname)] = sid;
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

                            ty::TypeId sig = types_.make_fn(ret, params.data(), (uint32_t)params.size());
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
                return;
            }
        };

        const auto& kids = ast_.stmt_children();
        const uint64_t begin = prog.stmt_begin;
        const uint64_t end = begin + prog.stmt_count;
        if (begin <= kids.size() && end <= kids.size()) {
            for (uint32_t i = 0; i < prog.stmt_count; ++i) {
                collect_stmt(collect_stmt, kids[prog.stmt_begin + i]);
            }
        }

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
            diag_(diag::Code::kTypeErrorGeneric, sp, oss.str());
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
        if (at.kind == ty::Kind::kNamedUser && types.to_string(actual) == "Self") {
            return true;
        }
        if (at.kind == ty::Kind::kBorrow) {
            const auto& elem = types.get(at.elem);
            if (elem.kind == ty::Kind::kNamedUser && types.to_string(at.elem) == "Self") {
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

    std::vector<TypeChecker::ActsMethodDecl>
    TypeChecker::lookup_acts_methods_for_call_(
        ty::TypeId owner_type,
        std::string_view name,
        const ActiveActsSelection* forced_selection
    ) const {
        std::vector<ActsMethodDecl> out;
        owner_type = canonicalize_acts_owner_type_(owner_type);
        if (owner_type == ty::kInvalidType || name.empty()) return out;
        auto oit = acts_default_method_map_.find(owner_type);
        if (oit == acts_default_method_map_.end()) return out;
        auto mit = oit->second.find(std::string(name));
        if (mit == oit->second.end()) return out;

        const auto* active = forced_selection ? forced_selection : lookup_active_acts_selection_(owner_type);
        if (active == nullptr || active->kind == ActiveActsSelectionKind::kDefaultOnly) {
            for (const auto& d : mit->second) {
                if (!d.from_named_set) out.push_back(d);
            }
            return out;
        }

        for (const auto& d : mit->second) {
            if (d.from_named_set && d.acts_decl_sid == active->named_decl_sid) {
                out.push_back(d);
            }
        }
        for (const auto& d : mit->second) {
            if (!d.from_named_set) out.push_back(d);
        }
        return out;
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

            default: return false;
        }
    }

    /// @brief field 멤버로 허용할 POD 값 내장 타입인지 판정한다.
    bool TypeChecker::is_field_pod_value_type_(const ty::TypePool& types, ty::TypeId id) {
        if (id == ty::kInvalidType) return false;
        const auto& t = types.get(id);
        if (t.kind == ty::Kind::kOptional) {
            // non-layout(c) field policy (v0): Optional<POD> is allowed.
            return is_field_pod_value_type_(types, t.elem);
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

        const auto& et = types_.get(expected);

        // ------------------------------------------------------------
        // (0) aggregate context: array
        // - expected가 array인 경우, array literal의 각 원소로 컨텍스트를 내려준다.
        // - 원소 타입에 {integer}가 포함된 경우에만 재귀 해소를 시도한다.
        // ------------------------------------------------------------
        if (et.kind == ty::Kind::kArray) {
            const ast::Expr& e = ast_.expr(eid);
            if (e.kind != ast::ExprKind::kArrayLit) return false;
            if (et.array_has_size && e.arg_count != et.array_size) return false;

            auto type_contains_infer_int = [&](ty::TypeId tid, const auto& self) -> bool {
                if (tid == ty::kInvalidType) return false;
                const auto& tt = types_.get(tid);
                switch (tt.kind) {
                    case ty::Kind::kBuiltin:
                        return tt.builtin == ty::Builtin::kInferInteger;
                    case ty::Kind::kOptional:
                    case ty::Kind::kArray:
                    case ty::Kind::kBorrow:
                    case ty::Kind::kEscape:
                        return self(tt.elem, self);
                    default:
                        return false;
                }
            };

            bool ok_all = true;
            const auto& args = ast_.args();
            const uint32_t end = e.arg_begin + e.arg_count;
            if (e.arg_begin >= args.size() || end > args.size()) return false;

            for (uint32_t i = 0; i < e.arg_count; ++i) {
                const auto& a = args[e.arg_begin + i];
                if (a.expr == ast::k_invalid_expr) continue;

                ty::TypeId elem_t = check_expr_(a.expr);
                if (!type_contains_infer_int(elem_t, type_contains_infer_int)) continue;

                if (!resolve_infer_int_in_context_(a.expr, et.elem)) {
                    ok_all = false;
                }
            }

            if (ok_all) {
                if ((size_t)eid < expr_type_cache_.size()) {
                    expr_type_cache_[eid] = expected;
                }
                return true;
            }
            return false;
        }

        // expected는 builtin int여야 한다.
        if (et.kind != ty::Kind::kBuiltin) return false;

        // float 컨텍스트면 즉시 에러 (암시적 int->float 금지)
        if (et.builtin == ty::Builtin::kF32 || et.builtin == ty::Builtin::kF64 || et.builtin == ty::Builtin::kF128) {
            diag_(diag::Code::kIntToFloatNotAllowed, ast_.expr(eid).span, types_.to_string(expected));
            return false;
        }

        auto is_int_builtin = [&](ty::Builtin b) -> bool {
            return b == ty::Builtin::kI8 || b == ty::Builtin::kI16 || b == ty::Builtin::kI32 ||
                b == ty::Builtin::kI64 || b == ty::Builtin::kI128 ||
                b == ty::Builtin::kU8 || b == ty::Builtin::kU16 || b == ty::Builtin::kU32 ||
                b == ty::Builtin::kU64 || b == ty::Builtin::kU128 ||
                b == ty::Builtin::kISize || b == ty::Builtin::kUSize;
        };
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
    void TypeChecker::second_pass_check_program_(ast::StmtId program_stmt) {
        check_stmt_(program_stmt);

        // ----------------------------------------
        // Finalize unresolved deferred integers:
        // - If an inferred integer "{integer}" is never consumed in a way that fixes the type,
        //   we pick the smallest signed integer type that fits (i8..i128).
        // - This keeps DX friendly and avoids leaving IR in an unresolved state.
        // ----------------------------------------
        for (auto& kv : pending_int_sym_) {
            const uint32_t sym_id = kv.first;
            PendingInt& pi = kv.second;

            if (!pi.has_value) continue;
            if (pi.resolved) continue;

            // pick smallest signed type
            ty::Builtin b = ty::Builtin::kI128;
            if      (pi.value.fits_i8())   b = ty::Builtin::kI8;
            else if (pi.value.fits_i16())  b = ty::Builtin::kI16;
            else if (pi.value.fits_i32())  b = ty::Builtin::kI32;
            else if (pi.value.fits_i64())  b = ty::Builtin::kI64;
            else                           b = ty::Builtin::kI128;

            pi.resolved = true;
            pi.resolved_type = types_.builtin(b);

            // NEW: SymbolTable에 확정 타입 반영
            sym_.update_declared_type(sym_id, pi.resolved_type);
        }
    }

    // --------------------
    // stmt dispatch
    // --------------------

} // namespace parus::tyck
