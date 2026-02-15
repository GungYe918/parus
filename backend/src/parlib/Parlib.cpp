// backend/src/parlib/Parlib.cpp
#include <parus/backend/parlib/Parlib.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace parus::backend::parlib {

    namespace {

        static constexpr std::array<uint8_t, 4> k_magic = {'P', 'R', 'L', 'B'};
        static constexpr uint16_t k_format_major_v1 = 1;
        static constexpr uint16_t k_format_minor_v1 = 0;
        static constexpr uint32_t k_header_size_v1 = 112;
        static constexpr uint32_t k_toc_entry_size_v1 = 48;
        static constexpr uint32_t k_target_triple_field_size = 48;

        /// @brief TOC 키(kind+lane)를 표현한다.
        struct ChunkKey {
            ParlibChunkKind kind = ParlibChunkKind::kManifest;
            ParlibLane lane = ParlibLane::kGlobal;

            bool operator==(const ChunkKey& other) const {
                return kind == other.kind && lane == other.lane;
            }
        };

        /// @brief ChunkKey 해시 함수.
        struct ChunkKeyHash {
            size_t operator()(const ChunkKey& k) const {
                return (static_cast<size_t>(k.kind) << 16u) ^ static_cast<size_t>(k.lane);
            }
        };

        /// @brief 빌드 중간 청크 정보.
        struct BuildChunk {
            ParlibChunkPayload payload{};
            ParlibChunkRecord record{};
        };

        /// @brief 중복 제거를 위해 공유되는 고유 payload 저장소.
        struct UniqueBlob {
            std::vector<uint8_t> bytes{};
            uint32_t alignment = 8;
            ParlibCompression compression = ParlibCompression::kNone;
            uint64_t content_hash = 0;
            uint64_t checksum = 0;
            uint64_t offset = 0;
        };

        /// @brief 2의 거듭제곱 정렬값인지 검사한다.
        bool is_power_of_two_(uint32_t x) {
            return x != 0 && (x & (x - 1u)) == 0;
        }

        /// @brief value를 align 기준으로 올림 정렬한다.
        uint64_t align_up_(uint64_t value, uint32_t align) {
            if (align <= 1) return value;
            const uint64_t mask = static_cast<uint64_t>(align) - 1u;
            return (value + mask) & ~mask;
        }

        /// @brief FNV-1a 64 해시를 계산한다.
        uint64_t fnv1a64_(const std::vector<uint8_t>& bytes, uint64_t seed) {
            uint64_t h = seed;
            for (uint8_t b : bytes) {
                h ^= static_cast<uint64_t>(b);
                h *= 1099511628211ull;
            }
            return h;
        }

        /// @brief little-endian u16 쓰기.
        void write_u16_le_(std::vector<uint8_t>& out, size_t off, uint16_t v) {
            out[off + 0] = static_cast<uint8_t>(v & 0xFFu);
            out[off + 1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
        }

        /// @brief little-endian u32 쓰기.
        void write_u32_le_(std::vector<uint8_t>& out, size_t off, uint32_t v) {
            out[off + 0] = static_cast<uint8_t>(v & 0xFFu);
            out[off + 1] = static_cast<uint8_t>((v >> 8u) & 0xFFu);
            out[off + 2] = static_cast<uint8_t>((v >> 16u) & 0xFFu);
            out[off + 3] = static_cast<uint8_t>((v >> 24u) & 0xFFu);
        }

        /// @brief little-endian u64 쓰기.
        void write_u64_le_(std::vector<uint8_t>& out, size_t off, uint64_t v) {
            for (uint32_t i = 0; i < 8; ++i) {
                out[off + i] = static_cast<uint8_t>((v >> (8u * i)) & 0xFFu);
            }
        }

        /// @brief little-endian u16 읽기.
        bool read_u16_le_(const std::vector<uint8_t>& in, size_t off, uint16_t& out) {
            if (off + 2 > in.size()) return false;
            out = static_cast<uint16_t>(in[off + 0]) |
                  static_cast<uint16_t>(in[off + 1] << 8u);
            return true;
        }

        /// @brief little-endian u32 읽기.
        bool read_u32_le_(const std::vector<uint8_t>& in, size_t off, uint32_t& out) {
            if (off + 4 > in.size()) return false;
            out = static_cast<uint32_t>(in[off + 0]) |
                  (static_cast<uint32_t>(in[off + 1]) << 8u) |
                  (static_cast<uint32_t>(in[off + 2]) << 16u) |
                  (static_cast<uint32_t>(in[off + 3]) << 24u);
            return true;
        }

        /// @brief little-endian u64 읽기.
        bool read_u64_le_(const std::vector<uint8_t>& in, size_t off, uint64_t& out) {
            if (off + 8 > in.size()) return false;
            out = 0;
            for (uint32_t i = 0; i < 8; ++i) {
                out |= (static_cast<uint64_t>(in[off + i]) << (8u * i));
            }
            return true;
        }

        /// @brief UTF-8 텍스트를 0-종단 string table 형식으로 변환한다.
        std::vector<uint8_t> encode_cstr_table_(const std::vector<std::string>& strings) {
            std::vector<uint8_t> out;
            out.reserve(64);
            for (const auto& s : strings) {
                out.insert(out.end(), s.begin(), s.end());
                out.push_back('\0');
            }
            return out;
        }

        /// @brief 기본 Manifest 텍스트를 생성한다.
        std::vector<uint8_t> default_manifest_payload_(
            const ParlibBuildOptions& opt,
            const std::vector<ParlibLane>& lanes
        ) {
            std::ostringstream oss;
            oss << "format=1.0\n";
            oss << "magic=PRLB\n";
            oss << "feature_bits=" << opt.feature_bits << "\n";
            oss << "flags=" << opt.flags << "\n";
            oss << "target_triple=" << opt.target_triple << "\n";
            oss << "lanes=";
            for (size_t i = 0; i < lanes.size(); ++i) {
                if (i) oss << ",";
                oss << lane_name(lanes[i]);
            }
            oss << "\n";
            const std::string txt = oss.str();
            return std::vector<uint8_t>(txt.begin(), txt.end());
        }

        /// @brief 기본 StringTable payload를 생성한다.
        std::vector<uint8_t> default_string_table_payload_(
            const ParlibBuildOptions& opt,
            const std::vector<ParlibLane>& lanes
        ) {
            std::vector<std::string> strings;
            strings.reserve(16);
            strings.push_back("");
            strings.push_back("pcore");
            strings.push_back("prt");
            strings.push_back("pstd");
            strings.push_back("Manifest");
            strings.push_back("StringTable");
            strings.push_back("SymbolIndex");
            strings.push_back("TypeMeta");
            strings.push_back("OIRArchive");
            strings.push_back("ObjectArchive");
            strings.push_back("Debug");
            if (!opt.target_triple.empty()) {
                strings.push_back(opt.target_triple);
            }
            for (ParlibLane lane : lanes) {
                strings.push_back(lane_name(lane));
            }
            return encode_cstr_table_(strings);
        }

        /// @brief 기본 lane 청크 payload를 생성한다.
        std::vector<uint8_t> default_lane_payload_(ParlibChunkKind kind, ParlibLane lane) {
            std::ostringstream oss;
            oss << "lane=" << lane_name(lane) << "\n";
            oss << "kind=" << chunk_kind_name(kind) << "\n";
            const std::string txt = oss.str();
            return std::vector<uint8_t>(txt.begin(), txt.end());
        }

        /// @brief 메시지 헬퍼(에러).
        void push_error_(std::vector<CompileMessage>& msgs, std::string text) {
            msgs.push_back(CompileMessage{true, std::move(text)});
        }

        /// @brief 메시지 헬퍼(정보).
        void push_info_(std::vector<CompileMessage>& msgs, std::string text) {
            msgs.push_back(CompileMessage{false, std::move(text)});
        }

        /// @brief lane 선택 옵션에서 실제 lane 목록을 구성한다.
        std::vector<ParlibLane> collect_enabled_lanes_(const ParlibBuildOptions& opt) {
            std::vector<ParlibLane> lanes;
            if (opt.include_pcore) lanes.push_back(ParlibLane::kPcore);
            if (opt.include_prt) lanes.push_back(ParlibLane::kPrt);
            if (opt.include_pstd) lanes.push_back(ParlibLane::kPstd);
            return lanes;
        }

        /// @brief 기본 필수 청크 집합을 생성한다.
        std::unordered_map<ChunkKey, ParlibChunkPayload, ChunkKeyHash> make_required_chunks_(
            const ParlibBuildOptions& opt,
            const std::vector<ParlibLane>& lanes
        ) {
            std::unordered_map<ChunkKey, ParlibChunkPayload, ChunkKeyHash> out;

            ParlibChunkPayload manifest{};
            manifest.kind = ParlibChunkKind::kManifest;
            manifest.lane = ParlibLane::kGlobal;
            manifest.alignment = 8;
            manifest.compression = ParlibCompression::kNone;
            manifest.bytes = default_manifest_payload_(opt, lanes);
            out[ChunkKey{manifest.kind, manifest.lane}] = std::move(manifest);

            ParlibChunkPayload strings{};
            strings.kind = ParlibChunkKind::kStringTable;
            strings.lane = ParlibLane::kGlobal;
            strings.alignment = 8;
            strings.compression = ParlibCompression::kNone;
            strings.bytes = default_string_table_payload_(opt, lanes);
            out[ChunkKey{strings.kind, strings.lane}] = std::move(strings);

            for (ParlibLane lane : lanes) {
                for (ParlibChunkKind kind : {
                         ParlibChunkKind::kSymbolIndex,
                         ParlibChunkKind::kTypeMeta,
                         ParlibChunkKind::kOirArchive,
                         ParlibChunkKind::kObjectArchive
                     }) {
                    ParlibChunkPayload c{};
                    c.kind = kind;
                    c.lane = lane;
                    c.alignment = 8;
                    c.compression = ParlibCompression::kNone;
                    c.bytes = default_lane_payload_(kind, lane);
                    out[ChunkKey{c.kind, c.lane}] = std::move(c);
                }
            }

            if (opt.include_debug) {
                ParlibChunkPayload debug{};
                debug.kind = ParlibChunkKind::kDebug;
                debug.lane = ParlibLane::kGlobal;
                debug.alignment = 8;
                debug.compression = ParlibCompression::kNone;
                const std::string msg = "debug=enabled\n";
                debug.bytes.assign(msg.begin(), msg.end());
                out[ChunkKey{debug.kind, debug.lane}] = std::move(debug);
            }

            return out;
        }

        /// @brief TOC 순서를 안정화하기 위해 청크를 lane/kind 오름차순으로 정렬한다.
        std::vector<BuildChunk> to_sorted_chunks_(
            const std::unordered_map<ChunkKey, ParlibChunkPayload, ChunkKeyHash>& chunk_map
        ) {
            std::vector<BuildChunk> out;
            out.reserve(chunk_map.size());
            for (const auto& kv : chunk_map) {
                BuildChunk c{};
                c.payload = kv.second;
                out.push_back(std::move(c));
            }
            std::sort(out.begin(), out.end(), [](const BuildChunk& a, const BuildChunk& b) {
                const auto la = static_cast<uint16_t>(a.payload.lane);
                const auto lb = static_cast<uint16_t>(b.payload.lane);
                if (la != lb) return la < lb;
                return static_cast<uint16_t>(a.payload.kind) < static_cast<uint16_t>(b.payload.kind);
            });
            return out;
        }

        /// @brief 빌드 청크의 해시/체크섬/정렬값을 초기화한다.
        bool prepare_chunk_records_(
            std::vector<BuildChunk>& chunks,
            std::vector<CompileMessage>& msgs
        ) {
            bool ok = true;
            for (auto& c : chunks) {
                if (!is_power_of_two_(c.payload.alignment)) {
                    ok = false;
                    push_error_(msgs,
                        "parlib: chunk alignment must be power-of-two. kind=" +
                        chunk_kind_name(c.payload.kind) + ", lane=" + lane_name(c.payload.lane));
                    continue;
                }
                if (c.payload.compression != ParlibCompression::kNone) {
                    ok = false;
                    push_error_(msgs,
                        "parlib: unsupported compression for v1. kind=" +
                        chunk_kind_name(c.payload.kind) + ", lane=" + lane_name(c.payload.lane));
                    continue;
                }

                c.record.kind = c.payload.kind;
                c.record.lane = c.payload.lane;
                c.record.alignment = c.payload.alignment;
                c.record.compression = c.payload.compression;
                c.record.size = static_cast<uint64_t>(c.payload.bytes.size());
                c.record.content_hash = fnv1a64_(c.payload.bytes, 1469598103934665603ull);
                c.record.checksum = fnv1a64_(c.payload.bytes, 1099511628211ull) ^
                                    static_cast<uint64_t>(c.payload.bytes.size());
            }
            return ok;
        }

        /// @brief payload dedup을 수행하고 고유 blob 목록을 만든다.
        std::vector<UniqueBlob> dedup_payloads_(
            std::vector<BuildChunk>& chunks
        ) {
            std::vector<UniqueBlob> unique;
            std::unordered_map<uint64_t, std::vector<size_t>> by_hash;

            for (auto& c : chunks) {
                const uint64_t h = c.record.content_hash;
                size_t chosen = static_cast<size_t>(-1);
                auto it = by_hash.find(h);
                if (it != by_hash.end()) {
                    for (size_t idx : it->second) {
                        const auto& u = unique[idx];
                        if (u.alignment != c.payload.alignment) continue;
                        if (u.compression != c.payload.compression) continue;
                        if (u.bytes != c.payload.bytes) continue;
                        chosen = idx;
                        break;
                    }
                }

                if (chosen == static_cast<size_t>(-1)) {
                    UniqueBlob u{};
                    u.bytes = c.payload.bytes;
                    u.alignment = c.payload.alignment;
                    u.compression = c.payload.compression;
                    u.content_hash = c.record.content_hash;
                    u.checksum = c.record.checksum;
                    chosen = unique.size();
                    unique.push_back(std::move(u));
                    by_hash[h].push_back(chosen);
                    c.record.deduplicated = false;
                } else {
                    c.record.deduplicated = true;
                }

                // temporary: offset 필드에 unique index를 보관(후속 단계에서 실제 offset으로 치환)
                c.record.offset = static_cast<uint64_t>(chosen);
            }
            return unique;
        }

        /// @brief header 필드를 image에 직렬화한다.
        void write_header_(
            std::vector<uint8_t>& image,
            const ParlibHeaderInfo& hdr
        ) {
            image[0] = k_magic[0];
            image[1] = k_magic[1];
            image[2] = k_magic[2];
            image[3] = k_magic[3];
            write_u16_le_(image, 4, hdr.format_major);
            write_u16_le_(image, 6, hdr.format_minor);
            write_u32_le_(image, 8, hdr.flags);
            write_u32_le_(image, 12, k_header_size_v1);
            write_u64_le_(image, 16, hdr.toc_offset);
            write_u32_le_(image, 24, hdr.toc_entry_size);
            write_u32_le_(image, 28, hdr.toc_entry_count);
            write_u64_le_(image, 32, hdr.chunk_data_offset);
            write_u64_le_(image, 40, hdr.file_size);
            write_u64_le_(image, 48, hdr.feature_bits);

            const size_t triple_off = 56;
            const size_t max_copy = k_target_triple_field_size - 1u;
            const size_t n = std::min(max_copy, hdr.target_triple.size());
            std::memcpy(image.data() + triple_off, hdr.target_triple.data(), n);
            image[triple_off + n] = '\0';
        }

        /// @brief TOC 엔트리 1개를 image에 직렬화한다.
        void write_toc_entry_(
            std::vector<uint8_t>& image,
            size_t off,
            const ParlibChunkRecord& r
        ) {
            write_u16_le_(image, off + 0, static_cast<uint16_t>(r.kind));
            write_u16_le_(image, off + 2, static_cast<uint16_t>(r.lane));
            write_u32_le_(image, off + 4, r.alignment);
            write_u16_le_(image, off + 8, static_cast<uint16_t>(r.compression));
            write_u16_le_(image, off + 10, 0);
            write_u64_le_(image, off + 12, r.offset);
            write_u64_le_(image, off + 20, r.size);
            write_u64_le_(image, off + 28, r.checksum);
            write_u64_le_(image, off + 36, r.content_hash);
            write_u32_le_(image, off + 44, 0);
        }

        /// @brief 파일 전체를 메모리로 읽는다.
        bool read_file_bytes_(const std::string& path, std::vector<uint8_t>& out) {
            std::ifstream ifs(path, std::ios::binary);
            if (!ifs.is_open()) return false;
            ifs.seekg(0, std::ios::end);
            const std::streampos end = ifs.tellg();
            if (end < 0) return false;
            ifs.seekg(0, std::ios::beg);
            out.resize(static_cast<size_t>(end));
            if (!out.empty()) {
                ifs.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
            }
            return ifs.good() || ifs.eof();
        }

    } // namespace

    /// @brief lane 식별자를 문자열로 변환한다.
    std::string lane_name(ParlibLane lane) {
        switch (lane) {
            case ParlibLane::kGlobal: return "global";
            case ParlibLane::kPcore: return "pcore";
            case ParlibLane::kPrt: return "prt";
            case ParlibLane::kPstd: return "pstd";
            case ParlibLane::kVendorBegin: return "vendor";
        }
        return "unknown";
    }

    /// @brief 청크 종류를 문자열로 변환한다.
    std::string chunk_kind_name(ParlibChunkKind kind) {
        switch (kind) {
            case ParlibChunkKind::kManifest: return "Manifest";
            case ParlibChunkKind::kStringTable: return "StringTable";
            case ParlibChunkKind::kSymbolIndex: return "SymbolIndex";
            case ParlibChunkKind::kTypeMeta: return "TypeMeta";
            case ParlibChunkKind::kOirArchive: return "OIRArchive";
            case ParlibChunkKind::kObjectArchive: return "ObjectArchive";
            case ParlibChunkKind::kDebug: return "Debug";
            case ParlibChunkKind::kReserved: return "Reserved";
        }
        return "Unknown";
    }

    /// @brief 압축 방식을 문자열로 변환한다.
    std::string compression_name(ParlibCompression c) {
        switch (c) {
            case ParlibCompression::kNone: return "none";
        }
        return "unknown";
    }

    /// @brief v1 parlib 파일을 생성한다.
    ParlibBuildResult build_parlib(const ParlibBuildOptions& opt) {
        ParlibBuildResult out{};
        out.output_path = opt.output_path;

        if (opt.output_path.empty()) {
            push_error_(out.messages, "parlib: output path is empty.");
            return out;
        }

        const std::vector<ParlibLane> lanes = collect_enabled_lanes_(opt);
        if (lanes.empty()) {
            push_error_(out.messages, "parlib: at least one lane(pcore/prt/pstd) must be enabled.");
            return out;
        }

        auto chunk_map = make_required_chunks_(opt, lanes);
        for (const auto& c : opt.extra_chunks) {
            chunk_map[ChunkKey{c.kind, c.lane}] = c;
        }

        auto chunks = to_sorted_chunks_(chunk_map);
        if (!prepare_chunk_records_(chunks, out.messages)) {
            out.ok = false;
            return out;
        }

        auto unique = dedup_payloads_(chunks);

        ParlibHeaderInfo hdr{};
        hdr.format_major = k_format_major_v1;
        hdr.format_minor = k_format_minor_v1;
        hdr.flags = opt.flags;
        hdr.feature_bits = opt.feature_bits;
        hdr.target_triple = opt.target_triple;
        if (hdr.target_triple.size() >= k_target_triple_field_size) {
            push_info_(out.messages, "parlib: target triple was truncated to fit header fixed field.");
            hdr.target_triple.resize(k_target_triple_field_size - 1u);
        }

        hdr.toc_offset = k_header_size_v1;
        hdr.toc_entry_size = k_toc_entry_size_v1;
        hdr.toc_entry_count = static_cast<uint32_t>(chunks.size());
        const uint64_t toc_bytes = static_cast<uint64_t>(hdr.toc_entry_size) * hdr.toc_entry_count;
        hdr.chunk_data_offset = align_up_(hdr.toc_offset + toc_bytes, 8);

        uint64_t cursor = hdr.chunk_data_offset;
        for (auto& u : unique) {
            cursor = align_up_(cursor, u.alignment);
            u.offset = cursor;
            cursor += static_cast<uint64_t>(u.bytes.size());
        }
        hdr.file_size = cursor;

        for (auto& c : chunks) {
            const size_t uid = static_cast<size_t>(c.record.offset);
            c.record.offset = unique[uid].offset;
            c.record.size = static_cast<uint64_t>(c.payload.bytes.size());
        }

        std::vector<uint8_t> image(static_cast<size_t>(hdr.file_size), 0);
        write_header_(image, hdr);

        for (size_t i = 0; i < chunks.size(); ++i) {
            const size_t off = static_cast<size_t>(hdr.toc_offset + i * hdr.toc_entry_size);
            write_toc_entry_(image, off, chunks[i].record);
        }

        for (const auto& u : unique) {
            if (!u.bytes.empty()) {
                std::memcpy(image.data() + u.offset, u.bytes.data(), u.bytes.size());
            }
        }

        namespace fs = std::filesystem;
        std::error_code ec;
        const fs::path out_path(opt.output_path);
        const fs::path parent = out_path.parent_path();
        if (!parent.empty()) {
            fs::create_directories(parent, ec);
            if (ec) {
                push_error_(out.messages, "parlib: failed to create output directory: " + parent.string());
                return out;
            }
        }

        std::ofstream ofs(opt.output_path, std::ios::binary | std::ios::trunc);
        if (!ofs.is_open()) {
            push_error_(out.messages, "parlib: failed to open output file: " + opt.output_path);
            return out;
        }
        if (!image.empty()) {
            ofs.write(reinterpret_cast<const char*>(image.data()), static_cast<std::streamsize>(image.size()));
        }
        if (!ofs.good()) {
            push_error_(out.messages, "parlib: failed to write output file: " + opt.output_path);
            return out;
        }

        out.ok = true;
        out.file_size = hdr.file_size;
        out.header = hdr;
        out.chunks.reserve(chunks.size());
        for (const auto& c : chunks) {
            out.chunks.push_back(c.record);
        }
        push_info_(out.messages, "parlib: wrote " + std::to_string(out.file_size) +
                                 " bytes to " + opt.output_path);
        return out;
    }

    /// @brief 기존 스켈레톤 API 이름을 유지하면서 실제 구현으로 연결한다.
    ParlibBuildResult build_parlib_skeleton(const ParlibBuildOptions& opt) {
        return build_parlib(opt);
    }

    /// @brief parlib 파일 메타데이터를 읽고 TOC/체크섬을 검사한다.
    ParlibInspectResult inspect_parlib(const std::string& input_path) {
        ParlibInspectResult out{};
        out.input_path = input_path;
        if (input_path.empty()) {
            push_error_(out.messages, "parlib inspect: input path is empty.");
            return out;
        }

        std::vector<uint8_t> bytes;
        if (!read_file_bytes_(input_path, bytes)) {
            push_error_(out.messages, "parlib inspect: failed to read input file: " + input_path);
            return out;
        }
        if (bytes.size() < k_header_size_v1) {
            push_error_(out.messages, "parlib inspect: file is too small for v1 header.");
            return out;
        }

        if (!(bytes[0] == k_magic[0] && bytes[1] == k_magic[1] &&
              bytes[2] == k_magic[2] && bytes[3] == k_magic[3])) {
            push_error_(out.messages, "parlib inspect: invalid magic (expected PRLB).");
            return out;
        }

        ParlibHeaderInfo hdr{};
        if (!read_u16_le_(bytes, 4, hdr.format_major) ||
            !read_u16_le_(bytes, 6, hdr.format_minor) ||
            !read_u32_le_(bytes, 8, hdr.flags) ||
            !read_u64_le_(bytes, 16, hdr.toc_offset) ||
            !read_u32_le_(bytes, 24, hdr.toc_entry_size) ||
            !read_u32_le_(bytes, 28, hdr.toc_entry_count) ||
            !read_u64_le_(bytes, 32, hdr.chunk_data_offset) ||
            !read_u64_le_(bytes, 40, hdr.file_size) ||
            !read_u64_le_(bytes, 48, hdr.feature_bits)) {
            push_error_(out.messages, "parlib inspect: failed to parse header fields.");
            return out;
        }

        std::string triple;
        triple.reserve(k_target_triple_field_size);
        for (size_t i = 0; i < k_target_triple_field_size; ++i) {
            const char c = static_cast<char>(bytes[56 + i]);
            if (c == '\0') break;
            triple.push_back(c);
        }
        hdr.target_triple = std::move(triple);

        out.header = hdr;
        bool ok = true;

        if (hdr.toc_entry_size != k_toc_entry_size_v1) {
            ok = false;
            push_error_(out.messages, "parlib inspect: unsupported TOC entry size.");
        }

        const uint64_t toc_end = hdr.toc_offset +
                                 static_cast<uint64_t>(hdr.toc_entry_size) * hdr.toc_entry_count;
        if (toc_end > bytes.size()) {
            ok = false;
            push_error_(out.messages, "parlib inspect: TOC range exceeds file size.");
        }

        if (hdr.file_size != bytes.size()) {
            ok = false;
            push_error_(out.messages, "parlib inspect: header file_size does not match actual file size.");
        }

        out.chunks.clear();
        out.chunks.reserve(hdr.toc_entry_count);

        if (ok) {
            for (uint32_t i = 0; i < hdr.toc_entry_count; ++i) {
                const size_t off = static_cast<size_t>(hdr.toc_offset + static_cast<uint64_t>(i) * hdr.toc_entry_size);
                uint16_t kind_raw = 0;
                uint16_t lane_raw = 0;
                uint32_t alignment = 0;
                uint16_t comp_raw = 0;
                uint64_t data_off = 0;
                uint64_t data_size = 0;
                uint64_t checksum = 0;
                uint64_t content_hash = 0;

                if (!read_u16_le_(bytes, off + 0, kind_raw) ||
                    !read_u16_le_(bytes, off + 2, lane_raw) ||
                    !read_u32_le_(bytes, off + 4, alignment) ||
                    !read_u16_le_(bytes, off + 8, comp_raw) ||
                    !read_u64_le_(bytes, off + 12, data_off) ||
                    !read_u64_le_(bytes, off + 20, data_size) ||
                    !read_u64_le_(bytes, off + 28, checksum) ||
                    !read_u64_le_(bytes, off + 36, content_hash)) {
                    ok = false;
                    push_error_(out.messages, "parlib inspect: failed to parse TOC entry #" + std::to_string(i));
                    continue;
                }

                ParlibChunkRecord rec{};
                rec.kind = static_cast<ParlibChunkKind>(kind_raw);
                rec.lane = static_cast<ParlibLane>(lane_raw);
                rec.alignment = alignment;
                rec.compression = static_cast<ParlibCompression>(comp_raw);
                rec.offset = data_off;
                rec.size = data_size;
                rec.checksum = checksum;
                rec.content_hash = content_hash;

                if (data_off + data_size > bytes.size()) {
                    ok = false;
                    push_error_(out.messages, "parlib inspect: chunk range out of file bounds, entry #" + std::to_string(i));
                    out.chunks.push_back(rec);
                    continue;
                }

                std::vector<uint8_t> payload;
                payload.resize(static_cast<size_t>(data_size));
                if (data_size > 0) {
                    std::memcpy(payload.data(), bytes.data() + data_off, static_cast<size_t>(data_size));
                }
                const uint64_t hash_now = fnv1a64_(payload, 1469598103934665603ull);
                const uint64_t checksum_now = fnv1a64_(payload, 1099511628211ull) ^ data_size;
                if (hash_now != content_hash || checksum_now != checksum) {
                    ok = false;
                    push_error_(out.messages,
                        "parlib inspect: checksum/hash mismatch at entry #" + std::to_string(i) +
                        " (" + chunk_kind_name(rec.kind) + ":" + lane_name(rec.lane) + ")");
                }

                out.chunks.push_back(rec);
            }
        }

        out.ok = ok;
        if (ok) {
            push_info_(out.messages, "parlib inspect: file is valid (" + std::to_string(out.chunks.size()) + " chunks).");
        }
        return out;
    }

} // namespace parus::backend::parlib
