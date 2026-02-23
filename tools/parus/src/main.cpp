#include <parus/Version.hpp>
#include <parus_tool/cli/Options.hpp>
#include <parus_tool/driver/Driver.hpp>

#include <cstdio>
#include <cstdlib>
#include <iostream>

#if defined(_WIN32)
#include <io.h>
#else
#include <unistd.h>
#endif

namespace {

constexpr const char* kAnsiReset = "\033[0m";
constexpr const char* kAnsiRed = "\033[31m";

bool use_stderr_color() {
    if (std::getenv("NO_COLOR") != nullptr) return false;
#if defined(_WIN32)
    return _isatty(_fileno(stderr)) != 0;
#else
    return isatty(fileno(stderr)) != 0;
#endif
}

} // namespace

int main(int argc, char** argv) {
    const auto opt = parus_tool::cli::parse_options(argc, argv);

    if (!opt.ok) {
        if (use_stderr_color()) {
            std::cerr << kAnsiRed << "error: " << opt.error << kAnsiReset << "\n";
        } else {
            std::cerr << "error: " << opt.error << "\n";
        }
        parus_tool::cli::print_usage(std::cerr);
        return 1;
    }

    if (opt.mode == parus_tool::cli::Mode::kVersion) {
        std::cout << parus::k_version_string << "\n";
        return 0;
    }

    if (opt.mode == parus_tool::cli::Mode::kUsage) {
        parus_tool::cli::print_usage(std::cout);
        return 0;
    }

    return parus_tool::driver::run(opt, argv[0]);
}
