#pragma once

#include <lei/diag/DiagCode.hpp>
#include <lei/eval/Evaluator.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace lei::graph {

struct ModuleNode {
    std::string head{};
    std::string bundle{};
    std::vector<std::string> sources{};
    std::vector<std::string> imports{};
};

struct BundleNode {
    std::string name{};
    std::string kind{};
    std::vector<std::string> modules{};
    std::vector<std::string> deps{};
};

struct TaskNode {
    std::string name{};
    std::vector<std::string> run{};
    std::vector<std::string> deps{};
    std::string cwd{"."};
    std::vector<std::string> inputs{};
    std::vector<std::string> outputs{};
    bool always_run = false;
};

struct CodegenNode {
    std::string name{};
    std::vector<std::string> tool{};
    std::vector<std::string> inputs{};
    std::vector<std::string> outputs{};
    std::vector<std::string> args{};
    std::vector<std::string> deps{};
    std::string cwd{"."};
    bool deterministic = true;
};

struct BuildGraph {
    std::string project_name{};
    std::string project_version{};
    std::vector<ModuleNode> modules{};
    std::vector<BundleNode> bundles{};
    std::vector<TaskNode> tasks{};
    std::vector<CodegenNode> codegens{};
};

enum class BuildActionKind : uint8_t {
    kCodegen,
    kCompile,
    kLink,
    kTask,
    kPhony,
};

enum class ArtifactKind : uint8_t {
    kGeneratedFile,
    kObjectFile,
    kBinaryFile,
    kStampFile,
};

enum class EdgeKind : uint8_t {
    kHard,
    kOrderOnly,
};

struct ArtifactNode {
    std::string id{};
    std::string path{};
    ArtifactKind kind = ArtifactKind::kGeneratedFile;
};

struct ExecNode {
    std::string id{};
    BuildActionKind kind = BuildActionKind::kPhony;
    std::string name{};
    std::string cwd{"."};
    std::vector<std::string> command{};
    std::vector<std::string> inputs{};
    std::vector<std::string> outputs{};
    bool always_run = false;
};

struct ExecEdge {
    std::string from{};
    std::string to{};
    EdgeKind kind = EdgeKind::kHard;
};

struct ExecGraph {
    std::string project_name{};
    std::string project_version{};
    std::vector<ArtifactNode> artifacts{};
    std::vector<ExecNode> actions{};
    std::vector<ExecEdge> edges{};
};

std::optional<BuildGraph> from_entry_plan_value(const lei::eval::Value& entry_plan,
                                                lei::diag::Bag& diags,
                                                const std::string& entry_name = "master");

std::optional<ExecGraph> lower_exec_graph(const BuildGraph& graph,
                                          const std::string& bundle_root,
                                          lei::diag::Bag& diags);
std::optional<std::string> emit_ninja(const ExecGraph& graph, lei::diag::Bag& diags);
std::optional<std::string> emit_graph_json(const BuildGraph& graph, lei::diag::Bag& diags);
std::optional<std::string> emit_graph_text(const BuildGraph& graph, lei::diag::Bag& diags);
std::optional<std::string> emit_graph_dot(const BuildGraph& graph, lei::diag::Bag& diags);

} // namespace lei::graph
