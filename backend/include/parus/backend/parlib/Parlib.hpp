// backend/include/parus/backend/parlib/Parlib.hpp
#pragma once

#include <parus/backend/Backend.hpp>

#include <cstdint>
#include <string>
#include <vector>

namespace parus::backend::parlib {

    /// @brief parlib 내부 청크 종류 식별자.
    enum class ParlibChunkKind : uint16_t {
        kManifest = 1,
        kStringTable = 2,
        kSymbolIndex = 3,
        kTypeMeta = 4,
        kOirArchive = 5,
        kObjectArchive = 6,
        kDebug = 7,
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

    /// @brief 청크 압축 방식 식별자(v1은 무압축만 사용).
    enum class ParlibCompression : uint16_t {
        kNone = 0,
    };

    /// @brief parlib 청크 입력 데이터.
    struct ParlibChunkPayload {
        ParlibChunkKind kind = ParlibChunkKind::kManifest;
        ParlibLane lane = ParlibLane::kGlobal;
        uint32_t alignment = 8;
        ParlibCompression compression = ParlibCompression::kNone;
        std::vector<uint8_t> bytes{};
    };

    /// @brief parlib 헤더 정보(읽기 결과).
    struct ParlibHeaderInfo {
        uint16_t format_major = 1;
        uint16_t format_minor = 0;
        uint32_t flags = 0;
        uint64_t feature_bits = 0;
        std::string target_triple{};
        uint64_t toc_offset = 0;
        uint32_t toc_entry_size = 0;
        uint32_t toc_entry_count = 0;
        uint64_t chunk_data_offset = 0;
        uint64_t file_size = 0;
    };

    /// @brief TOC 항목 1개에 대한 메타데이터.
    struct ParlibChunkRecord {
        ParlibChunkKind kind = ParlibChunkKind::kManifest;
        ParlibLane lane = ParlibLane::kGlobal;
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
        std::string target_triple{};
        uint64_t feature_bits = 0;
        uint32_t flags = 0;

        // 기본 lane 구성: pcore/prt/pstd
        bool include_pcore = true;
        bool include_prt = true;
        bool include_pstd = true;
        bool include_debug = false;

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
        std::vector<CompileMessage> messages{};
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

} // namespace parus::backend::parlib
