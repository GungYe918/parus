// frontend/src/passes/name_resolve.cpp
#include "name_resolve_internal.hpp"


namespace parus::passes {

    // -----------------------------------------------------------------------------
    // Diagnostics helpers
    // -----------------------------------------------------------------------------
    static void report(diag::Bag& bag, diag::Severity sev, diag::Code code, Span span, std::string_view a0 = {}) {
        diag::Diagnostic d(sev, code, span);
        if (!a0.empty()) d.add_arg(a0);
        bag.add(std::move(d));
    }

    // -----------------------------------------------------------------------------
    // Core invariants (v0 parser quirks)
    //
    // 1) BlockExpr stores dedicated ids:
    //    - block_stmt : StmtId
    //    - block_tail : ExprId (or invalid)
    //
    // 2) Loop expr stores:
    //    - loop_iter : ExprId
    //    - loop_body : StmtId
    //
    // This pass MUST treat these fields with the correct id-space.
    // -----------------------------------------------------------------------------

    // -----------------------------------------------------------------------------
    // Id validation helpers
    // -----------------------------------------------------------------------------
    struct IdRanges {
        uint32_t expr_count = 0;
        uint32_t stmt_count = 0;
        uint32_t arg_count = 0;
        uint32_t stmt_children_count = 0;
        uint32_t param_count = 0;
        uint32_t switch_case_count = 0;
    };

    static IdRanges ranges_of_(const ast::AstArena& ast) {
        IdRanges r{};
        r.expr_count = (uint32_t)ast.exprs().size();
        r.stmt_count = (uint32_t)ast.stmts().size();
        r.arg_count = (uint32_t)ast.args().size();
        r.stmt_children_count = (uint32_t)ast.stmt_children().size();
        r.param_count = (uint32_t)ast.params().size();
        r.switch_case_count = (uint32_t)ast.switch_cases().size();
        return r;
    }

    static bool is_valid_expr_id_(const IdRanges& r, ast::ExprId id) {
        return id != ast::k_invalid_expr && (uint32_t)id < r.expr_count;
    }

    static bool is_valid_stmt_id_(const IdRanges& r, ast::StmtId id) {
        return id != ast::k_invalid_stmt && (uint32_t)id < r.stmt_count;
    }

    static bool is_valid_param_index_(const IdRanges& r, uint32_t pid) {
        return pid < r.param_count;
    }

    static void ensure_result_sized_(const IdRanges& r, NameResolveResult& out) {
        if (out.expr_to_resolved.size() != r.expr_count ||
            out.stmt_to_resolved.size() != r.stmt_count ||
            out.param_to_resolved.size() != r.param_count)
        {
            out.reset_sizes(r.expr_count, r.stmt_count, r.param_count);
        }
    }

    // -----------------------------------------------------------------------------
    // Scope guard
    // -----------------------------------------------------------------------------
    struct ScopeGuard {
        sema::SymbolTable& sym;
        bool active = false;
        explicit ScopeGuard(sema::SymbolTable& s) : sym(s), active(true) { sym.push_scope(); }
        ~ScopeGuard() { if (active) sym.pop_scope(); }
        ScopeGuard(const ScopeGuard&) = delete;
        ScopeGuard& operator=(const ScopeGuard&) = delete;
    };

    struct AliasScopeGuard {
        std::unordered_map<std::string, std::string>& aliases;
        std::unordered_map<std::string, std::string> saved;
        explicit AliasScopeGuard(std::unordered_map<std::string, std::string>& a)
            : aliases(a), saved(a) {}
        ~AliasScopeGuard() { aliases = std::move(saved); }
        AliasScopeGuard(const AliasScopeGuard&) = delete;
        AliasScopeGuard& operator=(const AliasScopeGuard&) = delete;
    };

    // -----------------------------------------------------------------------------
    // ResolvedSymbol table helpers
    // -----------------------------------------------------------------------------
    static NameResolveResult::ResolvedId add_resolved_(
        NameResolveResult& out,
        BindingKind bind,
        uint32_t sym_id,
        Span span
    ) {
        ResolvedSymbol rs{};
        rs.bind = bind;
        rs.sym = sym_id;
        rs.span = span;

        out.resolved.push_back(rs);
        return (NameResolveResult::ResolvedId)(out.resolved.size() - 1);
    }

