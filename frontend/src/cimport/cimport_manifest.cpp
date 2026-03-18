#include <parus/cimport/CImportManifest.hpp>

#include <parus/cimport/LibClangProbe.hpp>
#include <parus/os/File.hpp>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string_view>
#include <system_error>
#include <type_traits>
#include <utility>

namespace parus::cimport {
    namespace {

        constexpr std::string_view kManifestMagic = "parus-cimport-manifest";
        constexpr uint32_t kManifestVersion = 2;
        constexpr std::string_view kTranslatorVersion = "parus-cimport-v2";

        void fnv1a64_append_(uint64_t& h, std::string_view s) {
            for (const unsigned char ch : s) {
                h ^= static_cast<uint64_t>(ch);
                h *= 1099511628211ull;
            }
            h ^= 0xffull;
            h *= 1099511628211ull;
        }

        std::string hex_u64_(uint64_t v) {
            std::ostringstream os;
            os << std::hex << v;
            return os.str();
        }

        uint64_t last_write_time_ns_(const std::filesystem::path& path) {
            std::error_code ec{};
            const auto ts = std::filesystem::last_write_time(path, ec);
            if (ec) return 0;
            return static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(ts.time_since_epoch()).count());
        }

        ImportedDependencyFile make_dep_entry_(const std::string& raw_path) {
            ImportedDependencyFile dep{};
            if (raw_path.empty()) return dep;
            const std::filesystem::path path(raw_path);
            std::error_code ec{};
            if (!std::filesystem::exists(path, ec) || ec) return dep;
            dep.path = parus::normalize_path(path.string());
            dep.mtime_ns = last_write_time_ns_(path);
            dep.size_bytes = std::filesystem::is_regular_file(path, ec) && !ec
                ? static_cast<uint64_t>(std::filesystem::file_size(path, ec))
                : 0;
            if (ec) dep.size_bytes = 0;
            return dep;
        }

        template<typename T>
        void write_pod_(std::ostream& os, const T& value) {
            os.write(reinterpret_cast<const char*>(&value), sizeof(T));
        }

        template<typename T>
        bool read_pod_(std::istream& is, T& value) {
            is.read(reinterpret_cast<char*>(&value), sizeof(T));
            return static_cast<bool>(is);
        }

        void write_bool_(std::ostream& os, bool value) {
            const uint8_t raw = value ? 1u : 0u;
            write_pod_(os, raw);
        }

        bool read_bool_(std::istream& is, bool& value) {
            uint8_t raw = 0;
            if (!read_pod_(is, raw)) return false;
            value = (raw != 0u);
            return true;
        }

        void write_string_(std::ostream& os, const std::string& value) {
            const uint32_t n = static_cast<uint32_t>(value.size());
            write_pod_(os, n);
            if (n != 0u) {
                os.write(value.data(), static_cast<std::streamsize>(value.size()));
            }
        }

        bool read_string_(std::istream& is, std::string& value) {
            value.clear();
            uint32_t n = 0;
            if (!read_pod_(is, n)) return false;
            if (n == 0u) return true;
            value.resize(n);
            is.read(value.data(), static_cast<std::streamsize>(n));
            return static_cast<bool>(is);
        }

        template<typename E>
        void write_enum_(std::ostream& os, E value) {
            const auto raw = static_cast<std::underlying_type_t<E>>(value);
            write_pod_(os, raw);
        }

        template<typename E>
        bool read_enum_(std::istream& is, E& value) {
            std::underlying_type_t<E> raw{};
            if (!read_pod_(is, raw)) return false;
            value = static_cast<E>(raw);
            return true;
        }

        void write_call_arg_(std::ostream& os, const ImportedMacroCallArg& arg) {
            write_pod_(os, arg.param_index);
            write_string_(os, arg.cast_prefix);
        }

        bool read_call_arg_(std::istream& is, ImportedMacroCallArg& arg) {
            return read_pod_(is, arg.param_index) &&
                   read_string_(is, arg.cast_prefix);
        }

        template<typename T, typename Writer>
        void write_vector_(std::ostream& os, const std::vector<T>& values, Writer writer) {
            const uint32_t n = static_cast<uint32_t>(values.size());
            write_pod_(os, n);
            for (const auto& value : values) writer(value);
        }

