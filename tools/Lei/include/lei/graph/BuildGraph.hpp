#pragma once

#include <lei/diag/DiagCode.hpp>
#include <lei/eval/Evaluator.hpp>

#include <optional>
#include <string>
#include <vector>

namespace lei::graph {

struct BundleNode {
    std::string name{};
    std::string kind{};
    std::vector<std::string> sources{};
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
    std::vector<BundleNode> bundles{};
    std::vector<TaskNode> tasks{};
    std::vector<CodegenNode> codegens{};
};

std::optional<BuildGraph> from_entry_plan_value(const lei::eval::Value& entry_plan,
                                                lei::diag::Bag& diags,
                                                const std::string& entry_name = "master");

std::optional<std::string> emit_ninja(const BuildGraph& graph, lei::diag::Bag& diags);
std::optional<std::string> emit_graph_json(const BuildGraph& graph, lei::diag::Bag& diags);
std::optional<std::string> emit_graph_text(const BuildGraph& graph, lei::diag::Bag& diags);
std::optional<std::string> emit_graph_dot(const BuildGraph& graph, lei::diag::Bag& diags);

} // namespace lei::graph
