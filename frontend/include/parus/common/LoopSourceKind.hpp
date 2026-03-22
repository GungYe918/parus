#pragma once

#include <cstdint>

namespace parus {

    enum class LoopSourceKind : uint8_t {
        kNone = 0,
        kSizedArray,
        kSliceView,
        kRangeExclusive,
        kRangeInclusive,
        kSequence,
        kIteratorFutureUnsupported,
    };

} // namespace parus