        template<typename T, typename Reader>
        bool read_vector_(std::istream& is, std::vector<T>& values, Reader reader) {
            values.clear();
            uint32_t n = 0;
            if (!read_pod_(is, n)) return false;
            values.resize(n);
            for (uint32_t i = 0; i < n; ++i) {
                if (!reader(values[i])) return false;
            }
            return true;
        }

        void write_function_(std::ostream& os, const ImportedFunctionDecl& fn) {
            write_string_(os, fn.name);
            write_string_(os, fn.link_name);
            write_string_(os, fn.type_repr);
            write_string_(os, fn.type_semantic);
            write_string_(os, fn.c_return_type);
            write_vector_(os, fn.c_arg_types, [&](const std::string& s) { write_string_(os, s); });
            write_string_(os, fn.decl_file);
            write_pod_(os, fn.decl_line);
            write_pod_(os, fn.decl_col);
            write_bool_(os, fn.is_c_abi);
            write_bool_(os, fn.is_variadic);
            write_enum_(os, fn.callconv);
            write_enum_(os, fn.format_kind);
            write_pod_(os, fn.fmt_param_index);
            write_pod_(os, fn.va_list_param_index);
            write_string_(os, fn.variadic_sibling_name);
        }

        bool read_function_(std::istream& is, ImportedFunctionDecl& fn) {
            return read_string_(is, fn.name) &&
                   read_string_(is, fn.link_name) &&
                   read_string_(is, fn.type_repr) &&
                   read_string_(is, fn.type_semantic) &&
                   read_string_(is, fn.c_return_type) &&
                   read_vector_(is, fn.c_arg_types, [&](std::string& s) { return read_string_(is, s); }) &&
                   read_string_(is, fn.decl_file) &&
                   read_pod_(is, fn.decl_line) &&
                   read_pod_(is, fn.decl_col) &&
                   read_bool_(is, fn.is_c_abi) &&
                   read_bool_(is, fn.is_variadic) &&
                   read_enum_(is, fn.callconv) &&
                   read_enum_(is, fn.format_kind) &&
                   read_pod_(is, fn.fmt_param_index) &&
                   read_pod_(is, fn.va_list_param_index) &&
                   read_string_(is, fn.variadic_sibling_name);
        }

        void write_global_(std::ostream& os, const ImportedGlobalDecl& gv) {
            write_string_(os, gv.name);
            write_string_(os, gv.link_name);
            write_string_(os, gv.type_repr);
            write_string_(os, gv.type_semantic);
            write_string_(os, gv.c_type);
            write_string_(os, gv.decl_file);
            write_pod_(os, gv.decl_line);
            write_pod_(os, gv.decl_col);
            write_bool_(os, gv.is_c_abi);
            write_bool_(os, gv.is_const);
            write_bool_(os, gv.is_volatile);
            write_bool_(os, gv.is_restrict);
            write_enum_(os, gv.tls_kind);
        }

        bool read_global_(std::istream& is, ImportedGlobalDecl& gv) {
            return read_string_(is, gv.name) &&
                   read_string_(is, gv.link_name) &&
                   read_string_(is, gv.type_repr) &&
                   read_string_(is, gv.type_semantic) &&
                   read_string_(is, gv.c_type) &&
                   read_string_(is, gv.decl_file) &&
                   read_pod_(is, gv.decl_line) &&
                   read_pod_(is, gv.decl_col) &&
                   read_bool_(is, gv.is_c_abi) &&
                   read_bool_(is, gv.is_const) &&
                   read_bool_(is, gv.is_volatile) &&
                   read_bool_(is, gv.is_restrict) &&
                   read_enum_(is, gv.tls_kind);
        }

        void write_union_field_(std::ostream& os, const ImportedUnionFieldDecl& field) {
            write_string_(os, field.name);
            write_string_(os, field.type_repr);
            write_string_(os, field.type_semantic);
        }

        bool read_union_field_(std::istream& is, ImportedUnionFieldDecl& field) {
            return read_string_(is, field.name) &&
                   read_string_(is, field.type_repr) &&
                   read_string_(is, field.type_semantic);
        }

