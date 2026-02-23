#pragma once

#include <parus_tool/config/Config.hpp>

#include <filesystem>
#include <string>
#include <vector>

namespace parus_tool::config::toml_lite {

bool parse_file(const std::filesystem::path& path,
                FlatMap& out,
                std::vector<std::string>& warnings,
                std::string& err);

bool write_file(const std::filesystem::path& path,
                const FlatMap& values,
                std::string& err);

} // namespace parus_tool::config::toml_lite
