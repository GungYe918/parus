// compiler/include/parus/oir/Builder.hpp
#pragma once
#include <parus/oir/Inst.hpp>
#include <parus/sir/SIR.hpp>
#include <parus/sir/Verify.hpp>
#include <parus/ty/TypePool.hpp>

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
                const parus::ty::TypePool& ty)
            : sir_(sir_mod), ty_(ty) {}

        BuildResult build();

    private:
        const parus::sir::Module& sir_;
        [[maybe_unused]] const parus::ty::TypePool& ty_;
    };

} // namespace parus::oir
