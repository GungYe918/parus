// compiler/src/oir/oir_verify.cpp
#include <gaupel/oir/Verify.hpp>


namespace gaupel::oir {

    std::vector<VerifyError> verify(const Module& m) {
        std::vector<VerifyError> errs;

        // funcs: entry validity
        for (const auto& f : m.funcs) {
            if (f.entry == kInvalidId || f.entry >= m.blocks.size()) {
                errs.push_back({"function has invalid entry: " + f.name});
            }
        }

        // blocks: terminator existence
        for (size_t i = 0; i < m.blocks.size(); i++) {
            if (!m.blocks[i].has_term) {
                errs.push_back({"block has no terminator: #" + std::to_string(i)});
            }
        }

        // (v0) you can later add:
        // - terminator block arg count checks
        // - value id range checks

        return errs;
    }

} // namespace gaupel::oir