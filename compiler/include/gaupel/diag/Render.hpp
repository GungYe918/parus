// compiler/include/gaupel/diag/Render.hpp
#pragma once
#include <gaupel/diag/Diagnostic.hpp>
#include <gaupel/text/SourceManager.hpp>

#include <string>


namespace gaupel::diag {

    std::string render_one(const Diagnostic& d, Language lang, const SourceManager& sm);

    /// @brief 진단을 렌더링하되, 에러 라인 주변 컨텍스트를 함께 출력
    std::string render_one_context(const Diagnostic& d, Language lang, const SourceManager& sm, uint32_t context_lines);

} // namespace gaupel::diag