        void write_union_(std::ostream& os, const ImportedUnionDecl& un) {
            write_string_(os, un.name);
            write_vector_(os, un.fields, [&](const ImportedUnionFieldDecl& field) { write_union_field_(os, field); });
            write_string_(os, un.decl_file);
            write_pod_(os, un.decl_line);
            write_pod_(os, un.decl_col);
            write_pod_(os, un.size_bytes);
            write_pod_(os, un.align_bytes);
        }

        bool read_union_(std::istream& is, ImportedUnionDecl& un) {
            return read_string_(is, un.name) &&
                   read_vector_(is, un.fields, [&](ImportedUnionFieldDecl& field) { return read_union_field_(is, field); }) &&
                   read_string_(is, un.decl_file) &&
                   read_pod_(is, un.decl_line) &&
                   read_pod_(is, un.decl_col) &&
                   read_pod_(is, un.size_bytes) &&
                   read_pod_(is, un.align_bytes);
        }

        void write_typedef_(std::ostream& os, const ImportedTypedefDecl& td) {
            write_string_(os, td.name);
            write_string_(os, td.type_repr);
            write_string_(os, td.type_semantic);
            write_bool_(os, td.is_transparent);
            write_string_(os, td.transparent_type_repr);
            write_string_(os, td.transparent_type_semantic);
            write_string_(os, td.decl_file);
            write_pod_(os, td.decl_line);
            write_pod_(os, td.decl_col);
        }

        bool read_typedef_(std::istream& is, ImportedTypedefDecl& td) {
            return read_string_(is, td.name) &&
                   read_string_(is, td.type_repr) &&
                   read_string_(is, td.type_semantic) &&
                   read_bool_(is, td.is_transparent) &&
                   read_string_(is, td.transparent_type_repr) &&
                   read_string_(is, td.transparent_type_semantic) &&
                   read_string_(is, td.decl_file) &&
                   read_pod_(is, td.decl_line) &&
                   read_pod_(is, td.decl_col);
        }

        void write_struct_field_(std::ostream& os, const ImportedStructFieldDecl& field) {
            write_string_(os, field.name);
            write_string_(os, field.type_repr);
            write_string_(os, field.type_semantic);
            write_string_(os, field.c_type);
            write_pod_(os, field.offset_bytes);
            write_bool_(os, field.from_flatten);
            write_bool_(os, field.union_origin);
            write_bool_(os, field.is_bitfield);
            write_pod_(os, field.bit_offset);
            write_pod_(os, field.bit_width);
            write_bool_(os, field.bit_signed);
            write_pod_(os, field.bit_storage_offset_bytes);
        }

        bool read_struct_field_(std::istream& is, ImportedStructFieldDecl& field) {
            return read_string_(is, field.name) &&
                   read_string_(is, field.type_repr) &&
                   read_string_(is, field.type_semantic) &&
                   read_string_(is, field.c_type) &&
                   read_pod_(is, field.offset_bytes) &&
                   read_bool_(is, field.from_flatten) &&
                   read_bool_(is, field.union_origin) &&
                   read_bool_(is, field.is_bitfield) &&
                   read_pod_(is, field.bit_offset) &&
                   read_pod_(is, field.bit_width) &&
                   read_bool_(is, field.bit_signed) &&
                   read_pod_(is, field.bit_storage_offset_bytes);
        }

        void write_struct_(std::ostream& os, const ImportedStructDecl& st) {
            write_string_(os, st.name);
            write_string_(os, st.c_type_spelling);
            write_vector_(os, st.fields, [&](const ImportedStructFieldDecl& field) { write_struct_field_(os, field); });
            write_string_(os, st.decl_file);
            write_pod_(os, st.decl_line);
            write_pod_(os, st.decl_col);
            write_pod_(os, st.size_bytes);
            write_pod_(os, st.align_bytes);
            write_bool_(os, st.is_packed);
        }

        bool read_struct_(std::istream& is, ImportedStructDecl& st) {
            return read_string_(is, st.name) &&
                   read_string_(is, st.c_type_spelling) &&
                   read_vector_(is, st.fields, [&](ImportedStructFieldDecl& field) { return read_struct_field_(is, field); }) &&
                   read_string_(is, st.decl_file) &&
                   read_pod_(is, st.decl_line) &&
                   read_pod_(is, st.decl_col) &&
                   read_pod_(is, st.size_bytes) &&
                   read_pod_(is, st.align_bytes) &&
                   read_bool_(is, st.is_packed);
        }

