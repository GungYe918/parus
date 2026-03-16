#pragma once

#include <string>


namespace parus::cimport {

    struct LibClangProbeResult {
        bool available = false;
        std::string version{};
        std::string include_dir{};
        std::string library_path{};
    };

    LibClangProbeResult probe_libclang();

} // namespace parus::cimport
