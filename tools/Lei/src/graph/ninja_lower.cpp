#include <lei/graph/BuildGraph.hpp>

#include <algorithm>
#include <set>
#include <sstream>
#include <unordered_map>

namespace lei::graph {

namespace {

const lei::eval::Value* find_key(const lei::eval::Value::Object& obj, const std::string& key) {
    auto it = obj.find(key);
    if (it == obj.end()) return nullptr;
    return &it->second;
}

std::string sanitize(std::string s) {
    for (char& c : s) {
        const bool ok = (c >= 'a' && c <= 'z') ||
                        (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') ||
                        c == '_' || c == '-';
        if (!ok) c = '_';
    }
    if (s.empty()) s = "unnamed";
    return s;
}

bool expect_string_field(const lei::eval::Value::Object& obj,
                         const std::string& field,
                         std::string& out,
                         lei::diag::Bag& diags,
                         const std::string& who) {
    auto it = obj.find(field);
    if (it == obj.end() || !it->second.is_string()) {
        diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                  "<entry>",
                  1,
                  1,
                  who + " requires string field '" + field + "'");
        return false;
    }
    out = std::get<std::string>(it->second.data);
    return true;
}

bool expect_bool_field(const lei::eval::Value::Object& obj,
                       const std::string& field,
                       bool& out,
                       lei::diag::Bag& diags,
                       const std::string& who,
                       bool optional,
                       bool default_value) {
    auto it = obj.find(field);
    if (it == obj.end()) {
        if (optional) {
            out = default_value;
            return true;
        }
        diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                  "<entry>",
                  1,
                  1,
                  who + " requires bool field '" + field + "'");
        return false;
    }
    if (!it->second.is_bool()) {
        diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                  "<entry>",
                  1,
                  1,
                  who + " field '" + field + "' must be bool");
        return false;
    }
    out = std::get<bool>(it->second.data);
    return true;
}

bool read_string_array_field(const lei::eval::Value::Object& obj,
                             const std::string& field,
                             std::vector<std::string>& out,
                             lei::diag::Bag& diags,
                             const std::string& who,
                             bool required) {
    auto it = obj.find(field);
    if (it == obj.end()) {
        if (required) {
            diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                      "<entry>",
                      1,
                      1,
                      who + " requires array field '" + field + "'");
            return false;
        }
        out.clear();
        return true;
    }

    auto arr = std::get_if<lei::eval::Value::Array>(&it->second.data);
    if (!arr) {
        diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                  "<entry>",
                  1,
                  1,
                  who + " field '" + field + "' must be [string]");
        return false;
    }

    out.clear();
    out.reserve(arr->size());
    for (const auto& v : *arr) {
        auto sp = std::get_if<std::string>(&v.data);
        if (!sp) {
            diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                      "<entry>",
                      1,
                      1,
                      who + " field '" + field + "' must contain only string");
            return false;
        }
        out.push_back(*sp);
    }
    return true;
}

} // namespace

