// compiler/include/parus/num/BigInt.hpp
#pragma once
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>


namespace parus::num {

    // Minimal BigInt for decimal parsing + comparison.
    // - Base 1e9 limbs (little-endian)
    // - Supports sign, parse from decimal, compare, and small-range checks for i/u(8..128).
    class BigInt {
    public:
        BigInt() = default;

        static BigInt zero() { return BigInt(); }

        bool is_zero() const { return limbs_.empty(); }
        bool is_neg() const { return neg_ && !is_zero(); }

        // Parse decimal: [-]?[0-9]+
        // Returns false if invalid text.
        static bool parse_dec(std::string_view text, BigInt& out);

        // Compare: -1,0,1
        int compare(const BigInt& rhs) const;

        // absolute compare
        int compare_abs(const BigInt& rhs) const;

        // String (for diagnostics; may truncate)
        std::string to_string(size_t max_digits = 80) const;

        // Range checks for builtin ints
        bool fits_i8()   const;
        bool fits_i16()  const;
        bool fits_i32()  const;
        bool fits_i64()  const;
        bool fits_i128() const;

        bool fits_u8()   const;
        bool fits_u16()  const;
        bool fits_u32()  const;
        bool fits_u64()  const;
        bool fits_u128() const;

    private:
        // helpers
        void normalize_();

        // multiply by small (<= 10) and add digit (0..9)
        void mul_add_dec_(uint32_t mul, uint32_t add);

        // Compare abs with (2^k) boundaries: implement via decimal precomputed constants
        static const BigInt& const_i128_min_abs_(); // 2^127
        static const BigInt& const_i128_max_();     // 2^127 - 1
        static const BigInt& const_u128_max_();     // 2^128 - 1

        static const BigInt& const_u64_max_();
        static const BigInt& const_i64_max_();
        static const BigInt& const_i64_min_abs_();

        static const BigInt& const_u32_max_();
        static const BigInt& const_i32_max_();
        static const BigInt& const_i32_min_abs_();

        static const BigInt& const_u16_max_();
        static const BigInt& const_i16_max_();
        static const BigInt& const_i16_min_abs_();

        static const BigInt& const_u8_max_();
        static const BigInt& const_i8_max_();
        static const BigInt& const_i8_min_abs_();

        bool neg_ = false;
        std::vector<uint32_t> limbs_; // base 1e9
    };

} // namespace parus::num