        void write_enum_const_(std::ostream& os, const ImportedEnumConstantDecl& cst) {
            write_string_(os, cst.name);
            write_string_(os, cst.value_text);
            write_string_(os, cst.decl_file);
            write_pod_(os, cst.decl_line);
            write_pod_(os, cst.decl_col);
        }

        bool read_enum_const_(std::istream& is, ImportedEnumConstantDecl& cst) {
            return read_string_(is, cst.name) &&
                   read_string_(is, cst.value_text) &&
                   read_string_(is, cst.decl_file) &&
                   read_pod_(is, cst.decl_line) &&
                   read_pod_(is, cst.decl_col);
        }

        void write_enum_(std::ostream& os, const ImportedEnumDecl& en) {
            write_string_(os, en.name);
            write_string_(os, en.underlying_type_repr);
            write_vector_(os, en.constants, [&](const ImportedEnumConstantDecl& cst) { write_enum_const_(os, cst); });
            write_string_(os, en.decl_file);
            write_pod_(os, en.decl_line);
            write_pod_(os, en.decl_col);
        }

        bool read_enum_(std::istream& is, ImportedEnumDecl& en) {
            return read_string_(is, en.name) &&
                   read_string_(is, en.underlying_type_repr) &&
                   read_vector_(is, en.constants, [&](ImportedEnumConstantDecl& cst) { return read_enum_const_(is, cst); }) &&
                   read_string_(is, en.decl_file) &&
                   read_pod_(is, en.decl_line) &&
                   read_pod_(is, en.decl_col);
        }

        void write_macro_(std::ostream& os, const ImportedMacroDecl& mc) {
            write_string_(os, mc.name);
            write_bool_(os, mc.is_function_like);
            write_bool_(os, mc.is_variadic);
            write_enum_(os, mc.const_kind);
            write_string_(os, mc.value_text);
            write_string_(os, mc.decl_file);
            write_pod_(os, mc.decl_line);
            write_pod_(os, mc.decl_col);
            write_enum_(os, mc.promote_kind);
            write_string_(os, mc.promote_callee_name);
            write_string_(os, mc.promote_callee_link_name);
            write_string_(os, mc.promote_type_repr);
            write_string_(os, mc.promote_type_semantic);
            write_string_(os, mc.promote_c_return_type);
            write_vector_(os, mc.params, [&](const std::string& s) { write_string_(os, s); });
            write_vector_(os, mc.promote_param_type_reprs, [&](const std::string& s) { write_string_(os, s); });
            write_vector_(os, mc.promote_param_c_types, [&](const std::string& s) { write_string_(os, s); });
            write_vector_(os, mc.promote_call_args, [&](const ImportedMacroCallArg& arg) { write_call_arg_(os, arg); });
            write_vector_(os, mc.replacement_tokens, [&](const std::string& s) { write_string_(os, s); });
            write_enum_(os, mc.skip_kind);
            write_string_(os, mc.skip_reason);
            write_bool_(os, mc.promote_is_c_abi);
            write_bool_(os, mc.promote_is_variadic);
            write_enum_(os, mc.promote_callconv);
        }

        bool read_macro_(std::istream& is, ImportedMacroDecl& mc) {
            return read_string_(is, mc.name) &&
                   read_bool_(is, mc.is_function_like) &&
                   read_bool_(is, mc.is_variadic) &&
                   read_enum_(is, mc.const_kind) &&
                   read_string_(is, mc.value_text) &&
                   read_string_(is, mc.decl_file) &&
                   read_pod_(is, mc.decl_line) &&
                   read_pod_(is, mc.decl_col) &&
                   read_enum_(is, mc.promote_kind) &&
                   read_string_(is, mc.promote_callee_name) &&
                   read_string_(is, mc.promote_callee_link_name) &&
                   read_string_(is, mc.promote_type_repr) &&
                   read_string_(is, mc.promote_type_semantic) &&
                   read_string_(is, mc.promote_c_return_type) &&
                   read_vector_(is, mc.params, [&](std::string& s) { return read_string_(is, s); }) &&
                   read_vector_(is, mc.promote_param_type_reprs, [&](std::string& s) { return read_string_(is, s); }) &&
                   read_vector_(is, mc.promote_param_c_types, [&](std::string& s) { return read_string_(is, s); }) &&
                   read_vector_(is, mc.promote_call_args, [&](ImportedMacroCallArg& arg) { return read_call_arg_(is, arg); }) &&
                   read_vector_(is, mc.replacement_tokens, [&](std::string& s) { return read_string_(is, s); }) &&
                   read_enum_(is, mc.skip_kind) &&
                   read_string_(is, mc.skip_reason) &&
                   read_bool_(is, mc.promote_is_c_abi) &&
                   read_bool_(is, mc.promote_is_variadic) &&
                   read_enum_(is, mc.promote_callconv);
        }

