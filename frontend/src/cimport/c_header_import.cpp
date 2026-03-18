#include <parus/cimport/CHeaderImport.hpp>
#include <parus/cimport/CImportPayload.hpp>
#include <parus/cimport/LibClangProbe.hpp>
#include <parus/os/File.hpp>

#include <algorithm>
#include <chrono>
#include <cerrno>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <limits>
#include <optional>
#include <sstream>
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

        struct InclusionCollectCtx {
            std::vector<std::string>* deps = nullptr;
        };

        static std::string to_std_string_(CXString s) {
            const char* c = clang_getCString(s);
            std::string out = (c != nullptr) ? std::string(c) : std::string{};
            clang_disposeString(s);
            return out;
        }

        static void fill_decl_location_(CXCursor c, std::string& out_file, uint32_t& out_line, uint32_t& out_col) {
            CXSourceLocation loc = clang_getCursorLocation(c);
            if (clang_equalLocations(loc, clang_getNullLocation()) != 0) return;
            CXFile file = nullptr;
            unsigned line = 0;
            unsigned col = 0;
            unsigned offset = 0;
            clang_getFileLocation(loc, &file, &line, &col, &offset);
            (void)offset;
            if (file == nullptr) return;
            out_file = to_std_string_(clang_getFileName(file));
            out_line = (line == 0u) ? 1u : static_cast<uint32_t>(line);
            out_col = (col == 0u) ? 1u : static_cast<uint32_t>(col);
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

        static ImportedGlobalTlsKind map_tls_kind_(CXTLSKind k) {
            switch (k) {
                case CXTLS_Dynamic: return ImportedGlobalTlsKind::kDynamic;
                case CXTLS_Static: return ImportedGlobalTlsKind::kStatic;
                case CXTLS_None:
                default:
                    return ImportedGlobalTlsKind::kNone;
            }
        }

        static void collect_inclusion_visitor_(
            CXFile included_file,
            CXSourceLocation*,
            unsigned,
            CXClientData client_data
        ) {
            auto* ctx = static_cast<InclusionCollectCtx*>(client_data);
            if (ctx == nullptr || ctx->deps == nullptr || included_file == nullptr) return;
            const std::string name = to_std_string_(clang_getFileName(included_file));
            if (name.empty()) return;
            ctx->deps->push_back(parus::normalize_path(name));
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

        static void set_macro_skip_(
            ImportedMacroDecl& out,
            ImportedMacroSkipKind kind,
            std::string_view reason
        ) {
            out.skip_kind = kind;
            out.skip_reason.assign(reason);
        }

        struct ParsedMacroCallArg {
            int32_t param_index = -1;
            std::string cast_prefix{};
        };

        struct ParsedMacroCall {
            std::string callee{};
            std::vector<ParsedMacroCallArg> args{};
            ImportedMacroSkipKind reason_kind = ImportedMacroSkipKind::kNone;
            std::string reason{};
        };

        static void set_parsed_macro_error_(
            ParsedMacroCall& out,
            ImportedMacroSkipKind kind,
            std::string_view reason
        ) {
            out.reason_kind = kind;
            out.reason.assign(reason);
        }

        static void trim_token_span_(const std::vector<std::string>& toks, size_t& begin, size_t& end) {
            while (begin < end && toks[begin].empty()) ++begin;
            while (end > begin && toks[end - 1].empty()) --end;
        }

        static bool is_wrapped_in_parens_(
            const std::vector<std::string>& toks,
            size_t begin,
            size_t end
        ) {
            if (end <= begin + 1) return false;
            if (toks[begin] != "(" || toks[end - 1] != ")") return false;
            int depth = 0;
            for (size_t i = begin; i < end; ++i) {
                if (toks[i] == "(") ++depth;
                else if (toks[i] == ")") {
                    --depth;
                    if (depth < 0) return false;
                    if (depth == 0 && i + 1 < end) return false;
                }
            }
            return depth == 0;
        }

        static void strip_wrapping_parens_(
            const std::vector<std::string>& toks,
            size_t& begin,
            size_t& end
        ) {
            trim_token_span_(toks, begin, end);
            while (is_wrapped_in_parens_(toks, begin, end)) {
                ++begin;
                --end;
                trim_token_span_(toks, begin, end);
            }
        }

        static bool parse_macro_function_like_signature_(
            const std::vector<std::string>& def_toks,
            size_t& cursor,
            ImportedMacroDecl& out
        ) {
            if (cursor >= def_toks.size() || def_toks[cursor] != "(") {
                set_macro_skip_(out, ImportedMacroSkipKind::kSyntaxUnsupported,
                                "unsupported function-like macro: missing parameter list");
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
                    set_macro_skip_(out, ImportedMacroSkipKind::kVariadicExcluded,
                                    "unsupported function-like macro: variadic parameter list");
                    return false;
                }
                if (!is_ident_token_(tok)) {
                    set_macro_skip_(out, ImportedMacroSkipKind::kSyntaxUnsupported,
                                    "unsupported function-like macro: invalid parameter token");
                    return false;
                }
                out.params.push_back(tok);
                ++cursor;
            }

            set_macro_skip_(out, ImportedMacroSkipKind::kSyntaxUnsupported,
                            "unsupported function-like macro: unterminated parameter list");
            return false;
        }

        static bool parse_macro_replacement_call_(
            const std::vector<std::string>& repl_toks,
            const ImportedMacroDecl& macro,
            ParsedMacroCall& out
        ) {
            if (repl_toks.empty()) {
                set_parsed_macro_error_(out, ImportedMacroSkipKind::kSyntaxUnsupported,
                                        "unsupported function-like macro: empty replacement");
                return false;
            }

            for (const auto& t : repl_toks) {
                if (t == "##") {
                    set_parsed_macro_error_(out, ImportedMacroSkipKind::kTokenPasteExcluded,
                                            "unsupported function-like macro: token-paste (##) is excluded");
                    return false;
                }
                if (t == "#") {
                    set_parsed_macro_error_(out, ImportedMacroSkipKind::kStringizeExcluded,
                                            "unsupported function-like macro: stringize (#) is excluded");
                    return false;
                }
                if (t == "{" || t == "}" || t == ";" || t == "__extension__") {
                    set_parsed_macro_error_(out, ImportedMacroSkipKind::kStatementOrExtensionExcluded,
                                            "unsupported function-like macro: statement/extension form is excluded");
                    return false;
                }
            }

            if (repl_toks.size() < 3 || !is_ident_token_(repl_toks[0]) || repl_toks[1] != "(") {
                set_parsed_macro_error_(out, ImportedMacroSkipKind::kSyntaxUnsupported,
                                        "unsupported function-like macro: replacement must be a single function call");
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
                set_parsed_macro_error_(out, ImportedMacroSkipKind::kSyntaxUnsupported,
                                        "unsupported function-like macro: replacement must be one call expression");
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
                trim_token_span_(repl_toks, b, e);
                if (b >= e) {
                    set_parsed_macro_error_(out, ImportedMacroSkipKind::kSyntaxUnsupported,
                                            "unsupported function-like macro: empty call argument");
                    return false;
                }

                ParsedMacroCallArg arg{};
                size_t db = b;
                size_t de = e;
                strip_wrapping_parens_(repl_toks, db, de);
                if ((de - db) == 1u) {
                    const auto it = param_index.find(repl_toks[db]);
                    if (it == param_index.end()) {
                        set_parsed_macro_error_(out, ImportedMacroSkipKind::kInvalidForwarding,
                                                "unsupported function-like macro: call arg must be macro parameter");
                        return false;
                    }
                    arg.param_index = it->second;
                    out.args.push_back(std::move(arg));
                    return true;
                }

                // simple cast forwarding: ( <type tokens> ) <param>
                if (repl_toks[db] != "(") {
                    set_parsed_macro_error_(out, ImportedMacroSkipKind::kInvalidForwarding,
                                            "unsupported function-like macro: only direct or simple-cast forwarding is allowed");
                    return false;
                }
                int cast_depth = 0;
                size_t cast_close = std::string::npos;
                for (size_t i = db; i < de; ++i) {
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
                if (cast_close == std::string::npos || cast_close + 1 >= de) {
                    set_parsed_macro_error_(out, ImportedMacroSkipKind::kInvalidForwarding,
                                            "unsupported function-like macro: only (Type)param cast form is allowed");
                    return false;
                }
                if (cast_close <= db + 1) {
                    set_parsed_macro_error_(out, ImportedMacroSkipKind::kInvalidForwarding,
                                            "unsupported function-like macro: empty cast type");
                    return false;
                }
                size_t pb = cast_close + 1;
                size_t pe = de;
                strip_wrapping_parens_(repl_toks, pb, pe);
                if (pe <= pb || pe - pb != 1u) {
                    set_parsed_macro_error_(out, ImportedMacroSkipKind::kInvalidForwarding,
                                            "unsupported function-like macro: cast argument must end with macro parameter");
                    return false;
                }
                const auto it = param_index.find(repl_toks[pb]);
                if (it == param_index.end()) {
                    set_parsed_macro_error_(out, ImportedMacroSkipKind::kInvalidForwarding,
                                            "unsupported function-like macro: cast argument must end with macro parameter");
                    return false;
                }
                arg.param_index = it->second;
                arg.cast_prefix = join_tokens_with_space_(repl_toks, db, cast_close + 1);
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

        enum class MacroConstEvalKind : uint8_t {
            kInvalid = 0,
            kInt,
            kFloat,
            kBool,
            kChar,
            kString,
        };

        struct MacroConstEvalValue {
            MacroConstEvalKind kind = MacroConstEvalKind::kInvalid;
            int64_t i64 = 0;
            double f64 = 0.0;
            bool b = false;
            std::string text{};
        };

        static bool parse_macro_int_value_(std::string_view text, int64_t& out) {
            std::string canonical{};
            if (!parse_macro_int_literal_text_(text, canonical)) return false;

            std::string normalized{};
            normalized.reserve(canonical.size());
            for (const char ch : canonical) {
                if (ch == '\'') continue;
                normalized.push_back(ch);
            }
            if (normalized.empty()) return false;

            errno = 0;
            char* endp = nullptr;
            const long long parsed = std::strtoll(normalized.c_str(), &endp, 0);
            if (endp == nullptr || *endp != '\0' || errno == ERANGE) return false;
            out = static_cast<int64_t>(parsed);
            return true;
        }

        static bool parse_macro_float_value_(std::string_view text, double& out) {
            std::string canonical{};
            if (!parse_macro_float_literal_text_(text, canonical)) return false;

            std::string normalized{};
            normalized.reserve(canonical.size());
            for (const char ch : canonical) {
                if (ch == '\'') continue;
                normalized.push_back(ch);
            }
            if (normalized.empty()) return false;

            errno = 0;
            char* endp = nullptr;
            const double parsed = std::strtod(normalized.c_str(), &endp);
            if (endp == nullptr || *endp != '\0' || errno == ERANGE) return false;
            out = parsed;
            return true;
        }

        static bool parse_macro_char_value_(
            std::string_view text,
            int64_t& out_code,
            std::string& out_lit
        ) {
            if (!parse_macro_char_literal_text_(text, out_lit)) return false;
            if (out_lit.size() < 3 || out_lit.front() != '\'' || out_lit.back() != '\'') return false;
            const std::string_view body = std::string_view(out_lit).substr(1, out_lit.size() - 2);
            if (body.empty()) return false;

            auto hex_val = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
                if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
                return -1;
            };

            int v = 0;
            if (body[0] != '\\') {
                if (body.size() != 1) return false;
                v = static_cast<unsigned char>(body[0]);
            } else {
                if (body.size() < 2) return false;
                switch (body[1]) {
                    case 'n': v = '\n'; break;
                    case 'r': v = '\r'; break;
                    case 't': v = '\t'; break;
                    case '\\': v = '\\'; break;
                    case '\'': v = '\''; break;
                    case '"': v = '"'; break;
                    case '0': v = '\0'; break;
                    case 'x': {
                        if (body.size() != 4) return false;
                        const int hi = hex_val(body[2]);
                        const int lo = hex_val(body[3]);
                        if (hi < 0 || lo < 0) return false;
                        v = (hi << 4) | lo;
                        break;
                    }
                    default:
                        return false;
                }
            }
            out_code = static_cast<int64_t>(v);
            return true;
        }

        static bool macro_const_is_numeric_(const MacroConstEvalValue& v) {
            return v.kind == MacroConstEvalKind::kInt ||
                   v.kind == MacroConstEvalKind::kFloat ||
                   v.kind == MacroConstEvalKind::kBool ||
                   v.kind == MacroConstEvalKind::kChar;
        }

        static bool macro_const_to_boolish_(const MacroConstEvalValue& v, bool& out) {
            switch (v.kind) {
                case MacroConstEvalKind::kBool:
                    out = v.b;
                    return true;
                case MacroConstEvalKind::kInt:
                    out = (v.i64 != 0);
                    return true;
                case MacroConstEvalKind::kFloat:
                    out = (v.f64 != 0.0);
                    return true;
                case MacroConstEvalKind::kChar:
                    out = (v.i64 != 0);
                    return true;
                default:
                    return false;
            }
        }

        static bool macro_const_to_int_(const MacroConstEvalValue& v, int64_t& out) {
            switch (v.kind) {
                case MacroConstEvalKind::kInt:
                case MacroConstEvalKind::kChar:
                    out = v.i64;
                    return true;
                case MacroConstEvalKind::kBool:
                    out = v.b ? 1 : 0;
                    return true;
                default:
                    return false;
            }
        }

        static bool macro_const_to_float_(const MacroConstEvalValue& v, double& out) {
            switch (v.kind) {
                case MacroConstEvalKind::kFloat:
                    out = v.f64;
                    return true;
                case MacroConstEvalKind::kInt:
                case MacroConstEvalKind::kChar:
                    out = static_cast<double>(v.i64);
                    return true;
                case MacroConstEvalKind::kBool:
                    out = v.b ? 1.0 : 0.0;
                    return true;
                default:
                    return false;
            }
        }

        static bool macro_const_to_imported_const_(
            const MacroConstEvalValue& in,
            ImportedConstKind& out_kind,
            std::string& out_text
        ) {
            switch (in.kind) {
                case MacroConstEvalKind::kInt:
                    out_kind = ImportedConstKind::kInt;
                    out_text = std::to_string(in.i64);
                    return true;
                case MacroConstEvalKind::kFloat: {
                    std::ostringstream oss;
                    oss.setf(std::ios::fmtflags(0), std::ios::floatfield);
                    oss.precision(17);
                    oss << in.f64;
                    out_kind = ImportedConstKind::kFloat;
                    out_text = oss.str();
                    return true;
                }
                case MacroConstEvalKind::kBool:
                    out_kind = ImportedConstKind::kBool;
                    out_text = in.b ? "1" : "0";
                    return true;
                case MacroConstEvalKind::kChar:
                    out_kind = ImportedConstKind::kChar;
                    out_text = in.text;
                    return !out_text.empty();
                case MacroConstEvalKind::kString:
                    out_kind = ImportedConstKind::kString;
                    out_text = in.text;
                    return !out_text.empty();
                default:
                    break;
            }
            out_kind = ImportedConstKind::kNone;
            out_text.clear();
            return false;
        }

        class MacroConstExprParser_ {
        public:
            using Resolver = std::function<std::optional<MacroConstEvalValue>(std::string_view)>;

            MacroConstExprParser_(const std::vector<std::string>& toks, Resolver resolver)
                : toks_(toks), resolver_(std::move(resolver)) {}

            bool parse(MacroConstEvalValue& out) {
                if (!parse_lor_(out)) return false;
                return pos_ == toks_.size();
            }

            std::string error() const { return error_; }

        private:
            bool parse_lor_(MacroConstEvalValue& out) {
                if (!parse_land_(out)) return false;
                while (match_("||")) {
                    MacroConstEvalValue rhs{};
                    if (!parse_land_(rhs)) return false;
                    bool lb = false;
                    bool rb = false;
                    if (!macro_const_to_boolish_(out, lb) || !macro_const_to_boolish_(rhs, rb)) {
                        return fail_("logical operator requires boolean-like operands");
                    }
                    out = {};
                    out.kind = MacroConstEvalKind::kBool;
                    out.b = lb || rb;
                }
                return true;
            }

            bool parse_land_(MacroConstEvalValue& out) {
                if (!parse_bitor_(out)) return false;
                while (match_("&&")) {
                    MacroConstEvalValue rhs{};
                    if (!parse_bitor_(rhs)) return false;
                    bool lb = false;
                    bool rb = false;
                    if (!macro_const_to_boolish_(out, lb) || !macro_const_to_boolish_(rhs, rb)) {
                        return fail_("logical operator requires boolean-like operands");
                    }
                    out = {};
                    out.kind = MacroConstEvalKind::kBool;
                    out.b = lb && rb;
                }
                return true;
            }

            bool parse_bitor_(MacroConstEvalValue& out) {
                if (!parse_bitxor_(out)) return false;
                while (match_("|")) {
                    MacroConstEvalValue rhs{};
                    if (!parse_bitxor_(rhs)) return false;
                    int64_t li = 0;
                    int64_t ri = 0;
                    if (!macro_const_to_int_(out, li) || !macro_const_to_int_(rhs, ri)) {
                        return fail_("bitwise operator requires integer-like operands");
                    }
                    out = {};
                    out.kind = MacroConstEvalKind::kInt;
                    out.i64 = li | ri;
                }
                return true;
            }

            bool parse_bitxor_(MacroConstEvalValue& out) {
                if (!parse_bitand_(out)) return false;
                while (match_("^")) {
                    MacroConstEvalValue rhs{};
                    if (!parse_bitand_(rhs)) return false;
                    int64_t li = 0;
                    int64_t ri = 0;
                    if (!macro_const_to_int_(out, li) || !macro_const_to_int_(rhs, ri)) {
                        return fail_("bitwise operator requires integer-like operands");
                    }
                    out = {};
                    out.kind = MacroConstEvalKind::kInt;
                    out.i64 = li ^ ri;
                }
                return true;
            }

            bool parse_bitand_(MacroConstEvalValue& out) {
                if (!parse_eq_(out)) return false;
                while (match_("&")) {
                    MacroConstEvalValue rhs{};
                    if (!parse_eq_(rhs)) return false;
                    int64_t li = 0;
                    int64_t ri = 0;
                    if (!macro_const_to_int_(out, li) || !macro_const_to_int_(rhs, ri)) {
                        return fail_("bitwise operator requires integer-like operands");
                    }
                    out = {};
                    out.kind = MacroConstEvalKind::kInt;
                    out.i64 = li & ri;
                }
                return true;
            }

            bool parse_eq_(MacroConstEvalValue& out) {
                if (!parse_rel_(out)) return false;
                for (;;) {
                    const bool is_eq = match_("==");
                    const bool is_ne = !is_eq && match_("!=");
                    if (!is_eq && !is_ne) break;
                    MacroConstEvalValue rhs{};
                    if (!parse_rel_(rhs)) return false;
                    bool ok = false;
                    bool result = false;
                    if (out.kind == MacroConstEvalKind::kString && rhs.kind == MacroConstEvalKind::kString) {
                        result = (out.text == rhs.text);
                        ok = true;
                    } else if (macro_const_is_numeric_(out) && macro_const_is_numeric_(rhs)) {
                        if (out.kind == MacroConstEvalKind::kFloat || rhs.kind == MacroConstEvalKind::kFloat) {
                            double lf = 0.0;
                            double rf = 0.0;
                            ok = macro_const_to_float_(out, lf) && macro_const_to_float_(rhs, rf);
                            if (ok) result = (lf == rf);
                        } else {
                            int64_t li = 0;
                            int64_t ri = 0;
                            ok = macro_const_to_int_(out, li) && macro_const_to_int_(rhs, ri);
                            if (ok) result = (li == ri);
                        }
                    }
                    if (!ok) return fail_("equality operator requires compatible operands");
                    out = {};
                    out.kind = MacroConstEvalKind::kBool;
                    out.b = is_eq ? result : !result;
                }
                return true;
            }

            bool parse_rel_(MacroConstEvalValue& out) {
                if (!parse_shift_(out)) return false;
                for (;;) {
                    std::string op{};
                    if (match_("<=")) op = "<=";
                    else if (match_("<")) op = "<";
                    else if (match_(">=")) op = ">=";
                    else if (match_(">")) op = ">";
                    else break;

                    MacroConstEvalValue rhs{};
                    if (!parse_shift_(rhs)) return false;
                    bool result = false;
                    if (out.kind == MacroConstEvalKind::kFloat || rhs.kind == MacroConstEvalKind::kFloat) {
                        double lf = 0.0;
                        double rf = 0.0;
                        if (!macro_const_to_float_(out, lf) || !macro_const_to_float_(rhs, rf)) {
                            return fail_("relational operator requires numeric operands");
                        }
                        if (op == "<") result = lf < rf;
                        else if (op == "<=") result = lf <= rf;
                        else if (op == ">") result = lf > rf;
                        else result = lf >= rf;
                    } else {
                        int64_t li = 0;
                        int64_t ri = 0;
                        if (!macro_const_to_int_(out, li) || !macro_const_to_int_(rhs, ri)) {
                            return fail_("relational operator requires numeric operands");
                        }
                        if (op == "<") result = li < ri;
                        else if (op == "<=") result = li <= ri;
                        else if (op == ">") result = li > ri;
                        else result = li >= ri;
                    }
                    out = {};
                    out.kind = MacroConstEvalKind::kBool;
                    out.b = result;
                }
                return true;
            }

            bool parse_shift_(MacroConstEvalValue& out) {
                if (!parse_add_(out)) return false;
                for (;;) {
                    const bool is_l = match_("<<");
                    const bool is_r = !is_l && match_(">>");
                    if (!is_l && !is_r) break;
                    MacroConstEvalValue rhs{};
                    if (!parse_add_(rhs)) return false;
                    int64_t li = 0;
                    int64_t ri = 0;
                    if (!macro_const_to_int_(out, li) || !macro_const_to_int_(rhs, ri)) {
                        return fail_("shift operator requires integer operands");
                    }
                    if (ri < 0 || ri >= 63) {
                        return fail_("shift amount must be within [0, 62]");
                    }
                    out = {};
                    out.kind = MacroConstEvalKind::kInt;
                    out.i64 = is_l ? (li << ri) : (li >> ri);
                }
                return true;
            }

            bool parse_add_(MacroConstEvalValue& out) {
                if (!parse_mul_(out)) return false;
                for (;;) {
                    const bool is_add = match_("+");
                    const bool is_sub = !is_add && match_("-");
                    if (!is_add && !is_sub) break;
                    MacroConstEvalValue rhs{};
                    if (!parse_mul_(rhs)) return false;
                    if (out.kind == MacroConstEvalKind::kFloat || rhs.kind == MacroConstEvalKind::kFloat) {
                        double lf = 0.0;
                        double rf = 0.0;
                        if (!macro_const_to_float_(out, lf) || !macro_const_to_float_(rhs, rf)) {
                            return fail_("arithmetic operator requires numeric operands");
                        }
                        out = {};
                        out.kind = MacroConstEvalKind::kFloat;
                        out.f64 = is_add ? (lf + rf) : (lf - rf);
                    } else {
                        int64_t li = 0;
                        int64_t ri = 0;
                        if (!macro_const_to_int_(out, li) || !macro_const_to_int_(rhs, ri)) {
                            return fail_("arithmetic operator requires numeric operands");
                        }
                        out = {};
                        out.kind = MacroConstEvalKind::kInt;
                        out.i64 = is_add ? (li + ri) : (li - ri);
                    }
                }
                return true;
            }

            bool parse_mul_(MacroConstEvalValue& out) {
                if (!parse_unary_(out)) return false;
                for (;;) {
                    const bool is_mul = match_("*");
                    const bool is_div = !is_mul && match_("/");
                    const bool is_mod = !is_mul && !is_div && match_("%");
                    if (!is_mul && !is_div && !is_mod) break;
                    MacroConstEvalValue rhs{};
                    if (!parse_unary_(rhs)) return false;
                    if (is_mod) {
                        int64_t li = 0;
                        int64_t ri = 0;
                        if (!macro_const_to_int_(out, li) || !macro_const_to_int_(rhs, ri)) {
                            return fail_("mod operator requires integer operands");
                        }
                        if (ri == 0) return fail_("division by zero in macro constant expression");
                        out = {};
                        out.kind = MacroConstEvalKind::kInt;
                        out.i64 = li % ri;
                        continue;
                    }
                    if (out.kind == MacroConstEvalKind::kFloat || rhs.kind == MacroConstEvalKind::kFloat) {
                        double lf = 0.0;
                        double rf = 0.0;
                        if (!macro_const_to_float_(out, lf) || !macro_const_to_float_(rhs, rf)) {
                            return fail_("arithmetic operator requires numeric operands");
                        }
                        if (is_div && rf == 0.0) return fail_("division by zero in macro constant expression");
                        out = {};
                        out.kind = MacroConstEvalKind::kFloat;
                        out.f64 = is_mul ? (lf * rf) : (lf / rf);
                    } else {
                        int64_t li = 0;
                        int64_t ri = 0;
                        if (!macro_const_to_int_(out, li) || !macro_const_to_int_(rhs, ri)) {
                            return fail_("arithmetic operator requires numeric operands");
                        }
                        if (is_div && ri == 0) return fail_("division by zero in macro constant expression");
                        out = {};
                        out.kind = MacroConstEvalKind::kInt;
                        out.i64 = is_mul ? (li * ri) : (li / ri);
                    }
                }
                return true;
            }

            bool parse_unary_(MacroConstEvalValue& out) {
                if (match_("+")) {
                    if (!parse_unary_(out)) return false;
                    if (!macro_const_is_numeric_(out)) {
                        return fail_("unary '+' requires numeric operand");
                    }
                    if (out.kind == MacroConstEvalKind::kBool || out.kind == MacroConstEvalKind::kChar) {
                        int64_t iv = 0;
                        if (!macro_const_to_int_(out, iv)) return false;
                        out = {};
                        out.kind = MacroConstEvalKind::kInt;
                        out.i64 = iv;
                    }
                    return true;
                }
                if (match_("-")) {
                    if (!parse_unary_(out)) return false;
                    if (out.kind == MacroConstEvalKind::kFloat) {
                        out.f64 = -out.f64;
                        return true;
                    }
                    int64_t iv = 0;
                    if (!macro_const_to_int_(out, iv)) {
                        return fail_("unary '-' requires numeric operand");
                    }
                    out = {};
                    out.kind = MacroConstEvalKind::kInt;
                    out.i64 = -iv;
                    return true;
                }
                if (match_("~")) {
                    if (!parse_unary_(out)) return false;
                    int64_t iv = 0;
                    if (!macro_const_to_int_(out, iv)) return fail_("unary '~' requires integer operand");
                    out = {};
                    out.kind = MacroConstEvalKind::kInt;
                    out.i64 = ~iv;
                    return true;
                }
                if (match_("!")) {
                    if (!parse_unary_(out)) return false;
                    bool bv = false;
                    if (!macro_const_to_boolish_(out, bv)) return fail_("unary '!' requires boolean-like operand");
                    out = {};
                    out.kind = MacroConstEvalKind::kBool;
                    out.b = !bv;
                    return true;
                }
                return parse_primary_(out);
            }

            bool parse_primary_(MacroConstEvalValue& out) {
                if (match_("(")) {
                    if (!parse_lor_(out)) return false;
                    if (!match_(")")) return fail_("missing closing ')' in macro constant expression");
                    return true;
                }

                if (pos_ >= toks_.size()) return fail_("unexpected end of macro constant expression");
                const std::string tok = toks_[pos_++];

                if (tok == "true" || tok == "false") {
                    out = {};
                    out.kind = MacroConstEvalKind::kBool;
                    out.b = (tok == "true");
                    return true;
                }

                {
                    std::string str_lit{};
                    if (parse_macro_string_literal_text_(tok, str_lit)) {
                        out = {};
                        out.kind = MacroConstEvalKind::kString;
                        out.text = std::move(str_lit);
                        return true;
                    }
                }
                {
                    int64_t ch = 0;
                    std::string ch_lit{};
                    if (parse_macro_char_value_(tok, ch, ch_lit)) {
                        out = {};
                        out.kind = MacroConstEvalKind::kChar;
                        out.i64 = ch;
                        out.text = std::move(ch_lit);
                        return true;
                    }
                }
                {
                    int64_t i64 = 0;
                    if (parse_macro_int_value_(tok, i64)) {
                        out = {};
                        out.kind = MacroConstEvalKind::kInt;
                        out.i64 = i64;
                        return true;
                    }
                }
                {
                    double f64 = 0.0;
                    if (parse_macro_float_value_(tok, f64)) {
                        out = {};
                        out.kind = MacroConstEvalKind::kFloat;
                        out.f64 = f64;
                        return true;
                    }
                }

                if (is_ident_token_(tok)) {
                    if (!resolver_) return fail_("identifier cannot be resolved in macro constant expression");
                    const auto resolved = resolver_(tok);
                    if (!resolved.has_value()) {
                        return fail_("unresolved identifier '" + tok + "' in macro constant expression");
                    }
                    out = *resolved;
                    return true;
                }

                return fail_("unsupported token '" + tok + "' in macro constant expression");
            }

            bool match_(std::string_view tok) {
                if (pos_ >= toks_.size()) return false;
                if (toks_[pos_] != tok) return false;
                ++pos_;
                return true;
            }

            bool fail_(std::string msg) {
                if (error_.empty()) error_ = std::move(msg);
                return false;
            }

            const std::vector<std::string>& toks_;
            Resolver resolver_{};
            size_t pos_ = 0;
            std::string error_{};
        };

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

            if (is_va_list_type_(t)) {
                out = "core::ext::vaList";
                return true;
            }

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
                    out = "core::ext::c_void";
                    return true;
                case CXType_Bool:
                    out = "bool";
                    return true;
                case CXType_Char_U:
                case CXType_UChar:
                    out = "core::ext::c_uchar";
                    return true;
                case CXType_Char_S:
                case CXType_SChar:
                    out = "core::ext::c_schar";
                    return true;
                case CXType_UShort:
                    out = "core::ext::c_ushort";
                    return true;
                case CXType_UInt:
                    out = "core::ext::c_uint";
                    return true;
                case CXType_ULong:
                    out = "core::ext::c_ulong";
                    return true;
                case CXType_ULongLong:
                    out = "core::ext::c_ulonglong";
                    return true;
                case CXType_UInt128:
                    out = "u128";
                    return true;
                case CXType_Short:
                    out = "core::ext::c_short";
                    return true;
                case CXType_Int:
                    out = "core::ext::c_int";
                    return true;
                case CXType_Long:
                    out = "core::ext::c_long";
                    return true;
                case CXType_LongLong:
                    out = "core::ext::c_longlong";
                    return true;
                case CXType_Int128:
                    out = "i128";
                    return true;
                case CXType_Float:
                    out = "core::ext::c_float";
                    return true;
                case CXType_Double:
                    out = "core::ext::c_double";
                    return true;
                case CXType_LongDouble:
                    out = "core::ext::c_double";
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
                    if (elem.kind == CXType_FunctionProto || elem.kind == CXType_FunctionNoProto) {
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

        static CCallConvKind map_callconv_kind_(CXCallingConv cc) {
            switch (cc) {
                case CXCallingConv_X86StdCall:
                    return CCallConvKind::kStdCall;
                case CXCallingConv_X86FastCall:
                    return CCallConvKind::kFastCall;
                case CXCallingConv_X86VectorCall:
                    return CCallConvKind::kVectorCall;
                case CXCallingConv_Win64:
                    return CCallConvKind::kWin64;
                case CXCallingConv_X86_64SysV:
                    return CCallConvKind::kSysV;
                case CXCallingConv_Default:
                case CXCallingConv_C:
                    return CCallConvKind::kCdecl;
                default:
                    return CCallConvKind::kDefault;
            }
        }

        static bool is_signed_integer_type_(CXType ty) {
            const CXType ct = clang_getCanonicalType(ty);
            switch (ct.kind) {
                case CXType_Char_S:
                case CXType_SChar:
                case CXType_Short:
                case CXType_Int:
                case CXType_Long:
                case CXType_LongLong:
                case CXType_Int128:
                    return true;
                default:
                    return false;
            }
        }

        static bool is_transparent_typedef_underlying_(CXType ty) {
            const CXType ct = clang_getCanonicalType(ty);
            switch (ct.kind) {
                case CXType_Bool:
                case CXType_Char_U:
                case CXType_UChar:
                case CXType_Char_S:
                case CXType_SChar:
                case CXType_UShort:
                case CXType_UInt:
                case CXType_ULong:
                case CXType_ULongLong:
                case CXType_UInt128:
                case CXType_Short:
                case CXType_Int:
                case CXType_Long:
                case CXType_LongLong:
                case CXType_Int128:
                case CXType_Float:
                case CXType_Double:
                case CXType_LongDouble:
                case CXType_Pointer:
                case CXType_FunctionProto:
                case CXType_FunctionNoProto:
                    return true;
                default:
                    return false;
            }
        }

        struct ImportCollectCtx {
            std::unordered_set<std::string> fn_dedup{};
            std::unordered_set<std::string> global_dedup{};
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
            std::vector<ImportedGlobalDecl>* out_globals = nullptr;
            std::vector<ImportedUnionDecl>* out_unions = nullptr;
            std::vector<ImportedStructDecl>* out_structs = nullptr;
            std::vector<ImportedEnumDecl>* out_enums = nullptr;
            std::vector<ImportedTypedefDecl>* out_typedefs = nullptr;
            std::vector<ImportedMacroDecl>* out_macros = nullptr;
            std::string hard_error_text{};
            bool had_hard_error = false;
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
            fill_decl_location_(c, u.decl_file, u.decl_line, u.decl_col);

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
            fill_decl_location_(c, s.decl_file, s.decl_line, s.decl_col);

            const CXType struct_ty = clang_getCursorType(c);
            s.c_type_spelling = to_std_string_(clang_getTypeSpelling(struct_ty));
            const long long sz = clang_Type_getSizeOf(struct_ty);
            const long long al = clang_Type_getAlignOf(struct_ty);
            if (sz > 0) s.size_bytes = static_cast<uint32_t>(sz);
            if (al > 0) s.align_bytes = static_cast<uint32_t>(al);
            s.is_packed = false;

            auto add_struct_field = [&](ImportedStructFieldDecl one, std::string_view source_path) -> bool {
                for (const auto& existing : s.fields) {
                    if (existing.name != one.name) continue;
                    ctx.had_hard_error = true;
                    ctx.hard_error_text =
                        "anonymous field flatten collision in struct '" + s.name +
                        "': duplicated field '" + one.name + "'";
                    if (!source_path.empty()) {
                        ctx.hard_error_text += " at ";
                        ctx.hard_error_text += std::string(source_path);
                    }
                    return false;
                }
                s.fields.push_back(std::move(one));
                return true;
            };

            auto collect_fields_recursive = [&](auto&& self,
                                                CXCursor record_decl,
                                                uint64_t base_offset_bits,
                                                bool union_origin,
                                                std::string_view trace_path) -> bool {
                if (ctx.had_hard_error) return false;
                const CXCursorKind record_kind = clang_getCursorKind(record_decl);
                const bool nested_union_origin = union_origin || (record_kind == CXCursor_UnionDecl);
                std::function<CXChildVisitResult(CXCursor)> visit_one =
                    [&](CXCursor child) -> CXChildVisitResult {
                        if (clang_getCursorKind(child) != CXCursor_FieldDecl) {
                            return CXChildVisit_Continue;
                        }

                        const long long rel_off_bits = clang_Cursor_getOffsetOfField(child);
                        if (rel_off_bits < 0) {
                            return CXChildVisit_Continue;
                        }
                        const uint64_t abs_off_bits = base_offset_bits + static_cast<uint64_t>(rel_off_bits);
                        const CXType child_ty = clang_getCursorType(child);
                        const CXType child_ty_can = clang_getCanonicalType(child_ty);
                        const std::string field_name = to_std_string_(clang_getCursorSpelling(child));

                        if (field_name.empty()) {
                            CXCursor nested_decl = clang_getTypeDeclaration(child_ty_can);
                            if (clang_Cursor_isNull(nested_decl)) {
                                nested_decl = clang_getTypeDeclaration(child_ty);
                            }
                            if (clang_Cursor_isNull(nested_decl)) {
                                return CXChildVisit_Continue;
                            }
                            const CXCursorKind nk = clang_getCursorKind(nested_decl);
                            if (nk != CXCursor_StructDecl && nk != CXCursor_UnionDecl) {
                                return CXChildVisit_Continue;
                            }
                            const std::string nested_name = to_std_string_(clang_getCursorSpelling(nested_decl));
                            const bool ok = self(
                                self,
                                nested_decl,
                                abs_off_bits,
                                nested_union_origin || nk == CXCursor_UnionDecl,
                                nested_name.empty() ? trace_path : std::string(trace_path).append("::").append(nested_name));
                            return ok ? CXChildVisit_Continue : CXChildVisit_Break;
                        }

                        std::string field_ty{};
                        if (!map_c_type_to_parus_(child_ty, field_ty, &ctx)) {
                            return CXChildVisit_Continue;
                        }

                        ImportedStructFieldDecl fd{};
                        fd.name = field_name;
                        fd.type_repr = std::move(field_ty);
                        fd.c_type = to_std_string_(clang_getTypeSpelling(child_ty));
                        fd.offset_bytes = static_cast<uint32_t>(abs_off_bits / 8u);
                        fd.from_flatten = !std::string(trace_path).empty() && std::string(trace_path) != s.name;
                        fd.union_origin = nested_union_origin;

                        if (clang_Cursor_isBitField(child) != 0) {
                            fd.is_bitfield = true;
                            const int width = clang_getFieldDeclBitWidth(child);
                            if (width > 0) {
                                fd.bit_width = static_cast<uint32_t>(width);
                            }
                            fd.bit_offset = static_cast<uint32_t>(abs_off_bits);
                            fd.bit_signed = is_signed_integer_type_(child_ty_can);
                            const long long storage_size = clang_Type_getSizeOf(child_ty_can);
                            if (storage_size > 0) {
                                const auto unit = static_cast<uint64_t>(storage_size);
                                const auto byte_off = static_cast<uint64_t>(abs_off_bits / 8u);
                                fd.bit_storage_offset_bytes = static_cast<uint32_t>((byte_off / unit) * unit);
                            } else {
                                fd.bit_storage_offset_bytes = fd.offset_bytes;
                            }
                        }

                        if (!add_struct_field(std::move(fd), trace_path)) {
                            return CXChildVisit_Break;
                        }
                        return CXChildVisit_Continue;
                    };

                clang_visitChildren(
                    record_decl,
                    [](CXCursor child, CXCursor, CXClientData data) -> CXChildVisitResult {
                        auto* fn = static_cast<std::function<CXChildVisitResult(CXCursor)>*>(data);
                        if (fn == nullptr) return CXChildVisit_Continue;
                        return (*fn)(child);
                    },
                    &visit_one
                );
                return !ctx.had_hard_error;
            };

            (void)collect_fields_recursive(collect_fields_recursive, c, 0u, false, s.name);
            if (ctx.had_hard_error) return true;

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
            fill_decl_location_(c, e.decl_file, e.decl_line, e.decl_col);
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
                    fill_decl_location_(child, one.decl_file, one.decl_line, one.decl_col);
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

            const CXType under_ty = clang_getTypedefDeclUnderlyingType(c);

            std::string type_repr{};
            if (!map_c_type_to_parus_(under_ty, type_repr, &ctx)) {
                return true;
            }

            ImportedTypedefDecl td{};
            td.name = name;
            td.type_repr = std::move(type_repr);
            td.is_transparent = is_transparent_typedef_underlying_(under_ty);
            if (td.is_transparent) {
                std::string canonical_repr{};
                if (map_c_type_to_parus_(clang_getCanonicalType(under_ty), canonical_repr, &ctx) &&
                    !canonical_repr.empty()) {
                    td.transparent_type_repr = std::move(canonical_repr);
                } else {
                    td.is_transparent = false;
                    td.transparent_type_repr.clear();
                }
            }
            fill_decl_location_(c, td.decl_file, td.decl_line, td.decl_col);
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
            fill_decl_location_(c, fn.api.decl_file, fn.api.decl_line, fn.api.decl_col);
            fn.api.c_return_type = to_std_string_(clang_getTypeSpelling(clang_getResultType(fn_ty)));
            fn.api.is_c_abi = true;
            fn.api.is_variadic = variadic;
            fn.api.callconv = map_callconv_kind_(clang_getFunctionTypeCallingConv(fn_ty));
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

        static bool collect_global_decl_(CXCursor c, ImportCollectCtx& ctx) {
            if (clang_getCursorKind(c) != CXCursor_VarDecl) return false;

            const CXLinkageKind linkage = clang_getCursorLinkage(c);
            if (linkage == CXLinkage_Internal || linkage == CXLinkage_NoLinkage) {
                return true;
            }

            const std::string name = to_std_string_(clang_getCursorSpelling(c));
            if (name.empty()) return true;
            if (!ctx.global_dedup.insert(name).second) return true;

            const CXType var_ty = clang_getCursorType(c);
            std::string type_repr{};
            if (!map_c_type_to_parus_(var_ty, type_repr, &ctx)) {
                return true;
            }

            ImportedGlobalDecl gv{};
            gv.name = name;
            gv.link_name = name;
            gv.type_repr = std::move(type_repr);
            gv.c_type = to_std_string_(clang_getTypeSpelling(var_ty));
            gv.is_c_abi = true;
            gv.is_const = clang_isConstQualifiedType(var_ty) != 0;
            gv.is_volatile = clang_isVolatileQualifiedType(var_ty) != 0;
            gv.is_restrict = clang_isRestrictQualifiedType(var_ty) != 0;
            gv.tls_kind = map_tls_kind_(clang_getCursorTLSKind(c));
            fill_decl_location_(c, gv.decl_file, gv.decl_line, gv.decl_col);

            if (ctx.out_globals != nullptr) {
                ctx.out_globals->push_back(std::move(gv));
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
            fill_decl_location_(c, out.decl_file, out.decl_line, out.decl_col);

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
            out.replacement_tokens = def_toks;

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
                    out.replacement_tokens = repl_toks;

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
                        out.skip_kind = parsed.reason_kind;
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
            (void)collect_global_decl_(c, *ctx);
            (void)collect_macro_decl_(c, *ctx);
            return CXChildVisit_Recurse;
        }

    } // namespace
#endif

    HeaderImportResult import_c_header_functions(
        const std::string& importer_source_path,
        const std::string& header_path,
        const std::string& target_triple,
        const std::string& sysroot_path,
        const std::string& apple_sdk_root,
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
        (void)target_triple;
        (void)sysroot_path;
        (void)apple_sdk_root;
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
        (void)sysroot_path;

        std::vector<std::string> args_storage{};
        args_storage.reserve(
            12 +
            (target_triple.empty() ? 0u : 2u) +
            (apple_sdk_root.empty() ? 0u : 2u) +
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
        if (!target_triple.empty()) {
            args_storage.emplace_back("-target");
            args_storage.emplace_back(target_triple);
        }
        if (!apple_sdk_root.empty()) {
            args_storage.emplace_back("-isysroot");
            args_storage.emplace_back(apple_sdk_root);
        }
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

        out.libclang_version = probe_libclang().version;

        {
            std::vector<std::string> dep_paths{};
            InclusionCollectCtx deps_ctx{};
            deps_ctx.deps = &dep_paths;
            clang_getInclusions(tu, collect_inclusion_visitor_, &deps_ctx);
            std::sort(dep_paths.begin(), dep_paths.end());
            dep_paths.erase(std::unique(dep_paths.begin(), dep_paths.end()), dep_paths.end());
            out.dependency_files.reserve(dep_paths.size());
            for (const auto& dep_path : dep_paths) {
                std::error_code stat_ec{};
                const std::filesystem::path dep_fs(dep_path);
                if (!std::filesystem::exists(dep_fs, stat_ec) || stat_ec) continue;
                ImportedDependencyFile dep{};
                dep.path = dep_path;
                dep.mtime_ns = static_cast<uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::filesystem::last_write_time(dep_fs, stat_ec).time_since_epoch()).count());
                if (stat_ec) dep.mtime_ns = 0;
                if (std::filesystem::is_regular_file(dep_fs, stat_ec) && !stat_ec) {
                    dep.size_bytes = static_cast<uint64_t>(std::filesystem::file_size(dep_fs, stat_ec));
                }
                if (stat_ec) dep.size_bytes = 0;
                out.dependency_files.push_back(std::move(dep));
            }
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
        ctx.out_globals = &out.globals;
        ctx.out_unions = &out.unions;
        ctx.out_structs = &out.structs;
        ctx.out_enums = &out.enums;
        ctx.out_typedefs = &out.typedefs;
        ctx.out_macros = &out.macros;
        ctx.tu = tu;

        const CXCursor root = clang_getTranslationUnitCursor(tu);
        clang_visitChildren(root, collect_decl_visitor_, &ctx);
        if (ctx.had_hard_error) {
            out.error = ImportErrorKind::kParseFailed;
            out.error_text = ctx.hard_error_text.empty()
                ? "failed to import C header due to anonymous-field flatten collision"
                : ctx.hard_error_text;
            clang_disposeTranslationUnit(tu);
            clang_disposeIndex(idx);
            return out;
        }

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

        // Evaluate object-like macro constants with a strict expression subset.
        std::unordered_map<std::string, size_t> object_macro_idx{};
        object_macro_idx.reserve(out.macros.size() * 2u + 1u);
        for (size_t i = 0; i < out.macros.size(); ++i) {
            if (out.macros[i].is_function_like) continue;
            if (out.macros[i].name.empty()) continue;
            object_macro_idx.emplace(out.macros[i].name, i);
        }

        enum class ObjEvalState : uint8_t { kUnvisited = 0, kVisiting, kDone };
        std::vector<ObjEvalState> obj_state(out.macros.size(), ObjEvalState::kUnvisited);

        auto object_macro_to_const_value =
            [&](const ImportedMacroDecl& mc, MacroConstEvalValue& outv) -> bool {
                switch (mc.const_kind) {
                    case ImportedConstKind::kInt: {
                        int64_t i64 = 0;
                        if (!parse_macro_int_value_(mc.value_text, i64)) return false;
                        outv.kind = MacroConstEvalKind::kInt;
                        outv.i64 = i64;
                        return true;
                    }
                    case ImportedConstKind::kFloat: {
                        double f64 = 0.0;
                        if (!parse_macro_float_value_(mc.value_text, f64)) return false;
                        outv.kind = MacroConstEvalKind::kFloat;
                        outv.f64 = f64;
                        return true;
                    }
                    case ImportedConstKind::kBool:
                        outv.kind = MacroConstEvalKind::kBool;
                        outv.b = (mc.value_text == "1" || mc.value_text == "true");
                        return true;
                    case ImportedConstKind::kChar: {
                        int64_t ch = 0;
                        std::string lit{};
                        if (!parse_macro_char_value_(mc.value_text, ch, lit)) return false;
                        outv.kind = MacroConstEvalKind::kChar;
                        outv.i64 = ch;
                        outv.text = std::move(lit);
                        return true;
                    }
                    case ImportedConstKind::kString:
                        outv.kind = MacroConstEvalKind::kString;
                        outv.text = mc.value_text;
                        return !outv.text.empty();
                    case ImportedConstKind::kNone:
                    default:
                        return false;
                }
            };

        std::function<bool(size_t, bool&)> eval_object_macro =
            [&](size_t idx, bool& cycle_detected) -> bool {
                if (idx >= out.macros.size()) return false;
                auto& mc = out.macros[idx];
                if (mc.is_function_like) return false;

                if (obj_state[idx] == ObjEvalState::kDone) {
                    return mc.const_kind != ImportedConstKind::kNone;
                }
                if (obj_state[idx] == ObjEvalState::kVisiting) {
                    cycle_detected = true;
                    set_macro_skip_(mc, ImportedMacroSkipKind::kChainCycleDetected,
                                    "unsupported object-like macro: reference cycle detected");
                    return false;
                }
                obj_state[idx] = ObjEvalState::kVisiting;

                if (mc.const_kind != ImportedConstKind::kNone) {
                    obj_state[idx] = ObjEvalState::kDone;
                    return true;
                }

                MacroConstExprParser_ parser(
                    mc.replacement_tokens,
                    [&](std::string_view ident) -> std::optional<MacroConstEvalValue> {
                        const auto it = object_macro_idx.find(std::string(ident));
                        if (it == object_macro_idx.end()) return std::nullopt;
                        bool local_cycle = false;
                        if (!eval_object_macro(it->second, local_cycle)) {
                            if (local_cycle) cycle_detected = true;
                            return std::nullopt;
                        }
                        MacroConstEvalValue v{};
                        if (!object_macro_to_const_value(out.macros[it->second], v)) return std::nullopt;
                        return v;
                    });

                MacroConstEvalValue expr_value{};
                if (!parser.parse(expr_value)) {
                    if (mc.skip_kind == ImportedMacroSkipKind::kNone) {
                        set_macro_skip_(
                            mc,
                            cycle_detected
                                ? ImportedMacroSkipKind::kChainCycleDetected
                                : ImportedMacroSkipKind::kConstExprUnsupported,
                            parser.error().empty()
                                ? "unsupported object-like macro constant expression"
                                : ("unsupported object-like macro constant expression: " + parser.error()));
                    }
                    obj_state[idx] = ObjEvalState::kDone;
                    return false;
                }

                ImportedConstKind out_kind = ImportedConstKind::kNone;
                std::string out_text{};
                if (!macro_const_to_imported_const_(expr_value, out_kind, out_text)) {
                    set_macro_skip_(
                        mc,
                        ImportedMacroSkipKind::kConstExprUnsupported,
                        "unsupported object-like macro constant expression result");
                    obj_state[idx] = ObjEvalState::kDone;
                    return false;
                }

                mc.const_kind = out_kind;
                mc.value_text = std::move(out_text);
                obj_state[idx] = ObjEvalState::kDone;
                return true;
            };

        for (size_t i = 0; i < out.macros.size(); ++i) {
            if (out.macros[i].is_function_like) continue;
            if (out.macros[i].const_kind != ImportedConstKind::kNone) continue;
            if (out.macros[i].replacement_tokens.empty()) continue;
            bool cycle = false;
            (void)eval_object_macro(i, cycle);
        }

        // Promote strictly-convertible function-like macros:
        //   - DirectAlias:   MACRO(a,b) -> callee(a,b)
        //   - IRWrapperCall: MACRO(y,x) -> callee(x,y) or cast forwarding
        //   - 1-step chain:  MACRO(...) -> OTHER_MACRO(...) -> callee(...)
        std::unordered_multimap<std::string, size_t> fn_idx_by_name{};
        fn_idx_by_name.reserve(collected_fns.size() * 2u + 1u);
        for (size_t i = 0; i < collected_fns.size(); ++i) {
            fn_idx_by_name.emplace(collected_fns[i].api.name, i);
        }

        auto extract_return_repr = [](std::string_view fn_type_repr, std::string& out_ret) -> bool {
            const size_t arrow = fn_type_repr.rfind("->");
            if (arrow == std::string_view::npos || arrow + 2 >= fn_type_repr.size()) return false;
            out_ret.assign(fn_type_repr.substr(arrow + 2));
            while (!out_ret.empty() && std::isspace(static_cast<unsigned char>(out_ret.front()))) {
                out_ret.erase(out_ret.begin());
            }
            while (!out_ret.empty() && std::isspace(static_cast<unsigned char>(out_ret.back()))) {
                out_ret.pop_back();
            }
            return !out_ret.empty();
        };

        auto populate_macro_from_function =
            [&](ImportedMacroDecl& mc, const CollectedFnDecl& selected) -> bool {
                if (mc.params.size() != selected.arg_type_reprs.size()) {
                    set_macro_skip_(mc, ImportedMacroSkipKind::kParamMismatch,
                                    "unsupported function-like macro: parameter count mismatch");
                    return false;
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
                    mc.promote_param_type_reprs[pi] = selected.arg_type_reprs[ai];
                    mc.promote_param_c_types[pi] = selected.api.c_arg_types[ai];
                    if (!call_arg.cast_prefix.empty()) has_cast = true;
                    if (pi != ai) identity_order = false;
                }
                if (invalid) {
                    set_macro_skip_(mc, ImportedMacroSkipKind::kInvalidForwarding,
                                    "unsupported function-like macro: each parameter must be forwarded exactly once");
                    return false;
                }
                for (size_t i = 0; i < param_use.size(); ++i) {
                    if (param_use[i] != 1u ||
                        mc.promote_param_type_reprs[i].empty() ||
                        mc.promote_param_c_types[i].empty()) {
                        set_macro_skip_(mc, ImportedMacroSkipKind::kInvalidForwarding,
                                        "unsupported function-like macro: each parameter must map to one callee argument");
                        return false;
                    }
                }

                mc.promote_callee_link_name = selected.api.link_name;
                mc.promote_c_return_type = selected.api.c_return_type;
                mc.promote_is_c_abi = selected.api.is_c_abi;
                mc.promote_is_variadic = selected.api.is_variadic;
                mc.promote_callconv = selected.api.callconv;
                mc.promote_type_repr = "def(";
                for (size_t i = 0; i < mc.promote_param_type_reprs.size(); ++i) {
                    if (i) mc.promote_type_repr += ", ";
                    mc.promote_type_repr += mc.promote_param_type_reprs[i];
                }
                mc.promote_type_repr += ") -> ";
                mc.promote_type_repr += selected.ret_type_repr;

                mc.promote_kind = (identity_order && !has_cast)
                    ? ImportedMacroPromoteKind::kDirectAlias
                    : ImportedMacroPromoteKind::kIRWrapperCall;
                return true;
            };

        auto populate_macro_from_macro =
            [&](ImportedMacroDecl& mc, const ImportedMacroDecl& callee_macro) -> bool {
                if (callee_macro.promote_kind == ImportedMacroPromoteKind::kNone ||
                    callee_macro.promote_type_repr.empty() ||
                    callee_macro.promote_param_type_reprs.size() != callee_macro.params.size() ||
                    callee_macro.promote_param_c_types.size() != callee_macro.params.size() ||
                    mc.promote_call_args.size() != callee_macro.params.size()) {
                    return false;
                }

                std::vector<ImportedMacroCallArg> composed_args{};
                composed_args.reserve(callee_macro.promote_call_args.size());
                for (const auto& inner : callee_macro.promote_call_args) {
                    if (inner.param_index < 0 ||
                        static_cast<size_t>(inner.param_index) >= mc.promote_call_args.size()) {
                        return false;
                    }
                    const auto& outer = mc.promote_call_args[static_cast<size_t>(inner.param_index)];
                    if (outer.param_index < 0 ||
                        static_cast<size_t>(outer.param_index) >= mc.params.size()) {
                        return false;
                    }
                    if (!inner.cast_prefix.empty() && !outer.cast_prefix.empty()) {
                        set_macro_skip_(mc, ImportedMacroSkipKind::kInvalidForwarding,
                                        "unsupported function-like macro: nested cast forwarding through macro chain");
                        return false;
                    }

                    ImportedMacroCallArg one{};
                    one.param_index = outer.param_index;
                    one.cast_prefix = !inner.cast_prefix.empty() ? inner.cast_prefix : outer.cast_prefix;
                    composed_args.push_back(std::move(one));
                }

                std::vector<uint32_t> param_use(mc.params.size(), 0u);
                mc.promote_param_type_reprs.assign(mc.params.size(), std::string{});
                mc.promote_param_c_types.assign(mc.params.size(), std::string{});
                for (size_t callee_param = 0; callee_param < callee_macro.params.size(); ++callee_param) {
                    const auto& outer = mc.promote_call_args[callee_param];
                    if (outer.param_index < 0 ||
                        static_cast<size_t>(outer.param_index) >= mc.params.size()) {
                        set_macro_skip_(mc, ImportedMacroSkipKind::kInvalidForwarding,
                                        "unsupported function-like macro: invalid argument forwarding in macro chain");
                        return false;
                    }
                    const size_t outer_param = static_cast<size_t>(outer.param_index);
                    ++param_use[outer_param];
                    if (param_use[outer_param] > 1u) {
                        set_macro_skip_(mc, ImportedMacroSkipKind::kInvalidForwarding,
                                        "unsupported function-like macro: each parameter must be forwarded exactly once");
                        return false;
                    }
                    mc.promote_param_type_reprs[outer_param] = callee_macro.promote_param_type_reprs[callee_param];
                    mc.promote_param_c_types[outer_param] = callee_macro.promote_param_c_types[callee_param];
                }
                for (size_t i = 0; i < param_use.size(); ++i) {
                    if (param_use[i] != 1u ||
                        mc.promote_param_type_reprs[i].empty() ||
                        mc.promote_param_c_types[i].empty()) {
                        set_macro_skip_(mc, ImportedMacroSkipKind::kInvalidForwarding,
                                        "unsupported function-like macro: each parameter must map to one callee argument");
                        return false;
                    }
                }

                std::string ret_repr{};
                if (!extract_return_repr(callee_macro.promote_type_repr, ret_repr)) {
                    set_macro_skip_(mc, ImportedMacroSkipKind::kInvalidForwarding,
                                    "unsupported function-like macro: invalid chained callee type representation");
                    return false;
                }

                bool has_cast = false;
                bool identity_order = true;
                for (size_t i = 0; i < composed_args.size(); ++i) {
                    if (!composed_args[i].cast_prefix.empty()) has_cast = true;
                    if (composed_args[i].param_index != static_cast<int32_t>(i)) identity_order = false;
                }

                mc.promote_call_args = std::move(composed_args);
                mc.promote_callee_name = callee_macro.promote_callee_name;
                mc.promote_callee_link_name = callee_macro.promote_callee_link_name;
                mc.promote_c_return_type = callee_macro.promote_c_return_type;
                mc.promote_is_c_abi = callee_macro.promote_is_c_abi;
                mc.promote_is_variadic = callee_macro.promote_is_variadic;
                mc.promote_callconv = callee_macro.promote_callconv;

                mc.promote_type_repr = "def(";
                for (size_t i = 0; i < mc.promote_param_type_reprs.size(); ++i) {
                    if (i) mc.promote_type_repr += ", ";
                    mc.promote_type_repr += mc.promote_param_type_reprs[i];
                }
                mc.promote_type_repr += ") -> ";
                mc.promote_type_repr += ret_repr;

                mc.promote_kind = (identity_order && !has_cast)
                    ? ImportedMacroPromoteKind::kDirectAlias
                    : ImportedMacroPromoteKind::kIRWrapperCall;
                return true;
            };

        for (auto& mc : out.macros) {
            if (!mc.is_function_like) continue;
            if (!mc.skip_reason.empty()) continue;
            mc.promote_kind = ImportedMacroPromoteKind::kNone;
            mc.promote_callee_link_name.clear();
            mc.promote_type_repr.clear();
            mc.promote_c_return_type.clear();
            mc.promote_param_type_reprs.clear();
            mc.promote_param_c_types.clear();
        }

        for (size_t round = 0; round < out.macros.size(); ++round) {
            bool progressed = false;
            for (auto& mc : out.macros) {
                if (!mc.is_function_like) continue;
                if (!mc.skip_reason.empty()) continue;
                if (mc.promote_kind != ImportedMacroPromoteKind::kNone) continue;
                if (mc.promote_callee_name.empty()) {
                    set_macro_skip_(mc, ImportedMacroSkipKind::kUnresolvedCallee,
                                    "unsupported function-like macro: unresolved callee");
                    continue;
                }

                bool promoted = false;
                const auto range = fn_idx_by_name.equal_range(mc.promote_callee_name);
                for (auto it = range.first; it != range.second; ++it) {
                    const auto& cand = collected_fns[it->second];
                    if (cand.api.is_variadic) continue;
                    if (cand.arg_type_reprs.size() != mc.promote_call_args.size()) continue;
                    if (cand.api.c_arg_types.size() != mc.promote_call_args.size()) continue;
                    if (populate_macro_from_function(mc, cand)) {
                        promoted = true;
                        break;
                    }
                }
                if (promoted) {
                    progressed = true;
                    continue;
                }

                for (const auto& callee_macro : out.macros) {
                    if (!callee_macro.is_function_like) continue;
                    if (callee_macro.name != mc.promote_callee_name) continue;
                    if (callee_macro.promote_kind == ImportedMacroPromoteKind::kNone) continue;
                    if (populate_macro_from_macro(mc, callee_macro)) {
                        promoted = true;
                        break;
                    }
                }
                if (promoted) {
                    progressed = true;
                }
            }
            if (!progressed) break;
        }

        std::unordered_map<std::string, size_t> fn_macro_idx{};
        fn_macro_idx.reserve(out.macros.size() * 2u + 1u);
        for (size_t i = 0; i < out.macros.size(); ++i) {
            const auto& mc = out.macros[i];
            if (!mc.is_function_like || mc.name.empty()) continue;
            fn_macro_idx.emplace(mc.name, i);
        }

        auto unresolved_macro_chain_cycle = [&](size_t root_idx) -> bool {
            std::unordered_set<size_t> visiting{};
            std::unordered_set<size_t> visited{};
            std::function<bool(size_t)> dfs = [&](size_t cur) -> bool {
                if (visiting.find(cur) != visiting.end()) return true;
                if (visited.find(cur) != visited.end()) return false;
                visited.insert(cur);
                visiting.insert(cur);

                const auto& mc = out.macros[cur];
                if (!mc.promote_callee_name.empty()) {
                    if (const auto it = fn_macro_idx.find(mc.promote_callee_name);
                        it != fn_macro_idx.end()) {
                        const size_t next = it->second;
                        const auto& next_mc = out.macros[next];
                        const bool unresolved =
                            next_mc.promote_kind == ImportedMacroPromoteKind::kNone &&
                            next_mc.skip_reason.empty();
                        if (unresolved && dfs(next)) return true;
                    }
                }
                visiting.erase(cur);
                return false;
            };
            return dfs(root_idx);
        };

        for (auto& mc : out.macros) {
            if (!mc.is_function_like) continue;
            if (!mc.skip_reason.empty()) continue;
            if (mc.promote_kind == ImportedMacroPromoteKind::kNone) {
                bool cycle = false;
                if (const auto it = fn_macro_idx.find(mc.name); it != fn_macro_idx.end()) {
                    cycle = unresolved_macro_chain_cycle(it->second);
                }
                set_macro_skip_(
                    mc,
                    cycle ? ImportedMacroSkipKind::kChainCycleDetected
                          : ImportedMacroSkipKind::kUnresolvableChain,
                    cycle
                        ? "unsupported function-like macro: cyclic macro-chain dependency detected"
                        : "unsupported function-like macro: callee function/macro chain not resolvable");
            }
        }

        out.functions.reserve(collected_fns.size());
        for (auto& f : collected_fns) {
            out.functions.push_back(std::move(f.api));
        }

        out.coverage.total_function_decls = static_cast<uint32_t>(collected_fns.size());
        out.coverage.imported_function_decls = static_cast<uint32_t>(out.functions.size());
        out.coverage.total_global_decls = static_cast<uint32_t>(out.globals.size());
        out.coverage.imported_global_decls = static_cast<uint32_t>(out.globals.size());
        out.coverage.total_type_decls = static_cast<uint32_t>(
            out.structs.size() + out.unions.size() + out.enums.size() + out.typedefs.size());
        out.coverage.imported_type_decls = out.coverage.total_type_decls;

        uint32_t enum_const_count = 0;
        for (const auto& e : out.enums) {
            enum_const_count += static_cast<uint32_t>(e.constants.size());
        }
        out.coverage.total_const_decls = enum_const_count;
        out.coverage.imported_const_decls = enum_const_count;

        for (const auto& mc : out.macros) {
            if (!mc.is_function_like) {
                if (mc.const_kind != ImportedConstKind::kNone) {
                    ++out.coverage.total_const_decls;
                    ++out.coverage.imported_const_decls;
                }
                continue;
            }

            ++out.coverage.total_function_macros;
            if (mc.promote_kind == ImportedMacroPromoteKind::kDirectAlias ||
                mc.promote_kind == ImportedMacroPromoteKind::kIRWrapperCall) {
                ++out.coverage.promoted_function_macros;
            } else {
                ++out.coverage.skipped_function_macros;
                if (!mc.skip_reason.empty()) {
                    out.coverage.skipped_reasons.push_back(mc.name + ": " + mc.skip_reason);
                }
                out.coverage.skipped_reason_codes.push_back(
                    mc.name + ": " + imported_macro_skip_code_text(mc.skip_kind));
            }
        }

        clang_disposeTranslationUnit(tu);
        clang_disposeIndex(idx);
        out.error = ImportErrorKind::kNone;
        return out;
#endif
    }

} // namespace parus::cimport
