// frontend/include/parus/oir/Builder.hpp
#pragma once
#include <parus/oir/Inst.hpp>
#include <parus/sema/SymbolTable.hpp>
#include <parus/sir/SIR.hpp>
#include <parus/sir/Verify.hpp>
#include <parus/ty/TypePool.hpp>

#include <unordered_set>
#include <vector>


namespace parus::oir {

    struct BuildResult {
        Module mod;
        bool gate_passed = true;
        std::vector<parus::sir::VerifyError> gate_errors;
    };

    class Builder {
    public:
        Builder(const parus::sir::Module& sir_mod,
                const parus::ty::TypePool& ty,
                const std::unordered_set<parus::ty::TypeId>* tag_only_enum_type_ids = nullptr,
                const parus::sema::SymbolTable* sym = nullptr)
            : sir_(sir_mod), ty_(ty), tag_only_enum_type_ids_(tag_only_enum_type_ids), sym_(sym) {}

        BuildResult build();

    private:
        const parus::sir::Module& sir_;
        [[maybe_unused]] const parus::ty::TypePool& ty_;
        const std::unordered_set<parus::ty::TypeId>* tag_only_enum_type_ids_ = nullptr;
        const parus::sema::SymbolTable* sym_ = nullptr;
    };

} // namespace parus::oir
