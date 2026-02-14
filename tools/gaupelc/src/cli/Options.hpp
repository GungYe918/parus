// tools/gaupelc/src/cli/Options.hpp
#pragma once

#include <gaupel/diag/Render.hpp>
#include <gaupel/passes/Passes.hpp>

#include <cstdint>
#include <ostream>
#include <string>
#include <string_view>


namespace gaupelc::cli {

    enum class Mode : uint8_t {
        kUsage,
        kVersion,
        kExpr,
        kStmt,
        kAll,
        kFile,
    };

    struct Options {
        Mode mode = Mode::kUsage;

        std::string payload{};
        bool dump_oir = false;

        gaupel::diag::Language lang = gaupel::diag::Language::kEn;
        uint32_t context_lines = 2;
        uint32_t max_errors = 64;

        gaupel::passes::PassOptions pass_opt{};

        bool ok = true;
        std::string error{};
    };

    /// @brief `gaupelc` CLI 사용법을 출력한다.
    void print_usage(std::ostream& os);

    /// @brief CLI 인자를 파싱해 실행 옵션 구조체로 변환한다.
    Options parse_options(int argc, char** argv);

} // namespace gaupelc::cli
