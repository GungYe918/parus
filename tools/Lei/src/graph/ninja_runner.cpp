#include <lei/graph/NinjaRunner.hpp>

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lei::graph {

namespace {

std::string quote_shell(std::string_view s) {
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
        if (c == '\'') out += "'\\''";
        else out.push_back(c);
    }
    out += "'";
    return out;
}

std::string join_cmd(const std::vector<std::string>& argv) {
    std::string out{};
    for (size_t i = 0; i < argv.size(); ++i) {
        if (i) out += " ";
        out += quote_shell(argv[i]);
    }
    return out;
}

std::string ltrim(std::string s) {
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t')) ++i;
    return s.substr(i);
}

std::string rtrim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    return s;
}

std::string trim(std::string s) {
    return rtrim(ltrim(std::move(s)));
}

std::vector<std::string> split_ws(std::string_view s) {
    std::vector<std::string> out{};
    size_t i = 0;
    while (i < s.size()) {
        while (i < s.size() && std::isspace(static_cast<unsigned char>(s[i]))) ++i;
        if (i >= s.size()) break;
        size_t j = i;
        while (j < s.size() && !std::isspace(static_cast<unsigned char>(s[j]))) ++j;
        out.emplace_back(s.substr(i, j - i));
        i = j;
    }
    return out;
}

int action_rank(BuildActionKind k) {
    switch (k) {
        case BuildActionKind::kCodegen: return 0;
        case BuildActionKind::kCompile: return 1;
        case BuildActionKind::kLink: return 2;
        case BuildActionKind::kTask: return 3;
        case BuildActionKind::kPhony: return 4;
    }
    return 99;
}

bool needs_run(const ExecNode& node) {
    if (node.always_run) return true;
    if (node.outputs.empty()) return true;

    std::error_code ec{};
    std::filesystem::file_time_type oldest_out{};
    bool first_out = true;
    for (const auto& o : node.outputs) {
        if (!std::filesystem::exists(o, ec)) return true;
        if (ec) return true;
        const auto t = std::filesystem::last_write_time(o, ec);
        if (ec) return true;
        if (first_out || t < oldest_out) {
            oldest_out = t;
            first_out = false;
        }
    }

    if (node.inputs.empty()) return false;

    std::filesystem::file_time_type newest_in{};
    bool first_in = true;
    for (const auto& in : node.inputs) {
        if (!std::filesystem::exists(in, ec)) return true;
        if (ec) return true;
        const auto t = std::filesystem::last_write_time(in, ec);
        if (ec) return true;
        if (first_in || t > newest_in) {
            newest_in = t;
            first_in = false;
        }
    }

    if (first_in) return false;
    return newest_in > oldest_out;
}

bool touch_outputs(const ExecNode& node, lei::diag::Bag& diags) {
    std::error_code ec{};
    for (const auto& out : node.outputs) {
        const auto p = std::filesystem::path(out);
        if (p.has_parent_path()) {
            std::filesystem::create_directories(p.parent_path(), ec);
            if (ec) {
                diags.add(lei::diag::Code::B_NINJA_EMIT_FAILED,
                          out,
                          1,
                          1,
                          "failed to create output directory: " + p.parent_path().string());
                return false;
            }
        }

        std::ofstream ofs(out, std::ios::app);
        if (!ofs) {
            diags.add(lei::diag::Code::B_NINJA_EMIT_FAILED,
                      out,
                      1,
                      1,
                      "failed to touch output file: " + out);
            return false;
        }
    }
    return true;
}

} // namespace

