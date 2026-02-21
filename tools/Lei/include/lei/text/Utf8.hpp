#pragma once

#include <cstdint>
#include <string_view>

namespace lei::text {

// Strict UTF-8 validator.
// Returns false and sets bad_off when an invalid byte sequence is found.
bool validate_utf8_strict(std::string_view s, uint32_t& bad_off);

} // namespace lei::text
