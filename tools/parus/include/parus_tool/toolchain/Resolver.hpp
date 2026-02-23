#pragma once

#include <string>
#include <string_view>

namespace parus_tool::toolchain {

struct ResolveOptions {
    std::string toolchain_root{};
    const char* argv0 = nullptr;
};

std::string resolve_tool(std::string_view tool_name, const ResolveOptions& opt);

} // namespace parus_tool::toolchain
