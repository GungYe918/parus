// backend/src/parlib/Parlib.cpp
#include "Parlib_internal.hpp"

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
