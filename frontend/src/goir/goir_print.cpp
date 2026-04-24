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

        const char* layout_name_(LayoutClass layout) {
            switch (layout) {
                case LayoutClass::Unknown: return "unknown";
                case LayoutClass::Scalar: return "scalar";
                case LayoutClass::FixedArray: return "fixed-array";
                case LayoutClass::SliceView: return "slice-view";
                case LayoutClass::PlainRecord: return "plain-record";
            }
            return "unknown";
        }

        const char* place_name_(PlaceKind place) {
            switch (place) {
                case PlaceKind::None: return "none";
                case PlaceKind::LocalSlot: return "local-slot";
                case PlaceKind::FieldPath: return "field-path";
                case PlaceKind::IndexPath: return "index-path";
                case PlaceKind::SubView: return "subview";
            }
            return "unknown";
        }

        std::string type_name_(TypeId ty, const parus::ty::TypePool* types) {
            if (ty == kInvalidType) return "<invalid>";
            if (types == nullptr) return "%" + std::to_string(ty);
            return types->to_string(ty);
        }

        void print_block_args_(std::ostringstream& os,
                               const std::vector<ValueId>& args) {
            if (args.empty()) return;
            os << "(";
            for (size_t i = 0; i < args.size(); ++i) {
                if (i != 0) os << ", ";
                os << "%" << args[i];
            }
            os << ")";
        }

        void print_inst_(std::ostringstream& os,
                         const Module& module,
                         const Inst& inst,
                         const parus::ty::TypePool* types) {
            if (inst.result != kInvalidId) {
                const auto& result = module.values[inst.result];
                os << "      %" << inst.result << " : "
                   << type_name_(result.ty, types)
                   << " [" << ownership_name_(result.ownership.kind)
                   << ", " << layout_name_(result.layout);
                if (result.is_place) os << ", place=" << place_name_(result.place_kind);
                os << "] = ";
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
                } else if constexpr (std::is_same_v<T, OpArrayMake>) {
                    os << "array.make";
                    print_block_args_(os, data.elems);
                } else if constexpr (std::is_same_v<T, OpArrayGet>) {
                    os << "array.get %" << data.base << ", %" << data.index;
                } else if constexpr (std::is_same_v<T, OpArrayLen>) {
                    os << "array.len %" << data.base;
                } else if constexpr (std::is_same_v<T, OpRecordMake>) {
                    os << "record.make {";
                    for (size_t i = 0; i < data.fields.size(); ++i) {
                        if (i != 0) os << ", ";
                        os << module.string(data.fields[i].name) << "=%" << data.fields[i].value;
                    }
                    os << "}";
                } else if constexpr (std::is_same_v<T, OpLocalSlot>) {
                    os << "local.slot";
                    if (data.debug_name != kInvalidId) os << " $" << module.string(data.debug_name);
                } else if constexpr (std::is_same_v<T, OpFieldPlace>) {
                    os << "field.place %" << data.base << "." << module.string(data.field_name);
                } else if constexpr (std::is_same_v<T, OpIndexPlace>) {
                    os << "index.place %" << data.base << "[%" << data.index << "]";
                } else if constexpr (std::is_same_v<T, OpSubView>) {
                    os << "subview %" << data.base << " offset=%" << data.offset << " len=%" << data.length;
                } else if constexpr (std::is_same_v<T, OpLoad>) {
                    os << "load %" << data.place;
                } else if constexpr (std::is_same_v<T, OpStore>) {
                    os << "store %" << data.value << " -> %" << data.place;
                } else if constexpr (std::is_same_v<T, OpBorrowView>) {
                    os << "borrow.view %" << data.source_place;
                } else if constexpr (std::is_same_v<T, OpEscapeView>) {
                    os << "escape.view %" << data.source_place;
                } else if constexpr (std::is_same_v<T, OpSemanticInvoke>) {
                    os << "semantic.invoke @" << module.string(module.computations[data.computation].name);
                    print_block_args_(os, data.args);
                } else if constexpr (std::is_same_v<T, OpCallDirect>) {
                    os << "call.direct @" << module.string(module.realizations[data.callee].name);
                    print_block_args_(os, data.args);
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
                os << "    block ^bb" << bid;
                if (!block.params.empty()) {
                    os << "(";
                    for (size_t pi = 0; pi < block.params.size(); ++pi) {
                        if (pi != 0) os << ", ";
                        os << "%" << block.params[pi] << ": " << type_name_(module.values[block.params[pi]].ty, types);
                    }
                    os << ")";
                }
                os << "\n";
                for (const auto iid : block.insts) {
                    print_inst_(os, module, module.insts[iid], types);
                }
                if (block.has_term) {
                    os << "      term ";
                    std::visit([&](const auto& term) {
                        using T = std::decay_t<decltype(term)>;
                        if constexpr (std::is_same_v<T, TermBr>) {
                            os << "br ^bb" << term.target;
                            print_block_args_(os, term.args);
                        } else if constexpr (std::is_same_v<T, TermCondBr>) {
                            os << "condbr %" << term.cond << ", ^bb" << term.then_bb;
                            print_block_args_(os, term.then_args);
                            os << ", ^bb" << term.else_bb;
                            print_block_args_(os, term.else_args);
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
