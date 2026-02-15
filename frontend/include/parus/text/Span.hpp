// frontend/include/parus/text/Span.hpp
#pragma once
#include <cstdint>


namespace parus {

    struct Span {
        uint32_t file_id = 0;
        uint32_t lo = 0;   // byte offset inclusive
        uint32_t hi = 0;   // byte offset exclusive
    };

} // namespace parus