    // forward
    static void walk_stmt(
        const ast::AstArena& ast,
        const IdRanges& r,
        ast::StmtId id,
        sema::SymbolTable& sym,
        diag::Bag& bag,
        const NameResolveOptions& opt,
        NameResolveResult& out,
        std::unordered_set<uint32_t>& param_symbol_ids,
        std::vector<std::string>& namespace_stack,
        std::unordered_map<std::string, std::string>& import_aliases,
        const std::unordered_set<std::string>& known_namespace_paths,
        bool file_scope
    );

    // -----------------------------------------------------------------------------
    // Shadowing/duplicate diagnostics + insert wrapper
    // -----------------------------------------------------------------------------
    static sema::SymbolTable::InsertResult declare_(
        sema::SymbolKind kind,
        std::string_view name,
        parus::ty::TypeId ty,
        Span span,
        sema::SymbolTable& sym,
        diag::Bag& bag,
        const NameResolveOptions& opt
    ) {
        auto ins = sym.insert(
            kind,
            name,
            ty,
            span,
            opt.current_file_id,
            opt.current_bundle_name,
            /*is_export=*/false,
            /*is_external=*/false,
            opt.current_module_head,
            opt.current_source_dir_norm
        );

        // 함수는 오버로딩을 허용한다.
        // SymbolTable은 이름당 1개 엔트리만 보관하므로,
        // 같은 스코프에 같은 이름의 함수가 이미 있으면 기존 심볼을 재사용한다.
        if (ins.is_duplicate && kind == sema::SymbolKind::kFn) {
            const auto& dup = sym.symbol(ins.symbol_id);
            if (dup.kind == sema::SymbolKind::kFn) {
                sema::SymbolTable::InsertResult ok{};
                ok.ok = true;
                ok.symbol_id = ins.symbol_id;
                return ok;
            }
        }

        if (ins.is_duplicate) {
            report(bag, diag::Severity::kError, diag::Code::kDuplicateDecl, span, name);
            return ins;
        }

        if (ins.is_shadowing) {
            switch (opt.shadowing) {
                case ShadowingMode::kAllow:
                    break;
                case ShadowingMode::kWarn:
                    report(bag, diag::Severity::kWarning, diag::Code::kShadowing, span, name);
                    break;
                case ShadowingMode::kError:
                    report(bag, diag::Severity::kError, diag::Code::kShadowingNotAllowed, span, name);
                    break;
            }
        }
        return ins;
    }

    static std::string path_join_(const ast::AstArena& ast, uint32_t begin, uint32_t count) {
        if (count == 0) return {};
        const auto& segs = ast.path_segs();
        if (begin >= segs.size() || begin + count > segs.size()) return {};

        std::string out;
        for (uint32_t i = 0; i < count; ++i) {
            if (i) out += "::";
            out += std::string(segs[begin + i]);
        }
        return out;
    }

    static std::string resolve_import_path_for_alias_(
        std::string_view raw_path,
        const NameResolveOptions& opt
    ) {
        if (raw_path.empty()) return {};
        if (!raw_path.starts_with('.')) return std::string(raw_path);

        size_t dot_count = 0;
        while (dot_count < raw_path.size() && raw_path[dot_count] == '.') {
            ++dot_count;
        }
        if (dot_count == 0) return std::string(raw_path);

        std::string_view rel = raw_path.substr(dot_count);
        if (rel.starts_with("::")) {
            rel.remove_prefix(2);
        }
        if (rel.empty()) return {};

        if (opt.current_module_head.empty()) {
            return std::string(rel);
        }

        std::vector<std::string> parts{};
        size_t pos = 0;
        while (pos < opt.current_module_head.size()) {
            size_t next = opt.current_module_head.find("::", pos);
            if (next == std::string::npos) {
                parts.push_back(opt.current_module_head.substr(pos));
                break;
            }
            parts.push_back(opt.current_module_head.substr(pos, next - pos));
            pos = next + 2;
        }

        if (parts.empty()) {
            return std::string(rel);
        }

        const size_t keep = (dot_count >= parts.size()) ? 0 : (parts.size() - dot_count);
        std::string out{};
        for (size_t i = 0; i < keep; ++i) {
            if (i) out += "::";
            out += parts[i];
        }
        if (!out.empty()) out += "::";
        out += rel;
        return out;
    }

    static std::string qualify_name_(const std::vector<std::string>& namespace_stack, std::string_view base) {
        if (base.empty()) return {};
        if (namespace_stack.empty()) return std::string(base);

        std::string out;
        for (size_t i = 0; i < namespace_stack.size(); ++i) {
            if (i) out += "::";
            out += namespace_stack[i];
        }
        out += "::";
        out += std::string(base);
        return out;
    }

