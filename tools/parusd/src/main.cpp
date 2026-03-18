#include <parus/ast/Nodes.hpp>
#include <parus/cap/CapabilityCheck.hpp>
#include <parus/cimport/CHeaderImport.hpp>
#include <parus/cimport/CImportPayload.hpp>
#include <parus/cimport/TypeReprNormalize.hpp>
#include <parus/cimport/ToolchainResolver.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/diag/Render.hpp>
#include <parus/lex/Lexer.hpp>
#include <parus/macro/Expander.hpp>
#include <parus/os/File.hpp>
#include <parus/parse/IncrementalParse.hpp>
#include <parus/parse/Parser.hpp>
#include <parus/passes/Passes.hpp>
#include <parus/text/SourceManager.hpp>
#include <parus/type/TypeResolve.hpp>
#include <parus/ty/TypePool.hpp>
#include <parus/tyck/TypeCheck.hpp>

#if defined(PARUSD_ENABLE_LEI) && PARUSD_ENABLE_LEI
#include <lei/diag/DiagCode.hpp>
#include <lei/eval/Evaluator.hpp>
#include <lei/graph/BuildGraph.hpp>
#include <lei/parse/Parser.hpp>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <optional>
#include <set>
#include <sstream>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifndef PARUSD_ENABLE_LEI
#define PARUSD_ENABLE_LEI 0
#endif

namespace {

    struct JsonValue {
        enum class Kind : uint8_t {
            kNull,
            kBool,
            kNumber,
            kString,
            kArray,
            kObject,
        };

        Kind kind = Kind::kNull;
        bool bool_v = false;
        double number_v = 0.0;
        std::string string_v{};
        std::vector<JsonValue> array_v{};
        std::unordered_map<std::string, JsonValue> object_v{};
    };

    class JsonParser {
    public:
        explicit JsonParser(std::string_view src) : src_(src) {}

        bool parse(JsonValue& out) {
            skip_ws_();
            if (!parse_value_(out)) return false;
            skip_ws_();
            return ok_ && pos_ == src_.size();
        }

    private:
        bool parse_value_(JsonValue& out) {
            skip_ws_();
            if (pos_ >= src_.size()) return fail_();

            const char ch = src_[pos_];
            if (ch == 'n') return parse_null_(out);
            if (ch == 't' || ch == 'f') return parse_bool_(out);
            if (ch == '"') return parse_string_value_(out);
            if (ch == '[') return parse_array_(out);
            if (ch == '{') return parse_object_(out);
            if (ch == '-' || (ch >= '0' && ch <= '9')) return parse_number_(out);
            return fail_();
        }

        bool parse_null_(JsonValue& out) {
            if (!consume_literal_("null")) return false;
            out = JsonValue{};
            out.kind = JsonValue::Kind::kNull;
            return true;
        }

        bool parse_bool_(JsonValue& out) {
            if (src_.substr(pos_, 4) == "true") {
                pos_ += 4;
                out = JsonValue{};
                out.kind = JsonValue::Kind::kBool;
                out.bool_v = true;
                return true;
            }
            if (src_.substr(pos_, 5) == "false") {
                pos_ += 5;
                out = JsonValue{};
                out.kind = JsonValue::Kind::kBool;
                out.bool_v = false;
                return true;
            }
            return fail_();
        }

        bool parse_number_(JsonValue& out) {
            const size_t begin = pos_;
            if (src_[pos_] == '-') ++pos_;

            if (pos_ >= src_.size()) return fail_();
            if (src_[pos_] == '0') {
                ++pos_;
            } else {
                if (!std::isdigit(static_cast<unsigned char>(src_[pos_]))) return fail_();
                while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
            }

            if (pos_ < src_.size() && src_[pos_] == '.') {
                ++pos_;
                if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) return fail_();
                while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
            }

            if (pos_ < src_.size() && (src_[pos_] == 'e' || src_[pos_] == 'E')) {
                ++pos_;
                if (pos_ < src_.size() && (src_[pos_] == '+' || src_[pos_] == '-')) ++pos_;
                if (pos_ >= src_.size() || !std::isdigit(static_cast<unsigned char>(src_[pos_]))) return fail_();
                while (pos_ < src_.size() && std::isdigit(static_cast<unsigned char>(src_[pos_]))) ++pos_;
            }

            const std::string text(src_.substr(begin, pos_ - begin));
            char* endp = nullptr;
            const double v = std::strtod(text.c_str(), &endp);
            if (endp == text.c_str() || (endp != nullptr && *endp != '\0')) return fail_();

            out = JsonValue{};
            out.kind = JsonValue::Kind::kNumber;
            out.number_v = v;
            return true;
        }

        bool parse_string_value_(JsonValue& out) {
            std::string s;
            if (!parse_string_(s)) return false;
            out = JsonValue{};
            out.kind = JsonValue::Kind::kString;
            out.string_v = std::move(s);
            return true;
        }

        static void append_utf8_(std::string& out, uint32_t cp) {
            if (cp <= 0x7F) {
                out.push_back(static_cast<char>(cp));
                return;
            }
            if (cp <= 0x7FF) {
                out.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                return;
            }
            if (cp <= 0xFFFF) {
                out.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
                out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                return;
            }
            out.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }

        static int hex_value_(char ch) {
            if (ch >= '0' && ch <= '9') return ch - '0';
            if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
            if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
            return -1;
        }

        bool parse_string_(std::string& out) {
            if (pos_ >= src_.size() || src_[pos_] != '"') return fail_();
            ++pos_;

            while (pos_ < src_.size()) {
                const char ch = src_[pos_++];
                if (ch == '"') return true;
                if (ch == '\\') {
                    if (pos_ >= src_.size()) return fail_();
                    const char esc = src_[pos_++];
                    switch (esc) {
                        case '"': out.push_back('"'); break;
                        case '\\': out.push_back('\\'); break;
                        case '/': out.push_back('/'); break;
                        case 'b': out.push_back('\b'); break;
                        case 'f': out.push_back('\f'); break;
                        case 'n': out.push_back('\n'); break;
                        case 'r': out.push_back('\r'); break;
                        case 't': out.push_back('\t'); break;
                        case 'u': {
                            if (pos_ + 4 > src_.size()) return fail_();
                            uint32_t cp = 0;
                            for (int i = 0; i < 4; ++i) {
                                const int hv = hex_value_(src_[pos_ + i]);
                                if (hv < 0) return fail_();
                                cp = (cp << 4) | static_cast<uint32_t>(hv);
                            }
                            pos_ += 4;
                            append_utf8_(out, cp);
                            break;
                        }
                        default:
                            return fail_();
                    }
                    continue;
                }
                out.push_back(ch);
            }
            return fail_();
        }

        bool parse_array_(JsonValue& out) {
            if (pos_ >= src_.size() || src_[pos_] != '[') return fail_();
            ++pos_;

            out = JsonValue{};
            out.kind = JsonValue::Kind::kArray;

            skip_ws_();
            if (pos_ < src_.size() && src_[pos_] == ']') {
                ++pos_;
                return true;
            }

            while (pos_ < src_.size()) {
                JsonValue elem{};
                if (!parse_value_(elem)) return false;
                out.array_v.push_back(std::move(elem));

                skip_ws_();
                if (pos_ >= src_.size()) return fail_();
                if (src_[pos_] == ',') {
                    ++pos_;
                    skip_ws_();
                    continue;
                }
                if (src_[pos_] == ']') {
                    ++pos_;
                    return true;
                }
                return fail_();
            }
            return fail_();
        }

        bool parse_object_(JsonValue& out) {
            if (pos_ >= src_.size() || src_[pos_] != '{') return fail_();
            ++pos_;

            out = JsonValue{};
            out.kind = JsonValue::Kind::kObject;

            skip_ws_();
            if (pos_ < src_.size() && src_[pos_] == '}') {
                ++pos_;
                return true;
            }

            while (pos_ < src_.size()) {
                std::string key;
                if (!parse_string_(key)) return false;

                skip_ws_();
                if (pos_ >= src_.size() || src_[pos_] != ':') return fail_();
                ++pos_;

                JsonValue val{};
                if (!parse_value_(val)) return false;
                out.object_v.emplace(std::move(key), std::move(val));

                skip_ws_();
                if (pos_ >= src_.size()) return fail_();
                if (src_[pos_] == ',') {
                    ++pos_;
                    skip_ws_();
                    continue;
                }
                if (src_[pos_] == '}') {
                    ++pos_;
                    return true;
                }
                return fail_();
            }
            return fail_();
        }

        bool consume_literal_(std::string_view lit) {
            if (src_.substr(pos_, lit.size()) != lit) return fail_();
            pos_ += lit.size();
            return true;
        }

        void skip_ws_() {
            while (pos_ < src_.size()) {
                const unsigned char ch = static_cast<unsigned char>(src_[pos_]);
                if (!std::isspace(ch)) break;
                ++pos_;
            }
        }

        bool fail_() {
            ok_ = false;
            return false;
        }

