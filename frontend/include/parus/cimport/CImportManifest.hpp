#pragma once

#include <parus/cimport/CHeaderImport.hpp>

#include <string>
#include <vector>

namespace parus::cimport {

    struct CachedHeaderImportOptions {
        std::string importer_source_path{};
        std::string header_path{};
        std::string cache_root{};
        std::string target_triple{};
        std::string sysroot_path{};
        std::string apple_sdk_root{};
        std::vector<std::string> include_dirs{};
        std::vector<std::string> isystem_dirs{};
        std::vector<std::string> defines{};
        std::vector<std::string> undefines{};
        std::vector<std::string> forced_includes{};
        std::vector<std::string> imacros{};
    };

    struct CachedHeaderImportResult {
        HeaderImportResult import{};
        std::string manifest_path{};
        bool from_cache = false;
    };

    CachedHeaderImportResult load_or_translate_c_header_cached(const CachedHeaderImportOptions& opt);

} // namespace parus::cimport
