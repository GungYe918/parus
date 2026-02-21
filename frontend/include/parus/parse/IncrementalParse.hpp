#pragma once

#include <parus/ast/Nodes.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/lex/Token.hpp>
#include <parus/parse/Parser.hpp>
#include <parus/ty/TypePool.hpp>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>
#include <string>
#include <string_view>

namespace parus::parse {

    struct EditWindow {
        uint32_t lo = 0;
        uint32_t hi = 0;
    };

    struct TopItemMeta {
        ast::StmtId sid = ast::k_invalid_stmt;
        uint32_t lo = 0;
        uint32_t hi = 0;
    };

    struct ParseSnapshot {
        ast::AstArena ast{};
        ty::TypePool types{};
        ast::StmtId root = ast::k_invalid_stmt;
        std::vector<Token> tokens{};
        std::vector<TopItemMeta> top_items{};
        uint64_t revision = 0;
    };

    enum class ReparseMode : uint8_t {
        kNone = 0,
        kFullRebuild,
        kIncrementalMerge,
        kFallbackFullRebuild,
    };

    class IncrementalParserSession {
    public:
        bool initialize(std::string_view source, uint32_t file_id, diag::Bag& bag);
        bool reparse_with_edits(std::string_view source,
                                uint32_t file_id,
                                std::span<const EditWindow> edits,
                                diag::Bag& bag);

        const ParseSnapshot& snapshot() const { return snapshot_; }
        ParseSnapshot& mutable_snapshot() { return snapshot_; }

        bool ready() const { return ready_; }
        ReparseMode last_mode() const { return last_mode_; }

        void set_feature_flags(ParserFeatureFlags flags) { feature_flags_ = flags; }
        const ParserFeatureFlags& feature_flags() const { return feature_flags_; }

    private:
        bool full_rebuild_(std::string_view source,
                           uint32_t file_id,
                           diag::Bag& bag,
                           ReparseMode mode);
        bool try_incremental_merge_(std::string_view source,
                                    uint32_t file_id,
                                    std::span<const EditWindow> edits,
                                    diag::Bag& bag);

        ParseSnapshot snapshot_{};
        bool ready_ = false;
        ReparseMode last_mode_ = ReparseMode::kNone;
        uint64_t revision_seq_ = 0;
        ParserFeatureFlags feature_flags_{};

        // AST/TypePool 내부 string_view 수명 보장을 위한 source 보관.
        std::vector<std::shared_ptr<std::string>> source_owners_{};
    };

} // namespace parus::parse
