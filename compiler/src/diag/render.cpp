// compiler/src/diag/render.cpp
#include <gaupel/diag/Render.hpp>

#include <array>
#include <algorithm>
#include <sstream>


namespace gaupel::diag {

    static constexpr uint32_t digits10(uint32_t v) {
        uint32_t d = 1;
        while (v >= 10) { v /= 10; ++d; }
        return d;
    }

    static std::string replace_all(std::string s, std::string_view from, std::string_view to) {
        size_t pos = 0;
        while ((pos = s.find(from, pos)) != std::string::npos) {
            s.replace(pos, from.size(), to);
            pos += to.size();
        }

        return s;
    }

    static std::string format_template(std::string templ, const std::vector<std::string>& args) {
        for (size_t i = 0; i < args.size(); ++i) {
            std::string key = "{" + std::to_string(i) + "}";
            templ = replace_all(std::move(templ), key, args[i]);
        }
        return templ;
    }

    static std::string_view code_name(Code c) {
        switch (c) {
            case Code::kInvalidUtf8: return "InvalidUtf8";
            case Code::kExpectedToken: return "ExpectedToken";
            case Code::kUnexpectedToken: return "UnexpectedToken";
            case Code::kUnexpectedEof: return "UnexpectedEof";
            case Code::kTooManyErrors: return "TooManyErrors";
            case Code::kNestedTernaryNotAllowed: return "NestedTernaryNotAllowed";
            case Code::kPipeRhsMustBeCall: return "PipeRhsMustBeCall";
            case Code::kPipeHoleMustBeLabeled: return "PipeHoleMustBeLabeled";
            case Code::kPipeHoleCountMismatch: return "PipeHoleCountMismatch";
            case Code::kPipeHolePositionalNotAllowed: return "PipeHolePositionalNotAllowed";
            case Code::kCallArgMixNotAllowed: return "CallArgMixNotAllowed";
        }
        return "Unknown";
    }


    static std::string template_en(Code c) {
        switch (c) {
            // args: {0}=byte offset, {1}=byte hex
            case Code::kInvalidUtf8: return "invalid UTF-8 sequence starting at byte offset {0} (byte=0x{1})";
            case Code::kExpectedToken: return "expected '{0}'";
            case Code::kUnexpectedToken: return "unexpected token '{0}'";
            case Code::kUnexpectedEof:  return "unexpected end of file; expected {0}";
            case Code::kTooManyErrors:  return "too many errors emitted; parsing stopped";
            case Code::kNestedTernaryNotAllowed: return "nested ternary operator is not allowed";
            case Code::kPipeRhsMustBeCall: return "pipe operator '<<' requires a function call on the right-hand side";
            case Code::kPipeHoleMustBeLabeled: return "hole '_' must appear as a labeled argument value (e.g., a: _)";
            case Code::kPipeHoleCountMismatch: return "pipe call must contain exactly one labeled hole '_' (found {0})";
            case Code::kPipeHolePositionalNotAllowed: return "hole '_' is not allowed as a positional argument in pipe calls";
            case Code::kCallArgMixNotAllowed: return "mixing labeled and positional arguments is not allowed";
        }

        return "unknown diagnostic";
    }