bool run_embedded_ninja(const std::filesystem::path& ninja_file,
                        uint32_t jobs,
                        bool verbose,
                        lei::diag::Bag& diags) {
    if (!std::filesystem::exists(ninja_file)) {
        diags.add(lei::diag::Code::B_NINJA_EMIT_FAILED,
                  ninja_file.string(),
                  1,
                  1,
                  "ninja file does not exist");
        return false;
    }

    std::ifstream ifs(ninja_file, std::ios::binary);
    if (!ifs) {
        diags.add(lei::diag::Code::B_NINJA_EMIT_FAILED,
                  ninja_file.string(),
                  1,
                  1,
                  "failed to open ninja file");
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());

    ExecGraph graph{};
    std::unordered_map<std::string, std::string> action_by_output{};
    size_t action_index = 0;

    std::istringstream iss(text);
    std::string line;
    while (std::getline(iss, line)) {
        const std::string raw = rtrim(line);
        const std::string t = trim(raw);
        if (t.empty()) continue;
        if (t.rfind("build ", 0) != 0) continue;
        if (t == "build all: phony") continue;

        std::string spec = t.substr(6);
        const auto colon = spec.find(':');
        if (colon == std::string::npos) {
            diags.add(lei::diag::Code::B_NINJA_EMIT_FAILED,
                      ninja_file.string(),
                      1,
                      1,
                      "invalid build line: " + t);
            return false;
        }

        const std::string outputs_part = trim(spec.substr(0, colon));
        const std::string rhs = trim(spec.substr(colon + 1));
        if (outputs_part.empty() || rhs.empty()) {
            diags.add(lei::diag::Code::B_NINJA_EMIT_FAILED,
                      ninja_file.string(),
                      1,
                      1,
                      "invalid build line: " + t);
            return false;
        }

        auto rhs_tokens = split_ws(rhs);
        if (rhs_tokens.empty()) {
            diags.add(lei::diag::Code::B_NINJA_EMIT_FAILED,
                      ninja_file.string(),
                      1,
                      1,
                      "invalid build rule: " + t);
            return false;
        }

        const std::string rule = rhs_tokens.front();
        rhs_tokens.erase(rhs_tokens.begin());

        std::vector<std::string> hard_inputs{};
        std::vector<std::string> order_inputs{};
        bool order_mode = false;
        for (const auto& tok : rhs_tokens) {
            if (tok == "||") {
                order_mode = true;
                continue;
            }
            if (order_mode) order_inputs.push_back(tok);
            else hard_inputs.push_back(tok);
        }

        ExecNode node{};
        node.id = "action:ninja:" + std::to_string(action_index++);
        node.kind = (rule == "lei_touch") ? BuildActionKind::kPhony : BuildActionKind::kTask;
        node.cwd = ".";
        node.outputs = split_ws(outputs_part);
        node.inputs = hard_inputs;
        node.always_run = false;
        node.name = node.outputs.empty() ? node.id : node.outputs.front();

        // Parse indented variable lines following this build line.
        std::streampos rollback = iss.tellg();
        while (std::getline(iss, line)) {
            const std::string prop = rtrim(line);
            if (prop.empty()) {
                rollback = iss.tellg();
                continue;
            }
            if (!(prop[0] == ' ' || prop[0] == '\t')) {
                iss.seekg(rollback);
                break;
            }
            rollback = iss.tellg();

            const std::string p = trim(prop);
            if (p.rfind("desc = ", 0) == 0) {
                node.name = p.substr(7);
                continue;
            }
            if (p.rfind("cmd = ", 0) == 0) {
                node.command = {"/usr/bin/env", "sh", "-c", p.substr(6)};
                continue;
            }
        }

        graph.actions.push_back(std::move(node));
        ExecNode& added = graph.actions.back();
        for (const auto& out : added.outputs) {
            action_by_output[out] = added.id;
        }
        for (const auto& out : added.outputs) {
            graph.artifacts.push_back(ArtifactNode{"artifact:ninja:" + out, out, ArtifactKind::kGeneratedFile});
        }
        for (const auto& in : order_inputs) {
            auto it = action_by_output.find(in);
            if (it != action_by_output.end()) {
                graph.edges.push_back(ExecEdge{it->second, added.id, EdgeKind::kOrderOnly});
            }
        }
    }

    // Hard edges from produced outputs.
    for (const auto& action : graph.actions) {
        for (const auto& in : action.inputs) {
            auto it = action_by_output.find(in);
            if (it != action_by_output.end()) {
                graph.edges.push_back(ExecEdge{it->second, action.id, EdgeKind::kHard});
            }
        }
    }

    return run_embedded_ninja(graph, jobs, verbose, diags);
}

