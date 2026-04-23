#include <parus/goir/Print.hpp>

#include <sstream>

namespace parus::goir {

    namespace {

        const char* stage_name_(StageKind stage) {
            switch (stage) {
                case StageKind::Open: return "open";
                case StageKind::Placed: return "placed";
            }
            return "unknown";
        }

        const char* family_name_(FamilyKind family) {
            switch (family) {
                case FamilyKind::None: return "none";
                case FamilyKind::Core: return "core";
                case FamilyKind::Cpu: return "cpu";
                case FamilyKind::Gpu: return "gpu";
                case FamilyKind::HwStruct: return "hw.struct";
                case FamilyKind::HwFlow: return "hw.flow";
                case FamilyKind::Bridge: return "bridge";
            }
            return "unknown";
        }

        const char* ownership_name_(OwnershipKind kind) {
            switch (kind) {
                case OwnershipKind::Plain: return "plain";
                case OwnershipKind::BorrowShared: return "borrow.shared";
                case OwnershipKind::BorrowMut: return "borrow.mut";
                case OwnershipKind::Escape: return "escape";
            }
            return "unknown";
        }

        std::string type_name_(TypeId ty, const parus::ty::TypePool* types) {
            if (ty == kInvalidType) return "<invalid>";
            if (types == nullptr) return "%" + std::to_string(ty);
            return types->to_string(ty);
        }

        void print_inst_(std::ostringstream& os,
                         const Module& module,
                         const Inst& inst,
                         const parus::ty::TypePool* types) {
            if (inst.result != kInvalidId) {
                os << "      %" << inst.result << " : "
                   << type_name_(module.values[inst.result].ty, types)
                   << " [" << ownership_name_(module.values[inst.result].ownership.kind) << "] = ";
            } else {
                os << "      ";
            }

            std::visit([&](const auto& data) {
                using T = std::decay_t<decltype(data)>;
                if constexpr (std::is_same_v<T, OpConstInt>) {
                    os << "const.int " << data.text;
                } else if constexpr (std::is_same_v<T, OpConstFloat>) {
                    os << "const.float " << data.text;
                } else if constexpr (std::is_same_v<T, OpConstBool>) {
                    os << "const.bool " << (data.value ? "true" : "false");
                } else if constexpr (std::is_same_v<T, OpConstNull>) {
                    os << "const.null";
                } else if constexpr (std::is_same_v<T, OpUnary>) {
                    os << "unary %" << data.src;
                } else if constexpr (std::is_same_v<T, OpBinary>) {
                    os << "binary %" << data.lhs << ", %" << data.rhs;
                } else if constexpr (std::is_same_v<T, OpCast>) {
                    os << "cast %" << data.src << " to " << type_name_(data.to, types);
                } else if constexpr (std::is_same_v<T, OpSemanticInvoke>) {
                    os << "semantic.invoke @" << module.string(module.computations[data.computation].name);
                } else if constexpr (std::is_same_v<T, OpCallDirect>) {
                    os << "call.direct @" << module.string(module.realizations[data.callee].name);
                }
            }, inst.data);
            os << "\n";
        }

    } // namespace

    std::string to_string(const Module& module, const parus::ty::TypePool* types) {
        std::ostringstream os{};
        os << "goir.module stage=" << stage_name_(module.header.stage_kind) << "\n";
        for (size_t i = 0; i < module.computations.size(); ++i) {
            const auto& comp = module.computations[i];
            os << "  computation @" << module.string(comp.name)
               << " sig=@" << module.string(module.semantic_sigs[comp.sig].name) << "\n";
        }
        for (size_t i = 0; i < module.realizations.size(); ++i) {
            const auto& real = module.realizations[i];
            os << "  realization @" << module.string(real.name)
               << " family=" << family_name_(real.family)
               << " entry=%" << real.entry << "\n";
            for (const auto bid : real.blocks) {
                const auto& block = module.blocks[bid];
                os << "    block ^bb" << bid << "\n";
                for (const auto iid : block.insts) {
                    print_inst_(os, module, module.insts[iid], types);
                }
                if (block.has_term) {
                    os << "      term ";
                    std::visit([&](const auto& term) {
                        using T = std::decay_t<decltype(term)>;
                        if constexpr (std::is_same_v<T, TermBr>) {
                            os << "br ^bb" << term.target;
                        } else if constexpr (std::is_same_v<T, TermCondBr>) {
                            os << "condbr %" << term.cond << ", ^bb" << term.then_bb << ", ^bb" << term.else_bb;
                        } else if constexpr (std::is_same_v<T, TermRet>) {
                            if (term.has_value) os << "ret %" << term.value;
                            else os << "ret";
                        }
                    }, block.term);
                    os << "\n";
                }
            }
        }
        return os.str();
    }

} // namespace parus::goir
