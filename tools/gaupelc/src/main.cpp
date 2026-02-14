// tools/gaupelc/src/main.cpp
#include "cli/Options.hpp"
#include "driver/Runner.hpp"

#include <gaupel/Version.hpp>

#include <iostream>

int main(int argc, char** argv) {
    if (argc <= 1) {
        std::cout << gaupel::k_version_string << "\n";
        gaupelc::cli::print_usage(std::cout);
        return 0;
    }

    const auto opt = gaupelc::cli::parse_options(argc, argv);

    if (!opt.ok) {
        std::cerr << "error: " << opt.error << "\n";
        gaupelc::cli::print_usage(std::cerr);
        return 1;
    }

    if (opt.mode == gaupelc::cli::Mode::kVersion) {
        std::cout << gaupel::k_version_string << "\n";
        return 0;
    }

    if (opt.mode == gaupelc::cli::Mode::kUsage) {
        gaupelc::cli::print_usage(std::cout);
        return 0;
    }

    return gaupelc::driver::run(opt);
}
