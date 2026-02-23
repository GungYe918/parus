#pragma once

#include <cstdint>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace parus_tool::cli {

enum class Mode : uint8_t {
    kUsage,
    kVersion,
    kCommand,
};

enum class Command : uint8_t {
    kNone,
    kBuild,
    kCheck,
    kGraph,
    kLsp,
    kDoctor,
    kConfig,
    kTool,
};

enum class ConfigAction : uint8_t {
    kNone,
    kPath,
    kShow,
    kGet,
    kSet,
    kUnset,
    kInit,
};

enum class ConfigScope : uint8_t {
    kAuto,
    kGlobal,
    kProject,
    kEffective,
};

enum class ConfigFormat : uint8_t {
    kToml,
    kJson,
};

struct BuildOptions {
    std::string entry{"config.lei"};
    std::string plan{"master"};
    std::optional<uint32_t> jobs{};
    bool verbose = false;
    std::optional<std::string> out_path{};
};

struct CheckOptions {
    std::string entry{"config.lei"};
    std::string plan{"master"};
    std::optional<std::string> diag_format{};
    std::optional<std::string> lang{};
    std::optional<std::string> context{};
    std::vector<std::string> macro_budget_flags{};
};

struct GraphOptions {
    std::string entry{"config.lei"};
    std::string plan{"master"};
    std::string format{"json"};
};

struct LspOptions {
    bool stdio = true;
};

struct DoctorOptions {
    bool quick = false;
    bool strict = false;
    bool json = false;
    bool verbose = false;
    bool show_hints = true;
    std::string color{"auto"};
};

struct ConfigOptions {
    ConfigAction action = ConfigAction::kNone;
    ConfigScope scope = ConfigScope::kAuto;
    ConfigFormat format = ConfigFormat::kToml;
    std::string key{};
    std::string value{};
};

struct ToolOptions {
    std::string tool_name{};
    std::vector<std::string> passthrough{};
};

struct Options {
    Mode mode = Mode::kUsage;
    Command command = Command::kNone;
    std::string toolchain_root{};

    BuildOptions build{};
    CheckOptions check{};
    GraphOptions graph{};
    LspOptions lsp{};
    DoctorOptions doctor{};
    ConfigOptions config{};
    ToolOptions tool{};

    bool ok = true;
    std::string error{};
};

void print_usage(std::ostream& os);
Options parse_options(int argc, char** argv);

} // namespace parus_tool::cli
