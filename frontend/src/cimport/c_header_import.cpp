#include <parus/cimport/CHeaderImport.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(PARUS_HAS_LIBCLANG) && PARUS_HAS_LIBCLANG
#include <clang-c/Index.h>
#endif

namespace parus::cimport {

#if defined(PARUS_HAS_LIBCLANG) && PARUS_HAS_LIBCLANG
    namespace {

        struct CollectedFnDecl {
            ImportedFunctionDecl api{};
            std::vector<std::string> arg_type_reprs{};
            std::string ret_type_repr{};
        };

        static std::string to_std_string_(CXString s) {
            const char* c = clang_getCString(s);
            std::string out = (c != nullptr) ? std::string(c) : std::string{};
            clang_disposeString(s);
            return out;
        }

        static std::string escape_include_text_(std::string_view s) {
            std::string out;
            out.reserve(s.size() + 8);
            for (const char ch : s) {
                if (ch == '\\' || ch == '"') {
                    out.push_back('\\');
                }
                out.push_back(ch);
            }
            return out;
        }

        static bool map_c_integer_by_size_(CXType t, bool is_unsigned, std::string& out) {
            const long long size = clang_Type_getSizeOf(t);
            if (size <= 0) return false;
            switch (size) {
                case 1: out = is_unsigned ? "u8" : "i8"; return true;
                case 2: out = is_unsigned ? "u16" : "i16"; return true;
                case 4: out = is_unsigned ? "u32" : "i32"; return true;
                case 8: out = is_unsigned ? "u64" : "i64"; return true;
                default: return false;
            }
        }

        static bool is_char_like_type_(CXType t) {
            const CXType ct = clang_getCanonicalType(t);
            switch (ct.kind) {
                case CXType_Char_U:
                case CXType_UChar:
                case CXType_Char_S:
                case CXType_SChar:
                case CXType_Char16:
                case CXType_Char32:
                case CXType_WChar:
                    return true;
                default:
                    return false;
            }
        }

        static bool is_c_string_param_(CXType t) {
            const CXType ct = clang_getCanonicalType(t);
            if (ct.kind != CXType_Pointer) return false;
            const CXType elem = clang_getPointeeType(ct);
            if (elem.kind == CXType_Invalid) return false;
            return is_char_like_type_(elem);
        }

        static bool spelling_contains_(std::string_view spelling, std::string_view needle) {
            return !needle.empty() && spelling.find(needle) != std::string_view::npos;
        }

        static bool is_va_list_type_(CXType t) {
            auto has_va_token = [](CXType ty) -> bool {
                const std::string s = to_std_string_(clang_getTypeSpelling(ty));
                return spelling_contains_(s, "va_list") || spelling_contains_(s, "__builtin_va_list");
            };

            if (has_va_token(t)) return true;

            const CXType canonical = clang_getCanonicalType(t);
            if (has_va_token(canonical)) return true;

            const CXCursor decl = clang_getTypeDeclaration(t);
            if (!clang_Cursor_isNull(decl)) {
                const std::string dn = to_std_string_(clang_getCursorSpelling(decl));
                if (spelling_contains_(dn, "va_list") || spelling_contains_(dn, "__builtin_va_list")) {
                    return true;
                }
                if (clang_getCursorKind(decl) == CXCursor_TypedefDecl) {
                    const CXType under = clang_getTypedefDeclUnderlyingType(decl);
                    if (under.kind != CXType_Invalid) {
                        if (has_va_token(under)) return true;
                        if (has_va_token(clang_getCanonicalType(under))) return true;
                    }
                }
            }

            if (canonical.kind == CXType_Pointer) {
                const CXType elem = clang_getPointeeType(canonical);
                if (elem.kind != CXType_Invalid) {
                    if (has_va_token(elem)) return true;
                    if (has_va_token(clang_getCanonicalType(elem))) return true;
                }
            }

            return false;
        }

        static bool map_c_type_to_parus_(CXType t, std::string& out, uint32_t depth = 0);

