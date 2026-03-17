#include <parus/cimport/CImportPayload.hpp>

#include <unordered_set>

namespace parus::cimport {
    namespace {

        bool is_ident_start_(char ch) {
            return (ch == '_') ||
                   (ch >= 'a' && ch <= 'z') ||
                   (ch >= 'A' && ch <= 'Z');
        }

        bool is_ident_continue_(char ch) {
            return is_ident_start_(ch) || (ch >= '0' && ch <= '9');
        }

        bool is_builtin_type_token_(std::string_view tok) {
            static const std::unordered_set<std::string> kBuiltins = {
                "void", "bool",
                "char",
                "i8", "i16", "i32", "i64", "i128",
                "u8", "u16", "u32", "u64", "u128",
                "isize", "usize",
                "f32", "f64", "f128",
                "text",
                "null",
                "ptr",
                "mut",
                "def",
            };
            return kBuiltins.find(std::string(tok)) != kBuiltins.end();
        }

    } // namespace

    std::string rewrite_cimport_type_with_alias(
        std::string_view type_repr,
        std::string_view alias,
        const std::unordered_set<std::string>& known_type_names
    ) {
        if (type_repr.empty() || alias.empty() || known_type_names.empty()) {
            return std::string(type_repr);
        }

        std::string out{};
        out.reserve(type_repr.size() + 16u);

        size_t i = 0;
        while (i < type_repr.size()) {
            const char ch = type_repr[i];
            if (!is_ident_start_(ch)) {
                out.push_back(ch);
                ++i;
                continue;
            }

            const size_t begin = i;
            ++i;
            while (i < type_repr.size() && is_ident_continue_(type_repr[i])) ++i;
            const std::string_view tok = type_repr.substr(begin, i - begin);

            const bool has_scope_prefix =
                begin >= 2 && type_repr[begin - 1] == ':' && type_repr[begin - 2] == ':';
            const bool has_scope_suffix =
                i + 1 < type_repr.size() && type_repr[i] == ':' && type_repr[i + 1] == ':';

            if (!has_scope_prefix &&
                !has_scope_suffix &&
                !is_builtin_type_token_(tok) &&
                known_type_names.find(std::string(tok)) != known_type_names.end()) {
                out += std::string(alias);
                out += "::";
                out += std::string(tok);
            } else {
                out += std::string(tok);
            }
        }

        return out;
    }

    std::string make_c_import_payload(
        std::string_view header,
        std::string_view alias,
        const ImportedFunctionDecl& fn
    ) {
        auto format_kind_text = [](CFormatKind k) -> std::string_view {
            switch (k) {
                case CFormatKind::kFmtVarargs: return "fmt_varargs";
                case CFormatKind::kFmtVList: return "fmt_vlist";
                case CFormatKind::kNone:
                default:
                    return "none";
            }
        };
        auto callconv_text = [](CCallConvKind cc) -> std::string_view {
            switch (cc) {
                case CCallConvKind::kCdecl: return "cdecl";
                case CCallConvKind::kStdCall: return "stdcall";
                case CCallConvKind::kFastCall: return "fastcall";
                case CCallConvKind::kVectorCall: return "vectorcall";
                case CCallConvKind::kWin64: return "win64";
                case CCallConvKind::kSysV: return "sysv";
                case CCallConvKind::kDefault:
                default:
                    return "default";
            }
        };

        std::string out = "parus_c_import|header=";
        out += std::string(header);
        out += "|is_c_abi=";
        out += fn.is_c_abi ? "1" : "0";
        out += "|variadic=";
        out += fn.is_variadic ? "1" : "0";
        out += "|format=";
        out += std::string(format_kind_text(fn.format_kind));
        out += "|fmt_idx=";
        out += std::to_string(fn.fmt_param_index);
        out += "|va_idx=";
        out += std::to_string(fn.va_list_param_index);
        out += "|callconv=";
        out += std::string(callconv_text(fn.callconv));
        if (!fn.variadic_sibling_name.empty()) {
            out += "|sibling=";
            out += std::string(alias);
            out += "::";
            out += fn.variadic_sibling_name;
        }
        return out;
    }

    std::string make_c_import_global_payload(
        std::string_view header,
        const ImportedGlobalDecl& gv
    ) {
        auto tls_kind_text = [](ImportedGlobalTlsKind k) -> std::string_view {
            switch (k) {
                case ImportedGlobalTlsKind::kDynamic: return "dynamic";
                case ImportedGlobalTlsKind::kStatic: return "static";
                case ImportedGlobalTlsKind::kNone:
                default:
                    return "none";
            }
        };

        std::string out = "parus_c_import_global|header=";
        out += std::string(header);
        out += "|is_c_abi=";
        out += gv.is_c_abi ? "1" : "0";
        out += "|const=";
        out += gv.is_const ? "1" : "0";
        out += "|volatile=";
        out += gv.is_volatile ? "1" : "0";
        out += "|restrict=";
        out += gv.is_restrict ? "1" : "0";
        out += "|tls=";
        out += std::string(tls_kind_text(gv.tls_kind));
        return out;
    }

