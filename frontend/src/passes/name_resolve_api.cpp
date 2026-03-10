    void name_resolve_stmt_tree(
        const ast::AstArena& ast,
        ast::StmtId root,
        sema::SymbolTable& sym,
        diag::Bag& bag,
        const NameResolveOptions& opt,
        NameResolveResult& out_result
    ) {
        const IdRanges r = ranges_of_(ast);
        out_result.reset_sizes(r.expr_count, r.stmt_count, r.param_count);

        // v0: param id 분류를 위해 pass 내부 상태로 "param symbol ids"를 유지한다.
        std::unordered_set<uint32_t> param_symbol_ids;
        param_symbol_ids.reserve(64);

        std::vector<std::string> namespace_stack;
        namespace_stack.reserve(8);
        std::unordered_map<std::string, std::string> import_aliases;
        import_aliases.reserve(32);
        init_file_context_(ast, r, root, namespace_stack, import_aliases, bag, opt);

        std::unordered_set<std::string> known_namespace_paths;
        known_namespace_paths.reserve(64);
        {
            std::vector<std::string> collect_ns = namespace_stack;
            collect_known_namespace_paths_stmt_(ast, r, root, collect_ns, known_namespace_paths);
        }
        for (const auto& ex : opt.external_exports) {
            add_namespace_prefixes_of_symbol_path_(ex.path, known_namespace_paths);
        }

        register_external_exports_(sym, bag, opt);
        std::vector<std::string> predeclare_ns = namespace_stack;
        predeclare_namespace_decls_(ast, r, root, sym, bag, opt, predeclare_ns);

        walk_stmt(
            ast, r, root, sym, bag, opt, out_result,
            param_symbol_ids, namespace_stack, import_aliases, known_namespace_paths, /*file_scope=*/true
        );
    }

} // namespace parus::passes
