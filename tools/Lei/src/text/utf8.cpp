#include <lei/text/Utf8.hpp>

namespace lei::text {

bool validate_utf8_strict(std::string_view s, uint32_t& bad_off) {
    const auto is_cont = [](unsigned char b) -> bool {
        return (b & 0xC0) == 0x80;
    };

    std::size_t i = 0;
    while (i < s.size()) {
        const unsigned char b0 = static_cast<unsigned char>(s[i]);

        if (b0 < 0x80) {
            i += 1;
            continue;
        }

        if (b0 >= 0x80 && b0 <= 0xBF) {
            bad_off = static_cast<uint32_t>(i);
            return false;
        }

        if (b0 >= 0xC2 && b0 <= 0xDF) {
            if (i + 1 >= s.size()) {
                bad_off = static_cast<uint32_t>(i);
                return false;
            }
            const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
            if (!is_cont(b1)) {
                bad_off = static_cast<uint32_t>(i);
                return false;
            }
            i += 2;
            continue;
        }

        if (b0 >= 0xE0 && b0 <= 0xEF) {
            if (i + 2 >= s.size()) {
                bad_off = static_cast<uint32_t>(i);
                return false;
            }
            const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
            const unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
            if (!is_cont(b1) || !is_cont(b2)) {
                bad_off = static_cast<uint32_t>(i);
                return false;
            }
            if (b0 == 0xE0 && b1 < 0xA0) {
                bad_off = static_cast<uint32_t>(i);
                return false;
            }
            if (b0 == 0xED && b1 >= 0xA0) {
                bad_off = static_cast<uint32_t>(i);
                return false;
            }
            i += 3;
            continue;
        }

        if (b0 >= 0xF0 && b0 <= 0xF4) {
            if (i + 3 >= s.size()) {
                bad_off = static_cast<uint32_t>(i);
                return false;
            }
            const unsigned char b1 = static_cast<unsigned char>(s[i + 1]);
            const unsigned char b2 = static_cast<unsigned char>(s[i + 2]);
            const unsigned char b3 = static_cast<unsigned char>(s[i + 3]);
            if (!is_cont(b1) || !is_cont(b2) || !is_cont(b3)) {
                bad_off = static_cast<uint32_t>(i);
                return false;
            }
            if (b0 == 0xF0 && b1 < 0x90) {
                bad_off = static_cast<uint32_t>(i);
                return false;
            }
            if (b0 == 0xF4 && b1 > 0x8F) {
                bad_off = static_cast<uint32_t>(i);
                return false;
            }
            i += 4;
            continue;
        }

        bad_off = static_cast<uint32_t>(i);
        return false;
    }

    return true;
}

} // namespace lei::text