        void write_dep_(std::ostream& os, const ImportedDependencyFile& dep) {
            write_string_(os, dep.path);
            write_pod_(os, dep.mtime_ns);
            write_pod_(os, dep.size_bytes);
        }

        bool read_dep_(std::istream& is, ImportedDependencyFile& dep) {
            return read_string_(is, dep.path) &&
                   read_pod_(is, dep.mtime_ns) &&
                   read_pod_(is, dep.size_bytes);
        }

        void write_coverage_(std::ostream& os, const ImportCoverageReport& cov) {
            write_pod_(os, cov.total_function_decls);
            write_pod_(os, cov.imported_function_decls);
            write_pod_(os, cov.total_global_decls);
            write_pod_(os, cov.imported_global_decls);
            write_pod_(os, cov.total_type_decls);
            write_pod_(os, cov.imported_type_decls);
            write_pod_(os, cov.total_const_decls);
            write_pod_(os, cov.imported_const_decls);
            write_pod_(os, cov.total_function_macros);
            write_pod_(os, cov.promoted_function_macros);
            write_pod_(os, cov.skipped_function_macros);
            write_vector_(os, cov.skipped_reasons, [&](const std::string& s) { write_string_(os, s); });
            write_vector_(os, cov.skipped_reason_codes, [&](const std::string& s) { write_string_(os, s); });
            write_vector_(os, cov.dropped_decl_reasons, [&](const std::string& s) { write_string_(os, s); });
            write_vector_(os, cov.dropped_decl_reason_codes, [&](const std::string& s) { write_string_(os, s); });
        }

        bool read_coverage_(std::istream& is, ImportCoverageReport& cov) {
            return read_pod_(is, cov.total_function_decls) &&
                   read_pod_(is, cov.imported_function_decls) &&
                   read_pod_(is, cov.total_global_decls) &&
                   read_pod_(is, cov.imported_global_decls) &&
                   read_pod_(is, cov.total_type_decls) &&
                   read_pod_(is, cov.imported_type_decls) &&
                   read_pod_(is, cov.total_const_decls) &&
                   read_pod_(is, cov.imported_const_decls) &&
                   read_pod_(is, cov.total_function_macros) &&
                   read_pod_(is, cov.promoted_function_macros) &&
                   read_pod_(is, cov.skipped_function_macros) &&
                   read_vector_(is, cov.skipped_reasons, [&](std::string& s) { return read_string_(is, s); }) &&
                   read_vector_(is, cov.skipped_reason_codes, [&](std::string& s) { return read_string_(is, s); }) &&
                   read_vector_(is, cov.dropped_decl_reasons, [&](std::string& s) { return read_string_(is, s); }) &&
                   read_vector_(is, cov.dropped_decl_reason_codes, [&](std::string& s) { return read_string_(is, s); });
        }