        static bool map_c_fn_type_to_parus_(CXType fn_ty, std::string& out, uint32_t depth) {
            if (depth > 8) return false;

            const CXType ct = clang_getCanonicalType(fn_ty);
            if (ct.kind != CXType_FunctionProto && ct.kind != CXType_FunctionNoProto) {
                return false;
            }

            const int arg_n = clang_getNumArgTypes(ct);
            if (arg_n < 0) return false;
            if (clang_isFunctionTypeVariadic(ct) != 0) return false;

            std::string ret_repr{};
            if (!map_c_type_to_parus_(clang_getResultType(ct), ret_repr, depth + 1)) {
                return false;
            }

            std::string sig = "def(";
            for (int i = 0; i < arg_n; ++i) {
                std::string arg_repr{};
                if (!map_c_type_to_parus_(clang_getArgType(ct, i), arg_repr, depth + 1)) {
                    return false;
                }
                if (i > 0) sig += ", ";
                sig += arg_repr;
            }
            sig += ") -> ";
            sig += ret_repr;
            out = std::move(sig);
            return true;
        }

        static bool map_c_type_to_parus_(CXType t, std::string& out, uint32_t depth) {
            if (depth > 8) return false;

            if (t.kind == CXType_Typedef) {
                const CXCursor td = clang_getTypeDeclaration(t);
                const std::string td_name = to_std_string_(clang_getCursorSpelling(td));
                if (!td_name.empty()) {
                    out = td_name;
                    return true;
                }
            }

            const CXType ct = clang_getCanonicalType(t);
            switch (ct.kind) {
                case CXType_Void:
                    out = "void";
                    return true;
                case CXType_Bool:
                    out = "bool";
                    return true;
                case CXType_Char_U:
                case CXType_UChar:
                    out = "u8";
                    return true;
                case CXType_Char_S:
                case CXType_SChar:
                    out = "i8";
                    return true;
                case CXType_UShort:
                    out = "u16";
                    return true;
                case CXType_UInt:
                    out = "u32";
                    return true;
                case CXType_ULong:
                    return map_c_integer_by_size_(ct, /*is_unsigned=*/true, out);
                case CXType_ULongLong:
                    out = "u64";
                    return true;
                case CXType_UInt128:
                    out = "u128";
                    return true;
                case CXType_Short:
                    out = "i16";
                    return true;
                case CXType_Int:
                    out = "i32";
                    return true;
                case CXType_Long:
                    return map_c_integer_by_size_(ct, /*is_unsigned=*/false, out);
                case CXType_LongLong:
                    out = "i64";
                    return true;
                case CXType_Int128:
                    out = "i128";
                    return true;
                case CXType_Float:
                    out = "f32";
                    return true;
                case CXType_Double:
                    out = "f64";
                    return true;
                case CXType_LongDouble:
                    out = "f64";
                    return true;
                case CXType_FunctionProto:
                case CXType_FunctionNoProto:
                    return map_c_fn_type_to_parus_(ct, out, depth + 1);
                case CXType_Enum: {
                    const CXCursor decl = clang_getTypeDeclaration(ct);
                    if (!clang_Cursor_isNull(decl)) {
                        const std::string name = to_std_string_(clang_getCursorSpelling(decl));
                        if (!name.empty()) {
                            out = name;
                            return true;
                        }
                        const CXType int_ty = clang_getEnumDeclIntegerType(decl);
                        if (int_ty.kind != CXType_Invalid) {
                            return map_c_type_to_parus_(int_ty, out, depth + 1);
                        }
                    }
                    return false;
                }
                case CXType_Pointer: {
                    const CXType elem = clang_getPointeeType(ct);
                    if (elem.kind == CXType_Invalid) return false;

                    std::string elem_repr{};
                    if (elem.kind == CXType_Void) {
                        elem_repr = "u8";
                    } else if (elem.kind == CXType_FunctionProto || elem.kind == CXType_FunctionNoProto) {
                        if (!map_c_fn_type_to_parus_(elem, elem_repr, depth + 1)) {
                            return false;
                        }
                    } else if (!map_c_type_to_parus_(elem, elem_repr, depth + 1)) {
                        return false;
                    }

                    const bool pointee_const = clang_isConstQualifiedType(elem) != 0;
                    out = pointee_const ? ("ptr " + elem_repr) : ("ptr mut " + elem_repr);
                    return true;
                }
                case CXType_Elaborated:
                case CXType_Record: {
                    const CXCursor decl = clang_getTypeDeclaration(ct);
                    if (clang_Cursor_isNull(decl) != 0) return false;
                    const CXCursorKind dk = clang_getCursorKind(decl);
                    if (dk != CXCursor_UnionDecl && dk != CXCursor_StructDecl) return false;
                    const std::string name = to_std_string_(clang_getCursorSpelling(decl));
                    if (name.empty()) return false;
                    out = name;
                    return true;
                }
                default:
                    return false;
            }
        }

