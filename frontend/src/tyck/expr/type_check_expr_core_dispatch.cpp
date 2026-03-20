// frontend/src/tyck/type_check_expr_core.cpp
#include <parus/tyck/TypeCheck.hpp>
#include <parus/cimport/TypeReprNormalize.hpp>
#include <parus/syntax/TokenKind.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/diag/DiagCode.hpp>
#include "../common/type_check_literals.hpp"

#include <charconv>
#include <cstdlib>
#include <iomanip>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace parus::tyck {

    namespace {
        bool starts_with_(std::string_view s, std::string_view pfx) {
            return s.size() >= pfx.size() && s.substr(0, pfx.size()) == pfx;
        }

        bool ends_with_(std::string_view s, std::string_view sfx) {
            return s.size() >= sfx.size() && s.substr(s.size() - sfx.size()) == sfx;
        }

        bool is_hex_digit_(char c) {
            return (c >= '0' && c <= '9') ||
                   (c >= 'a' && c <= 'f') ||
                   (c >= 'A' && c <= 'F');
        }

        uint8_t hex_digit_value_(char c) {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + (c - 'a'));
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(10 + (c - 'A'));
            return 0;
        }

        std::string quote_bytes_as_string_lit_(std::string_view bytes) {
            static constexpr char kHex[] = "0123456789ABCDEF";
            std::string out;
            out.reserve(bytes.size() + 2);
            out.push_back('"');
            for (unsigned char c : bytes) {
                switch (c) {
                    case '\\': out += "\\\\"; break;
                    case '"': out += "\\\""; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    case '\0': out += "\\0"; break;
                    default:
                        if (c < 0x20u) {
                            out += "\\x";
                            out.push_back(kHex[(c >> 4) & 0xF]);
                            out.push_back(kHex[c & 0xF]);
                        } else {
                            out.push_back(static_cast<char>(c));
                        }
                        break;
                }
            }
            out.push_back('"');
            return out;
        }

        std::string decode_escaped_string_body_(std::string_view body) {
            std::string out;
            out.reserve(body.size());

            for (size_t i = 0; i < body.size(); ++i) {
                const char c = body[i];
                if (c != '\\') {
                    out.push_back(c);
                    continue;
                }

                if (i + 1 >= body.size()) {
                    out.push_back('\\');
                    break;
                }

                const char esc = body[++i];
                switch (esc) {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case '\\': out.push_back('\\'); break;
                    case '"': out.push_back('"'); break;
                    case '\'': out.push_back('\''); break;
                    case '0': out.push_back('\0'); break;
                    case 'x': {
                        if (i + 2 < body.size() &&
                            is_hex_digit_(body[i + 1]) &&
                            is_hex_digit_(body[i + 2])) {
                            const uint8_t hi = hex_digit_value_(body[i + 1]);
                            const uint8_t lo = hex_digit_value_(body[i + 2]);
                            out.push_back(static_cast<char>((hi << 4) | lo));
                            i += 2;
                            break;
                        }
                        out.push_back('x');
                        break;
                    }
                    default:
                        out.push_back(esc);
                        break;
                }
            }
            return out;
        }

        bool parse_i64_lit_(std::string_view lit, int64_t& out) {
            const detail::ParsedIntLiteral p = detail::parse_int_literal_(lit);
            if (!p.ok) return false;

            unsigned long long uv = 0;
            const char* begin = p.digits_no_sep.data();
            const char* end = begin + p.digits_no_sep.size();
            auto [ptr, ec] = std::from_chars(begin, end, uv, 10);
            if (ec != std::errc{} || ptr != end) return false;
            if (uv > static_cast<unsigned long long>(std::numeric_limits<int64_t>::max())) return false;
            out = static_cast<int64_t>(uv);
            return true;
        }

        bool parse_f64_lit_(std::string_view lit, double& out) {
            std::string s;
            s.reserve(lit.size());
            for (char c : lit) {
                if (c == '_') continue;
                s.push_back(c);
            }

            auto strip_suffix = [&](std::string_view sfx) {
                if (s.size() >= sfx.size() && s.substr(s.size() - sfx.size()) == sfx) {
                    s.resize(s.size() - sfx.size());
                    return true;
                }
                return false;
            };
            (void)strip_suffix("f128");
            if (!strip_suffix("f128")) {
                (void)strip_suffix("f64");
                (void)strip_suffix("f32");
                (void)strip_suffix("lf");
                (void)strip_suffix("f");
            }

            char* endp = nullptr;
            const double v = std::strtod(s.c_str(), &endp);
            if (endp == nullptr || *endp != '\0') return false;
            out = v;
            return true;
        }

        bool parse_char_byte_lit_(std::string_view lit, char& out) {
            if (lit.size() < 3 || lit.front() != '\'' || lit.back() != '\'') return false;
            const std::string_view body = lit.substr(1, lit.size() - 2);
            if (body.empty()) return false;
            if (body.size() == 1) {
                out = body[0];
                return true;
            }
            if (body[0] != '\\' || body.size() != 2) return false;
            switch (body[1]) {
                case 'n': out = '\n'; return true;
                case 'r': out = '\r'; return true;
                case 't': out = '\t'; return true;
                case '\\': out = '\\'; return true;
                case '\'': out = '\''; return true;
                case '"': out = '"'; return true;
                case '0': out = '\0'; return true;
                default:
                    return false;
            }
        }

        bool decode_string_bytes_(const ast::Expr& e, std::string& out) {
            const std::string_view text = e.text;
            if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
                out = decode_escaped_string_body_(text.substr(1, text.size() - 2));
                return true;
            }
            if (starts_with_(text, "R\"\"\"") && ends_with_(text, "\"\"\"") && text.size() >= 7) {
                out = std::string(text.substr(4, text.size() - 7));
                return true;
            }
            return false;
        }

        bool decode_c_string_bytes_(std::string_view text, std::string& out) {
            if (starts_with_(text, "c\"") && text.size() >= 3 && text.back() == '"') {
                out = decode_escaped_string_body_(text.substr(2, text.size() - 3));
                return true;
            }
            if (starts_with_(text, "cr\"") && text.size() >= 4 && text.back() == '"') {
                out = std::string(text.substr(3, text.size() - 4));
                return true;
            }
            return false;
        }

        /// @brief 타입이 borrow(`&T`/`&mut T`)인지 판정하고, 내부 요소 타입/가변 여부를 반환한다.
        bool borrow_info_(
            const ty::TypePool& types,
            ty::TypeId t,
            ty::TypeId& out_elem,
            bool& out_is_mut
        ) {
            if (t == ty::kInvalidType) return false;
            if (t >= types.count()) return false;
            const auto& tt = types.get(t);
            if (tt.kind != ty::Kind::kBorrow) return false;
            out_elem = tt.elem;
            out_is_mut = tt.borrow_is_mut;
            return true;
        }

        /// @brief 값 읽기 컨텍스트에서 borrow 타입을 요소 타입으로 1단계 디케이한다.
        ty::TypeId read_decay_borrow_(const ty::TypePool& types, ty::TypeId t) {
            ty::TypeId elem = ty::kInvalidType;
            bool is_mut = false;
            if (!borrow_info_(types, t, elem, is_mut)) return t;
            (void)is_mut;
            return elem;
        }
    } // namespace

    
    using K = parus::syntax::TokenKind;
    using detail::ParsedFloatLiteral;
    using detail::ParsedIntLiteral;
    using detail::parse_float_literal_;
    using detail::parse_int_literal_;

    bool TypeChecker::fstring_const_to_text_(const FStringConstValue& v, std::string& out) const {
        switch (v.kind) {
            case FStringConstValue::Kind::kInt:
                out = std::to_string(v.i64);
                return true;
            case FStringConstValue::Kind::kFloat: {
                std::ostringstream oss;
                oss << std::setprecision(17) << v.f64;
                out = oss.str();
                return true;
            }
            case FStringConstValue::Kind::kBool:
                out = v.b ? "true" : "false";
                return true;
            case FStringConstValue::Kind::kChar:
                out.assign(1, v.ch);
                return true;
            case FStringConstValue::Kind::kText:
                out = v.text;
                return true;
            case FStringConstValue::Kind::kInvalid:
            default:
                return false;
        }
    }

    bool TypeChecker::eval_fstring_const_expr_(ast::ExprId expr_eid, FStringConstValue& out) {
        if (expr_eid == ast::k_invalid_expr || expr_eid >= ast_.exprs().size()) return false;
        const ast::Expr& e = ast_.expr(expr_eid);

        auto fail = [&](std::string_view msg) -> bool {
            diag_(diag::Code::kTypeErrorGeneric, e.span, msg);
            err_(e.span, std::string(msg));
            return false;
        };

        switch (e.kind) {
            case ast::ExprKind::kIntLit: {
                int64_t v = 0;
                if (!parse_i64_lit_(e.text, v)) {
                    return fail("fstring interpolation: integer literal is out of temporary const-eval range");
                }
                out.kind = FStringConstValue::Kind::kInt;
                out.i64 = v;
                return true;
            }
            case ast::ExprKind::kFloatLit: {
                double v = 0.0;
                if (!parse_f64_lit_(e.text, v)) {
                    return fail("fstring interpolation: invalid float literal");
                }
                out.kind = FStringConstValue::Kind::kFloat;
                out.f64 = v;
                return true;
            }
            case ast::ExprKind::kBoolLit:
                out.kind = FStringConstValue::Kind::kBool;
                out.b = (e.text == "true");
                return true;
            case ast::ExprKind::kCharLit: {
                char v = '\0';
                if (!parse_char_byte_lit_(e.text, v)) {
                    return fail("fstring interpolation: unsupported char literal");
                }
                out.kind = FStringConstValue::Kind::kChar;
                out.ch = v;
                return true;
            }
            case ast::ExprKind::kStringLit: {
                if (e.string_is_format) {
                    std::string folded;
                    if (!fold_fstring_expr_(expr_eid, folded)) {
                        return false;
                    }
                    out.kind = FStringConstValue::Kind::kText;
                    out.text = std::move(folded);
                    return true;
                }

                std::string bytes;
                if (!decode_string_bytes_(e, bytes)) {
                    return fail("fstring interpolation: unsupported string literal form");
                }
                out.kind = FStringConstValue::Kind::kText;
                out.text = std::move(bytes);
                return true;
            }
            case ast::ExprKind::kUnary: {
                FStringConstValue a{};
                if (!eval_fstring_const_expr_(e.a, a)) return false;

                if (e.op == K::kPlus && a.kind == FStringConstValue::Kind::kInt) {
                    out = a;
                    return true;
                }
                if (e.op == K::kMinus && a.kind == FStringConstValue::Kind::kInt) {
                    out.kind = FStringConstValue::Kind::kInt;
                    out.i64 = -a.i64;
                    return true;
                }
                if (e.op == K::kPlus && a.kind == FStringConstValue::Kind::kFloat) {
                    out = a;
                    return true;
                }
                if (e.op == K::kMinus && a.kind == FStringConstValue::Kind::kFloat) {
                    out.kind = FStringConstValue::Kind::kFloat;
                    out.f64 = -a.f64;
                    return true;
                }
                if (e.op == K::kKwNot && a.kind == FStringConstValue::Kind::kBool) {
                    out.kind = FStringConstValue::Kind::kBool;
                    out.b = !a.b;
                    return true;
                }
                if (e.op == K::kBang && a.kind == FStringConstValue::Kind::kInt) {
                    out.kind = FStringConstValue::Kind::kInt;
                    out.i64 = ~a.i64;
                    return true;
                }
                return fail("fstring interpolation: unsupported unary const expression");
            }
            case ast::ExprKind::kBinary: {
                FStringConstValue a{};
                FStringConstValue b{};
                if (!eval_fstring_const_expr_(e.a, a)) return false;
                if (!eval_fstring_const_expr_(e.b, b)) return false;

                if (a.kind == FStringConstValue::Kind::kInt && b.kind == FStringConstValue::Kind::kInt) {
                    switch (e.op) {
                        case K::kPlus: out.kind = FStringConstValue::Kind::kInt; out.i64 = a.i64 + b.i64; return true;
                        case K::kMinus: out.kind = FStringConstValue::Kind::kInt; out.i64 = a.i64 - b.i64; return true;
                        case K::kStar: out.kind = FStringConstValue::Kind::kInt; out.i64 = a.i64 * b.i64; return true;
                        case K::kSlash:
                            if (b.i64 == 0) return fail("fstring interpolation: division by zero");
                            out.kind = FStringConstValue::Kind::kInt;
                            out.i64 = a.i64 / b.i64;
                            return true;
                        case K::kPercent:
                            if (b.i64 == 0) return fail("fstring interpolation: modulo by zero");
                            out.kind = FStringConstValue::Kind::kInt;
                            out.i64 = a.i64 % b.i64;
                            return true;
                        case K::kEqEq: out.kind = FStringConstValue::Kind::kBool; out.b = (a.i64 == b.i64); return true;
                        case K::kBangEq: out.kind = FStringConstValue::Kind::kBool; out.b = (a.i64 != b.i64); return true;
                        case K::kLt: out.kind = FStringConstValue::Kind::kBool; out.b = (a.i64 < b.i64); return true;
                        case K::kLtEq: out.kind = FStringConstValue::Kind::kBool; out.b = (a.i64 <= b.i64); return true;
                        case K::kGt: out.kind = FStringConstValue::Kind::kBool; out.b = (a.i64 > b.i64); return true;
                        case K::kGtEq: out.kind = FStringConstValue::Kind::kBool; out.b = (a.i64 >= b.i64); return true;
                        default:
                            break;
                    }
                }

                if (a.kind == FStringConstValue::Kind::kFloat && b.kind == FStringConstValue::Kind::kFloat) {
                    switch (e.op) {
                        case K::kPlus: out.kind = FStringConstValue::Kind::kFloat; out.f64 = a.f64 + b.f64; return true;
                        case K::kMinus: out.kind = FStringConstValue::Kind::kFloat; out.f64 = a.f64 - b.f64; return true;
                        case K::kStar: out.kind = FStringConstValue::Kind::kFloat; out.f64 = a.f64 * b.f64; return true;
                        case K::kSlash:
                            out.kind = FStringConstValue::Kind::kFloat;
                            out.f64 = a.f64 / b.f64;
                            return true;
                        case K::kEqEq: out.kind = FStringConstValue::Kind::kBool; out.b = (a.f64 == b.f64); return true;
                        case K::kBangEq: out.kind = FStringConstValue::Kind::kBool; out.b = (a.f64 != b.f64); return true;
                        case K::kLt: out.kind = FStringConstValue::Kind::kBool; out.b = (a.f64 < b.f64); return true;
                        case K::kLtEq: out.kind = FStringConstValue::Kind::kBool; out.b = (a.f64 <= b.f64); return true;
                        case K::kGt: out.kind = FStringConstValue::Kind::kBool; out.b = (a.f64 > b.f64); return true;
                        case K::kGtEq: out.kind = FStringConstValue::Kind::kBool; out.b = (a.f64 >= b.f64); return true;
                        default:
                            break;
                    }
                }

                if (a.kind == FStringConstValue::Kind::kBool && b.kind == FStringConstValue::Kind::kBool) {
                    switch (e.op) {
                        case K::kEqEq: out.kind = FStringConstValue::Kind::kBool; out.b = (a.b == b.b); return true;
                        case K::kBangEq: out.kind = FStringConstValue::Kind::kBool; out.b = (a.b != b.b); return true;
                        case K::kKwAnd:
                            out.kind = FStringConstValue::Kind::kBool;
                            out.b = (a.b && b.b);
                            return true;
                        case K::kKwOr:
                            out.kind = FStringConstValue::Kind::kBool;
                            out.b = (a.b || b.b);
                            return true;
                        default:
                            break;
                    }
                }

                if (e.op == K::kPlus &&
                    a.kind == FStringConstValue::Kind::kText &&
                    b.kind == FStringConstValue::Kind::kText) {
                    out.kind = FStringConstValue::Kind::kText;
                    out.text = a.text;
                    out.text += b.text;
                    return true;
                }

                return fail("fstring interpolation: unsupported binary const expression");
            }
            case ast::ExprKind::kTernary: {
                FStringConstValue c{};
                if (!eval_fstring_const_expr_(e.a, c)) return false;
                if (c.kind != FStringConstValue::Kind::kBool) {
                    return fail("fstring interpolation: ternary condition must be bool");
                }
                return eval_fstring_const_expr_(c.b ? e.b : e.c, out);
            }
            default:
                return fail("fstring interpolation currently supports only simple compile-time expressions");
        }
    }

    bool TypeChecker::fold_fstring_expr_(ast::ExprId string_eid, std::string& out_bytes) {
        if (string_eid == ast::k_invalid_expr || string_eid >= ast_.exprs().size()) return false;
        const ast::Expr& e = ast_.expr(string_eid);
        if (e.kind != ast::ExprKind::kStringLit || !e.string_is_format) return false;

        const auto& parts = ast_.fstring_parts();
        const uint32_t begin = e.string_part_begin;
        const uint32_t count = e.string_part_count;
        if (begin > parts.size() || begin + count > parts.size()) {
            diag_(diag::Code::kTypeErrorGeneric, e.span, "internal fstring part range is invalid");
            err_(e.span, "internal fstring part range is invalid");
            return false;
        }

        out_bytes.clear();
        for (uint32_t i = 0; i < count; ++i) {
            const auto& p = parts[begin + i];
            if (!p.is_expr) {
                out_bytes.append(p.text.data(), p.text.size());
                continue;
            }

            FStringConstValue v{};
            if (!eval_fstring_const_expr_(p.expr, v)) return false;
            std::string seg;
            if (!fstring_const_to_text_(v, seg)) {
                diag_(diag::Code::kTypeErrorGeneric, p.span, "fstring interpolation to_text conversion failed");
                err_(p.span, "fstring interpolation to_text conversion failed");
                return false;
            }
            out_bytes += seg;
        }
        return true;
    }

    bool TypeChecker::try_fold_fstring_expr_no_diag_(ast::ExprId string_eid, std::string& out_bytes) {
        if (string_eid == ast::k_invalid_expr || string_eid >= ast_.exprs().size()) return false;
        const ast::Expr& root = ast_.expr(string_eid);
        if (root.kind != ast::ExprKind::kStringLit || !root.string_is_format) return false;

        const auto& parts = ast_.fstring_parts();
        const uint32_t begin = root.string_part_begin;
        const uint32_t count = root.string_part_count;
        if (begin > parts.size() || begin + count > parts.size()) return false;

        auto eval_const = [&](auto&& self, ast::ExprId expr_eid, FStringConstValue& out) -> bool {
            if (expr_eid == ast::k_invalid_expr || expr_eid >= ast_.exprs().size()) return false;
            const ast::Expr& e = ast_.expr(expr_eid);

            switch (e.kind) {
                case ast::ExprKind::kIntLit: {
                    int64_t v = 0;
                    if (!parse_i64_lit_(e.text, v)) return false;
                    out.kind = FStringConstValue::Kind::kInt;
                    out.i64 = v;
                    return true;
                }
                case ast::ExprKind::kFloatLit: {
                    double v = 0.0;
                    if (!parse_f64_lit_(e.text, v)) return false;
                    out.kind = FStringConstValue::Kind::kFloat;
                    out.f64 = v;
                    return true;
                }
                case ast::ExprKind::kBoolLit:
                    out.kind = FStringConstValue::Kind::kBool;
                    out.b = (e.text == "true");
                    return true;
                case ast::ExprKind::kCharLit: {
                    char v = '\0';
                    if (!parse_char_byte_lit_(e.text, v)) return false;
                    out.kind = FStringConstValue::Kind::kChar;
                    out.ch = v;
                    return true;
                }
                case ast::ExprKind::kStringLit: {
                    if (e.string_is_format) {
                        std::string folded;
                        if (!try_fold_fstring_expr_no_diag_(expr_eid, folded)) return false;
                        out.kind = FStringConstValue::Kind::kText;
                        out.text = std::move(folded);
                        return true;
                    }

                    std::string bytes;
                    if (!decode_string_bytes_(e, bytes)) return false;
                    out.kind = FStringConstValue::Kind::kText;
                    out.text = std::move(bytes);
                    return true;
                }
                case ast::ExprKind::kUnary: {
                    FStringConstValue a{};
                    if (!self(self, e.a, a)) return false;

                    if (e.op == K::kPlus && a.kind == FStringConstValue::Kind::kInt) {
                        out = a;
                        return true;
                    }
                    if (e.op == K::kMinus && a.kind == FStringConstValue::Kind::kInt) {
                        out.kind = FStringConstValue::Kind::kInt;
                        out.i64 = -a.i64;
                        return true;
                    }
                    if (e.op == K::kPlus && a.kind == FStringConstValue::Kind::kFloat) {
                        out = a;
                        return true;
                    }
                    if (e.op == K::kMinus && a.kind == FStringConstValue::Kind::kFloat) {
                        out.kind = FStringConstValue::Kind::kFloat;
                        out.f64 = -a.f64;
                        return true;
                    }
                    if (e.op == K::kKwNot && a.kind == FStringConstValue::Kind::kBool) {
                        out.kind = FStringConstValue::Kind::kBool;
                        out.b = !a.b;
                        return true;
                    }
                    if (e.op == K::kBang && a.kind == FStringConstValue::Kind::kInt) {
                        out.kind = FStringConstValue::Kind::kInt;
                        out.i64 = ~a.i64;
                        return true;
                    }
                    return false;
                }
                case ast::ExprKind::kBinary: {
                    FStringConstValue a{};
                    FStringConstValue b{};
                    if (!self(self, e.a, a)) return false;
                    if (!self(self, e.b, b)) return false;

                    if (a.kind == FStringConstValue::Kind::kInt && b.kind == FStringConstValue::Kind::kInt) {
                        switch (e.op) {
                            case K::kPlus: out.kind = FStringConstValue::Kind::kInt; out.i64 = a.i64 + b.i64; return true;
                            case K::kMinus: out.kind = FStringConstValue::Kind::kInt; out.i64 = a.i64 - b.i64; return true;
                            case K::kStar: out.kind = FStringConstValue::Kind::kInt; out.i64 = a.i64 * b.i64; return true;
                            case K::kSlash:
                                if (b.i64 == 0) return false;
                                out.kind = FStringConstValue::Kind::kInt;
                                out.i64 = a.i64 / b.i64;
                                return true;
                            case K::kPercent:
                                if (b.i64 == 0) return false;
                                out.kind = FStringConstValue::Kind::kInt;
                                out.i64 = a.i64 % b.i64;
                                return true;
                            case K::kEqEq: out.kind = FStringConstValue::Kind::kBool; out.b = (a.i64 == b.i64); return true;
                            case K::kBangEq: out.kind = FStringConstValue::Kind::kBool; out.b = (a.i64 != b.i64); return true;
                            case K::kLt: out.kind = FStringConstValue::Kind::kBool; out.b = (a.i64 < b.i64); return true;
                            case K::kLtEq: out.kind = FStringConstValue::Kind::kBool; out.b = (a.i64 <= b.i64); return true;
                            case K::kGt: out.kind = FStringConstValue::Kind::kBool; out.b = (a.i64 > b.i64); return true;
                            case K::kGtEq: out.kind = FStringConstValue::Kind::kBool; out.b = (a.i64 >= b.i64); return true;
                            default:
                                break;
                        }
                    }

                    if (a.kind == FStringConstValue::Kind::kFloat && b.kind == FStringConstValue::Kind::kFloat) {
                        switch (e.op) {
                            case K::kPlus: out.kind = FStringConstValue::Kind::kFloat; out.f64 = a.f64 + b.f64; return true;
                            case K::kMinus: out.kind = FStringConstValue::Kind::kFloat; out.f64 = a.f64 - b.f64; return true;
                            case K::kStar: out.kind = FStringConstValue::Kind::kFloat; out.f64 = a.f64 * b.f64; return true;
                            case K::kSlash: out.kind = FStringConstValue::Kind::kFloat; out.f64 = a.f64 / b.f64; return true;
                            case K::kEqEq: out.kind = FStringConstValue::Kind::kBool; out.b = (a.f64 == b.f64); return true;
                            case K::kBangEq: out.kind = FStringConstValue::Kind::kBool; out.b = (a.f64 != b.f64); return true;
                            case K::kLt: out.kind = FStringConstValue::Kind::kBool; out.b = (a.f64 < b.f64); return true;
                            case K::kLtEq: out.kind = FStringConstValue::Kind::kBool; out.b = (a.f64 <= b.f64); return true;
                            case K::kGt: out.kind = FStringConstValue::Kind::kBool; out.b = (a.f64 > b.f64); return true;
                            case K::kGtEq: out.kind = FStringConstValue::Kind::kBool; out.b = (a.f64 >= b.f64); return true;
                            default:
                                break;
                        }
                    }

                    if (a.kind == FStringConstValue::Kind::kBool && b.kind == FStringConstValue::Kind::kBool) {
                        switch (e.op) {
                            case K::kEqEq: out.kind = FStringConstValue::Kind::kBool; out.b = (a.b == b.b); return true;
                            case K::kBangEq: out.kind = FStringConstValue::Kind::kBool; out.b = (a.b != b.b); return true;
                            case K::kKwAnd: out.kind = FStringConstValue::Kind::kBool; out.b = (a.b && b.b); return true;
                            case K::kKwOr: out.kind = FStringConstValue::Kind::kBool; out.b = (a.b || b.b); return true;
                            default:
                                break;
                        }
                    }

                    if (e.op == K::kPlus &&
                        a.kind == FStringConstValue::Kind::kText &&
                        b.kind == FStringConstValue::Kind::kText) {
                        out.kind = FStringConstValue::Kind::kText;
                        out.text = a.text;
                        out.text += b.text;
                        return true;
                    }

                    return false;
                }
                case ast::ExprKind::kTernary: {
                    FStringConstValue c{};
                    if (!self(self, e.a, c)) return false;
                    if (c.kind != FStringConstValue::Kind::kBool) return false;
                    return self(self, c.b ? e.b : e.c, out);
                }
                default:
                    return false;
            }
        };

        out_bytes.clear();
        for (uint32_t i = 0; i < count; ++i) {
            const auto& p = parts[begin + i];
            if (!p.is_expr) {
                out_bytes.append(p.text.data(), p.text.size());
                continue;
            }

            FStringConstValue v{};
            if (!eval_const(eval_const, p.expr, v)) return false;

            std::string seg;
            if (!fstring_const_to_text_(v, seg)) return false;
            out_bytes += seg;
        }
        return true;
    }

    bool TypeChecker::check_fstring_runtime_form_(ast::ExprId string_eid, ast::ExprId& out_runtime_text_expr) {
        out_runtime_text_expr = ast::k_invalid_expr;
        if (string_eid == ast::k_invalid_expr || string_eid >= ast_.exprs().size()) return false;
        const ast::Expr& e = ast_.expr(string_eid);
        if (e.kind != ast::ExprKind::kStringLit || !e.string_is_format) return false;

        const auto& parts = ast_.fstring_parts();
        const uint32_t begin = e.string_part_begin;
        const uint32_t count = e.string_part_count;
        if (begin > parts.size() || begin + count > parts.size()) {
            diag_(diag::Code::kTypeErrorGeneric, e.span, "internal fstring part range is invalid");
            err_(e.span, "internal fstring part range is invalid");
            return false;
        }

        if (count != 1) {
            diag_(diag::Code::kFStringRuntimeShapeUnsupported, e.span);
            err_(e.span, "runtime f-string shape is not supported in v1");
            return false;
        }

        const auto& only = parts[begin];
        if (!only.is_expr || only.expr == ast::k_invalid_expr) {
            diag_(diag::Code::kFStringRuntimeShapeUnsupported, only.span);
            err_(only.span, "runtime f-string shape is not supported in v1");
            return false;
        }

        const ty::TypeId expr_t = check_expr_(only.expr, Slot::kValue);
        if (expr_t != string_type_) {
            if (!is_error_(expr_t)) {
                diag_(diag::Code::kFStringRuntimeExprMustBeText, only.span, types_.to_string(expr_t));
            }
            err_(only.span, "runtime f-string interpolation expression must be text");
            return false;
        }

        out_runtime_text_expr = only.expr;
        return true;
    }

    ty::TypeId TypeChecker::check_expr_(ast::ExprId eid, Slot slot) {
        if (eid == ast::k_invalid_expr) return types_.error();
        if (eid >= expr_type_cache_.size()) return types_.error();

        const ast::Expr& e = ast_.expr(eid);
        const ast::ExprId saved_expr_id = current_expr_id_;
        current_expr_id_ = eid;

        // NOTE(slot-sensitive caching)
        // - 일부 expr는 "Value vs Discard" 컨텍스트에 따라 진단/타입 규칙이 달라질 수 있다.
        // - 특히 block-expr는 Slot::kValue에서 tail이 없으면 에러를 내야 한다.
        //   그런데 Discard 컨텍스트에서 먼저 체크되며 캐시에 타입이 박히면,
        //   나중에 Value 컨텍스트에서 재방문해도 진단이 발생하지 않는 버그가 생긴다.
        //
        // 정책:
        // - Slot에 의존하는 expr(kind)가 있다면, 그 expr는 "Value에서만 캐시"하거나
        //   아예 캐시를 우회해서 컨텍스트별로 재검사한다.
        const bool slot_sensitive =
            (e.kind == ast::ExprKind::kBlockExpr); // v0: currently only block-expr depends on slot

        // memoized
        // - slot-sensitive expr는 Value에서만 캐시를 신뢰한다.
        if (!slot_sensitive || slot == Slot::kValue) {
            if (expr_type_cache_[eid] != ty::kInvalidType) {
                current_expr_id_ = saved_expr_id;
                return expr_type_cache_[eid];
            }
        }

        ty::TypeId t = types_.error();

        switch (e.kind) {
            case ast::ExprKind::kError:
                t = types_.error();
                break;

            case ast::ExprKind::kIntLit: {
                const ParsedIntLiteral lit = parse_int_literal_(e.text);
                if (!lit.ok) {
                    diag_(diag::Code::kIntLiteralInvalid, e.span, e.text);
                    err_(e.span, "invalid integer literal");
                    t = types_.error();
                    break;
                }

                num::BigInt v;
                if (!num::BigInt::parse_dec(lit.digits_no_sep, v)) {
                    diag_(diag::Code::kIntLiteralInvalid, e.span, e.text);
                    err_(e.span, "invalid integer literal");
                    t = types_.error();
                    break;
                }

                if (lit.has_suffix) {
                    t = types_.builtin(lit.suffix);
                    if (!fits_builtin_int_big_(v, lit.suffix)) {
                        diag_(diag::Code::kIntLiteralOverflow, e.span, e.text, types_.to_string(t));
                        err_(e.span, "integer literal overflow");
                        t = types_.error();
                    }
                } else {
                    t = types_.builtin(ty::Builtin::kInferInteger);
                    PendingInt pi{};
                    pi.value = v;
                    pi.has_value = true;
                    pi.resolved = false;
                    pi.resolved_type = ty::kInvalidType;
                    pending_int_expr_[(uint32_t)eid] = pi;
                }
                break;
            }

            case ast::ExprKind::kFloatLit: {
                const ParsedFloatLiteral lit = parse_float_literal_(e.text);
                if (!lit.ok) {
                    diag_(diag::Code::kTypeErrorGeneric, e.span, "invalid float literal");
                    err_(e.span, "invalid float literal");
                    t = types_.error();
                    break;
                }

                t = types_.builtin(lit.builtin);
                break;
            }

            case ast::ExprKind::kStringLit: {
                const bool is_c_literal =
                    starts_with_(e.text, "c\"") || starts_with_(e.text, "cr\"");
                if (is_c_literal) {
                    std::string c_bytes{};
                    if (!decode_c_string_bytes_(e.text, c_bytes)) {
                        diag_(diag::Code::kTypeErrorGeneric, e.span, "invalid C string literal");
                        err_(e.span, "invalid C string literal");
                        t = types_.error();
                        break;
                    }

                    bool has_nul = false;
                    for (size_t i = 0; i < c_bytes.size(); ++i) {
                        if (c_bytes[i] == '\0') {
                            has_nul = true;
                            break;
                        }
                    }
                    if (has_nul) {
                        diag_(diag::Code::kCStringInteriorNulForbidden, e.span);
                        err_(e.span, "C string literal must not contain interior NUL byte");
                        t = types_.error();
                        break;
                    }

                    const std::string quoted = quote_bytes_as_string_lit_(c_bytes);
                    auto& me = ast_.expr_mut(eid);
                    me.string_folded_text = ast_.add_owned_string(quoted);

                    auto lookup_cstr = [&](std::string_view qname) -> ty::TypeId {
                        if (auto sid = sym_.lookup(std::string(qname))) {
                            const auto& ss = sym_.symbol(*sid);
                            if (ss.kind == sema::SymbolKind::kType &&
                                ss.declared_type != ty::kInvalidType) {
                                return ss.declared_type;
                            }
                        }
                        return ty::kInvalidType;
                    };
                    t = lookup_cstr("core::ext::CStr");
                    if (t == ty::kInvalidType) t = lookup_cstr("core::CStr");
                    if (t == ty::kInvalidType) t = lookup_cstr("ext::CStr");
                    if (t == ty::kInvalidType) t = lookup_cstr("CStr");
                    if (t == ty::kInvalidType) {
                        diag_(diag::Code::kCStringLiteralRequiresCoreExt, e.span);
                        err_(e.span, "C string literal requires core::ext::CStr (core injection unavailable)");
                        t = types_.error();
                    }
                    break;
                }

                if (e.string_is_format) {
                    std::string folded;
                    if (try_fold_fstring_expr_no_diag_(eid, folded)) {
                        const std::string quoted = quote_bytes_as_string_lit_(folded);
                        auto& me = ast_.expr_mut(eid);
                        me.string_folded_text = ast_.add_owned_string(quoted);
                        if ((size_t)eid < expr_fstring_runtime_expr_cache_.size()) {
                            expr_fstring_runtime_expr_cache_[eid] = ast::k_invalid_expr;
                        }
                    } else {
                        ast::ExprId runtime_text_expr = ast::k_invalid_expr;
                        if (!check_fstring_runtime_form_(eid, runtime_text_expr)) {
                            t = types_.error();
                            break;
                        }
                        if ((size_t)eid < expr_fstring_runtime_expr_cache_.size()) {
                            expr_fstring_runtime_expr_cache_[eid] = runtime_text_expr;
                        }
                    }
                } else if ((size_t)eid < expr_fstring_runtime_expr_cache_.size()) {
                    expr_fstring_runtime_expr_cache_[eid] = ast::k_invalid_expr;
                }
                t = string_type_;
                break;
            }

            case ast::ExprKind::kCharLit:
                t = types_.builtin(ty::Builtin::kChar);
                break;

            case ast::ExprKind::kBoolLit:
                t = types_.builtin(ty::Builtin::kBool);
                break;

            case ast::ExprKind::kNullLit:
                t = types_.builtin(ty::Builtin::kNull);
                break;

            case ast::ExprKind::kIdent: {
                auto id = lookup_symbol_(e.text);
                if (!id && !fn_sid_stack_.empty()) {
                    const ast::StmtId cur_fn_sid = fn_sid_stack_.back();
                    if (cur_fn_sid != ast::k_invalid_stmt && (size_t)cur_fn_sid < ast_.stmts().size()) {
                        const auto& fs = ast_.stmt(cur_fn_sid);
                        if (fs.kind == ast::StmtKind::kFnDecl) {
                            for (uint32_t i = 0; i < fs.param_count; ++i) {
                                const uint32_t pidx = fs.param_begin + i;
                                if ((size_t)pidx >= ast_.params().size()) break;
                                const auto& p = ast_.params()[pidx];
                                if (p.name != e.text) continue;
                                if ((size_t)pidx < param_resolved_symbol_cache_.size()) {
                                    const uint32_t sid = param_resolved_symbol_cache_[pidx];
                                    if (sid != sema::SymbolTable::kNoScope) {
                                        id = sid;
                                    }
                                }
                                break;
                            }
                        }
                    }
                }
                if (!id) {
                    diag_(diag::Code::kUndefinedName, e.span, e.text);
                    err_(e.span, "unknown identifier");
                    t = types_.error();
                } else {
                    if (eid < expr_resolved_symbol_cache_.size()) {
                        expr_resolved_symbol_cache_[eid] = *id;
                    }
                    if (!suppress_ownership_read_) {
                        (void)ensure_symbol_readable_(*id, e.span);
                    }
                    const auto& ss = sym_.symbol(*id);
                    t = ss.declared_type;
                    if (t == ty::kInvalidType) t = types_.error();
                    t = canonicalize_transparent_external_typedef_(t);

                    if (ss.is_external && !ss.external_payload.empty()) {
                        ConstInitData imported_const{};
                        if (parse_external_c_const_payload_(ss.external_payload, imported_const)) {
                            if (current_expr_id_ != ast::k_invalid_expr) {
                                expr_external_const_value_cache_[current_expr_id_] = imported_const;
                            }
                        }
                    }

                    const auto& tt = types_.get(t);
                    if (tt.kind == ty::Kind::kBuiltin && tt.builtin == ty::Builtin::kInferInteger) {
                        auto it = pending_int_sym_.find(*id);
                        if (it != pending_int_sym_.end() && it->second.has_value) {
                            pending_int_expr_[(uint32_t)eid] = it->second;
                        }
                    }
                }
                break;
            }

            case ast::ExprKind::kHole:
                err_(e.span, "unresolved hole '_' in expression");
                diag_(diag::Code::kTypeUnresolvedHole, e.span);
                t = types_.error();
                break;

            case ast::ExprKind::kUnary:
                t = check_expr_unary_(e);
                break;

            case ast::ExprKind::kPostfixUnary:
                t = check_expr_postfix_unary_(e);
                break;

            case ast::ExprKind::kBinary:
                t = check_expr_binary_(e);
                break;

            case ast::ExprKind::kAssign:
                t = check_expr_assign_(e);
                break;

            case ast::ExprKind::kTernary:
                t = check_expr_ternary_(e);
                break;

            case ast::ExprKind::kCall:
                t = check_expr_call_(e);
                break;

            case ast::ExprKind::kIndex:
                t = check_expr_index_(e);
                break;

            case ast::ExprKind::kArrayLit:
                t = check_expr_array_lit_(e);
                break;

            case ast::ExprKind::kFieldInit:
                t = check_expr_field_init_(e);
                break;

            case ast::ExprKind::kIfExpr:
                t = check_expr_if_(e, slot);
                break;

            case ast::ExprKind::kBlockExpr:
                t = check_expr_block_(e, slot);
                break;

            case ast::ExprKind::kLoop:
                t = check_expr_loop_(e, slot);
                break;

            case ast::ExprKind::kCast:
                t = check_expr_cast_(e);
                break;

            case ast::ExprKind::kMacroCall:
                diag_(diag::Code::kMacroReparseFail, e.span, "unexpanded expr macro placeholder");
                err_(e.span, "unexpanded expr macro placeholder");
                t = types_.error();
                break;
        }

        // caching policy:
        // - slot-sensitive expr는 Value 컨텍스트에서만 캐시한다.
        //   (Discard에서의 결과를 캐시하면, 나중에 Value에서 필요한 진단이 누락될 수 있다.)
        if (!slot_sensitive || slot == Slot::kValue) {
            expr_type_cache_[eid] = t;
        }

        current_expr_id_ = saved_expr_id;
        return t;
    }

    ty::TypeId TypeChecker::check_expr_(ast::ExprId eid) {
        return check_expr_(eid, Slot::kValue);
    }

    // --------------------
    // helpers: type predicates
    // --------------------
    bool TypeChecker::is_optional_(ty::TypeId t) const {
        if (t == ty::kInvalidType) return false;
        return types_.get(t).kind == ty::Kind::kOptional;
    }

    ty::TypeId TypeChecker::optional_elem_(ty::TypeId opt) const {
        if (!is_optional_(opt)) return ty::kInvalidType;
        return types_.get(opt).elem;
    }

    bool TypeChecker::is_null_(ty::TypeId t) const {
        return t == types_.builtin(ty::Builtin::kNull);
    }

    bool TypeChecker::is_error_(ty::TypeId t) const {
        return t == types_.error() || types_.get(t).kind == ty::Kind::kError;
    }

    void TypeChecker::note_break_(ty::TypeId t, bool is_value_break) {
        if (loop_stack_.empty()) return;

        LoopCtx& lc = loop_stack_.back();
        lc.has_any_break = true;

        if (!is_value_break) {
            lc.has_null_break = true;
            return;
        }

        lc.has_value_break = true;

        if (lc.joined_value == ty::kInvalidType) {
            lc.joined_value = t;
        } else {
            lc.joined_value = unify_(lc.joined_value, t);
        }
    }

    bool TypeChecker::can_assign_(ty::TypeId dst, ty::TypeId src) const {
        if (is_error_(dst) || is_error_(src)) return true;
        if (dst == src) return true;

        // never -> anything (bottom type)
        if (src == types_.builtin(ty::Builtin::kNever)) return true;
        // only never can be assigned into never
        if (dst == types_.builtin(ty::Builtin::kNever)) return src == dst;

        dst = canonicalize_transparent_external_typedef_(dst);
        src = canonicalize_transparent_external_typedef_(src);
        if (dst == src) return true;

        // null -> T? 허용
        if (is_null_(src) && is_optional_(dst)) return true;

        const auto& dt = types_.get(dst);
        const auto& st = types_.get(src);

        // read-only coercion: &mut T -> &T
        if (dt.kind == ty::Kind::kBorrow && st.kind == ty::Kind::kBorrow) {
            if (!dt.borrow_is_mut && st.borrow_is_mut && dt.elem == st.elem) {
                return true;
            }
        }

        // array assignability (v0)
        // - elem type must be assignable
        // - dst T[] accepts src T[] and src T[N]
        // - dst T[N] accepts only src T[N] with same N
        if (dt.kind == ty::Kind::kArray && st.kind == ty::Kind::kArray) {
            if (dt.array_has_size) {
                if (!st.array_has_size) return false;
                if (dt.array_size != st.array_size) return false;
            }
            return can_assign_(dt.elem, st.elem);
        }

        // -------------------------------------------------
        // "{integer}" placeholder rules (Rust-like)
        // - placeholder can be assigned ONLY into an integer type (signed/unsigned),
        //   and only if the literal value fits (checked at resolution site).
        // - placeholder -> float is NOT allowed (no implicit int->float).
        // -------------------------------------------------
        auto is_int_builtin = [&](ty::Builtin b) -> bool {
            return b == ty::Builtin::kI8 || b == ty::Builtin::kI16 || b == ty::Builtin::kI32 ||
                b == ty::Builtin::kI64 || b == ty::Builtin::kI128 ||
                b == ty::Builtin::kU8 || b == ty::Builtin::kU16 || b == ty::Builtin::kU32 ||
                b == ty::Builtin::kU64 || b == ty::Builtin::kU128 ||
                b == ty::Builtin::kISize || b == ty::Builtin::kUSize ||
                b == ty::Builtin::kCChar || b == ty::Builtin::kCSChar || b == ty::Builtin::kCUChar ||
                b == ty::Builtin::kCShort || b == ty::Builtin::kCUShort ||
                b == ty::Builtin::kCInt || b == ty::Builtin::kCUInt ||
                b == ty::Builtin::kCLong || b == ty::Builtin::kCULong ||
                b == ty::Builtin::kCLongLong || b == ty::Builtin::kCULongLong ||
                b == ty::Builtin::kCSize || b == ty::Builtin::kCSSize ||
                b == ty::Builtin::kCPtrDiff;
        };

        auto is_float_builtin = [&](ty::Builtin b) -> bool {
            return b == ty::Builtin::kF32 || b == ty::Builtin::kF64 || b == ty::Builtin::kF128 ||
                   b == ty::Builtin::kCFloat || b == ty::Builtin::kCDouble;
        };

        const bool dst_is_builtin = (dt.kind == ty::Kind::kBuiltin);
        const bool src_is_builtin = (st.kind == ty::Kind::kBuiltin);

        if (dst_is_builtin && src_is_builtin &&
            st.builtin == ty::Builtin::kInferInteger) {

            if (is_float_builtin(dt.builtin)) {
                return false;
            }

            if (!is_int_builtin(dt.builtin)) {
                return false;
            }

            // 값 fit 여부는 소비 지점에서 resolve할 때 체크한다.
            return true;
        }

        return false;
    }

    std::string TypeChecker::type_for_user_diag_(ty::TypeId t, ast::ExprId eid) const {
        (void)eid;
        if (t == ty::kInvalidType) return "<invalid>";
        const auto& tt = types_.get(t);
        if (tt.kind == ty::Kind::kBuiltin && tt.builtin == ty::Builtin::kInferInteger) {
            return "unsuffixed integer literal";
        }
        return types_.to_string(t);
    }

    TypeChecker::CoercionPlan TypeChecker::classify_assign_with_coercion_(
        AssignSite site,
        ty::TypeId dst,
        ast::ExprId src_eid,
        Span diag_span
    ) {
        (void)diag_span;

        CoercionPlan plan{};
        plan.dst = dst;
        if (src_eid == ast::k_invalid_expr) {
            plan.ok = false;
            plan.kind = CoercionKind::Reject;
            plan.src_before = types_.error();
            plan.src_after = types_.error();
            return plan;
        }

        ty::TypeId src = check_expr_(src_eid);
        plan.src_before = src;
        plan.src_after = src;
        if (is_error_(dst) || is_error_(src)) {
            plan.ok = true;
            plan.kind = CoercionKind::Exact;
            return plan;
        }

        const bool dst_is_opt = is_optional_(dst);
        const ty::TypeId dst_elem = dst_is_opt ? optional_elem_(dst) : ty::kInvalidType;
        plan.optional_elem = dst_elem;

        const auto assign_site_allows_null_to_ptr_boundary = [&](AssignSite s) -> bool {
            switch (s) {
                case AssignSite::LetInit:
                case AssignSite::SetInit:
                case AssignSite::Assign:
                case AssignSite::FieldInit:
                case AssignSite::CallArg:
                case AssignSite::Return:
                    return true;
                case AssignSite::DefaultArg:
                case AssignSite::NullCoalesceAssign:
                    return false;
            }
            return false;
        };

        bool infer_resolved = false;
        const auto resolve_array_literal_in_context = [&](ty::TypeId expected) -> bool {
            if (src_eid == ast::k_invalid_expr ||
                static_cast<size_t>(src_eid) >= ast_.exprs().size() ||
                expected == ty::kInvalidType) {
                return false;
            }
            const ast::Expr& src_expr = ast_.expr(src_eid);
            if (src_expr.kind != ast::ExprKind::kArrayLit) return false;

            expected = canonicalize_transparent_external_typedef_(expected);
            if (expected == ty::kInvalidType || expected >= types_.count()) return false;

            const auto& expected_t = types_.get(expected);
            if (expected_t.kind != ty::Kind::kArray) return false;

            ty::TypeId array_expected = expected;
            if (!expected_t.array_has_size) {
                array_expected = types_.make_array(expected_t.elem, /*has_size=*/true, src_expr.arg_count);
            }
            return resolve_infer_int_in_context_(src_eid, array_expected);
        };

        if (!is_error_(src)) {
            const ty::TypeId infer_expected_elem =
                (dst_is_opt && dst_elem != ty::kInvalidType) ? dst_elem : dst;
            const ty::TypeId infer_expected_full = dst;
            if (type_contains_infer_int_(src)) {
                bool resolved_now = false;

                if (dst_is_opt && infer_expected_full != ty::kInvalidType) {
                    resolved_now = resolve_infer_int_in_context_(src_eid, infer_expected_full);
                }

                if (!resolved_now &&
                    infer_expected_elem != ty::kInvalidType &&
                    resolve_array_literal_in_context(infer_expected_elem)) {
                    resolved_now = true;
                }

                if (!resolved_now && infer_expected_elem != ty::kInvalidType) {
                    resolved_now = resolve_infer_int_in_context_(src_eid, infer_expected_elem);
                }

                if (!resolved_now &&
                    dst_is_opt &&
                    src_eid != ast::k_invalid_expr &&
                    static_cast<size_t>(src_eid) < ast_.exprs().size()) {
                    const ast::Expr& src_expr = ast_.expr(src_eid);
                    if (src_expr.kind == ast::ExprKind::kIdent) {
                        auto sid = lookup_symbol_(src_expr.text);
                        if (sid) {
                            auto origin_it = pending_int_sym_origin_.find(*sid);
                            if (origin_it != pending_int_sym_origin_.end()) {
                                ty::TypeId finalized = ty::kInvalidType;
                                if (finalize_infer_int_shape_(origin_it->second, src, finalized)) {
                                    sym_.update_declared_type(*sid, finalized);
                                    resolved_now = true;
                                }
                            }
                        }
                    }
                }

                if (!resolved_now) {
                    ty::TypeId finalized = ty::kInvalidType;
                    if (finalize_infer_int_shape_(src_eid, src, finalized)) {
                        if (static_cast<size_t>(src_eid) < expr_type_cache_.size()) {
                            expr_type_cache_[src_eid] = finalized;
                        }
                        auto& pe = pending_int_expr_[(uint32_t)src_eid];
                        pe.has_value = false;
                        pe.resolved = true;
                        pe.resolved_type = finalized;
                        resolved_now = true;
                    }
                }

                if (resolved_now) {
                    infer_resolved = true;
                    src = check_expr_(src_eid);
                    plan.src_after = src;
                }
            }
        }

        if (can_assign_(dst, src)) {
            plan.ok = true;
            plan.kind = infer_resolved ? CoercionKind::InferThenExact : CoercionKind::Exact;
            return plan;
        }

        const bool src_is_null_literal =
            src_eid != ast::k_invalid_expr &&
            static_cast<size_t>(src_eid) < ast_.exprs().size() &&
            ast_.expr(src_eid).kind == ast::ExprKind::kNullLit;
        if (src_is_null_literal &&
            assign_site_allows_null_to_ptr_boundary(site) &&
            dst != ty::kInvalidType &&
            dst < types_.count() &&
            types_.get(dst).kind == ty::Kind::kPtr) {
            plan.ok = true;
            plan.kind = CoercionKind::NullToPtrBoundary;
            plan.src_after = src;
            return plan;
        }

        if (dst_is_opt && dst_elem != ty::kInvalidType) {
            if (is_null_(src)) {
                plan.ok = true;
                plan.kind = CoercionKind::NullToOptionalNone;
                return plan;
            }

            if (can_assign_(dst_elem, src)) {
                plan.ok = true;
                plan.kind = infer_resolved
                    ? CoercionKind::InferThenLiftToOptionalSome
                    : CoercionKind::LiftToOptionalSome;
                return plan;
            }
        }

        plan.ok = false;
        plan.kind = CoercionKind::Reject;
        return plan;
    }

    ty::TypeId TypeChecker::unify_(ty::TypeId a, ty::TypeId b) {
        if (is_error_(a) || is_error_(b)) return types_.error();
        if (a == b) return a;

        if (a == types_.builtin(ty::Builtin::kNever)) return b;
        if (b == types_.builtin(ty::Builtin::kNever)) return a;

        // null + T? => T?
        if (is_null_(a) && is_optional_(b)) return b;
        if (is_null_(b) && is_optional_(a)) return a;

        // null + T => T? (정책: 삼항 등에서 null 섞이면 옵셔널로 승격)
        if (is_null_(a)) return types_.make_optional(b);
        if (is_null_(b)) return types_.make_optional(a);

        // v0: 나머지는 error
        return types_.error();
    }

    // v0: place(ident/index)에서 "근원 로컬 심볼"을 최대한 뽑는다.
    // - ident      => sym id
    // - index(a,i) => a가 ident면 그 sym id (v0 보수 규칙)
    std::optional<uint32_t> TypeChecker::root_place_symbol_(ast::ExprId place) const {
        if (place == ast::k_invalid_expr) return std::nullopt;
        const ast::Expr& e = ast_.expr(place);

        if (e.kind == ast::ExprKind::kIdent) {
            auto sid = lookup_symbol_(e.text);
            if (!sid) return std::nullopt;
            return *sid;
        }

        if (e.kind == ast::ExprKind::kIndex) {
            return root_place_symbol_(e.a);
        }

        if (e.kind == ast::ExprKind::kBinary && e.op == K::kDot) {
            return root_place_symbol_(e.a);
        }

        if (e.kind == ast::ExprKind::kUnary && e.op == K::kStar) {
            return root_place_symbol_(e.a);
        }

        return std::nullopt;
    }

    bool TypeChecker::is_mutable_symbol_(uint32_t sym_id) const {
        auto it = sym_is_mut_.find(sym_id);
        if (it == sym_is_mut_.end()) {
            if (sym_id >= sym_.symbols().size()) return false;
            const auto& sym = sym_.symbol(sym_id);
            if (!sym.is_external || sym.kind != sema::SymbolKind::kVar || sym.external_payload.empty()) {
                return false;
            }

            // Imported external const payload is immutable by definition.
            ConstInitData imported_const{};
            if (parse_external_c_const_payload_(sym.external_payload, imported_const)) {
                return false;
            }

            // Imported C globals are mutable unless explicitly const-qualified.
            ExternalCGlobalMeta gmeta{};
            if (parse_external_c_global_payload_(sym.external_payload, gmeta)) {
                return !gmeta.is_const;
            }
            return false;
        }
        return it->second;
    }

    bool TypeChecker::is_global_like_symbol_(uint32_t sym_id) const {
        if (sym_id == sema::SymbolTable::kNoScope || sym_id >= sym_.symbols().size()) return false;
        return sym_.symbol(sym_id).owner_scope == 0;
    }

    bool TypeChecker::class_has_user_deinit_(ty::TypeId t) const {
        auto it = class_decl_by_type_.find(t);
        if (it == class_decl_by_type_.end()) return false;
        const ast::StmtId sid = it->second;
        if (sid == ast::k_invalid_stmt || (size_t)sid >= ast_.stmts().size()) return false;
        const auto& cls = ast_.stmt(sid);
        const auto& kids = ast_.stmt_children();
        const uint64_t begin = cls.stmt_begin;
        const uint64_t end = begin + cls.stmt_count;
        if (begin > kids.size() || end > kids.size()) return false;
        for (uint32_t i = cls.stmt_begin; i < cls.stmt_begin + cls.stmt_count; ++i) {
            const ast::StmtId msid = kids[i];
            if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
            const auto& m = ast_.stmt(msid);
            if (m.kind == ast::StmtKind::kFnDecl && m.name == "deinit") return true;
        }
        return false;
    }

    bool TypeChecker::type_needs_drop_(ty::TypeId t) const {
        std::unordered_set<ty::TypeId> visiting{};
        auto rec = [&](auto&& self, ty::TypeId cur) -> bool {
            if (cur == ty::kInvalidType || is_error_(cur)) return false;
            if (!visiting.insert(cur).second) return false;

            const auto& tt = types_.get(cur);
            switch (tt.kind) {
                case ty::Kind::kBuiltin:
                case ty::Kind::kBorrow:
                case ty::Kind::kEscape:
                case ty::Kind::kPtr:
                case ty::Kind::kFn:
                case ty::Kind::kError:
                    return false;

                case ty::Kind::kOptional:
                    return tt.elem != ty::kInvalidType && self(self, tt.elem);

                case ty::Kind::kArray:
                    if (!tt.array_has_size) return false;
                    return tt.elem != ty::kInvalidType && self(self, tt.elem);

                case ty::Kind::kNamedUser: {
                    if (actor_decl_by_type_.find(cur) != actor_decl_by_type_.end()) return true;

                    if (field_abi_meta_by_type_.find(cur) != field_abi_meta_by_type_.end()) {
                        const auto& layout = field_abi_meta_by_type_.at(cur);
                        const ast::StmtId sid = layout.sid;
                        if (sid != ast::k_invalid_stmt && (size_t)sid < ast_.stmts().size()) {
                            const auto& s = ast_.stmt(sid);
                            const uint64_t begin = s.field_member_begin;
                            const uint64_t end = begin + s.field_member_count;
                            if (begin <= ast_.field_members().size() && end <= ast_.field_members().size()) {
                                for (uint32_t i = s.field_member_begin; i < s.field_member_begin + s.field_member_count; ++i) {
                                    if (self(self, ast_.field_members()[i].type)) return true;
                                }
                            }
                        }
                    }

                    auto eit = enum_abi_meta_by_type_.find(cur);
                    if (eit != enum_abi_meta_by_type_.end()) {
                        for (const auto& variant : eit->second.variants) {
                            for (const auto& field : variant.fields) {
                                if (self(self, field.type)) return true;
                            }
                        }
                    }

                    if (class_has_user_deinit_(cur)) return true;
                    return false;
                }
            }
            return false;
        };
        return rec(rec, t);
    }

    bool TypeChecker::is_move_only_type_(ty::TypeId t) const {
        return type_needs_drop_(t);
    }

    bool TypeChecker::is_trivial_copy_clone_type_(ty::TypeId t) const {
        if (t == ty::kInvalidType || is_error_(t)) return false;
        const auto& tv = types_.get(t);
        if (tv.kind == ty::Kind::kBorrow) return true;
        if (tv.kind != ty::Kind::kBuiltin) return false;
        switch (tv.builtin) {
            case ty::Builtin::kBool:
            case ty::Builtin::kChar:
            case ty::Builtin::kI8:
            case ty::Builtin::kI16:
            case ty::Builtin::kI32:
            case ty::Builtin::kI64:
            case ty::Builtin::kI128:
            case ty::Builtin::kU8:
            case ty::Builtin::kU16:
            case ty::Builtin::kU32:
            case ty::Builtin::kU64:
            case ty::Builtin::kU128:
            case ty::Builtin::kISize:
            case ty::Builtin::kUSize:
            case ty::Builtin::kF32:
            case ty::Builtin::kF64:
            case ty::Builtin::kF128:
            case ty::Builtin::kCVoid:
            case ty::Builtin::kCChar:
            case ty::Builtin::kCSChar:
            case ty::Builtin::kCUChar:
            case ty::Builtin::kCShort:
            case ty::Builtin::kCUShort:
            case ty::Builtin::kCInt:
            case ty::Builtin::kCUInt:
            case ty::Builtin::kCLong:
            case ty::Builtin::kCULong:
            case ty::Builtin::kCLongLong:
            case ty::Builtin::kCULongLong:
            case ty::Builtin::kCFloat:
            case ty::Builtin::kCDouble:
            case ty::Builtin::kCSize:
            case ty::Builtin::kCSSize:
            case ty::Builtin::kCPtrDiff:
            case ty::Builtin::kVaList:
                return true;
            default:
                return false;
        }
    }

    TypeChecker::OwnershipState TypeChecker::ownership_state_of_(uint32_t sym_id) const {
        auto it = ownership_state_.find(sym_id);
        if (it == ownership_state_.end()) return OwnershipState::kInitialized;
        return it->second;
    }

    bool TypeChecker::ensure_symbol_readable_(uint32_t sym_id, Span use_span) {
        const auto it = ownership_state_.find(sym_id);
        if (it == ownership_state_.end()) return true;

        switch (it->second) {
            case OwnershipState::kInitialized:
                return true;

            case OwnershipState::kMovedUninitialized:
                diag_(diag::Code::kUseAfterMove, use_span, sym_.symbol(sym_id).name);
                err_(use_span, "value was moved and must be reinitialized before use");
                return false;

            case OwnershipState::kMaybeUninitialized:
                diag_(diag::Code::kMaybeUninitializedMoveOnlyUse, use_span, sym_.symbol(sym_id).name);
                err_(use_span, "value may be uninitialized after move on some control-flow path");
                return false;
        }
        return true;
    }

    void TypeChecker::mark_symbol_initialized_(uint32_t sym_id) {
        if (sym_id == sema::SymbolTable::kNoScope) return;
        if (!is_move_only_type_(sym_.symbol(sym_id).declared_type)) return;
        ownership_state_[sym_id] = OwnershipState::kInitialized;
    }

    void TypeChecker::mark_symbol_moved_(uint32_t sym_id) {
        if (sym_id == sema::SymbolTable::kNoScope) return;
        if (!is_move_only_type_(sym_.symbol(sym_id).declared_type)) return;
        ownership_state_[sym_id] = OwnershipState::kMovedUninitialized;
    }

    void TypeChecker::mark_expr_move_consumed_(ast::ExprId expr_id, ty::TypeId expected_type, Span diag_span) {
        if (!is_move_only_type_(expected_type)) return;
        if (expr_id == ast::k_invalid_expr) return;

        const auto root = root_place_symbol_(expr_id);
        if (!root.has_value()) return;

        if (is_global_like_symbol_(*root)) {
            diag_(diag::Code::kMoveFromGlobalOrStaticForbidden, diag_span, sym_.symbol(*root).name);
            err_(diag_span, "move from global/static storage is not allowed in v1");
            return;
        }

        const auto& e = ast_.expr(expr_id);
        if (e.kind != ast::ExprKind::kIdent) {
            diag_(diag::Code::kMoveFromNonRootPlaceNotAllowed, diag_span);
            err_(diag_span, "partial move from non-root place is not supported in v1");
            return;
        }

        if (!ensure_symbol_readable_(*root, e.span)) return;
        ownership_state_[*root] = OwnershipState::kMovedUninitialized;
    }

    TypeChecker::OwnershipStateMap TypeChecker::capture_ownership_state_() const {
        return ownership_state_;
    }

    void TypeChecker::restore_ownership_state_(const OwnershipStateMap& state) {
        ownership_state_ = state;
    }

    void TypeChecker::merge_ownership_state_from_branches_(
        const OwnershipStateMap& before,
        const std::vector<OwnershipStateMap>& branches,
        bool include_before_as_fallthrough
    ) {
        OwnershipStateMap merged = before;
        std::unordered_set<uint32_t> keys{};
        for (const auto& [sym, _] : before) keys.insert(sym);
        for (const auto& branch : branches) {
            for (const auto& [sym, _] : branch) keys.insert(sym);
        }

        auto get_state = [](const OwnershipStateMap& map, uint32_t sym) {
            auto it = map.find(sym);
            return (it == map.end()) ? OwnershipState::kInitialized : it->second;
        };
        auto merge_pair = [](OwnershipState a, OwnershipState b) {
            if (a == b) return a;
            return OwnershipState::kMaybeUninitialized;
        };

        for (const uint32_t sym : keys) {
            OwnershipState state = get_state(before, sym);
            bool seeded = include_before_as_fallthrough;
            if (!seeded && !branches.empty()) {
                state = get_state(branches.front(), sym);
                seeded = true;
            }
            if (!seeded) {
                merged[sym] = state;
                continue;
            }

            size_t begin = include_before_as_fallthrough ? 0u : 1u;
            for (size_t i = begin; i < branches.size(); ++i) {
                state = merge_pair(state, get_state(branches[i], sym));
            }
            merged[sym] = state;
        }

        ownership_state_ = std::move(merged);
    }

    ty::TypeId TypeChecker::check_expr_place_no_read_(ast::ExprId eid) {
        const bool saved = suppress_ownership_read_;
        suppress_ownership_read_ = true;
        ty::TypeId out = check_expr_(eid);
        suppress_ownership_read_ = saved;
        return out;
    }

    /// @brief range 식(`a..b`, `a..:b`)인지 확인한다.
    bool TypeChecker::is_range_expr_(ast::ExprId eid) const {
        if (eid == ast::k_invalid_expr) return false;
        const auto& e = ast_.expr(eid);
        if (e.kind != ast::ExprKind::kBinary) return false;
        return e.op == K::kDotDot || e.op == K::kDotDotColon;
    }

    /// @brief 인덱스/슬라이스 경계에 허용되는 정수 타입인지 확인한다.
    bool TypeChecker::is_index_int_type_(ty::TypeId t) const {
        if (t == ty::kInvalidType || is_error_(t)) return false;
        const auto& tt = types_.get(t);
        if (tt.kind != ty::Kind::kBuiltin) return false;

        switch (tt.builtin) {
            case ty::Builtin::kI8:
            case ty::Builtin::kI16:
            case ty::Builtin::kI32:
            case ty::Builtin::kI64:
            case ty::Builtin::kI128:
            case ty::Builtin::kU8:
            case ty::Builtin::kU16:
            case ty::Builtin::kU32:
            case ty::Builtin::kU64:
            case ty::Builtin::kU128:
            case ty::Builtin::kISize:
            case ty::Builtin::kUSize:
            case ty::Builtin::kCChar:
            case ty::Builtin::kCSChar:
            case ty::Builtin::kCUChar:
            case ty::Builtin::kCShort:
            case ty::Builtin::kCUShort:
            case ty::Builtin::kCInt:
            case ty::Builtin::kCUInt:
            case ty::Builtin::kCLong:
            case ty::Builtin::kCULong:
            case ty::Builtin::kCLongLong:
            case ty::Builtin::kCULongLong:
            case ty::Builtin::kCSize:
            case ty::Builtin::kCSSize:
            case ty::Builtin::kCPtrDiff:
                return true;
            default:
                return false;
        }
    }

    // place expr (v0: Ident, Index, Field(dot)만 place로 인정)
    bool TypeChecker::is_place_expr_(ast::ExprId eid) const {
        if (eid == ast::k_invalid_expr) return false;
        const auto& e = ast_.expr(eid);
        if (e.kind == ast::ExprKind::kIdent) return true;
        if (e.kind == ast::ExprKind::kIndex) {
            // range index는 slice 생성용 view이므로 v0에서 write/place로 취급하지 않는다.
            if (is_range_expr_(e.b)) return false;
            return is_place_expr_(e.a);
        }
        if (e.kind == ast::ExprKind::kBinary && e.op == K::kDot) {
            if (e.b == ast::k_invalid_expr) return false;
            const auto& rhs = ast_.expr(e.b);
            if (rhs.kind != ast::ExprKind::kIdent) return false;
            return is_place_expr_(e.a);
        }
        return false;
    }

    // --------------------
    // unary / postfix unary
    // --------------------
