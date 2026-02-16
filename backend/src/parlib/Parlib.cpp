// backend/src/parlib/Parlib.cpp
#include <parus/backend/parlib/Parlib.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <memory>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

namespace parus::backend::parlib {

    namespace {

        static constexpr std::array<uint8_t, 4> k_magic = {'P', 'R', 'L', 'B'};
        static constexpr std::array<uint8_t, 4> k_footer_magic = {'P', 'F', 'T', '1'};

        static constexpr uint16_t k_format_major_v1 = 1;
        static constexpr uint16_t k_format_minor_v1 = 1;

        static constexpr uint32_t k_header_size_v1 = 256;
        static constexpr uint32_t k_toc_entry_size_v1 = 64;
        static constexpr uint32_t k_footer_size_v1 = 32;

        static constexpr uint32_t k_target_triple_field_size = 32;
        static constexpr uint32_t k_bundle_id_field_size = 32;
        static constexpr uint32_t k_target_summary_field_size = 32;

        static constexpr uint64_t k_hash_seed_content = 1469598103934665603ull;
        static constexpr uint64_t k_hash_seed_checksum = 1099511628211ull;

        /// @brief TOC 키(kind+lane+target)를 표현한다.
        struct ChunkKey {
            ParlibChunkKind kind = ParlibChunkKind::kManifest;
            ParlibLane lane = ParlibLane::kGlobal;
            uint32_t target_id = 0;

            bool operator==(const ChunkKey& other) const {
                return kind == other.kind && lane == other.lane && target_id == other.target_id;
            }
        };

        /// @brief ChunkKey 해시 함수.
        struct ChunkKeyHash {
            size_t operator()(const ChunkKey& k) const {
                return (static_cast<size_t>(k.kind) << 32u) ^
                       (static_cast<size_t>(k.lane) << 16u) ^
                       static_cast<size_t>(k.target_id);
            }
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

