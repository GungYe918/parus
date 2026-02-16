// backend/include/parus/backend/parlib/Parlib.hpp
#pragma once

#include <parus/backend/Backend.hpp>

#include <cstddef>
#include <cstdint>
#include <fstream>
#include <iosfwd>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace parus::backend::parlib {

    /// @brief parlib 내부 청크 종류 식별자(v1 정본).
    enum class ParlibChunkKind : uint16_t {
        kManifest = 1,
        kStringTable = 2,
        kExportCIndex = 3,
        kNativeDeps = 4,
        kSymbolIndex = 5,
        kTypeMeta = 6,
        kOirArchive = 7,
        kObjectArchive = 8,
        kDebug = 9,
        kSourceMap = 10,
        kNativeArchivePayload = 11,
        kReserved = 0x7FFF,
    };

    /// @brief parlib lane 식별자.
    enum class ParlibLane : uint16_t {
        kGlobal = 0,
        kPcore = 1,
        kPrt = 2,
        kPstd = 3,
        kVendorBegin = 0x8000,
    };

    /// @brief 청크 압축 방식 식별자(v1은 무압축만 지원).
    enum class ParlibCompression : uint16_t {
        kNone = 0,
    };

    /// @brief NativeDeps의 라이브러리 종류.
    enum class ParlibNativeDepKind : uint8_t {
        kStatic = 0,
        kShared = 1,
        kFramework = 2,
        kSystem = 3,
    };

    /// @brief NativeDeps의 저장 모드.
    enum class ParlibNativeDepMode : uint8_t {
        kEmbed = 0,
        kReference = 1,
    };

    /// @brief C ABI export 심볼 인덱스 항목.
    struct ParlibExportCEntry {
        std::string symbol{};
        std::string signature{};
        ParlibLane lane = ParlibLane::kGlobal;
        ParlibChunkKind chunk_kind = ParlibChunkKind::kObjectArchive;
        uint32_t target_id = 0;
        bool visible = true;
    };

    /// @brief 외부 네이티브 의존성 인덱스 항목.
    struct ParlibNativeDepEntry {
        std::string name{};
        ParlibNativeDepKind kind = ParlibNativeDepKind::kStatic;
        ParlibNativeDepMode mode = ParlibNativeDepMode::kReference;
        std::string target_filter{};
        uint32_t link_order = 0;
        bool required = true;
        uint64_t hash = 0;
        std::string reference{};
    };

    /// @brief parlib 청크 입력 데이터.
    struct ParlibChunkPayload {
        ParlibChunkKind kind = ParlibChunkKind::kManifest;
        ParlibLane lane = ParlibLane::kGlobal;
        uint32_t target_id = 0;
        uint32_t alignment = 8;
        ParlibCompression compression = ParlibCompression::kNone;
        std::vector<uint8_t> bytes{};
    };

    /// @brief parlib 헤더 정보(읽기 결과).
    struct ParlibHeaderInfo {
        uint16_t format_major = 1;
        uint16_t format_minor = 1;
        uint32_t flags = 0;
        uint64_t feature_bits = 0;
        uint64_t compiler_hash = 0;
        std::string bundle_id{};
        std::string target_triple{};
        std::string target_summary{};

        uint32_t header_size = 0;
        uint64_t chunk_stream_offset = 0;
        uint64_t chunk_stream_size = 0;
        uint64_t toc_offset = 0;
        uint64_t toc_size = 0;
        uint32_t toc_entry_size = 0;
        uint32_t toc_entry_count = 0;
        uint64_t footer_offset = 0;
        uint64_t file_size = 0;
    };

    /// @brief TOC 항목 1개에 대한 메타데이터.
    struct ParlibChunkRecord {
        ParlibChunkKind kind = ParlibChunkKind::kManifest;
        ParlibLane lane = ParlibLane::kGlobal;
        uint32_t target_id = 0;
        uint32_t alignment = 8;
        ParlibCompression compression = ParlibCompression::kNone;

        uint64_t offset = 0;
        uint64_t size = 0;
        uint64_t checksum = 0;
        uint64_t content_hash = 0;
        bool deduplicated = false;
    };

    /// @brief parlib 생성 입력.
    struct ParlibBuildOptions {
        std::string output_path{};
        std::string bundle_id{};
        std::string target_triple{};
        std::string target_summary{};
        uint64_t feature_bits = 0;
        uint32_t flags = 0;
        uint64_t compiler_hash = 0;

        // 기본 lane 구성: pcore/prt/pstd
        bool include_pcore = true;
        bool include_prt = true;
        bool include_pstd = true;
        bool include_debug = false;

        std::vector<ParlibExportCEntry> export_c_symbols{};
        std::vector<ParlibNativeDepEntry> native_deps{};

        // 기본 생성 청크를 덮어쓰거나 추가할 사용자 청크.
        std::vector<ParlibChunkPayload> extra_chunks{};
    };

    /// @brief parlib 생성 결과.
    struct ParlibBuildResult {
        bool ok = false;
        std::string output_path{};
        uint64_t file_size = 0;
        ParlibHeaderInfo header{};
        std::vector<ParlibChunkRecord> chunks{};
        std::vector<CompileMessage> messages{};
    };

    /// @brief parlib 검사 결과.
    struct ParlibInspectResult {
        bool ok = false;
        std::string input_path{};
        ParlibHeaderInfo header{};
        std::vector<ParlibChunkRecord> chunks{};
        std::vector<ParlibExportCEntry> export_c_symbols{};
        std::vector<ParlibNativeDepEntry> native_deps{};
        std::vector<CompileMessage> messages{};
    };

    /// @brief chunk 범위를 스트리밍으로 읽기 위한 reader.
    class ParlibChunkStream {
    public:
        bool ok() const { return ok_ && file_ != nullptr; }
        uint64_t remaining() const { return remaining_; }

        /// @brief 남은 범위에서 최대 max_bytes 만큼 읽는다.
        bool read_some(std::vector<uint8_t>& out, size_t max_bytes);

    private:
        friend class ParlibReader;

        std::shared_ptr<std::ifstream> file_{};
        uint64_t remaining_ = 0;
        bool ok_ = false;
    };

    /// @brief Footer/TOC 기반 랜덤 액세스 리더.
    class ParlibReader {
    public:
        static std::optional<ParlibReader> open(
            const std::string& input_path,
            std::vector<CompileMessage>* external_messages = nullptr
        );

        bool ok() const { return ok_; }
        const ParlibHeaderInfo& read_header() const { return header_; }
        const std::vector<ParlibChunkRecord>& list_chunks() const { return chunks_; }
        const std::vector<CompileMessage>& messages() const { return messages_; }

        std::optional<ParlibChunkRecord> find_chunk(
            ParlibChunkKind kind,
            ParlibLane lane,
            uint32_t target_id = 0
        ) const;

        std::vector<uint8_t> read_chunk_slice(
            const ParlibChunkRecord& rec,
            uint64_t offset,
            uint64_t size
        ) const;

        ParlibChunkStream open_chunk_stream(const ParlibChunkRecord& rec) const;

        std::optional<ParlibExportCEntry> lookup_export_c(std::string_view symbol_name) const;
        std::vector<ParlibExportCEntry> read_export_c_index() const;
        std::vector<ParlibNativeDepEntry> read_native_deps() const;

    private:
        std::string input_path_{};
        ParlibHeaderInfo header_{};
        std::vector<ParlibChunkRecord> chunks_{};
        std::vector<CompileMessage> messages_{};
        bool ok_ = false;
    };

    /// @brief [Header][Chunk Stream][TOC][Footer] 순서로 parlib를 쓰는 스트리밍 writer.
    class ParlibStreamWriter {
    public:
        bool begin(const ParlibBuildOptions& opt, std::vector<CompileMessage>* external_messages = nullptr);
        bool append_chunk(const ParlibChunkPayload& chunk);
        bool append_chunk_stream(const ParlibChunkPayload& chunk_meta, std::istream& in);
        ParlibBuildResult finalize();

    private:
        bool append_chunk_impl_(ParlibChunkPayload meta, const uint8_t* bytes, size_t n, bool has_all_bytes, std::istream* stream);

        ParlibBuildOptions opt_{};
        std::ofstream* of_raw_ = nullptr;
        std::unique_ptr<std::ofstream> owned_of_{};
        std::vector<ParlibChunkRecord> chunks_{};
        std::vector<CompileMessage> messages_{};
        bool begun_ = false;
        bool finalized_ = false;
    };

    /// @brief v1 parlib 파일을 생성한다.
    ParlibBuildResult build_parlib(const ParlibBuildOptions& opt);

    /// @brief 기존 스켈레톤 이름 호환용 API.
    ParlibBuildResult build_parlib_skeleton(const ParlibBuildOptions& opt);

    /// @brief 생성된 parlib 파일 메타데이터를 읽고 무결성을 점검한다.
    ParlibInspectResult inspect_parlib(const std::string& input_path);

    /// @brief lane 이름을 텍스트로 변환한다.
    std::string lane_name(ParlibLane lane);

    /// @brief chunk kind 이름을 텍스트로 변환한다.
    std::string chunk_kind_name(ParlibChunkKind kind);

    /// @brief compression 이름을 텍스트로 변환한다.
    std::string compression_name(ParlibCompression c);

    /// @brief native dep kind 이름을 텍스트로 변환한다.
    std::string native_dep_kind_name(ParlibNativeDepKind k);

    /// @brief native dep mode 이름을 텍스트로 변환한다.
    std::string native_dep_mode_name(ParlibNativeDepMode m);

} // namespace parus::backend::parlib
