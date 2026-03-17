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

    enum class CCallConvKind : uint8_t {
        kDefault = 0,
        kCdecl,
        kStdCall,
        kFastCall,
        kVectorCall,
        kWin64,
        kSysV,
    };

    enum class ImportedConstKind : uint8_t {
        kNone = 0,
        kInt,
        kFloat,
        kBool,
        kChar,
        kString,
    };

    enum class ImportedMacroPromoteKind : uint8_t {
        kNone = 0,
        kDirectAlias,
        kShimForward,
    };

    enum class ImportedMacroSkipKind : uint8_t {
        kNone = 0,
        kSyntaxUnsupported,
        kVariadicExcluded,
        kTokenPasteExcluded,
        kStringizeExcluded,
        kStatementOrExtensionExcluded,
        kInvalidForwarding,
        kParamMismatch,
        kUnresolvedCallee,
        kUnresolvableChain,
        kChainCycleDetected,
        kConstExprUnsupported,
    };

    struct ImportedMacroCallArg {
        int32_t param_index = -1;
        std::string cast_prefix{}; // e.g. "(int)" for simple cast forwarding
    };

    struct ImportedFunctionDecl {
        std::string name{};
        std::string link_name{};
        std::string type_repr{};
        std::string c_return_type{};
        std::vector<std::string> c_arg_types{};
        std::string decl_file{};
        uint32_t decl_line = 1;
        uint32_t decl_col = 1;
        bool is_c_abi = true;
        bool is_variadic = false;
        CCallConvKind callconv = CCallConvKind::kDefault;
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
        std::string decl_file{};
        uint32_t decl_line = 1;
        uint32_t decl_col = 1;
        uint32_t size_bytes = 0;
        uint32_t align_bytes = 0;
    };

    struct ImportedTypedefDecl {
        std::string name{};
        std::string type_repr{};
        bool is_transparent = false;
        std::string transparent_type_repr{};
        std::string decl_file{};
        uint32_t decl_line = 1;
        uint32_t decl_col = 1;
    };

    struct ImportedStructFieldDecl {
        std::string name{};
        std::string type_repr{};
        std::string c_type{};
        uint32_t offset_bytes = 0;
        bool from_flatten = false;
        bool union_origin = false;
        bool is_bitfield = false;
        uint32_t bit_offset = 0;
        uint32_t bit_width = 0;
        bool bit_signed = false;
        std::string bit_getter_name{};
        std::string bit_setter_name{};
    };

    struct ImportedStructDecl {
        std::string name{};
        std::string c_type_spelling{};
        std::vector<ImportedStructFieldDecl> fields{};
        std::string decl_file{};
        uint32_t decl_line = 1;
        uint32_t decl_col = 1;
        uint32_t size_bytes = 0;
        uint32_t align_bytes = 0;
        bool is_packed = false;
    };

    struct ImportedEnumConstantDecl {
        std::string name{};
        std::string value_text{};
        std::string decl_file{};
        uint32_t decl_line = 1;
        uint32_t decl_col = 1;
    };

    struct ImportedEnumDecl {
        std::string name{};
        std::string underlying_type_repr{};
        std::vector<ImportedEnumConstantDecl> constants{};
        std::string decl_file{};
        uint32_t decl_line = 1;
        uint32_t decl_col = 1;
    };

    struct ImportedMacroDecl {
        std::string name{};
        bool is_function_like = false;
        bool is_variadic = false;
        ImportedConstKind const_kind = ImportedConstKind::kNone;
        std::string value_text{};
        std::string decl_file{};
        uint32_t decl_line = 1;
        uint32_t decl_col = 1;

        ImportedMacroPromoteKind promote_kind = ImportedMacroPromoteKind::kNone;
        std::string promote_callee_name{};
        std::string promote_callee_link_name{};
        std::string promote_type_repr{};
        std::string promote_c_return_type{};
        std::vector<std::string> params{};
        std::vector<std::string> promote_param_type_reprs{};
        std::vector<std::string> promote_param_c_types{};
        std::vector<ImportedMacroCallArg> promote_call_args{};
        std::vector<std::string> replacement_tokens{};
        ImportedMacroSkipKind skip_kind = ImportedMacroSkipKind::kNone;
        std::string skip_reason{};
    };

    struct ImportCoverageReport {
        uint32_t total_function_decls = 0;
        uint32_t imported_function_decls = 0;
        uint32_t total_type_decls = 0;
        uint32_t imported_type_decls = 0;
        uint32_t total_const_decls = 0;
        uint32_t imported_const_decls = 0;
        uint32_t total_function_macros = 0;
        uint32_t promoted_function_macros = 0;
        uint32_t skipped_function_macros = 0;
        std::vector<std::string> skipped_reasons{};
        std::vector<std::string> skipped_reason_codes{};
    };

    struct HeaderImportResult {
        ImportErrorKind error = ImportErrorKind::kNone;
        std::string error_text{};
        std::vector<ImportedFunctionDecl> functions{};
        std::vector<ImportedUnionDecl> unions{};
        std::vector<ImportedTypedefDecl> typedefs{};
        std::vector<ImportedStructDecl> structs{};
        std::vector<ImportedEnumDecl> enums{};
        std::vector<ImportedMacroDecl> macros{};
        ImportCoverageReport coverage{};
    };

    HeaderImportResult import_c_header_functions(
        const std::string& importer_source_path,
        const std::string& header_path,
        const std::vector<std::string>& include_dirs,
        const std::vector<std::string>& isystem_dirs,
        const std::vector<std::string>& defines,
        const std::vector<std::string>& undefines,
        const std::vector<std::string>& forced_includes,
        const std::vector<std::string>& imacros
    );

} // namespace parus::cimport
