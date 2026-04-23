#include <parus/goir/Placement.hpp>
#include <parus/goir/Passes.hpp>
#include <parus/goir/Verify.hpp>

namespace parus::goir {

    PlacementResult place_module(const Module& open_module) {
        PlacementResult out{};

        if (open_module.header.stage_kind != StageKind::Open) {
            out.messages.push_back(Message{"gOIR placement expects an open-stage module"});
            return out;
        }

        const auto verrs = verify(open_module);
        if (!verrs.empty()) {
            out.messages = verrs;
            return out;
        }

        out.mod = open_module;
        out.mod.header.stage_kind = StageKind::Placed;

        for (auto& real : out.mod.realizations) {
            if (real.family == FamilyKind::None || real.family == FamilyKind::Core) {
                real.family = FamilyKind::Cpu;
            }
        }

        for (auto& inst : out.mod.insts) {
            if (const auto* invoke = std::get_if<OpSemanticInvoke>(&inst.data)) {
                if (invoke->computation == kInvalidId ||
                    static_cast<size_t>(invoke->computation) >= out.mod.computations.size()) {
                    out.messages.push_back(Message{"placement found invalid semantic invoke computation"});
                    return out;
                }
                const auto& comp = out.mod.computations[invoke->computation];
                if (comp.realizations.empty()) {
                    out.messages.push_back(Message{"placement found computation without realizations"});
                    return out;
                }
                inst.data = OpCallDirect{
                    .callee = comp.realizations.front(),
                    .args = invoke->args,
                };
                inst.eff = Effect::Call;
            }
        }

        for (const auto& value : out.mod.values) {
            if (value.ownership.kind != OwnershipKind::Plain) {
                out.messages.push_back(Message{
                    "M0 placement rejects ownership-sensitive gOIR values; runtime ownership lowering is not implemented yet."
                });
                return out;
            }
        }

        run_placed_passes(out.mod);
        out.messages = verify(out.mod);
        out.ok = out.messages.empty();
        return out;
    }

} // namespace parus::goir
