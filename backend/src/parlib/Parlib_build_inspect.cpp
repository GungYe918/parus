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