    static std::string template_ko(Code c) {
        switch (c) {
            // args: {0}=byte offset, {1}=byte hex
            case Code::kInvalidUtf8: return "UTF-8 시퀀스가 바이트 오프셋 {0}에서 깨졌습니다 (바이트=0x{1})";
            case Code::kExpectedToken: return "'{0}'이(가) 필요합니다";
            case Code::kUnexpectedToken: return "예상치 못한 토큰 '{0}'";
            case Code::kUnexpectedEof:  return "예상치 못한 파일 끝(EOF)입니다; {0}이(가) 필요합니다";
            case Code::kTooManyErrors:  return "오류가 너무 많아 파싱을 중단합니다";
            case Code::kNestedTernaryNotAllowed: return "삼항 연산자 중첩은 허용되지 않습니다";
            case Code::kPipeRhsMustBeCall: return "파이프 연산자 '<<'의 오른쪽은 함수 호출이어야 합니다";
            case Code::kPipeHoleMustBeLabeled: return "'_'는 라벨 인자 값 위치에만 올 수 있습니다(예: a: _)";
            case Code::kPipeHoleCountMismatch: return "파이프 호출에는 라벨 인자 값으로 '_'가 정확히 1개 있어야 합니다(현재 {0}개)";
            case Code::kPipeHolePositionalNotAllowed: return "'_'는 파이프 호출에서 위치 인자로 사용할 수 없습니다";
            case Code::kCallArgMixNotAllowed: return "라벨 인자와 위치 인자를 섞어 호출할 수 없습니다";
        }

        return "알 수 없는 진단";
    }

    std::string render_one(const Diagnostic& d, Language lang, const SourceManager& sm) {
        std::string msg = (lang == Language::kKo) ? template_ko(d.code()) : template_en(d.code());
        msg = format_template(std::move(msg), d.args());

        const auto sp = d.span();
        auto lc = sm.line_col(sp.file_id, sp.lo);
        auto sn = sm.snippet_for_span(sp);

        std::ostringstream oss;
        auto sev = d.severity();
        const char* sev_name =
            (sev == Severity::kWarning) ? "warning" :
            (sev == Severity::kFatal)   ? "fatal"   : "error";

        oss << sev_name << "[" << code_name(d.code()) << "]: " << msg << "\n";
        oss << " --> " << sm.name(sp.file_id) << ":" << lc.line << ":" << lc.col << "\n";
        oss << "  |\n";
        oss << sn.line_no << " | " << sn.line_text << "\n";
        oss << "  | ";

        // spaces to caret
        for (uint32_t i = 0; i < sn.caret_cols_before; ++i) oss << ' ';

        // underline
        for (uint32_t i = 0; i < sn.caret_cols_len; ++i) oss << '^';
        
        return oss.str();
    }

    std::string render_one_context(const Diagnostic& d, Language lang, const SourceManager& sm, uint32_t context_lines) {
        std::string msg = (lang == Language::kKo) ? template_ko(d.code()) : template_en(d.code());
        msg = format_template(std::move(msg), d.args());

        const auto sp = d.span();
        auto lc = sm.line_col(sp.file_id, sp.lo);

        // 컨텍스트 스니펫
        auto blk = sm.snippet_block_for_span(sp, context_lines);

        uint32_t last_line_no = blk.first_line_no + static_cast<uint32_t>(blk.lines.size()) - 1;
        uint32_t w = digits10(last_line_no);

        std::ostringstream out;

        // error / fatal 출력
        auto sev = d.severity();
        const char* sev_name =
            (sev == Severity::kWarning) ? "warning" :
            (sev == Severity::kFatal)   ? "fatal"   : "error";

        out << sev_name << "[" << code_name(d.code()) << "]: " << msg << "\n";
        out << " --> " << sm.name(sp.file_id) << ":" << lc.line << ":" << lc.col << "\n";
        out << "  |\n";

        for (uint32_t i = 0; i < blk.lines.size(); ++i) {
            uint32_t line_no = blk.first_line_no + i;

            // "  12 | code..."
            out << std::string(2, ' ');
            {
                std::string num = std::to_string(line_no);
                out << std::string(w - static_cast<uint32_t>(num.size()), ' ') << num;
            }
            out << " | " << blk.lines[i] << "\n";

            if (i == blk.caret_line_offset) {
                out << std::string(2, ' ');
                out << std::string(w, ' ') << " | ";
                out << std::string(blk.caret_cols_before, ' ');
                out << std::string(blk.caret_cols_len, '^') << "\n";
            }
        }

        return out.str();
    }

} // namespace gaupel::diag