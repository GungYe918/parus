#pragma once

#include <parus_tool/cli/Options.hpp>

#include <string>
#include <vector>

namespace parus_tool::doctor {

enum class Level : unsigned char {
    kPass,
    kWarn,
    kFail,
};

enum class Category : unsigned char {
    kTools,
    kEnvironment,
    kFilesystem,
    kSmoke,
};

struct Item {
    std::string id{};
    Category category = Category::kTools;
    Level level = Level::kPass;
    std::string message{};
    std::vector<std::string> hints{};
};

struct Report {
    std::vector<Item> items{};

    int exit_code(bool strict) const;
    std::string render_text(bool verbose, bool show_hints, const std::string& color_mode) const;
    std::string render_json() const;
};

Report run(const cli::DoctorOptions& doctor_opt,
           const std::string& toolchain_root,
           const char* argv0);

} // namespace parus_tool::doctor
