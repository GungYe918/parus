#pragma once

#include <string>
#include <vector>

namespace parus_tool::proc {

int run_argv(const std::vector<std::string>& argv);
bool run_argv_capture_stdout(const std::vector<std::string>& argv, std::string& out, int& exit_code);

} // namespace parus_tool::proc
