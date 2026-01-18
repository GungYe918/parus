// compiler/include/slyte/text/Span.hpp
#pragma once
#include <cstdint>


namespace slyte {

    struct Span {
        uint32_t file_id = 0;
        uint32_t lo = 0;   // byte offset inclusive
        uint32_t hi = 0;   // byte offset exclusive
    };

} // namespace slyte