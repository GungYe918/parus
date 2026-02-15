// backend/include/parus/backend/link/Linker.hpp
#pragma once

#include <parus/backend/Backend.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace parus::backend::link {

    /// @brief 실행 파일 링크 시 사용할 링커 모드.
    enum class LinkerMode : uint8_t {
        kAuto,
        kParusLld,
        kSystemLld,
        kSystemClang,
    };

    /// @brief 단일 링크 실행 옵션.
    struct LinkOptions {
        std::vector<std::string> object_paths{};
        std::string output_path{};
        std::string target_triple{};

        LinkerMode mode = LinkerMode::kAuto;
        bool allow_fallback = true;
    };

    /// @brief 링크 실행 결과.
    struct LinkResult {
        bool ok = false;
        std::string linker_used{};
        std::vector<CompileMessage> messages{};
    };

    /// @brief object 목록을 링크해 실행 파일을 생성한다.
    LinkResult link_executable(const LinkOptions& opt);

} // namespace parus::backend::link
