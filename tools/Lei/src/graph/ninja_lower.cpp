#include <lei/graph/BuildGraph.hpp>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <set>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

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

std::string obj_path_for(const std::string& bundle_name, const std::string& source) {
    const auto h = hex64(fnv1a64(source));
    return ".lei/out/obj/" + sanitize(bundle_name) + "/" + h + ".o";
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
            if (node.kind != "bin" && node.kind != "lib") {
                diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                          "<entry>",
                          1,
                          1,
                          "bundle.kind must be 'bin' or 'lib' in v1: " + node.kind);
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

    if (!detect_bundle_cycle(g, diags)) return std::nullopt;

    return g;
}

std::optional<ExecGraph> lower_exec_graph(const BuildGraph& graph, lei::diag::Bag& diags) {
    LowerCtx ctx{};
    ctx.g.project_name = graph.project_name;
    ctx.g.project_version = graph.project_version;

    std::unordered_set<std::string> names{};
    for (const auto& b : graph.bundles) {
        if (!names.insert("bundle:" + b.name).second) {
            diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE, "<entry>", 1, 1, "duplicate bundle name: " + b.name);
            return std::nullopt;
        }
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
        std::vector<std::string> obj_paths{};
        obj_paths.reserve(b.sources.size());

        const std::string index_path = ".lei-cache/index/" + sanitize(b.name) + ".exports.json";
        ctx.bundle_index_path_by_name[b.name] = index_path;
        add_artifact(ctx, index_path, ArtifactKind::kGeneratedFile);

        std::vector<std::string> prepass_cmd = {
            "parusc",
            b.sources.front(),
            "-fsyntax-only",
            "--bundle-name",
            b.name,
            "--emit-export-index",
            index_path,
        };
        for (const auto& src : b.sources) {
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
                                                      b.sources,
                                                      {index_path},
                                                      false);
        ctx.bundle_prepass_action_by_name[b.name] = prepass_action;
        ctx.bundle_compile_actions_by_name[b.name] = {};

        for (const auto& src : b.sources) {
            const std::string obj = obj_path_for(b.name, src);
            obj_paths.push_back(obj);
            add_artifact(ctx, obj, ArtifactKind::kObjectFile);

            std::vector<std::string> cmd = {
                "parusc",
                src,
                "--emit-object",
                "-o",
                obj,
            };
            cmd.push_back("--bundle-name");
            cmd.push_back(b.name);
            for (const auto& all_src : b.sources) {
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

        std::vector<std::string> cmd = {"parus-lld", "-o", bin_out};
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