        static std::string format_family_key_(std::string_view name) {
            if (name.size() >= 2 && name.front() == 'v') {
                return std::string(name.substr(1));
            }
            return std::string(name);
        }

        struct ImportCollectCtx {
            std::unordered_set<std::string> fn_dedup{};
            std::unordered_set<std::string> union_dedup{};
            std::unordered_set<std::string> struct_dedup{};
            std::unordered_set<std::string> enum_dedup{};
            std::unordered_set<std::string> typedef_dedup{};
            std::vector<CollectedFnDecl>* out_fns = nullptr;
            std::vector<ImportedUnionDecl>* out_unions = nullptr;
            std::vector<ImportedStructDecl>* out_structs = nullptr;
            std::vector<ImportedEnumDecl>* out_enums = nullptr;
            std::vector<ImportedTypedefDecl>* out_typedefs = nullptr;
        };

        static bool collect_union_decl_(CXCursor c, ImportCollectCtx& ctx) {
            if (clang_getCursorKind(c) != CXCursor_UnionDecl) return false;
            if (clang_isCursorDefinition(c) == 0) return true;

            const std::string name = to_std_string_(clang_getCursorSpelling(c));
            if (name.empty()) return true;
            if (!ctx.union_dedup.insert(name).second) return true;

            ImportedUnionDecl u{};
            u.name = name;

            const CXType union_ty = clang_getCursorType(c);
            const long long sz = clang_Type_getSizeOf(union_ty);
            const long long al = clang_Type_getAlignOf(union_ty);
            if (sz > 0) u.size_bytes = static_cast<uint32_t>(sz);
            if (al > 0) u.align_bytes = static_cast<uint32_t>(al);

            clang_visitChildren(
                c,
                [](CXCursor child, CXCursor, CXClientData data) -> CXChildVisitResult {
                    auto* out = static_cast<ImportedUnionDecl*>(data);
                    if (out == nullptr) return CXChildVisit_Continue;
                    if (clang_getCursorKind(child) != CXCursor_FieldDecl) {
                        return CXChildVisit_Continue;
                    }

                    const std::string field_name = to_std_string_(clang_getCursorSpelling(child));
                    if (field_name.empty()) return CXChildVisit_Continue;

                    std::string field_ty{};
                    if (!map_c_type_to_parus_(clang_getCursorType(child), field_ty)) {
                        return CXChildVisit_Continue;
                    }

                    ImportedUnionFieldDecl fd{};
                    fd.name = field_name;
                    fd.type_repr = std::move(field_ty);
                    out->fields.push_back(std::move(fd));
                    return CXChildVisit_Continue;
                },
                &u
            );

            if (ctx.out_unions != nullptr) {
                ctx.out_unions->push_back(std::move(u));
            }
            return true;
        }