        std::string_view src_{};
        size_t pos_ = 0;
        bool ok_ = true;
    };

    const JsonValue* obj_get_(const JsonValue& obj, std::string_view key) {
        if (obj.kind != JsonValue::Kind::kObject) return nullptr;
        const auto it = obj.object_v.find(std::string(key));
        if (it == obj.object_v.end()) return nullptr;
        return &it->second;
    }

    std::optional<std::string_view> as_string_(const JsonValue* v) {
        if (v == nullptr || v->kind != JsonValue::Kind::kString) return std::nullopt;
        return v->string_v;
    }

    std::optional<int64_t> as_i64_(const JsonValue* v) {
        if (v == nullptr || v->kind != JsonValue::Kind::kNumber) return std::nullopt;
        return static_cast<int64_t>(v->number_v);
    }

    std::optional<bool> as_bool_(const JsonValue* v) {
        if (v == nullptr || v->kind != JsonValue::Kind::kBool) return std::nullopt;
        return v->bool_v;
    }

    std::string trim_(std::string_view s) {
        size_t b = 0;
        while (b < s.size() && std::isspace(static_cast<unsigned char>(s[b]))) ++b;
        size_t e = s.size();
        while (e > b && std::isspace(static_cast<unsigned char>(s[e - 1]))) --e;
        return std::string(s.substr(b, e - b));
    }

    bool iequals_(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            const unsigned char ca = static_cast<unsigned char>(a[i]);
            const unsigned char cb = static_cast<unsigned char>(b[i]);
            if (std::tolower(ca) != std::tolower(cb)) return false;
        }
        return true;
    }

    bool read_lsp_message_(std::istream& in, std::string& out_payload) {
        out_payload.clear();
        std::string line;
        size_t content_length = 0;
        bool has_content_length = false;

        while (std::getline(in, line)) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.empty()) break;

            const size_t colon = line.find(':');
            if (colon == std::string::npos) continue;

            const std::string key = trim_(std::string_view(line).substr(0, colon));
            const std::string value = trim_(std::string_view(line).substr(colon + 1));
            if (iequals_(key, "Content-Length")) {
                try {
                    content_length = static_cast<size_t>(std::stoull(value));
                    has_content_length = true;
                } catch (...) {
                    return false;
                }
            }
        }

        if (!has_content_length) return false;

        out_payload.resize(content_length);
        in.read(out_payload.data(), static_cast<std::streamsize>(content_length));
        return static_cast<size_t>(in.gcount()) == content_length;
    }

    std::string json_escape_(std::string_view s) {
        std::string out;
        out.reserve(s.size() + 8);
        for (const char ch : s) {
            switch (ch) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default:
                    if (static_cast<unsigned char>(ch) < 0x20) {
                        char buf[7]{};
                        std::snprintf(buf, sizeof(buf), "\\u%04X", static_cast<unsigned char>(ch));
                        out += buf;
                    } else {
                        out.push_back(ch);
                    }
                    break;
            }
        }
        return out;
    }

    std::string json_value_to_text_(const JsonValue& v) {
        switch (v.kind) {
            case JsonValue::Kind::kNull:
                return "null";
            case JsonValue::Kind::kBool:
                return v.bool_v ? "true" : "false";
            case JsonValue::Kind::kNumber: {
                std::string s = std::to_string(v.number_v);
                while (s.size() > 1 && s.back() == '0') s.pop_back();
                if (!s.empty() && s.back() == '.') s.pop_back();
                return s;
            }
            case JsonValue::Kind::kString:
                return "\"" + json_escape_(v.string_v) + "\"";
            case JsonValue::Kind::kArray: {
                std::string out = "[";
                for (size_t i = 0; i < v.array_v.size(); ++i) {
                    if (i != 0) out += ",";
                    out += json_value_to_text_(v.array_v[i]);
                }
                out += "]";
                return out;
            }
            case JsonValue::Kind::kObject: {
                std::string out = "{";
                bool first = true;
                for (const auto& [k, val] : v.object_v) {
                    if (!first) out += ",";
                    first = false;
                    out += "\"" + json_escape_(k) + "\":" + json_value_to_text_(val);
                }
                out += "}";
                return out;
            }
        }
        return "null";
    }

    void write_lsp_message_(std::ostream& out, std::string_view payload) {
        out << "Content-Length: " << payload.size() << "\r\n\r\n";
        out.write(payload.data(), static_cast<std::streamsize>(payload.size()));
        out.flush();
    }

    struct LspDiag {
        uint32_t start_line = 0;
        uint32_t start_character = 0;
        uint32_t end_line = 0;
        uint32_t end_character = 0;
        int severity = 1;
        std::string code{};
        std::string message{};
    };

    enum class DocLang : uint8_t {
        kParus,
        kLei,
        kUnknown,
    };

    enum class SemTokenType : uint32_t {
        kNamespace = 0,
        kType,
        kClass,
        kFunction,
        kParameter,
        kVariable,
        kProperty,
        kKeyword,
        kString,
        kNumber,
        kOperator,
        kDecorator,
    };

    struct SemToken {
        uint32_t line = 0;
        uint32_t start_character = 0;
        uint32_t length = 0;
        uint32_t token_type = 0;
        uint32_t token_modifiers = 0;
    };

    struct SemClass {
        uint32_t token_type = static_cast<uint32_t>(SemTokenType::kVariable);
        uint32_t token_modifiers = 0;
    };

    struct LspLocation {
        std::string uri{};
        uint32_t start_line = 0;
        uint32_t start_character = 0;
        uint32_t end_line = 0;
        uint32_t end_character = 0;
    };

    struct CompletionEntry {
        std::string label{};
        uint32_t kind = 14; // CompletionItemKind::Keyword
        std::string detail{};
    };

    struct DefinitionBinding {
        uint32_t use_lo = 0;
        uint32_t use_hi = 0;
        std::vector<LspLocation> targets{};
    };

    struct AnalysisResult {
        std::vector<LspDiag> diagnostics{};
        std::vector<SemToken> semantic_tokens{};
        std::vector<CompletionEntry> completion_items{};
        std::vector<DefinitionBinding> definition_bindings{};
        std::unordered_map<std::string, std::vector<LspLocation>> top_level_definitions{};
        parus::parse::ReparseMode parse_mode = parus::parse::ReparseMode::kNone;
    };

    static constexpr uint32_t kSemModDeclaration = 1u << 0;
    static constexpr uint32_t kSemModReadonly = 1u << 1;
    static constexpr uint32_t kSemModStatic = 1u << 2;

    constexpr std::array<std::string_view, 12> kSemTokenTypeNames = {
        "namespace",
        "type",
        "class",
        "function",
        "parameter",
        "variable",
        "property",
        "keyword",
        "string",
        "number",
        "operator",
        "decorator",
    };

    constexpr std::array<std::string_view, 3> kSemTokenModifierNames = {
        "declaration",
        "readonly",
        "static",
    };

    constexpr std::array<std::string_view, 50> kParusCompletionKeywords = {
        "def", "macro", "struct", "proto", "class", "actor", "acts",
        "let", "set", "mut", "static", "const", "if", "elif", "else",
        "while", "do", "loop", "in", "return", "break", "continue",
        "true", "false", "null", "and", "or", "not", "xor",
        "export", "extern", "layout", "align", "switch", "case", "default",
        "import", "module", "use", "nest", "as", "with", "require",
        "commit", "recast", "manual", "init", "deinit", "draft"
    };

    constexpr std::array<std::string_view, 20> kLeiCompletionKeywords = {
        "import", "from", "export", "proto", "plan", "let", "var",
        "def", "assert", "if", "else", "true", "false", "int", "float",
        "string", "bool", "return", "for", "in"
    };

    struct Position {
        uint32_t line = 0;
        uint32_t character = 0;
    };

    struct Range {
        Position start{};
        Position end{};
    };

    struct TextChange {
        bool has_range = false;
        Range range{};
        std::string text{};
    };

    struct DocumentState {
        std::string text{};
        int64_t version = 0;
        uint64_t revision = 0;
        DocLang lang = DocLang::kUnknown;

        std::vector<parus::parse::EditWindow> pending_edits{};

        parus::parse::IncrementalParserSession parse_session{};
        bool parse_ready = false;

        struct AnalysisCache {
            uint64_t revision = 0;
            bool valid = false;
            std::vector<LspDiag> diagnostics{};
            std::vector<SemToken> semantic_tokens{};
            std::vector<CompletionEntry> completion_items{};
            std::vector<DefinitionBinding> definition_bindings{};
            std::unordered_map<std::string, std::vector<LspLocation>> top_level_definitions{};
        } analysis{};
    };

    struct ServerMacroConfig {
        parus::macro::ExpansionBudget budget = parus::macro::default_budget_jit();
        parus::ParserFeatureFlags parser_features{};
        std::vector<std::string> warnings{};
    };

    struct ServerCImportConfig {
        std::vector<std::string> include_dirs{};
        std::vector<std::string> isystem_dirs{};
        std::vector<std::string> defines{};
        std::vector<std::string> undefines{};
        std::vector<std::string> forced_includes{};
        std::vector<std::string> imacros{};
        std::vector<std::string> warnings{};
    };

    std::string lower_ascii_(std::string s) {
        std::transform(s.begin(), s.end(), s.begin(), [](unsigned char ch) {
            return static_cast<char>(std::tolower(ch));
        });
        return s;
    }

    bool ends_with_(std::string_view text, std::string_view suffix) {
        if (suffix.size() > text.size()) return false;
        return text.substr(text.size() - suffix.size()) == suffix;
    }

    DocLang doc_lang_from_uri_(std::string_view uri) {
        std::string u(uri);
        if (const auto pos = u.find_first_of("?#"); pos != std::string::npos) {
            u.resize(pos);
        }
        u = lower_ascii_(std::move(u));
        if (ends_with_(u, ".pr") || ends_with_(u, ".parus")) return DocLang::kParus;
        if (ends_with_(u, ".lei")) return DocLang::kLei;
        return DocLang::kUnknown;
    }

    int hex_nibble_(char ch) {
        if (ch >= '0' && ch <= '9') return ch - '0';
        if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
        if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
        return -1;
    }

    bool percent_decode_(std::string_view input, std::string& out) {
        out.clear();
        out.reserve(input.size());
        for (size_t i = 0; i < input.size(); ++i) {
            const char ch = input[i];
            if (ch != '%') {
                out.push_back(ch);
                continue;
            }
            if (i + 2 >= input.size()) return false;
            const int hi = hex_nibble_(input[i + 1]);
            const int lo = hex_nibble_(input[i + 2]);
            if (hi < 0 || lo < 0) return false;
            out.push_back(static_cast<char>((hi << 4) | lo));
            i += 2;
        }
        return true;
    }

    std::optional<std::string> uri_to_file_path_(std::string_view uri) {
        if (!uri.starts_with("file://")) return std::nullopt;

        std::string rest(uri.substr(7));
        if (rest.empty()) return std::nullopt;

        if (rest[0] != '/') {
            const auto slash = rest.find('/');
            if (slash == std::string::npos) return std::nullopt;
            const std::string_view host(rest.data(), slash);
            if (!host.empty() && host != "localhost") return std::nullopt;
            rest.erase(0, slash);
        }

        std::string decoded{};
        if (!percent_decode_(rest, decoded)) return std::nullopt;

#if defined(_WIN32)
        if (decoded.size() >= 3
            && decoded[0] == '/'
            && std::isalpha(static_cast<unsigned char>(decoded[1]))
            && decoded[2] == ':') {
            decoded.erase(decoded.begin());
        }
        std::replace(decoded.begin(), decoded.end(), '/', '\\');
#endif
        return decoded;
    }

    std::string file_path_to_uri_(std::string_view raw_path) {
        namespace fs = std::filesystem;
        std::error_code ec{};
        fs::path p(raw_path);
        if (p.is_relative()) {
            p = fs::absolute(p, ec);
            if (ec) {
                ec.clear();
                p = fs::path(raw_path);
            }
        }
        fs::path canon = fs::weakly_canonical(p, ec);
        if (ec || canon.empty()) {
            ec.clear();
            canon = p.lexically_normal();
        }
        std::string norm = canon.string();
#if defined(_WIN32)
        std::replace(norm.begin(), norm.end(), '\\', '/');
        if (!norm.empty() && norm[0] != '/') {
            norm.insert(norm.begin(), '/');
        }
#endif
        std::string encoded{};
        encoded.reserve(norm.size() + 16);
        auto append_hex = [&](unsigned char c) {
            static constexpr char kHex[] = "0123456789ABCDEF";
            encoded.push_back('%');
            encoded.push_back(kHex[(c >> 4) & 0x0F]);
            encoded.push_back(kHex[c & 0x0F]);
        };
        for (const unsigned char c : norm) {
            const bool safe =
                (c >= 'a' && c <= 'z') ||
                (c >= 'A' && c <= 'Z') ||
                (c >= '0' && c <= '9') ||
                c == '/' || c == '_' || c == '-' || c == '.' || c == '~';
            if (safe) {
                encoded.push_back(static_cast<char>(c));
            } else {
                append_hex(c);
            }
        }
        return "file://" + encoded;
    }

    std::string normalize_host_path_(std::string_view raw_path) {
        namespace fs = std::filesystem;
        const fs::path p(raw_path);
        std::error_code ec{};
        const fs::path canonical = fs::weakly_canonical(p, ec);
        if (!ec) return canonical.string();
        return p.lexically_normal().string();
    }

    bool decode_utf8_code_point_(std::string_view text, size_t off, uint32_t& cp, size_t& len) {
        if (off >= text.size()) return false;

        const auto b0 = static_cast<unsigned char>(text[off]);
        auto is_cont = [](unsigned char b) -> bool { return (b & 0xC0) == 0x80; };

        if (b0 < 0x80) {
            cp = b0;
            len = 1;
            return true;
        }

        if (b0 >= 0xC2 && b0 <= 0xDF) {
            if (off + 1 >= text.size()) return false;
            const auto b1 = static_cast<unsigned char>(text[off + 1]);
            if (!is_cont(b1)) return false;
            cp = (static_cast<uint32_t>(b0 & 0x1F) << 6) | static_cast<uint32_t>(b1 & 0x3F);
            len = 2;
            return true;
        }

        if (b0 >= 0xE0 && b0 <= 0xEF) {
            if (off + 2 >= text.size()) return false;
            const auto b1 = static_cast<unsigned char>(text[off + 1]);
            const auto b2 = static_cast<unsigned char>(text[off + 2]);
            if (!is_cont(b1) || !is_cont(b2)) return false;
            if (b0 == 0xE0 && b1 < 0xA0) return false;
            if (b0 == 0xED && b1 >= 0xA0) return false;
            cp = (static_cast<uint32_t>(b0 & 0x0F) << 12)
               | (static_cast<uint32_t>(b1 & 0x3F) << 6)
               | static_cast<uint32_t>(b2 & 0x3F);
            len = 3;
            return true;
        }

        if (b0 >= 0xF0 && b0 <= 0xF4) {
            if (off + 3 >= text.size()) return false;
            const auto b1 = static_cast<unsigned char>(text[off + 1]);
            const auto b2 = static_cast<unsigned char>(text[off + 2]);
            const auto b3 = static_cast<unsigned char>(text[off + 3]);
            if (!is_cont(b1) || !is_cont(b2) || !is_cont(b3)) return false;
            if (b0 == 0xF0 && b1 < 0x90) return false;
            if (b0 == 0xF4 && b1 > 0x8F) return false;
            cp = (static_cast<uint32_t>(b0 & 0x07) << 18)
               | (static_cast<uint32_t>(b1 & 0x3F) << 12)
               | (static_cast<uint32_t>(b2 & 0x3F) << 6)
               | static_cast<uint32_t>(b3 & 0x3F);
            len = 4;
            return true;
        }

        return false;
    }

    uint32_t utf16_units_for_code_point_(uint32_t cp) {
        return (cp > 0xFFFF) ? 2u : 1u;
    }

    size_t byte_offset_from_position_(std::string_view text, const Position& p) {
        size_t off = 0;
        uint32_t line = 0;
        while (off < text.size() && line < p.line) {
            if (text[off] == '\n') ++line;
            ++off;
        }

        size_t col_off = off;
        uint32_t col_utf16 = 0;
        while (col_off < text.size() && text[col_off] != '\n' && col_utf16 < p.character) {
            uint32_t cp = 0;
            size_t len = 1;
            if (!decode_utf8_code_point_(text, col_off, cp, len)) {
                cp = static_cast<unsigned char>(text[col_off]);
                len = 1;
            }

            const uint32_t units = utf16_units_for_code_point_(cp);
            if (col_utf16 + units > p.character) {
                break;
            }

            col_off += len;
            col_utf16 += units;
        }
        return col_off;
    }

    bool parse_position_(const JsonValue* node, Position& out) {
        if (node == nullptr || node->kind != JsonValue::Kind::kObject) return false;
        const auto line = as_i64_(obj_get_(*node, "line"));
        const auto ch = as_i64_(obj_get_(*node, "character"));
        if (!line.has_value() || !ch.has_value() || *line < 0 || *ch < 0) return false;
        out.line = static_cast<uint32_t>(*line);
        out.character = static_cast<uint32_t>(*ch);
        return true;
    }

    bool parse_range_(const JsonValue* node, Range& out) {
        if (node == nullptr || node->kind != JsonValue::Kind::kObject) return false;
        return parse_position_(obj_get_(*node, "start"), out.start)
            && parse_position_(obj_get_(*node, "end"), out.end);
    }

    bool parse_text_change_(const JsonValue& node, TextChange& out) {
        if (node.kind != JsonValue::Kind::kObject) return false;
        const auto text = as_string_(obj_get_(node, "text"));
        if (!text.has_value()) return false;
        out.text = std::string(*text);

        Range r{};
        if (parse_range_(obj_get_(node, "range"), r)) {
            out.has_range = true;
            out.range = r;
        }
        return true;
    }

    ServerMacroConfig parse_macro_config_from_initialize_(const JsonValue* params) {
        ServerMacroConfig cfg{};
        if (params == nullptr || params->kind != JsonValue::Kind::kObject) return cfg;

        const auto* init_opts = obj_get_(*params, "initializationOptions");
        if (init_opts == nullptr || init_opts->kind != JsonValue::Kind::kObject) return cfg;

        const JsonValue* root = init_opts;
        if (const auto* parus_cfg = obj_get_(*init_opts, "parus");
            parus_cfg != nullptr && parus_cfg->kind == JsonValue::Kind::kObject) {
            root = parus_cfg;
        }

        if (const auto* budget_cfg = obj_get_(*root, "macroBudget");
            budget_cfg != nullptr && budget_cfg->kind == JsonValue::Kind::kObject) {
            const auto set_budget_field = [&](const char* key, uint32_t& out_field) {
                const auto v = as_i64_(obj_get_(*budget_cfg, key));
                if (!v.has_value()) return;
                if (*v <= 0) {
                    out_field = 0;
                    return;
                }
                if (static_cast<uint64_t>(*v) > std::numeric_limits<uint32_t>::max()) {
                    out_field = std::numeric_limits<uint32_t>::max();
                    return;
                }
                out_field = static_cast<uint32_t>(*v);
            };
            set_budget_field("maxDepth", cfg.budget.max_depth);
            set_budget_field("maxSteps", cfg.budget.max_steps);
            set_budget_field("maxOutputTokens", cfg.budget.max_output_tokens);
        }

        const auto before = cfg.budget;
        const auto clamped = parus::macro::clamp_budget(cfg.budget);
        if (clamped.depth) {
            cfg.warnings.push_back(
                "macro budget clamped: maxDepth "
                + std::to_string(before.max_depth)
                + " -> "
                + std::to_string(cfg.budget.max_depth));
        }
        if (clamped.steps) {
            cfg.warnings.push_back(
                "macro budget clamped: maxSteps "
                + std::to_string(before.max_steps)
                + " -> "
                + std::to_string(cfg.budget.max_steps));
        }
        if (clamped.output_tokens) {
            cfg.warnings.push_back(
                "macro budget clamped: maxOutputTokens "
                + std::to_string(before.max_output_tokens)
                + " -> "
                + std::to_string(cfg.budget.max_output_tokens));
        }
        return cfg;
    }

    void parse_string_array_field_(const std::unordered_map<std::string, JsonValue>& obj,
                                   std::string_view key,
                                   std::vector<std::string>& out,
                                   std::vector<std::string>& warnings,
                                   std::string_view warning_prefix) {
        const auto it = obj.find(std::string(key));
        if (it == obj.end()) return;
        const JsonValue* node = &it->second;
        if (node->kind != JsonValue::Kind::kArray) {
            warnings.push_back(std::string(warning_prefix) + std::string(key) + " must be array<string>");
            return;
        }
        for (size_t i = 0; i < node->array_v.size(); ++i) {
            const auto sv = as_string_(&node->array_v[i]);
            if (!sv.has_value()) {
                warnings.push_back(std::string(warning_prefix) + std::string(key) + "[" + std::to_string(i)
                                   + "] must be string");
                continue;
            }
            out.push_back(std::string(*sv));
        }
    }

    ServerCImportConfig parse_cimport_config_from_initialize_(const JsonValue* params) {
        ServerCImportConfig cfg{};
        if (params == nullptr || params->kind != JsonValue::Kind::kObject) return cfg;
        const auto* init_opts = obj_get_(*params, "initializationOptions");
        if (init_opts == nullptr || init_opts->kind != JsonValue::Kind::kObject) return cfg;

        const JsonValue* root = init_opts;
        if (const auto* parus_cfg = obj_get_(*init_opts, "parus");
            parus_cfg != nullptr && parus_cfg->kind == JsonValue::Kind::kObject) {
            root = parus_cfg;
        }
        const auto* cimport = obj_get_(*root, "cimport");
        if (cimport == nullptr || cimport->kind != JsonValue::Kind::kObject) return cfg;
        const auto* cobj = &cimport->object_v;

        constexpr std::string_view kWarnPrefix = "initializationOptions.parus.cimport.";
        parse_string_array_field_(*cobj, "includeDirs", cfg.include_dirs, cfg.warnings, kWarnPrefix);
        parse_string_array_field_(*cobj, "isystemDirs", cfg.isystem_dirs, cfg.warnings, kWarnPrefix);
        parse_string_array_field_(*cobj, "defines", cfg.defines, cfg.warnings, kWarnPrefix);
        parse_string_array_field_(*cobj, "undefines", cfg.undefines, cfg.warnings, kWarnPrefix);
        parse_string_array_field_(*cobj, "forcedIncludes", cfg.forced_includes, cfg.warnings, kWarnPrefix);
        parse_string_array_field_(*cobj, "imacros", cfg.imacros, cfg.warnings, kWarnPrefix);
        return cfg;
    }

    bool apply_text_change_(DocumentState& doc, const TextChange& ch) {
        if (!ch.has_range) {
            const size_t old_size = doc.text.size();
            if (doc.text == ch.text) return false;
            doc.text = ch.text;
            doc.pending_edits.clear();
            doc.pending_edits.push_back(parus::parse::EditWindow{
                0u,
                static_cast<uint32_t>(std::min(old_size, static_cast<size_t>(std::numeric_limits<uint32_t>::max())))
            });
            return true;
        }

        const size_t start = byte_offset_from_position_(doc.text, ch.range.start);
        const size_t end = byte_offset_from_position_(doc.text, ch.range.end);
        const size_t lo = std::min(start, end);
        const size_t hi = std::max(start, end);
        if (lo > doc.text.size()) return false;

        const size_t clamped_hi = std::min(hi, doc.text.size());
        if (doc.text.compare(lo, clamped_hi - lo, ch.text) == 0) {
            return false;
        }
        doc.text.replace(lo, clamped_hi - lo, ch.text);

        doc.pending_edits.push_back(parus::parse::EditWindow{
            static_cast<uint32_t>(std::min(lo, static_cast<size_t>(std::numeric_limits<uint32_t>::max()))),
            static_cast<uint32_t>(std::min(clamped_hi, static_cast<size_t>(std::numeric_limits<uint32_t>::max()))),
        });
        return true;
    }

    int to_lsp_severity_(parus::diag::Severity sev) {
        switch (sev) {
            case parus::diag::Severity::kWarning: return 2;
            case parus::diag::Severity::kFatal: return 1;
            case parus::diag::Severity::kError:
            default:
                return 1;
        }
    }

    bool location_from_span_(
        const parus::SourceManager& sm,
        const parus::Span& sp,
        std::string_view uri,
        LspLocation& out
    ) {
        if (sp.hi < sp.lo) return false;
        const auto begin_lc = sm.line_col(sp.file_id, sp.lo);
        const auto end_lc = sm.line_col(sp.file_id, (sp.hi > sp.lo) ? sp.hi : sp.lo + 1);
        if (begin_lc.line == 0 || begin_lc.col == 0 || end_lc.line == 0 || end_lc.col == 0) {
            return false;
        }
        out.uri = std::string(uri);
        out.start_line = begin_lc.line - 1;
        out.start_character = begin_lc.col - 1;
        out.end_line = end_lc.line - 1;
        out.end_character = end_lc.col - 1;
        return true;
    }

    bool same_location_(const LspLocation& a, const LspLocation& b) {
        return a.uri == b.uri &&
               a.start_line == b.start_line &&
               a.start_character == b.start_character &&
               a.end_line == b.end_line &&
               a.end_character == b.end_character;
    }

    void append_unique_location_(std::vector<LspLocation>& out, const LspLocation& loc) {
        for (const auto& e : out) {
            if (same_location_(e, loc)) return;
        }
        out.push_back(loc);
    }

    void append_keyword_completions_(
        const std::span<const std::string_view> kws,
        std::vector<CompletionEntry>& out
    ) {
        out.reserve(out.size() + kws.size());
        for (const auto kw : kws) {
            CompletionEntry item{};
            item.label = std::string(kw);
            item.kind = 14; // CompletionItemKind::Keyword
            item.detail = "keyword";
            out.push_back(std::move(item));
        }
    }

    void append_completion_entry_unique_(
        std::vector<CompletionEntry>& out,
        std::string_view label,
        uint32_t kind,
        std::string_view detail
    ) {
        if (label.empty()) return;
        for (const auto& it : out) {
            if (it.label == label) return;
        }
        CompletionEntry e{};
        e.label = std::string(label);
        e.kind = kind;
        e.detail = std::string(detail);
        out.push_back(std::move(e));
    }

    void append_definition_target_(
        std::unordered_map<std::string, std::vector<LspLocation>>& out,
        std::string_view key,
        const LspLocation& loc
    ) {
        if (key.empty()) return;
        append_unique_location_(out[std::string(key)], loc);
    }

    uint64_t sem_span_key_(const parus::Span& sp) {
        return (static_cast<uint64_t>(sp.lo) << 32) | static_cast<uint64_t>(sp.hi);
    }

    bool sem_token_from_span_(
        const parus::SourceManager& sm,
        const parus::Span& sp,
        const SemClass& sem_class,
        SemToken& out
    ) {
        if (sp.hi <= sp.lo) return false;

        const auto begin_lc = sm.line_col(sp.file_id, sp.lo);
        const auto end_lc = sm.line_col(sp.file_id, sp.hi);
        if (begin_lc.line == 0 || begin_lc.col == 0 || end_lc.line == 0 || end_lc.col == 0) {
            return false;
        }

        const uint32_t start_line = begin_lc.line - 1;
        const uint32_t end_line = end_lc.line - 1;
        if (start_line != end_line) {
            return false;
        }

        const uint32_t start_col = begin_lc.col - 1;
        const uint32_t end_col = end_lc.col - 1;

        SemToken tok{};
        tok.line = start_line;
        tok.start_character = start_col;
        tok.length = (end_col > start_col) ? (end_col - start_col) : 1;
        tok.token_type = sem_class.token_type;
        tok.token_modifiers = sem_class.token_modifiers;
        out = tok;
        return true;
    }

    bool is_keyword_token_kind_(parus::syntax::TokenKind kind) {
        using K = parus::syntax::TokenKind;
        switch (kind) {
            case K::kKwTrue:
            case K::kKwFalse:
            case K::kKwNull:
            case K::kKwAnd:
            case K::kKwOr:
            case K::kKwNot:
            case K::kKwXor:
            case K::kKwMut:
            case K::kKwStatic:
            case K::kKwConst:
            case K::kKwLet:
            case K::kKwSet:
            case K::kKwIf:
            case K::kKwElif:
            case K::kKwElse:
            case K::kKwWhile:
            case K::kKwDo:
            case K::kKwReturn:
            case K::kKwBreak:
            case K::kKwContinue:
            case K::kKwManual:
            case K::kKwExport:
            case K::kKwExtern:
            case K::kKwLayout:
            case K::kKwAlign:
            case K::kKwFn:
            case K::kKwMacro:
            case K::kKwField:
            case K::kKwActs:
            case K::kKwClass:
            case K::kKwProto:
            case K::kKwActor:
            case K::kKwInit:
            case K::kKwDeinit:
            case K::kKwDraft:
            case K::kKwSwitch:
            case K::kKwCase:
            case K::kKwDefault:
            case K::kKwLoop:
            case K::kKwIn:
            case K::kKwCommit:
            case K::kKwRecast:
            case K::kKwPub:
            case K::kKwSub:
            case K::kKwPure:
            case K::kKwComptime:
            case K::kKwUse:
            case K::kKwImport:
            case K::kKwModule:
            case K::kKwAs:
            case K::kKwNest:
            case K::kKwWith:
            case K::kKwRequire:
                return true;
            default:
                return false;
        }
    }

    bool is_operator_token_kind_(parus::syntax::TokenKind kind) {
        using K = parus::syntax::TokenKind;
        switch (kind) {
            case K::kArrow:
            case K::kLParen:
            case K::kRParen:
            case K::kLBrace:
            case K::kRBrace:
            case K::kLBracket:
            case K::kRBracket:
            case K::kComma:
            case K::kColon:
            case K::kSemicolon:
            case K::kQuestion:
            case K::kQuestionQuestion:
            case K::kQuestionQuestionAssign:
            case K::kDot:
            case K::kAssign:
            case K::kPlusAssign:
            case K::kMinusAssign:
            case K::kStarAssign:
            case K::kSlashAssign:
            case K::kPercentAssign:
            case K::kPlus:
            case K::kMinus:
            case K::kStar:
            case K::kSlash:
            case K::kPercent:
            case K::kPlusPlus:
            case K::kBang:
            case K::kCaret:
            case K::kAmp:
            case K::kAmpAmp:
            case K::kPipePipe:
            case K::kEqEq:
            case K::kBangEq:
            case K::kLt:
            case K::kLtEq:
            case K::kGt:
            case K::kGtEq:
            case K::kShiftLeft:
            case K::kShiftRight:
            case K::kPipeFwd:
            case K::kPipeRev:
            case K::kDotDot:
            case K::kDotDotColon:
            case K::kColonColon:
            case K::kUnknownPunct:
                return true;
            default:
                return false;
        }
    }

    SemClass sem_class_from_binding_(parus::passes::BindingKind bind) {
        switch (bind) {
            case parus::passes::BindingKind::kFn:
                return SemClass{static_cast<uint32_t>(SemTokenType::kFunction), 0};
            case parus::passes::BindingKind::kParam:
                return SemClass{static_cast<uint32_t>(SemTokenType::kParameter), 0};
            case parus::passes::BindingKind::kType:
                return SemClass{static_cast<uint32_t>(SemTokenType::kType), 0};
            case parus::passes::BindingKind::kLocalVar:
            default:
                return SemClass{static_cast<uint32_t>(SemTokenType::kVariable), 0};
        }
    }

    std::unordered_map<uint64_t, SemClass> collect_decl_semantic_map_(
        const std::vector<parus::Token>& toks
    ) {
        std::unordered_map<uint64_t, SemClass> out;
        using K = parus::syntax::TokenKind;

        auto mark_ident = [&](size_t idx, SemTokenType type, uint32_t mods) {
            if (idx >= toks.size()) return;
            if (toks[idx].kind != K::kIdent) return;
            const uint64_t key = sem_span_key_(toks[idx].span);
            out[key] = SemClass{static_cast<uint32_t>(type), mods | kSemModDeclaration};
        };

        for (size_t i = 0; i < toks.size(); ++i) {
            const auto kind = toks[i].kind;

            if (kind == K::kKwFn) {
                const size_t name_idx = i + 1;
                mark_ident(name_idx, SemTokenType::kFunction, 0);

                size_t lparen_idx = name_idx;
                while (lparen_idx < toks.size()) {
                    if (toks[lparen_idx].kind == K::kLParen) break;
                    if (toks[lparen_idx].kind == K::kLBrace || toks[lparen_idx].kind == K::kSemicolon) break;
                    ++lparen_idx;
                }

                if (lparen_idx < toks.size() && toks[lparen_idx].kind == K::kLParen) {
                    uint32_t depth = 0;
                    for (size_t j = lparen_idx; j < toks.size(); ++j) {
                        const auto jk = toks[j].kind;
                        if (jk == K::kLParen) {
                            ++depth;
                            continue;
                        }
                        if (jk == K::kRParen) {
                            if (depth == 0) break;
                            --depth;
                            if (depth == 0) break;
                            continue;
                        }
                        if (depth != 1) continue;
                        if (jk != K::kIdent) continue;
                        if (j + 1 < toks.size() && toks[j + 1].kind == K::kColon) {
                            mark_ident(j, SemTokenType::kParameter, 0);
                        }
                    }
                }
            }

            if (kind == K::kKwLet || kind == K::kKwSet) {
                bool is_static = false;
                bool is_mut = false;
                size_t j = i + 1;
                while (j < toks.size()) {
                    if (toks[j].kind == K::kKwMut) {
                        is_mut = true;
                        ++j;
                        continue;
                    }
                    if (toks[j].kind == K::kKwStatic) {
                        is_static = true;
                        ++j;
                        continue;
                    }
                    break;
                }
                uint32_t mods = 0;
                if (is_static) mods |= kSemModStatic;
                if (kind == K::kKwLet && !is_mut) mods |= kSemModReadonly;
                mark_ident(j, SemTokenType::kVariable, mods);
            }

            if (kind == K::kKwStatic) {
                size_t j = i + 1;
                bool is_mut = false;
                while (j < toks.size() && toks[j].kind == K::kKwMut) {
                    is_mut = true;
                    ++j;
                }
                uint32_t mods = kSemModStatic;
                if (!is_mut) mods |= kSemModReadonly;
                mark_ident(j, SemTokenType::kVariable, mods);
            }

            if (kind == K::kKwLoop) {
                size_t lparen_idx = i + 1;
                while (lparen_idx < toks.size()) {
                    if (toks[lparen_idx].kind == K::kLParen) break;
                    if (toks[lparen_idx].kind == K::kLBrace || toks[lparen_idx].kind == K::kSemicolon) break;
                    ++lparen_idx;
                }
                if (lparen_idx < toks.size() && toks[lparen_idx].kind == K::kLParen) {
                    uint32_t depth = 0;
                    for (size_t j = lparen_idx; j < toks.size(); ++j) {
                        const auto jk = toks[j].kind;
                        if (jk == K::kLParen) {
                            ++depth;
                            continue;
                        }
                        if (jk == K::kRParen) {
                            if (depth == 0) break;
                            --depth;
                            if (depth == 0) break;
                            continue;
                        }
                        if (depth != 1) continue;
                        if (jk == K::kIdent && j + 1 < toks.size() && toks[j + 1].kind == K::kKwIn) {
                            mark_ident(j, SemTokenType::kVariable, 0);
                            break;
                        }
                    }
                }
            }

            if (kind == K::kKwField) {
                mark_ident(i + 1, SemTokenType::kType, 0);
            }

            if (kind == K::kKwActs || kind == K::kKwClass || kind == K::kKwActor) {
                mark_ident(i + 1, SemTokenType::kClass, 0);
            }

            if (kind == K::kKwProto) {
                mark_ident(i + 1, SemTokenType::kType, 0);
            }

            if (kind == K::kKwModule || kind == K::kKwNest) {
                mark_ident(i + 1, SemTokenType::kNamespace, 0);
            }

            if (kind == K::kKwImport) {
                for (size_t j = i + 1; j < toks.size(); ++j) {
                    if (toks[j].kind == K::kSemicolon) break;
                    if (toks[j].kind == K::kKwAs) {
                        mark_ident(j + 1, SemTokenType::kNamespace, 0);
                        break;
                    }
                }
            }
        }

        return out;
    }

    std::unordered_map<uint64_t, SemClass> collect_resolved_semantic_map_(
        const parus::passes::NameResolveResult& resolve
    ) {
        std::unordered_map<uint64_t, SemClass> out;

        auto append_resolved = [&](parus::passes::NameResolveResult::ResolvedId rid) {
            if (rid == parus::passes::NameResolveResult::k_invalid_resolved) return;
            if (rid >= resolve.resolved.size()) return;
            const auto& rs = resolve.resolved[rid];
            if (rs.span.hi <= rs.span.lo) return;
            out[sem_span_key_(rs.span)] = sem_class_from_binding_(rs.bind);
        };

        for (const auto rid : resolve.expr_to_resolved) {
            append_resolved(rid);
        }
        for (const auto rid : resolve.stmt_to_resolved) {
            append_resolved(rid);
        }
        for (const auto rid : resolve.param_to_resolved) {
            append_resolved(rid);
        }

        return out;
    }

    std::string qualify_path_(const std::vector<std::string>& ns_stack, std::string_view name) {
        if (name.empty()) return {};
        if (ns_stack.empty()) return std::string(name);
        std::string out{};
        for (size_t i = 0; i < ns_stack.size(); ++i) {
            if (i) out += "::";
            out += ns_stack[i];
        }
        out += "::";
        out += std::string(name);
        return out;
    }

    uint32_t completion_kind_for_stmt_(parus::ast::StmtKind kind) {
        using K = parus::ast::StmtKind;
        switch (kind) {
            case K::kFnDecl: return 3;      // Function
            case K::kProtoDecl: return 8;   // Interface
            case K::kFieldDecl:
            case K::kClassDecl:
            case K::kActorDecl:
            case K::kActsDecl: return 7;    // Class
            case K::kNestDecl: return 9;    // Module
            case K::kVar: return 6;         // Variable
            default: return 1;
        }
    }

    void collect_parus_top_level_symbols_stmt_(
        const parus::ast::AstArena& ast,
        parus::ast::StmtId sid,
        const parus::SourceManager& sm,
        std::string_view uri,
        std::vector<std::string>& ns_stack,
        std::vector<CompletionEntry>& completion_items,
        std::unordered_map<std::string, std::vector<LspLocation>>& definitions
    ) {
        if (sid == parus::ast::k_invalid_stmt) return;
        const auto& s = ast.stmt(sid);
        const auto& kids = ast.stmt_children();

        if (s.kind == parus::ast::StmtKind::kBlock) {
            const uint64_t begin = s.stmt_begin;
            const uint64_t end = begin + s.stmt_count;
            if (begin <= kids.size() && end <= kids.size()) {
                for (uint32_t i = 0; i < s.stmt_count; ++i) {
                    collect_parus_top_level_symbols_stmt_(
                        ast,
                        kids[s.stmt_begin + i],
                        sm,
                        uri,
                        ns_stack,
                        completion_items,
                        definitions
                    );
                }
            }
            return;
        }

        if (s.kind == parus::ast::StmtKind::kNestDecl) {
            const auto& segs = ast.path_segs();
            uint32_t pushed = 0;
            const uint64_t begin = s.nest_path_begin;
            const uint64_t end = begin + s.nest_path_count;
            if (begin <= segs.size() && end <= segs.size()) {
                for (uint32_t i = 0; i < s.nest_path_count; ++i) {
                    ns_stack.push_back(std::string(segs[s.nest_path_begin + i]));
                    ++pushed;
                }
            }
            if (!s.nest_is_file_directive) {
                collect_parus_top_level_symbols_stmt_(
                    ast, s.a, sm, uri, ns_stack, completion_items, definitions
                );
            }
            while (pushed > 0) {
                ns_stack.pop_back();
                --pushed;
            }
            return;
        }

        auto add_named_decl = [&](std::string_view name, parus::ast::StmtKind kind) {
            if (name.empty()) return;
            LspLocation loc{};
            if (!location_from_span_(sm, s.span, uri, loc)) return;

            append_completion_entry_unique_(
                completion_items,
                name,
                completion_kind_for_stmt_(kind),
                "top-level declaration"
            );
            append_definition_target_(definitions, name, loc);

            const std::string qname = qualify_path_(ns_stack, name);
            if (!qname.empty() && qname != name) {
                append_definition_target_(definitions, qname, loc);
            }
        };

        switch (s.kind) {
            case parus::ast::StmtKind::kFnDecl:
            case parus::ast::StmtKind::kFieldDecl:
            case parus::ast::StmtKind::kProtoDecl:
            case parus::ast::StmtKind::kClassDecl:
            case parus::ast::StmtKind::kActorDecl:
            case parus::ast::StmtKind::kActsDecl:
                add_named_decl(s.name, s.kind);
                break;
            case parus::ast::StmtKind::kVar: {
                const bool global_decl =
                    s.is_static || s.is_extern || s.is_export || (s.link_abi == parus::ast::LinkAbi::kC);
                if (global_decl) {
                    add_named_decl(s.name, s.kind);
                }
                break;
            }
            default:
                break;
        }
    }

    void collect_parus_definition_bindings_(
        const parus::passes::NameResolveResult& resolve,
        const parus::sema::SymbolTable& sym,
        const parus::SourceManager& sm,
        uint32_t current_file_id,
        std::string_view current_uri,
        const std::unordered_map<std::string, std::vector<LspLocation>>* external_defs,
        std::vector<DefinitionBinding>& out
    ) {
        auto append_from_resolved_id = [&](parus::passes::NameResolveResult::ResolvedId rid) {
            if (rid == parus::passes::NameResolveResult::k_invalid_resolved) return;
            if (rid >= resolve.resolved.size()) return;
            const auto& rs = resolve.resolved[rid];
            if (rs.sym == parus::passes::ResolvedSymbol::k_invalid_symbol) return;
            if (rs.sym >= sym.symbols().size()) return;
            if (rs.span.hi <= rs.span.lo) return;
            if (rs.span.file_id != current_file_id) return;

            const auto& symobj = sym.symbol(rs.sym);
            DefinitionBinding bind{};
            bind.use_lo = rs.span.lo;
            bind.use_hi = rs.span.hi;

            if (!symobj.is_external && symobj.decl_span.file_id == current_file_id) {
                LspLocation loc{};
                if (location_from_span_(sm, symobj.decl_span, current_uri, loc)) {
                    append_unique_location_(bind.targets, loc);
                }
            }

            if (symobj.is_external && external_defs != nullptr) {
                auto append_locs = [&](const std::vector<LspLocation>& locs) {
                    for (const auto& loc : locs) {
                        append_unique_location_(bind.targets, loc);
                    }
                };

                if (auto it = external_defs->find(symobj.name); it != external_defs->end()) {
                    append_locs(it->second);
                } else {
                    std::string tail = symobj.name;
                    if (const size_t pos = tail.rfind("::"); pos != std::string::npos && pos + 2 < tail.size()) {
                        tail = tail.substr(pos + 2);
                    }
                    const std::string suffix = "::" + tail;
                    for (const auto& [k, v] : *external_defs) {
                        if (k == tail || ends_with_(k, suffix)) {
                            append_locs(v);
                        }
                    }
                }
            }

            if (!bind.targets.empty()) {
                out.push_back(std::move(bind));
            }
        };

        for (const auto rid : resolve.expr_to_resolved) append_from_resolved_id(rid);
    }

