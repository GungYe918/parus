// compiler/include/gaupel/diag/Render.hpp
#pragma once
#include <gaupel/diag/Diagnostic.hpp>
#include <gaupel/text/SourceManager.hpp>

#include <string>


namespace gaupel::diag {

    std::string render_one(const Diagnostic& d, Language lang, const SourceManager& sm);

} // namespace gaupel::diag