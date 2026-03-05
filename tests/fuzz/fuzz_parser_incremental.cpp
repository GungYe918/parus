#include <parus/parse/IncrementalParse.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <span>
#include <string>

namespace {

static std::string apply_edit_(const std::string& old_src, uint32_t lo, uint32_t hi, std::string_view repl) {
    std::string out;
    out.reserve(old_src.size() + repl.size() + 8);
    out.append(old_src.data(), lo);
    out.append(repl.data(), repl.size());
    out.append(old_src.data() + hi, old_src.size() - hi);
    return out;
}

struct DisableIncrementalMergeEnv {
    DisableIncrementalMergeEnv() {
        setenv("PARUS_DISABLE_INCREMENTAL_MERGE", "1", 1);
    }
} kDisableIncrementalMergeEnv{};

} // namespace

extern "C" const char* __asan_default_options() {
    return "detect_container_overflow=0:detect_leaks=0";
}

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (data == nullptr || size == 0) return 0;

    std::string src(reinterpret_cast<const char*>(data), size);

    parus::parse::IncrementalParserSession session;
    parus::diag::Bag init_bag;
    (void)session.initialize(src, /*file_id=*/1, init_bag);

    static constexpr char kAlphabet[] =
        " \n\t(){}[];,:+-*/_=<>!&?|"
        "abcdefghijklmnopqrstuvwxyz"
        "ABCDEFGHIJKLMNOPQRSTUVWXYZ"
        "0123456789";

    size_t cursor = 0;
    constexpr uint32_t kMaxSteps = 8;
    for (uint32_t step = 0; step < kMaxSteps && cursor + 3 < size; ++step) {
        const uint32_t n = static_cast<uint32_t>(src.size());

        const uint32_t lo = (n == 0) ? 0 : static_cast<uint32_t>(data[cursor++] % (n + 1));
        const uint32_t span = (n > lo) ? static_cast<uint32_t>(data[cursor++] % (std::min<uint32_t>(n - lo, 4u) + 1)) : 0;
        const uint32_t hi = lo + span;

        const uint32_t repl_len = static_cast<uint32_t>(data[cursor++] % 4u);
        std::string repl;
        repl.reserve(repl_len);
        for (uint32_t i = 0; i < repl_len && cursor < size; ++i) {
            const uint8_t b = data[cursor++];
            repl.push_back(kAlphabet[b % (sizeof(kAlphabet) - 1)]);
        }

        const std::string next_src = apply_edit_(src, lo, hi, repl);

        parus::diag::Bag bag;
        const parus::parse::EditWindow edit{lo, hi};
        (void)session.reparse_with_edits(next_src, /*file_id=*/1, std::span<const parus::parse::EditWindow>(&edit, 1), bag);

        src = next_src;
    }

    return 0;
}
