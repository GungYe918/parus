#pragma once

#include <lei/diag/DiagCode.hpp>
#include <lei/eval/Evaluator.hpp>

#include <optional>
#include <string>
#include <vector>

namespace lei::graph {

struct BuildConventions {
    std::string bundles_field = "bundles";
    std::string bundle_name_field = "name";
    std::string module_map_field = "module_map";
    std::string defaults_field = "defaults";
};

BuildConventions make_default_build_conventions();

struct BuildGraph {
    std::vector<std::string> bundle_names{};
    std::vector<std::string> module_files{};
};

std::optional<BuildGraph> from_build_value(const lei::eval::Value& v,
                                           lei::diag::Bag& diags,
                                           const BuildConventions& conventions);
inline std::optional<BuildGraph> from_build_value(const lei::eval::Value& v, lei::diag::Bag& diags) {
    return from_build_value(v, diags, make_default_build_conventions());
}
std::optional<std::string> emit_ninja(const BuildGraph& graph, lei::diag::Bag& diags);

} // namespace lei::graph
