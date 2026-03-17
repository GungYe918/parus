std::optional<ExecGraph> lower_exec_graph(const BuildGraph& graph,
                                          const std::string& bundle_root,
                                          lei::diag::Bag& diags) {
    LowerCtx ctx{};
    ctx.g.project_name = graph.project_name;
    ctx.g.project_version = graph.project_version;
    const std::string parusc_cmd = tool_from_env("PARUSC", "parusc");
    const std::string parus_lld_cmd = tool_from_env("PARUS_LLD", "parus-lld");

    std::unordered_set<std::string> names{};
    for (const auto& b : graph.bundles) {
        if (!names.insert("bundle:" + b.name).second) {
            diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE, "<entry>", 1, 1, "duplicate bundle name: " + b.name);
            return std::nullopt;
        }
    }

    std::unordered_map<std::string, const ModuleNode*> module_by_head{};
    for (const auto& m : graph.modules) {
        module_by_head[m.head] = &m;
    }

    for (const auto& c : graph.codegens) {
        std::vector<std::string> outs{};
        outs.reserve(c.outputs.size() + 1);
        for (const auto& out_file : c.outputs) {
            outs.push_back(out_file);
            add_artifact(ctx, out_file, ArtifactKind::kGeneratedFile);
        }
        const std::string stamp_path = ".lei/out/codegen/" + sanitize(c.name) + ".stamp";
        outs.push_back(stamp_path);
        add_artifact(ctx, stamp_path, ArtifactKind::kStampFile);

        std::vector<std::string> cmd = c.tool;
        cmd.insert(cmd.end(), c.args.begin(), c.args.end());

        const std::string action_id = add_action(ctx,
                                                 BuildActionKind::kCodegen,
                                                 "codegen:" + c.name,
                                                 c.cwd,
                                                 cmd,
                                                 c.inputs,
                                                 outs,
                                                 false);
        ctx.codegen_action_by_name[c.name] = action_id;
        ctx.codegen_stamp_by_name[c.name] = stamp_path;
        for (const auto& out_file : c.outputs) {
            ctx.output_file_codegen_action[out_file] = action_id;
        }
    }

    for (const auto& b : graph.bundles) {
        std::vector<std::string> resolved_sources{};
        std::unordered_map<std::string, std::string> source_module_head{};
        std::unordered_map<std::string, std::vector<std::string>> source_module_imports{};
        std::unordered_map<std::string, std::vector<std::string>> source_module_cimport_isystem{};

        for (const auto& mod_head : b.modules) {
            auto mit = module_by_head.find(mod_head);
            if (mit == module_by_head.end() || mit->second == nullptr) {
                diags.add(lei::diag::Code::B_IMPORT_MODULE_NOT_DECLARED,
                          "<entry>",
                          1,
                          1,
                          "bundle '" + b.name + "' references unknown module '" + mod_head + "'");
                return std::nullopt;
            }
            const auto* m = mit->second;
            if (m->bundle != b.name) {
                diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                          "<entry>",
                          1,
                          1,
                          "bundle '" + b.name + "' cannot include module '" + mod_head
                            + "' owned by bundle '" + m->bundle + "'");
                return std::nullopt;
            }
            for (const auto& src : m->sources) {
                const std::string resolved = resolve_source_path(bundle_root, src);
                resolved_sources.push_back(resolved);
                source_module_head[resolved] = m->head;
                source_module_imports[resolved] = m->imports;
                source_module_cimport_isystem[resolved] = m->cimport_isystem;
            }
        }
        std::sort(resolved_sources.begin(), resolved_sources.end());
        resolved_sources.erase(std::unique(resolved_sources.begin(), resolved_sources.end()), resolved_sources.end());
        if (resolved_sources.empty()) {
            diags.add(lei::diag::Code::B_BUNDLE_MODULES_REQUIRED,
                      "<entry>",
                      1,
                      1,
                      "bundle '" + b.name + "' resolved to zero source files");
            return std::nullopt;
        }

        std::vector<std::string> obj_paths{};
        obj_paths.reserve(resolved_sources.size());

        const std::string index_path = ".lei-cache/index/" + sanitize(b.name) + ".exports.json";
        ctx.bundle_index_path_by_name[b.name] = index_path;
        add_artifact(ctx, index_path, ArtifactKind::kGeneratedFile);

        std::vector<std::string> prepass_cmd = {
            parusc_cmd,
            resolved_sources.front(),
            "-fsyntax-only",
            "--bundle-name",
            b.name,
            "--bundle-root",
            bundle_root,
            "--module-head",
            source_module_head[resolved_sources.front()],
            "--emit-export-index",
            index_path,
        };
        if (auto it = source_module_imports.find(resolved_sources.front()); it != source_module_imports.end()) {
            for (const auto& im : it->second) {
                prepass_cmd.push_back("--module-import");
                prepass_cmd.push_back(im);
            }
        }
        if (auto it = source_module_cimport_isystem.find(resolved_sources.front());
            it != source_module_cimport_isystem.end()) {
            for (const auto& d : it->second) {
                if (d.empty()) continue;
                prepass_cmd.push_back("-isystem");
                prepass_cmd.push_back(d);
            }
        }
        for (const auto& d : b.cimport_isystem) {
            if (d.empty()) continue;
            prepass_cmd.push_back("-isystem");
            prepass_cmd.push_back(d);
        }
        for (const auto& src : resolved_sources) {
            prepass_cmd.push_back("--bundle-source");
            prepass_cmd.push_back(src);
        }
        for (const auto& dep : b.deps) {
            prepass_cmd.push_back("--bundle-dep");
            prepass_cmd.push_back(dep);
        }
        const std::string prepass_action = add_action(ctx,
                                                      BuildActionKind::kCodegen,
                                                      "bundle-prepass:" + b.name,
                                                      ".",
                                                      std::move(prepass_cmd),
                                                      resolved_sources,
                                                      {index_path},
                                                      false);
        ctx.bundle_prepass_action_by_name[b.name] = prepass_action;
        ctx.bundle_compile_actions_by_name[b.name] = {};

        for (const auto& src : resolved_sources) {
            const std::string obj = obj_path_for(b.name, src);
            obj_paths.push_back(obj);
            add_artifact(ctx, obj, ArtifactKind::kObjectFile);

            std::string module_head = b.name;
            auto mhit = source_module_head.find(src);
            if (mhit != source_module_head.end()) {
                module_head = mhit->second;
            }

            std::vector<std::string> cmd = {
                parusc_cmd,
                src,
                "--emit-object",
                "-o",
                obj,
            };
            cmd.push_back("--bundle-name");
            cmd.push_back(b.name);
            cmd.push_back("--bundle-root");
            cmd.push_back(bundle_root);
            cmd.push_back("--module-head");
            cmd.push_back(module_head);
            auto miit = source_module_imports.find(src);
            if (miit != source_module_imports.end()) {
                for (const auto& im : miit->second) {
                    cmd.push_back("--module-import");
                    cmd.push_back(im);
                }
            }
            if (auto ciit = source_module_cimport_isystem.find(src);
                ciit != source_module_cimport_isystem.end()) {
                for (const auto& d : ciit->second) {
                    if (d.empty()) continue;
                    cmd.push_back("-isystem");
                    cmd.push_back(d);
                }
            }
            for (const auto& d : b.cimport_isystem) {
                if (d.empty()) continue;
                cmd.push_back("-isystem");
                cmd.push_back(d);
            }
            for (const auto& all_src : resolved_sources) {
                cmd.push_back("--bundle-source");
                cmd.push_back(all_src);
            }
            for (const auto& dep : b.deps) {
                cmd.push_back("--bundle-dep");
                cmd.push_back(dep);
                const std::string dep_index = ".lei-cache/index/" + sanitize(dep) + ".exports.json";
                cmd.push_back("--load-export-index");
                cmd.push_back(dep_index);
            }

            const std::string compile_action = add_action(ctx,
                                                          BuildActionKind::kCompile,
                                                          "compile:" + b.name + ":" + src,
                                                          ".",
                                                          std::move(cmd),
                                                          {src},
                                                          {obj},
                                                          false);
            ctx.bundle_compile_actions_by_name[b.name].push_back(compile_action);

            add_edge(ctx, prepass_action, compile_action, EdgeKind::kHard);

            auto gen_it = ctx.output_file_codegen_action.find(src);
            if (gen_it != ctx.output_file_codegen_action.end()) {
                add_edge(ctx, gen_it->second, compile_action, EdgeKind::kHard);
            }
        }

        ctx.bundle_obj_artifacts[b.name] = obj_paths;
        ctx.bundle_dep_names[b.name] = b.deps;

        if (b.kind == "lib") {
            const std::string lib_stamp = ".lei/out/lib/" + sanitize(b.name) + ".stamp";
            add_artifact(ctx, lib_stamp, ArtifactKind::kStampFile);

            const std::string lib_action = add_action(ctx,
                                                      BuildActionKind::kPhony,
                                                      "bundle-lib:" + b.name,
                                                      ".",
                                                      {},
                                                      obj_paths,
                                                      {lib_stamp},
                                                      false);
            ctx.bundle_action_by_name[b.name] = lib_action;
            ctx.bundle_stamp_or_bin_by_name[b.name] = lib_stamp;
        }
    }

    std::unordered_map<std::string, std::string> bundle_kind{};
    for (const auto& b : graph.bundles) bundle_kind[b.name] = b.kind;

    std::function<std::vector<std::string>(const std::string&, std::unordered_set<std::string>&)> collect_lib_objs;
    collect_lib_objs = [&](const std::string& bundle, std::unordered_set<std::string>& vis) -> std::vector<std::string> {
        if (!vis.insert(bundle).second) return {};
        std::vector<std::string> out{};
        auto it_self = ctx.bundle_obj_artifacts.find(bundle);
        if (it_self != ctx.bundle_obj_artifacts.end()) {
            out.insert(out.end(), it_self->second.begin(), it_self->second.end());
        }
        auto deps_it = ctx.bundle_dep_names.find(bundle);
        if (deps_it != ctx.bundle_dep_names.end()) {
            for (const auto& dep : deps_it->second) {
                auto kind_it = bundle_kind.find(dep);
                if (kind_it == bundle_kind.end()) continue;
                if (kind_it->second == "lib") {
                    auto child = collect_lib_objs(dep, vis);
                    out.insert(out.end(), child.begin(), child.end());
                }
            }
        }
        return out;
    };

    for (const auto& b : graph.bundles) {
        if (b.kind != "bin") continue;

        std::unordered_set<std::string> vis{};
        auto all_objs = collect_lib_objs(b.name, vis);
        std::sort(all_objs.begin(), all_objs.end());
        all_objs.erase(std::unique(all_objs.begin(), all_objs.end()), all_objs.end());

        const std::string bin_out = ".lei/out/bin/" + sanitize(b.name);
        const std::string bin_stamp = ".lei/out/bin/" + sanitize(b.name) + ".stamp";
        add_artifact(ctx, bin_out, ArtifactKind::kBinaryFile);
        add_artifact(ctx, bin_stamp, ArtifactKind::kStampFile);

        std::vector<std::string> cmd = {parus_lld_cmd, "-o", bin_out};
        cmd.insert(cmd.end(), all_objs.begin(), all_objs.end());

        std::vector<std::string> outs = {bin_out, bin_stamp};
        const std::string link_action = add_action(ctx,
                                                   BuildActionKind::kLink,
                                                   "bundle-bin:" + b.name,
                                                   ".",
                                                   std::move(cmd),
                                                   all_objs,
                                                   outs,
                                                   false);
        ctx.bundle_action_by_name[b.name] = link_action;
        ctx.bundle_stamp_or_bin_by_name[b.name] = bin_out;
    }

    for (const auto& t : graph.tasks) {
        std::vector<std::string> outs = t.outputs;
        for (const auto& out : t.outputs) add_artifact(ctx, out, ArtifactKind::kGeneratedFile);

        const std::string stamp = ".lei/out/task/" + sanitize(t.name) + ".stamp";
        outs.push_back(stamp);
        add_artifact(ctx, stamp, ArtifactKind::kStampFile);

        const std::string action_id = add_action(ctx,
                                                 BuildActionKind::kTask,
                                                 "task:" + t.name,
                                                 t.cwd,
                                                 t.run,
                                                 t.inputs,
                                                 outs,
                                                 t.always_run);
        ctx.task_action_by_name[t.name] = action_id;
        ctx.task_stamp_by_name[t.name] = stamp;
    }

    for (const auto& c : graph.codegens) {
        const auto self_it = ctx.codegen_action_by_name.find(c.name);
        if (self_it == ctx.codegen_action_by_name.end()) continue;
        const std::string self = self_it->second;
        for (const auto& dep : c.deps) {
            auto itc = ctx.codegen_action_by_name.find(dep);
            if (itc != ctx.codegen_action_by_name.end()) {
                add_edge(ctx, itc->second, self, EdgeKind::kHard);
                continue;
            }
            auto itb = ctx.bundle_action_by_name.find(dep);
            if (itb != ctx.bundle_action_by_name.end()) {
                add_edge(ctx, itb->second, self, EdgeKind::kHard);
                continue;
            }
            auto itt = ctx.task_action_by_name.find(dep);
            if (itt != ctx.task_action_by_name.end()) {
                add_edge(ctx, itt->second, self, EdgeKind::kHard);
                continue;
            }
            diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                      "<entry>",
                      1,
                      1,
                      "unknown codegen dependency: " + c.name + " -> " + dep);
            return std::nullopt;
        }
    }

    for (const auto& b : graph.bundles) {
        auto self_it = ctx.bundle_action_by_name.find(b.name);
        if (self_it == ctx.bundle_action_by_name.end()) continue;
        const std::string self = self_it->second;
        auto self_prepass = ctx.bundle_prepass_action_by_name.find(b.name);
        if (self_prepass != ctx.bundle_prepass_action_by_name.end()) {
            add_edge(ctx, self_prepass->second, self, EdgeKind::kHard);
        }
        for (const auto& dep : b.deps) {
            auto itb = ctx.bundle_action_by_name.find(dep);
            if (itb != ctx.bundle_action_by_name.end()) {
                add_edge(ctx, itb->second, self, EdgeKind::kHard);
            } else {
                diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                          "<entry>",
                          1,
                          1,
                          "unknown bundle dependency: " + b.name + " -> " + dep);
                return std::nullopt;
            }

            auto dep_prepass = ctx.bundle_prepass_action_by_name.find(dep);
            if (dep_prepass != ctx.bundle_prepass_action_by_name.end()) {
                auto itc = ctx.bundle_compile_actions_by_name.find(b.name);
                if (itc != ctx.bundle_compile_actions_by_name.end()) {
                    for (const auto& ca : itc->second) {
                        add_edge(ctx, dep_prepass->second, ca, EdgeKind::kHard);
                    }
                }
            }
        }
    }

    for (const auto& t : graph.tasks) {
        auto self_it = ctx.task_action_by_name.find(t.name);
        if (self_it == ctx.task_action_by_name.end()) continue;
        const std::string self = self_it->second;
        for (const auto& dep : t.deps) {
            auto itc = ctx.codegen_action_by_name.find(dep);
            if (itc != ctx.codegen_action_by_name.end()) {
                add_edge(ctx, itc->second, self, EdgeKind::kHard);
                continue;
            }
            auto itb = ctx.bundle_action_by_name.find(dep);
            if (itb != ctx.bundle_action_by_name.end()) {
                add_edge(ctx, itb->second, self, EdgeKind::kHard);
                continue;
            }
            auto itt = ctx.task_action_by_name.find(dep);
            if (itt != ctx.task_action_by_name.end()) {
                add_edge(ctx, itt->second, self, EdgeKind::kHard);
                continue;
            }
            diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                      "<entry>",
                      1,
                      1,
                      "unknown task dependency: " + t.name + " -> " + dep);
            return std::nullopt;
        }
    }

    std::sort(ctx.g.artifacts.begin(), ctx.g.artifacts.end(), [](const ArtifactNode& a, const ArtifactNode& b) {
        if (a.path != b.path) return a.path < b.path;
        if (a.id != b.id) return a.id < b.id;
        return static_cast<int>(a.kind) < static_cast<int>(b.kind);
    });

    auto action_rank = [](BuildActionKind k) {
        switch (k) {
            case BuildActionKind::kCodegen: return 0;
            case BuildActionKind::kCompile: return 1;
            case BuildActionKind::kLink: return 2;
            case BuildActionKind::kTask: return 3;
            case BuildActionKind::kPhony: return 4;
        }
        return 99;
    };

    std::sort(ctx.g.actions.begin(), ctx.g.actions.end(), [&](const ExecNode& a, const ExecNode& b) {
        const int ar = action_rank(a.kind);
        const int br = action_rank(b.kind);
        if (ar != br) return ar < br;
        if (a.name != b.name) return a.name < b.name;
        return a.id < b.id;
    });

    std::sort(ctx.g.edges.begin(), ctx.g.edges.end(), [](const ExecEdge& a, const ExecEdge& b) {
        if (a.from != b.from) return a.from < b.from;
        if (a.to != b.to) return a.to < b.to;
        return static_cast<int>(a.kind) < static_cast<int>(b.kind);
    });

    return ctx.g;
}

static void append_json_escaped(std::ostringstream& oss, const std::string& s) {
    for (char c : s) {
        switch (c) {
            case '"': oss << "\\\""; break;
            case '\\': oss << "\\\\"; break;
            case '\n': oss << "\\n"; break;
            case '\r': oss << "\\r"; break;
            case '\t': oss << "\\t"; break;
            default: oss << c; break;
        }
    }
}

static void append_string_array_json(std::ostringstream& oss, const std::vector<std::string>& arr) {
    oss << "[";
    for (size_t i = 0; i < arr.size(); ++i) {
        if (i) oss << ", ";
        oss << "\"";
        append_json_escaped(oss, arr[i]);
        oss << "\"";
    }
    oss << "]";
}
