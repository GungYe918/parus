#include "ninja_lower_internal.hpp"

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

std::string top_head_of(std::string_view head) {
    if (head.empty()) return {};
    const size_t pos = head.find("::");
    if (pos == std::string_view::npos) return std::string(head);
    return std::string(head.substr(0, pos));
}

std::optional<std::string> canonical_import_top_head(std::string_view raw, std::string& why) {
    why.clear();
    std::string_view s = raw;
    if (s.empty()) {
        why = "module.imports entry must not be empty";
        return std::nullopt;
    }
    if (s.starts_with("::")) {
        s.remove_prefix(2);
    }
    if (s.empty()) {
        why = "module.imports entry must contain at least one segment";
        return std::nullopt;
    }
    if (s.ends_with("::")) {
        why = "module.imports entry must not end with '::'";
        return std::nullopt;
    }

    size_t begin = 0;
    std::string top{};
    while (begin < s.size()) {
        const size_t pos = s.find("::", begin);
        const size_t end = (pos == std::string_view::npos) ? s.size() : pos;
        if (end == begin) {
            why = "module.imports entry has empty path segment";
            return std::nullopt;
        }
        const std::string_view seg = s.substr(begin, end - begin);
        if (seg.find(':') != std::string_view::npos) {
            why = "module.imports entry uses invalid ':' separator";
            return std::nullopt;
        }
        if (top.empty()) {
            top = std::string(seg);
        }
        if (pos == std::string_view::npos) break;
        begin = pos + 2;
    }

    if (top.empty()) {
        why = "module.imports entry must not be empty";
        return std::nullopt;
    }
    return top;
}

std::string compute_module_head_from_source(std::string_view source, std::string_view bundle_name) {
    namespace fs = std::filesystem;
    fs::path p{std::string(source)};
    p = p.lexically_normal();
    fs::path dir = p.parent_path();

    std::vector<std::string> segs{};
    bool stripped_src = false;
    for (const auto& seg : dir) {
        const std::string s = seg.string();
        if (s.empty() || s == ".") continue;
        if (!stripped_src && s == "src") {
            stripped_src = true;
            continue;
        }
        segs.push_back(s);
    }

    if (segs.empty()) return std::string(bundle_name);

    std::string out{};
    for (size_t i = 0; i < segs.size(); ++i) {
        if (i) out += "::";
        out += segs[i];
    }
    return out;
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

bool read_cimport_isystem_field(const lei::eval::Value::Object& obj,
                                std::vector<std::string>& out,
                                lei::diag::Bag& diags,
                                const std::string& who) {
    out.clear();
    auto it = obj.find("cimport");
    if (it == obj.end()) return true;
    auto cobj = std::get_if<lei::eval::Value::Object>(&it->second.data);
    if (!cobj) {
        diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                  "<entry>",
                  1,
                  1,
                  who + " field 'cimport' must be object");
        return false;
    }
    auto it_isystem = cobj->find("isystem");
    if (it_isystem == cobj->end()) return true;
    auto arr = std::get_if<lei::eval::Value::Array>(&it_isystem->second.data);
    if (!arr) {
        diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                  "<entry>",
                  1,
                  1,
                  who + " field 'cimport.isystem' must be [string]");
        return false;
    }
    out.reserve(arr->size());
    for (const auto& v : *arr) {
        auto sp = std::get_if<std::string>(&v.data);
        if (!sp) {
            diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                      "<entry>",
                      1,
                      1,
                      who + " field 'cimport.isystem' must contain only string");
            return false;
        }
        out.push_back(*sp);
    }
    return true;
}

uint64_t fnv1a64(std::string_view s) {
    uint64_t h = 1469598103934665603ull;
    for (const unsigned char c : s) {
        h ^= static_cast<uint64_t>(c);
        h *= 1099511628211ull;
    }
    return h;
}

std::string hex64(uint64_t v) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out(16, '0');
    for (int i = 15; i >= 0; --i) {
        out[static_cast<size_t>(i)] = kHex[v & 0xfu];
        v >>= 4u;
    }
    return out;
}

std::string obj_path_for(const std::string& bundle_root,
                         const std::string& bundle_name,
                         const std::string& source) {
    const auto h = hex64(fnv1a64(source));
    return (parus_tools::paths::out_obj_dir(bundle_root) / sanitize(bundle_name) / (h + ".o"))
        .lexically_normal()
        .string();
}

