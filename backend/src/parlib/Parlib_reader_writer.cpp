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