    std::string make_c_import_union_payload(
        std::string_view header,
        std::string_view alias,
        const std::unordered_set<std::string>& known_type_names,
        const ImportedUnionDecl& un
    ) {
        std::string payload = "parus_c_import_union|header=" + std::string(header);
        payload += "|size=" + std::to_string(un.size_bytes);
        payload += "|align=" + std::to_string(un.align_bytes);
        payload += "|fields=";
        for (size_t fi = 0; fi < un.fields.size(); ++fi) {
            if (fi) payload += ",";
            payload += un.fields[fi].name;
            payload += ":";
            payload += rewrite_cimport_type_with_alias(
                un.fields[fi].type_repr, alias, known_type_names);
        }
        return payload;
    }

    std::string make_c_import_struct_payload(
        std::string_view header,
        std::string_view alias,
        const std::unordered_set<std::string>& known_type_names,
        const ImportedStructDecl& st
    ) {
        auto dash_if_empty = [](std::string_view s) -> std::string {
            if (s.empty()) return "-";
            return std::string(s);
        };
        std::string payload = "parus_c_import_struct|header=" + std::string(header);
        payload += "|size=" + std::to_string(st.size_bytes);
        payload += "|align=" + std::to_string(st.align_bytes);
        payload += "|packed=" + std::string(st.is_packed ? "1" : "0");
        payload += "|fields=";
        for (size_t fi = 0; fi < st.fields.size(); ++fi) {
            if (fi) payload += ",";
            const auto& f = st.fields[fi];
            payload += f.name;
            payload += ":";
            payload += rewrite_cimport_type_with_alias(
                f.type_repr, alias, known_type_names);
            payload += "@";
            payload += std::to_string(f.offset_bytes);
            payload += "@";
            payload += f.union_origin ? "1" : "0";
            payload += "@";
            payload += std::to_string(f.is_bitfield ? f.bit_offset : 0u);
            payload += "@";
            payload += std::to_string(f.is_bitfield ? f.bit_width : 0u);
            payload += "@";
            payload += f.bit_signed ? "1" : "0";
            payload += "@";
            payload += dash_if_empty(f.bit_getter_name);
            payload += "@";
            payload += dash_if_empty(f.bit_setter_name);
        }
        return payload;
    }

    std::string make_c_import_const_payload(std::string_view kind, std::string_view text) {
        std::string payload = "parus_c_import_const|kind=";
        payload += std::string(kind);
        payload += "|text=";
        payload += std::string(text);
        return payload;
    }

    std::string make_c_import_typedef_payload(
        std::string_view header,
        std::string_view alias,
        const std::unordered_set<std::string>& known_type_names,
        const ImportedTypedefDecl& td
    ) {
        std::string payload = "parus_c_import_typedef|header=" + std::string(header);
        payload += "|transparent=";
        payload += (td.is_transparent && !td.transparent_type_repr.empty()) ? "1" : "0";
        if (td.is_transparent && !td.transparent_type_repr.empty()) {
            payload += "|target=";
            payload += rewrite_cimport_type_with_alias(
                td.transparent_type_repr, alias, known_type_names);
        }
        return payload;
    }

    std::string imported_macro_skip_code_text(ImportedMacroSkipKind kind) {
        switch (kind) {
            case ImportedMacroSkipKind::kNone: return "none";
            case ImportedMacroSkipKind::kSyntaxUnsupported: return "syntax";
            case ImportedMacroSkipKind::kVariadicExcluded: return "variadic";
            case ImportedMacroSkipKind::kTokenPasteExcluded: return "token_paste";
            case ImportedMacroSkipKind::kStringizeExcluded: return "stringize";
            case ImportedMacroSkipKind::kStatementOrExtensionExcluded: return "statement_or_extension";
            case ImportedMacroSkipKind::kInvalidForwarding: return "invalid_forwarding";
            case ImportedMacroSkipKind::kParamMismatch: return "param_mismatch";
            case ImportedMacroSkipKind::kUnresolvedCallee: return "unresolved_callee";
            case ImportedMacroSkipKind::kUnresolvableChain: return "unresolvable_chain";
            case ImportedMacroSkipKind::kChainCycleDetected: return "chain_cycle";
            case ImportedMacroSkipKind::kConstExprUnsupported: return "const_expr";
            default: return "unknown";
        }
    }

} // namespace parus::cimport
