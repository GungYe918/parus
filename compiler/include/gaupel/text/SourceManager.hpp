// compiler/include/gaupel/text/SourceManager.hpp
#pragma once
#include <gaupel/text/Span.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>


namespace gaupel {

    struct LineCol {
        uint32_t line = 1; // 1-based
        uint32_t col  = 1; // 1-based (in bytes for now)
    };

    struct Snippet {
        std::string_view line_text{};
        uint32_t line_no = 1;     // 1-based
        uint32_t col_lo = 1;      // 1-based
        uint32_t col_hi = 1;      // 1-based
    };

    class SourceManager {
    public:
        // Adds a "file" (or expr buffer) file_id 반환
        uint32_t add(std::string name, std::string content);

        std::string_view name(uint32_t file_id) const;
        std::string_view content(uint32_t file_id) const;

        // Convert byte offset to (line,col)
        LineCol line_col(uint32_t file_id, uint32_t byte_off) const;

        // Get the full line containing byte_off, plus col range.
        Snippet snippet_for_span(const Span& sp) const;
        
    private:
        struct File {
            std::string name;
            std::string content;
            std::vector<uint32_t> line_starts; // byte offsets, includes 0
        }; 

        static std::vector<uint32_t> build_line_starts(std::string_view s);
        std::vector<File> files_;
    };

} // namespace gaupel