        bool write_manifest_(const std::filesystem::path& path,
                             const CachedHeaderImportOptions& opt,
                             const HeaderImportResult& result,
                             const std::string& libclang_version) {
            std::error_code ec{};
            std::filesystem::create_directories(path.parent_path(), ec);
            if (ec) return false;

            std::ofstream os(path, std::ios::binary | std::ios::trunc);
            if (!os.is_open()) return false;

            write_string_(os, std::string(kManifestMagic));
            write_pod_(os, kManifestVersion);
            write_string_(os, std::string(kTranslatorVersion));
            write_string_(os, libclang_version);
            write_string_(os, parus::normalize_path(opt.header_path));
            write_string_(os, opt.target_triple);
            write_string_(os, parus::normalize_path(opt.sysroot_path));
            write_string_(os, parus::normalize_path(opt.apple_sdk_root));
            write_vector_(os, opt.include_dirs, [&](const std::string& s) { write_string_(os, s); });
            write_vector_(os, opt.isystem_dirs, [&](const std::string& s) { write_string_(os, s); });
            write_vector_(os, opt.defines, [&](const std::string& s) { write_string_(os, s); });
            write_vector_(os, opt.undefines, [&](const std::string& s) { write_string_(os, s); });
            write_vector_(os, opt.forced_includes, [&](const std::string& s) { write_string_(os, s); });
            write_vector_(os, opt.imacros, [&](const std::string& s) { write_string_(os, s); });

            write_enum_(os, result.error);
            write_string_(os, result.error_text);
            write_vector_(os, result.functions, [&](const ImportedFunctionDecl& fn) { write_function_(os, fn); });
            write_vector_(os, result.globals, [&](const ImportedGlobalDecl& gv) { write_global_(os, gv); });
            write_vector_(os, result.unions, [&](const ImportedUnionDecl& un) { write_union_(os, un); });
            write_vector_(os, result.typedefs, [&](const ImportedTypedefDecl& td) { write_typedef_(os, td); });
            write_vector_(os, result.structs, [&](const ImportedStructDecl& st) { write_struct_(os, st); });
            write_vector_(os, result.enums, [&](const ImportedEnumDecl& en) { write_enum_(os, en); });
            write_vector_(os, result.macros, [&](const ImportedMacroDecl& mc) { write_macro_(os, mc); });
            write_vector_(os, result.dependency_files, [&](const ImportedDependencyFile& dep) { write_dep_(os, dep); });
            write_coverage_(os, result.coverage);
            return static_cast<bool>(os);
        }

        bool read_manifest_(const std::filesystem::path& path,
                            HeaderImportResult& out,
                            std::string& translator_version,
                            std::string& libclang_version) {
            std::ifstream is(path, std::ios::binary);
            if (!is.is_open()) return false;

            std::string magic{};
            uint32_t version = 0;
            if (!read_string_(is, magic) ||
                !read_pod_(is, version) ||
                !read_string_(is, translator_version) ||
                !read_string_(is, libclang_version)) {
                return false;
            }
            if (magic != kManifestMagic || version != kManifestVersion) return false;

            std::string ignore{};
            std::vector<std::string> ignore_vec{};
            if (!read_string_(is, ignore) ||
                !read_string_(is, ignore) ||
                !read_string_(is, ignore) ||
                !read_string_(is, ignore) ||
                !read_vector_(is, ignore_vec, [&](std::string& s) { return read_string_(is, s); }) ||
                !read_vector_(is, ignore_vec, [&](std::string& s) { return read_string_(is, s); }) ||
                !read_vector_(is, ignore_vec, [&](std::string& s) { return read_string_(is, s); }) ||
                !read_vector_(is, ignore_vec, [&](std::string& s) { return read_string_(is, s); }) ||
                !read_vector_(is, ignore_vec, [&](std::string& s) { return read_string_(is, s); }) ||
                !read_vector_(is, ignore_vec, [&](std::string& s) { return read_string_(is, s); })) {
                return false;
            }

            if (!read_enum_(is, out.error) ||
                !read_string_(is, out.error_text) ||
                !read_vector_(is, out.functions, [&](ImportedFunctionDecl& fn) { return read_function_(is, fn); }) ||
                !read_vector_(is, out.globals, [&](ImportedGlobalDecl& gv) { return read_global_(is, gv); }) ||
                !read_vector_(is, out.unions, [&](ImportedUnionDecl& un) { return read_union_(is, un); }) ||
                !read_vector_(is, out.typedefs, [&](ImportedTypedefDecl& td) { return read_typedef_(is, td); }) ||
                !read_vector_(is, out.structs, [&](ImportedStructDecl& st) { return read_struct_(is, st); }) ||
                !read_vector_(is, out.enums, [&](ImportedEnumDecl& en) { return read_enum_(is, en); }) ||
                !read_vector_(is, out.macros, [&](ImportedMacroDecl& mc) { return read_macro_(is, mc); }) ||
                !read_vector_(is, out.dependency_files, [&](ImportedDependencyFile& dep) { return read_dep_(is, dep); }) ||
                !read_coverage_(is, out.coverage)) {
                return false;
            }
            out.libclang_version = libclang_version;
            return true;
        }

