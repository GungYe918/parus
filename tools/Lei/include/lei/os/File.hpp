#pragma once

#include <string>
#include <string_view>

namespace lei::os {

struct ReadTextResult {
    bool ok = false;
    std::string text{};
    std::string err{};
};

ReadTextResult read_text_file(std::string_view path);
std::string normalize_path(std::string_view path);
std::string resolve_relative_path(std::string_view base_path, std::string_view rel_path);

} // namespace lei::os
