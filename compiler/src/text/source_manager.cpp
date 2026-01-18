// compiler/src/text/source_manager.cpp
#include <gaupel/text/SourceManager.hpp>

#include <algorithm>


namespace gaupel {

    std::vector<uint32_t> SourceManager::build_line_starts(std::string_view s) {
        std::vector<uint32_t> starts;
        starts.push_back(0);

        for (uint32_t i = 0; i < s.size(); ++i) {
            if (s[i] == '\n') {
                uint32_t next = i + 1;
                if (next <= s.size()) starts.push_back(next);
            }
        }
        return starts;
    }

    uint32_t SourceManager::add(std::string name, std::string content) {
        File f;
        f.name = std::move(name);
        f.content = std::move(content);
        f.line_starts = build_line_starts(f.content);
        files_.push_back(std::move(f));

        return static_cast<uint32_t>(files_.size() - 1);
    }

    std::string_view SourceManager::name(uint32_t file_id) const {
        return files_[file_id].name;
    }

    std::string_view SourceManager::content(uint32_t file_id) const {
        return files_[file_id].content;
    }

    LineCol SourceManager::line_col(uint32_t file_id, uint32_t byte_off) const {
        const auto& f = files_[file_id];
        const auto& starts = f.line_starts;
        
        // upper_bound: first start > off
        auto it = std::upper_bound(starts.begin(), starts.end(), byte_off);
        uint32_t idx = (it == starts.begin()) ? 0 : static_cast<uint32_t>((it - starts.begin()) - 1);

        uint32_t line_start = starts[idx];
        LineCol lc;
        lc.line = idx + 1;
        lc.col  = (byte_off - line_start) + 1;
        
        return lc;
    }

    Snippet SourceManager::snippet_for_span(const Span& sp) const {
        const auto& f = files_[sp.file_id];
        const auto& starts = f.line_starts;

        uint32_t lo = std::min<uint32_t>(sp.lo, static_cast<uint32_t>(f.content.size()));
        uint32_t hi = std::min<uint32_t>(sp.hi, static_cast<uint32_t>(f.content.size()));

        LineCol lc_lo = line_col(sp.file_id, lo);
        LineCol lc_hi = line_col(sp.file_id, hi);

        // v0: single-line snippet only
        uint32_t line_idx = lc_lo.line - 1;
        uint32_t line_start = starts[line_idx];
        uint32_t line_end = (line_idx + 1 < starts.size()) ? (starts[line_idx + 1] - 1) : static_cast<uint32_t>(f.content.size());

        std::string_view line_text = std::string_view(f.content).substr(line_start, line_end - line_start);

        Snippet sn;
        sn.line_text = line_text;
        sn.line_no = lc_lo.line;
        sn.col_lo = lc_lo.col;

        // if span crosses lines, clamp highlight to end of this line
        sn.col_hi = (lc_hi.line == lc_lo.line) ? std::max(lc_hi.col, lc_lo.col) : static_cast<uint32_t>(line_text.size() + 1);
        return sn;
    }

} // namespace gaupel