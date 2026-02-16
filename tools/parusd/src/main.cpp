#include <parus/ast/Nodes.hpp>
#include <parus/cap/CapabilityCheck.hpp>
#include <parus/diag/Render.hpp>
#include <parus/lex/Lexer.hpp>
#include <parus/parse/Parser.hpp>
#include <parus/passes/Passes.hpp>
#include <parus/text/SourceManager.hpp>
#include <parus/ty/TypePool.hpp>
#include <parus/tyck/TypeCheck.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

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
            if (consume_literal_("true")) {
                out = JsonValue{};
                out.kind = JsonValue::Kind::kBool;
                out.bool_v = true;
                return true;
            }
            if (consume_literal_("false")) {
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

        struct EditWindow {
            size_t start = 0;
            size_t end = 0;
        };
        std::vector<EditWindow> pending_edits{};
    };

    size_t byte_offset_from_position_(std::string_view text, const Position& p) {
        size_t off = 0;
        uint32_t line = 0;
        while (off < text.size() && line < p.line) {
            if (text[off] == '\n') ++line;
            ++off;
        }

        size_t col_off = off;
        uint32_t col = 0;
        while (col_off < text.size() && text[col_off] != '\n' && col < p.character) {
            ++col_off;
            ++col;
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

    void apply_text_change_(DocumentState& doc, const TextChange& ch) {
        if (!ch.has_range) {
            doc.text = ch.text;
            doc.pending_edits.clear();
            return;
        }

        const size_t start = byte_offset_from_position_(doc.text, ch.range.start);
        const size_t end = byte_offset_from_position_(doc.text, ch.range.end);
        const size_t lo = std::min(start, end);
        const size_t hi = std::max(start, end);
        if (lo > doc.text.size()) return;

        const size_t clamped_hi = std::min(hi, doc.text.size());
        doc.text.replace(lo, clamped_hi - lo, ch.text);

        doc.pending_edits.push_back(DocumentState::EditWindow{lo, lo + ch.text.size()});
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

    std::vector<LspDiag> analyze_document_(std::string_view uri, std::string_view source) {
        std::vector<LspDiag> out;

        parus::SourceManager sm;
        const uint32_t file_id = sm.add(std::string(uri), std::string(source));

        parus::diag::Bag bag;
        parus::Lexer lx(sm.content(file_id), file_id, &bag);
        const auto toks = lx.lex_all();

        parus::ast::AstArena ast;
        parus::ty::TypePool types;

        parus::Parser parser(toks, ast, types, &bag, /*max_errors=*/256);
        const auto root = parser.parse_program();

        if (!bag.has_error()) {
            parus::passes::PassOptions popt{};
            const auto pass_res = parus::passes::run_on_program(ast, root, bag, popt);

            if (!bag.has_error()) {
                parus::tyck::TypeChecker tc(ast, types, bag);
                const auto ty = tc.check_program(root);

                if (!bag.has_error() && ty.errors.empty()) {
                    (void)parus::cap::run_capability_check(ast, root, pass_res.name_resolve, ty, types, bag);
                }
            }
        }

        out.reserve(bag.diags().size());
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
            out.push_back(std::move(ld));
        }

        return out;
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
                    const std::string result = "{\"capabilities\":{"
                        "\"textDocumentSync\":{\"openClose\":true,\"change\":2},"
                        "\"positionEncoding\":\"utf-16\"}}";
                    const auto response = build_response_result_(id, result);
                    if (!response.empty()) write_lsp_message_(std::cout, response);
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

            documents_[std::string(*uri)] = st;
            const auto diags = analyze_document_(*uri, st.text);
            publish_diagnostics_(*uri, st.version, diags);
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

            for (const auto& change_node : changes->array_v) {
                TextChange change{};
                if (!parse_text_change_(change_node, change)) continue;
                apply_text_change_(it->second, change);
            }

            it->second.version = as_i64_(obj_get_(*td, "version")).value_or(it->second.version + 1);
            it->second.revision = ++revision_seq_;

            const auto diags = analyze_document_(*uri, it->second.text);
            publish_diagnostics_(*uri, it->second.version, diags);
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

        std::unordered_map<std::string, DocumentState> documents_{};
        bool shutdown_requested_ = false;
        uint64_t revision_seq_ = 0;
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