        static bool collect_struct_decl_(CXCursor c, ImportCollectCtx& ctx) {
            if (clang_getCursorKind(c) != CXCursor_StructDecl) return false;
            if (clang_isCursorDefinition(c) == 0) return true;

            const std::string name = to_std_string_(clang_getCursorSpelling(c));
            if (name.empty()) return true;
            if (!ctx.struct_dedup.insert(name).second) return true;

            ImportedStructDecl s{};
            s.name = name;

            const CXType struct_ty = clang_getCursorType(c);
            const long long sz = clang_Type_getSizeOf(struct_ty);
            const long long al = clang_Type_getAlignOf(struct_ty);
            if (sz > 0) s.size_bytes = static_cast<uint32_t>(sz);
            if (al > 0) s.align_bytes = static_cast<uint32_t>(al);

            clang_visitChildren(
                c,
                [](CXCursor child, CXCursor, CXClientData data) -> CXChildVisitResult {
                    auto* out = static_cast<ImportedStructDecl*>(data);
                    if (out == nullptr) return CXChildVisit_Continue;
                    if (clang_getCursorKind(child) != CXCursor_FieldDecl) {
                        return CXChildVisit_Continue;
                    }
                    if (clang_Cursor_isBitField(child) != 0) {
                        return CXChildVisit_Continue;
                    }

                    const std::string field_name = to_std_string_(clang_getCursorSpelling(child));
                    if (field_name.empty()) return CXChildVisit_Continue;

                    std::string field_ty{};
                    if (!map_c_type_to_parus_(clang_getCursorType(child), field_ty)) {
                        return CXChildVisit_Continue;
                    }

                    const long long off_bits = clang_Cursor_getOffsetOfField(child);
                    if (off_bits < 0) return CXChildVisit_Continue;

                    ImportedStructFieldDecl fd{};
                    fd.name = field_name;
                    fd.type_repr = std::move(field_ty);
                    fd.offset_bytes = static_cast<uint32_t>(off_bits / 8);
                    out->fields.push_back(std::move(fd));
                    return CXChildVisit_Continue;
                },
                &s
            );

            if (ctx.out_structs != nullptr) {
                ctx.out_structs->push_back(std::move(s));
            }
            return true;
        }

        static bool collect_enum_decl_(CXCursor c, ImportCollectCtx& ctx) {
            if (clang_getCursorKind(c) != CXCursor_EnumDecl) return false;
            if (clang_isCursorDefinition(c) == 0) return true;

            const std::string name = to_std_string_(clang_getCursorSpelling(c));
            if (name.empty()) return true;
            if (!ctx.enum_dedup.insert(name).second) return true;

            ImportedEnumDecl e{};
            e.name = name;
            if (!map_c_type_to_parus_(clang_getEnumDeclIntegerType(c), e.underlying_type_repr)) {
                e.underlying_type_repr = "i32";
            }

            clang_visitChildren(
                c,
                [](CXCursor child, CXCursor, CXClientData data) -> CXChildVisitResult {
                    auto* out = static_cast<ImportedEnumDecl*>(data);
                    if (out == nullptr) return CXChildVisit_Continue;
                    if (clang_getCursorKind(child) != CXCursor_EnumConstantDecl) {
                        return CXChildVisit_Continue;
                    }

                    const std::string constant_name = to_std_string_(clang_getCursorSpelling(child));
                    if (constant_name.empty()) return CXChildVisit_Continue;

                    ImportedEnumConstantDecl one{};
                    one.name = constant_name;
                    one.value_text = std::to_string(clang_getEnumConstantDeclValue(child));
                    out->constants.push_back(std::move(one));
                    return CXChildVisit_Continue;
                },
                &e
            );

            if (ctx.out_enums != nullptr) {
                ctx.out_enums->push_back(std::move(e));
            }
            return true;
        }

        static bool collect_typedef_decl_(CXCursor c, ImportCollectCtx& ctx) {
            if (clang_getCursorKind(c) != CXCursor_TypedefDecl) return false;

            const std::string name = to_std_string_(clang_getCursorSpelling(c));
            if (name.empty()) return true;
            if (!ctx.typedef_dedup.insert(name).second) return true;

            std::string type_repr{};
            if (!map_c_type_to_parus_(clang_getTypedefDeclUnderlyingType(c), type_repr)) {
                return true;
            }

            ImportedTypedefDecl td{};
            td.name = name;
            td.type_repr = std::move(type_repr);
            if (ctx.out_typedefs != nullptr) {
                ctx.out_typedefs->push_back(std::move(td));
            }
            return true;
        }