#if PARUSD_ENABLE_LEI
    struct ExternalDeclLocation {
        std::string path{};
        std::string file_uri{};
        uint32_t line = 0;
        uint32_t character = 0;
    };

    struct BundleUnitMeta {
        std::string bundle_name{};
        std::vector<std::string> bundle_deps{};
        std::vector<std::string> bundle_cimport_isystem{};
        std::vector<std::string> normalized_sources{};
        std::unordered_map<std::string, std::string> module_head_by_source{};
        std::unordered_map<std::string, std::vector<std::string>> module_imports_by_source{};
        std::unordered_map<std::string, std::vector<std::string>> module_cimport_isystem_by_source{};
    };

    struct ParusBundleLintContext {
        std::string bundle_name{};
        std::string current_module_head{};
        std::string current_source_dir_norm{};
        std::unordered_set<std::string> allowed_import_heads{};
        std::vector<std::string> cimport_isystem_dirs{};
        std::vector<parus::passes::NameResolveOptions::ExternalExport> external_exports{};
        std::unordered_map<std::string, std::vector<ExternalDeclLocation>> external_decl_locs{};
        std::vector<std::string> index_load_errors{};
    };

    struct BundleUnitsSnapshotCache {
        std::filesystem::path config_lei{};
        std::string cache_key{};
        std::vector<std::string> loaded_module_paths{};
        std::vector<BundleUnitMeta> units{};
    };

    struct LintContextCacheEntry {
        std::filesystem::path config_lei{};
        std::string cache_key{};
        ParusBundleLintContext ctx{};
    };

    std::unordered_map<std::string, BundleUnitsSnapshotCache> g_bundle_units_cache_{};
    std::unordered_map<std::string, LintContextCacheEntry> g_lint_context_cache_{};

    std::string parent_dir_norm_(std::string_view path) {
        namespace fs = std::filesystem;
        std::error_code ec{};
        fs::path p(path);
        if (p.is_relative()) p = fs::absolute(p, ec);
        if (ec) {
            ec.clear();
            p = fs::path(path);
        }
        fs::path dir = p.parent_path();
        fs::path canon = fs::weakly_canonical(dir, ec);
        if (!ec && !canon.empty()) {
            return canon.lexically_normal().string();
        }
        return dir.lexically_normal().string();
    }

    std::string compute_module_head_(
        const std::string& bundle_root,
        const std::string& source_path,
        const std::string& bundle_name
    ) {
        namespace fs = std::filesystem;
        std::error_code ec{};
        fs::path root(bundle_root);
        if (root.is_relative()) root = fs::absolute(root, ec);
        if (ec) {
            ec.clear();
            root = fs::path(bundle_root);
        }
        fs::path root_norm = fs::weakly_canonical(root, ec);
        if (ec || root_norm.empty()) {
            ec.clear();
            root_norm = root.lexically_normal();
        }

        fs::path src(source_path);
        if (src.is_relative()) src = fs::absolute(src, ec);
        if (ec) {
            ec.clear();
            src = fs::path(source_path);
        }
        fs::path src_norm = fs::weakly_canonical(src, ec);
        if (ec || src_norm.empty()) {
            ec.clear();
            src_norm = src.lexically_normal();
        }

        fs::path rel = src_norm.lexically_relative(root_norm);
        const std::string rel_s = rel.generic_string();
        if (rel.empty() || rel_s.empty() || rel_s == "." || rel_s.starts_with("..")) {
            rel = src_norm.filename();
        }

        fs::path dir = rel.parent_path();
        std::vector<std::string> segs{};
        bool stripped_src = false;
        for (const auto& seg : dir) {
            const std::string s = seg.string();
            if (s.empty() || s == ".") continue;
            if (!stripped_src && s == "src") {
                stripped_src = true;
                continue;
            }
            segs.push_back(s);
        }

        if (segs.empty()) return bundle_name;
        std::string out{};
        for (size_t i = 0; i < segs.size(); ++i) {
            if (i) out += "::";
            out += segs[i];
        }
        return out;
    }

    std::string normalize_import_head_(std::string_view import_head) {
        if (import_head.empty()) return {};
        std::string_view s = import_head;
        if (s.starts_with("::")) s.remove_prefix(2);
        if (s.empty() || s.ends_with("::")) return {};
        const size_t pos = s.find("::");
        std::string_view top = (pos == std::string_view::npos) ? s : s.substr(0, pos);
        if (top.empty() || top.find(':') != std::string_view::npos) return {};
        return std::string(top);
    }

    std::string normalize_core_public_module_head_(
        std::string_view bundle_name,
        std::string_view module_head
    ) {
        if (bundle_name != "core") {
            return std::string(module_head);
        }
        if (module_head.empty()) {
            return std::string("core");
        }
        if (module_head == "core" || module_head.starts_with("core::")) {
            return std::string(module_head);
        }
        std::string out = "core::";
        out += module_head;
        return out;
    }

    uint64_t file_mtime_tick_(const std::filesystem::path& p) {
        std::error_code ec{};
        const auto t = std::filesystem::last_write_time(p, ec);
        if (ec) return 0;
        return static_cast<uint64_t>(t.time_since_epoch().count());
    }

    std::string make_bundle_units_cache_key_(
        const std::filesystem::path& config_lei,
        const std::vector<std::string>& loaded_modules
    ) {
        std::vector<std::string> mods = loaded_modules;
        std::sort(mods.begin(), mods.end());
        mods.erase(std::unique(mods.begin(), mods.end()), mods.end());

        std::string key = normalize_host_path_(config_lei.string());
        key += "|cfg_m=" + std::to_string(file_mtime_tick_(config_lei));
        for (const auto& m : mods) {
            key += "|m=" + normalize_host_path_(m);
            key += "@";
            key += std::to_string(file_mtime_tick_(std::filesystem::path(m)));
        }
        return key;
    }

    bool same_file_path_(const std::filesystem::path& a, const std::filesystem::path& b) {
        const auto an = normalize_host_path_(a.string());
        const auto bn = normalize_host_path_(b.string());
        return an == bn;
    }

    bool env_flag_truthy_(std::string_view s) {
        if (s.empty()) return false;
        std::string v(s);
        std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return v == "1" || v == "true" || v == "yes" || v == "on";
    }

    std::string getenv_string_(const char* key) {
        if (key == nullptr || *key == '\0') return {};
        if (const char* v = std::getenv(key); v != nullptr && *v != '\0') {
            return std::string(v);
        }
        return {};
    }

    std::string resolve_lsp_sysroot_() {
        std::string sysroot = getenv_string_("PARUS_SYSROOT");
        if (!sysroot.empty()) return sysroot;

        const std::string toolchain_root = getenv_string_("PARUS_TOOLCHAIN_ROOT");
        if (!toolchain_root.empty()) {
            namespace fs = std::filesystem;
            std::error_code ec{};
            fs::path p = fs::path(toolchain_root) / "sysroot";
            const fs::path canonical = fs::weakly_canonical(p, ec);
            if (!ec) return canonical.string();
            return p.lexically_normal().string();
        }
        return {};
    }

    std::string resolve_core_export_index_path_() {
        namespace fs = std::filesystem;
        std::error_code ec{};

        const std::string sysroot = resolve_lsp_sysroot_();
        if (sysroot.empty()) return {};

        const fs::path idx = fs::path(sysroot) / ".cache" / "exports" / "core.exports.json";
        const fs::path normalized = fs::weakly_canonical(idx, ec);
        if (!ec && fs::exists(normalized, ec) && !ec && fs::is_regular_file(normalized, ec)) {
            return normalize_host_path_(normalized.string());
        }
        ec.clear();
        if (fs::exists(idx, ec) && !ec && fs::is_regular_file(idx, ec)) {
            return normalize_host_path_(idx.string());
        }
        return {};
    }

    std::string resolve_core_macro_prelude_path_() {
        namespace fs = std::filesystem;
        std::error_code ec{};

        const std::string sysroot = resolve_lsp_sysroot_();
        if (sysroot.empty()) return {};

        const fs::path p = fs::path(sysroot) / "core" / "ext" / "cstr.pr";
        const fs::path normalized = fs::weakly_canonical(p, ec);
        if (!ec && fs::exists(normalized, ec) && !ec && fs::is_regular_file(normalized, ec)) {
            return normalize_host_path_(normalized.string());
        }
        ec.clear();
        if (fs::exists(p, ec) && !ec && fs::is_regular_file(p, ec)) {
            return normalize_host_path_(p.string());
        }
        return {};
    }

    std::string_view clone_sv_into_ast_(parus::ast::AstArena& dst, std::string_view s) {
        if (s.empty()) return {};
        return dst.add_owned_string(std::string(s));
    }

    parus::Token clone_token_into_ast_(parus::ast::AstArena& dst, const parus::Token& src) {
        parus::Token out = src;
        out.lexeme = clone_sv_into_ast_(dst, src.lexeme);
        return out;
    }

    void copy_macro_token_range_into_ast_(
        const parus::ast::AstArena& src,
        parus::ast::AstArena& dst,
        uint32_t in_begin,
        uint32_t in_count,
        uint32_t& out_begin,
        uint32_t& out_count
    ) {
        out_begin = static_cast<uint32_t>(dst.macro_tokens().size());
        out_count = 0;

        const auto& toks = src.macro_tokens();
        const uint64_t begin = in_begin;
        const uint64_t end = begin + in_count;
        if (begin > toks.size() || end > toks.size()) return;
        for (uint32_t i = 0; i < in_count; ++i) {
            const auto& t = toks[in_begin + i];
            (void)dst.add_macro_token(clone_token_into_ast_(dst, t));
            ++out_count;
        }
    }

    void append_top_level_macro_decls_(
        const parus::ast::AstArena& src,
        parus::ast::AstArena& dst
    ) {
        const auto& src_decls = src.macro_decls();
        const auto& src_groups = src.macro_groups();
        const auto& src_arms = src.macro_arms();
        const auto& src_caps = src.macro_captures();

        for (const auto& d : src_decls) {
            if (d.scope_depth != 0) continue;

            parus::ast::MacroDecl nd{};
            nd.name = clone_sv_into_ast_(dst, d.name);
            nd.scope_depth = 0;
            nd.span = d.span;
            nd.group_begin = static_cast<uint32_t>(dst.macro_groups().size());
            nd.group_count = 0;

            const uint64_t g_begin = d.group_begin;
            const uint64_t g_end = g_begin + d.group_count;
            if (g_begin > src_groups.size() || g_end > src_groups.size()) continue;

            for (uint32_t gi = 0; gi < d.group_count; ++gi) {
                const auto& g = src_groups[d.group_begin + gi];
                parus::ast::MacroGroup ng{};
                ng.match_kind = g.match_kind;
                ng.span = g.span;
                ng.arm_begin = static_cast<uint32_t>(dst.macro_arms().size());
                ng.arm_count = 0;

                const uint64_t a_begin = g.arm_begin;
                const uint64_t a_end = a_begin + g.arm_count;
                if (a_begin > src_arms.size() || a_end > src_arms.size()) continue;

                for (uint32_t ai = 0; ai < g.arm_count; ++ai) {
                    const auto& a = src_arms[g.arm_begin + ai];
                    parus::ast::MacroArm na{};
                    na.out_kind = a.out_kind;
                    na.span = a.span;
                    na.capture_begin = static_cast<uint32_t>(dst.macro_captures().size());
                    na.capture_count = 0;

                    const uint64_t c_begin = a.capture_begin;
                    const uint64_t c_end = c_begin + a.capture_count;
                    if (c_begin <= src_caps.size() && c_end <= src_caps.size()) {
                        for (uint32_t ci = 0; ci < a.capture_count; ++ci) {
                            auto cap = src_caps[a.capture_begin + ci];
                            cap.name = clone_sv_into_ast_(dst, cap.name);
                            (void)dst.add_macro_capture(cap);
                            ++na.capture_count;
                        }
                    }

                    copy_macro_token_range_into_ast_(
                        src, dst, a.pattern_token_begin, a.pattern_token_count,
                        na.pattern_token_begin, na.pattern_token_count
                    );
                    copy_macro_token_range_into_ast_(
                        src, dst, a.template_token_begin, a.template_token_count,
                        na.template_token_begin, na.template_token_count
                    );

                    (void)dst.add_macro_arm(na);
                    ++ng.arm_count;
                }

                (void)dst.add_macro_group(ng);
                ++nd.group_count;
            }

            (void)dst.add_macro_decl(nd);
        }
    }

    bool load_core_macro_prelude_into_ast_(
        parus::ast::AstArena& dst_ast,
        parus::SourceManager& sm,
        parus::diag::Bag& bag,
        std::string& out_err
    ) {
        out_err.clear();
        const std::string prelude_path = resolve_core_macro_prelude_path_();
        if (prelude_path.empty()) {
            return true; // optional unless user relies on core-provided macros
        }

        std::string text{};
        {
            std::ifstream ifs(prelude_path, std::ios::binary);
            if (!ifs) {
                out_err = "failed to read core macro prelude: " + prelude_path;
                return false;
            }
            std::ostringstream oss{};
            oss << ifs.rdbuf();
            text = std::move(oss).str();
        }
        if (text.empty()) {
            out_err = "failed to read core macro prelude: " + prelude_path;
            return false;
        }

        const uint32_t fid = sm.add(prelude_path, text);
        parus::Lexer lexer(sm.content(fid), fid, &bag);
        auto tokens = lexer.lex_all();
        if (bag.has_error()) {
            out_err = "failed to lex core macro prelude: " + prelude_path;
            return false;
        }

        parus::ast::AstArena src_ast{};
        parus::ty::TypePool src_types{};
        parus::diag::Bag local_bag{};
        parus::ParserFeatureFlags flags{};
        parus::Parser p(tokens, src_ast, src_types, &local_bag, 128, flags);
        (void)p.parse_program();
        if (local_bag.has_error()) {
            for (const auto& d : local_bag.diags()) bag.add(d);
            out_err = "failed to parse core macro prelude: " + prelude_path;
            return false;
        }

        append_top_level_macro_decls_(src_ast, dst_ast);
        return true;
    }

    bool is_core_impl_marker_stmt_(const parus::ast::AstArena& ast, const parus::ast::Stmt& s) {
        if (s.kind != parus::ast::StmtKind::kCompilerIntrinsicDirective) return false;
        if (s.directive_target_path_count != 0) return false; // tag form only
        if (s.directive_key_path_count != 2) return false;
        const auto& segs = ast.path_segs();
        const uint64_t begin = s.directive_key_path_begin;
        const uint64_t end = begin + s.directive_key_path_count;
        if (begin > segs.size() || end > segs.size()) return false;
        return segs[s.directive_key_path_begin] == "Impl" &&
               segs[s.directive_key_path_begin + 1] == "Core";
    }

    void collect_core_impl_marker_file_ids_(const parus::ast::AstArena& ast,
                                            parus::ast::StmtId root_sid,
                                            std::unordered_set<uint32_t>& out) {
        if (root_sid == parus::ast::k_invalid_stmt || static_cast<size_t>(root_sid) >= ast.stmts().size()) {
            return;
        }
        const auto& root = ast.stmt(root_sid);
        if (root.kind != parus::ast::StmtKind::kBlock) return;
        const auto& kids = ast.stmt_children();
        const uint64_t begin = root.stmt_begin;
        const uint64_t end = begin + root.stmt_count;
        if (begin > kids.size() || end > kids.size()) return;
        for (uint32_t i = 0; i < root.stmt_count; ++i) {
            const auto sid = kids[root.stmt_begin + i];
            if (sid == parus::ast::k_invalid_stmt || static_cast<size_t>(sid) >= ast.stmts().size()) continue;
            const auto& s = ast.stmt(sid);
            if (!is_core_impl_marker_stmt_(ast, s)) continue;
            out.insert(s.span.file_id);
        }
    }

    struct CHeaderImportSpec {
        std::string header{};
        std::string alias{};
        parus::Span span{};
    };

    void collect_c_header_imports_(const parus::ast::AstArena& ast,
                                   parus::ast::StmtId root_sid,
                                   std::vector<CHeaderImportSpec>& out) {
        if (root_sid == parus::ast::k_invalid_stmt || static_cast<size_t>(root_sid) >= ast.stmts().size()) {
            return;
        }
        const auto& root = ast.stmt(root_sid);
        if (root.kind != parus::ast::StmtKind::kBlock) return;
        const auto& kids = ast.stmt_children();
        const uint64_t begin = root.stmt_begin;
        const uint64_t end = begin + root.stmt_count;
        if (begin > kids.size() || end > kids.size()) return;

        for (uint32_t i = 0; i < root.stmt_count; ++i) {
            const auto sid = kids[root.stmt_begin + i];
            if (sid == parus::ast::k_invalid_stmt || static_cast<size_t>(sid) >= ast.stmts().size()) continue;
            const auto& s = ast.stmt(sid);
            if (s.kind != parus::ast::StmtKind::kUse) continue;
            if (s.use_kind != parus::ast::UseKind::kImportCHeader) continue;
            if (s.use_rhs_ident.empty()) continue;

            CHeaderImportSpec one{};
            if (s.use_path_count > 0) {
                const auto& segs = ast.path_segs();
                if (s.use_path_begin + s.use_path_count > segs.size()) continue;
                one.header = std::string(segs[s.use_path_begin]);
            } else if (!s.use_name.empty()) {
                one.header = std::string(s.use_name);
            } else {
                continue;
            }
            one.alias = std::string(s.use_rhs_ident);
            one.span = s.span;
            out.push_back(std::move(one));
        }
    }

    bool is_under_root_(const std::filesystem::path& path, const std::filesystem::path& root) {
        const auto p = normalize_host_path_(path.string());
        const auto r = normalize_host_path_(root.string());
        if (r.empty()) return false;
        if (p == r) return true;
        if (p.size() < r.size()) return false;
        if (!p.starts_with(r)) return false;
        if (p.size() == r.size()) return false;
        const char next = p[r.size()];
        return next == '/' || next == '\\';
    }

    void invalidate_lint_caches_for_root_(const std::filesystem::path& root) {
        for (auto it = g_bundle_units_cache_.begin(); it != g_bundle_units_cache_.end();) {
            if (is_under_root_(it->second.config_lei.parent_path(), root)) {
                it = g_bundle_units_cache_.erase(it);
            } else {
                ++it;
            }
        }
        for (auto it = g_lint_context_cache_.begin(); it != g_lint_context_cache_.end();) {
            if (is_under_root_(it->second.config_lei.parent_path(), root)) {
                it = g_lint_context_cache_.erase(it);
            } else {
                ++it;
            }
        }
    }

    std::string shell_quote_(std::string_view s) {
        bool need = s.empty();
        for (const char c : s) {
            if (std::isspace(static_cast<unsigned char>(c)) || c == '\'' || c == '"' || c == '\\') {
                need = true;
                break;
            }
        }
        if (!need) return std::string(s);
        std::string out{"'"};
        for (const char c : s) {
            if (c == '\'') out += "'\\''";
            else out.push_back(c);
        }
        out += "'";
        return out;
    }

    int run_argv_system_(const std::vector<std::string>& argv) {
        if (argv.empty()) return 1;
        std::string cmd{};
        for (size_t i = 0; i < argv.size(); ++i) {
            if (i) cmd.push_back(' ');
            cmd += shell_quote_(argv[i]);
        }
        return std::system(cmd.c_str());
    }

    std::string resolve_parusc_path_() {
        if (const char* p = std::getenv("PARUSC"); p != nullptr && *p != '\0') {
            return std::string(p);
        }
        if (const char* root = std::getenv("PARUS_TOOLCHAIN_ROOT"); root != nullptr && *root != '\0') {
            std::filesystem::path cand = std::filesystem::path(root) / "bin" / "parusc";
            std::error_code ec{};
            if (std::filesystem::exists(cand, ec) && !ec) return cand.string();
        }
        return "parusc";
    }

    bool index_stale_for_bundle_(const std::filesystem::path& index_path, const BundleUnitMeta& unit) {
        namespace fs = std::filesystem;
        std::error_code ec{};
        if (!fs::exists(index_path, ec) || ec) return true;
        const auto index_time = fs::last_write_time(index_path, ec);
        if (ec) return true;
        for (const auto& src : unit.normalized_sources) {
            const fs::path src_path(src);
            const auto src_time = fs::last_write_time(src_path, ec);
            if (ec) return true;
            if (src_time > index_time) return true;
        }
        return false;
    }

    bool ensure_bundle_export_index_(
        const std::filesystem::path& config_dir,
        const BundleUnitMeta& unit,
        std::filesystem::path& out_index_path
    ) {
        namespace fs = std::filesystem;
        std::error_code ec{};
        const fs::path index_dir = config_dir / ".lei-cache" / "index";
        fs::create_directories(index_dir, ec);
        if (ec) return false;
        out_index_path = index_dir / (unit.bundle_name + ".exports.json");
        if (!index_stale_for_bundle_(out_index_path, unit)) return true;
        if (unit.normalized_sources.empty()) return false;

        std::vector<std::string> argv{
            resolve_parusc_path_(),
            unit.normalized_sources.front(),
            "-fsyntax-only",
            "--bundle-name",
            unit.bundle_name,
            "--bundle-root",
            config_dir.lexically_normal().string(),
            "--emit-export-index",
            out_index_path.string(),
        };
        if (auto it = unit.module_head_by_source.find(unit.normalized_sources.front());
            it != unit.module_head_by_source.end() && !it->second.empty()) {
            argv.push_back("--module-head");
            argv.push_back(it->second);
        }
        if (auto it = unit.module_imports_by_source.find(unit.normalized_sources.front());
            it != unit.module_imports_by_source.end()) {
            for (const auto& import_head : it->second) {
                if (import_head.empty()) continue;
                argv.push_back("--module-import");
                argv.push_back(normalize_import_head_(import_head));
            }
        }
        std::vector<std::string> cimport_isystem = unit.bundle_cimport_isystem;
        if (auto it = unit.module_cimport_isystem_by_source.find(unit.normalized_sources.front());
            it != unit.module_cimport_isystem_by_source.end()) {
            parus::cimport::append_unique_normalized_paths(cimport_isystem, it->second);
        }
        for (const auto& d : cimport_isystem) {
            if (d.empty()) continue;
            argv.push_back("-isystem");
            argv.push_back(d);
        }
        for (const auto& src : unit.normalized_sources) {
            argv.push_back("--bundle-source");
            argv.push_back(src);
        }
        for (const auto& dep : unit.bundle_deps) {
            argv.push_back("--bundle-dep");
            argv.push_back(dep);
        }
        const int rc = run_argv_system_(argv);
        return rc == 0;
    }

    bool read_text_file_(const std::filesystem::path& path, std::string& out) {
        std::ifstream ifs(path, std::ios::binary);
        if (!ifs.is_open()) return false;
        out.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        return true;
    }

    std::optional<std::filesystem::path> find_config_lei_for_file_(const std::filesystem::path& source_file) {
        namespace fs = std::filesystem;
        std::error_code ec{};
        fs::path cur = source_file.parent_path();
        while (!cur.empty()) {
            const fs::path cand = cur / "config.lei";
            if (fs::exists(cand, ec) && !ec) {
                return fs::weakly_canonical(cand, ec);
            }
            if (!cur.has_parent_path()) break;
            const fs::path parent = cur.parent_path();
            if (parent == cur) break;
            cur = parent;
        }
        return std::nullopt;
    }

    bool collect_bundle_units_from_master_(
        const std::filesystem::path& config_lei,
        std::vector<BundleUnitMeta>& out_units,
        std::vector<std::string>* out_loaded_modules = nullptr,
        const std::unordered_map<std::string, std::string>* overlays = nullptr
    ) {
        lei::diag::Bag eval_bag;
        lei::eval::EvaluatorBudget budget{};
        auto builtins = lei::eval::make_default_builtin_registry();
        auto builtin_plans = lei::eval::make_default_builtin_plan_registry();
        lei::parse::ParserControl parser_control{};
        lei::eval::Evaluator evaluator(
            budget,
            eval_bag,
            std::move(builtins),
            std::move(builtin_plans),
            parser_control
        );

        lei::eval::EvaluateOptions eval_options{};
        eval_options.entry_plan = "master";
        if (overlays != nullptr) {
            eval_options.source_overlay =
                [overlays](std::string_view normalized_path) -> std::optional<std::string> {
                    const auto it = overlays->find(std::string(normalized_path));
                    if (it == overlays->end()) return std::nullopt;
                    return it->second;
                };
        }
        const auto entry = evaluator.evaluate_entry(config_lei, eval_options);
        if (!entry.has_value()) return false;
        if (eval_bag.has_error()) return false;

        auto graph = lei::graph::from_entry_plan_value(*entry, eval_bag, "master");
        if (!graph.has_value() || eval_bag.has_error()) return false;

        if (out_loaded_modules != nullptr) {
            *out_loaded_modules = evaluator.loaded_module_paths();
            for (auto& p : *out_loaded_modules) p = normalize_host_path_(p);
            std::sort(out_loaded_modules->begin(), out_loaded_modules->end());
            out_loaded_modules->erase(
                std::unique(out_loaded_modules->begin(), out_loaded_modules->end()),
                out_loaded_modules->end()
            );
        }

        const auto config_dir = config_lei.parent_path();
        out_units.clear();
        out_units.reserve(graph->bundles.size());

        std::unordered_map<std::string, size_t> unit_by_bundle{};
        for (const auto& b : graph->bundles) {
            BundleUnitMeta unit{};
            unit.bundle_name = b.name;
            unit.bundle_deps = b.deps;
            for (const auto& d : b.cimport_isystem) {
                if (d.empty()) continue;
                std::filesystem::path p(d);
                if (p.is_relative()) p = config_dir / p;
                unit.bundle_cimport_isystem.push_back(normalize_host_path_(p.string()));
            }
            std::sort(unit.bundle_cimport_isystem.begin(), unit.bundle_cimport_isystem.end());
            unit.bundle_cimport_isystem.erase(
                std::unique(unit.bundle_cimport_isystem.begin(), unit.bundle_cimport_isystem.end()),
                unit.bundle_cimport_isystem.end());
            std::sort(unit.bundle_deps.begin(), unit.bundle_deps.end());
            unit.bundle_deps.erase(std::unique(unit.bundle_deps.begin(), unit.bundle_deps.end()), unit.bundle_deps.end());
            unit_by_bundle[b.name] = out_units.size();
            out_units.push_back(std::move(unit));
        }

        for (const auto& m : graph->modules) {
            const auto bit = unit_by_bundle.find(m.bundle);
            if (bit == unit_by_bundle.end()) continue;
            auto& unit = out_units[bit->second];
            for (const auto& src : m.sources) {
                const auto abs_src = normalize_host_path_((config_dir / src).string());
                unit.normalized_sources.push_back(abs_src);
                unit.module_head_by_source[abs_src] = m.head;
                auto& imports = unit.module_imports_by_source[abs_src];
                for (const auto& import_head : m.imports) {
                    const auto top_head = normalize_import_head_(import_head);
                    if (top_head.empty()) continue;
                    imports.push_back(top_head);
                }
                auto& isystem = unit.module_cimport_isystem_by_source[abs_src];
                for (const auto& d : m.cimport_isystem) {
                    if (d.empty()) continue;
                    std::filesystem::path p(d);
                    if (p.is_relative()) p = config_dir / p;
                    isystem.push_back(normalize_host_path_(p.string()));
                }
            }
        }

        for (auto& unit : out_units) {
            std::sort(unit.normalized_sources.begin(), unit.normalized_sources.end());
            unit.normalized_sources.erase(
                std::unique(unit.normalized_sources.begin(), unit.normalized_sources.end()),
                unit.normalized_sources.end()
            );
            for (auto& [src, imports] : unit.module_imports_by_source) {
                std::sort(imports.begin(), imports.end());
                imports.erase(std::unique(imports.begin(), imports.end()), imports.end());
            }
            for (auto& [src, isystem] : unit.module_cimport_isystem_by_source) {
                std::sort(isystem.begin(), isystem.end());
                isystem.erase(std::unique(isystem.begin(), isystem.end()), isystem.end());
            }
        }

        return !out_units.empty();
    }

    std::optional<parus::sema::SymbolKind> export_kind_from_string_(std::string_view kind) {
        if (kind == "fn") return parus::sema::SymbolKind::kFn;
        if (kind == "var") return parus::sema::SymbolKind::kVar;
        if (kind == "type") return parus::sema::SymbolKind::kType;
        if (kind == "field" || kind == "struct") return parus::sema::SymbolKind::kField;
        if (kind == "act") return parus::sema::SymbolKind::kAct;
        return std::nullopt;
    }

    parus::ty::TypeId parse_type_repr_for_lint_(
        std::string_view type_repr,
        std::string_view inst_payload,
        parus::ty::TypePool& types
    ) {
        return parus::cimport::parse_external_type_repr(type_repr, inst_payload, types);
    }

    bool load_export_index_for_lint_(
        const std::filesystem::path& index_path,
        std::string_view fallback_bundle_name,
        std::string_view current_module_head,
        bool same_bundle,
        std::vector<parus::passes::NameResolveOptions::ExternalExport>& out_exports,
        std::unordered_map<std::string, std::vector<ExternalDeclLocation>>* out_decl_locs = nullptr,
        std::string* out_err = nullptr
    ) {
        auto fail = [&](std::string msg) {
            if (out_err != nullptr) *out_err = std::move(msg);
            return false;
        };
        if (out_err != nullptr) out_err->clear();

        std::string json{};
        if (!read_text_file_(index_path, json)) {
            return fail("failed to read export-index: " + index_path.string());
        }

        JsonValue root{};
        JsonParser parser(json);
        if (!parser.parse(root)) {
            return fail("failed to parse export-index json: " + index_path.string());
        }
        if (root.kind != JsonValue::Kind::kObject) {
            return fail("invalid export-index root object: " + index_path.string());
        }

        std::string bundle_name(fallback_bundle_name);
        if (const auto b = as_string_(obj_get_(root, "bundle")); b.has_value() && !b->empty()) {
            bundle_name = std::string(*b);
        }

        const auto version = as_i64_(obj_get_(root, "version"));
        if (!version.has_value() || (*version != 4 && *version != 5)) {
            return fail("unsupported export-index version (expected v4 or v5): " + index_path.string());
        }

        const auto* exports_node = obj_get_(root, "exports");
        if (exports_node == nullptr || exports_node->kind != JsonValue::Kind::kArray) {
            return fail("invalid export-index exports array: " + index_path.string());
        }

        for (const auto& ev : exports_node->array_v) {
            if (ev.kind != JsonValue::Kind::kObject) continue;

            const auto kind_s = as_string_(obj_get_(ev, "kind"));
            const auto path_s = as_string_(obj_get_(ev, "path"));
            const auto link_name_s = as_string_(obj_get_(ev, "link_name"));
            const auto module_head_s = as_string_(obj_get_(ev, "module_head"));
            const auto decl_dir_s = as_string_(obj_get_(ev, "decl_dir"));
            const auto type_repr_s = as_string_(obj_get_(ev, "type_repr"));
            const auto inst_payload_s = as_string_(obj_get_(ev, "inst_payload"));
            const auto is_export_s = as_bool_(obj_get_(ev, "is_export"));
            const auto* decl_span_node = obj_get_(ev, "decl_span");
            if (!kind_s.has_value() ||
                !path_s.has_value() ||
                path_s->empty() ||
                !link_name_s.has_value() ||
                !module_head_s.has_value() ||
                module_head_s->empty() ||
                !decl_dir_s.has_value() ||
                !type_repr_s.has_value() ||
                !is_export_s.has_value()) {
                return fail("invalid export-index entry schema: " + index_path.string());
            }

            const bool is_export = *is_export_s;

            const auto mapped_kind = export_kind_from_string_(*kind_s);
            if (!mapped_kind.has_value()) continue;

            std::string module_head = normalize_core_public_module_head_(bundle_name, *module_head_s);
            std::string decl_source_dir = std::string(*decl_dir_s);

            std::string lookup_path = std::string(*path_s);
            if (!module_head.empty()) {
                const std::string prefix = module_head + "::";
                const bool already_prefixed =
                    lookup_path == module_head ||
                    lookup_path.starts_with(prefix);
                const bool same_module = same_bundle && module_head == current_module_head;
                if (!same_module && !already_prefixed) {
                    lookup_path = prefix + lookup_path;
                }
            }
            const std::string lookup_path_for_nav = lookup_path;

            parus::passes::NameResolveOptions::ExternalExport ex{};
            ex.kind = *mapped_kind;
            ex.path = lookup_path;
            ex.link_name = std::string(*link_name_s);
            ex.declared_type_repr = std::string(*type_repr_s);
            ex.decl_bundle_name = bundle_name;
            ex.module_head = std::move(module_head);
            ex.decl_source_dir_norm = std::move(decl_source_dir);
            ex.is_export = is_export;
            if (inst_payload_s.has_value()) {
                ex.inst_payload = std::string(*inst_payload_s);
            }
            out_exports.push_back(std::move(ex));

            if (out_decl_locs != nullptr && decl_span_node != nullptr &&
                decl_span_node->kind == JsonValue::Kind::kObject) {
                const auto decl_file = as_string_(obj_get_(*decl_span_node, "file"));
                const auto decl_line = as_i64_(obj_get_(*decl_span_node, "line"));
                const auto decl_col = as_i64_(obj_get_(*decl_span_node, "col"));
                if (decl_file.has_value() && !decl_file->empty()) {
                    ExternalDeclLocation loc{};
                    loc.file_uri = file_path_to_uri_(*decl_file);
                    loc.line = (decl_line.has_value() && *decl_line > 0)
                        ? static_cast<uint32_t>(*decl_line - 1)
                        : 0;
                    loc.character = (decl_col.has_value() && *decl_col > 0)
                        ? static_cast<uint32_t>(*decl_col - 1)
                        : 0;

                    auto add_decl = [&](const std::string& key) {
                        if (key.empty()) return;
                        loc.path = key;
                        (*out_decl_locs)[key].push_back(loc);
                    };

                    add_decl(lookup_path_for_nav);
                    if (!module_head.empty()) {
                        const std::string prefix = module_head + "::";
                        const bool already_prefixed =
                            lookup_path_for_nav == module_head ||
                            lookup_path_for_nav.starts_with(prefix);
                        if (!already_prefixed) {
                            add_decl(prefix + lookup_path_for_nav);
                        }
                        if (same_bundle && current_module_head == module_head) {
                            std::string local = lookup_path_for_nav;
                            if (local.starts_with(prefix)) {
                                local.erase(0, prefix.size());
                            }
                            add_decl(local);
                        }
                    }
                }
            }
        }
        return true;
    }

    bool get_bundle_units_for_config_(
        const std::filesystem::path& config_lei,
        std::vector<BundleUnitMeta>& out_units,
        std::string& out_cache_key,
        const std::unordered_map<std::string, std::string>* overlays = nullptr
    ) {
        const bool use_overlay = (overlays != nullptr && !overlays->empty());
        const std::string config_key = normalize_host_path_(config_lei.string());
        if (!use_overlay) {
            if (auto it = g_bundle_units_cache_.find(config_key); it != g_bundle_units_cache_.end()) {
                const std::string key_now = make_bundle_units_cache_key_(config_lei, it->second.loaded_module_paths);
                if (key_now == it->second.cache_key) {
                    out_units = it->second.units;
                    out_cache_key = key_now;
                    return true;
                }
            }
        }

        std::vector<BundleUnitMeta> units{};
        std::vector<std::string> loaded_modules{};
        if (!collect_bundle_units_from_master_(config_lei, units, &loaded_modules, overlays)) {
            return false;
        }
        const std::string key_now = make_bundle_units_cache_key_(config_lei, loaded_modules);

        if (!use_overlay) {
            BundleUnitsSnapshotCache snap{};
            snap.config_lei = config_lei;
            snap.cache_key = key_now;
            snap.loaded_module_paths = std::move(loaded_modules);
            snap.units = units;
            g_bundle_units_cache_[config_key] = std::move(snap);
        }

        out_units = std::move(units);
        out_cache_key = key_now;
        return true;
    }

    std::optional<ParusBundleLintContext> build_parus_bundle_lint_context_(
        std::string_view uri_or_path,
        const std::unordered_map<std::string, std::string>* overlays = nullptr
    ) {
        std::string current_file = std::string(uri_or_path);
        if (const auto fs_path = uri_to_file_path_(uri_or_path); fs_path.has_value()) {
            current_file = *fs_path;
        }
        const auto normalized_current = normalize_host_path_(current_file);
        const auto config_lei = find_config_lei_for_file_(std::filesystem::path(normalized_current));
        if (!config_lei.has_value()) return std::nullopt;

        std::vector<BundleUnitMeta> units{};
        std::string units_cache_key{};
        if (!get_bundle_units_for_config_(*config_lei, units, units_cache_key, overlays)) {
            return std::nullopt;
        }

        const bool use_overlay = (overlays != nullptr && !overlays->empty());
        if (!use_overlay) {
            if (auto it = g_lint_context_cache_.find(normalized_current); it != g_lint_context_cache_.end()) {
                if (same_file_path_(it->second.config_lei, *config_lei) &&
                    it->second.cache_key == units_cache_key) {
                    return it->second.ctx;
                }
            }
        }

        const BundleUnitMeta* current_unit = nullptr;
        for (const auto& u : units) {
            if (std::find(u.normalized_sources.begin(), u.normalized_sources.end(), normalized_current)
                != u.normalized_sources.end()) {
                current_unit = &u;
                break;
            }
        }
        if (current_unit == nullptr) return std::nullopt;

        ParusBundleLintContext ctx{};
        ctx.bundle_name = current_unit->bundle_name;
        if (auto mh = current_unit->module_head_by_source.find(normalized_current);
            mh != current_unit->module_head_by_source.end() && !mh->second.empty()) {
            ctx.current_module_head = mh->second;
        } else {
            ctx.current_module_head =
                compute_module_head_(config_lei->parent_path().string(), normalized_current, current_unit->bundle_name);
        }
        ctx.current_source_dir_norm = parent_dir_norm_(normalized_current);

        if (!current_unit->bundle_name.empty()) {
            ctx.allowed_import_heads.insert(current_unit->bundle_name);
        }
        if (auto mi = current_unit->module_imports_by_source.find(normalized_current);
            mi != current_unit->module_imports_by_source.end()) {
            for (const auto& import_head : mi->second) {
                const auto top_head = normalize_import_head_(import_head);
                if (!top_head.empty()) ctx.allowed_import_heads.insert(top_head);
            }
        }
        ctx.cimport_isystem_dirs = current_unit->bundle_cimport_isystem;
        if (auto ci = current_unit->module_cimport_isystem_by_source.find(normalized_current);
            ci != current_unit->module_cimport_isystem_by_source.end()) {
            parus::cimport::append_unique_normalized_paths(ctx.cimport_isystem_dirs, ci->second);
        }

        std::unordered_map<std::string, const BundleUnitMeta*> units_by_name{};
        for (const auto& u : units) {
            units_by_name.emplace(u.bundle_name, &u);
        }

        const auto config_dir = config_lei->parent_path();
        auto load_one_bundle = [&](std::string_view bundle_name, bool same_bundle) {
            auto it = units_by_name.find(std::string(bundle_name));
            if (it == units_by_name.end()) return;

            std::filesystem::path idx_path{};
            if (!ensure_bundle_export_index_(config_dir, *it->second, idx_path)) {
                ctx.index_load_errors.push_back(
                    "failed to generate export-index for bundle '" + std::string(bundle_name) + "'");
                return;
            }
            std::string first_err{};
            if (load_export_index_for_lint_(
                    idx_path,
                    bundle_name,
                    ctx.current_module_head,
                    same_bundle,
                    ctx.external_exports,
                    &ctx.external_decl_locs,
                    &first_err)) {
                return;
            }

            // stale cache/schema mismatch: one forced regeneration retry.
            std::error_code ec{};
            std::filesystem::remove(idx_path, ec);
            if (!ensure_bundle_export_index_(config_dir, *it->second, idx_path)) {
                std::string msg =
                    "failed to regenerate export-index for bundle '" + std::string(bundle_name) + "'";
                if (!first_err.empty()) msg += " (first load error: " + first_err + ")";
                ctx.index_load_errors.push_back(std::move(msg));
                return;
            }
            std::string retry_err{};
            if (!load_export_index_for_lint_(
                idx_path,
                bundle_name,
                ctx.current_module_head,
                same_bundle,
                ctx.external_exports,
                &ctx.external_decl_locs,
                &retry_err
            )) {
                std::string msg =
                    "failed to load export-index for bundle '" + std::string(bundle_name) + "'";
                if (!first_err.empty()) msg += " (first: " + first_err + ")";
                if (!retry_err.empty()) msg += " (retry: " + retry_err + ")";
                ctx.index_load_errors.push_back(std::move(msg));
            }
        };

        load_one_bundle(current_unit->bundle_name, /*same_bundle=*/true);
        for (const auto& dep : current_unit->bundle_deps) {
            load_one_bundle(dep, /*same_bundle=*/false);
        }

        if (!use_overlay) {
            g_lint_context_cache_[normalized_current] = LintContextCacheEntry{
                *config_lei,
                units_cache_key,
                ctx,
            };
        }
        return ctx;
    }