std::string tool_from_env(const char* key, std::string_view fallback) {
    if (key != nullptr) {
        if (const char* v = std::getenv(key); v != nullptr && *v != '\0') {
            return std::string(v);
        }
    }
    return std::string(fallback);
}

std::string resolve_source_path(const std::string& bundle_root, const std::string& source) {
    namespace fs = std::filesystem;
    std::error_code ec{};
    fs::path p(source);
    if (p.is_relative()) {
        p = fs::path(bundle_root) / p;
    }
    fs::path canon = fs::weakly_canonical(p, ec);
    if (!ec && !canon.empty()) {
        return canon.lexically_normal().string();
    }
    return p.lexically_normal().string();
}

std::string join_shell_command(const std::vector<std::string>& argv) {
    auto quote = [](std::string_view s) -> std::string {
        bool need = s.empty();
        for (char c : s) {
            if (c == ' ' || c == '\t' || c == '\n' || c == '\"' || c == '\'' || c == '\\' || c == '$' || c == '&' || c == ';' || c == '|') {
                need = true;
                break;
            }
        }
        if (!need) return std::string(s);
        std::string out{"'"};
        for (char c : s) {
            if (c == '\'') {
                out += "'\\''";
            } else {
                out.push_back(c);
            }
        }
        out += "'";
        return out;
    };

    std::string cmd{};
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) cmd += " ";
        cmd += quote(argv[i]);
    }
    return cmd;
}

bool detect_bundle_cycle(const BuildGraph& graph, lei::diag::Bag& diags) {
    std::unordered_map<std::string, std::vector<std::string>> adj{};
    for (const auto& b : graph.bundles) adj[b.name] = b.deps;

    enum class Mark : uint8_t { kNone, kVisiting, kDone };
    std::unordered_map<std::string, Mark> mark{};
    std::vector<std::string> stack{};

    std::function<bool(const std::string&)> dfs = [&](const std::string& node) -> bool {
        auto it = mark.find(node);
        if (it != mark.end() && it->second == Mark::kVisiting) {
            std::string chain;
            for (const auto& n : stack) {
                if (!chain.empty()) chain += " -> ";
                chain += n;
            }
            if (!chain.empty()) chain += " -> ";
            chain += node;
            diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                      "<entry>",
                      1,
                      1,
                      "bundle dependency cycle detected: " + chain);
            return false;
        }
        if (it != mark.end() && it->second == Mark::kDone) return true;

        mark[node] = Mark::kVisiting;
        stack.push_back(node);
        auto ait = adj.find(node);
        if (ait != adj.end()) {
            for (const auto& dep : ait->second) {
                if (adj.find(dep) == adj.end()) continue;
                if (!dfs(dep)) return false;
            }
        }
        stack.pop_back();
        mark[node] = Mark::kDone;
        return true;
    };

    for (const auto& b : graph.bundles) {
        if (!dfs(b.name)) return false;
    }
    return true;
}

struct LowerCtx {
    ExecGraph g{};
    std::unordered_map<std::string, std::string> artifact_by_path{};

    std::unordered_map<std::string, std::string> codegen_action_by_name{};
    std::unordered_map<std::string, std::string> codegen_stamp_by_name{};
    std::unordered_map<std::string, std::string> output_file_codegen_action{};

    std::unordered_map<std::string, std::string> bundle_action_by_name{};
    std::unordered_map<std::string, std::string> bundle_stamp_or_bin_by_name{};
    std::unordered_map<std::string, std::string> bundle_prepass_action_by_name{};
    std::unordered_map<std::string, std::string> bundle_index_path_by_name{};
    std::unordered_map<std::string, std::vector<std::string>> bundle_prepass_fragment_actions_by_name{};
    std::unordered_map<std::string, std::vector<std::string>> bundle_compile_actions_by_name{};

    std::unordered_map<std::string, std::string> task_action_by_name{};
    std::unordered_map<std::string, std::string> task_stamp_by_name{};

    std::unordered_map<std::string, std::vector<std::string>> bundle_obj_artifacts{};
    std::unordered_map<std::string, std::vector<std::string>> bundle_dep_names{};
};

