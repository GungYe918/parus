#pragma once

#include <parus/cimport/CHeaderImport.hpp>

#include <string>
#include <string_view>
#include <unordered_set>

namespace parus::cimport {

    std::string rewrite_cimport_type_with_alias(
        std::string_view type_repr,
        std::string_view alias,
        const std::unordered_set<std::string>& known_type_names
    );

    std::string rewrite_cimport_type_semantic_with_alias(
        std::string_view type_semantic,
        std::string_view alias,
        const std::unordered_set<std::string>& known_type_names
    );

    std::string make_c_import_payload(
        std::string_view header,
        std::string_view alias,
        const ImportedFunctionDecl& fn
    );

    std::string make_c_import_wrapper_payload(
        std::string_view header,
        std::string_view alias,
        const ImportedMacroDecl& mc
    );

    std::string make_c_import_global_payload(
        std::string_view header,
        const ImportedGlobalDecl& gv
    );

    std::string make_c_import_union_payload(
        std::string_view header,
        std::string_view alias,
        const std::unordered_set<std::string>& known_type_names,
        const ImportedUnionDecl& un
    );

    std::string make_c_import_struct_payload(
        std::string_view header,
        std::string_view alias,
        const std::unordered_set<std::string>& known_type_names,
        const ImportedStructDecl& st
    );

    std::string make_c_import_const_payload(std::string_view kind, std::string_view text);

    std::string make_c_import_typedef_payload(
        std::string_view header,
        std::string_view alias,
        const std::unordered_set<std::string>& known_type_names,
        const ImportedTypedefDecl& td
    );

    std::string make_c_import_wrapper_symbol(
        std::string_view header,
        std::string_view alias,
        std::string_view macro_name
    );

    std::string imported_macro_skip_code_text(ImportedMacroSkipKind kind);

} // namespace parus::cimport
