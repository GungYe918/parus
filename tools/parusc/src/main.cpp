// tools/parusc/src/main.cpp
#include "cli/Options.hpp"
#include "driver/Runner.hpp"
#include <parus/Version.hpp>

#include <iostream>


int main(int argc, char** argv) {
    if (argc <= 1) {
        std::cout << parus::k_version_string << "\n";
        parusc::cli::print_usage(std::cout);
        return 0;
    }

    const auto opt = parusc::cli::parse_options(argc, argv);

    if (!opt.ok) {
        std::cerr << "error: " << opt.error << "\n";
        parusc::cli::print_usage(std::cerr);
        return 1;
    }

    if (opt.mode == parusc::cli::Mode::kVersion) {
        std::cout << parus::k_version_string << "\n";
        return 0;
    }

    if (opt.mode == parusc::cli::Mode::kUsage) {
        parusc::cli::print_usage(std::cout);
        return 0;
    }

    return parusc::driver::run(opt);
}