std::string add_artifact(LowerCtx& ctx, const std::string& path, ArtifactKind kind) {
    auto it = ctx.artifact_by_path.find(path);
    if (it != ctx.artifact_by_path.end()) return it->second;
    const std::string id = "artifact:" + hex64(fnv1a64(path));
    ctx.g.artifacts.push_back(ArtifactNode{id, path, kind});
    ctx.artifact_by_path[path] = id;
    return id;
}

std::string add_action(LowerCtx& ctx,
                       BuildActionKind kind,
                       std::string name,
                       std::string cwd,
                       std::vector<std::string> command,
                       std::vector<std::string> inputs,
                       std::vector<std::string> outputs,
                       bool always_run) {
    const std::string id = "action:" + hex64(fnv1a64(std::to_string(static_cast<int>(kind)) + ":" + name + ":" + (outputs.empty() ? std::string("none") : outputs.front())));
    ctx.g.actions.push_back(ExecNode{std::move(id), kind, std::move(name), std::move(cwd), std::move(command), std::move(inputs), std::move(outputs), always_run});
    return ctx.g.actions.back().id;
}

void add_edge(LowerCtx& ctx, std::string from, std::string to, EdgeKind kind) {
    ctx.g.edges.push_back(ExecEdge{std::move(from), std::move(to), kind});
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

    std::unordered_map<std::string, size_t> bundle_index_by_name{};
    std::unordered_map<std::string, std::string> module_owner_by_head{};
    std::unordered_map<std::string, std::string> module_owner_by_top_head{};
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

            if (bobj->find("sources") != bobj->end()) {
                diags.add(lei::diag::Code::B_LEGACY_BUNDLE_SOURCES_REMOVED,
                          "<entry>",
                          1,
                          1,
                          "bundle.sources is removed; use bundle.modules + module.sources");
                return std::nullopt;
            }

            BundleNode bundle{};
            if (!expect_string_field(*bobj, "name", bundle.name, diags, "bundle")) return std::nullopt;
            if (!expect_string_field(*bobj, "kind", bundle.kind, diags, "bundle")) return std::nullopt;
            if (!read_string_array_field(*bobj, "deps", bundle.deps, diags, "bundle", false)) return std::nullopt;
            if (!read_cimport_isystem_field(*bobj, bundle.cimport_isystem, diags, "bundle")) return std::nullopt;

            if (bundle.kind != "bin" && bundle.kind != "lib") {
                diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                          "<entry>",
                          1,
                          1,
                          "bundle.kind must be 'bin' or 'lib' in v1: " + bundle.kind);
                return std::nullopt;
            }

            auto it_modules = bobj->find("modules");
            if (it_modules == bobj->end()) {
                diags.add(lei::diag::Code::B_BUNDLE_MODULES_REQUIRED,
                          "<entry>",
                          1,
                          1,
                          "bundle.modules is required");
                return std::nullopt;
            }
            auto mod_arr = std::get_if<lei::eval::Value::Array>(&it_modules->second.data);
            if (!mod_arr) {
                diags.add(lei::diag::Code::B_BUNDLE_MODULES_REQUIRED,
                          "<entry>",
                          1,
                          1,
                          "bundle.modules must be array");
                return std::nullopt;
            }
            if (mod_arr->empty()) {
                diags.add(lei::diag::Code::B_BUNDLE_MODULES_REQUIRED,
                          "<entry>",
                          1,
                          1,
                          "bundle.modules must not be empty");
                return std::nullopt;
            }

            for (const auto& mv : *mod_arr) {
                auto mobj = mv.as_object();
                if (!mobj) {
                    diags.add(lei::diag::Code::B_MODULE_SCHEMA_INVALID,
                              "<entry>",
                              1,
                              1,
                              "module entry must be object");
                    return std::nullopt;
                }

                ModuleNode module{};
                module.bundle = bundle.name;

                if (!read_string_array_field(*mobj, "sources", module.sources, diags, "module", true)) return std::nullopt;
                std::vector<std::string> raw_imports{};
                if (!read_string_array_field(*mobj, "imports", raw_imports, diags, "module", false)) return std::nullopt;
                if (!read_cimport_isystem_field(*mobj, module.cimport_isystem, diags, "module")) return std::nullopt;

                if (mobj->find("head") != mobj->end()) {
                    diags.add(lei::diag::Code::B_MODULE_HEAD_REMOVED,
                              "<entry>",
                              1,
                              1,
                              "module.head is removed; module head is auto-computed from module.sources");
                    return std::nullopt;
                }

                if (module.sources.empty()) {
                    diags.add(lei::diag::Code::B_MODULE_SCHEMA_INVALID,
                              "<entry>",
                              1,
                              1,
                              "module.sources must not be empty");
                    return std::nullopt;
                }

                std::string computed_head{};
                for (const auto& src : module.sources) {
                    const std::string h = compute_module_head_from_source(src, bundle.name);
                    if (computed_head.empty()) {
                        computed_head = h;
                    } else if (computed_head != h) {
                        diags.add(lei::diag::Code::B_MODULE_AUTO_HEAD_CONFLICT,
                                  "<entry>",
                                  1,
                                  1,
                                  "module.sources derive conflicting heads: '" + computed_head + "' vs '" + h + "'");
                        return std::nullopt;
                    }
                }
                module.head = computed_head;

                for (const auto& import_raw : raw_imports) {
                    std::string why{};
                    auto canonical = canonical_import_top_head(import_raw, why);
                    if (!canonical.has_value()) {
                        diags.add(lei::diag::Code::B_MODULE_IMPORT_INVALID,
                                  "<entry>",
                                  1,
                                  1,
                                  "invalid module.imports entry '" + import_raw + "': " + why);
                        return std::nullopt;
                    }
                    module.imports.push_back(*canonical);
                }
                std::sort(module.imports.begin(), module.imports.end());
                module.imports.erase(std::unique(module.imports.begin(), module.imports.end()), module.imports.end());

                auto [it, inserted] = module_owner_by_head.emplace(module.head, bundle.name);
                if (!inserted) {
                    diags.add(lei::diag::Code::B_MODULE_HEAD_COLLISION,
                              "<entry>",
                              1,
                              1,
                              "duplicate module head: " + module.head);
                    return std::nullopt;
                }
                const std::string top_head = top_head_of(module.head);
                if (!top_head.empty()) {
                    auto top_it = module_owner_by_top_head.find(top_head);
                    if (top_it == module_owner_by_top_head.end()) {
                        module_owner_by_top_head.emplace(top_head, bundle.name);
                    } else if (top_it->second != bundle.name) {
                        diags.add(lei::diag::Code::B_MODULE_TOP_HEAD_COLLISION,
                                  "<entry>",
                                  1,
                                  1,
                                  "top-head collision '" + top_head + "' between bundles '" + top_it->second
                                    + "' and '" + bundle.name + "'");
                        return std::nullopt;
                    }
                }

                bundle.modules.push_back(module.head);
                g.modules.push_back(std::move(module));
            }

            bundle_index_by_name[bundle.name] = g.bundles.size();
            g.bundles.push_back(std::move(bundle));
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

    for (const auto& module : g.modules) {
        auto owner_it = bundle_index_by_name.find(module.bundle);
        if (owner_it == bundle_index_by_name.end()) continue;
        const auto& owner_bundle = g.bundles[owner_it->second];
        const std::unordered_set<std::string> dep_set(owner_bundle.deps.begin(), owner_bundle.deps.end());

        for (const auto& import_head : module.imports) {
            std::string target_bundle{};
            if (auto top_it = module_owner_by_top_head.find(import_head);
                top_it != module_owner_by_top_head.end()) {
                target_bundle = top_it->second;
            }

            if (target_bundle.empty()) {
                diags.add(lei::diag::Code::B_IMPORT_MODULE_NOT_DECLARED,
                          "<entry>",
                          1,
                          1,
                          "module '" + module.head + "' imports unknown module '" + import_head + "'");
                return std::nullopt;
            }
            if (target_bundle != module.bundle && dep_set.find(target_bundle) == dep_set.end()) {
                diags.add(lei::diag::Code::B_BUNDLE_DEP_NOT_DECLARED,
                          "<entry>",
                          1,
                          1,
                          "module '" + module.head + "' imports '" + import_head
                            + "' from bundle '" + target_bundle
                            + "' but bundle.deps does not declare it");
                return std::nullopt;
            }
        }
    }

    if (!detect_bundle_cycle(g, diags)) return std::nullopt;

    return g;
}
