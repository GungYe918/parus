#include <cstddef>
#include <cstdint>

namespace std {
inline namespace __1 {

std::size_t __hash_memory(const void* data, std::size_t size) noexcept {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    std::size_t hash = sizeof(std::size_t) == 8
        ? static_cast<std::size_t>(14695981039346656037ull)
        : static_cast<std::size_t>(2166136261u);
    const std::size_t prime = sizeof(std::size_t) == 8
        ? static_cast<std::size_t>(1099511628211ull)
        : static_cast<std::size_t>(16777619u);

    for (std::size_t i = 0; i < size; ++i) {
        hash ^= static_cast<std::size_t>(bytes[i]);
        hash *= prime;
    }
    return hash;
}

} // namespace __1
} // namespace std
