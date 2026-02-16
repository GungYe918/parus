// tests/harness/run_parlib_tests.cpp
#include <parus/backend/parlib/Parlib.hpp>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <sstream>
#include <string>
#include <unordered_set>
#include <vector>

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

    bool messages_have_text_(const std::vector<parus::backend::CompileMessage>& msgs, std::string_view needle) {
        for (const auto& m : msgs) {
            if (m.text.find(needle) != std::string::npos) return true;
        }
        return false;
    }

    void write_u16_le_(std::vector<uint8_t>& out, size_t off, uint16_t v) {
        out[off + 0] = static_cast<uint8_t>(v & 0xFFu);
        out[off + 1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
    }

    void write_u32_le_(std::vector<uint8_t>& out, size_t off, uint32_t v) {
        out[off + 0] = static_cast<uint8_t>(v & 0xFFu);
        out[off + 1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
        out[off + 2] = static_cast<uint8_t>((v >> 16u) & 0xFFu);
        out[off + 3] = static_cast<uint8_t>((v >> 24u) & 0xFFu);
    }

    void write_u64_le_(std::vector<uint8_t>& out, size_t off, uint64_t v) {
        for (uint32_t i = 0; i < 8; ++i) {
            out[off + i] = static_cast<uint8_t>((v >> (8u * i)) & 0xFFu);
        }
    }

    /// @brief PARLIB.md의 필수 청크/포맷/인덱스 조건을 통합 검증한다.
    bool test_build_and_inspect_v1_() {
        using namespace parus::backend::parlib;
        namespace fs = std::filesystem;

        const fs::path out_path = fs::temp_directory_path() / "parus_parlib_v1_spec_test.parlib";
        std::error_code ec;
        fs::remove(out_path, ec);

        ParlibBuildOptions opt{};
        opt.output_path = out_path.string();
        opt.bundle_id = "ffi_demo_bundle";
        opt.target_triple = "aarch64-apple-darwin";
        opt.target_summary = "darwin-arm64";
        opt.feature_bits = 0x0000'0000'0000'0031ull;
        opt.flags = 0x11u;
        opt.compiler_hash = 0x1020'3040'5060'7080ull;
        opt.include_pcore = true;
        opt.include_prt = true;
        opt.include_pstd = true;
        opt.include_debug = true;

        ParlibExportCEntry exp{};
        exp.symbol = "p_add";
        exp.signature = "(i32,i32)->i32";
        exp.lane = ParlibLane::kPcore;
        exp.chunk_kind = ParlibChunkKind::kObjectArchive;
        exp.target_id = 0;
        exp.visible = true;
        opt.export_c_symbols.push_back(exp);

        ParlibNativeDepEntry dep{};
        dep.name = "c";
        dep.kind = ParlibNativeDepKind::kSystem;
        dep.mode = ParlibNativeDepMode::kReference;
        dep.target_filter = "*";
        dep.link_order = 0;
        dep.required = true;
        dep.hash = 0;
        dep.reference = "-lc";
        opt.native_deps.push_back(dep);

        ParlibChunkPayload source_map{};
        source_map.kind = ParlibChunkKind::kSourceMap;
        source_map.lane = ParlibLane::kGlobal;
        source_map.target_id = 0;
        source_map.alignment = 8;
        source_map.compression = ParlibCompression::kNone;
        const std::string sm = "main.pr\tsha256:dummy\n";
        source_map.bytes.assign(sm.begin(), sm.end());
        opt.extra_chunks.push_back(source_map);

        const auto built = build_parlib(opt);
        bool ok = true;
        ok &= require_(built.ok, "parlib build must succeed");
        ok &= require_(fs::exists(out_path), "parlib file must be created");
        ok &= require_(built.file_size > 0, "parlib file size must be > 0");
        if (!ok) return false;

        const auto inspected = inspect_parlib(out_path.string());
        ok &= require_(inspected.ok, "parlib inspect must succeed");
        ok &= require_(inspected.header.format_major == 1, "format major must be 1");
        ok &= require_(inspected.header.format_minor == 1, "format minor must be 1");
        ok &= require_(inspected.header.toc_entry_size == 64, "TOC entry size must be 64");
        ok &= require_(inspected.header.header_size == 256, "header size must be 256");
        ok &= require_(inspected.header.toc_offset > inspected.header.chunk_stream_offset,
                       "TOC must be placed after chunk stream");
        ok &= require_(inspected.header.file_size == built.file_size,
                       "header file_size must match build result");
        ok &= require_(inspected.chunks.size() == 18,
                       "mandatory(4 global + 12 lane) + debug + source_map must produce 18 chunks");
        if (!ok) return false;

        std::unordered_set<std::string> keys;
        for (const auto& c : inspected.chunks) {
            keys.insert(chunk_key_(c.kind, c.lane));
            ok &= require_(c.alignment != 0, "chunk alignment must be non-zero");
            ok &= require_(c.offset % c.alignment == 0, "chunk offset must satisfy alignment");
        }

        ok &= require_(keys.contains(chunk_key_(ParlibChunkKind::kManifest, ParlibLane::kGlobal)),
                       "Manifest::global chunk must exist");
        ok &= require_(keys.contains(chunk_key_(ParlibChunkKind::kStringTable, ParlibLane::kGlobal)),
                       "StringTable::global chunk must exist");
        ok &= require_(keys.contains(chunk_key_(ParlibChunkKind::kExportCIndex, ParlibLane::kGlobal)),
                       "ExportCIndex::global chunk must exist");
        ok &= require_(keys.contains(chunk_key_(ParlibChunkKind::kNativeDeps, ParlibLane::kGlobal)),
                       "NativeDeps::global chunk must exist");
        ok &= require_(keys.contains(chunk_key_(ParlibChunkKind::kDebug, ParlibLane::kGlobal)),
                       "Debug::global chunk must exist when include_debug=true");
        ok &= require_(keys.contains(chunk_key_(ParlibChunkKind::kSourceMap, ParlibLane::kGlobal)),
                       "SourceMap::global chunk must exist when provided as extra chunk");

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

        ok &= require_(!inspected.export_c_symbols.empty(), "ExportCIndex must be parsed");
        ok &= require_(!inspected.native_deps.empty(), "NativeDeps must be parsed");
        ok &= require_(inspected.export_c_symbols.front().symbol == "p_add",
                       "parsed export symbol must match input");
        ok &= require_(inspected.native_deps.front().name == "c",
                       "parsed native dep must match input");
        if (!ok) return false;

        auto reader_opt = ParlibReader::open(out_path.string());
        ok &= require_(reader_opt.has_value(), "ParlibReader::open must succeed");
        if (!ok) return false;

        const auto& reader = *reader_opt;
        auto obj_rec = reader.find_chunk(ParlibChunkKind::kObjectArchive, ParlibLane::kPcore, 0);
        ok &= require_(obj_rec.has_value(), "ParlibReader::find_chunk must locate lane object chunk");
        if (!ok) return false;

        const auto first_bytes = reader.read_chunk_slice(*obj_rec, 0, 16);
        ok &= require_(!first_bytes.empty(), "ParlibReader::read_chunk_slice must support partial read");

        auto stream = reader.open_chunk_stream(*obj_rec);
        ok &= require_(stream.ok(), "ParlibReader::open_chunk_stream must return valid stream");
        std::vector<uint8_t> stream_chunk;
        ok &= require_(stream.read_some(stream_chunk, 8), "chunk stream must provide first segment");

        const auto hit = reader.lookup_export_c("p_add");
        ok &= require_(hit.has_value(), "lookup_export_c must find symbol from ExportCIndex");

        return ok;
    }

    /// @brief v1 리더가 레거시(major=1, minor=0) 헤더를 명시적으로 거부하는지 검사한다.
    bool test_legacy_format_rejected_() {
        namespace fs = std::filesystem;

        const fs::path p = fs::temp_directory_path() / "parus_parlib_legacy_reject.bin";
        std::error_code ec;
        fs::remove(p, ec);

        std::vector<uint8_t> image(300, 0);
        image[0] = 'P'; image[1] = 'R'; image[2] = 'L'; image[3] = 'B';
        write_u16_le_(image, 4, 1);   // major
        write_u16_le_(image, 6, 0);   // legacy minor
        write_u32_le_(image, 12, 256);
        write_u64_le_(image, 48, static_cast<uint64_t>(image.size()));

        {
            std::ofstream ofs(p, std::ios::binary | std::ios::trunc);
            ofs.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
        }

        const auto inspected = parus::backend::parlib::inspect_parlib(p.string());
        bool ok = true;
        ok &= require_(!inspected.ok, "legacy parlib format must be rejected");
        ok &= require_(messages_have_text_(inspected.messages, "legacy parlib format is not supported"),
                       "legacy reject reason must be explicit");
        return ok;
    }

    /// @brief 스트리밍 writer API(begin/append_stream/finalize)가 동작하는지 검사한다.
    bool test_stream_writer_api_() {
        using namespace parus::backend::parlib;
        namespace fs = std::filesystem;

        const fs::path out_path = fs::temp_directory_path() / "parus_parlib_stream_writer_test.parlib";
        std::error_code ec;
        fs::remove(out_path, ec);

        ParlibBuildOptions opt{};
        opt.output_path = out_path.string();
        opt.bundle_id = "stream_bundle";
        opt.target_triple = "aarch64-apple-darwin";
        opt.target_summary = "darwin-arm64";
        opt.include_pcore = true;
        opt.include_prt = false;
        opt.include_pstd = false;
        opt.include_debug = false;

        ParlibStreamWriter writer;
        bool ok = true;
        ok &= require_(writer.begin(opt), "ParlibStreamWriter::begin must succeed");
        if (!ok) return false;

        ParlibChunkPayload c1{};
        c1.kind = ParlibChunkKind::kManifest;
        c1.lane = ParlibLane::kGlobal;
        c1.alignment = 16;
        const std::string m1 = "format=1.1\n";
        std::istringstream in1(m1);
        ok &= require_(writer.append_chunk_stream(c1, in1), "append_chunk_stream must succeed");

        ParlibChunkPayload c2{};
        c2.kind = ParlibChunkKind::kExportCIndex;
        c2.lane = ParlibLane::kGlobal;
        c2.alignment = 8;
        const std::string m2 = "p_add\t(i32,i32)->i32\tpcore\tObjectArchive\t0\t1\n";
        c2.bytes.assign(m2.begin(), m2.end());
        ok &= require_(writer.append_chunk(c2), "append_chunk must succeed");

        const auto built = writer.finalize();
        ok &= require_(built.ok, "ParlibStreamWriter::finalize must succeed");
        ok &= require_(built.header.toc_offset > built.header.chunk_stream_offset,
                       "stream writer output must place TOC after chunk stream");
        ok &= require_(fs::exists(out_path), "stream writer output file must exist");
        if (!ok) return false;

        auto reader_opt = ParlibReader::open(out_path.string());
        ok &= require_(reader_opt.has_value(), "reader open for stream writer output must succeed");
        if (!ok) return false;

        const auto rec = reader_opt->find_chunk(ParlibChunkKind::kExportCIndex, ParlibLane::kGlobal, 0);
        ok &= require_(rec.has_value(), "stream-writer chunk must be discoverable via find_chunk");
        if (!ok) return false;

        const auto bytes = reader_opt->read_chunk_slice(*rec, 0, rec->size);
        ok &= require_(!bytes.empty(), "stream-writer chunk must be readable");

        return ok;
    }

} // namespace

int main() {
    struct Case {
        const char* name;
        bool (*fn)();
    };

    const Case cases[] = {
        {"build_and_inspect_v1", test_build_and_inspect_v1_},
        {"legacy_format_rejected", test_legacy_format_rejected_},
        {"stream_writer_api", test_stream_writer_api_},
    };

    int failed = 0;
    for (const auto& tc : cases) {
        std::cout << "[TEST] " << tc.name << "\n";
        const bool ok = tc.fn();
        if (!ok) {
            ++failed;
            std::cout << "  -> FAIL\n";
        } else {
            std::cout << "  -> PASS\n";
        }
    }

    if (failed != 0) {
        std::cerr << "[parlib tests] FAILED: " << failed << " case(s)\n";
        return 1;
    }

    std::cout << "[parlib tests] OK\n";
    return 0;
}