    static std::string import_head_of_path_(std::string_view path) {
        if (path.empty()) return {};
        const size_t pos = path.find("::");
        if (pos == std::string_view::npos) return std::string(path);
        return std::string(path.substr(0, pos));
    }

    static bool is_allowed_import_head_(
        std::string_view head,
        const NameResolveOptions& opt
    ) {
        if (head.empty()) return false;
        if (opt.allowed_import_heads.empty()) {
            // In bundle-aware mode we enforce strict deps.
            return opt.current_bundle_name.empty();
        }
        return opt.allowed_import_heads.find(std::string(head)) != opt.allowed_import_heads.end();
    }

    static void validate_import_dep_(
        diag::Bag& bag,
        const NameResolveOptions& opt,
        Span span,
        std::string_view full_path
    ) {
        const std::string head = import_head_of_path_(full_path);
        if (head.empty()) return;
        if (!is_allowed_import_head_(head, opt)) {
            report(bag, diag::Severity::kError, diag::Code::kImportDepNotDeclared, span, head);
        }
    }

    static bool is_symbol_visible_from_use_site_(
        const sema::Symbol& symobj,
        const NameResolveOptions& opt
    ) {
        if (symobj.is_external) {
            return symobj.is_export;
        }

        if (opt.current_file_id != 0 &&
            symobj.decl_file_id != 0 &&
            symobj.decl_file_id != opt.current_file_id &&
            !symobj.is_export) {
            return false;
        }

        if (!opt.current_bundle_name.empty() &&
            !symobj.decl_bundle_name.empty() &&
            symobj.decl_bundle_name != opt.current_bundle_name &&
            !symobj.is_export) {
            return false;
        }

        return true;
    }

    static void add_namespace_prefixes_of_symbol_path_(
        std::string_view symbol_path,
        std::unordered_set<std::string>& known_namespace_paths
    ) {
        if (symbol_path.empty()) return;
        size_t pos = symbol_path.find("::");
        while (pos != std::string_view::npos) {
            known_namespace_paths.insert(std::string(symbol_path.substr(0, pos)));
            pos = symbol_path.find("::", pos + 2);
        }
    }

    static bool is_known_namespace_path_(
        std::string_view path,
        const std::unordered_set<std::string>& known_namespace_paths
    ) {
        if (path.empty()) return false;
        return known_namespace_paths.find(std::string(path)) != known_namespace_paths.end();
    }

    static void collect_known_namespace_paths_stmt_(
        const ast::AstArena& ast,
        const IdRanges& r,
        ast::StmtId sid,
        std::vector<std::string>& namespace_stack,
        std::unordered_set<std::string>& known_namespace_paths
    ) {
        if (!is_valid_stmt_id_(r, sid)) return;
        const auto& s = ast.stmt(sid);

        if (s.kind == ast::StmtKind::kBlock) {
            const auto& kids = ast.stmt_children();
            const uint64_t begin = s.stmt_begin;
            const uint64_t end = begin + s.stmt_count;
            if (begin <= kids.size() && end <= kids.size()) {
                for (uint32_t i = 0; i < s.stmt_count; ++i) {
                    collect_known_namespace_paths_stmt_(
                        ast, r, kids[s.stmt_begin + i], namespace_stack, known_namespace_paths
                    );
                }
            }
            return;
        }

        if (s.kind == ast::StmtKind::kNestDecl) {
            uint32_t pushed = 0;
            const auto& segs = ast.path_segs();
            const uint64_t begin = s.nest_path_begin;
            const uint64_t end = begin + s.nest_path_count;
            if (begin <= segs.size() && end <= segs.size()) {
                for (uint32_t i = 0; i < s.nest_path_count; ++i) {
                    namespace_stack.push_back(std::string(segs[s.nest_path_begin + i]));
                    ++pushed;
                }
            }

            if (!namespace_stack.empty()) {
                std::string ns;
                for (size_t i = 0; i < namespace_stack.size(); ++i) {
                    if (i) ns += "::";
                    ns += namespace_stack[i];
                    known_namespace_paths.insert(ns);
                }
            }

            if (!s.nest_is_file_directive) {
                collect_known_namespace_paths_stmt_(ast, r, s.a, namespace_stack, known_namespace_paths);
            }

            while (pushed > 0) {
                namespace_stack.pop_back();
                --pushed;
            }
            return;
        }

        if ((s.kind == ast::StmtKind::kFnDecl ||
             s.kind == ast::StmtKind::kFieldDecl ||
             s.kind == ast::StmtKind::kEnumDecl ||
             s.kind == ast::StmtKind::kProtoDecl ||
             s.kind == ast::StmtKind::kClassDecl ||
             s.kind == ast::StmtKind::kActorDecl ||
             s.kind == ast::StmtKind::kActsDecl ||
             s.kind == ast::StmtKind::kInstDecl) &&
            !s.name.empty()) {
            const std::string qname = qualify_name_(namespace_stack, s.name);
            add_namespace_prefixes_of_symbol_path_(qname, known_namespace_paths);
            return;
        }

        if (s.kind == ast::StmtKind::kVar) {
            const bool is_global_decl =
                s.is_static || s.is_const || s.is_extern || s.is_export || (s.link_abi == ast::LinkAbi::kC);
            if (!is_global_decl || s.name.empty()) return;
            const std::string qname = qualify_name_(namespace_stack, s.name);
            add_namespace_prefixes_of_symbol_path_(qname, known_namespace_paths);
            return;
        }
    }

