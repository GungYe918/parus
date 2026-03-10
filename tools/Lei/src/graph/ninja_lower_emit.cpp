std::optional<std::string> emit_graph_json(const BuildGraph& graph, lei::diag::Bag& diags) {
    (void)diags;

    std::ostringstream oss;
    oss << "{\n";
    oss << "  \"project\": {\n";
    oss << "    \"name\": \"";
    append_json_escaped(oss, graph.project_name);
    oss << "\",\n";
    oss << "    \"version\": \"";
    append_json_escaped(oss, graph.project_version);
    oss << "\"\n";
    oss << "  },\n";

    oss << "  \"modules\": [\n";
    for (size_t i = 0; i < graph.modules.size(); ++i) {
        const auto& m = graph.modules[i];
        oss << "    {\"head\": \""; append_json_escaped(oss, m.head);
        oss << "\", \"bundle\": \""; append_json_escaped(oss, m.bundle);
        oss << "\", \"sources\": "; append_string_array_json(oss, m.sources);
        oss << ", \"imports\": "; append_string_array_json(oss, m.imports);
        oss << "}";
        if (i + 1 != graph.modules.size()) oss << ",";
        oss << "\n";
    }
    oss << "  ],\n";

    oss << "  \"bundles\": [\n";
    for (size_t i = 0; i < graph.bundles.size(); ++i) {
        const auto& b = graph.bundles[i];
        oss << "    {\"name\": \""; append_json_escaped(oss, b.name);
        oss << "\", \"kind\": \""; append_json_escaped(oss, b.kind);
        oss << "\", \"modules\": "; append_string_array_json(oss, b.modules);
        oss << ", \"deps\": "; append_string_array_json(oss, b.deps);
        oss << "}";
        if (i + 1 != graph.bundles.size()) oss << ",";
        oss << "\n";
    }
    oss << "  ],\n";

    oss << "  \"tasks\": [\n";
    for (size_t i = 0; i < graph.tasks.size(); ++i) {
        const auto& t = graph.tasks[i];
        oss << "    {\"name\": \""; append_json_escaped(oss, t.name);
        oss << "\", \"run\": "; append_string_array_json(oss, t.run);
        oss << ", \"deps\": "; append_string_array_json(oss, t.deps);
        oss << ", \"cwd\": \""; append_json_escaped(oss, t.cwd);
        oss << "\", \"inputs\": "; append_string_array_json(oss, t.inputs);
        oss << ", \"outputs\": "; append_string_array_json(oss, t.outputs);
        oss << ", \"always_run\": " << (t.always_run ? "true" : "false");
        oss << "}";
        if (i + 1 != graph.tasks.size()) oss << ",";
        oss << "\n";
    }
    oss << "  ],\n";

    oss << "  \"codegens\": [\n";
    for (size_t i = 0; i < graph.codegens.size(); ++i) {
        const auto& c = graph.codegens[i];
        oss << "    {\"name\": \""; append_json_escaped(oss, c.name);
        oss << "\", \"tool\": "; append_string_array_json(oss, c.tool);
        oss << ", \"inputs\": "; append_string_array_json(oss, c.inputs);
        oss << ", \"outputs\": "; append_string_array_json(oss, c.outputs);
        oss << ", \"args\": "; append_string_array_json(oss, c.args);
        oss << ", \"deps\": "; append_string_array_json(oss, c.deps);
        oss << ", \"cwd\": \""; append_json_escaped(oss, c.cwd);
        oss << "\", \"deterministic\": " << (c.deterministic ? "true" : "false");
        oss << "}";
        if (i + 1 != graph.codegens.size()) oss << ",";
        oss << "\n";
    }
    oss << "  ]\n";
    oss << "}\n";

    return oss.str();
}

std::optional<std::string> emit_graph_text(const BuildGraph& graph, lei::diag::Bag& diags) {
    (void)diags;

    std::ostringstream oss;
    oss << "project.name=" << graph.project_name << "\n";
    oss << "project.version=" << graph.project_version << "\n";
    oss << "modules=" << graph.modules.size() << "\n";
    for (const auto& m : graph.modules) {
        oss << "  module " << m.head << " bundle=" << m.bundle
            << " srcs=" << m.sources.size()
            << " imports=" << m.imports.size() << "\n";
    }
    oss << "bundles=" << graph.bundles.size() << "\n";
    for (const auto& b : graph.bundles) {
        oss << "  bundle " << b.name << " kind=" << b.kind << " modules=" << b.modules.size()
            << " deps=" << b.deps.size() << "\n";
    }
    oss << "tasks=" << graph.tasks.size() << "\n";
    for (const auto& t : graph.tasks) {
        oss << "  task " << t.name << " run=" << t.run.size() << " deps=" << t.deps.size()
            << " always_run=" << (t.always_run ? "true" : "false") << "\n";
    }
    oss << "codegens=" << graph.codegens.size() << "\n";
    for (const auto& c : graph.codegens) {
        oss << "  codegen " << c.name << " tool=" << c.tool.size() << " in=" << c.inputs.size()
            << " out=" << c.outputs.size() << " deps=" << c.deps.size() << "\n";
    }
    return oss.str();
}

