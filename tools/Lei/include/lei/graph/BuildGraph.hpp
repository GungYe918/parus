#pragma once

#include <lei/diag/DiagCode.hpp>
#include <lei/eval/Evaluator.hpp>

#include <optional>
#include <string>
#include <vector>

namespace lei::graph {

struct BuildGraph {
    std::vector<std::string> bundle_names{};
    std::vector<std::string> module_files{};
};

std::optional<BuildGraph> from_build_value(const lei::eval::Value& v, lei::diag::Bag& diags);
std::optional<std::string> emit_ninja(const BuildGraph& graph, lei::diag::Bag& diags);

} // namespace lei::graph