#endif

    AnalysisResult analyze_parus_document_(
        std::string_view uri,
        DocumentState& doc,
        const parus::macro::ExpansionBudget& macro_budget,
        const ServerCImportConfig& cimport_cfg,
        const std::unordered_map<std::string, std::string>* lei_overlays
    ) {
        AnalysisResult out;

        parus::SourceManager sm;
        const uint32_t file_id = sm.add(std::string(uri), std::string(doc.text));
        const auto fs_path = uri_to_file_path_(uri);
        const std::string normalized_current =
            fs_path.has_value() ? normalize_host_path_(*fs_path) : normalize_host_path_(std::string(uri));
        const std::string current_dir = parent_dir_norm_(normalized_current);

        parus::diag::Bag bag;
        if (!doc.parse_ready || !doc.parse_session.ready()) {
            doc.parse_ready = doc.parse_session.initialize(sm.content(file_id), file_id, bag);
            doc.pending_edits.clear();
        } else if (!doc.pending_edits.empty()) {
            const auto edits = std::span<const parus::parse::EditWindow>(doc.pending_edits.data(),
                                                                          doc.pending_edits.size());
            doc.parse_ready = doc.parse_session.reparse_with_edits(sm.content(file_id), file_id, edits, bag);
            doc.pending_edits.clear();
        }

        if (!doc.parse_ready || !doc.parse_session.ready()) {
            out.parse_mode = parus::parse::ReparseMode::kNone;
            return out;
        }

        out.parse_mode = doc.parse_session.last_mode();

        // Incremental parser snapshot은 다음 reparse의 정본으로 유지해야 한다.
        // (macro/type-check 단계에서 snapshot을 직접 변형하면 didChange 이후 진단이 불안정해진다.)
        const auto& snapshot = doc.parse_session.snapshot();
        auto ast = snapshot.ast;
        auto types = snapshot.types;
        auto root = snapshot.root;
        const auto& toks = snapshot.tokens;

#if PARUSD_ENABLE_LEI
        std::optional<ParusBundleLintContext> lint_ctx_for_doc{};
        if (!bag.has_error()) {
            lint_ctx_for_doc = build_parus_bundle_lint_context_(uri, lei_overlays);
        }
#endif
        std::unordered_set<uint32_t> core_impl_marker_file_ids{};
        collect_core_impl_marker_file_ids_(ast, root, core_impl_marker_file_ids);
#if PARUSD_ENABLE_LEI
        if (!lint_ctx_for_doc.has_value() && !core_impl_marker_file_ids.empty()) {
            constexpr std::string_view kCoreRootNeedle = "/sysroot/core/";
            const size_t pos = normalized_current.find(kCoreRootNeedle);
            if (pos != std::string::npos) {
                ParusBundleLintContext fallback{};
                fallback.bundle_name = "core";
                fallback.current_source_dir_norm = parent_dir_norm_(normalized_current);
                fallback.allowed_import_heads.insert("core");

                std::string rel = normalized_current.substr(pos + kCoreRootNeedle.size());
                if (!rel.empty()) {
                    const size_t slash = rel.find('/');
                    if (slash != std::string::npos && slash > 0) {
                        fallback.current_module_head = rel.substr(0, slash);
                    } else if (slash == std::string::npos) {
                        fallback.current_module_head = rel;
                    }
                }
                if (fallback.current_module_head.empty()) fallback.current_module_head = "core";
                lint_ctx_for_doc = std::move(fallback);
            }
        }
#endif
        std::vector<CHeaderImportSpec> c_header_imports{};
        collect_c_header_imports_(ast, root, c_header_imports);

        std::unordered_map<uint64_t, SemClass> resolved_map;
        parus::passes::PassResults pass_res{};
        bool has_pass_results = false;
        std::unordered_map<std::string, std::vector<LspLocation>> external_definitions{};
        if (!bag.has_error()) {
            const bool auto_core_macro_injection =
                !env_flag_truthy_(getenv_string_("PARUS_NO_CORE")) &&
                (!lint_ctx_for_doc.has_value() || lint_ctx_for_doc->bundle_name != "core");
            if (auto_core_macro_injection) {
                std::string macro_prelude_err{};
                if (!load_core_macro_prelude_into_ast_(ast, sm, bag, macro_prelude_err)) {
                    parus::diag::Diagnostic d(
                        parus::diag::Severity::kError,
                        parus::diag::Code::kTypeErrorGeneric,
                        parus::Span{file_id, 0, 0}
                    );
                    d.add_arg(macro_prelude_err.empty()
                        ? std::string("failed to load core macro prelude")
                        : macro_prelude_err);
                    bag.add(std::move(d));
                }
            }

            const bool macro_ok = parus::macro::expand_program(ast, types, root, bag, macro_budget);
            if (!bag.has_error() && macro_ok) {
                auto type_resolve = parus::type::resolve_program_types(ast, types, root, bag);
                if (!bag.has_error() && type_resolve.ok) {
                    parus::passes::PassOptions popt{};
                    popt.name_resolve.current_file_id = file_id;
#if PARUSD_ENABLE_LEI
                    if (lint_ctx_for_doc.has_value()) {
                        const auto& lint_ctx = *lint_ctx_for_doc;
                        popt.name_resolve.current_bundle_name = lint_ctx.bundle_name;
                        popt.name_resolve.current_module_head = lint_ctx.current_module_head;
                        popt.name_resolve.current_source_dir_norm = lint_ctx.current_source_dir_norm;
                        popt.name_resolve.allowed_import_heads = lint_ctx.allowed_import_heads;
                        popt.name_resolve.external_exports = lint_ctx.external_exports;
                        for (const auto& [k, v] : lint_ctx.external_decl_locs) {
                            auto& dst = external_definitions[k];
                            for (const auto& loc : v) {
                                LspLocation lsp{};
                                lsp.uri = loc.file_uri;
                                lsp.start_line = loc.line;
                                lsp.start_character = loc.character;
                                lsp.end_line = loc.line;
                                lsp.end_character = loc.character + 1;
                                append_unique_location_(dst, lsp);
                            }
                        }
                        for (const auto& err : lint_ctx.index_load_errors) {
                            parus::diag::Diagnostic d(
                                parus::diag::Severity::kError,
                                parus::diag::Code::kExportIndexSchema,
                                parus::Span{file_id, 0, 0}
                            );
                            d.add_arg(err);
                            bag.add(std::move(d));
                        }
                    }
#endif
                    if (!bag.has_error() && !c_header_imports.empty()) {
                        struct CImportCacheEntry {
                            std::vector<parus::passes::NameResolveOptions::ExternalExport> exports{};
                            std::unordered_map<std::string, std::vector<LspLocation>> defs{};
                        };
                        static std::unordered_map<std::string, CImportCacheEntry> g_lsp_cimport_cache{};

                        std::vector<std::string> cimport_include_dirs{};
                        std::vector<std::string> cimport_isystem_dirs{};
                        std::vector<std::string> cimport_defines{};
                        std::vector<std::string> cimport_undefines{};
                        std::vector<std::string> cimport_forced_includes{};
                        std::vector<std::string> cimport_imacros{};
                        if (!current_dir.empty()) cimport_include_dirs.push_back(parus::normalize_path(current_dir));
                        for (const auto& d : cimport_cfg.include_dirs) {
                            if (!d.empty()) cimport_include_dirs.push_back(parus::normalize_path(d));
                        }
                        parus::cimport::append_unique_normalized_paths(cimport_include_dirs, {});
#if PARUSD_ENABLE_LEI
                        if (lint_ctx_for_doc.has_value()) {
                            parus::cimport::append_unique_normalized_paths(
                                cimport_isystem_dirs,
                                lint_ctx_for_doc->cimport_isystem_dirs
                            );
                        }
#endif
                        for (const auto& d : cimport_cfg.isystem_dirs) {
                            if (!d.empty()) cimport_isystem_dirs.push_back(parus::normalize_path(d));
                        }
                        const auto auto_isystem = parus::cimport::probe_default_c_system_include_dirs();
                        if (!auto_isystem.warning.empty()) {
                            parus::diag::Diagnostic d(
                                parus::diag::Severity::kWarning,
                                parus::diag::Code::kTypeErrorGeneric,
                                parus::Span{file_id, 0, 0}
                            );
                            d.add_arg(auto_isystem.warning);
                            bag.add(std::move(d));
                        }
                        parus::cimport::append_unique_normalized_paths(cimport_isystem_dirs, auto_isystem.isystem_dirs);
                        for (const auto& d : cimport_cfg.defines) {
                            if (!d.empty()) cimport_defines.push_back(d);
                        }
                        for (const auto& d : cimport_cfg.undefines) {
                            if (!d.empty()) cimport_undefines.push_back(d);
                        }
                        for (const auto& d : cimport_cfg.forced_includes) {
                            if (!d.empty()) cimport_forced_includes.push_back(parus::normalize_path(d));
                        }
                        parus::cimport::append_unique_normalized_paths(cimport_forced_includes, {});
                        for (const auto& d : cimport_cfg.imacros) {
                            if (!d.empty()) cimport_imacros.push_back(parus::normalize_path(d));
                        }
                        parus::cimport::append_unique_normalized_paths(cimport_imacros, {});

                        std::string cache_key = normalized_current;
                        cache_key += "|hdr=";
                        for (const auto& spec : c_header_imports) {
                            cache_key += spec.header;
                            cache_key += "@";
                            cache_key += spec.alias;
                            cache_key += ";";
                        }
                        auto append_vec_to_key = [&](std::string_view tag, const std::vector<std::string>& v) {
                            cache_key += "|";
                            cache_key += std::string(tag);
                            cache_key += "=";
                            for (const auto& one : v) {
                                cache_key += one;
                                cache_key += ";";
                            }
                        };
                        append_vec_to_key("I", cimport_include_dirs);
                        append_vec_to_key("S", cimport_isystem_dirs);
                        append_vec_to_key("D", cimport_defines);
                        append_vec_to_key("U", cimport_undefines);
                        append_vec_to_key("F", cimport_forced_includes);
                        append_vec_to_key("M", cimport_imacros);

                        if (auto it = g_lsp_cimport_cache.find(cache_key); it != g_lsp_cimport_cache.end()) {
                            for (const auto& ex : it->second.exports) {
                                popt.name_resolve.external_exports.push_back(ex);
                            }
                            for (const auto& [k, v] : it->second.defs) {
                                auto& dst = external_definitions[k];
                                for (const auto& loc : v) {
                                    append_unique_location_(dst, loc);
                                }
                            }
                        } else {
                            CImportCacheEntry cache_entry{};

                            std::set<std::pair<int, std::string>> cimport_seen{};
                            auto add_decl_loc = [&](const std::string& path,
                                                    const std::string& file,
                                                    uint32_t line,
                                                    uint32_t col) {
                                if (path.empty() || file.empty()) return;
                                LspLocation lsp{};
                                lsp.uri = file_path_to_uri_(file);
                                lsp.start_line = (line > 0) ? (line - 1) : 0;
                                lsp.start_character = (col > 0) ? (col - 1) : 0;
                                lsp.end_line = lsp.start_line;
                                lsp.end_character = lsp.start_character + 1;
                                append_unique_location_(external_definitions[path], lsp);
                                append_unique_location_(cache_entry.defs[path], lsp);
                            };

                            auto add_external = [&](parus::sema::SymbolKind kind,
                                                    std::string path,
                                                    std::string type_repr,
                                                    std::string link_name,
                                                    std::string inst_payload) {
                                if (path.empty() || type_repr.empty()) return;
                                const auto key = std::make_pair(static_cast<int>(kind), path);
                                if (!cimport_seen.insert(key).second) return;
                                parus::passes::NameResolveOptions::ExternalExport ex{};
                                ex.kind = kind;
                                ex.path = std::move(path);
                                ex.link_name = std::move(link_name);
                                ex.declared_type_repr = std::move(type_repr);
                                ex.declared_type = parus::ty::kInvalidType;
                                ex.decl_span = parus::Span{0, 0, 0};
                                ex.decl_bundle_name = "__cimport__";
                                ex.decl_source_dir_norm = current_dir;
                                ex.module_head.clear();
                                ex.is_export = true;
                                ex.inst_payload = std::move(inst_payload);
                                cache_entry.exports.push_back(ex);
                                popt.name_resolve.external_exports.push_back(std::move(ex));
                            };

                            for (const auto& spec : c_header_imports) {
                            const auto imported = parus::cimport::import_c_header_functions(
                                normalized_current,
                                spec.header,
                                cimport_include_dirs,
                                cimport_isystem_dirs,
                                cimport_defines,
                                cimport_undefines,
                                cimport_forced_includes,
                                cimport_imacros
                            );
                            if (imported.error != parus::cimport::ImportErrorKind::kNone) {
                                parus::diag::Diagnostic d(
                                    parus::diag::Severity::kError,
                                    imported.error == parus::cimport::ImportErrorKind::kLibClangUnavailable
                                        ? parus::diag::Code::kCImportLibClangUnavailable
                                        : parus::diag::Code::kTypeErrorGeneric,
                                    spec.span
                                );
                                if (imported.error == parus::cimport::ImportErrorKind::kLibClangUnavailable) {
                                    if (!imported.error_text.empty()) {
                                        d.add_arg(imported.error_text);
                                    } else {
                                        d.add_arg("libclang unavailable");
                                    }
                                } else {
                                    d.add_arg("failed to import C header '" + spec.header + "': " + imported.error_text);
                                }
                                bag.add(std::move(d));
                                continue;
                            }

                            if (imported.coverage.skipped_function_macros > 0) {
                                std::string msg = "some function-like C macros were skipped: ";
                                constexpr size_t kPreview = 3;
                                size_t printed = 0;
                                for (size_t i = 0;
                                     i < imported.coverage.skipped_reasons.size() && printed < kPreview;
                                     ++i, ++printed) {
                                    if (printed) msg += "; ";
                                    msg += imported.coverage.skipped_reasons[i];
                                }
                                if (printed == 0) {
                                    for (size_t i = 0;
                                         i < imported.coverage.skipped_reason_codes.size() && printed < kPreview;
                                         ++i, ++printed) {
                                        if (printed) msg += "; ";
                                        msg += imported.coverage.skipped_reason_codes[i];
                                    }
                                }
                                if (imported.coverage.skipped_function_macros > printed) {
                                    msg += "; ...";
                                }
                                parus::diag::Diagnostic d(
                                    parus::diag::Severity::kWarning,
                                    parus::diag::Code::kCImportFnMacroSkipped,
                                    spec.span
                                );
                                d.add_arg(msg);
                                bag.add(std::move(d));
                            }

                            std::unordered_set<std::string> known_type_names{};
                            for (const auto& un : imported.unions) {
                                if (!un.name.empty()) known_type_names.insert(un.name);
                            }
                            for (const auto& st : imported.structs) {
                                if (!st.name.empty()) known_type_names.insert(st.name);
                            }
                            for (const auto& en : imported.enums) {
                                if (!en.name.empty()) known_type_names.insert(en.name);
                            }
                            for (const auto& td : imported.typedefs) {
                                if (!td.name.empty()) known_type_names.insert(td.name);
                            }

                            std::unordered_map<std::string, const parus::cimport::ImportedFunctionDecl*> imported_fn_by_name{};
                            imported_fn_by_name.reserve(imported.functions.size() * 2u + 1u);
                            for (const auto& fn : imported.functions) {
                                if (!fn.name.empty()) imported_fn_by_name.emplace(fn.name, &fn);
                                if (fn.name.empty() || fn.type_repr.empty() || fn.link_name.empty()) continue;
                                const std::string path = spec.alias + "::" + fn.name;
                                add_external(
                                    parus::sema::SymbolKind::kFn,
                                    path,
                                    parus::cimport::rewrite_cimport_type_with_alias(fn.type_repr, spec.alias, known_type_names),
                                    fn.link_name,
                                    parus::cimport::make_c_import_payload(spec.header, spec.alias, fn)
                                );
                                add_decl_loc(path, fn.decl_file, fn.decl_line, fn.decl_col);
                            }

                            for (const auto& gv : imported.globals) {
                                if (gv.name.empty() || gv.type_repr.empty()) continue;
                                const std::string path = spec.alias + "::" + gv.name;
                                add_external(
                                    parus::sema::SymbolKind::kVar,
                                    path,
                                    parus::cimport::rewrite_cimport_type_with_alias(gv.type_repr, spec.alias, known_type_names),
                                    gv.link_name.empty() ? gv.name : gv.link_name,
                                    parus::cimport::make_c_import_global_payload(spec.header, gv)
                                );
                                add_decl_loc(path, gv.decl_file, gv.decl_line, gv.decl_col);
                            }

                            for (const auto& un : imported.unions) {
                                if (un.name.empty()) continue;
                                const std::string type_path = spec.alias + "::" + un.name;
                                add_external(
                                    parus::sema::SymbolKind::kType,
                                    type_path,
                                    type_path,
                                    {},
                                    parus::cimport::make_c_import_union_payload(spec.header, spec.alias, known_type_names, un)
                                );
                                add_decl_loc(type_path, un.decl_file, un.decl_line, un.decl_col);
                            }

                            for (const auto& st : imported.structs) {
                                if (st.name.empty()) continue;
                                const std::string type_path = spec.alias + "::" + st.name;
                                add_external(
                                    parus::sema::SymbolKind::kType,
                                    type_path,
                                    type_path,
                                    {},
                                    parus::cimport::make_c_import_struct_payload(spec.header, spec.alias, known_type_names, st)
                                );
                                add_decl_loc(type_path, st.decl_file, st.decl_line, st.decl_col);
                            }

                            for (const auto& td : imported.typedefs) {
                                if (td.name.empty() || td.type_repr.empty()) continue;
                                const std::string type_path = spec.alias + "::" + td.name;
                                add_external(
                                    parus::sema::SymbolKind::kType,
                                    type_path,
                                    parus::cimport::rewrite_cimport_type_with_alias(td.type_repr, spec.alias, known_type_names),
                                    {},
                                    parus::cimport::make_c_import_typedef_payload(spec.header, spec.alias, known_type_names, td)
                                );
                                add_decl_loc(type_path, td.decl_file, td.decl_line, td.decl_col);
                            }

                            for (const auto& en : imported.enums) {
                                if (en.name.empty()) continue;
                                const std::string enum_path = spec.alias + "::" + en.name;
                                add_external(parus::sema::SymbolKind::kType, enum_path, enum_path, {}, {});
                                add_decl_loc(enum_path, en.decl_file, en.decl_line, en.decl_col);

                                const std::string const_ty = parus::cimport::rewrite_cimport_type_with_alias(
                                    en.underlying_type_repr.empty()
                                        ? std::string_view("i32")
                                        : std::string_view(en.underlying_type_repr),
                                    spec.alias,
                                    known_type_names
                                );
                                for (const auto& cst : en.constants) {
                                    if (cst.name.empty() || cst.value_text.empty()) continue;
                                    const std::string cpath = spec.alias + "::" + cst.name;
                                    add_external(
                                        parus::sema::SymbolKind::kVar,
                                        cpath,
                                        const_ty,
                                        {},
                                        parus::cimport::make_c_import_const_payload("int", cst.value_text)
                                    );
                                    add_decl_loc(cpath, cst.decl_file, cst.decl_line, cst.decl_col);
                                }
                            }

                            for (const auto& mc : imported.macros) {
                                if (mc.name.empty()) continue;
                                if (mc.is_function_like) {
                                    if (mc.promote_kind == parus::cimport::ImportedMacroPromoteKind::kDirectAlias) {
                                        const auto it = imported_fn_by_name.find(mc.promote_callee_name);
                                        if (it != imported_fn_by_name.end() && it->second != nullptr) {
                                            const auto* callee = it->second;
                                            const std::string mpath = spec.alias + "::" + mc.name;
                                            add_external(
                                                parus::sema::SymbolKind::kFn,
                                                mpath,
                                                parus::cimport::rewrite_cimport_type_with_alias(callee->type_repr, spec.alias, known_type_names),
                                                callee->link_name,
                                                parus::cimport::make_c_import_payload(spec.header, spec.alias, *callee)
                                            );
                                            add_decl_loc(mpath, mc.decl_file, mc.decl_line, mc.decl_col);
                                        }
                                    } else if (mc.promote_kind == parus::cimport::ImportedMacroPromoteKind::kShimForward &&
                                               !mc.promote_type_repr.empty()) {
                                        const std::string mpath = spec.alias + "::" + mc.name;
                                        parus::cimport::ImportedFunctionDecl shim_fn{};
                                        shim_fn.name = mc.name;
                                        shim_fn.link_name = "__parusd_lsp_cimport_shim_" + spec.alias + "_" + mc.name;
                                        shim_fn.type_repr = mc.promote_type_repr;
                                        shim_fn.is_c_abi = true;
                                        shim_fn.is_variadic = false;
                                        add_external(
                                            parus::sema::SymbolKind::kFn,
                                            mpath,
                                            parus::cimport::rewrite_cimport_type_with_alias(mc.promote_type_repr, spec.alias, known_type_names),
                                            shim_fn.link_name,
                                            parus::cimport::make_c_import_payload(spec.header, spec.alias, shim_fn)
                                        );
                                        add_decl_loc(mpath, mc.decl_file, mc.decl_line, mc.decl_col);
                                    }
                                    continue;
                                }
                                if (mc.const_kind == parus::cimport::ImportedConstKind::kNone) continue;
                                const std::string cpath = spec.alias + "::" + mc.name;
                                std::string ty = "i64";
                                std::string payload_kind = "int";
                                switch (mc.const_kind) {
                                    case parus::cimport::ImportedConstKind::kInt:
                                        ty = "i64";
                                        payload_kind = "int";
                                        break;
                                    case parus::cimport::ImportedConstKind::kFloat:
                                        ty = "f64";
                                        payload_kind = "float";
                                        break;
                                    case parus::cimport::ImportedConstKind::kBool:
                                        ty = "bool";
                                        payload_kind = "bool";
                                        break;
                                    case parus::cimport::ImportedConstKind::kChar:
                                        ty = "char";
                                        payload_kind = "char";
                                        break;
                                    case parus::cimport::ImportedConstKind::kString:
                                        ty = "ptr i8";
                                        payload_kind = "string";
                                        break;
                                    case parus::cimport::ImportedConstKind::kNone:
                                    default:
                                        continue;
                                }
                                add_external(
                                    parus::sema::SymbolKind::kVar,
                                    cpath,
                                    ty,
                                    {},
                                    parus::cimport::make_c_import_const_payload(payload_kind, mc.value_text)
                                );
                                add_decl_loc(cpath, mc.decl_file, mc.decl_line, mc.decl_col);
                            }
                            }
                            g_lsp_cimport_cache[cache_key] = std::move(cache_entry);
                        }
                    }
                    if (!bag.has_error()) {
                        const bool auto_core_injection =
                            !env_flag_truthy_(getenv_string_("PARUS_NO_CORE")) &&
                            popt.name_resolve.current_bundle_name != "core";
                        if (auto_core_injection) {
                            popt.name_resolve.allowed_import_heads.insert("core");
                            popt.name_resolve.implicit_import_aliases["core"] = "core";
                            popt.name_resolve.warn_core_path_when_std = true;

                            const std::string core_index_path = resolve_core_export_index_path_();
                            if (!core_index_path.empty()) {
                                std::unordered_map<std::string, std::vector<ExternalDeclLocation>> core_decl_locs{};
                                std::string load_err{};
                                if (!load_export_index_for_lint_(
                                        std::filesystem::path(core_index_path),
                                        /*fallback_bundle_name=*/"core",
                                        popt.name_resolve.current_module_head,
                                        /*same_bundle=*/false,
                                        popt.name_resolve.external_exports,
                                        &core_decl_locs,
                                        &load_err)) {
                                    const parus::diag::Code code = load_err.find("failed to read export-index") != std::string::npos
                                        ? parus::diag::Code::kExportIndexMissing
                                        : parus::diag::Code::kExportIndexSchema;
                                    parus::diag::Diagnostic d(
                                        parus::diag::Severity::kError,
                                        code,
                                        parus::Span{file_id, 0, 0}
                                    );
                                    d.add_arg(load_err);
                                    bag.add(std::move(d));
                                } else {
                                    for (const auto& [k, v] : core_decl_locs) {
                                        auto& dst = external_definitions[k];
                                        for (const auto& loc : v) {
                                            LspLocation lsp{};
                                            lsp.uri = loc.file_uri;
                                            lsp.start_line = loc.line;
                                            lsp.start_character = loc.character;
                                            lsp.end_line = loc.line;
                                            lsp.end_character = loc.character + 1;
                                            append_unique_location_(dst, lsp);
                                        }
                                    }
                                }
                            }
                        }
                    }
                    for (auto& ex : popt.name_resolve.external_exports) {
                        if (ex.declared_type == parus::ty::kInvalidType) {
                            if (!ex.declared_type_repr.empty()) {
                                ex.declared_type = parse_type_repr_for_lint_(
                                    ex.declared_type_repr,
                                    ex.inst_payload,
                                    types
                                );
                            }
                            if (ex.declared_type == parus::ty::kInvalidType) {
                                if (ex.kind == parus::sema::SymbolKind::kFn) {
                                    ex.declared_type = types.make_fn(types.error(), nullptr, 0);
                                } else {
                                    ex.declared_type = types.error();
                                }
                            }
                        }
                    }
                    if (!bag.has_error()) {
                        pass_res = parus::passes::run_on_program(ast, root, bag, popt);
                        has_pass_results = true;
                        resolved_map = collect_resolved_semantic_map_(pass_res.name_resolve);

                        if (!bag.has_error()) {
                            parus::tyck::TypeChecker tc(ast, types, bag, &type_resolve, &pass_res.generic_prep);
                            if (!popt.name_resolve.current_bundle_name.empty() ||
                                !popt.name_resolve.external_exports.empty()) {
                                tc.set_seed_symbol_table(&pass_res.sym);
                            }
                            if (!core_impl_marker_file_ids.empty()) {
                                tc.set_core_impl_marker_file_ids(std::move(core_impl_marker_file_ids));
                            }
                            const auto ty = tc.check_program(root);

                            if (!bag.has_error() && ty.errors.empty()) {
                                (void)parus::cap::run_capability_check(ast, root, pass_res.name_resolve, ty, types, bag);
                            }
                        }
                    }
                }
            }
        }

        {
            std::vector<std::string> ns_stack{};
            collect_parus_top_level_symbols_stmt_(
                ast,
                root,
                sm,
                uri,
                ns_stack,
                out.completion_items,
                out.top_level_definitions
            );
            append_keyword_completions_(kParusCompletionKeywords, out.completion_items);
            for (const auto& [k, locs] : external_definitions) {
                auto& dst = out.top_level_definitions[k];
                for (const auto& loc : locs) append_unique_location_(dst, loc);
            }
            if (has_pass_results) {
                collect_parus_definition_bindings_(
                    pass_res.name_resolve,
                    pass_res.sym,
                    sm,
                    file_id,
                    uri,
                    external_definitions.empty() ? nullptr : &external_definitions,
                    out.definition_bindings
                );
            }
        }

        const auto decl_map = collect_decl_semantic_map_(toks);
        out.semantic_tokens.reserve(toks.size());
        using K = parus::syntax::TokenKind;

        for (size_t i = 0; i < toks.size(); ++i) {
            const auto& tok = toks[i];
            if (tok.kind == K::kEof || tok.kind == K::kError) continue;

            const auto prev_kind = (i > 0) ? toks[i - 1].kind : K::kError;
            const auto next_kind = (i + 1 < toks.size()) ? toks[i + 1].kind : K::kError;
            SemClass sem_class{};
            bool has_sem_class = false;

            if (tok.kind == K::kIdent || tok.kind == K::kHole) {
                const uint64_t key = sem_span_key_(tok.span);
                const auto decl_it = decl_map.find(key);
                if (decl_it != decl_map.end()) {
                    sem_class = decl_it->second;
                    has_sem_class = true;
                } else {
                    const auto resolved_it = resolved_map.find(key);
                    if (resolved_it != resolved_map.end()) {
                        sem_class = resolved_it->second;
                        has_sem_class = true;
                    }
                }

                if (!has_sem_class) {
                    if (next_kind == K::kLParen) {
                        sem_class = SemClass{static_cast<uint32_t>(SemTokenType::kFunction), 0};
                    } else if (next_kind == K::kColonColon || prev_kind == K::kColonColon) {
                        sem_class = SemClass{static_cast<uint32_t>(SemTokenType::kNamespace), 0};
                    } else if (
                        prev_kind == K::kColon ||
                        prev_kind == K::kArrow ||
                        prev_kind == K::kKwAs ||
                        prev_kind == K::kKwRecast
                    ) {
                        sem_class = SemClass{static_cast<uint32_t>(SemTokenType::kType), 0};
                    } else if (prev_kind == K::kKwField) {
                        sem_class = SemClass{static_cast<uint32_t>(SemTokenType::kType), kSemModDeclaration};
                    } else if (prev_kind == K::kKwActs || prev_kind == K::kKwClass || prev_kind == K::kKwActor) {
                        sem_class = SemClass{static_cast<uint32_t>(SemTokenType::kClass), kSemModDeclaration};
                    } else if (prev_kind == K::kKwProto) {
                        sem_class = SemClass{static_cast<uint32_t>(SemTokenType::kType), kSemModDeclaration};
                    } else {
                        sem_class = SemClass{static_cast<uint32_t>(SemTokenType::kVariable), 0};
                    }
                    has_sem_class = true;
                }
            } else if (tok.kind == K::kIntLit || tok.kind == K::kFloatLit) {
                sem_class = SemClass{static_cast<uint32_t>(SemTokenType::kNumber), 0};
                has_sem_class = true;
            } else if (tok.kind == K::kStringLit || tok.kind == K::kCharLit) {
                sem_class = SemClass{static_cast<uint32_t>(SemTokenType::kString), 0};
                has_sem_class = true;
            } else if (tok.kind == K::kAt) {
                sem_class = SemClass{static_cast<uint32_t>(SemTokenType::kDecorator), 0};
                has_sem_class = true;
            } else if (is_keyword_token_kind_(tok.kind)) {
                sem_class = SemClass{static_cast<uint32_t>(SemTokenType::kKeyword), 0};
                has_sem_class = true;
            } else if (is_operator_token_kind_(tok.kind)) {
                sem_class = SemClass{static_cast<uint32_t>(SemTokenType::kOperator), 0};
                has_sem_class = true;
            }

            if (!has_sem_class) continue;

            SemToken sem_tok{};
            if (sem_token_from_span_(sm, tok.span, sem_class, sem_tok)) {
                out.semantic_tokens.push_back(sem_tok);
            }
        }

        out.diagnostics.reserve(bag.diags().size());
        for (const auto& d : bag.diags()) {
            const auto sp = d.span();
            const uint32_t end_off = (sp.hi >= sp.lo) ? sp.hi : sp.lo;
            const auto begin_lc = sm.line_col(sp.file_id, sp.lo);
            const auto end_lc = sm.line_col(sp.file_id, end_off);

            LspDiag ld{};
            ld.start_line = (begin_lc.line > 0) ? (begin_lc.line - 1) : 0;
            ld.start_character = (begin_lc.col > 0) ? (begin_lc.col - 1) : 0;
            ld.end_line = (end_lc.line > 0) ? (end_lc.line - 1) : 0;
            ld.end_character = (end_lc.col > 0) ? (end_lc.col - 1) : 0;
            ld.severity = to_lsp_severity_(d.severity());
            ld.code = parus::diag::code_name(d.code());
            ld.message = parus::diag::render_message(d, parus::diag::Language::kEn);
            out.diagnostics.push_back(std::move(ld));
        }

        return out;
    }

