#include <parus/cimport/CHeaderImport.hpp>

#include <algorithm>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#if defined(PARUS_HAS_LIBCLANG) && PARUS_HAS_LIBCLANG
#include <clang-c/Index.h>
#endif

namespace parus::cimport {

#if defined(PARUS_HAS_LIBCLANG) && PARUS_HAS_LIBCLANG
    namespace {

        struct ImportCollectCtx;

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

        static std::string cursor_identity_key_(CXCursor c) {
            const std::string usr = to_std_string_(clang_getCursorUSR(c));
            if (!usr.empty()) return std::string("usr:") + usr;
            return std::string("hash:") + std::to_string(clang_hashCursor(c));
        }

        static bool parse_macro_int_literal_text_(std::string_view text, std::string& out) {
            auto trim = [](std::string_view s) {
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
                return s;
            };
            text = trim(text);
            if (text.empty()) return false;

            bool neg = false;
            if (text.front() == '+' || text.front() == '-') {
                neg = (text.front() == '-');
                text.remove_prefix(1);
            }
            if (text.empty()) return false;

            while (!text.empty()) {
                const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(text.back())));
                if (c == 'u' || c == 'l') {
                    text.remove_suffix(1);
                    continue;
                }
                break;
            }
            text = trim(text);
            if (text.empty()) return false;

            const bool is_hex = text.size() > 2 &&
                                text[0] == '0' &&
                                (text[1] == 'x' || text[1] == 'X');
            for (char c : text) {
                if (c == '\'') continue;
                if (is_hex) {
                    const bool ok = std::isdigit(static_cast<unsigned char>(c)) ||
                                    (c >= 'a' && c <= 'f') ||
                                    (c >= 'A' && c <= 'F') ||
                                    c == 'x' || c == 'X';
                    if (!ok) return false;
                } else if (!std::isdigit(static_cast<unsigned char>(c))) {
                    return false;
                }
            }

