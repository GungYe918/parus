#pragma once

#include <lei/diag/DiagCode.hpp>
#include <lei/graph/BuildGraph.hpp>

#include <cstdint>
#include <filesystem>

namespace lei::graph {

bool run_embedded_ninja(const std::filesystem::path& ninja_file,
                        uint32_t jobs,
                        bool verbose,
                        lei::diag::Bag& diags);

bool run_embedded_ninja(const ExecGraph& graph,
                        uint32_t jobs,
                        bool verbose,
                        lei::diag::Bag& diags);

} // namespace lei::graph
