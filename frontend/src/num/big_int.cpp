// frontend/src/num/big_int.cpp
#include <parus/num/BigInt.hpp>


namespace parus::num {

    static constexpr uint32_t kBase = 1000000000u; // 1e9

    void BigInt::normalize_() {
        while (!limbs_.empty() && limbs_.back() == 0) limbs_.pop_back();
        if (limbs_.empty()) neg_ = false;
    }

    void BigInt::mul_add_dec_(uint32_t mul, uint32_t add) {
        uint64_t carry = add;
        for (size_t i = 0; i < limbs_.size(); ++i) {
            uint64_t x = (uint64_t)limbs_[i] * mul + carry;
            limbs_[i] = (uint32_t)(x % kBase);
            carry = x / kBase;
        }
        if (carry) limbs_.push_back((uint32_t)carry);
    }

    bool BigInt::parse_dec(std::string_view text, BigInt& out) {
        out = BigInt{};
        if (text.empty()) return false;

        size_t i = 0;
        bool neg = false;

        if (text[0] == '-') { neg = true; i = 1; }
        if (i >= text.size()) return false;

        // no leading '+'
        for (; i < text.size(); ++i) {
            char c = text[i];
            if (c < '0' || c > '9') return false;
            out.mul_add_dec_(10, (uint32_t)(c - '0'));
        }

        out.normalize_();
        out.neg_ = neg && !out.is_zero();
        return true;
    }

    int BigInt::compare_abs(const BigInt& rhs) const {
        if (limbs_.size() < rhs.limbs_.size()) return -1;
        if (limbs_.size() > rhs.limbs_.size()) return 1;
        for (size_t i = limbs_.size(); i-- > 0;) {
            if (limbs_[i] < rhs.limbs_[i]) return -1;
            if (limbs_[i] > rhs.limbs_[i]) return 1;
        }
        return 0;
    }

    int BigInt::compare(const BigInt& rhs) const {
        const bool a_neg = is_neg();
        const bool b_neg = rhs.is_neg();
        if (a_neg != b_neg) return a_neg ? -1 : 1;

        int abs = compare_abs(rhs);
        if (!a_neg) return abs;
        return -abs; // both negative
    }

    std::string BigInt::to_string(size_t max_digits) const {
        if (is_zero()) return "0";

        // naive conversion: repeated div by 10 is heavy.
        // For diagnostics: print via base limbs, quick and readable enough.
        // This prints full number, but can be truncated by max_digits.
        std::string s;
        if (is_neg()) s.push_back('-');

        // print most significant limb without padding, then others with 9-digit padding
        size_t n = limbs_.size();
        uint32_t ms = limbs_[n - 1];

        auto append_uint = [&](uint32_t v, bool pad9) {
            char buf[16];
            int len = 0;

            if (!pad9) {
                // no pad
                uint32_t x = v;
                char tmp[16];
                do {
                    tmp[len++] = char('0' + (x % 10));
                    x /= 10;
                } while (x);
                for (int i = len - 1; i >= 0; --i) s.push_back(tmp[i]);
                return;
            }

            // pad9
            for (int k = 0; k < 9; ++k) {
                buf[8 - k] = char('0' + (v % 10));
                v /= 10;
            }
            for (int k = 0; k < 9; ++k) s.push_back(buf[k]);
        };

        append_uint(ms, false);
        for (size_t i = n - 1; i-- > 0;) append_uint(limbs_[i], true);

        if (s.size() > max_digits) {
            s.resize(max_digits);
            s += "...";
        }
        return s;
    }

    // ---- decimal constants helpers ----
    // NOTE: keep it simple: store constants as decimal strings and parse once (static local).
    static BigInt parse_const_(const char* dec) {
        BigInt x;
        (void)BigInt::parse_dec(dec, x);
        return x;
    }

    const BigInt& BigInt::const_i128_min_abs_() { static BigInt v = parse_const_("170141183460469231731687303715884105728"); return v; } // 2^127
    const BigInt& BigInt::const_i128_max_()     { static BigInt v = parse_const_("170141183460469231731687303715884105727"); return v; } // 2^127 - 1
    const BigInt& BigInt::const_u128_max_()     { static BigInt v = parse_const_("340282366920938463463374607431768211455"); return v; } // 2^128 - 1

    const BigInt& BigInt::const_u64_max_()      { static BigInt v = parse_const_("18446744073709551615"); return v; }
    const BigInt& BigInt::const_i64_max_()      { static BigInt v = parse_const_("9223372036854775807"); return v; }
    const BigInt& BigInt::const_i64_min_abs_()  { static BigInt v = parse_const_("9223372036854775808"); return v; }

    const BigInt& BigInt::const_u32_max_()      { static BigInt v = parse_const_("4294967295"); return v; }
    const BigInt& BigInt::const_i32_max_()      { static BigInt v = parse_const_("2147483647"); return v; }
    const BigInt& BigInt::const_i32_min_abs_()  { static BigInt v = parse_const_("2147483648"); return v; }

    const BigInt& BigInt::const_u16_max_()      { static BigInt v = parse_const_("65535"); return v; }
    const BigInt& BigInt::const_i16_max_()      { static BigInt v = parse_const_("32767"); return v; }
    const BigInt& BigInt::const_i16_min_abs_()  { static BigInt v = parse_const_("32768"); return v; }

    const BigInt& BigInt::const_u8_max_()       { static BigInt v = parse_const_("255"); return v; }
    const BigInt& BigInt::const_i8_max_()       { static BigInt v = parse_const_("127"); return v; }
    const BigInt& BigInt::const_i8_min_abs_()   { static BigInt v = parse_const_("128"); return v; }

    bool BigInt::fits_i8() const {
        if (is_zero()) return true;
        if (is_neg()) return compare_abs(const_i8_min_abs_()) <= 0;
        return compare_abs(const_i8_max_()) <= 0;
    }
    
    bool BigInt::fits_i16() const {
        if (is_zero()) return true;
        if (is_neg()) return compare_abs(const_i16_min_abs_()) <= 0;
        return compare_abs(const_i16_max_()) <= 0;
    }
    
    bool BigInt::fits_i32() const {
        if (is_zero()) return true;
        if (is_neg()) return compare_abs(const_i32_min_abs_()) <= 0;
        return compare_abs(const_i32_max_()) <= 0;
    }
    
    bool BigInt::fits_i64() const {
        if (is_zero()) return true;
        if (is_neg()) return compare_abs(const_i64_min_abs_()) <= 0;
        return compare_abs(const_i64_max_()) <= 0;
    }

    bool BigInt::fits_i128() const {
        if (is_zero()) return true;
        if (is_neg()) return compare_abs(const_i128_min_abs_()) <= 0;
        return compare_abs(const_i128_max_()) <= 0;
    }

    bool BigInt::fits_u8() const   { return !is_neg() && compare_abs(const_u8_max_()) <= 0; }
    bool BigInt::fits_u16() const  { return !is_neg() && compare_abs(const_u16_max_()) <= 0; }
    bool BigInt::fits_u32() const  { return !is_neg() && compare_abs(const_u32_max_()) <= 0; }
    bool BigInt::fits_u64() const  { return !is_neg() && compare_abs(const_u64_max_()) <= 0; }
    bool BigInt::fits_u128() const { return !is_neg() && compare_abs(const_u128_max_()) <= 0; }

} // namespace parus::num