            out.clear();
            if (neg) out.push_back('-');
            out.append(text);
            return true;
        }

        static bool parse_macro_float_literal_text_(std::string_view text, std::string& out) {
            auto trim = [](std::string_view s) {
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
                return s;
            };
            text = trim(text);
            if (text.empty()) return false;

            bool neg = false;
            if (text.front() == '+' || text.front() == '-') {
                neg = (text.front() == '-');
                text.remove_prefix(1);
            }
            if (text.empty()) return false;

            while (!text.empty()) {
                const char c = static_cast<char>(std::tolower(static_cast<unsigned char>(text.back())));
                if (c == 'f' || c == 'l') {
                    text.remove_suffix(1);
                    continue;
                }
                break;
            }
            text = trim(text);
            if (text.empty()) return false;

            bool has_dot = false;
            bool has_exp = false;
            for (char c : text) {
                if (c == '.') {
                    has_dot = true;
                    continue;
                }
                if (c == 'e' || c == 'E' || c == 'p' || c == 'P') {
                    has_exp = true;
                    continue;
                }
            }
            if (!has_dot && !has_exp) return false;

            out.clear();
            if (neg) out.push_back('-');
            out.append(text);
            return true;
        }

        static bool parse_macro_char_literal_text_(std::string_view text, std::string& out) {
            if (text.size() < 3) return false;
            const size_t quote = text.find('\'');
            if (quote == std::string_view::npos) return false;
            const size_t last_quote = text.rfind('\'');
            if (last_quote == std::string_view::npos || last_quote <= quote) return false;
            out.assign(text.substr(quote, last_quote - quote + 1));
            return true;
        }

        static bool parse_macro_string_literal_text_(std::string_view text, std::string& out) {
            auto trim = [](std::string_view s) {
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
                return s;
            };
            text = trim(text);
            if (text.empty()) return false;
            if (text.front() == 'L' || text.front() == 'u' || text.front() == 'U') {
                text.remove_prefix(1);
            } else if (text.size() >= 2 && text.substr(0, 2) == "u8") {
                text.remove_prefix(2);
            }
            text = trim(text);
            if (text.size() < 2 || text.front() != '"' || text.back() != '"') return false;
            out.assign(text);
            return true;
        }

        static bool try_parse_macro_const_value_(
            std::string_view text,
            ImportedConstKind& out_kind,
            std::string& out_text
        ) {
            auto trim = [](std::string_view s) {
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) s.remove_prefix(1);
                while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) s.remove_suffix(1);
                return s;
            };

            text = trim(text);
            while (text.size() >= 2 && text.front() == '(' && text.back() == ')') {
                text.remove_prefix(1);
                text.remove_suffix(1);
                text = trim(text);
            }

            if (text == "true" || text == "false") {
                out_kind = ImportedConstKind::kBool;
                out_text = (text == "true") ? "1" : "0";
                return true;
            }
            if (parse_macro_char_literal_text_(text, out_text)) {
                out_kind = ImportedConstKind::kChar;
                return true;
            }
            if (parse_macro_string_literal_text_(text, out_text)) {
                out_kind = ImportedConstKind::kString;
                return true;
            }
            if (parse_macro_float_literal_text_(text, out_text)) {
                out_kind = ImportedConstKind::kFloat;
                return true;
            }
            if (parse_macro_int_literal_text_(text, out_text)) {
                out_kind = ImportedConstKind::kInt;
                return true;
            }

            out_kind = ImportedConstKind::kNone;
            out_text.clear();
            return false;
        }

        static bool is_ident_token_(std::string_view tok) {
            if (tok.empty()) return false;
            const auto is_start = [](unsigned char ch) {
                return ch == '_' || (ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z');
            };
            const auto is_continue = [&](unsigned char ch) {
                return is_start(ch) || (ch >= '0' && ch <= '9');
            };
            if (!is_start(static_cast<unsigned char>(tok.front()))) return false;
            for (size_t i = 1; i < tok.size(); ++i) {
                if (!is_continue(static_cast<unsigned char>(tok[i]))) return false;
            }
            return true;
        }

        static std::string join_tokens_with_space_(
            const std::vector<std::string>& toks,
            size_t begin,
            size_t end
        ) {
            std::string out{};
            for (size_t i = begin; i < end && i < toks.size(); ++i) {
                if (!out.empty()) out.push_back(' ');
                out += toks[i];
            }
            return out;
        }

        struct ParsedMacroCallArg {
            int32_t param_index = -1;
            std::string cast_prefix{};
        };

        struct ParsedMacroCall {
            std::string callee{};
            std::vector<ParsedMacroCallArg> args{};
            std::string reason{};
        };

        static bool parse_macro_function_like_signature_(
            const std::vector<std::string>& def_toks,
            size_t& cursor,
            ImportedMacroDecl& out
        ) {
            if (cursor >= def_toks.size() || def_toks[cursor] != "(") {
                out.skip_reason = "unsupported function-like macro: missing parameter list";
                return false;
            }
            ++cursor; // skip '('

            while (cursor < def_toks.size()) {
                const auto& tok = def_toks[cursor];
                if (tok == ")") {
                    ++cursor;
                    return true;
                }
                if (tok == ",") {
                    ++cursor;
                    continue;
                }
                if (tok == "...") {
                    out.is_variadic = true;
                    out.skip_reason = "unsupported function-like macro: variadic parameter list";
                    return false;
                }
                if (!is_ident_token_(tok)) {
                    out.skip_reason = "unsupported function-like macro: invalid parameter token";
                    return false;
                }
                out.params.push_back(tok);
                ++cursor;
            }

            out.skip_reason = "unsupported function-like macro: unterminated parameter list";
            return false;
        }

        static bool parse_macro_replacement_call_(
            const std::vector<std::string>& repl_toks,
            const ImportedMacroDecl& macro,
            ParsedMacroCall& out
        ) {
            if (repl_toks.empty()) {
                out.reason = "unsupported function-like macro: empty replacement";
                return false;
            }

            for (const auto& t : repl_toks) {
                if (t == "##") {
                    out.reason = "unsupported function-like macro: token-paste (##) is excluded";
                    return false;
                }
                if (t == "#") {
                    out.reason = "unsupported function-like macro: stringize (#) is excluded";
                    return false;
                }
                if (t == "{" || t == "}" || t == ";" || t == "__extension__") {
                    out.reason = "unsupported function-like macro: statement/extension form is excluded";
                    return false;
                }
            }

            if (repl_toks.size() < 3 || !is_ident_token_(repl_toks[0]) || repl_toks[1] != "(") {
                out.reason = "unsupported function-like macro: replacement must be a single function call";
                return false;
            }

            int depth = 0;
            size_t close_idx = std::string::npos;
            for (size_t i = 1; i < repl_toks.size(); ++i) {
                if (repl_toks[i] == "(") {
                    ++depth;
                } else if (repl_toks[i] == ")") {
                    --depth;
                    if (depth == 0) {
                        close_idx = i;
                        break;
                    }
                    if (depth < 0) break;
                }
            }
            if (close_idx == std::string::npos || close_idx + 1 != repl_toks.size()) {
                out.reason = "unsupported function-like macro: replacement must be one call expression";
                return false;
            }

            out.callee = repl_toks[0];
            std::unordered_map<std::string, int32_t> param_index{};
            param_index.reserve(macro.params.size());
            for (size_t i = 0; i < macro.params.size(); ++i) {
                param_index.emplace(macro.params[i], static_cast<int32_t>(i));
            }

            int inner_depth = 0;
            size_t arg_begin = 2;
            auto parse_one_arg = [&](size_t b, size_t e) -> bool {
                if (b >= e) {
                    out.reason = "unsupported function-like macro: empty call argument";
                    return false;
                }
                // trim trivial spaces/tokens (token stream normally has none)
                while (b < e && repl_toks[b].empty()) ++b;
                while (e > b && repl_toks[e - 1].empty()) --e;
                if (b >= e) {
                    out.reason = "unsupported function-like macro: empty call argument";
                    return false;
                }

                ParsedMacroCallArg arg{};
                if ((e - b) == 1u) {
                    const auto it = param_index.find(repl_toks[b]);
                    if (it == param_index.end()) {
                        out.reason = "unsupported function-like macro: call arg must be macro parameter";
                        return false;
                    }
                    arg.param_index = it->second;
                    out.args.push_back(std::move(arg));
                    return true;
                }

                // simple cast forwarding: ( <type tokens> ) <param>
                if (repl_toks[b] != "(") {
                    out.reason = "unsupported function-like macro: only direct or simple-cast forwarding is allowed";
                    return false;
                }
                int cast_depth = 0;
                size_t cast_close = std::string::npos;
                for (size_t i = b; i < e; ++i) {
                    if (repl_toks[i] == "(") ++cast_depth;
                    else if (repl_toks[i] == ")") {
                        --cast_depth;
                        if (cast_depth == 0) {
                            cast_close = i;
                            break;
                        }
                    }
                    if (cast_depth < 0) break;
                }
                if (cast_close == std::string::npos || cast_close + 1 != (e - 1)) {
                    out.reason = "unsupported function-like macro: only (Type)param cast form is allowed";
                    return false;
                }
                if (cast_close <= b + 1) {
                    out.reason = "unsupported function-like macro: empty cast type";
                    return false;
                }
                const auto it = param_index.find(repl_toks[e - 1]);
                if (it == param_index.end()) {
                    out.reason = "unsupported function-like macro: cast argument must end with macro parameter";
                    return false;
                }
                arg.param_index = it->second;
                arg.cast_prefix = join_tokens_with_space_(repl_toks, b, cast_close + 1);
                out.args.push_back(std::move(arg));
                return true;
            };

            for (size_t i = 2; i < close_idx; ++i) {
                if (repl_toks[i] == "(") ++inner_depth;
                else if (repl_toks[i] == ")") --inner_depth;
                else if (repl_toks[i] == "," && inner_depth == 0) {
                    if (!parse_one_arg(arg_begin, i)) return false;
                    arg_begin = i + 1;
                }
            }
            if (arg_begin < close_idx || (close_idx == 2 && arg_begin == 2)) {
                if (!(close_idx == 2 && arg_begin == 2)) {
                    if (!parse_one_arg(arg_begin, close_idx)) return false;
                } else {
                    // empty argument list is valid
                }
            }
            return true;
        }

        static std::string ensure_synthetic_name_(
            std::unordered_map<std::string, std::string>& map,
            uint32_t& counter,
            const std::string& key,
            std::string_view prefix
        ) {
            if (auto it = map.find(key); it != map.end()) return it->second;
            std::string out = std::string(prefix) + std::to_string(counter++);
            map.emplace(key, out);
            return out;
        }

        static std::string ensure_enum_name_(CXCursor decl, ImportCollectCtx& ctx);
        static std::string ensure_record_name_(CXCursor decl, ImportCollectCtx& ctx);

        static bool map_c_type_to_parus_(
            CXType t, std::string& out, ImportCollectCtx* ctx, uint32_t depth = 0);

        static bool map_c_fn_type_to_parus_(
            CXType fn_ty, std::string& out, ImportCollectCtx* ctx, uint32_t depth) {
            if (depth > 8) return false;

            const CXType ct = clang_getCanonicalType(fn_ty);
            if (ct.kind != CXType_FunctionProto && ct.kind != CXType_FunctionNoProto) {
                return false;
            }

            const int arg_n = clang_getNumArgTypes(ct);
            if (arg_n < 0) return false;
            if (clang_isFunctionTypeVariadic(ct) != 0) return false;

            std::string ret_repr{};
            if (!map_c_type_to_parus_(clang_getResultType(ct), ret_repr, ctx, depth + 1)) {
                return false;
            }

            std::string sig = "def(";
            for (int i = 0; i < arg_n; ++i) {
                std::string arg_repr{};
                if (!map_c_type_to_parus_(clang_getArgType(ct, i), arg_repr, ctx, depth + 1)) {
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

        static bool map_c_type_to_parus_(CXType t, std::string& out, ImportCollectCtx* ctx, uint32_t depth) {
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
                    return map_c_fn_type_to_parus_(ct, out, ctx, depth + 1);
                case CXType_Enum: {
                    const CXCursor decl = clang_getTypeDeclaration(ct);
                    if (!clang_Cursor_isNull(decl)) {
                        const std::string name = to_std_string_(clang_getCursorSpelling(decl));
                        if (!name.empty()) {
                            out = name;
                            return true;
                        }
                        if (ctx != nullptr) {
                            out = ensure_enum_name_(decl, *ctx);
                            if (!out.empty()) return true;
                        }
                        const CXType int_ty = clang_getEnumDeclIntegerType(decl);
                        if (int_ty.kind != CXType_Invalid) {
                            return map_c_type_to_parus_(int_ty, out, ctx, depth + 1);
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
                        if (!map_c_fn_type_to_parus_(elem, elem_repr, ctx, depth + 1)) {
                            return false;
                        }
                    } else if (!map_c_type_to_parus_(elem, elem_repr, ctx, depth + 1)) {
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
                    if (!name.empty()) {
                        out = name;
                        return true;
                    }
                    if (ctx != nullptr) {
                        out = ensure_record_name_(decl, *ctx);
                        return !out.empty();
                    }
                    return false;
                }
                default:
                    return false;
            }
        }

        static bool map_c_type_to_parus_(CXType t, std::string& out, uint32_t depth = 0) {
            return map_c_type_to_parus_(t, out, nullptr, depth);
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
            std::unordered_set<std::string> macro_dedup{};
            std::unordered_map<std::string, std::string> anon_record_name_by_key{};
            std::unordered_map<std::string, std::string> anon_enum_name_by_key{};
            uint32_t anon_record_counter = 0;
            uint32_t anon_enum_counter = 0;
            CXTranslationUnit tu = nullptr;
            std::vector<CollectedFnDecl>* out_fns = nullptr;
            std::vector<ImportedUnionDecl>* out_unions = nullptr;
            std::vector<ImportedStructDecl>* out_structs = nullptr;
            std::vector<ImportedEnumDecl>* out_enums = nullptr;
            std::vector<ImportedTypedefDecl>* out_typedefs = nullptr;
            std::vector<ImportedMacroDecl>* out_macros = nullptr;
        };

        static std::string ensure_enum_name_(CXCursor decl, ImportCollectCtx& ctx) {
            const std::string key = cursor_identity_key_(decl);
            return ensure_synthetic_name_(
                ctx.anon_enum_name_by_key, ctx.anon_enum_counter, key, "__anon_enum_");
        }

        static std::string ensure_record_name_(CXCursor decl, ImportCollectCtx& ctx) {
            const std::string key = cursor_identity_key_(decl);
            std::string_view prefix = "__anon_record_";
            switch (clang_getCursorKind(decl)) {
                case CXCursor_StructDecl: prefix = "__anon_struct_"; break;
                case CXCursor_UnionDecl: prefix = "__anon_union_"; break;
                default: break;
            }
            return ensure_synthetic_name_(
                ctx.anon_record_name_by_key, ctx.anon_record_counter, key, prefix);
        }

        static bool collect_union_decl_(CXCursor c, ImportCollectCtx& ctx) {
            if (clang_getCursorKind(c) != CXCursor_UnionDecl) return false;
            if (clang_isCursorDefinition(c) == 0) return true;

            std::string name = to_std_string_(clang_getCursorSpelling(c));
            if (name.empty()) name = ensure_record_name_(c, ctx);
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

            std::string name = to_std_string_(clang_getCursorSpelling(c));
            if (name.empty()) name = ensure_record_name_(c, ctx);
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

            std::string name = to_std_string_(clang_getCursorSpelling(c));
            if (name.empty()) name = ensure_enum_name_(c, ctx);
            if (!ctx.enum_dedup.insert(name).second) return true;

            ImportedEnumDecl e{};
            e.name = name;
            if (!map_c_type_to_parus_(clang_getEnumDeclIntegerType(c), e.underlying_type_repr, &ctx)) {
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
            if (!map_c_type_to_parus_(clang_getTypedefDeclUnderlyingType(c), type_repr, &ctx)) {
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
            if (!map_c_type_to_parus_(clang_getResultType(fn_ty), ret_repr, &ctx)) {
                return true;
            }

            std::vector<std::string> arg_repr{};
            arg_repr.reserve(static_cast<size_t>(arg_n));
            std::vector<CXType> arg_types{};
            arg_types.reserve(static_cast<size_t>(arg_n));
            for (int i = 0; i < arg_n; ++i) {
                const CXType arg_ty = clang_getArgType(fn_ty, i);
                std::string one{};
                if (!map_c_type_to_parus_(arg_ty, one, &ctx)) {
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
            fn.api.c_return_type = to_std_string_(clang_getTypeSpelling(clang_getResultType(fn_ty)));
            fn.api.is_c_abi = true;
            fn.api.is_variadic = variadic;
            fn.arg_type_reprs = arg_repr;
            fn.ret_type_repr = ret_repr;
            fn.api.c_arg_types.reserve(arg_types.size());
            for (const auto arg_ty : arg_types) {
                fn.api.c_arg_types.push_back(to_std_string_(clang_getTypeSpelling(arg_ty)));
            }

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

        static bool collect_macro_decl_(CXCursor c, ImportCollectCtx& ctx) {
            if (clang_getCursorKind(c) != CXCursor_MacroDefinition) return false;
            if (ctx.tu == nullptr) return true;

            if (clang_Cursor_isMacroBuiltin(c) != 0) return true;

            const std::string name = to_std_string_(clang_getCursorSpelling(c));
            if (name.empty()) return true;
            if (name.size() >= 2 && name[0] == '_' && name[1] == '_') return true;
            if (!ctx.macro_dedup.insert(name).second) return true;

            ImportedMacroDecl out{};
            out.name = name;
            out.is_function_like = (clang_Cursor_isMacroFunctionLike(c) != 0);

            CXToken* tokens = nullptr;
            unsigned token_count = 0;
            clang_tokenize(ctx.tu, clang_getCursorExtent(c), &tokens, &token_count);
            if (tokens == nullptr || token_count == 0) {
                if (ctx.out_macros != nullptr) ctx.out_macros->push_back(std::move(out));
                if (tokens != nullptr) clang_disposeTokens(ctx.tu, tokens, token_count);
                return true;
            }

            unsigned begin = 0;
            for (unsigned i = 0; i < token_count; ++i) {
                const std::string tok = to_std_string_(clang_getTokenSpelling(ctx.tu, tokens[i]));
                if (tok == name) {
                    begin = i + 1;
                    break;
                }
            }
            std::vector<std::string> def_toks{};
            def_toks.reserve(token_count > begin ? token_count - begin : 0u);
            for (unsigned i = begin; i < token_count; ++i) {
                std::string tok = to_std_string_(clang_getTokenSpelling(ctx.tu, tokens[i]));
                if (tok.empty()) continue;
                def_toks.push_back(std::move(tok));
            }
            clang_disposeTokens(ctx.tu, tokens, token_count);

            if (!out.is_function_like) {
                const std::string replacement = join_tokens_with_space_(def_toks, 0, def_toks.size());
                ImportedConstKind kind = ImportedConstKind::kNone;
                std::string value_text{};
                if (try_parse_macro_const_value_(replacement, kind, value_text)) {
                    out.const_kind = kind;
                    out.value_text = std::move(value_text);
                }
            } else {
                size_t cursor = 0;
                if (parse_macro_function_like_signature_(def_toks, cursor, out) &&
                    !out.is_variadic) {
                    std::vector<std::string> repl_toks{};
                    repl_toks.reserve(def_toks.size() > cursor ? def_toks.size() - cursor : 0u);
                    for (size_t i = cursor; i < def_toks.size(); ++i) {
                        repl_toks.push_back(def_toks[i]);
                    }

                    ParsedMacroCall parsed{};
                    if (parse_macro_replacement_call_(repl_toks, out, parsed)) {
                        out.promote_callee_name = std::move(parsed.callee);
                        out.promote_call_args.reserve(parsed.args.size());
                        for (const auto& pa : parsed.args) {
                            ImportedMacroCallArg a{};
                            a.param_index = pa.param_index;
                            a.cast_prefix = pa.cast_prefix;
                            out.promote_call_args.push_back(std::move(a));
                        }
                    } else {
                        out.skip_reason = std::move(parsed.reason);
                    }
                }
            }

            if (ctx.out_macros != nullptr) {
                ctx.out_macros->push_back(std::move(out));
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
            (void)collect_macro_decl_(c, *ctx);
            return CXChildVisit_Recurse;
        }

    } // namespace
#endif

    HeaderImportResult import_c_header_functions(
        const std::string& importer_source_path,
        const std::string& header_path,
        const std::vector<std::string>& include_dirs,
        const std::vector<std::string>& isystem_dirs,
        const std::vector<std::string>& defines,
        const std::vector<std::string>& undefines,
        const std::vector<std::string>& forced_includes,
        const std::vector<std::string>& imacros
    ) {
        HeaderImportResult out{};

#if !defined(PARUS_HAS_LIBCLANG) || !PARUS_HAS_LIBCLANG
        (void)importer_source_path;
        (void)header_path;
        (void)include_dirs;
        (void)isystem_dirs;
        (void)defines;
        (void)undefines;
        (void)forced_includes;
        (void)imacros;
        out.error = ImportErrorKind::kLibClangUnavailable;
        out.error_text = "libclang unavailable";
        return out;
#else
        const std::string escaped = escape_include_text_(header_path);
        const std::string source = "#include \"" + escaped + "\"\n";
        const std::string tu_name = importer_source_path + ".parus_cimport.c";

        std::vector<std::string> args_storage{};
        args_storage.reserve(
            8 +
            include_dirs.size() +
            isystem_dirs.size() * 2u +
            defines.size() +
            undefines.size() +
            forced_includes.size() * 2u +
            imacros.size() * 2u);
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
        for (const auto& one : defines) {
            if (one.empty()) continue;
            args_storage.emplace_back("-D" + one);
        }
        for (const auto& one : undefines) {
            if (one.empty()) continue;
            args_storage.emplace_back("-U" + one);
        }
        for (const auto& one : forced_includes) {
            if (one.empty()) continue;
            args_storage.emplace_back("-include");
            args_storage.emplace_back(one);
        }
        for (const auto& one : imacros) {
            if (one.empty()) continue;
            args_storage.emplace_back("-imacros");
            args_storage.emplace_back(one);
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
            CXTranslationUnit_DetailedPreprocessingRecord,
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
        ctx.out_macros = &out.macros;
        ctx.tu = tu;

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

        // Promote strictly-convertible function-like macros:
        //   - DirectAlias:  MACRO(a,b) -> callee(a,b)
        //   - ShimForward:  MACRO(y,x) -> callee(x,y) or cast forwarding
        std::unordered_multimap<std::string, size_t> fn_idx_by_name{};
        fn_idx_by_name.reserve(collected_fns.size() * 2u + 1u);
        for (size_t i = 0; i < collected_fns.size(); ++i) {
            fn_idx_by_name.emplace(collected_fns[i].api.name, i);
        }

        for (auto& mc : out.macros) {
            if (!mc.is_function_like) continue;
            if (!mc.skip_reason.empty()) continue;
            if (mc.promote_callee_name.empty()) {
                mc.skip_reason = "unsupported function-like macro: unresolved callee";
                continue;
            }

            const auto range = fn_idx_by_name.equal_range(mc.promote_callee_name);
            const CollectedFnDecl* selected = nullptr;
            for (auto it = range.first; it != range.second; ++it) {
                const auto& cand = collected_fns[it->second];
                if (cand.api.is_variadic) continue;
                if (cand.arg_type_reprs.size() != mc.promote_call_args.size()) continue;
                if (cand.api.c_arg_types.size() != mc.promote_call_args.size()) continue;
                selected = &cand;
                break;
            }
            if (selected == nullptr) {
                mc.skip_reason = "unsupported function-like macro: callee function not found or variadic";
                continue;
            }

            if (mc.params.size() != selected->arg_type_reprs.size()) {
                mc.skip_reason = "unsupported function-like macro: parameter count mismatch";
                continue;
            }

            std::vector<uint32_t> param_use(mc.params.size(), 0u);
            mc.promote_param_type_reprs.assign(mc.params.size(), std::string{});
            mc.promote_param_c_types.assign(mc.params.size(), std::string{});

            bool has_cast = false;
            bool identity_order = true;
            bool invalid = false;
            for (size_t ai = 0; ai < mc.promote_call_args.size(); ++ai) {
                const auto& call_arg = mc.promote_call_args[ai];
                if (call_arg.param_index < 0 ||
                    static_cast<size_t>(call_arg.param_index) >= mc.params.size()) {
                    invalid = true;
                    break;
                }
                const size_t pi = static_cast<size_t>(call_arg.param_index);
                ++param_use[pi];
                if (param_use[pi] > 1u) {
                    invalid = true;
                    break;
                }
                mc.promote_param_type_reprs[pi] = selected->arg_type_reprs[ai];
                mc.promote_param_c_types[pi] = selected->api.c_arg_types[ai];
                if (!call_arg.cast_prefix.empty()) has_cast = true;
                if (pi != ai) identity_order = false;
            }
            if (invalid) {
                mc.skip_reason = "unsupported function-like macro: each parameter must be forwarded exactly once";
                continue;
            }
            for (size_t i = 0; i < param_use.size(); ++i) {
                if (param_use[i] != 1u ||
                    mc.promote_param_type_reprs[i].empty() ||
                    mc.promote_param_c_types[i].empty()) {
                    invalid = true;
                    break;
                }
            }
            if (invalid) {
                mc.skip_reason = "unsupported function-like macro: each parameter must map to one callee argument";
                continue;
            }

            mc.promote_callee_link_name = selected->api.link_name;
            mc.promote_c_return_type = selected->api.c_return_type;
            mc.promote_type_repr = "def(";
            for (size_t i = 0; i < mc.promote_param_type_reprs.size(); ++i) {
                if (i) mc.promote_type_repr += ", ";
                mc.promote_type_repr += mc.promote_param_type_reprs[i];
            }
            mc.promote_type_repr += ") -> ";
            mc.promote_type_repr += selected->ret_type_repr;

            mc.promote_kind = (identity_order && !has_cast)
                ? ImportedMacroPromoteKind::kDirectAlias
                : ImportedMacroPromoteKind::kShimForward;
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