#if PARUSD_ENABLE_LEI
    bool is_lei_keyword_token_kind_(lei::syntax::TokenKind kind) {
        using K = lei::syntax::TokenKind;
        switch (kind) {
            case K::kKwImport:
            case K::kKwFrom:
            case K::kKwExport:
            case K::kKwProto:
            case K::kKwPlan:
            case K::kKwLet:
            case K::kKwVar:
            case K::kKwDef:
            case K::kKwAssert:
            case K::kKwIf:
            case K::kKwElse:
            case K::kKwTrue:
            case K::kKwFalse:
            case K::kKwInt:
            case K::kKwFloat:
            case K::kKwString:
            case K::kKwBool:
            case K::kKwReturn:
            case K::kKwFor:
            case K::kKwIn:
                return true;
            default:
                return false;
        }
    }

    bool is_lei_operator_token_kind_(lei::syntax::TokenKind kind) {
        using K = lei::syntax::TokenKind;
        switch (kind) {
            case K::kLParen:
            case K::kRParen:
            case K::kLBrace:
            case K::kRBrace:
            case K::kLBracket:
            case K::kRBracket:
            case K::kComma:
            case K::kColon:
            case K::kSemicolon:
            case K::kDot:
            case K::kAssign:
            case K::kArrow:
            case K::kPlus:
            case K::kMinus:
            case K::kStar:
            case K::kSlash:
            case K::kAndAnd:
            case K::kOrOr:
            case K::kAmp:
            case K::kEqEq:
            case K::kBangEq:
            case K::kBang:
            case K::kColonColon:
                return true;
            default:
                return false;
        }
    }

    uint32_t lei_token_length_(const lei::syntax::Token& tok) {
        if (tok.kind == lei::syntax::TokenKind::kStringLit) {
            if (tok.lexeme.find('\n') != std::string::npos || tok.lexeme.find('\r') != std::string::npos) {
                return 1;
            }
            const uint32_t body_len = static_cast<uint32_t>(tok.lexeme.size());
            return std::max(1u, body_len + 2u);
        }

        if (!tok.lexeme.empty()) {
            return std::max(1u, static_cast<uint32_t>(tok.lexeme.size()));
        }

        const auto token_name = lei::syntax::token_kind_name(tok.kind);
        if (token_name.empty() || token_name == "unknown") return 1;
        return std::max(1u, static_cast<uint32_t>(token_name.size()));
    }

    bool sem_token_from_lei_token_(
        const lei::syntax::Token& tok,
        const SemClass& sem_class,
        SemToken& out
    ) {
        if (tok.loc.line == 0 || tok.loc.column == 0) return false;
        SemToken next{};
        next.line = tok.loc.line - 1;
        next.start_character = tok.loc.column - 1;
        next.length = lei_token_length_(tok);
        next.token_type = sem_class.token_type;
        next.token_modifiers = sem_class.token_modifiers;
        out = next;
        return true;
    }

    std::vector<bool> collect_lei_parameter_declarations_(
        const std::vector<lei::syntax::Token>& toks
    ) {
        using K = lei::syntax::TokenKind;
        std::vector<bool> out(toks.size(), false);

        for (size_t i = 0; i < toks.size(); ++i) {
            if (toks[i].kind != K::kKwDef) continue;

            size_t lparen_idx = i + 1;
            while (lparen_idx < toks.size()) {
                const auto kind = toks[lparen_idx].kind;
                if (kind == K::kLParen) break;
                if (kind == K::kLBrace || kind == K::kSemicolon || kind == K::kEof) break;
                ++lparen_idx;
            }
            if (lparen_idx >= toks.size() || toks[lparen_idx].kind != K::kLParen) continue;

            uint32_t depth = 0;
            for (size_t j = lparen_idx; j < toks.size(); ++j) {
                const auto kind = toks[j].kind;
                if (kind == K::kLParen) {
                    ++depth;
                    continue;
                }
                if (kind == K::kRParen) {
                    if (depth == 0) break;
                    --depth;
                    if (depth == 0) break;
                    continue;
                }
                if (depth != 1) continue;
                if (kind != K::kIdent) continue;
                if (j + 1 < toks.size() && toks[j + 1].kind == K::kColon) {
                    out[j] = true;
                }
            }
        }
        return out;
    }

    void collect_lei_completion_and_definitions_(
        const std::vector<lei::syntax::Token>& toks,
        std::string_view uri,
        std::vector<CompletionEntry>& completion_items,
        std::unordered_map<std::string, std::vector<LspLocation>>& top_level_definitions
    ) {
        append_keyword_completions_(kLeiCompletionKeywords, completion_items);
        using K = lei::syntax::TokenKind;

        auto add_decl = [&](const lei::syntax::Token& name_tok, uint32_t kind, std::string_view detail) {
            if (name_tok.kind != K::kIdent || name_tok.lexeme.empty()) return;
            append_completion_entry_unique_(completion_items, name_tok.lexeme, kind, detail);
            LspLocation loc{};
            loc.uri = std::string(uri);
            loc.start_line = (name_tok.loc.line > 0) ? (name_tok.loc.line - 1) : 0;
            loc.start_character = (name_tok.loc.column > 0) ? (name_tok.loc.column - 1) : 0;
            const uint32_t len = std::max<uint32_t>(1, static_cast<uint32_t>(name_tok.lexeme.size()));
            loc.end_line = loc.start_line;
            loc.end_character = loc.start_character + len;
            append_definition_target_(top_level_definitions, name_tok.lexeme, loc);
        };

        for (size_t i = 0; i < toks.size(); ++i) {
            const auto& tok = toks[i];
            if (tok.kind == K::kKwDef && i + 1 < toks.size()) {
                add_decl(toks[i + 1], 3, "function");
                continue;
            }
            if (tok.kind == K::kKwProto && i + 1 < toks.size()) {
                add_decl(toks[i + 1], 8, "proto");
                continue;
            }
            if (tok.kind == K::kKwPlan && i + 1 < toks.size()) {
                add_decl(toks[i + 1], 9, "plan");
                continue;
            }
            if ((tok.kind == K::kKwLet || tok.kind == K::kKwVar) && i + 1 < toks.size()) {
                add_decl(toks[i + 1], 6, "variable");
                continue;
            }
            if (tok.kind == K::kKwImport) {
                for (size_t j = i + 1; j < toks.size(); ++j) {
                    if (toks[j].kind == K::kSemicolon) break;
                    if (toks[j].kind == K::kKwFrom && j > i + 1 && toks[j - 1].kind == K::kIdent) {
                        add_decl(toks[j - 1], 9, "import alias");
                        break;
                    }
                }
            }
        }
    }

    [[maybe_unused]] std::vector<SemToken> semantic_tokens_for_lei_document_(
        std::string_view source,
        std::string_view file_path
    ) {
        std::vector<SemToken> out{};
        lei::diag::Bag lex_bag;
        const auto toks = lei::parse::lex(source, file_path, lex_bag);
        (void)lex_bag;
        if (toks.empty()) return out;

        const auto parameter_decl = collect_lei_parameter_declarations_(toks);
        out.reserve(toks.size());

        using K = lei::syntax::TokenKind;
        for (size_t i = 0; i < toks.size(); ++i) {
            const auto& tok = toks[i];
            if (tok.kind == K::kEof || tok.kind == K::kError) continue;

            const auto prev_kind = (i > 0) ? toks[i - 1].kind : K::kError;
            const auto next_kind = (i + 1 < toks.size()) ? toks[i + 1].kind : K::kError;

            SemClass sem_class{};
            bool has_sem_class = false;

            if (tok.kind == K::kIdent) {
                if (prev_kind == K::kKwDef) {
                    sem_class = SemClass{
                        static_cast<uint32_t>(SemTokenType::kFunction),
                        kSemModDeclaration,
                    };
                    has_sem_class = true;
                } else if (parameter_decl[i]) {
                    sem_class = SemClass{
                        static_cast<uint32_t>(SemTokenType::kParameter),
                        kSemModDeclaration,
                    };
                    has_sem_class = true;
                } else if (prev_kind == K::kKwLet || prev_kind == K::kKwVar || prev_kind == K::kKwFor) {
                    sem_class = SemClass{
                        static_cast<uint32_t>(SemTokenType::kVariable),
                        kSemModDeclaration,
                    };
                    has_sem_class = true;
                } else if (next_kind == K::kLParen) {
                    sem_class = SemClass{
                        static_cast<uint32_t>(SemTokenType::kFunction),
                        0,
                    };
                    has_sem_class = true;
                } else {
                    sem_class = SemClass{
                        static_cast<uint32_t>(SemTokenType::kVariable),
                        0,
                    };
                    has_sem_class = true;
                }
            } else if (tok.kind == K::kIntLit || tok.kind == K::kFloatLit) {
                sem_class = SemClass{
                    static_cast<uint32_t>(SemTokenType::kNumber),
                    0,
                };
                has_sem_class = true;
            } else if (tok.kind == K::kStringLit) {
                sem_class = SemClass{
                    static_cast<uint32_t>(SemTokenType::kString),
                    0,
                };
                has_sem_class = true;
            } else if (is_lei_keyword_token_kind_(tok.kind)) {
                sem_class = SemClass{
                    static_cast<uint32_t>(SemTokenType::kKeyword),
                    0,
                };
                has_sem_class = true;
            } else if (is_lei_operator_token_kind_(tok.kind)) {
                sem_class = SemClass{
                    static_cast<uint32_t>(SemTokenType::kOperator),
                    0,
                };
                has_sem_class = true;
            }

            if (!has_sem_class) continue;

            SemToken sem_tok{};
            if (sem_token_from_lei_token_(tok, sem_class, sem_tok)) {
                out.push_back(sem_tok);
            }
        }

        return out;
    }

    std::string lei_diagnostic_dedupe_key_(const lei::diag::Diagnostic& d) {
        return std::string(lei::diag::code_name(d.code))
            + "|"
            + std::to_string(d.line)
            + "|"
            + std::to_string(d.column)
            + "|"
            + d.message;
    }

    bool lei_diagnostic_matches_current_file_(
        const lei::diag::Diagnostic& d,
        std::string_view normalized_current_file
    ) {
        if (normalized_current_file.empty()) return true;
        if (d.file.empty()) return false;

        std::string normalized_file = d.file;
        if (normalized_file.starts_with("file://")) {
            const auto decoded = uri_to_file_path_(normalized_file);
            if (!decoded.has_value()) return false;
            normalized_file = *decoded;
        }
        normalized_file = normalize_host_path_(normalized_file);
        return normalized_file == normalized_current_file;
    }

    void append_lei_diagnostic_(
        std::vector<LspDiag>& out,
        std::unordered_set<std::string>& dedupe,
        const lei::diag::Diagnostic& d
    ) {
        const std::string key = lei_diagnostic_dedupe_key_(d);
        if (!dedupe.insert(key).second) return;

        LspDiag ld{};
        ld.start_line = (d.line > 0) ? (d.line - 1) : 0;
        ld.start_character = (d.column > 0) ? (d.column - 1) : 0;
        ld.end_line = ld.start_line;
        ld.end_character = ld.start_character + 1;
        ld.severity = 1;
        ld.code = lei::diag::code_name(d.code);
        ld.message = d.message;
        out.push_back(std::move(ld));
    }
