// compiler/include/gaupel/oir/Builder.hpp
#pragma once
#include <gaupel/oir/Inst.hpp>
#include <gaupel/sir/SIR.hpp>
#include <gaupel/sir/Verify.hpp>
#include <gaupel/ty/TypePool.hpp>

#include <vector>


namespace gaupel::oir {

    struct BuildResult {
        Module mod;
        bool gate_passed = true;
        std::vector<gaupel::sir::VerifyError> gate_errors;
    };

    class Builder {
    public:
        Builder(const gaupel::sir::Module& sir_mod,
                const gaupel::ty::TypePool& ty)
            : sir_(sir_mod), ty_(ty) {}

        BuildResult build();

    private:
        const gaupel::sir::Module& sir_;
        [[maybe_unused]] const gaupel::ty::TypePool& ty_;
    };

} // namespace gaupel::oir
