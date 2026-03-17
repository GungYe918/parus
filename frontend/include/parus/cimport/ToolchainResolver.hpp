#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace parus::cimport {

    struct CImportToolchainProbeResult {
        std::vector<std::string> isystem_dirs{};
        std::string warning{};
    };

    CImportToolchainProbeResult probe_default_c_system_include_dirs(std::string_view compiler_hint = {});

    void append_unique_normalized_paths(std::vector<std::string>& dst,
                                        const std::vector<std::string>& src);

} // namespace parus::cimport

