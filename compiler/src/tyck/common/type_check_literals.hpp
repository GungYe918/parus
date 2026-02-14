// compiler/src/tyck/common/type_check_literals.hpp
#pragma once
#include <gaupel/ty/Type.hpp>

#include <cctype>
#include <string>
#include <string_view>


namespace gaupel::tyck::detail {

    /// @brief 정수 리터럴의 본문/접미사 파싱 결과를 담는다.
    struct ParsedIntLiteral {
        bool ok = false;
        bool has_suffix = false;
        ty::Builtin suffix = ty::Builtin::kI32;
        std::string digits_no_sep{};
    };

    /// @brief 실수 리터럴의 접미사 파싱 결과를 담는다.
    struct ParsedFloatLiteral {
        bool ok = false;
        ty::Builtin builtin = ty::Builtin::kF64;
    };

    /// @brief 숫자 리터럴 문자열에서 `_` 구분자를 제거한다.
    inline std::string strip_underscores_(std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            if (c == '_') continue;
            out.push_back(c);
        }
        return out;
    }

    /// @brief 정수 접미사를 builtin 정수 타입으로 변환한다.
    inline bool parse_int_suffix_(std::string_view s, ty::Builtin& out) {
        using B = ty::Builtin;
        if (s == "i8")    { out = B::kI8; return true; }
        if (s == "i16")   { out = B::kI16; return true; }
        if (s == "i32")   { out = B::kI32; return true; }
        if (s == "i64")   { out = B::kI64; return true; }
        if (s == "i128")  { out = B::kI128; return true; }
        if (s == "u8")    { out = B::kU8; return true; }
        if (s == "u16")   { out = B::kU16; return true; }
        if (s == "u32")   { out = B::kU32; return true; }
        if (s == "u64")   { out = B::kU64; return true; }
        if (s == "u128")  { out = B::kU128; return true; }
        if (s == "isize") { out = B::kISize; return true; }
        if (s == "usize") { out = B::kUSize; return true; }
        return false;
    }

    /// @brief `123`, `10_000u32` 같은 정수 리터럴 문자열을 파싱한다.
    inline ParsedIntLiteral parse_int_literal_(std::string_view lit) {
        ParsedIntLiteral out{};
        if (lit.empty()) return out;

        size_t i = 0;
        bool saw_digit = false;
        while (i < lit.size()) {
            const unsigned char u = static_cast<unsigned char>(lit[i]);
            if (std::isdigit(u)) {
                saw_digit = true;
                ++i;
                continue;
            }
            if (lit[i] == '_') {
                ++i;
                continue;
            }
            break;
        }
        if (!saw_digit) return out;

        const std::string_view body = lit.substr(0, i);
        const std::string_view suffix = lit.substr(i);

        out.digits_no_sep = strip_underscores_(body);
        if (out.digits_no_sep.empty()) return out;

        if (suffix.empty()) {
            out.ok = true;
            out.has_suffix = false;
            return out;
        }

        ty::Builtin b{};
        if (!parse_int_suffix_(suffix, b)) {
            return out;
        }

        out.ok = true;
        out.has_suffix = true;
        out.suffix = b;
        return out;
    }

    /// @brief `10.0f32`, `1_000.25` 같은 실수 리터럴 문자열을 파싱한다.
    inline ParsedFloatLiteral parse_float_literal_(std::string_view lit) {
        ParsedFloatLiteral out{};
        if (lit.empty()) return out;

        size_t i = 0;
        bool saw_digit = false;
        while (i < lit.size()) {
            const unsigned char u = static_cast<unsigned char>(lit[i]);
            if (std::isdigit(u)) {
                saw_digit = true;
                ++i;
                continue;
            }
            if (lit[i] == '_') {
                ++i;
                continue;
            }
            break;
        }
        if (!saw_digit) return out;

        if (i < lit.size() && lit[i] == '.') {
            ++i;
            while (i < lit.size()) {
                const unsigned char u = static_cast<unsigned char>(lit[i]);
                if (std::isdigit(u) || lit[i] == '_') {
                    ++i;
                    continue;
                }
                break;
            }
        }

        const std::string_view suffix = lit.substr(i);
        if (suffix.empty()) {
            out.ok = true;
            out.builtin = ty::Builtin::kF64;
            return out;
        }

        if (suffix == "f" || suffix == "f32") {
            out.ok = true;
            out.builtin = ty::Builtin::kF32;
            return out;
        }
        if (suffix == "lf" || suffix == "f64") {
            out.ok = true;
            out.builtin = ty::Builtin::kF64;
            return out;
        }
        if (suffix == "f128") {
            out.ok = true;
            out.builtin = ty::Builtin::kF128;
            return out;
        }

        return out;
    }

} // namespace gaupel::tyck::detail