        /// @brief FNV-1a 64 해시를 chunk 단위로 업데이트한다.
        uint64_t fnv1a64_update_(uint64_t h, const uint8_t* p, size_t n) {
            for (size_t i = 0; i < n; ++i) {
                h ^= static_cast<uint64_t>(p[i]);
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

        /// @brief 문자열 필드를 고정 길이로 직렬화한다.
        void write_cstr_field_(std::vector<uint8_t>& out, size_t off, size_t field_size, const std::string& s) {
            const size_t max_copy = (field_size == 0) ? 0 : (field_size - 1u);
            const size_t n = std::min(max_copy, s.size());
            if (n != 0) {
                std::memcpy(out.data() + off, s.data(), n);
            }
            out[off + n] = '\0';
        }

        /// @brief 고정 길이 C 문자열 필드를 역직렬화한다.
        std::string read_cstr_field_(const std::vector<uint8_t>& in, size_t off, size_t field_size) {
            std::string out;
            out.reserve(field_size);
            for (size_t i = 0; i < field_size; ++i) {
                const char c = static_cast<char>(in[off + i]);
                if (c == '\0') break;
                out.push_back(c);
            }
            return out;
        }

        /// @brief 텍스트 payload line용 unsafe 문자(tab/newline)를 공백으로 정규화한다.
        std::string sanitize_line_field_(std::string_view s) {
            std::string out;
            out.reserve(s.size());
            for (char c : s) {
                if (c == '\t' || c == '\n' || c == '\r') out.push_back(' ');
                else out.push_back(c);
            }
            return out;
        }

        /// @brief 문자열을 unsigned 정수로 파싱한다. 실패 시 0.
        uint64_t parse_u64_or_zero_(std::string_view s) {
            if (s.empty()) return 0;
            try {
                return static_cast<uint64_t>(std::stoull(std::string(s)));
            } catch (...) {
                return 0;
            }
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
            oss << "format=1.1\n";
            oss << "magic=PRLB\n";
            oss << "bundle_id=" << opt.bundle_id << "\n";
            oss << "target_triple=" << opt.target_triple << "\n";
            oss << "target_summary=" << opt.target_summary << "\n";
            oss << "feature_bits=" << opt.feature_bits << "\n";
            oss << "compiler_hash=" << opt.compiler_hash << "\n";
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
            strings.reserve(24);
            strings.push_back("");
            strings.push_back("global");
            strings.push_back("pcore");
            strings.push_back("prt");
            strings.push_back("pstd");
            strings.push_back("Manifest");
            strings.push_back("StringTable");
            strings.push_back("ExportCIndex");
            strings.push_back("NativeDeps");
            strings.push_back("SymbolIndex");
            strings.push_back("TypeMeta");
            strings.push_back("OIRArchive");
            strings.push_back("ObjectArchive");
            strings.push_back("Debug");
            strings.push_back("SourceMap");
            strings.push_back("NativeArchivePayload");
            if (!opt.bundle_id.empty()) strings.push_back(opt.bundle_id);
            if (!opt.target_triple.empty()) strings.push_back(opt.target_triple);
            if (!opt.target_summary.empty()) strings.push_back(opt.target_summary);
            for (ParlibLane lane : lanes) strings.push_back(lane_name(lane));
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

        /// @brief ExportCIndex payload를 텍스트로 인코딩한다.
        std::vector<uint8_t> encode_export_c_index_(const std::vector<ParlibExportCEntry>& entries) {
            std::ostringstream oss;
            for (const auto& e : entries) {
                oss << sanitize_line_field_(e.symbol) << "\t"
                    << sanitize_line_field_(e.signature) << "\t"
                    << lane_name(e.lane) << "\t"
                    << chunk_kind_name(e.chunk_kind) << "\t"
                    << e.target_id << "\t"
                    << (e.visible ? 1 : 0) << "\n";
            }
            const std::string txt = oss.str();
            return std::vector<uint8_t>(txt.begin(), txt.end());
        }

        /// @brief NativeDeps payload를 텍스트로 인코딩한다.
        std::vector<uint8_t> encode_native_deps_(const std::vector<ParlibNativeDepEntry>& entries) {
            std::ostringstream oss;
            for (const auto& e : entries) {
                oss << sanitize_line_field_(e.name) << "\t"
                    << native_dep_kind_name(e.kind) << "\t"
                    << native_dep_mode_name(e.mode) << "\t"
                    << sanitize_line_field_(e.target_filter) << "\t"
                    << e.link_order << "\t"
                    << (e.required ? 1 : 0) << "\t"
                    << e.hash << "\t"
                    << sanitize_line_field_(e.reference) << "\n";
            }
            const std::string txt = oss.str();
            return std::vector<uint8_t>(txt.begin(), txt.end());
        }

        /// @brief 문자열을 구분자로 분할한다.
        std::vector<std::string_view> split_view_(std::string_view s, char sep) {
            std::vector<std::string_view> out;
            size_t pos = 0;
            while (pos <= s.size()) {
                const size_t next = s.find(sep, pos);
                if (next == std::string_view::npos) {
                    out.push_back(s.substr(pos));
                    break;
                }
                out.push_back(s.substr(pos, next - pos));
                pos = next + 1;
            }
            return out;
        }

        std::optional<ParlibLane> parse_lane_name_(std::string_view s) {
            if (s == "global") return ParlibLane::kGlobal;
            if (s == "pcore") return ParlibLane::kPcore;
            if (s == "prt") return ParlibLane::kPrt;
            if (s == "pstd") return ParlibLane::kPstd;
            if (s == "vendor") return ParlibLane::kVendorBegin;
            return std::nullopt;
        }

        std::optional<ParlibChunkKind> parse_chunk_kind_name_(std::string_view s) {
            if (s == "Manifest") return ParlibChunkKind::kManifest;
            if (s == "StringTable") return ParlibChunkKind::kStringTable;
            if (s == "ExportCIndex") return ParlibChunkKind::kExportCIndex;
            if (s == "NativeDeps") return ParlibChunkKind::kNativeDeps;
            if (s == "SymbolIndex") return ParlibChunkKind::kSymbolIndex;
            if (s == "TypeMeta") return ParlibChunkKind::kTypeMeta;
            if (s == "OIRArchive") return ParlibChunkKind::kOirArchive;
            if (s == "ObjectArchive") return ParlibChunkKind::kObjectArchive;
            if (s == "Debug") return ParlibChunkKind::kDebug;
            if (s == "SourceMap") return ParlibChunkKind::kSourceMap;
            if (s == "NativeArchivePayload") return ParlibChunkKind::kNativeArchivePayload;
            if (s == "Reserved") return ParlibChunkKind::kReserved;
            return std::nullopt;
        }

        std::optional<ParlibNativeDepKind> parse_native_dep_kind_name_(std::string_view s) {
            if (s == "static") return ParlibNativeDepKind::kStatic;
            if (s == "shared") return ParlibNativeDepKind::kShared;
            if (s == "framework") return ParlibNativeDepKind::kFramework;
            if (s == "system") return ParlibNativeDepKind::kSystem;
            return std::nullopt;
        }

        std::optional<ParlibNativeDepMode> parse_native_dep_mode_name_(std::string_view s) {
            if (s == "embed") return ParlibNativeDepMode::kEmbed;
            if (s == "reference") return ParlibNativeDepMode::kReference;
            return std::nullopt;
        }

        /// @brief ExportCIndex payload를 파싱한다.
        std::vector<ParlibExportCEntry> parse_export_c_index_(const std::vector<uint8_t>& bytes) {
            std::vector<ParlibExportCEntry> out;
            const std::string txt(bytes.begin(), bytes.end());
            std::istringstream iss(txt);
            std::string line;
            while (std::getline(iss, line)) {
                if (line.empty()) continue;
                const auto f = split_view_(line, '\t');
                if (f.size() < 6) continue;

                auto lane = parse_lane_name_(f[2]);
                auto kind = parse_chunk_kind_name_(f[3]);
                if (!lane.has_value() || !kind.has_value()) continue;

                ParlibExportCEntry e{};
                e.symbol = std::string(f[0]);
                e.signature = std::string(f[1]);
                e.lane = *lane;
                e.chunk_kind = *kind;
                e.target_id = static_cast<uint32_t>(parse_u64_or_zero_(f[4]));
                e.visible = (parse_u64_or_zero_(f[5]) != 0);
                out.push_back(std::move(e));
            }
            return out;
        }

        /// @brief NativeDeps payload를 파싱한다.
        std::vector<ParlibNativeDepEntry> parse_native_deps_(const std::vector<uint8_t>& bytes) {
            std::vector<ParlibNativeDepEntry> out;
            const std::string txt(bytes.begin(), bytes.end());
            std::istringstream iss(txt);
            std::string line;
            while (std::getline(iss, line)) {
                if (line.empty()) continue;
                const auto f = split_view_(line, '\t');
                if (f.size() < 8) continue;

                auto kind = parse_native_dep_kind_name_(f[1]);
                auto mode = parse_native_dep_mode_name_(f[2]);
                if (!kind.has_value() || !mode.has_value()) continue;

                ParlibNativeDepEntry e{};
                e.name = std::string(f[0]);
                e.kind = *kind;
                e.mode = *mode;
                e.target_filter = std::string(f[3]);
                e.link_order = static_cast<uint32_t>(parse_u64_or_zero_(f[4]));
                e.required = (parse_u64_or_zero_(f[5]) != 0);
                e.hash = parse_u64_or_zero_(f[6]);
                e.reference = std::string(f[7]);
                out.push_back(std::move(e));
            }
            return out;
        }

        /// @brief TOC 순서를 안정화하기 위해 청크를 lane/target/kind 오름차순으로 정렬한다.
        std::vector<ParlibChunkPayload> to_sorted_chunks_(
            const std::unordered_map<ChunkKey, ParlibChunkPayload, ChunkKeyHash>& chunk_map
        ) {
            std::vector<ParlibChunkPayload> out;
            out.reserve(chunk_map.size());
            for (const auto& kv : chunk_map) {
                out.push_back(kv.second);
            }
            std::sort(out.begin(), out.end(), [](const ParlibChunkPayload& a, const ParlibChunkPayload& b) {
                const auto la = static_cast<uint16_t>(a.lane);
                const auto lb = static_cast<uint16_t>(b.lane);
                if (la != lb) return la < lb;
                if (a.target_id != b.target_id) return a.target_id < b.target_id;
                return static_cast<uint16_t>(a.kind) < static_cast<uint16_t>(b.kind);
            });
            return out;
        }

        /// @brief Header를 직렬화한다.
        std::vector<uint8_t> serialize_header_(const ParlibHeaderInfo& h) {
            std::vector<uint8_t> out(k_header_size_v1, 0);
            out[0] = k_magic[0];
            out[1] = k_magic[1];
            out[2] = k_magic[2];
            out[3] = k_magic[3];
            write_u16_le_(out, 4, h.format_major);
            write_u16_le_(out, 6, h.format_minor);
            write_u32_le_(out, 8, h.flags);
            write_u32_le_(out, 12, h.header_size);
            write_u64_le_(out, 16, h.chunk_stream_offset);
            write_u64_le_(out, 24, h.chunk_stream_size);
            write_u64_le_(out, 32, h.toc_offset);
            write_u64_le_(out, 40, h.toc_size);
            write_u64_le_(out, 48, h.file_size);
            write_u64_le_(out, 56, h.feature_bits);
            write_u64_le_(out, 64, h.compiler_hash);
            write_u32_le_(out, 72, h.toc_entry_count);
            write_u32_le_(out, 76, h.toc_entry_size);
            write_cstr_field_(out, 80, k_target_triple_field_size, h.target_triple);
            write_cstr_field_(out, 112, k_bundle_id_field_size, h.bundle_id);
            write_cstr_field_(out, 144, k_target_summary_field_size, h.target_summary);
            return out;
        }

        /// @brief Header를 역직렬화한다.
        bool deserialize_header_(const std::vector<uint8_t>& in, ParlibHeaderInfo& h) {
            if (in.size() < k_header_size_v1) return false;
            if (!(in[0] == k_magic[0] && in[1] == k_magic[1] && in[2] == k_magic[2] && in[3] == k_magic[3])) {
                return false;
            }

            if (!read_u16_le_(in, 4, h.format_major) ||
                !read_u16_le_(in, 6, h.format_minor) ||
                !read_u32_le_(in, 8, h.flags) ||
                !read_u32_le_(in, 12, h.header_size) ||
                !read_u64_le_(in, 16, h.chunk_stream_offset) ||
                !read_u64_le_(in, 24, h.chunk_stream_size) ||
                !read_u64_le_(in, 32, h.toc_offset) ||
                !read_u64_le_(in, 40, h.toc_size) ||
                !read_u64_le_(in, 48, h.file_size) ||
                !read_u64_le_(in, 56, h.feature_bits) ||
                !read_u64_le_(in, 64, h.compiler_hash) ||
                !read_u32_le_(in, 72, h.toc_entry_count) ||
                !read_u32_le_(in, 76, h.toc_entry_size)) {
                return false;
            }

            h.target_triple = read_cstr_field_(in, 80, k_target_triple_field_size);
            h.bundle_id = read_cstr_field_(in, 112, k_bundle_id_field_size);
            h.target_summary = read_cstr_field_(in, 144, k_target_summary_field_size);
            return true;
        }

        /// @brief TOC entry 1개를 직렬화한다.
        std::vector<uint8_t> serialize_toc_entry_(const ParlibChunkRecord& r) {
            std::vector<uint8_t> out(k_toc_entry_size_v1, 0);
            write_u16_le_(out, 0, static_cast<uint16_t>(r.kind));
            write_u16_le_(out, 2, static_cast<uint16_t>(r.lane));
            write_u32_le_(out, 4, r.target_id);
            write_u32_le_(out, 8, r.alignment);
            write_u16_le_(out, 12, static_cast<uint16_t>(r.compression));
            write_u16_le_(out, 14, 0);
            write_u64_le_(out, 16, r.offset);
            write_u64_le_(out, 24, r.size);
            write_u64_le_(out, 32, r.checksum);
            write_u64_le_(out, 40, r.content_hash);
            write_u64_le_(out, 48, 0);
            write_u64_le_(out, 56, 0);
            return out;
        }

        /// @brief TOC entry를 역직렬화한다.
        bool deserialize_toc_entry_(const std::vector<uint8_t>& in, size_t off, ParlibChunkRecord& r) {
            uint16_t kind_raw = 0;
            uint16_t lane_raw = 0;
            uint16_t comp_raw = 0;
            if (!read_u16_le_(in, off + 0, kind_raw) ||
                !read_u16_le_(in, off + 2, lane_raw) ||
                !read_u32_le_(in, off + 4, r.target_id) ||
                !read_u32_le_(in, off + 8, r.alignment) ||
                !read_u16_le_(in, off + 12, comp_raw) ||
                !read_u64_le_(in, off + 16, r.offset) ||
                !read_u64_le_(in, off + 24, r.size) ||
                !read_u64_le_(in, off + 32, r.checksum) ||
                !read_u64_le_(in, off + 40, r.content_hash)) {
                return false;
            }
            r.kind = static_cast<ParlibChunkKind>(kind_raw);
            r.lane = static_cast<ParlibLane>(lane_raw);
            r.compression = static_cast<ParlibCompression>(comp_raw);
            r.deduplicated = false;
            return true;
        }

        /// @brief Footer를 직렬화한다.
        std::vector<uint8_t> serialize_footer_(uint64_t toc_offset, uint64_t toc_size) {
            std::vector<uint8_t> out(k_footer_size_v1, 0);
            out[0] = k_footer_magic[0];
            out[1] = k_footer_magic[1];
            out[2] = k_footer_magic[2];
            out[3] = k_footer_magic[3];
            write_u32_le_(out, 4, 1);
            write_u64_le_(out, 8, toc_offset);
            write_u64_le_(out, 16, toc_size);
            const uint64_t checksum = fnv1a64_update_(k_hash_seed_content, out.data(), 24);
            write_u64_le_(out, 24, checksum);
            return out;
        }

        /// @brief Footer를 검증/역직렬화한다.
        bool deserialize_footer_(const std::vector<uint8_t>& in, uint64_t& toc_offset, uint64_t& toc_size) {
            if (in.size() != k_footer_size_v1) return false;
            if (!(in[0] == k_footer_magic[0] && in[1] == k_footer_magic[1] &&
                  in[2] == k_footer_magic[2] && in[3] == k_footer_magic[3])) {
                return false;
            }
            uint32_t ver = 0;
            uint64_t checksum = 0;
            if (!read_u32_le_(in, 4, ver) ||
                !read_u64_le_(in, 8, toc_offset) ||
                !read_u64_le_(in, 16, toc_size) ||
                !read_u64_le_(in, 24, checksum)) {
                return false;
            }
            if (ver != 1) return false;
            const uint64_t now = fnv1a64_update_(k_hash_seed_content, in.data(), 24);
            return checksum == now;
        }

        /// @brief stream 현재 위치를 align으로 올려 패딩 바이트를 쓴다.
        bool align_output_stream_(std::ofstream& of, uint32_t align) {
            if (align <= 1) return true;
            const std::streampos pos = of.tellp();
            if (pos < 0) return false;
            const uint64_t p = static_cast<uint64_t>(pos);
            const uint64_t aligned = align_up_(p, align);
            if (aligned == p) return true;
            const uint64_t pad = aligned - p;
            std::vector<uint8_t> zeros(static_cast<size_t>(pad), 0);
            of.write(reinterpret_cast<const char*>(zeros.data()), static_cast<std::streamsize>(zeros.size()));
            return of.good();
        }

        /// @brief 오류 메시지가 하나라도 있으면 true.
        bool has_error_messages_(const std::vector<CompileMessage>& msgs) {
            for (const auto& m : msgs) {
                if (m.is_error) return true;
            }
            return false;
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
            case ParlibChunkKind::kExportCIndex: return "ExportCIndex";
            case ParlibChunkKind::kNativeDeps: return "NativeDeps";
            case ParlibChunkKind::kSymbolIndex: return "SymbolIndex";
            case ParlibChunkKind::kTypeMeta: return "TypeMeta";
            case ParlibChunkKind::kOirArchive: return "OIRArchive";
            case ParlibChunkKind::kObjectArchive: return "ObjectArchive";
            case ParlibChunkKind::kDebug: return "Debug";
            case ParlibChunkKind::kSourceMap: return "SourceMap";
            case ParlibChunkKind::kNativeArchivePayload: return "NativeArchivePayload";
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

    /// @brief native dep kind 이름을 문자열로 변환한다.
    std::string native_dep_kind_name(ParlibNativeDepKind k) {
        switch (k) {
            case ParlibNativeDepKind::kStatic: return "static";
            case ParlibNativeDepKind::kShared: return "shared";
            case ParlibNativeDepKind::kFramework: return "framework";
            case ParlibNativeDepKind::kSystem: return "system";
        }
        return "unknown";
    }

    /// @brief native dep mode 이름을 문자열로 변환한다.
    std::string native_dep_mode_name(ParlibNativeDepMode m) {
        switch (m) {
            case ParlibNativeDepMode::kEmbed: return "embed";
            case ParlibNativeDepMode::kReference: return "reference";
        }
        return "unknown";
    }

    /// @brief chunk 범위에서 최대 max_bytes 만큼 읽는다.
    bool ParlibChunkStream::read_some(std::vector<uint8_t>& out, size_t max_bytes) {
        out.clear();
        if (!ok() || remaining_ == 0 || max_bytes == 0) return false;

        const uint64_t n64 = std::min<uint64_t>(remaining_, static_cast<uint64_t>(max_bytes));
        if (n64 == 0 || n64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) return false;
        const size_t n = static_cast<size_t>(n64);

        out.resize(n);
        file_->read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(n));
        const std::streamsize got = file_->gcount();
        if (got <= 0) {
            out.clear();
            ok_ = false;
            return false;
        }

        if (static_cast<size_t>(got) < n) {
            out.resize(static_cast<size_t>(got));
            ok_ = false;
        }

        remaining_ -= static_cast<uint64_t>(out.size());
        return !out.empty();
    }

    /// @brief 랜덤 액세스 리더를 열고 Footer/TOC를 검증한다.
    std::optional<ParlibReader> ParlibReader::open(
        const std::string& input_path,
        std::vector<CompileMessage>* external_messages
    ) {
        ParlibReader out{};
        out.input_path_ = input_path;
        if (input_path.empty()) {
            push_error_(out.messages_, "parlib reader: input path is empty.");
            if (external_messages != nullptr) *external_messages = out.messages_;
            return std::nullopt;
        }

        std::ifstream ifs(input_path, std::ios::binary);
        if (!ifs.is_open()) {
            push_error_(out.messages_, "parlib reader: failed to open input file: " + input_path);
            if (external_messages != nullptr) *external_messages = out.messages_;
            return std::nullopt;
        }

        ifs.seekg(0, std::ios::end);
        const std::streampos end = ifs.tellg();
        if (end < 0) {
            push_error_(out.messages_, "parlib reader: failed to seek end: " + input_path);
            if (external_messages != nullptr) *external_messages = out.messages_;
            return std::nullopt;
        }
        const uint64_t file_size = static_cast<uint64_t>(end);
        if (file_size < (k_header_size_v1 + k_footer_size_v1)) {
            push_error_(out.messages_, "parlib reader: file is too small for v1 format.");
            if (external_messages != nullptr) *external_messages = out.messages_;
            return std::nullopt;
        }

        ifs.seekg(0, std::ios::beg);
        std::vector<uint8_t> header_bytes(k_header_size_v1, 0);
        ifs.read(reinterpret_cast<char*>(header_bytes.data()), static_cast<std::streamsize>(header_bytes.size()));
        if (ifs.gcount() != static_cast<std::streamsize>(header_bytes.size())) {
            push_error_(out.messages_, "parlib reader: failed to read full header.");
            if (external_messages != nullptr) *external_messages = out.messages_;
            return std::nullopt;
        }

        if (!deserialize_header_(header_bytes, out.header_)) {
            push_error_(out.messages_, "parlib reader: invalid header or magic.");
            if (external_messages != nullptr) *external_messages = out.messages_;
            return std::nullopt;
        }

        if (out.header_.format_major != k_format_major_v1 || out.header_.format_minor != k_format_minor_v1) {
            if (out.header_.format_major == 1 && out.header_.format_minor == 0) {
                push_error_(out.messages_, "parlib reader: legacy parlib format is not supported.");
            } else {
                push_error_(out.messages_, "parlib reader: unsupported parlib version.");
            }
            if (external_messages != nullptr) *external_messages = out.messages_;
            return std::nullopt;
        }

        if (out.header_.header_size != k_header_size_v1) {
            push_error_(out.messages_, "parlib reader: unsupported header size.");
            if (external_messages != nullptr) *external_messages = out.messages_;
            return std::nullopt;
        }

        if (out.header_.file_size != file_size) {
            push_error_(out.messages_, "parlib reader: header file_size mismatch.");
            if (external_messages != nullptr) *external_messages = out.messages_;
            return std::nullopt;
        }

        if (file_size < k_footer_size_v1) {
            push_error_(out.messages_, "parlib reader: missing footer.");
            if (external_messages != nullptr) *external_messages = out.messages_;
            return std::nullopt;
        }

        ifs.seekg(static_cast<std::streamoff>(file_size - k_footer_size_v1), std::ios::beg);
        std::vector<uint8_t> footer_bytes(k_footer_size_v1, 0);
        ifs.read(reinterpret_cast<char*>(footer_bytes.data()), static_cast<std::streamsize>(footer_bytes.size()));
        if (ifs.gcount() != static_cast<std::streamsize>(footer_bytes.size())) {
            push_error_(out.messages_, "parlib reader: failed to read footer.");
            if (external_messages != nullptr) *external_messages = out.messages_;
            return std::nullopt;
        }

        uint64_t footer_toc_offset = 0;
        uint64_t footer_toc_size = 0;
        if (!deserialize_footer_(footer_bytes, footer_toc_offset, footer_toc_size)) {
            push_error_(out.messages_, "parlib reader: invalid footer.");
            if (external_messages != nullptr) *external_messages = out.messages_;
            return std::nullopt;
        }

        if (footer_toc_offset != out.header_.toc_offset || footer_toc_size != out.header_.toc_size) {
            push_error_(out.messages_, "parlib reader: TOC pointer mismatch between header and footer.");
            if (external_messages != nullptr) *external_messages = out.messages_;
            return std::nullopt;
        }

        if (out.header_.toc_entry_size != k_toc_entry_size_v1) {
            push_error_(out.messages_, "parlib reader: unsupported TOC entry size.");
            if (external_messages != nullptr) *external_messages = out.messages_;
            return std::nullopt;
        }

        if (out.header_.toc_offset + out.header_.toc_size + k_footer_size_v1 != file_size) {
            push_error_(out.messages_, "parlib reader: file layout is not [Header][Chunk Stream][TOC][Footer].");
            if (external_messages != nullptr) *external_messages = out.messages_;
            return std::nullopt;
        }

        if (out.header_.toc_size != static_cast<uint64_t>(out.header_.toc_entry_size) * out.header_.toc_entry_count) {
            push_error_(out.messages_, "parlib reader: TOC size/count mismatch.");
            if (external_messages != nullptr) *external_messages = out.messages_;
            return std::nullopt;
        }

        ifs.seekg(static_cast<std::streamoff>(out.header_.toc_offset), std::ios::beg);
        std::vector<uint8_t> toc_bytes(static_cast<size_t>(out.header_.toc_size), 0);
        if (!toc_bytes.empty()) {
            ifs.read(reinterpret_cast<char*>(toc_bytes.data()), static_cast<std::streamsize>(toc_bytes.size()));
            if (ifs.gcount() != static_cast<std::streamsize>(toc_bytes.size())) {
                push_error_(out.messages_, "parlib reader: failed to read full TOC.");
                if (external_messages != nullptr) *external_messages = out.messages_;
                return std::nullopt;
            }
        }

        out.chunks_.clear();
        out.chunks_.reserve(out.header_.toc_entry_count);
        for (uint32_t i = 0; i < out.header_.toc_entry_count; ++i) {
            const size_t off = static_cast<size_t>(i) * out.header_.toc_entry_size;
            ParlibChunkRecord rec{};
            if (!deserialize_toc_entry_(toc_bytes, off, rec)) {
                push_error_(out.messages_, "parlib reader: failed to parse TOC entry #" + std::to_string(i));
                if (external_messages != nullptr) *external_messages = out.messages_;
                return std::nullopt;
            }

            if (rec.offset + rec.size > file_size) {
                push_error_(out.messages_, "parlib reader: chunk range out of file bounds at entry #" + std::to_string(i));
                if (external_messages != nullptr) *external_messages = out.messages_;
                return std::nullopt;
            }
            out.chunks_.push_back(rec);
        }

        out.header_.footer_offset = out.header_.toc_offset + out.header_.toc_size;
        out.ok_ = true;
        push_info_(out.messages_, "parlib reader: opened v1 file (" + std::to_string(out.chunks_.size()) + " chunks).");

        if (external_messages != nullptr) *external_messages = out.messages_;
        return out;
    }

    /// @brief kind/lane/target으로 chunk를 찾는다.
    std::optional<ParlibChunkRecord> ParlibReader::find_chunk(
        ParlibChunkKind kind,
        ParlibLane lane,
        uint32_t target_id
    ) const {
        for (const auto& c : chunks_) {
            if (c.kind == kind && c.lane == lane && c.target_id == target_id) {
                return c;
            }
        }
        return std::nullopt;
    }

    /// @brief chunk의 부분 범위를 읽는다.
    std::vector<uint8_t> ParlibReader::read_chunk_slice(
        const ParlibChunkRecord& rec,
        uint64_t offset,
        uint64_t size
    ) const {
        std::vector<uint8_t> out;
        if (!ok_ || size == 0 || offset > rec.size) return out;
        const uint64_t max_size = rec.size - offset;
        const uint64_t n64 = std::min(size, max_size);
        if (n64 == 0 || n64 > static_cast<uint64_t>(std::numeric_limits<size_t>::max())) return out;

        std::ifstream ifs(input_path_, std::ios::binary);
        if (!ifs.is_open()) return out;

        ifs.seekg(static_cast<std::streamoff>(rec.offset + offset), std::ios::beg);
        out.resize(static_cast<size_t>(n64));
        ifs.read(reinterpret_cast<char*>(out.data()), static_cast<std::streamsize>(out.size()));
        const std::streamsize got = ifs.gcount();
        if (got < 0) {
            out.clear();
            return out;
        }
        if (static_cast<size_t>(got) != out.size()) {
            out.resize(static_cast<size_t>(got));
        }
        return out;
    }

    /// @brief chunk 전체를 스트리밍으로 읽는 핸들을 연다.
    ParlibChunkStream ParlibReader::open_chunk_stream(const ParlibChunkRecord& rec) const {
        ParlibChunkStream s{};
        if (!ok_) return s;
        auto fp = std::make_shared<std::ifstream>(input_path_, std::ios::binary);
        if (!fp->is_open()) return s;
        fp->seekg(static_cast<std::streamoff>(rec.offset), std::ios::beg);
        if (!fp->good()) return s;

        s.file_ = std::move(fp);
        s.remaining_ = rec.size;
        s.ok_ = true;
        return s;
    }

    /// @brief ExportCIndex 전체를 읽는다.
    std::vector<ParlibExportCEntry> ParlibReader::read_export_c_index() const {
        const auto rec = find_chunk(ParlibChunkKind::kExportCIndex, ParlibLane::kGlobal, 0);
        if (!rec.has_value()) return {};
        auto bytes = read_chunk_slice(*rec, 0, rec->size);
        return parse_export_c_index_(bytes);
    }

    /// @brief NativeDeps 전체를 읽는다.
    std::vector<ParlibNativeDepEntry> ParlibReader::read_native_deps() const {
        const auto rec = find_chunk(ParlibChunkKind::kNativeDeps, ParlibLane::kGlobal, 0);
        if (!rec.has_value()) return {};
        auto bytes = read_chunk_slice(*rec, 0, rec->size);
        return parse_native_deps_(bytes);
    }

    /// @brief 특정 C export 심볼 1개를 조회한다.
    std::optional<ParlibExportCEntry> ParlibReader::lookup_export_c(std::string_view symbol_name) const {
        const auto entries = read_export_c_index();
        for (const auto& e : entries) {
            if (e.symbol == symbol_name) return e;
        }
        return std::nullopt;
    }

    bool ParlibStreamWriter::begin(const ParlibBuildOptions& opt, std::vector<CompileMessage>* external_messages) {
        messages_.clear();
        chunks_.clear();
        begun_ = false;
        finalized_ = false;
        opt_ = opt;

        if (opt.output_path.empty()) {
            push_error_(messages_, "parlib writer: output path is empty.");
            if (external_messages != nullptr) *external_messages = messages_;
            return false;
        }

        namespace fs = std::filesystem;
        const fs::path out_path(opt.output_path);
        std::error_code ec;
        const fs::path parent = out_path.parent_path();
        if (!parent.empty()) {
            fs::create_directories(parent, ec);
            if (ec) {
                push_error_(messages_, "parlib writer: failed to create output directory: " + parent.string());
                if (external_messages != nullptr) *external_messages = messages_;
                return false;
            }
        }

        owned_of_ = std::make_unique<std::ofstream>(opt.output_path, std::ios::binary | std::ios::trunc);
        if (!owned_of_->is_open()) {
            push_error_(messages_, "parlib writer: failed to open output file: " + opt.output_path);
            if (external_messages != nullptr) *external_messages = messages_;
            return false;
        }
        of_raw_ = owned_of_.get();

        std::vector<uint8_t> zero_header(k_header_size_v1, 0);
        of_raw_->write(reinterpret_cast<const char*>(zero_header.data()), static_cast<std::streamsize>(zero_header.size()));
        if (!of_raw_->good()) {
            push_error_(messages_, "parlib writer: failed to write header placeholder.");
            if (external_messages != nullptr) *external_messages = messages_;
            return false;
        }

        begun_ = true;
        push_info_(messages_, "parlib writer: begin output to " + opt.output_path);
        if (external_messages != nullptr) *external_messages = messages_;
        return true;
    }

    bool ParlibStreamWriter::append_chunk_impl_(ParlibChunkPayload meta, const uint8_t* bytes, size_t n, bool has_all_bytes, std::istream* stream) {
        if (!begun_ || finalized_ || of_raw_ == nullptr) {
            push_error_(messages_, "parlib writer: append_chunk called in invalid state.");
            return false;
        }
        if (!is_power_of_two_(meta.alignment)) {
            push_error_(messages_, "parlib writer: chunk alignment must be power-of-two.");
            return false;
        }
        if (meta.compression != ParlibCompression::kNone) {
            push_error_(messages_, "parlib writer: unsupported compression in v1.");
            return false;
        }

        if (!align_output_stream_(*of_raw_, meta.alignment)) {
            push_error_(messages_, "parlib writer: failed to align chunk output position.");
            return false;
        }

        const std::streampos pos = of_raw_->tellp();
        if (pos < 0) {
            push_error_(messages_, "parlib writer: failed to read current output position.");
            return false;
        }

        ParlibChunkRecord rec{};
        rec.kind = meta.kind;
        rec.lane = meta.lane;
        rec.target_id = meta.target_id;
        rec.alignment = meta.alignment;
        rec.compression = meta.compression;
        rec.offset = static_cast<uint64_t>(pos);
        rec.size = 0;
        rec.content_hash = k_hash_seed_content;
        rec.checksum = k_hash_seed_checksum;

        if (has_all_bytes) {
            if (n != 0) {
                of_raw_->write(reinterpret_cast<const char*>(bytes), static_cast<std::streamsize>(n));
                if (!of_raw_->good()) {
                    push_error_(messages_, "parlib writer: failed to write chunk payload.");
                    return false;
                }
                rec.content_hash = fnv1a64_update_(rec.content_hash, bytes, n);
                rec.checksum = fnv1a64_update_(rec.checksum, bytes, n);
                rec.size = static_cast<uint64_t>(n);
            }
        } else {
            if (stream == nullptr) {
                push_error_(messages_, "parlib writer: stream input is null.");
                return false;
            }
            std::vector<uint8_t> buf(64 * 1024, 0);
            while (true) {
                stream->read(reinterpret_cast<char*>(buf.data()), static_cast<std::streamsize>(buf.size()));
                const std::streamsize got = stream->gcount();
                if (got > 0) {
                    const size_t have = static_cast<size_t>(got);
                    of_raw_->write(reinterpret_cast<const char*>(buf.data()), got);
                    if (!of_raw_->good()) {
                        push_error_(messages_, "parlib writer: failed to write chunk payload stream.");
                        return false;
                    }
                    rec.content_hash = fnv1a64_update_(rec.content_hash, buf.data(), have);
                    rec.checksum = fnv1a64_update_(rec.checksum, buf.data(), have);
                    rec.size += static_cast<uint64_t>(have);
                }
                if (!stream->good()) break;
            }
            if (!stream->eof()) {
                push_error_(messages_, "parlib writer: failed while reading chunk input stream.");
                return false;
            }
        }

        rec.checksum ^= rec.size;
        rec.deduplicated = false;
        chunks_.push_back(rec);
        return true;
    }

    bool ParlibStreamWriter::append_chunk(const ParlibChunkPayload& chunk) {
        const uint8_t* p = chunk.bytes.empty() ? nullptr : chunk.bytes.data();
        return append_chunk_impl_(chunk, p, chunk.bytes.size(), /*has_all_bytes=*/true, nullptr);
    }

    bool ParlibStreamWriter::append_chunk_stream(const ParlibChunkPayload& chunk_meta, std::istream& in) {
        return append_chunk_impl_(chunk_meta, nullptr, 0, /*has_all_bytes=*/false, &in);
    }

    ParlibBuildResult ParlibStreamWriter::finalize() {
        ParlibBuildResult out{};
        out.output_path = opt_.output_path;

        if (!begun_ || of_raw_ == nullptr) {
            push_error_(messages_, "parlib writer: finalize called before begin.");
            out.ok = false;
            out.messages = messages_;
            return out;
        }
        if (finalized_) {
            push_error_(messages_, "parlib writer: finalize called more than once.");
            out.ok = false;
            out.messages = messages_;
            return out;
        }

        if (!align_output_stream_(*of_raw_, 8)) {
            push_error_(messages_, "parlib writer: failed to align before TOC.");
        }

        const std::streampos toc_pos = of_raw_->tellp();
        if (toc_pos < 0) {
            push_error_(messages_, "parlib writer: failed to get TOC offset.");
        }

        for (const auto& c : chunks_) {
            const auto ent = serialize_toc_entry_(c);
            of_raw_->write(reinterpret_cast<const char*>(ent.data()), static_cast<std::streamsize>(ent.size()));
        }
        if (!of_raw_->good()) {
            push_error_(messages_, "parlib writer: failed to write TOC.");
        }

        const std::streampos footer_pos = of_raw_->tellp();
        if (footer_pos < 0) {
            push_error_(messages_, "parlib writer: failed to get footer offset.");
        }

        const uint64_t toc_offset = (toc_pos < 0) ? 0 : static_cast<uint64_t>(toc_pos);
        const uint64_t toc_size = static_cast<uint64_t>(chunks_.size()) * k_toc_entry_size_v1;
        const auto footer = serialize_footer_(toc_offset, toc_size);
        of_raw_->write(reinterpret_cast<const char*>(footer.data()), static_cast<std::streamsize>(footer.size()));
        if (!of_raw_->good()) {
            push_error_(messages_, "parlib writer: failed to write footer.");
        }

        const std::streampos end = of_raw_->tellp();
        if (end < 0) {
            push_error_(messages_, "parlib writer: failed to get final file size.");
        }

        ParlibHeaderInfo h{};
        h.format_major = k_format_major_v1;
        h.format_minor = k_format_minor_v1;
        h.flags = opt_.flags;
        h.feature_bits = opt_.feature_bits;
        h.compiler_hash = opt_.compiler_hash;
        h.bundle_id = opt_.bundle_id;
        h.target_triple = opt_.target_triple;
        h.target_summary = opt_.target_summary;
        h.header_size = k_header_size_v1;
        h.chunk_stream_offset = k_header_size_v1;
        h.toc_offset = toc_offset;
        h.toc_size = toc_size;
        h.toc_entry_size = k_toc_entry_size_v1;
        h.toc_entry_count = static_cast<uint32_t>(chunks_.size());
        h.footer_offset = toc_offset + toc_size;
        h.file_size = (end < 0) ? 0 : static_cast<uint64_t>(end);
        h.chunk_stream_size = (h.toc_offset >= h.chunk_stream_offset)
            ? (h.toc_offset - h.chunk_stream_offset)
            : 0;

        const auto header_bytes = serialize_header_(h);
        of_raw_->seekp(0, std::ios::beg);
        of_raw_->write(reinterpret_cast<const char*>(header_bytes.data()), static_cast<std::streamsize>(header_bytes.size()));
        if (!of_raw_->good()) {
            push_error_(messages_, "parlib writer: failed to patch final header.");
        }

        of_raw_->flush();
        if (!of_raw_->good()) {
            push_error_(messages_, "parlib writer: failed to flush output.");
        }

        owned_of_->close();
        finalized_ = true;

        out.header = h;
        out.chunks = chunks_;
        out.file_size = h.file_size;
        out.messages = messages_;
        out.ok = !has_error_messages_(messages_);
        if (out.ok) {
            push_info_(out.messages, "parlib writer: wrote " + std::to_string(out.file_size) + " bytes to " + opt_.output_path);
        }
        return out;
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

        std::unordered_map<ChunkKey, ParlibChunkPayload, ChunkKeyHash> chunk_map;

        ParlibChunkPayload manifest{};
        manifest.kind = ParlibChunkKind::kManifest;
        manifest.lane = ParlibLane::kGlobal;
        manifest.target_id = 0;
        manifest.alignment = 8;
        manifest.compression = ParlibCompression::kNone;
        manifest.bytes = default_manifest_payload_(opt, lanes);
        chunk_map[ChunkKey{manifest.kind, manifest.lane, manifest.target_id}] = std::move(manifest);

        ParlibChunkPayload strings{};
        strings.kind = ParlibChunkKind::kStringTable;
        strings.lane = ParlibLane::kGlobal;
        strings.target_id = 0;
        strings.alignment = 8;
        strings.compression = ParlibCompression::kNone;
        strings.bytes = default_string_table_payload_(opt, lanes);
        chunk_map[ChunkKey{strings.kind, strings.lane, strings.target_id}] = std::move(strings);

        ParlibChunkPayload exports{};
        exports.kind = ParlibChunkKind::kExportCIndex;
        exports.lane = ParlibLane::kGlobal;
        exports.target_id = 0;
        exports.alignment = 8;
        exports.compression = ParlibCompression::kNone;
        exports.bytes = encode_export_c_index_(opt.export_c_symbols);
        chunk_map[ChunkKey{exports.kind, exports.lane, exports.target_id}] = std::move(exports);

        ParlibChunkPayload ndeps{};
        ndeps.kind = ParlibChunkKind::kNativeDeps;
        ndeps.lane = ParlibLane::kGlobal;
        ndeps.target_id = 0;
        ndeps.alignment = 8;
        ndeps.compression = ParlibCompression::kNone;
        ndeps.bytes = encode_native_deps_(opt.native_deps);
        chunk_map[ChunkKey{ndeps.kind, ndeps.lane, ndeps.target_id}] = std::move(ndeps);

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
                c.target_id = 0;
                c.alignment = 8;
                c.compression = ParlibCompression::kNone;
                c.bytes = default_lane_payload_(kind, lane);
                chunk_map[ChunkKey{c.kind, c.lane, c.target_id}] = std::move(c);
            }
        }

        if (opt.include_debug) {
            ParlibChunkPayload debug{};
            debug.kind = ParlibChunkKind::kDebug;
            debug.lane = ParlibLane::kGlobal;
            debug.target_id = 0;
            debug.alignment = 8;
            debug.compression = ParlibCompression::kNone;
            const std::string msg = "debug=enabled\n";
            debug.bytes.assign(msg.begin(), msg.end());
            chunk_map[ChunkKey{debug.kind, debug.lane, debug.target_id}] = std::move(debug);
        }

        for (const auto& c : opt.extra_chunks) {
            chunk_map[ChunkKey{c.kind, c.lane, c.target_id}] = c;
        }

        const auto sorted_chunks = to_sorted_chunks_(chunk_map);

        ParlibStreamWriter writer;
        std::vector<CompileMessage> begin_msgs;
        if (!writer.begin(opt, &begin_msgs)) {
            out.ok = false;
            out.messages = std::move(begin_msgs);
            return out;
        }
        out.messages = begin_msgs;

        bool append_ok = true;
        for (const auto& c : sorted_chunks) {
            if (!writer.append_chunk(c)) {
                append_ok = false;
            }
        }

        auto built = writer.finalize();
        if (!append_ok) {
            built.ok = false;
        }
        return built;
    }

    /// @brief 기존 스켈레톤 API 이름을 유지하면서 실제 구현으로 연결한다.
    ParlibBuildResult build_parlib_skeleton(const ParlibBuildOptions& opt) {
        return build_parlib(opt);
    }

    /// @brief parlib 파일 메타데이터를 읽고 TOC/체크섬을 검사한다.
    ParlibInspectResult inspect_parlib(const std::string& input_path) {
        ParlibInspectResult out{};
        out.input_path = input_path;

        std::vector<CompileMessage> msgs;
        const auto reader_opt = ParlibReader::open(input_path, &msgs);
        out.messages = msgs;
        if (!reader_opt.has_value()) {
            out.ok = false;
            return out;
        }

        const auto& reader = *reader_opt;
        out.ok = reader.ok();
        out.header = reader.read_header();
        out.chunks = reader.list_chunks();
        out.export_c_symbols = reader.read_export_c_index();
        out.native_deps = reader.read_native_deps();

        // checksum/hash 무결성 검증
        bool hash_ok = true;
        for (size_t i = 0; i < out.chunks.size(); ++i) {
            const auto& c = out.chunks[i];
            const auto payload = reader.read_chunk_slice(c, 0, c.size);
            if (payload.size() != c.size) {
                hash_ok = false;
                push_error_(out.messages, "parlib inspect: failed to read full payload for entry #" + std::to_string(i));
                continue;
            }
            const uint64_t h = fnv1a64_update_(k_hash_seed_content, payload.data(), payload.size());
            uint64_t cs = fnv1a64_update_(k_hash_seed_checksum, payload.data(), payload.size());
            cs ^= static_cast<uint64_t>(payload.size());
            if (h != c.content_hash || cs != c.checksum) {
                hash_ok = false;
                push_error_(out.messages,
                    "parlib inspect: checksum/hash mismatch at entry #" + std::to_string(i) +
                    " (" + chunk_kind_name(c.kind) + ":" + lane_name(c.lane) + ")");
            }
        }

        out.ok = out.ok && hash_ok;
        if (out.ok) {
            push_info_(out.messages, "parlib inspect: file is valid (" + std::to_string(out.chunks.size()) + " chunks).");
        }
        return out;
    }

} // namespace parus::backend::parlib
