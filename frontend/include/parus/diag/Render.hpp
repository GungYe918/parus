// frontend/include/parus/diag/Render.hpp
#pragma once
#include <parus/diag/Diagnostic.hpp>
#include <parus/text/SourceManager.hpp>

#include <string>


namespace parus::diag {

    std::string render_one(const Diagnostic& d, Language lang, const SourceManager& sm);

    /// @brief 진단을 렌더링하되, 에러 라인 주변 컨텍스트를 함께 출력
    std::string render_one_context(const Diagnostic& d, Language lang, const SourceManager& sm, uint32_t context_lines);

} // namespace parus::diag