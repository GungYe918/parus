// compiler/src/text/source_manager.cpp
#include <gaupel/text/SourceManager.hpp>

#include <algorithm>


namespace gaupel {

    bool SourceManager::utf8_decode_one(std::string_view s, uint32_t& i, uint32_t& cp) {
        if (i >= s.size()) return false;
        unsigned char c0 = static_cast<unsigned char>(s[i]);

        // ASCII
        if (c0 < 0x80) {
            cp = c0;
            i += 1;
            return true;
        }

        auto cont = [&](uint32_t idx) -> bool {
            if (idx >= s.size()) return false;
            unsigned char cc = static_cast<unsigned char>(s[idx]);
            return (cc & 0xC0) == 0x80;
        };

        // 2-byte
        if ((c0 & 0xE0) == 0xC0) {
            if (!cont(i + 1)) { cp = 0xFFFD; i += 1; return false; }
            unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            cp = ((c0 & 0x1F) << 6) | (c1 & 0x3F);
            i += 2;
            return true;
        }

        // 3-byte
        if ((c0 & 0xF0) == 0xE0) {
            if (!cont(i + 1) || !cont(i + 2)) { cp = 0xFFFD; i += 1; return false; }
            unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
            cp = ((c0 & 0x0F) << 12) | ((c1 & 0x3F) << 6) | (c2 & 0x3F);
            i += 3;
            return true;
        }

        // 4-byte
        if ((c0 & 0xF8) == 0xF0) {
            if (!cont(i + 1) || !cont(i + 2) || !cont(i + 3)) { cp = 0xFFFD; i += 1; return false; }
            unsigned char c1 = static_cast<unsigned char>(s[i + 1]);
            unsigned char c2 = static_cast<unsigned char>(s[i + 2]);
            unsigned char c3 = static_cast<unsigned char>(s[i + 3]);
            cp = ((c0 & 0x07) << 18) | ((c1 & 0x3F) << 12) | ((c2 & 0x3F) << 6) | (c3 & 0x3F);
            i += 4;
            return true;
        }

        cp = 0xFFFD;
        i += 1;
        return false;
    }

    // ---- approximate unicode display width (0/1/2) ----
    // Covers ASCII, combining marks (0), Hangul/CJK/Fullwidth (2).
    uint32_t SourceManager::unicode_display_width(uint32_t cp) {
        // control chars
        if (cp == 0) return 0;
        if (cp < 32 || (cp >= 0x7F && cp < 0xA0)) return 0;

        // combining marks (very rough ranges)
        if ((cp >= 0x0300 && cp <= 0x036F) || (cp >= 0x1AB0 && cp <= 0x1AFF) ||
            (cp >= 0x1DC0 && cp <= 0x1DFF) || (cp >= 0x20D0 && cp <= 0x20FF) ||
            (cp >= 0xFE20 && cp <= 0xFE2F)) {
            return 0;
        }

        // Wide (CJK / 한글 / Fullwidth)
        if ((cp >= 0x1100 && cp <= 0x115F) || // 한글 자모 init
            (cp >= 0x2329 && cp <= 0x232A) ||
            (cp >= 0x2E80 && cp <= 0xA4CF) || // CJK, Yi, etc (rough)
            (cp >= 0xAC00 && cp <= 0xD7A3) || // 한글 음절
            (cp >= 0xF900 && cp <= 0xFAFF) || // CJK compat
            (cp >= 0xFE10 && cp <= 0xFE19) ||
            (cp >= 0xFE30 && cp <= 0xFE6F) ||
            (cp >= 0xFF00 && cp <= 0xFF60) || // Fullwidth forms
            (cp >= 0xFFE0 && cp <= 0xFFE6)) {
            return 2;
        }

        return 1;
    }

    uint32_t SourceManager::display_width_between(std::string_view s, uint32_t byte_lo, uint32_t byte_hi) {
        uint32_t i = byte_lo;
        uint32_t w = 0;
        while (i < byte_hi && i < s.size()) {
            uint32_t cp = 0;
            uint32_t before = i;
            utf8_decode_one(s, i, cp);
            if (i == before) i += 1;
            w += unicode_display_width(cp);
        }
        return w;
    }


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

        uint32_t off = std::min<uint32_t>(byte_off, static_cast<uint32_t>(f.content.size()));

        auto it = std::upper_bound(starts.begin(), starts.end(), off);
        uint32_t idx = (it == starts.begin()) ? 0 : static_cast<uint32_t>((it - starts.begin()) - 1);

        uint32_t line_start = starts[idx];
        std::string_view line_view = std::string_view(f.content).substr(line_start, off - line_start);

        uint32_t w = display_width_between(line_view, 0, static_cast<uint32_t>(line_view.size()));

        LineCol lc;
        lc.line = idx + 1;
        lc.col  = w + 1; // 1-based
        return lc;
    }

    Snippet SourceManager::snippet_for_span(const Span& sp) const {
        const auto& f = files_[sp.file_id];
        const auto& starts = f.line_starts;

        uint32_t lo = std::min<uint32_t>(sp.lo, static_cast<uint32_t>(f.content.size()));
        uint32_t hi = std::min<uint32_t>(sp.hi, static_cast<uint32_t>(f.content.size()));

        auto lc = line_col(sp.file_id, lo);

        // find current line [start, end)
        auto it = std::upper_bound(starts.begin(), starts.end(), lo);
        uint32_t idx = (it == starts.begin()) ? 0 : static_cast<uint32_t>((it - starts.begin()) - 1);

        uint32_t line_start = starts[idx];
        uint32_t line_end = (idx + 1 < starts.size()) ? (starts[idx + 1] - 1) : static_cast<uint32_t>(f.content.size());

        std::string_view line_text = std::string_view(f.content).substr(line_start, line_end - line_start);

        // clamp highlight within this line (v0 single-line snippet)
        uint32_t hi_clamped = std::min<uint32_t>(hi, line_end);

        uint32_t before_cols = display_width_between(line_text, 0, lo - line_start);
        uint32_t len_cols = display_width_between(line_text, lo - line_start, hi_clamped - line_start);
        if (len_cols == 0) len_cols = 1;

        Snippet sn;
        sn.line_text = line_text;
        sn.line_no = idx + 1;
        sn.col = lc.col;
        sn.caret_cols_before = before_cols;
        sn.caret_cols_len = len_cols;
        return sn;
    }


} // namespace gaupel