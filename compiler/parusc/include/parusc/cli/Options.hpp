// compiler/parusc/include/parusc/cli/Options.hpp
#pragma once

#include <parus/diag/Render.hpp>
#include <parus/passes/Passes.hpp>

#include <cstdint>
#include <ostream>
#include <string>
#include <vector>

namespace parusc::cli {

    /// @brief `parusc` 실행 모드.
    enum class Mode : uint8_t {
        kUsage,
        kVersion,
        kCompile,
    };

    /// @brief `-Xparus`로만 접근 가능한 내부 개발 옵션.
    struct InternalOptions {
        bool token_dump = false;
        bool ast_dump = false;
        bool sir_dump = false;
        bool oir_dump = false;

        bool emit_llvm_ir = false;
        bool emit_object = false;
    };

    /// @brief `parusc` 최종 실행 옵션.
    struct Options {
        Mode mode = Mode::kUsage;

        std::vector<std::string> inputs{};
        std::string output_path{};
        uint8_t opt_level = 0;

        bool has_xparus = false;
        InternalOptions internal{};

        parus::diag::Language lang = parus::diag::Language::kEn;
        uint32_t context_lines = 2;
        uint32_t max_errors = 64;
        parus::passes::PassOptions pass_opt{};

        bool ok = true;
        std::string error{};
    };

    /// @brief `parusc` CLI 사용법을 출력한다.
    void print_usage(std::ostream& os);

    /// @brief CLI 인자를 파싱해 실행 옵션 구조체로 변환한다.
    Options parse_options(int argc, char** argv);

} // namespace parusc::cli