        static bool collect_function_decl_(CXCursor c, ImportCollectCtx& ctx) {
            if (clang_getCursorKind(c) != CXCursor_FunctionDecl) return false;

            const CXLinkageKind linkage = clang_getCursorLinkage(c);
            if (linkage == CXLinkage_Internal || linkage == CXLinkage_NoLinkage) {
                return true;
            }

            const std::string name = to_std_string_(clang_getCursorSpelling(c));
            if (name.empty()) return true;

            const CXType fn_ty = clang_getCanonicalType(clang_getCursorType(c));
            if (fn_ty.kind != CXType_FunctionProto && fn_ty.kind != CXType_FunctionNoProto) {
                return true;
            }

            const int arg_n = clang_getNumArgTypes(fn_ty);
            if (arg_n < 0) {
                return true;
            }

            std::string ret_repr{};
            if (!map_c_type_to_parus_(clang_getResultType(fn_ty), ret_repr)) {
                return true;
            }

            std::vector<std::string> arg_repr{};
            arg_repr.reserve(static_cast<size_t>(arg_n));
            std::vector<CXType> arg_types{};
            arg_types.reserve(static_cast<size_t>(arg_n));
            for (int i = 0; i < arg_n; ++i) {
                const CXType arg_ty = clang_getArgType(fn_ty, i);
                std::string one{};
                if (!map_c_type_to_parus_(arg_ty, one)) {
                    return true;
                }
                arg_types.push_back(arg_ty);
                arg_repr.push_back(std::move(one));
            }

            std::string type_repr = "def(";
            for (size_t i = 0; i < arg_repr.size(); ++i) {
                if (i) type_repr += ", ";
                type_repr += arg_repr[i];
            }
            type_repr += ") -> ";
            type_repr += ret_repr;

            const bool variadic = clang_isFunctionTypeVariadic(fn_ty) != 0;
            const std::string dedup_key = name + "|" + type_repr + "|" + (variadic ? "1" : "0");
            if (!ctx.fn_dedup.insert(dedup_key).second) {
                return true;
            }

            CollectedFnDecl fn{};
            fn.api.name = name;
            fn.api.link_name = name;
            fn.api.type_repr = std::move(type_repr);
            fn.api.is_c_abi = true;
            fn.api.is_variadic = variadic;
            fn.arg_type_reprs = arg_repr;
            fn.ret_type_repr = ret_repr;

            if (!arg_types.empty() && is_c_string_param_(arg_types[0])) {
                if (variadic) {
                    fn.api.format_kind = CFormatKind::kFmtVarargs;
                    fn.api.fmt_param_index = 0;
                } else {
                    for (int i = 1; i < arg_n; ++i) {
                        if (is_va_list_type_(arg_types[static_cast<size_t>(i)])) {
                            fn.api.format_kind = CFormatKind::kFmtVList;
                            fn.api.fmt_param_index = 0;
                            fn.api.va_list_param_index = i;
                            break;
                        }
                    }
                }
            }

            if (ctx.out_fns != nullptr) {
                ctx.out_fns->push_back(std::move(fn));
            }
            return true;
        }

        static CXChildVisitResult collect_decl_visitor_(CXCursor c, CXCursor, CXClientData client_data) {
            auto* ctx = static_cast<ImportCollectCtx*>(client_data);
            if (ctx == nullptr) return CXChildVisit_Continue;

            (void)collect_union_decl_(c, *ctx);
            (void)collect_struct_decl_(c, *ctx);
            (void)collect_enum_decl_(c, *ctx);
            (void)collect_typedef_decl_(c, *ctx);
            (void)collect_function_decl_(c, *ctx);
            return CXChildVisit_Recurse;
        }

    } // namespace
#endif

