#pragma once

#include <parus/ast/Nodes.hpp>
#include <parus/diag/Diagnostic.hpp>

#include <string>
#include <unordered_map>

namespace parus::passes {

    enum class GenericTemplateKind : uint8_t {
        kFn = 0,
        kClass,
        kProto,
        kActs,
        kStruct,
    };

    struct GenericTemplateInfo {
        GenericTemplateKind kind = GenericTemplateKind::kFn;
        uint32_t arity = 0;
        ast::StmtId sid = ast::k_invalid_stmt;
        Span span{};
    };

    struct GenericPrepResult {
        std::unordered_map<std::string, GenericTemplateInfo> templates{};
    };

    GenericPrepResult run_generic_prep(
        const ast::AstArena& ast,
        ast::StmtId program_root,
        diag::Bag& bag
    );

} // namespace parus::passes

