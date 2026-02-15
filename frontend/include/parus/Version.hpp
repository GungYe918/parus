// frontend/include/parus/Version.hpp
#pragma once
#include <string_view>


namespace parus {

    inline constexpr int k_version_major = 0;
    inline constexpr int k_version_minor = 1;
    inline constexpr int k_version_patch = 0;

    inline constexpr std::string_view k_version_string = "parus v0.1.0 (bootstrap)";

} // namespace parus
