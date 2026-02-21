#pragma once

#include <cstdint>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace lei::diag {

enum class Code : uint16_t {
    C_UNEXPECTED_TOKEN = 1,
    C_UNEXPECTED_EOF,
    C_INVALID_LITERAL,

    L_UNKNOWN_IDENTIFIER = 100,
    L_TYPE_MISMATCH,
    L_IMPORT_NOT_FOUND,
    L_IMPORT_SYMBOL_NOT_FOUND,
    L_IMPORT_CYCLE,
    L_RECURSION_FORBIDDEN,
    L_BUDGET_EXCEEDED,
    L_DEFAULT_OVERLAY_INVALID,
    L_MERGE_CONFLICT,

    B_INVALID_BUILD_SHAPE = 200,
    B_NINJA_EMIT_FAILED,
};

inline const char* code_name(Code c) {
    switch (c) {
        case Code::C_UNEXPECTED_TOKEN: return "C_UNEXPECTED_TOKEN";
        case Code::C_UNEXPECTED_EOF: return "C_UNEXPECTED_EOF";
        case Code::C_INVALID_LITERAL: return "C_INVALID_LITERAL";
        case Code::L_UNKNOWN_IDENTIFIER: return "L_UNKNOWN_IDENTIFIER";
        case Code::L_TYPE_MISMATCH: return "L_TYPE_MISMATCH";
        case Code::L_IMPORT_NOT_FOUND: return "L_IMPORT_NOT_FOUND";
        case Code::L_IMPORT_SYMBOL_NOT_FOUND: return "L_IMPORT_SYMBOL_NOT_FOUND";
        case Code::L_IMPORT_CYCLE: return "L_IMPORT_CYCLE";
        case Code::L_RECURSION_FORBIDDEN: return "L_RECURSION_FORBIDDEN";
        case Code::L_BUDGET_EXCEEDED: return "L_BUDGET_EXCEEDED";
        case Code::L_DEFAULT_OVERLAY_INVALID: return "L_DEFAULT_OVERLAY_INVALID";
        case Code::L_MERGE_CONFLICT: return "L_MERGE_CONFLICT";
        case Code::B_INVALID_BUILD_SHAPE: return "B_INVALID_BUILD_SHAPE";
        case Code::B_NINJA_EMIT_FAILED: return "B_NINJA_EMIT_FAILED";
    }
    return "UNKNOWN";
}

struct Diagnostic {
    Code code{};
    std::string file;
    uint32_t line = 1;
    uint32_t column = 1;
    std::string message;
};

class Bag {
public:
    void add(Code code, std::string file, uint32_t line, uint32_t column, std::string message) {
        diagnostics_.push_back(Diagnostic{code, std::move(file), line, column, std::move(message)});
    }

    bool has_error() const { return !diagnostics_.empty(); }

    const std::vector<Diagnostic>& all() const { return diagnostics_; }

    std::string render_text() const {
        std::ostringstream oss;
        for (const auto& d : diagnostics_) {
            oss << "error[" << code_name(d.code) << "]: " << d.message << "\n";
            oss << " --> " << d.file << ":" << d.line << ":" << d.column << "\n";
        }
        return oss.str();
    }

private:
    std::vector<Diagnostic> diagnostics_{};
};

} // namespace lei::diag

