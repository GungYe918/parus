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
            case Code::kPipeFwdRhsMustBeCall: return "PipeFwdRhsMustBeCall";
            case Code::kPipeRevLhsMustBeCall: return "PipeRevLhsMustBeCall";
            case Code::kPipeHoleMustBeLabeled: return "PipeHoleMustBeLabeled";
            case Code::kPipeHoleCountMismatch: return "PipeHoleCountMismatch";
            case Code::kPipeHolePositionalNotAllowed: return "PipeHolePositionalNotAllowed";
            case Code::kCallArgMixNotAllowed: return "CallArgMixNotAllowed";
            case Code::kNamedGroupEntryExpectedColon: return "NamedGroupEntryExpectedColon";
            case Code::kCallOnlyOneNamedGroupAllowed: return "CallOnlyOneNamedGroupAllowed";
            case Code::kAttrNameExpectedAfterAt:      return "AttrNameExpectedAfterAt";
            case Code::kFnParamDefaultNotAllowedOutsideNamedGroup: return "FnParamDefaultNotAllowedOutsideNamedGroup";
            case Code::kFnParamDefaultExprExpected:                return "FnParamDefaultExprExpected";
            case Code::kNamedGroupLabelMustBeIdent: return "NamedGroupLabelMustBeIdent";
            case Code::kNamedGroupLabelUnderscoreReserved: return "NamedGroupLabelUnderscoreReserved";
            case Code::kFnOnlyOneNamedGroupAllowed: return "FnOnlyOneNamedGroupAllowed";
            case Code::kPubSubOnlyAllowedInClass: return "PubSubOnlyAllowedInClass";
            case Code::kTypeNameExpected: return "TypeNameExpected";
            case Code::kTypeArrayMissingRBracket: return "TypeArrayMissingRBracket";
            case Code::kTypeOptionalDuplicate: return "TypeOptionalDuplicate";
            case Code::kTypeRecovery: return "TypeRecovery";
            case Code::kWhileHeaderExpectedLParen: return "WhileHeaderExpectedLParen";
            case Code::kWhileHeaderExpectedRParen: return "WhileHeaderExpectedRParen";
            case Code::kWhileBodyExpectedBlock:    return "WhileBodyExpectedBlock";
            case Code::kLoopHeaderExpectedLParen:    return "LoopHeaderExpectedLParen";
            case Code::kLoopHeaderVarExpectedIdent:  return "LoopHeaderVarExpectedIdent";
            case Code::kLoopHeaderExpectedIn:        return "LoopHeaderExpectedIn";
            case Code::kLoopHeaderExpectedRParen:    return "LoopHeaderExpectedRParen";
            case Code::kLoopBodyExpectedBlock:       return "LoopBodyExpectedBlock";
            case Code::kSwitchHeaderExpectedLParen:  return "SwitchHeaderExpectedLParen";
            case Code::kSwitchHeaderExpectedRParen:  return "SwitchHeaderExpectedRParen";
            case Code::kSwitchBodyExpectedLBrace:    return "SwitchBodyExpectedLBrace";
            case Code::kSwitchBodyExpectedRBrace:    return "SwitchBodyExpectedRBrace";
            case Code::kSwitchCaseExpectedPattern:   return "SwitchCaseExpectedPattern";
            case Code::kSwitchCaseExpectedColon:     return "SwitchCaseExpectedColon";
            case Code::kSwitchCaseBodyExpectedBlock: return "SwitchCaseBodyExpectedBlock";
            case Code::kSwitchDefaultDuplicate:      return "SwitchDefaultDuplicate";
            case Code::kSwitchNeedsAtLeastOneCase:   return "SwitchNeedsAtLeastOneCase";
            case Code::kSwitchOnlyCaseOrDefaultAllowed: return "SwitchOnlyCaseOrDefaultAllowed";
            case Code::kVarMutMustFollowKw: return "VarMutMustFollowKw";
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
            case Code::kPipeRhsMustBeCall: return "pipe operator requires a function call on the required side";
            case Code::kPipeFwdRhsMustBeCall: return "pipe operator '|>' requires a function call on the right-hand side";
            case Code::kPipeRevLhsMustBeCall: return "pipe operator '<|' requires a function call on the left-hand side";
            case Code::kPipeHoleMustBeLabeled: return "hole '_' must appear as a labeled argument value (e.g., a: _)";
            case Code::kPipeHoleCountMismatch: return "pipe call must contain exactly one labeled hole '_' (found {0})";
            case Code::kPipeHolePositionalNotAllowed: return "hole '_' is not allowed as a positional argument in pipe calls";
            case Code::kCallArgMixNotAllowed: return "mixing labeled and positional arguments is not allowed";
            case Code::kNamedGroupEntryExpectedColon: return "named-group entry must be 'label: expr' or 'label: _'";
            case Code::kCallOnlyOneNamedGroupAllowed: return "only one named-group '{ ... }' is allowed in a call";
            case Code::kAttrNameExpectedAfterAt: return "attribute name expected after '@'";
            case Code::kFnParamDefaultNotAllowedOutsideNamedGroup: return "default value is only allowed inside named-group '{ ... }'";
            case Code::kFnParamDefaultExprExpected: return "default expression expected after '='";
            case Code::kNamedGroupLabelMustBeIdent: return "named-group label must be an identifier";
            case Code::kNamedGroupLabelUnderscoreReserved: return "'_' cannot be used as a named-group label; use it only as a value (e.g., x: _)";
            case Code::kFnOnlyOneNamedGroupAllowed: return "function parameters allow at most one named-group '{ ... }'";
            case Code::kPubSubOnlyAllowedInClass: return "'pub'/'sub' is only allowed inside a class;";
            case Code::kTypeNameExpected: return "type name expected";
            case Code::kTypeArrayMissingRBracket: return "array type suffix requires closing ']'";
            case Code::kTypeOptionalDuplicate: return "duplicate optional suffix '?'";
            case Code::kTypeRecovery: return "failed to parse type; recovered";
            case Code::kWhileHeaderExpectedLParen: return "expected '(' after 'while'";
            case Code::kWhileHeaderExpectedRParen: return "expected ')' to close while header";
            case Code::kWhileBodyExpectedBlock:    return "expected while body block '{ ... }'";
            case Code::kLoopHeaderExpectedLParen:   return "expected '(' after 'loop' header";
            case Code::kLoopHeaderVarExpectedIdent: return "loop header variable must be an identifier";
            case Code::kLoopHeaderExpectedIn:       return "expected 'in' in loop header (e.g., loop (v in xs))";
            case Code::kLoopHeaderExpectedRParen:   return "expected ')' to close loop header";
            case Code::kLoopBodyExpectedBlock:      return "expected loop body block '{ ... }'";
            case Code::kSwitchHeaderExpectedLParen: return "expected '(' after 'switch'";
            case Code::kSwitchHeaderExpectedRParen: return "expected ')' to close switch header";
            case Code::kSwitchBodyExpectedLBrace:   return "expected '{' to start switch body";
            case Code::kSwitchBodyExpectedRBrace:   return "expected '}' to close switch body";
            case Code::kSwitchCaseExpectedPattern:  return "case pattern expected (literal/ident)";
            case Code::kSwitchCaseExpectedColon:    return "expected ':' after case/default label";
            case Code::kSwitchCaseBodyExpectedBlock:return "expected case/default body block '{ ... }'";
            case Code::kSwitchDefaultDuplicate:     return "duplicate 'default' clause in switch";
            case Code::kSwitchNeedsAtLeastOneCase:  return "switch must contain at least one 'case' clause";
            case Code::kSwitchOnlyCaseOrDefaultAllowed: return "only 'case'/'default' clauses are allowed inside switch body";
            case Code::kVarMutMustFollowKw: return "'mut' must appear immediately after 'let'/'set' (e.g., 'set mut x = ...')";

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
            case Code::kPipeRhsMustBeCall: return "파이프 연산자는 필요한 쪽에 함수 호출이 있어야 합니다";
            case Code::kPipeFwdRhsMustBeCall: return "파이프 연산자 '|>'의 오른쪽은 함수 호출이어야 합니다";
            case Code::kPipeRevLhsMustBeCall: return "파이프 연산자 '<|'의 왼쪽은 함수 호출이어야 합니다";
            case Code::kPipeHoleMustBeLabeled: return "'_'는 라벨 인자 값 위치에만 올 수 있습니다(예: a: _)";
            case Code::kPipeHoleCountMismatch: return "파이프 호출에는 라벨 인자 값으로 '_'가 정확히 1개 있어야 합니다(현재 {0}개)";
            case Code::kPipeHolePositionalNotAllowed: return "'_'는 파이프 호출에서 위치 인자로 사용할 수 없습니다";
            case Code::kCallArgMixNotAllowed: return "라벨 인자와 위치 인자를 섞어 호출할 수 없습니다";
            case Code::kNamedGroupEntryExpectedColon: return "named-group entry는 'label: expr' 또는 'label: _' 형태여야 합니다";
            case Code::kCallOnlyOneNamedGroupAllowed: return "호출 인자에서 named-group '{ ... }'는 1개만 허용됩니다";
            case Code::kAttrNameExpectedAfterAt: return "'@' 뒤에는 attribute 이름이 와야 합니다";
            case Code::kFnParamDefaultNotAllowedOutsideNamedGroup: return "기본값은 named-group '{ ... }' 안에서만 사용할 수 있습니다";
            case Code::kFnParamDefaultExprExpected: return "'=' 뒤에는 기본값 식이 와야 합니다";
            case Code::kNamedGroupLabelMustBeIdent: return "named-group의 라벨은 식별자(ident)여야 합니다";
            case Code::kNamedGroupLabelUnderscoreReserved: return "'_'는 named-group의 라벨로 사용할 수 없습니다. 값 위치에서만 사용하세요(예: x: _)";
            case Code::kFnOnlyOneNamedGroupAllowed: return "함수 파라미터에서는 named-group '{ ... }'를 최대 1개만 사용할 수 있습니다";
            case Code::kPubSubOnlyAllowedInClass: return "pub/sub는 class 내부에서만 사용할 수 있습니다.";
            case Code::kTypeNameExpected: return "타입 이름(ident)이 필요합니다";
            case Code::kTypeArrayMissingRBracket: return "배열 타입 접미사 '[]'를 닫는 ']'이(가) 필요합니다";
            case Code::kTypeOptionalDuplicate: return "nullable 접미사 '?'가 중복되었습니다";
            case Code::kTypeRecovery: return "타입 파싱에 실패하여 복구했습니다";
            case Code::kWhileHeaderExpectedLParen: return "'while' 뒤에는 '('이(가) 필요합니다";
            case Code::kWhileHeaderExpectedRParen: return "while 헤더를 닫는 ')'이(가) 필요합니다";
            case Code::kWhileBodyExpectedBlock:    return "while 본문 블록 '{ ... }'이(가) 필요합니다";

            case Code::kLoopHeaderExpectedLParen:   return "'loop' 헤더 뒤에는 '('이(가) 필요합니다";
            case Code::kLoopHeaderVarExpectedIdent: return "loop 헤더의 변수는 식별자(ident)여야 합니다";
            case Code::kLoopHeaderExpectedIn:       return "loop 헤더에는 'in'이(가) 필요합니다 (예: loop (v in xs))";
            case Code::kLoopHeaderExpectedRParen:   return "loop 헤더를 닫는 ')'이(가) 필요합니다";
            case Code::kLoopBodyExpectedBlock:      return "loop 본문 블록 '{ ... }'이(가) 필요합니다";

            case Code::kSwitchHeaderExpectedLParen: return "'switch' 뒤에는 '('이(가) 필요합니다";
            case Code::kSwitchHeaderExpectedRParen: return "switch 헤더를 닫는 ')'이(가) 필요합니다";
            case Code::kSwitchBodyExpectedLBrace:   return "switch 본문을 시작하는 '{'이(가) 필요합니다";
            case Code::kSwitchBodyExpectedRBrace:   return "switch 본문을 닫는 '}'이(가) 필요합니다";
            case Code::kSwitchCaseExpectedPattern:  return "case 패턴(literal/ident)이 필요합니다";
            case Code::kSwitchCaseExpectedColon:    return "case/default 라벨 뒤에는 ':'이(가) 필요합니다";
            case Code::kSwitchCaseBodyExpectedBlock:return "case/default 본문 블록 '{ ... }'이(가) 필요합니다";
            case Code::kSwitchDefaultDuplicate:     return "switch에서 default 절은 1개만 허용됩니다";
            case Code::kSwitchNeedsAtLeastOneCase:  return "switch에는 최소 1개의 case 절이 필요합니다";
            case Code::kSwitchOnlyCaseOrDefaultAllowed: return "switch 본문에는 case/default 절만 올 수 있습니다";

            case Code::kVarMutMustFollowKw: return "'mut'는 'let/set' 바로 뒤에만 올 수 있습니다 (예: set mut x = ...)";
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