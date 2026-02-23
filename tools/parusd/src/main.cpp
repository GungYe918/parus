#include <parus/ast/Nodes.hpp>
#include <parus/cap/CapabilityCheck.hpp>
#include <parus/diag/Render.hpp>
#include <parus/lex/Lexer.hpp>
#include <parus/macro/Expander.hpp>
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
#include <lei/parse/Parser.hpp>
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <optional>
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

    struct AnalysisResult {
        std::vector<LspDiag> diagnostics{};
        std::vector<SemToken> semantic_tokens{};
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
        } analysis{};
    };

    struct ServerMacroConfig {
        parus::macro::ExpansionBudget budget = parus::macro::default_budget_jit();
        parus::ParserFeatureFlags parser_features{};
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

        if (const auto* exp_cfg = obj_get_(*root, "experimental");
            exp_cfg != nullptr && exp_cfg->kind == JsonValue::Kind::kObject) {
            if (const auto v = as_bool_(obj_get_(*exp_cfg, "macroWithToken")); v.has_value()) {
                cfg.parser_features.macro_with_token = *v;
            }
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
            case K::kKwField:
            case K::kKwActs:
            case K::kKwClass:
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

            if (kind == K::kKwActs || kind == K::kKwClass) {
                mark_ident(i + 1, SemTokenType::kClass, 0);
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

    AnalysisResult analyze_parus_document_(
        std::string_view uri,
        DocumentState& doc,
        const parus::macro::ExpansionBudget& macro_budget
    ) {
        AnalysisResult out;

        parus::SourceManager sm;
        const uint32_t file_id = sm.add(std::string(uri), std::string(doc.text));

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

        auto& snapshot = doc.parse_session.mutable_snapshot();
        auto& ast = snapshot.ast;
        auto& types = snapshot.types;
        const auto root = snapshot.root;
        const auto& toks = snapshot.tokens;

        std::unordered_map<uint64_t, SemClass> resolved_map;
        if (!bag.has_error()) {
            const bool macro_ok = parus::macro::expand_program(ast, types, root, bag, macro_budget);
            if (!bag.has_error() && macro_ok) {
                auto type_resolve = parus::type::resolve_program_types(ast, types, root, bag);
                if (!bag.has_error() && type_resolve.ok) {
                    parus::passes::PassOptions popt{};
                    const auto pass_res = parus::passes::run_on_program(ast, root, bag, popt);
                    resolved_map = collect_resolved_semantic_map_(pass_res.name_resolve);

                    if (!bag.has_error()) {
                        parus::tyck::TypeChecker tc(ast, types, bag, &type_resolve);
                        const auto ty = tc.check_program(root);

                        if (!bag.has_error() && ty.errors.empty()) {
                            (void)parus::cap::run_capability_check(ast, root, pass_res.name_resolve, ty, types, bag);
                        }
                    }
                }
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
                    } else if (prev_kind == K::kKwActs || prev_kind == K::kKwClass) {
                        sem_class = SemClass{static_cast<uint32_t>(SemTokenType::kClass), kSemModDeclaration};
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

    std::vector<SemToken> semantic_tokens_for_lei_document_(
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
        (void)lei::parse::parse_source(doc.text, parsed_file, parse_bag);
        out.diagnostics.reserve(parse_bag.all().size());
        for (const auto& d : parse_bag.all()) {
            append_lei_diagnostic_(out.diagnostics, dedupe, d);
        }

        out.semantic_tokens = semantic_tokens_for_lei_document_(doc.text, parsed_file);

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
        const std::unordered_map<std::string, std::string>* lei_overlays
    ) {
        switch (doc.lang) {
            case DocLang::kParus:
                return analyze_parus_document_(uri, doc, macro_budget);
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

    std::string build_initialize_result_() {
        std::string json = "{\"capabilities\":{";
        json += "\"textDocumentSync\":{\"openClose\":true,\"change\":2},";
        json += "\"positionEncoding\":\"utf-16\",";
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
                    macro_budget_ = macro_cfg.budget;
                    parser_features_ = macro_cfg.parser_features;

                    const std::string result = build_initialize_result_();
                    const auto response = build_response_result_(id, result);
                    if (!response.empty()) write_lsp_message_(std::cout, response);
                    for (const auto& w : macro_cfg.warnings) {
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

        void ensure_analysis_cache_(std::string_view uri, DocumentState& st) {
            if (st.analysis.valid && st.analysis.revision == st.revision) {
                return;
            }

            if (st.lang == DocLang::kParus) {
                st.parse_session.set_feature_flags(parser_features_);
            }

            std::unordered_map<std::string, std::string> lei_overlays{};
            const std::unordered_map<std::string, std::string>* lei_overlays_ptr = nullptr;
            if (st.lang == DocLang::kLei) {
                lei_overlays = build_lei_overlay_map_(documents_);
                lei_overlays_ptr = &lei_overlays;
            }

            auto analyzed = analyze_document_(uri, st, macro_budget_, lei_overlays_ptr);
            st.analysis.revision = st.revision;
            st.analysis.valid = true;
            st.analysis.diagnostics = std::move(analyzed.diagnostics);
            st.analysis.semantic_tokens = std::move(analyzed.semantic_tokens);

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
                return;
            }

            it->second.version = incoming_version.value_or(it->second.version + 1);
            it->second.revision = ++revision_seq_;
            it->second.analysis.valid = false;

            ensure_analysis_cache_(*uri, it->second);
            publish_diagnostics_(*uri, it->second.version, it->second.analysis.diagnostics);
            it->second.pending_edits.clear();
        }

        void handle_did_close_(const JsonValue* params) {
            if (params == nullptr || params->kind != JsonValue::Kind::kObject) return;
            const auto td = obj_get_(*params, "textDocument");
            if (td == nullptr || td->kind != JsonValue::Kind::kObject) return;

            const auto uri = as_string_(obj_get_(*td, "uri"));
            if (!uri.has_value()) return;

            documents_.erase(std::string(*uri));
            publish_diagnostics_(*uri, /*version=*/0, {});
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

        std::unordered_map<std::string, DocumentState> documents_{};
        bool shutdown_requested_ = false;
        uint64_t revision_seq_ = 0;
        bool trace_incremental_ = (std::getenv("PARUSD_TRACE_INCREMENTAL") != nullptr);
        parus::macro::ExpansionBudget macro_budget_ = parus::macro::default_budget_jit();
        parus::ParserFeatureFlags parser_features_{};
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
