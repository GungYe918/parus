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
    C_LEGACY_SYNTAX_REMOVED,
    C_PROTO_FIELD_INVALID,
    C_RESERVED_IDENTIFIER,

    L_UNKNOWN_IDENTIFIER = 100,
    L_TYPE_MISMATCH,
    L_IMPORT_NOT_FOUND,
    L_IMPORT_SYMBOL_NOT_FOUND,
    L_IMPORT_CYCLE,
    L_RECURSION_FORBIDDEN,
    L_BUDGET_EXCEEDED,
    L_MERGE_CONFLICT,
    L_PLAN_NOT_FOUND,
    L_EXPORT_PLAN_NOT_FOUND,
    L_MASTER_EXPORT_FORBIDDEN,
    L_PROTO_REQUIRED_FIELD_MISSING,
    L_PROTO_TYPE_MISMATCH,
    L_BUILTIN_PLAN_SCHEMA_VIOLATION,
    L_LEGACY_EXPLICIT_GRAPH_REMOVED,

    B_INVALID_BUILD_SHAPE = 200,
    B_NINJA_EMIT_FAILED,
    B_MASTER_GRAPH_MISSING,
    B_VIEW_FORMAT_INVALID,
    B_IMPORT_MODULE_NOT_DECLARED,
    B_BUNDLE_DEP_NOT_DECLARED,
    B_INLINE_BUNDLE_MULTI_FORBIDDEN,
    B_BUNDLE_MODULES_REQUIRED,
    B_MODULE_SCHEMA_INVALID,
    B_MODULE_HEAD_REMOVED,
    B_MODULE_AUTO_HEAD_CONFLICT,
    B_MODULE_IMPORT_INVALID,
    B_MODULE_TOP_HEAD_COLLISION,
    B_MODULE_HEAD_COLLISION,
    B_LEGACY_BUNDLE_SOURCES_REMOVED,
};

inline const char* code_name(Code c) {
    switch (c) {
        case Code::C_UNEXPECTED_TOKEN: return "C_UNEXPECTED_TOKEN";
        case Code::C_UNEXPECTED_EOF: return "C_UNEXPECTED_EOF";
        case Code::C_INVALID_LITERAL: return "C_INVALID_LITERAL";
        case Code::C_LEGACY_SYNTAX_REMOVED: return "C_LEGACY_SYNTAX_REMOVED";
        case Code::C_PROTO_FIELD_INVALID: return "C_PROTO_FIELD_INVALID";
        case Code::C_RESERVED_IDENTIFIER: return "C_RESERVED_IDENTIFIER";
        case Code::L_UNKNOWN_IDENTIFIER: return "L_UNKNOWN_IDENTIFIER";
        case Code::L_TYPE_MISMATCH: return "L_TYPE_MISMATCH";
        case Code::L_IMPORT_NOT_FOUND: return "L_IMPORT_NOT_FOUND";
        case Code::L_IMPORT_SYMBOL_NOT_FOUND: return "L_IMPORT_SYMBOL_NOT_FOUND";
        case Code::L_IMPORT_CYCLE: return "L_IMPORT_CYCLE";
        case Code::L_RECURSION_FORBIDDEN: return "L_RECURSION_FORBIDDEN";
        case Code::L_BUDGET_EXCEEDED: return "L_BUDGET_EXCEEDED";
        case Code::L_MERGE_CONFLICT: return "L_MERGE_CONFLICT";
        case Code::L_PLAN_NOT_FOUND: return "L_PLAN_NOT_FOUND";
        case Code::L_EXPORT_PLAN_NOT_FOUND: return "L_EXPORT_PLAN_NOT_FOUND";
        case Code::L_MASTER_EXPORT_FORBIDDEN: return "L_MASTER_EXPORT_FORBIDDEN";
        case Code::L_PROTO_REQUIRED_FIELD_MISSING: return "L_PROTO_REQUIRED_FIELD_MISSING";
        case Code::L_PROTO_TYPE_MISMATCH: return "L_PROTO_TYPE_MISMATCH";
        case Code::L_BUILTIN_PLAN_SCHEMA_VIOLATION: return "L_BUILTIN_PLAN_SCHEMA_VIOLATION";
        case Code::L_LEGACY_EXPLICIT_GRAPH_REMOVED: return "L_LEGACY_EXPLICIT_GRAPH_REMOVED";
        case Code::B_INVALID_BUILD_SHAPE: return "B_INVALID_BUILD_SHAPE";
        case Code::B_NINJA_EMIT_FAILED: return "B_NINJA_EMIT_FAILED";
        case Code::B_MASTER_GRAPH_MISSING: return "B_MASTER_GRAPH_MISSING";
        case Code::B_VIEW_FORMAT_INVALID: return "B_VIEW_FORMAT_INVALID";
        case Code::B_IMPORT_MODULE_NOT_DECLARED: return "B_IMPORT_MODULE_NOT_DECLARED";
        case Code::B_BUNDLE_DEP_NOT_DECLARED: return "B_BUNDLE_DEP_NOT_DECLARED";
        case Code::B_INLINE_BUNDLE_MULTI_FORBIDDEN: return "B_INLINE_BUNDLE_MULTI_FORBIDDEN";
        case Code::B_BUNDLE_MODULES_REQUIRED: return "B_BUNDLE_MODULES_REQUIRED";
        case Code::B_MODULE_SCHEMA_INVALID: return "B_MODULE_SCHEMA_INVALID";
        case Code::B_MODULE_HEAD_REMOVED: return "B_MODULE_HEAD_REMOVED";
        case Code::B_MODULE_AUTO_HEAD_CONFLICT: return "B_MODULE_AUTO_HEAD_CONFLICT";
        case Code::B_MODULE_IMPORT_INVALID: return "B_MODULE_IMPORT_INVALID";
        case Code::B_MODULE_TOP_HEAD_COLLISION: return "B_MODULE_TOP_HEAD_COLLISION";
        case Code::B_MODULE_HEAD_COLLISION: return "B_MODULE_HEAD_COLLISION";
        case Code::B_LEGACY_BUNDLE_SOURCES_REMOVED: return "B_LEGACY_BUNDLE_SOURCES_REMOVED";
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