std::optional<BuildGraph> from_entry_plan_value(const lei::eval::Value& entry_plan,
                                                lei::diag::Bag& diags,
                                                const std::string& entry_name) {
    BuildGraph g{};

    const auto* root = entry_plan.as_object();
    if (!root) {
        diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                  "<entry>",
                  1,
                  1,
                  "entry plan '" + entry_name + "' must be object");
        return std::nullopt;
    }

    const auto& graph_obj = *root;

    // Optional project metadata.
    if (const auto* project_v = find_key(graph_obj, "project"); project_v && project_v->is_object()) {
        const auto& pobj = std::get<lei::eval::Value::Object>(project_v->data);
        auto it_name = pobj.find("name");
        if (it_name != pobj.end() && it_name->second.is_string()) {
            g.project_name = std::get<std::string>(it_name->second.data);
        }
        auto it_ver = pobj.find("version");
        if (it_ver != pobj.end() && it_ver->second.is_string()) {
            g.project_version = std::get<std::string>(it_ver->second.data);
        }
    }

    if (const auto* bundles_v = find_key(graph_obj, "bundles")) {
        auto arr = std::get_if<lei::eval::Value::Array>(&bundles_v->data);
        if (!arr) {
            diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE, "<entry>", 1, 1, "bundles must be array");
            return std::nullopt;
        }

        for (const auto& b : *arr) {
            auto bobj = b.as_object();
            if (!bobj) {
                diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                          "<entry>",
                          1,
                          1,
                          "bundle entry must be object");
                return std::nullopt;
            }

            BundleNode node{};
            if (!expect_string_field(*bobj, "name", node.name, diags, "bundle")) return std::nullopt;
            if (!expect_string_field(*bobj, "kind", node.kind, diags, "bundle")) return std::nullopt;
            if (!read_string_array_field(*bobj, "sources", node.sources, diags, "bundle", true)) return std::nullopt;
            if (!read_string_array_field(*bobj, "deps", node.deps, diags, "bundle", true)) return std::nullopt;
            if (node.sources.empty()) {
                diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                          "<entry>",
                          1,
                          1,
                          "bundle.sources must not be empty");
                return std::nullopt;
            }
            g.bundles.push_back(std::move(node));
        }
    }

    if (const auto* tasks_v = find_key(graph_obj, "tasks")) {
        auto arr = std::get_if<lei::eval::Value::Array>(&tasks_v->data);
        if (!arr) {
            diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE, "<entry>", 1, 1, "tasks must be array");
            return std::nullopt;
        }

        for (const auto& t : *arr) {
            auto tobj = t.as_object();
            if (!tobj) {
                diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                          "<entry>",
                          1,
                          1,
                          "task entry must be object");
                return std::nullopt;
            }

            TaskNode node{};
            if (!expect_string_field(*tobj, "name", node.name, diags, "task")) return std::nullopt;
            if (!read_string_array_field(*tobj, "run", node.run, diags, "task", true)) return std::nullopt;
            if (!read_string_array_field(*tobj, "deps", node.deps, diags, "task", false)) return std::nullopt;
            if (!read_string_array_field(*tobj, "inputs", node.inputs, diags, "task", false)) return std::nullopt;
            if (!read_string_array_field(*tobj, "outputs", node.outputs, diags, "task", false)) return std::nullopt;
            if (!expect_bool_field(*tobj, "always_run", node.always_run, diags, "task", true, false)) return std::nullopt;

            auto cwd_it = tobj->find("cwd");
            if (cwd_it != tobj->end()) {
                if (!cwd_it->second.is_string()) {
                    diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                              "<entry>",
                              1,
                              1,
                              "task.cwd must be string");
                    return std::nullopt;
                }
                node.cwd = std::get<std::string>(cwd_it->second.data);
            }

            if (node.run.empty()) {
                diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                          "<entry>",
                          1,
                          1,
                          "task.run must not be empty");
                return std::nullopt;
            }

            g.tasks.push_back(std::move(node));
        }
    }

    if (const auto* codegens_v = find_key(graph_obj, "codegens")) {
        auto arr = std::get_if<lei::eval::Value::Array>(&codegens_v->data);
        if (!arr) {
            diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE, "<entry>", 1, 1, "codegens must be array");
            return std::nullopt;
        }

        for (const auto& c : *arr) {
            auto cobj = c.as_object();
            if (!cobj) {
                diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                          "<entry>",
                          1,
                          1,
                          "codegen entry must be object");
                return std::nullopt;
            }

            CodegenNode node{};
            if (!expect_string_field(*cobj, "name", node.name, diags, "codegen")) return std::nullopt;
            if (!read_string_array_field(*cobj, "tool", node.tool, diags, "codegen", true)) return std::nullopt;
            if (!read_string_array_field(*cobj, "inputs", node.inputs, diags, "codegen", true)) return std::nullopt;
            if (!read_string_array_field(*cobj, "outputs", node.outputs, diags, "codegen", true)) return std::nullopt;
            if (!read_string_array_field(*cobj, "args", node.args, diags, "codegen", false)) return std::nullopt;
            if (!read_string_array_field(*cobj, "deps", node.deps, diags, "codegen", false)) return std::nullopt;
            if (!expect_bool_field(*cobj, "deterministic", node.deterministic, diags, "codegen", true, true)) return std::nullopt;

            auto cwd_it = cobj->find("cwd");
            if (cwd_it != cobj->end()) {
                if (!cwd_it->second.is_string()) {
                    diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                              "<entry>",
                              1,
                              1,
                              "codegen.cwd must be string");
                    return std::nullopt;
                }
                node.cwd = std::get<std::string>(cwd_it->second.data);
            }

            if (node.outputs.empty()) {
                diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                          "<entry>",
                          1,
                          1,
                          "codegen.outputs must not be empty");
                return std::nullopt;
            }

            g.codegens.push_back(std::move(node));
        }
    }

    return g;
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

    oss << "  \"bundles\": [\n";
    for (size_t i = 0; i < graph.bundles.size(); ++i) {
        const auto& b = graph.bundles[i];
        oss << "    {\"name\": \""; append_json_escaped(oss, b.name);
        oss << "\", \"kind\": \""; append_json_escaped(oss, b.kind);
        oss << "\", \"sources\": "; append_string_array_json(oss, b.sources);
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
    oss << "bundles=" << graph.bundles.size() << "\n";
    for (const auto& b : graph.bundles) {
        oss << "  bundle " << b.name << " kind=" << b.kind << " srcs=" << b.sources.size()
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

std::optional<std::string> emit_ninja(const BuildGraph& graph, lei::diag::Bag& diags) {
    (void)diags;

    std::ostringstream oss;
    oss << "# generated by lei-build\n";
    oss << "ninja_required_version = 1.10\n\n";

    oss << "rule phony_touch\n";
    oss << "  command = /usr/bin/env true\n\n";

    std::unordered_map<std::string, std::string> bundle_stamp{};
    std::unordered_map<std::string, std::string> task_stamp{};
    std::unordered_map<std::string, std::string> codegen_stamp{};

    for (const auto& c : graph.codegens) {
        codegen_stamp[c.name] = "out/codegen/" + sanitize(c.name) + ".stamp";
    }
    for (const auto& b : graph.bundles) {
        bundle_stamp[b.name] = "out/bundle/" + sanitize(b.name) + ".stamp";
    }
    for (const auto& t : graph.tasks) {
        task_stamp[t.name] = "out/task/" + sanitize(t.name) + ".stamp";
    }

    // codegen nodes first
    for (const auto& c : graph.codegens) {
        const auto out = codegen_stamp[c.name];
        oss << "# codegen " << c.name << "\n";
        if (!c.tool.empty()) {
            oss << "# tool: ";
            for (size_t i = 0; i < c.tool.size(); ++i) {
                if (i) oss << ' ';
                oss << c.tool[i];
            }
            oss << "\n";
        }
        oss << "build " << out << ": phony_touch";
        for (const auto& in : c.inputs) oss << " " << in;
        for (const auto& dep : c.deps) {
            auto itc = codegen_stamp.find(dep);
            if (itc != codegen_stamp.end()) oss << " " << itc->second;
            auto itb = bundle_stamp.find(dep);
            if (itb != bundle_stamp.end()) oss << " " << itb->second;
            auto itt = task_stamp.find(dep);
            if (itt != task_stamp.end()) oss << " " << itt->second;
        }
        oss << "\n\n";
    }

    // bundle nodes
    for (const auto& b : graph.bundles) {
        const auto out = bundle_stamp[b.name];
        oss << "# bundle " << b.name << " (" << b.kind << ")\n";
        oss << "build " << out << ": phony_touch";
        for (const auto& src : b.sources) oss << " " << src;
        for (const auto& dep : b.deps) {
            auto it = bundle_stamp.find(dep);
            if (it != bundle_stamp.end()) oss << " " << it->second;
        }
        oss << "\n\n";
    }

    // task nodes
    for (const auto& t : graph.tasks) {
        const auto out = task_stamp[t.name];
        oss << "# task " << t.name << "\n";
        if (!t.run.empty()) {
            oss << "# run: ";
            for (size_t i = 0; i < t.run.size(); ++i) {
                if (i) oss << ' ';
                oss << t.run[i];
            }
            oss << "\n";
        }
        oss << "build " << out << ": phony_touch";
        for (const auto& in : t.inputs) oss << " " << in;
        for (const auto& dep : t.deps) {
            auto itc = codegen_stamp.find(dep);
            if (itc != codegen_stamp.end()) oss << " " << itc->second;
            auto itb = bundle_stamp.find(dep);
            if (itb != bundle_stamp.end()) oss << " " << itb->second;
            auto itt = task_stamp.find(dep);
            if (itt != task_stamp.end()) oss << " " << itt->second;
        }
        oss << "\n\n";
    }

    std::set<std::string> all_outputs;
    for (const auto& [_, s] : codegen_stamp) all_outputs.insert(s);
    for (const auto& [_, s] : bundle_stamp) all_outputs.insert(s);
    for (const auto& [_, s] : task_stamp) all_outputs.insert(s);

    oss << "build all: phony";
    for (const auto& out : all_outputs) {
        oss << " " << out;
    }
    oss << "\n";

    oss << "default all\n";

    return oss.str();
}

} // namespace lei::graph