    static std::optional<std::string> rewrite_imported_path_(
        std::string_view name,
        const std::unordered_map<std::string, std::string>& import_aliases
    ) {
        if (name.empty()) return std::nullopt;

        const size_t pos = name.find("::");
        if (pos == std::string_view::npos) {
            auto it = import_aliases.find(std::string(name));
            if (it == import_aliases.end()) return std::nullopt;
            return it->second;
        }

        std::string head(name.substr(0, pos));
        auto it = import_aliases.find(head);
        if (it == import_aliases.end()) return std::nullopt;

        std::string out = it->second;
        out += name.substr(pos);
        return out;
    }

    static bool qualified_path_requires_import_(
        std::string_view raw_name,
        std::string_view current_module_head,
        const std::unordered_set<std::string>& known_namespace_paths
    ) {
        const size_t pos = raw_name.find("::");
        if (pos == std::string_view::npos || pos == 0) return false;

        const std::string_view head = raw_name.substr(0, pos);
        if (known_namespace_paths.find(std::string(head)) != known_namespace_paths.end()) {
            return false;
        }
        if (!current_module_head.empty()) {
            if (head == current_module_head) return false;
            if (current_module_head.starts_with("core::") &&
                head == std::string_view(current_module_head).substr(std::string_view("core::").size())) {
                return false;
            }
        }
        return true;
    }

    static std::optional<uint32_t> lookup_symbol_(
        sema::SymbolTable& sym,
        std::string_view raw_name,
        const std::vector<std::string>& namespace_stack,
        const std::unordered_map<std::string, std::string>& import_aliases,
        const std::unordered_set<std::string>& known_namespace_paths,
        std::string_view current_module_head
    ) {
        if (raw_name.empty()) return std::nullopt;

        auto head_resolves_without_import = [&](std::string_view head) -> bool {
            if (head.empty()) return false;
            if (sym.lookup(head).has_value()) return true;
            const std::string prefix = std::string(head) + "::";
            for (const auto& candidate : sym.symbols()) {
                if (!candidate.is_external && candidate.name.starts_with(prefix)) return true;
            }
            for (size_t depth = namespace_stack.size(); depth > 0; --depth) {
                std::string q;
                for (size_t i = 0; i < depth; ++i) {
                    if (i) q += "::";
                    q += namespace_stack[i];
                }
                q += "::";
                q += head;
                if (sym.lookup(q).has_value()) return true;
                const std::string nested_prefix = q + "::";
                for (const auto& candidate : sym.symbols()) {
                    if (!candidate.is_external && candidate.name.starts_with(nested_prefix)) return true;
                }
            }
            return false;
        };

        std::string name(raw_name);
        if (auto rewritten = rewrite_imported_path_(name, import_aliases)) {
            name = *rewritten;
        } else if (qualified_path_requires_import_(raw_name, current_module_head, known_namespace_paths)) {
            const size_t pos = raw_name.find("::");
            if (pos == std::string_view::npos ||
                !head_resolves_without_import(raw_name.substr(0, pos))) {
                return std::nullopt;
            }
        }

        if (auto sid = sym.lookup(name)) {
            return sid;
        }

        for (size_t depth = namespace_stack.size(); depth > 0; --depth) {
            std::string q;
            for (size_t i = 0; i < depth; ++i) {
                if (i) q += "::";
                q += namespace_stack[i];
            }
            q += "::";
            q += name;
            if (auto sid = sym.lookup(q)) {
                return sid;
            }
        }
        return std::nullopt;
    }
