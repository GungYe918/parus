// frontend/include/parus/text/SourceManager.hpp
#pragma once
#include <parus/text/Span.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>


namespace parus {

    struct LineCol {
        uint32_t line = 1; // 1-based
        uint32_t col  = 1; // 1-based, DISPLAY COLUMNS
    };

    struct Snippet {
        std::string_view line_text{};
        uint32_t line_no = 1;           // 1-based
        uint32_t col = 1;               // 1-based, DISPLAY COLUMNS (location)
        uint32_t caret_cols_before = 0; // number of spaces before '^'
        uint32_t caret_cols_len = 1;    // number of '^'
    };

    struct SnippetBlock {
        uint32_t first_line_no = 1;             // 1-based
        std::vector<std::string_view> lines;    // [first_line_no ...]
        uint32_t caret_line_offset = 0;         // lines[]에서 캐럿이 찍힐 줄 (0-based)
        uint32_t caret_cols_before = 0;
        uint32_t caret_cols_len = 1;
        uint32_t col = 1;                       // caret 시작 col (1-based)
    };

    class SourceManager {
    public:
        // Adds a "file" (or expr buffer) file_id 반환
        uint32_t add(std::string name, std::string content);

        std::string_view name(uint32_t file_id) const;
        std::string_view content(uint32_t file_id) const;

        // byte_off -> (line, display-col)
        LineCol line_col(uint32_t file_id, uint32_t byte_off) const;

        // single-line snippet for span (v0)
        Snippet snippet_for_span(const Span& sp) const;

        /// @brief span 기준으로 여러 줄 컨텍스트 스니펫을 생성한다.
        /// @param context_lines 위/아래로 추가로 보여줄 줄 수 (예: 2)
        SnippetBlock snippet_block_for_span(const Span& sp, uint32_t context_lines) const;
        
    private:
        struct File {
            std::string name;
            std::string content;
            std::vector<uint32_t> line_starts; // byte offsets, includes 0
        }; 

        static std::vector<uint32_t> build_line_starts(std::string_view s);

        static bool utf8_decode_one(std::string_view s, uint32_t& i, uint32_t& cp);
        static uint32_t unicode_display_width(uint32_t cp);     // 0/1/2 (approx like rust unicode-width)
        static uint32_t display_width_between(std::string_view s, uint32_t byte_lo, uint32_t byte_hi);

        static uint32_t line_index_from_byte(const File& f, uint32_t byte_off);
        static uint32_t line_end_byte(const File& f, uint32_t line_index);

        std::vector<File> files_;
    };

} // namespace parus