    HeaderImportResult import_c_header_functions(
        const std::string& importer_source_path,
        const std::string& header_path,
        const std::vector<std::string>& include_dirs,
        const std::vector<std::string>& isystem_dirs
    ) {
        HeaderImportResult out{};

#if !defined(PARUS_HAS_LIBCLANG) || !PARUS_HAS_LIBCLANG
        (void)importer_source_path;
        (void)header_path;
        (void)include_dirs;
        (void)isystem_dirs;
        out.error = ImportErrorKind::kLibClangUnavailable;
        out.error_text = "libclang unavailable";
        return out;
#else
        const std::string escaped = escape_include_text_(header_path);
        const std::string source = "#include \"" + escaped + "\"\n";
        const std::string tu_name = importer_source_path + ".parus_cimport.c";

        std::vector<std::string> args_storage{};
        args_storage.reserve(8 + include_dirs.size() + isystem_dirs.size() * 2u);
        args_storage.emplace_back("-x");
        args_storage.emplace_back("c");
        args_storage.emplace_back("-std=c17");
        args_storage.emplace_back("-fsyntax-only");
        for (const auto& dir : include_dirs) {
            if (dir.empty()) continue;
            args_storage.emplace_back("-I" + dir);
        }
        for (const auto& dir : isystem_dirs) {
            if (dir.empty()) continue;
            args_storage.emplace_back("-isystem");
            args_storage.emplace_back(dir);
        }

        std::vector<const char*> args{};
        args.reserve(args_storage.size());
        for (const auto& s : args_storage) {
            args.push_back(s.c_str());
        }

        CXUnsavedFile unsaved{};
        unsaved.Filename = tu_name.c_str();
        unsaved.Contents = source.c_str();
        unsaved.Length = source.size();

        CXIndex idx = clang_createIndex(/*excludeDeclarationsFromPCH=*/0, /*displayDiagnostics=*/0);
        if (idx == nullptr) {
            out.error = ImportErrorKind::kParseFailed;
            out.error_text = "failed to create libclang index";
            return out;
        }

        CXTranslationUnit tu = nullptr;
        const CXErrorCode ec = clang_parseTranslationUnit2(
            idx,
            tu_name.c_str(),
            args.empty() ? nullptr : args.data(),
            static_cast<int>(args.size()),
            &unsaved,
            1,
            CXTranslationUnit_None,
            &tu
        );
        if (ec != CXError_Success || tu == nullptr) {
            out.error = ImportErrorKind::kParseFailed;
            out.error_text = "failed to parse C header '" + header_path + "'";
            clang_disposeIndex(idx);
            return out;
        }

        for (unsigned i = 0; i < clang_getNumDiagnostics(tu); ++i) {
            CXDiagnostic d = clang_getDiagnostic(tu, i);
            const CXDiagnosticSeverity sev = clang_getDiagnosticSeverity(d);
            if (sev == CXDiagnostic_Error || sev == CXDiagnostic_Fatal) {
                out.error = ImportErrorKind::kParseFailed;
                out.error_text = to_std_string_(clang_formatDiagnostic(d, clang_defaultDiagnosticDisplayOptions()));
                clang_disposeDiagnostic(d);
                clang_disposeTranslationUnit(tu);
                clang_disposeIndex(idx);
                return out;
            }
            clang_disposeDiagnostic(d);
        }

        std::vector<CollectedFnDecl> collected_fns{};
        ImportCollectCtx ctx{};
        ctx.out_fns = &collected_fns;
        ctx.out_unions = &out.unions;
        ctx.out_structs = &out.structs;
        ctx.out_enums = &out.enums;
        ctx.out_typedefs = &out.typedefs;

        const CXCursor root = clang_getTranslationUnitCursor(tu);
        clang_visitChildren(root, collect_decl_visitor_, &ctx);

        // Resolve `fmt_vlist` -> variadic sibling mapping without function-name hardcoding.
        for (auto& one : collected_fns) {
            if (one.api.format_kind != CFormatKind::kFmtVList) continue;
            if (one.api.va_list_param_index < 0) continue;
            const size_t va_idx = static_cast<size_t>(one.api.va_list_param_index);
            if (va_idx >= one.arg_type_reprs.size()) continue;

            std::vector<std::string> fixed_args{};
            fixed_args.reserve(one.arg_type_reprs.size() - 1u);
            for (size_t i = 0; i < one.arg_type_reprs.size(); ++i) {
                if (i == va_idx) continue;
                fixed_args.push_back(one.arg_type_reprs[i]);
            }

            const std::string family = format_family_key_(one.api.name);
            for (const auto& cand : collected_fns) {
                if (!cand.api.is_variadic) continue;
                if (cand.ret_type_repr != one.ret_type_repr) continue;
                if (cand.arg_type_reprs != fixed_args) continue;
                if (format_family_key_(cand.api.name) != family) continue;
                one.api.variadic_sibling_name = cand.api.name;
                break;
            }
        }

        out.functions.reserve(collected_fns.size());
        for (auto& f : collected_fns) {
            out.functions.push_back(std::move(f.api));
        }

        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(idx);
        out.error = ImportErrorKind::kNone;
        return out;
#endif
    }

} // namespace parus::cimport
