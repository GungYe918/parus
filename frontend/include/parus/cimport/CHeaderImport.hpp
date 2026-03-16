#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace parus::cimport {

    enum class ImportErrorKind : unsigned char {
        kNone = 0,
        kLibClangUnavailable,
        kParseFailed,
    };

    enum class CFormatKind : uint8_t {
        kNone = 0,
        kFmtVarargs,
        kFmtVList,
    };

    struct ImportedFunctionDecl {
        std::string name{};
        std::string link_name{};
        std::string type_repr{};
        bool is_c_abi = true;
        bool is_variadic = false;
        CFormatKind format_kind = CFormatKind::kNone;
        int32_t fmt_param_index = -1;
        int32_t va_list_param_index = -1;
        std::string variadic_sibling_name{};
    };

    struct ImportedUnionFieldDecl {
        std::string name{};
        std::string type_repr{};
    };

    struct ImportedUnionDecl {
        std::string name{};
        std::vector<ImportedUnionFieldDecl> fields{};
        uint32_t size_bytes = 0;
        uint32_t align_bytes = 0;
    };

    struct ImportedTypedefDecl {
        std::string name{};
        std::string type_repr{};
    };

    struct ImportedStructFieldDecl {
        std::string name{};
        std::string type_repr{};
        uint32_t offset_bytes = 0;
    };

    struct ImportedStructDecl {
        std::string name{};
        std::vector<ImportedStructFieldDecl> fields{};
        uint32_t size_bytes = 0;
        uint32_t align_bytes = 0;
    };

    struct ImportedEnumConstantDecl {
        std::string name{};
        std::string value_text{};
    };

    struct ImportedEnumDecl {
        std::string name{};
        std::string underlying_type_repr{};
        std::vector<ImportedEnumConstantDecl> constants{};
    };

    struct HeaderImportResult {
        ImportErrorKind error = ImportErrorKind::kNone;
        std::string error_text{};
        std::vector<ImportedFunctionDecl> functions{};
        std::vector<ImportedUnionDecl> unions{};
        std::vector<ImportedTypedefDecl> typedefs{};
        std::vector<ImportedStructDecl> structs{};
        std::vector<ImportedEnumDecl> enums{};
    };

    HeaderImportResult import_c_header_functions(
        const std::string& importer_source_path,
        const std::string& header_path,
        const std::vector<std::string>& include_dirs,
        const std::vector<std::string>& isystem_dirs
    );

} // namespace parus::cimport