#endif

    AnalysisResult analyze_lei_document_(
        std::string_view uri,
        DocumentState& doc,
        const std::unordered_map<std::string, std::string>& overlays
    ) {
        AnalysisResult out;
#if PARUSD_ENABLE_LEI
        std::string parsed_file = std::string(uri);
        std::string normalized_current_file{};
        if (const auto fs_path = uri_to_file_path_(uri); fs_path.has_value()) {
            normalized_current_file = normalize_host_path_(*fs_path);
            parsed_file = normalized_current_file;
        }

        std::unordered_set<std::string> dedupe{};

        lei::diag::Bag parse_bag;
        const auto toks = lei::parse::lex(doc.text, parsed_file, parse_bag);
        (void)lei::parse::parse_source(doc.text, parsed_file, parse_bag);
        out.diagnostics.reserve(parse_bag.all().size());
        for (const auto& d : parse_bag.all()) {
            append_lei_diagnostic_(out.diagnostics, dedupe, d);
        }
        collect_lei_completion_and_definitions_(
            toks,
            uri,
            out.completion_items,
            out.top_level_definitions
        );

        // v1 LEI LSP semantic tokens: intentionally empty for stability.
        out.semantic_tokens.clear();

        if (!normalized_current_file.empty()) {
            lei::diag::Bag eval_bag;
            lei::eval::EvaluatorBudget budget{};
            auto builtins = lei::eval::make_default_builtin_registry();
            auto builtin_plans = lei::eval::make_default_builtin_plan_registry();
            lei::parse::ParserControl parser_control{};
            lei::eval::Evaluator evaluator(
                budget,
                eval_bag,
                std::move(builtins),
                std::move(builtin_plans),
                parser_control
            );

            lei::eval::EvaluateOptions eval_options{};
            eval_options.entry_plan = "master";
            eval_options.source_overlay = [&overlays](std::string_view normalized_path) -> std::optional<std::string> {
                const auto it = overlays.find(std::string(normalized_path));
                if (it == overlays.end()) return std::nullopt;
                return it->second;
            };

            (void)evaluator.evaluate_entry(std::filesystem::path(normalized_current_file), eval_options);
            for (const auto& d : eval_bag.all()) {
                if (!lei_diagnostic_matches_current_file_(d, normalized_current_file)) {
                    continue;
                }
                append_lei_diagnostic_(out.diagnostics, dedupe, d);
            }
        }
#else
        (void)doc;
        (void)uri;
        (void)overlays;
        append_keyword_completions_(kLeiCompletionKeywords, out.completion_items);
        LspDiag ld{};
        ld.start_line = 0;
        ld.start_character = 0;
        ld.end_line = 0;
        ld.end_character = 1;
        ld.severity = 1;
        ld.code = "LSP_LEI_NOT_BUILT";
        ld.message = "LEI support is not built in this parusd binary (PARUS_BUILD_LEI=OFF)";
        out.diagnostics.push_back(std::move(ld));
#endif
        return out;
    }

    std::unordered_map<std::string, std::string> build_lei_overlay_map_(
        const std::unordered_map<std::string, DocumentState>& documents
    ) {
        std::unordered_map<std::string, std::string> out{};
        out.reserve(documents.size());
        for (const auto& [doc_uri, state] : documents) {
            if (state.lang != DocLang::kLei) continue;
            const auto fs_path = uri_to_file_path_(doc_uri);
            if (!fs_path.has_value()) continue;
            out.insert_or_assign(normalize_host_path_(*fs_path), state.text);
        }
        return out;
    }

    AnalysisResult analyze_document_(
        std::string_view uri,
        DocumentState& doc,
        const parus::macro::ExpansionBudget& macro_budget,
        const ServerCImportConfig& cimport_cfg,
        const std::unordered_map<std::string, std::string>* lei_overlays
    ) {
        switch (doc.lang) {
            case DocLang::kParus:
                return analyze_parus_document_(uri, doc, macro_budget, cimport_cfg, lei_overlays);
            case DocLang::kLei: {
                static const std::unordered_map<std::string, std::string> kEmptyOverlays{};
                return analyze_lei_document_(
                    uri,
                    doc,
                    (lei_overlays != nullptr) ? *lei_overlays : kEmptyOverlays
                );
            }
            case DocLang::kUnknown:
            default:
                return AnalysisResult{};
        }
    }

    const char* reparse_mode_name_(parus::parse::ReparseMode mode) {
        switch (mode) {
            case parus::parse::ReparseMode::kFullRebuild: return "full";
            case parus::parse::ReparseMode::kIncrementalMerge: return "incremental";
            case parus::parse::ReparseMode::kFallbackFullRebuild: return "fallback-full";
            case parus::parse::ReparseMode::kNone:
            default:
                return "none";
        }
    }

    std::vector<uint32_t> encode_semantic_tokens_data_(std::vector<SemToken> toks) {
        std::sort(toks.begin(), toks.end(), [](const SemToken& a, const SemToken& b) {
            if (a.line != b.line) return a.line < b.line;
            if (a.start_character != b.start_character) return a.start_character < b.start_character;
            if (a.length != b.length) return a.length < b.length;
            if (a.token_type != b.token_type) return a.token_type < b.token_type;
            return a.token_modifiers < b.token_modifiers;
        });

        std::vector<uint32_t> data;
        data.reserve(toks.size() * 5);

        uint32_t prev_line = 0;
        uint32_t prev_start = 0;
        bool first = true;

        for (const auto& tok : toks) {
            const uint32_t delta_line = first ? tok.line : (tok.line - prev_line);
            const uint32_t delta_start = (first || delta_line != 0)
                ? tok.start_character
                : (tok.start_character - prev_start);

            data.push_back(delta_line);
            data.push_back(delta_start);
            data.push_back(tok.length);
            data.push_back(tok.token_type);
            data.push_back(tok.token_modifiers);

            prev_line = tok.line;
            prev_start = tok.start_character;
            first = false;
        }

        return data;
    }

    std::string build_semantic_tokens_result_(const std::vector<SemToken>& toks) {
        const auto data = encode_semantic_tokens_data_(toks);
        std::string json = "{\"data\":[";
        for (size_t i = 0; i < data.size(); ++i) {
            if (i != 0) json += ",";
            json += std::to_string(data[i]);
        }
        json += "]}";
        return json;
    }

    bool is_ident_char_(char ch) {
        const unsigned char u = static_cast<unsigned char>(ch);
        return std::isalnum(u) || ch == '_';
    }

    std::string symbol_prefix_before_offset_(std::string_view text, size_t off) {
        if (off > text.size()) off = text.size();
        size_t begin = off;
        while (begin > 0) {
            const char ch = text[begin - 1];
            if (is_ident_char_(ch) || ch == ':') {
                --begin;
                continue;
            }
            break;
        }
        return std::string(text.substr(begin, off - begin));
    }

    std::string symbol_at_offset_(std::string_view text, size_t off) {
        if (off > text.size()) off = text.size();
        size_t begin = off;
        while (begin > 0) {
            const char ch = text[begin - 1];
            if (is_ident_char_(ch) || ch == ':') {
                --begin;
                continue;
            }
            break;
        }
        size_t end = off;
        while (end < text.size()) {
            const char ch = text[end];
            if (is_ident_char_(ch) || ch == ':') {
                ++end;
                continue;
            }
            break;
        }
        return std::string(text.substr(begin, end - begin));
    }

    std::string build_completion_result_(
        const std::vector<CompletionEntry>& items,
        std::string_view prefix
    ) {
        std::string json = "[";
        std::unordered_set<std::string> seen{};
        bool first = true;
        size_t emitted = 0;
        for (const auto& it : items) {
            if (it.label.empty()) continue;
            if (!prefix.empty() && !it.label.starts_with(prefix)) continue;
            if (!seen.insert(it.label).second) continue;

            if (!first) json += ",";
            first = false;
            json += "{";
            json += "\"label\":\"" + json_escape_(it.label) + "\",";
            json += "\"kind\":" + std::to_string(it.kind);
            if (!it.detail.empty()) {
                json += ",\"detail\":\"" + json_escape_(it.detail) + "\"";
            }
            json += "}";

            ++emitted;
            if (emitted >= 200) break;
        }
        json += "]";
        return json;
    }

    std::string build_definition_result_(const std::vector<LspLocation>& targets) {
        if (targets.empty()) return "null";
        std::string json = "[";
        for (size_t i = 0; i < targets.size(); ++i) {
            if (i != 0) json += ",";
            const auto& loc = targets[i];
            json += "{";
            json += "\"uri\":\"" + json_escape_(loc.uri) + "\",";
            json += "\"range\":{";
            json += "\"start\":{\"line\":" + std::to_string(loc.start_line) +
                    ",\"character\":" + std::to_string(loc.start_character) + "},";
            json += "\"end\":{\"line\":" + std::to_string(loc.end_line) +
                    ",\"character\":" + std::to_string(loc.end_character) + "}";
            json += "}";
            json += "}";
        }
        json += "]";
        return json;
    }

    std::string build_initialize_result_() {
        std::string json = "{\"capabilities\":{";
        json += "\"textDocumentSync\":{\"openClose\":true,\"change\":2},";
        json += "\"positionEncoding\":\"utf-16\",";
        json += "\"completionProvider\":{\"triggerCharacters\":[\".\",\":\"],\"resolveProvider\":false},";
        json += "\"definitionProvider\":true,";
        json += "\"semanticTokensProvider\":{";
        json += "\"legend\":{";
        json += "\"tokenTypes\":[";
        for (size_t i = 0; i < kSemTokenTypeNames.size(); ++i) {
            if (i != 0) json += ",";
            json += "\"" + json_escape_(kSemTokenTypeNames[i]) + "\"";
        }
        json += "],";
        json += "\"tokenModifiers\":[";
        for (size_t i = 0; i < kSemTokenModifierNames.size(); ++i) {
            if (i != 0) json += ",";
            json += "\"" + json_escape_(kSemTokenModifierNames[i]) + "\"";
        }
        json += "]";
        json += "},";
        json += "\"full\":true,";
        json += "\"range\":false";
        json += "}";
        json += "}}";
        return json;
    }

    std::string build_publish_diagnostics_(
        std::string_view uri,
        int64_t version,
        const std::vector<LspDiag>& diags
    ) {
        std::string json;
        json += "{\"jsonrpc\":\"2.0\",\"method\":\"textDocument/publishDiagnostics\",\"params\":{";
        json += "\"uri\":\"" + json_escape_(uri) + "\",";
        json += "\"version\":" + std::to_string(version) + ",";
        json += "\"diagnostics\":[";
        for (size_t i = 0; i < diags.size(); ++i) {
            if (i != 0) json += ",";
            const auto& d = diags[i];
            json += "{";
            json += "\"range\":{";
            json += "\"start\":{\"line\":" + std::to_string(d.start_line)
                 + ",\"character\":" + std::to_string(d.start_character) + "},";
            json += "\"end\":{\"line\":" + std::to_string(d.end_line)
                 + ",\"character\":" + std::to_string(d.end_character) + "}";
            json += "},";
            json += "\"severity\":" + std::to_string(d.severity) + ",";
            json += "\"code\":\"" + json_escape_(d.code) + "\",";
            json += "\"source\":\"parusd\",";
            json += "\"message\":\"" + json_escape_(d.message) + "\"";
            json += "}";
        }
        json += "]}}";
        return json;
    }

    std::string build_window_log_message_(int severity, std::string_view message) {
        std::string json;
        json += "{\"jsonrpc\":\"2.0\",\"method\":\"window/logMessage\",\"params\":{";
        json += "\"type\":" + std::to_string(severity) + ",";
        json += "\"message\":\"" + json_escape_(message) + "\"";
        json += "}}";
        return json;
    }

    std::string build_response_result_(const JsonValue* id, std::string_view result_json) {
        if (id == nullptr) return {};
        std::string out = "{\"jsonrpc\":\"2.0\",\"id\":" + json_value_to_text_(*id)
                        + ",\"result\":" + std::string(result_json) + "}";
        return out;
    }

    std::string build_response_error_(const JsonValue* id, int code, std::string_view message) {
        if (id == nullptr) return {};
        std::string out = "{\"jsonrpc\":\"2.0\",\"id\":" + json_value_to_text_(*id)
                        + ",\"error\":{";
        out += "\"code\":" + std::to_string(code) + ",";
        out += "\"message\":\"" + json_escape_(message) + "\"";
        out += "}}";
        return out;
    }

    class LspServer {
    public:
        int run() {
            while (true) {
                std::string payload;
                if (!read_lsp_message_(std::cin, payload)) {
                    return 0;
                }

                JsonValue msg{};
                JsonParser parser(payload);
                if (!parser.parse(msg) || msg.kind != JsonValue::Kind::kObject) {
                    continue;
                }

                const JsonValue* id = obj_get_(msg, "id");
                const auto method = as_string_(obj_get_(msg, "method"));
                if (!method.has_value()) {
                    continue;
                }

                const auto params = obj_get_(msg, "params");
                if (*method == "initialize") {
                    const auto macro_cfg = parse_macro_config_from_initialize_(params);
                    const auto cimport_cfg = parse_cimport_config_from_initialize_(params);
                    macro_budget_ = macro_cfg.budget;
                    parser_features_ = macro_cfg.parser_features;
                    cimport_cfg_ = cimport_cfg;

                    const std::string result = build_initialize_result_();
                    const auto response = build_response_result_(id, result);
                    if (!response.empty()) write_lsp_message_(std::cout, response);
                    for (const auto& w : macro_cfg.warnings) {
                        notify_log_message_(/*warning=*/2, w);
                    }
                    for (const auto& w : cimport_cfg.warnings) {
                        notify_log_message_(/*warning=*/2, w);
                    }
                    continue;
                }

                if (*method == "initialized") {
                    continue;
                }

                if (*method == "shutdown") {
                    shutdown_requested_ = true;
                    const auto response = build_response_result_(id, "null");
                    if (!response.empty()) write_lsp_message_(std::cout, response);
                    continue;
                }

                if (*method == "exit") {
                    return shutdown_requested_ ? 0 : 1;
                }

                if (*method == "textDocument/didOpen") {
                    handle_did_open_(params);
                    continue;
                }

                if (*method == "textDocument/didChange") {
                    handle_did_change_(params);
                    continue;
                }

                if (*method == "textDocument/didClose") {
                    handle_did_close_(params);
                    continue;
                }

                if (*method == "workspace/didChangeWatchedFiles") {
                    handle_did_change_watched_files_(params);
                    continue;
                }

                if (*method == "textDocument/completion") {
                    handle_completion_(id, params);
                    continue;
                }

                if (*method == "textDocument/definition") {
                    handle_definition_(id, params);
                    continue;
                }

                if (*method == "textDocument/semanticTokens/full") {
                    handle_semantic_tokens_full_(id, params);
                    continue;
                }

                if (id != nullptr) {
                    const auto response = build_response_error_(id, -32601, "method not found");
                    if (!response.empty()) write_lsp_message_(std::cout, response);
                }
            }
        }

    private:
        void publish_diagnostics_(std::string_view uri, int64_t version, const std::vector<LspDiag>& diags) {
            const auto msg = build_publish_diagnostics_(uri, version, diags);
            write_lsp_message_(std::cout, msg);
        }

        void notify_log_message_(int severity, std::string_view text) {
            const auto msg = build_window_log_message_(severity, text);
            write_lsp_message_(std::cout, msg);
        }

        std::optional<std::filesystem::path> config_lei_for_uri_(std::string_view uri) const {
            const auto fs_path = uri_to_file_path_(uri);
            if (!fs_path.has_value()) return std::nullopt;
            return find_config_lei_for_file_(std::filesystem::path(*fs_path));
        }

        bool root_list_contains_(const std::vector<std::filesystem::path>& roots,
                                 const std::filesystem::path& root) const {
            for (const auto& r : roots) {
                if (same_file_path_(r, root)) return true;
            }
            return false;
        }

        void refresh_open_documents_for_project_roots_(
            const std::vector<std::filesystem::path>& roots,
            std::optional<std::string_view> skip_uri = std::nullopt
        ) {
            if (roots.empty()) return;
            for (auto& [doc_uri, state] : documents_) {
                if (skip_uri.has_value() && doc_uri == *skip_uri) continue;
                if (state.lang != DocLang::kParus && state.lang != DocLang::kLei) continue;

                const auto cfg = config_lei_for_uri_(doc_uri);
                if (!cfg.has_value()) continue;
                const auto root = cfg->parent_path();
                if (!root_list_contains_(roots, root)) continue;

                state.analysis.valid = false;
                state.revision = ++revision_seq_;
                ensure_analysis_cache_(doc_uri, state);
                publish_diagnostics_(doc_uri, state.version, state.analysis.diagnostics);
                state.pending_edits.clear();
            }
        }

        void handle_did_change_watched_files_(const JsonValue* params) {
            if (params == nullptr || params->kind != JsonValue::Kind::kObject) return;
            const auto* changes = obj_get_(*params, "changes");
            if (changes == nullptr || changes->kind != JsonValue::Kind::kArray) return;

            std::vector<std::filesystem::path> roots{};
            for (const auto& c : changes->array_v) {
                if (c.kind != JsonValue::Kind::kObject) continue;
                const auto uri = as_string_(obj_get_(c, "uri"));
                if (!uri.has_value() || uri->empty()) continue;
                const auto fs_path = uri_to_file_path_(*uri);
                if (!fs_path.has_value()) continue;
                std::filesystem::path changed(*fs_path);
                if (changed.extension() != ".lei") continue;
                const auto cfg = find_config_lei_for_file_(changed);
                if (!cfg.has_value()) continue;
                const auto root = cfg->parent_path();
                if (!root_list_contains_(roots, root)) roots.push_back(root);
            }

            if (roots.empty()) return;
            for (const auto& root : roots) {
                invalidate_lint_caches_for_root_(root);
            }
            refresh_open_documents_for_project_roots_(roots);
        }

        void ensure_analysis_cache_(std::string_view uri, DocumentState& st) {
            if (st.analysis.valid && st.analysis.revision == st.revision) {
                return;
            }

            if (st.lang == DocLang::kParus) {
                st.parse_session.set_feature_flags(parser_features_);
            }

            std::unordered_map<std::string, std::string> lei_overlays{};
            const std::unordered_map<std::string, std::string>* lei_overlays_ptr = nullptr;
#if PARUSD_ENABLE_LEI
            lei_overlays = build_lei_overlay_map_(documents_);
            lei_overlays_ptr = &lei_overlays;
#endif

            auto analyzed = analyze_document_(uri, st, macro_budget_, cimport_cfg_, lei_overlays_ptr);
            st.analysis.revision = st.revision;
            st.analysis.valid = true;
            st.analysis.diagnostics = std::move(analyzed.diagnostics);
            st.analysis.semantic_tokens = std::move(analyzed.semantic_tokens);
            st.analysis.completion_items = std::move(analyzed.completion_items);
            st.analysis.definition_bindings = std::move(analyzed.definition_bindings);
            st.analysis.top_level_definitions = std::move(analyzed.top_level_definitions);

            if (trace_incremental_) {
                const char* lang_name = "unknown";
                if (st.lang == DocLang::kParus) lang_name = "parus";
                if (st.lang == DocLang::kLei) lang_name = "lei";
                std::cerr << "[parusd] uri=" << uri
                          << " lang=" << lang_name
                          << " revision=" << st.revision
                          << " parse=" << reparse_mode_name_(analyzed.parse_mode)
                          << "\n";
            }
        }

        void handle_did_open_(const JsonValue* params) {
            if (params == nullptr || params->kind != JsonValue::Kind::kObject) return;
            const auto td = obj_get_(*params, "textDocument");
            if (td == nullptr || td->kind != JsonValue::Kind::kObject) return;

            const auto uri = as_string_(obj_get_(*td, "uri"));
            const auto text = as_string_(obj_get_(*td, "text"));
            if (!uri.has_value() || !text.has_value()) return;

            DocumentState st{};
            st.text = std::string(*text);
            st.version = as_i64_(obj_get_(*td, "version")).value_or(0);
            st.revision = ++revision_seq_;
            st.lang = doc_lang_from_uri_(*uri);
            if (st.lang == DocLang::kParus) {
                st.parse_session.set_feature_flags(parser_features_);
            }

            auto it = documents_.insert_or_assign(std::string(*uri), std::move(st)).first;
            ensure_analysis_cache_(*uri, it->second);
            publish_diagnostics_(*uri, it->second.version, it->second.analysis.diagnostics);

            if (it->second.lang == DocLang::kLei) {
                if (const auto cfg = config_lei_for_uri_(*uri); cfg.has_value()) {
                    const std::vector<std::filesystem::path> roots{cfg->parent_path()};
                    invalidate_lint_caches_for_root_(cfg->parent_path());
                    refresh_open_documents_for_project_roots_(roots, *uri);
                }
            }
        }

        void handle_did_change_(const JsonValue* params) {
            if (params == nullptr || params->kind != JsonValue::Kind::kObject) return;

            const auto td = obj_get_(*params, "textDocument");
            const auto changes = obj_get_(*params, "contentChanges");
            if (td == nullptr || td->kind != JsonValue::Kind::kObject) return;
            if (changes == nullptr || changes->kind != JsonValue::Kind::kArray) return;

            const auto uri = as_string_(obj_get_(*td, "uri"));
            if (!uri.has_value()) return;

            auto it = documents_.find(std::string(*uri));
            if (it == documents_.end()) return;

            const auto incoming_version = as_i64_(obj_get_(*td, "version"));
            if (incoming_version.has_value() && *incoming_version <= it->second.version) {
                return;
            }

            bool has_valid_change = false;
            bool changed_any = false;
            for (const auto& change_node : changes->array_v) {
                TextChange change{};
                if (!parse_text_change_(change_node, change)) continue;
                has_valid_change = true;
                if (apply_text_change_(it->second, change)) {
                    changed_any = true;
                }
            }
            if (!has_valid_change) return;

            if (!changed_any) {
                if (incoming_version.has_value()) {
                    it->second.version = *incoming_version;
                }
                ensure_analysis_cache_(*uri, it->second);
                publish_diagnostics_(*uri, it->second.version, it->second.analysis.diagnostics);
                it->second.pending_edits.clear();
                if (it->second.lang == DocLang::kLei) {
                    if (const auto cfg = config_lei_for_uri_(*uri); cfg.has_value()) {
                        const std::vector<std::filesystem::path> roots{cfg->parent_path()};
                        invalidate_lint_caches_for_root_(cfg->parent_path());
                        refresh_open_documents_for_project_roots_(roots, *uri);
                    }
                }
                return;
            }

            it->second.version = incoming_version.value_or(it->second.version + 1);
            it->second.revision = ++revision_seq_;
            it->second.analysis.valid = false;

            ensure_analysis_cache_(*uri, it->second);
            publish_diagnostics_(*uri, it->second.version, it->second.analysis.diagnostics);
            it->second.pending_edits.clear();
            if (it->second.lang == DocLang::kLei) {
                if (const auto cfg = config_lei_for_uri_(*uri); cfg.has_value()) {
                    const std::vector<std::filesystem::path> roots{cfg->parent_path()};
                    invalidate_lint_caches_for_root_(cfg->parent_path());
                    refresh_open_documents_for_project_roots_(roots, *uri);
                }
            }
        }

        void handle_did_close_(const JsonValue* params) {
            if (params == nullptr || params->kind != JsonValue::Kind::kObject) return;
            const auto td = obj_get_(*params, "textDocument");
            if (td == nullptr || td->kind != JsonValue::Kind::kObject) return;

            const auto uri = as_string_(obj_get_(*td, "uri"));
            if (!uri.has_value()) return;

            const auto it = documents_.find(std::string(*uri));
            DocLang closing_lang = DocLang::kUnknown;
            if (it != documents_.end()) closing_lang = it->second.lang;

            documents_.erase(std::string(*uri));
            publish_diagnostics_(*uri, /*version=*/0, {});

            if (closing_lang == DocLang::kLei) {
                if (const auto cfg = config_lei_for_uri_(*uri); cfg.has_value()) {
                    const std::vector<std::filesystem::path> roots{cfg->parent_path()};
                    invalidate_lint_caches_for_root_(cfg->parent_path());
                    refresh_open_documents_for_project_roots_(roots);
                }
            }
        }

        void handle_semantic_tokens_full_(const JsonValue* id, const JsonValue* params) {
            if (id == nullptr) return;
            if (params == nullptr || params->kind != JsonValue::Kind::kObject) {
                const auto response = build_response_error_(id, -32602, "invalid params");
                if (!response.empty()) write_lsp_message_(std::cout, response);
                return;
            }

            const auto td = obj_get_(*params, "textDocument");
            if (td == nullptr || td->kind != JsonValue::Kind::kObject) {
                const auto response = build_response_error_(id, -32602, "invalid params");
                if (!response.empty()) write_lsp_message_(std::cout, response);
                return;
            }

            const auto uri = as_string_(obj_get_(*td, "uri"));
            if (!uri.has_value()) {
                const auto response = build_response_error_(id, -32602, "textDocument.uri is required");
                if (!response.empty()) write_lsp_message_(std::cout, response);
                return;
            }

            const auto it = documents_.find(std::string(*uri));
            if (it == documents_.end()) {
                const auto result = build_semantic_tokens_result_({});
                const auto response = build_response_result_(id, result);
                if (!response.empty()) write_lsp_message_(std::cout, response);
                return;
            }

            ensure_analysis_cache_(*uri, it->second);
            const auto result = build_semantic_tokens_result_(it->second.analysis.semantic_tokens);
            const auto response = build_response_result_(id, result);
            if (!response.empty()) write_lsp_message_(std::cout, response);
        }

        std::vector<LspLocation> find_definition_targets_(
            const DocumentState& st,
            size_t offset
        ) const {
            std::vector<LspLocation> out{};
            uint32_t best_span = std::numeric_limits<uint32_t>::max();
            for (const auto& bind : st.analysis.definition_bindings) {
                if (offset < bind.use_lo || offset >= bind.use_hi) continue;
                const uint32_t span = (bind.use_hi > bind.use_lo) ? (bind.use_hi - bind.use_lo) : 1;
                if (span < best_span) {
                    best_span = span;
                    out = bind.targets;
                } else if (span == best_span) {
                    for (const auto& loc : bind.targets) {
                        append_unique_location_(out, loc);
                    }
                }
            }
            if (!out.empty()) return out;

            const std::string sym = symbol_at_offset_(st.text, offset);
            if (sym.empty()) return out;

            if (auto it = st.analysis.top_level_definitions.find(sym);
                it != st.analysis.top_level_definitions.end()) {
                out = it->second;
                return out;
            }

            std::string tail = sym;
            if (const size_t pos = tail.rfind("::"); pos != std::string::npos && pos + 2 < tail.size()) {
                tail = tail.substr(pos + 2);
            }
            if (!tail.empty()) {
                const std::string suffix = "::" + tail;
                for (const auto& [k, v] : st.analysis.top_level_definitions) {
                    if (!(k == tail || ends_with_(k, suffix))) continue;
                    for (const auto& loc : v) append_unique_location_(out, loc);
                }
            }
            return out;
        }

        std::vector<LspLocation> find_external_definition_fallback_(
            std::string_view uri,
            const DocumentState& st,
            size_t offset
        ) const {
            std::vector<LspLocation> out{};
#if PARUSD_ENABLE_LEI
            if (st.lang != DocLang::kParus) return out;
            const auto lint_ctx = build_parus_bundle_lint_context_(uri, nullptr);
            if (!lint_ctx.has_value()) return out;

            const std::string sym = symbol_at_offset_(st.text, offset);
            if (sym.empty()) return out;
            std::string tail = sym;
            if (const size_t pos = tail.rfind("::"); pos != std::string::npos && pos + 2 < tail.size()) {
                tail = tail.substr(pos + 2);
            }
            const std::string suffix = "::" + tail;

            auto append_loc = [&](const ExternalDeclLocation& loc) {
                LspLocation l{};
                l.uri = loc.file_uri;
                l.start_line = loc.line;
                l.start_character = loc.character;
                l.end_line = loc.line;
                l.end_character = loc.character + 1;
                append_unique_location_(out, l);
            };

            for (const auto& [k, locs] : lint_ctx->external_decl_locs) {
                if (!(k == sym || k == tail || ends_with_(k, suffix))) continue;
                for (const auto& loc : locs) append_loc(loc);
            }
#else
            (void)uri;
            (void)st;
            (void)offset;
#endif
            return out;
        }

        void handle_completion_(const JsonValue* id, const JsonValue* params) {
            if (id == nullptr) return;
            if (params == nullptr || params->kind != JsonValue::Kind::kObject) {
                const auto response = build_response_error_(id, -32602, "invalid params");
                if (!response.empty()) write_lsp_message_(std::cout, response);
                return;
            }

            const auto* td = obj_get_(*params, "textDocument");
            if (td == nullptr || td->kind != JsonValue::Kind::kObject) {
                const auto response = build_response_error_(id, -32602, "invalid params");
                if (!response.empty()) write_lsp_message_(std::cout, response);
                return;
            }
            const auto uri = as_string_(obj_get_(*td, "uri"));
            Position pos{};
            if (!uri.has_value() || !parse_position_(obj_get_(*params, "position"), pos)) {
                const auto response = build_response_error_(id, -32602, "textDocument.uri/position is required");
                if (!response.empty()) write_lsp_message_(std::cout, response);
                return;
            }

            const auto it = documents_.find(std::string(*uri));
            if (it == documents_.end()) {
                const auto response = build_response_result_(id, "[]");
                if (!response.empty()) write_lsp_message_(std::cout, response);
                return;
            }

            ensure_analysis_cache_(*uri, it->second);
            const size_t off = byte_offset_from_position_(it->second.text, pos);
            const std::string prefix = symbol_prefix_before_offset_(it->second.text, off);
            const auto result = build_completion_result_(it->second.analysis.completion_items, prefix);
            const auto response = build_response_result_(id, result);
            if (!response.empty()) write_lsp_message_(std::cout, response);
        }

        void handle_definition_(const JsonValue* id, const JsonValue* params) {
            if (id == nullptr) return;
            if (params == nullptr || params->kind != JsonValue::Kind::kObject) {
                const auto response = build_response_error_(id, -32602, "invalid params");
                if (!response.empty()) write_lsp_message_(std::cout, response);
                return;
            }

            const auto* td = obj_get_(*params, "textDocument");
            if (td == nullptr || td->kind != JsonValue::Kind::kObject) {
                const auto response = build_response_error_(id, -32602, "invalid params");
                if (!response.empty()) write_lsp_message_(std::cout, response);
                return;
            }
            const auto uri = as_string_(obj_get_(*td, "uri"));
            Position pos{};
            if (!uri.has_value() || !parse_position_(obj_get_(*params, "position"), pos)) {
                const auto response = build_response_error_(id, -32602, "textDocument.uri/position is required");
                if (!response.empty()) write_lsp_message_(std::cout, response);
                return;
            }

            const auto it = documents_.find(std::string(*uri));
            if (it == documents_.end()) {
                const auto response = build_response_result_(id, "null");
                if (!response.empty()) write_lsp_message_(std::cout, response);
                return;
            }

            ensure_analysis_cache_(*uri, it->second);
            const size_t off = byte_offset_from_position_(it->second.text, pos);
            auto targets = find_definition_targets_(it->second, off);
            if (targets.empty()) {
                targets = find_external_definition_fallback_(*uri, it->second, off);
            }
            const auto result = build_definition_result_(targets);
            const auto response = build_response_result_(id, result);
            if (!response.empty()) write_lsp_message_(std::cout, response);
        }

        std::unordered_map<std::string, DocumentState> documents_{};
        bool shutdown_requested_ = false;
        uint64_t revision_seq_ = 0;
        bool trace_incremental_ = (std::getenv("PARUSD_TRACE_INCREMENTAL") != nullptr);
        parus::macro::ExpansionBudget macro_budget_ = parus::macro::default_budget_jit();
        parus::ParserFeatureFlags parser_features_{};
        ServerCImportConfig cimport_cfg_{};
    };

    void print_usage_() {
        std::cerr
            << "parusd --stdio\n"
            << "  standalone Parus language server (LSP over stdio).\n";
    }

} // namespace

int main(int argc, char** argv) {
    if (argc == 2 && (std::strcmp(argv[1], "-h") == 0 || std::strcmp(argv[1], "--help") == 0)) {
        print_usage_();
        return 0;
    }
    if (argc == 2 && std::strcmp(argv[1], "--version") == 0) {
        std::cout << "parusd dev\n";
        return 0;
    }

    bool stdio = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--stdio") == 0) {
            stdio = true;
            continue;
        }

        std::cerr << "error: unknown option: " << argv[i] << "\n";
        print_usage_();
        return 1;
    }

    if (!stdio) {
        std::cerr << "error: parusd requires --stdio\n";
        print_usage_();
        return 1;
    }

    LspServer server;
    return server.run();
}
