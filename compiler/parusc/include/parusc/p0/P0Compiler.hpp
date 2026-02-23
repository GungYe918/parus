// compiler/parusc/include/parusc/p0/P0Compiler.hpp
#pragma once

#include <parusc/cli/Options.hpp>

#include <string>
#include <vector>

namespace parusc::p0 {

    /// @brief p0 내부 컴파일러 호출 정보.
    struct Invocation {
        std::string input_path{};
        std::string normalized_input_path{};
        std::string source_text{};
        std::string driver_executable_path{};
        std::vector<std::string> bundle_sources{};
        std::vector<std::string> bundle_deps{};
        std::vector<std::string> load_export_index_paths{};
        const cli::Options* options = nullptr;
    };

    /// @brief p0 내부 컴파일러를 실행한다.
    int run(const Invocation& inv);

} // namespace parusc::p0
