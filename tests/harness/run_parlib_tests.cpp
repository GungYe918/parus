// tests/harness/run_parlib_tests.cpp
#include <parus/backend/parlib/Parlib.hpp>

#include <filesystem>
#include <iostream>
#include <string>
#include <unordered_set>

namespace {

    bool require_(bool cond, const char* msg) {
        if (cond) return true;
        std::cerr << "  - " << msg << "\n";
        return false;
    }

    std::string chunk_key_(parus::backend::parlib::ParlibChunkKind kind, parus::backend::parlib::ParlibLane lane) {
        using namespace parus::backend::parlib;
        return chunk_kind_name(kind) + "::" + lane_name(lane);
    }

    /// @brief v1 parlib 빌드/검증 경로가 동작하는지 검사한다.
    bool test_build_and_inspect_v1_() {
        using namespace parus::backend::parlib;
        namespace fs = std::filesystem;

        const fs::path out_path = fs::temp_directory_path() / "parus_parlib_v1_test.parlib";
        std::error_code ec;
        fs::remove(out_path, ec);

        ParlibBuildOptions opt{};
        opt.output_path = out_path.string();
        opt.target_triple = "aarch64-apple-darwin";
        opt.feature_bits = 0x0000'0000'0000'0003ull;
        opt.flags = 0;
        opt.include_pcore = true;
        opt.include_prt = true;
        opt.include_pstd = true;
        opt.include_debug = true;

        const auto built = build_parlib(opt);
        bool ok = true;
        ok &= require_(built.ok, "parlib build must succeed");
        ok &= require_(fs::exists(out_path), "parlib file must be created");
        ok &= require_(built.file_size > 0, "parlib file size must be > 0");
        if (!ok) return false;

        const auto inspected = inspect_parlib(out_path.string());
        ok &= require_(inspected.ok, "parlib inspect must succeed");
        ok &= require_(inspected.header.format_major == 1, "format major must be 1");
        ok &= require_(inspected.header.toc_entry_size == 48, "TOC entry size must be 48");
        ok &= require_(inspected.header.toc_entry_count == inspected.chunks.size(),
                       "TOC entry count must match parsed chunk count");
        ok &= require_(inspected.chunks.size() == 15, "default lanes(3) + debug should produce 15 chunks");
        if (!ok) return false;

        std::unordered_set<std::string> keys;
        for (const auto& c : inspected.chunks) {
            keys.insert(chunk_key_(c.kind, c.lane));
            ok &= require_(c.alignment != 0, "chunk alignment must be non-zero");
            ok &= require_(c.offset % c.alignment == 0, "chunk offset must satisfy chunk alignment");
        }

        ok &= require_(keys.contains(chunk_key_(ParlibChunkKind::kManifest, ParlibLane::kGlobal)),
                       "Manifest::global chunk must exist");
        ok &= require_(keys.contains(chunk_key_(ParlibChunkKind::kStringTable, ParlibLane::kGlobal)),
                       "StringTable::global chunk must exist");
        ok &= require_(keys.contains(chunk_key_(ParlibChunkKind::kDebug, ParlibLane::kGlobal)),
                       "Debug::global chunk must exist when include_debug=true");

        for (ParlibLane lane : {ParlibLane::kPcore, ParlibLane::kPrt, ParlibLane::kPstd}) {
            ok &= require_(keys.contains(chunk_key_(ParlibChunkKind::kSymbolIndex, lane)),
                           "SymbolIndex chunk must exist for all enabled lanes");
            ok &= require_(keys.contains(chunk_key_(ParlibChunkKind::kTypeMeta, lane)),
                           "TypeMeta chunk must exist for all enabled lanes");
            ok &= require_(keys.contains(chunk_key_(ParlibChunkKind::kOirArchive, lane)),
                           "OIRArchive chunk must exist for all enabled lanes");
            ok &= require_(keys.contains(chunk_key_(ParlibChunkKind::kObjectArchive, lane)),
                           "ObjectArchive chunk must exist for all enabled lanes");
        }

        return ok;
    }

} // namespace

int main() {
    bool ok = true;
    ok &= test_build_and_inspect_v1_();

    if (!ok) {
        std::cerr << "[parlib tests] FAILED\n";
        return 1;
    }
    std::cout << "[parlib tests] OK\n";
    return 0;
}