std::optional<std::string> emit_graph_dot(const BuildGraph& graph, lei::diag::Bag& diags) {
    (void)diags;

    std::ostringstream oss;
    oss << "digraph lei_build {\n";
    oss << "  rankdir=LR;\n";

    for (const auto& m : graph.modules) {
        oss << "  \"module:" << sanitize(m.head) << "\" [label=\"module:" << m.head << "\"];\n";
    }
    for (const auto& b : graph.bundles) {
        oss << "  \"bundle:" << sanitize(b.name) << "\" [label=\"bundle:" << b.name << "\"];\n";
    }
    for (const auto& t : graph.tasks) {
        oss << "  \"task:" << sanitize(t.name) << "\" [label=\"task:" << t.name << "\"];\n";
    }
    for (const auto& c : graph.codegens) {
        oss << "  \"codegen:" << sanitize(c.name) << "\" [label=\"codegen:" << c.name << "\"];\n";
    }

    for (const auto& b : graph.bundles) {
        for (const auto& m : b.modules) {
            oss << "  \"module:" << sanitize(m) << "\" -> \"bundle:" << sanitize(b.name) << "\";\n";
        }
    }
    for (const auto& m : graph.modules) {
        for (const auto& dep : m.imports) {
            oss << "  \"module:" << sanitize(dep) << "\" -> \"module:" << sanitize(m.head) << "\";\n";
        }
    }
    for (const auto& b : graph.bundles) {
        for (const auto& dep : b.deps) {
            oss << "  \"bundle:" << sanitize(dep) << "\" -> \"bundle:" << sanitize(b.name) << "\";\n";
        }
    }
    for (const auto& t : graph.tasks) {
        for (const auto& dep : t.deps) {
            oss << "  \"bundle:" << sanitize(dep) << "\" -> \"task:" << sanitize(t.name) << "\";\n";
            oss << "  \"task:" << sanitize(dep) << "\" -> \"task:" << sanitize(t.name) << "\";\n";
            oss << "  \"codegen:" << sanitize(dep) << "\" -> \"task:" << sanitize(t.name) << "\";\n";
        }
    }
    for (const auto& c : graph.codegens) {
        for (const auto& dep : c.deps) {
            oss << "  \"bundle:" << sanitize(dep) << "\" -> \"codegen:" << sanitize(c.name) << "\";\n";
            oss << "  \"task:" << sanitize(dep) << "\" -> \"codegen:" << sanitize(c.name) << "\";\n";
            oss << "  \"codegen:" << sanitize(dep) << "\" -> \"codegen:" << sanitize(c.name) << "\";\n";
        }
    }

    oss << "}\n";
    return oss.str();
}

std::optional<std::string> emit_ninja(const ExecGraph& graph, lei::diag::Bag& diags) {
    (void)diags;

    std::unordered_map<std::string, std::string> first_output_by_action{};
    for (const auto& a : graph.actions) {
        if (!a.outputs.empty()) first_output_by_action[a.id] = a.outputs.front();
    }

    std::unordered_map<std::string, std::vector<std::string>> hard_inputs_by_action{};
    std::unordered_map<std::string, std::vector<std::string>> order_inputs_by_action{};
    for (const auto& e : graph.edges) {
        auto it = first_output_by_action.find(e.from);
        if (it == first_output_by_action.end()) continue;
        if (e.kind == EdgeKind::kHard) {
            hard_inputs_by_action[e.to].push_back(it->second);
        } else {
            order_inputs_by_action[e.to].push_back(it->second);
        }
    }

    std::ostringstream oss;
    oss << "# generated by lei\n";
    oss << "ninja_required_version = 1.10\n\n";

    oss << "rule lei_exec\n";
    oss << "  command = $cmd\n";
    oss << "  description = $desc\n";
    oss << "  restat = 1\n\n";

    oss << "rule lei_touch\n";
    oss << "  command = /usr/bin/env sh -c \"mkdir -p \\\"$(dirname '$out')\\\" && : > '$out'\"\n";
    oss << "  description = touch $out\n\n";

    std::set<std::string> all_outputs{};

    for (const auto& a : graph.actions) {
        if (a.outputs.empty()) continue;
        for (const auto& o : a.outputs) all_outputs.insert(o);

        const bool is_touch = a.command.empty();
        oss << "build";
        for (const auto& o : a.outputs) oss << " " << o;
        oss << ": " << (is_touch ? "lei_touch" : "lei_exec");

        std::vector<std::string> inputs = a.inputs;
        auto hit = hard_inputs_by_action.find(a.id);
        if (hit != hard_inputs_by_action.end()) {
            inputs.insert(inputs.end(), hit->second.begin(), hit->second.end());
        }
        std::sort(inputs.begin(), inputs.end());
        inputs.erase(std::unique(inputs.begin(), inputs.end()), inputs.end());
        for (const auto& in : inputs) oss << " " << in;

        auto oit = order_inputs_by_action.find(a.id);
        if (oit != order_inputs_by_action.end() && !oit->second.empty()) {
            std::vector<std::string> ord = oit->second;
            std::sort(ord.begin(), ord.end());
            ord.erase(std::unique(ord.begin(), ord.end()), ord.end());
            oss << " ||";
            for (const auto& in : ord) oss << " " << in;
        }
        oss << "\n";

        if (!is_touch) {
            std::vector<std::string> wrapped = a.command;
            if (!a.cwd.empty() && a.cwd != ".") {
                std::vector<std::string> cwd_wrapped = {
                    "/usr/bin/env", "sh", "-c",
                    "cd " + join_shell_command({a.cwd}) + " && " + join_shell_command(a.command)
                };
                wrapped = std::move(cwd_wrapped);
            }
            oss << "  cmd = " << join_shell_command(wrapped) << "\n";
        }
        oss << "  desc = " << a.name << "\n\n";
    }

    oss << "build all: phony";
    for (const auto& out : all_outputs) oss << " " << out;
    oss << "\n";
    oss << "default all\n";

    return oss.str();
}

} // namespace lei::graph
