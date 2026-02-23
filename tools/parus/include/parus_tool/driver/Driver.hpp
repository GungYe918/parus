#pragma once

#include <parus_tool/cli/Options.hpp>

namespace parus_tool::driver {

int run(const cli::Options& opt, const char* argv0);

} // namespace parus_tool::driver