        bool deps_are_fresh_(const std::vector<ImportedDependencyFile>& deps) {
            for (const auto& dep : deps) {
                if (dep.path.empty()) return false;
                const auto current = make_dep_entry_(dep.path);
                if (current.path.empty()) return false;
                if (current.mtime_ns != dep.mtime_ns || current.size_bytes != dep.size_bytes) {
                    return false;
                }
            }
            return true;
        }

        std::string compute_cache_key_(const CachedHeaderImportOptions& opt, std::string_view libclang_version) {
            uint64_t hash = 1469598103934665603ull;
            auto append = [&](std::string_view s) { fnv1a64_append_(hash, s); };
            append(parus::normalize_path(opt.header_path));
            append(opt.target_triple);
            append(parus::normalize_path(opt.sysroot_path));
            append(parus::normalize_path(opt.apple_sdk_root));
            append(libclang_version);
            append(kTranslatorVersion);
            for (const auto& v : opt.include_dirs) append(v);
            for (const auto& v : opt.isystem_dirs) append(v);
            for (const auto& v : opt.defines) append(v);
            for (const auto& v : opt.undefines) append(v);
            for (const auto& v : opt.forced_includes) append(v);
            for (const auto& v : opt.imacros) append(v);
            return hex_u64_(hash);
        }

        std::filesystem::path default_cache_root_(const CachedHeaderImportOptions& opt) {
            if (!opt.cache_root.empty()) return std::filesystem::path(opt.cache_root);
            const std::filesystem::path importer(opt.importer_source_path);
            return importer.parent_path() / ".parus-cache" / "cimport";
        }

        void normalize_paths_(std::vector<std::string>& values) {
            for (auto& value : values) {
                if (!value.empty()) value = parus::normalize_path(value);
            }
        }

    } // namespace

    CachedHeaderImportResult load_or_translate_c_header_cached(const CachedHeaderImportOptions& raw_opt) {
        CachedHeaderImportResult out{};
        CachedHeaderImportOptions opt = raw_opt;
        if (!opt.importer_source_path.empty()) {
            opt.importer_source_path = parus::normalize_path(opt.importer_source_path);
        }
        if (!opt.header_path.empty()) {
            opt.header_path = parus::normalize_path(opt.header_path);
        }
        if (!opt.sysroot_path.empty()) {
            opt.sysroot_path = parus::normalize_path(opt.sysroot_path);
        }
        if (!opt.apple_sdk_root.empty()) {
            opt.apple_sdk_root = parus::normalize_path(opt.apple_sdk_root);
        }
        normalize_paths_(opt.include_dirs);
        normalize_paths_(opt.isystem_dirs);
        normalize_paths_(opt.forced_includes);
        normalize_paths_(opt.imacros);

        const auto probe = probe_libclang();
        const std::string libclang_version = probe.version;
        const std::filesystem::path cache_root = default_cache_root_(opt);
        const std::string key = compute_cache_key_(opt, libclang_version);
        out.manifest_path = (cache_root / (key + ".bin")).string();

        HeaderImportResult cached{};
        std::string cached_translator_version{};
        std::string cached_libclang_version{};
        if (read_manifest_(out.manifest_path, cached, cached_translator_version, cached_libclang_version) &&
            cached_translator_version == kTranslatorVersion &&
            cached_libclang_version == libclang_version &&
            deps_are_fresh_(cached.dependency_files)) {
            out.import = std::move(cached);
            out.import.libclang_version = libclang_version;
            out.from_cache = true;
            return out;
        }

        out.import = import_c_header_functions(
            opt.importer_source_path,
            opt.header_path,
            opt.target_triple,
            opt.sysroot_path,
            opt.apple_sdk_root,
            opt.include_dirs,
            opt.isystem_dirs,
            opt.defines,
            opt.undefines,
            opt.forced_includes,
            opt.imacros);
        out.import.libclang_version = libclang_version;
        for (auto& dep : out.import.dependency_files) {
            if (!dep.path.empty()) dep.path = parus::normalize_path(dep.path);
        }
        if (out.import.error == ImportErrorKind::kNone) {
            (void)write_manifest_(out.manifest_path, opt, out.import, libclang_version);
        }
        return out;
    }

} // namespace parus::cimport