bool run_embedded_ninja(const ExecGraph& graph,
                        uint32_t jobs,
                        bool verbose,
                        lei::diag::Bag& diags) {
    if (jobs == 0) jobs = 1;

    std::unordered_map<std::string, const ExecNode*> action_by_id{};
    for (const auto& a : graph.actions) action_by_id[a.id] = &a;

    std::unordered_map<std::string, std::vector<std::string>> incoming{};
    for (const auto& e : graph.edges) incoming[e.to].push_back(e.from);

    std::vector<const ExecNode*> ordered{};
    ordered.reserve(graph.actions.size());
    for (const auto& a : graph.actions) ordered.push_back(&a);
    std::sort(ordered.begin(), ordered.end(), [](const ExecNode* a, const ExecNode* b) {
        const int ar = action_rank(a->kind);
        const int br = action_rank(b->kind);
        if (ar != br) return ar < br;
        if (a->name != b->name) return a->name < b->name;
        return a->id < b->id;
    });

    std::unordered_map<std::string, bool> done{};
    done.reserve(graph.actions.size());

    size_t completed = 0;
    size_t safety = 0;
    while (completed < ordered.size()) {
        bool progressed = false;
        ++safety;
        if (safety > ordered.size() * ordered.size() + 8) {
            diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                      "<build>",
                      1,
                      1,
                      "embedded runner stuck while scheduling actions");
            return false;
        }

        for (const auto* node : ordered) {
            if (done[node->id]) continue;

            bool ready = true;
            auto in_it = incoming.find(node->id);
            if (in_it != incoming.end()) {
                for (const auto& dep : in_it->second) {
                    if (!done[dep]) {
                        ready = false;
                        break;
                    }
                }
            }
            if (!ready) continue;

            const bool run = needs_run(*node);
            if (verbose) {
                std::cerr << "[lei] " << (run ? "run " : "skip ") << node->name << "\n";
            }

            if (run) {
                if (node->command.empty()) {
                    if (!touch_outputs(*node, diags)) return false;
                } else {
                    std::error_code ec{};
                    for (const auto& out : node->outputs) {
                        const auto p = std::filesystem::path(out);
                        if (p.has_parent_path()) {
                            std::filesystem::create_directories(p.parent_path(), ec);
                            if (ec) {
                                diags.add(lei::diag::Code::B_NINJA_EMIT_FAILED,
                                          out,
                                          1,
                                          1,
                                          "failed to create output directory: " + p.parent_path().string());
                                return false;
                            }
                        }
                    }

                    std::string cmd = join_cmd(node->command);
                    if (!node->cwd.empty() && node->cwd != ".") {
                        cmd = "cd " + quote_shell(node->cwd) + " && " + cmd;
                    }
                    const int rc = std::system(cmd.c_str());
                    if (rc != 0) {
                        diags.add(lei::diag::Code::B_NINJA_EMIT_FAILED,
                                  "<build>",
                                  1,
                                  1,
                                  "action failed: " + node->name + " (exit=" + std::to_string(rc) + ")");
                        return false;
                    }
                }
            }

            done[node->id] = true;
            ++completed;
            progressed = true;
        }

        if (!progressed) {
            diags.add(lei::diag::Code::B_INVALID_BUILD_SHAPE,
                      "<build>",
                      1,
                      1,
                      "dependency cycle detected in exec graph");
            return false;
        }
    }

    return true;
}

} // namespace lei::graph
