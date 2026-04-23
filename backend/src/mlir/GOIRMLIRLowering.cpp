#include <parus/backend/mlir/GOIRMLIRLowering.hpp>

#include <parus/goir/Verify.hpp>

#include <mlir/Conversion/Passes.h>
#include <mlir/Dialect/Arith/IR/Arith.h>
#include <mlir/Dialect/ControlFlow/IR/ControlFlow.h>
#include <mlir/Dialect/Func/IR/FuncOps.h>
#include <mlir/Dialect/Index/IR/IndexDialect.h>
#include <mlir/Dialect/LLVMIR/LLVMDialect.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/DialectRegistry.h>
#include <mlir/Parser/Parser.h>
#include <mlir/Pass/PassManager.h>
#include <mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h>
#include <mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h>
#include <mlir/Target/LLVMIR/Export.h>

#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/raw_ostream.h>

#include <sstream>
#include <string>
#include <unordered_map>

namespace parus::backend::mlir {

    namespace {

        void push_error_(std::vector<CompileMessage>& out, std::string text) {
            out.push_back(CompileMessage{
                .is_error = true,
                .text = std::move(text),
            });
        }

        bool is_builtin_(const parus::ty::TypePool& types, parus::ty::TypeId ty, parus::ty::Builtin builtin) {
            if (ty == parus::ty::kInvalidType) return false;
            const auto& t = types.get(ty);
            return t.kind == parus::ty::Kind::kBuiltin && t.builtin == builtin;
        }

        bool is_integer_builtin_(parus::ty::Builtin builtin) {
            using B = parus::ty::Builtin;
            switch (builtin) {
                case B::kBool:
                case B::kChar:
                case B::kI8:
                case B::kI16:
                case B::kI32:
                case B::kI64:
                case B::kU8:
                case B::kU16:
                case B::kU32:
                case B::kU64:
                case B::kISize:
                case B::kUSize:
                    return true;
                default:
                    return false;
            }
        }

        bool is_signed_builtin_(parus::ty::Builtin builtin) {
            using B = parus::ty::Builtin;
            switch (builtin) {
                case B::kChar:
                case B::kI8:
                case B::kI16:
                case B::kI32:
                case B::kI64:
                case B::kISize:
                    return true;
                default:
                    return false;
            }
        }

        bool is_float_builtin_(parus::ty::Builtin builtin) {
            return builtin == parus::ty::Builtin::kF32 || builtin == parus::ty::Builtin::kF64;
        }

        std::optional<parus::ty::Builtin> builtin_of_(const parus::ty::TypePool& types, parus::ty::TypeId ty) {
            if (ty == parus::ty::kInvalidType) return std::nullopt;
            const auto& t = types.get(ty);
            if (t.kind != parus::ty::Kind::kBuiltin) return std::nullopt;
            return t.builtin;
        }

        std::optional<std::string> mlir_type_(const parus::ty::TypePool& types, parus::ty::TypeId ty) {
            const auto builtin = builtin_of_(types, ty);
            if (!builtin.has_value()) return std::nullopt;
            using B = parus::ty::Builtin;
            switch (*builtin) {
                case B::kUnit: return std::string("()");
                case B::kBool: return std::string("i1");
                case B::kChar: return std::string("i32");
                case B::kI8:
                case B::kU8: return std::string("i8");
                case B::kI16:
                case B::kU16: return std::string("i16");
                case B::kI32:
                case B::kU32: return std::string("i32");
                case B::kI64:
                case B::kU64:
                case B::kISize:
                case B::kUSize: return std::string("i64");
                case B::kF32: return std::string("f32");
                case B::kF64: return std::string("f64");
                default: return std::nullopt;
            }
        }

        std::string sanitize_int_literal_(std::string_view text) {
            std::string out{};
            out.reserve(text.size());
            size_t i = 0;
            if (!text.empty() && (text[0] == '+' || text[0] == '-')) {
                out.push_back(text[0]);
                i = 1;
            }
            for (; i < text.size(); ++i) {
                const char ch = text[i];
                if (ch >= '0' && ch <= '9') out.push_back(ch);
                else if (ch == '_') continue;
                else break;
            }
            return out.empty() ? std::string("0") : out;
        }

        std::string sanitize_float_literal_(std::string_view text) {
            std::string out{};
            out.reserve(text.size());
            bool in_exp = false;
            size_t i = 0;
            if (!text.empty() && (text[0] == '+' || text[0] == '-')) {
                out.push_back(text[0]);
                i = 1;
            }
            for (; i < text.size(); ++i) {
                const char ch = text[i];
                if ((ch >= '0' && ch <= '9') || ch == '.') {
                    out.push_back(ch);
                    continue;
                }
                if (ch == '_' ) continue;
                if ((ch == 'e' || ch == 'E') && !in_exp) {
                    out.push_back(ch);
                    in_exp = true;
                    continue;
                }
                if ((ch == '+' || ch == '-') && in_exp &&
                    !out.empty() && (out.back() == 'e' || out.back() == 'E')) {
                    out.push_back(ch);
                    continue;
                }
                break;
            }
            return out.empty() ? std::string("0.0") : out;
        }

        class MlirEmitter {
        public:
            MlirEmitter(const parus::goir::Module& module, const parus::ty::TypePool& types)
                : module_(module), types_(types), value_names_(module.values.size()) {}

            GOIRLoweringResult emit() {
                GOIRLoweringResult out{};
                if (module_.header.stage_kind != parus::goir::StageKind::Placed) {
                    push_error_(out.messages, "MLIR lowering expects a gOIR-placed module.");
                    return out;
                }
                const auto verrs = parus::goir::verify(module_);
                for (const auto& err : verrs) push_error_(out.messages, err.msg);
                if (!out.messages.empty()) return out;

                std::ostringstream os{};
                os << "module {\n";
                for (size_t i = 0; i < module_.realizations.size(); ++i) {
                    const auto& real = module_.realizations[i];
                    if (real.family != parus::goir::FamilyKind::Cpu &&
                        real.family != parus::goir::FamilyKind::Core) {
                        push_error_(out.messages, "MLIR lowering only supports CPU/core placed realizations in M0.");
                        continue;
                    }
                    if (failed_) break;
                    emit_realization_(os, static_cast<parus::goir::RealizationId>(i), real);
                }
                os << "}\n";

                if (!out.messages.empty()) return out;
                out.ok = true;
                out.mlir_text = os.str();
                return out;
            }

        private:
            const parus::goir::Module& module_;
            const parus::ty::TypePool& types_;
            std::vector<std::string> value_names_;
            uint32_t temp_counter_ = 0;
            bool failed_ = false;
            std::vector<CompileMessage> messages_{};

            void fail_(std::string text) {
                failed_ = true;
                messages_.push_back(CompileMessage{.is_error = true, .text = std::move(text)});
            }

            std::string value_name_(parus::goir::ValueId id) {
                if (id == parus::goir::kInvalidId || static_cast<size_t>(id) >= value_names_.size()) return "%invalid";
                if (!value_names_[id].empty()) return value_names_[id];
                value_names_[id] = "%v" + std::to_string(id);
                return value_names_[id];
            }

            std::string make_temp_() {
                return "%tmp" + std::to_string(temp_counter_++);
            }

            bool is_unit_(parus::ty::TypeId ty) const {
                return is_builtin_(types_, ty, parus::ty::Builtin::kUnit);
            }

            bool is_integer_(parus::ty::TypeId ty) const {
                const auto builtin = builtin_of_(types_, ty);
                return builtin.has_value() && is_integer_builtin_(*builtin);
            }

            bool is_float_(parus::ty::TypeId ty) const {
                const auto builtin = builtin_of_(types_, ty);
                return builtin.has_value() && is_float_builtin_(*builtin);
            }

            bool is_signed_(parus::ty::TypeId ty) const {
                const auto builtin = builtin_of_(types_, ty);
                return builtin.has_value() && is_signed_builtin_(*builtin);
            }

            std::string const_zero_(std::ostringstream& os, parus::ty::TypeId ty) {
                const auto mlir_ty = mlir_type_(types_, ty);
                const auto name = make_temp_();
                if (is_float_(ty)) {
                    os << "    " << name << " = arith.constant 0.0 : " << *mlir_ty << "\n";
                } else {
                    os << "    " << name << " = arith.constant 0 : " << *mlir_ty << "\n";
                }
                return name;
            }

            std::string const_all_ones_(std::ostringstream& os, parus::ty::TypeId ty) {
                const auto mlir_ty = mlir_type_(types_, ty);
                const auto name = make_temp_();
                os << "    " << name << " = arith.constant -1 : " << *mlir_ty << "\n";
                return name;
            }

            std::string const_bool_(std::ostringstream& os, bool value) {
                const auto name = make_temp_();
                os << "    " << name << " = arith.constant " << (value ? "true" : "false") << " : i1\n";
                return name;
            }

            void emit_inst_(std::ostringstream& os, const parus::goir::Inst& inst) {
                std::visit([&](const auto& data) {
                    using T = std::decay_t<decltype(data)>;
                    if constexpr (std::is_same_v<T, parus::goir::OpConstInt>) {
                        const auto ty = module_.values[inst.result].ty;
                        const auto mlir_ty = mlir_type_(types_, ty);
                        if (!mlir_ty.has_value()) return fail_("unsupported integer constant type for MLIR lowering");
                        os << "    " << value_name_(inst.result) << " = arith.constant "
                           << sanitize_int_literal_(data.text) << " : " << *mlir_ty << "\n";
                    } else if constexpr (std::is_same_v<T, parus::goir::OpConstFloat>) {
                        const auto ty = module_.values[inst.result].ty;
                        const auto mlir_ty = mlir_type_(types_, ty);
                        if (!mlir_ty.has_value()) return fail_("unsupported float constant type for MLIR lowering");
                        os << "    " << value_name_(inst.result) << " = arith.constant "
                           << sanitize_float_literal_(data.text) << " : " << *mlir_ty << "\n";
                    } else if constexpr (std::is_same_v<T, parus::goir::OpConstBool>) {
                        os << "    " << value_name_(inst.result) << " = arith.constant "
                           << (data.value ? "true" : "false") << " : i1\n";
                    } else if constexpr (std::is_same_v<T, parus::goir::OpConstNull>) {
                        fail_("null constants are outside the M0 MLIR lowering subset");
                    } else if constexpr (std::is_same_v<T, parus::goir::OpUnary>) {
                        const auto ty = module_.values[inst.result].ty;
                        const auto result = value_name_(inst.result);
                        const auto src = value_name_(data.src);
                        const auto mlir_ty = mlir_type_(types_, ty);
                        if (!mlir_ty.has_value()) return fail_("unsupported unary type for MLIR lowering");
                        switch (data.op) {
                            case parus::goir::UnOp::Plus:
                                value_names_[inst.result] = src;
                                break;
                            case parus::goir::UnOp::Neg: {
                                const auto zero = const_zero_(os, ty);
                                os << "    " << result << " = "
                                   << (is_float_(ty) ? "arith.subf " : "arith.subi ")
                                   << zero << ", " << src << " : " << *mlir_ty << "\n";
                                break;
                            }
                            case parus::goir::UnOp::Not: {
                                if (!is_builtin_(types_, ty, parus::ty::Builtin::kBool)) {
                                    fail_("logical not requires bool in M0 MLIR lowering");
                                    return;
                                }
                                const auto t = const_bool_(os, true);
                                os << "    " << result << " = arith.xori " << src << ", " << t << " : i1\n";
                                break;
                            }
                            case parus::goir::UnOp::BitNot: {
                                if (!is_integer_(ty)) {
                                    fail_("bitnot requires integer in M0 MLIR lowering");
                                    return;
                                }
                                const auto all_ones = const_all_ones_(os, ty);
                                os << "    " << result << " = arith.xori " << src << ", "
                                   << all_ones << " : " << *mlir_ty << "\n";
                                break;
                            }
                        }
                    } else if constexpr (std::is_same_v<T, parus::goir::OpBinary>) {
                        const auto ty = module_.values[inst.result].ty;
                        const auto result = value_name_(inst.result);
                        const auto lhs = value_name_(data.lhs);
                        const auto rhs = value_name_(data.rhs);
                        const auto mlir_ty = mlir_type_(types_, ty);
                        if (!mlir_ty.has_value()) return fail_("unsupported binary type for MLIR lowering");

                        const bool is_float = is_float_(module_.values[data.lhs].ty);
                        const bool is_int = is_integer_(module_.values[data.lhs].ty);
                        switch (data.op) {
                            case parus::goir::BinOp::Add:
                                os << "    " << result << " = " << (is_float ? "arith.addf " : "arith.addi ")
                                   << lhs << ", " << rhs << " : " << *mlir_ty << "\n";
                                break;
                            case parus::goir::BinOp::Sub:
                                os << "    " << result << " = " << (is_float ? "arith.subf " : "arith.subi ")
                                   << lhs << ", " << rhs << " : " << *mlir_ty << "\n";
                                break;
                            case parus::goir::BinOp::Mul:
                                os << "    " << result << " = " << (is_float ? "arith.mulf " : "arith.muli ")
                                   << lhs << ", " << rhs << " : " << *mlir_ty << "\n";
                                break;
                            case parus::goir::BinOp::Div:
                                if (is_float) {
                                    os << "    " << result << " = arith.divf " << lhs << ", " << rhs
                                       << " : " << *mlir_ty << "\n";
                                } else {
                                    os << "    " << result << " = "
                                       << (is_signed_(module_.values[data.lhs].ty) ? "arith.divsi " : "arith.divui ")
                                       << lhs << ", " << rhs << " : " << *mlir_ty << "\n";
                                }
                                break;
                            case parus::goir::BinOp::Rem:
                                if (is_float) {
                                    os << "    " << result << " = arith.remf " << lhs << ", " << rhs
                                       << " : " << *mlir_ty << "\n";
                                } else {
                                    os << "    " << result << " = "
                                       << (is_signed_(module_.values[data.lhs].ty) ? "arith.remsi " : "arith.remui ")
                                       << lhs << ", " << rhs << " : " << *mlir_ty << "\n";
                                }
                                break;
                            case parus::goir::BinOp::Lt:
                            case parus::goir::BinOp::Le:
                            case parus::goir::BinOp::Gt:
                            case parus::goir::BinOp::Ge:
                            case parus::goir::BinOp::Eq:
                            case parus::goir::BinOp::Ne: {
                                if (is_float) {
                                    const char* pred = "oeq";
                                    switch (data.op) {
                                        case parus::goir::BinOp::Lt: pred = "olt"; break;
                                        case parus::goir::BinOp::Le: pred = "ole"; break;
                                        case parus::goir::BinOp::Gt: pred = "ogt"; break;
                                        case parus::goir::BinOp::Ge: pred = "oge"; break;
                                        case parus::goir::BinOp::Eq: pred = "oeq"; break;
                                        case parus::goir::BinOp::Ne: pred = "one"; break;
                                        default: break;
                                    }
                                    os << "    " << result << " = arith.cmpf " << pred << ", "
                                       << lhs << ", " << rhs << " : " << *mlir_ty << "\n";
                                } else if (is_int) {
                                    const char* pred = "eq";
                                    switch (data.op) {
                                        case parus::goir::BinOp::Lt: pred = is_signed_(module_.values[data.lhs].ty) ? "slt" : "ult"; break;
                                        case parus::goir::BinOp::Le: pred = is_signed_(module_.values[data.lhs].ty) ? "sle" : "ule"; break;
                                        case parus::goir::BinOp::Gt: pred = is_signed_(module_.values[data.lhs].ty) ? "sgt" : "ugt"; break;
                                        case parus::goir::BinOp::Ge: pred = is_signed_(module_.values[data.lhs].ty) ? "sge" : "uge"; break;
                                        case parus::goir::BinOp::Eq: pred = "eq"; break;
                                        case parus::goir::BinOp::Ne: pred = "ne"; break;
                                        default: break;
                                    }
                                    os << "    " << result << " = arith.cmpi " << pred << ", "
                                       << lhs << ", " << rhs << " : " << *mlir_ty << "\n";
                                } else {
                                    fail_("comparison requires scalar integer/float operands in M0");
                                }
                                break;
                            }
                            case parus::goir::BinOp::LogicalAnd:
                                os << "    " << result << " = arith.andi " << lhs << ", " << rhs
                                   << " : " << *mlir_ty << "\n";
                                break;
                            case parus::goir::BinOp::LogicalOr:
                                os << "    " << result << " = arith.ori " << lhs << ", " << rhs
                                   << " : " << *mlir_ty << "\n";
                                break;
                        }
                    } else if constexpr (std::is_same_v<T, parus::goir::OpCast>) {
                        const auto src_ty = module_.values[data.src].ty;
                        const auto dst_ty = module_.values[inst.result].ty;
                        const auto src_name = value_name_(data.src);
                        const auto dst_name = value_name_(inst.result);
                        const auto src_mlir_ty = mlir_type_(types_, src_ty);
                        const auto dst_mlir_ty = mlir_type_(types_, dst_ty);
                        if (!src_mlir_ty.has_value() || !dst_mlir_ty.has_value()) {
                            fail_("unsupported cast type in M0 MLIR lowering");
                            return;
                        }
                        if (*src_mlir_ty == *dst_mlir_ty) {
                            value_names_[inst.result] = src_name;
                            return;
                        }
                        if (is_integer_(src_ty) && is_integer_(dst_ty)) {
                            const auto src_width = src_mlir_ty->substr(1);
                            const auto dst_width = dst_mlir_ty->substr(1);
                            if (src_width == dst_width) {
                                value_names_[inst.result] = src_name;
                            } else if (std::stoi(std::string(dst_width)) > std::stoi(std::string(src_width))) {
                                os << "    " << dst_name << " = "
                                   << (is_signed_(src_ty) ? "arith.extsi " : "arith.extui ")
                                   << src_name << " : " << *src_mlir_ty << " to " << *dst_mlir_ty << "\n";
                            } else {
                                os << "    " << dst_name << " = arith.trunci " << src_name
                                   << " : " << *src_mlir_ty << " to " << *dst_mlir_ty << "\n";
                            }
                            return;
                        }
                        if (is_integer_(src_ty) && is_float_(dst_ty)) {
                            os << "    " << dst_name << " = "
                               << (is_signed_(src_ty) ? "arith.sitofp " : "arith.uitofp ")
                               << src_name << " : " << *src_mlir_ty << " to " << *dst_mlir_ty << "\n";
                            return;
                        }
                        if (is_float_(src_ty) && is_integer_(dst_ty)) {
                            os << "    " << dst_name << " = "
                               << (is_signed_(dst_ty) ? "arith.fptosi " : "arith.fptoui ")
                               << src_name << " : " << *src_mlir_ty << " to " << *dst_mlir_ty << "\n";
                            return;
                        }
                        if (is_float_(src_ty) && is_float_(dst_ty)) {
                            const auto src_kind = builtin_of_(types_, src_ty);
                            const auto dst_kind = builtin_of_(types_, dst_ty);
                            os << "    " << dst_name << " = "
                               << ((*dst_kind == parus::ty::Builtin::kF64 && *src_kind == parus::ty::Builtin::kF32)
                                        ? "arith.extf " : "arith.truncf ")
                               << src_name << " : " << *src_mlir_ty << " to " << *dst_mlir_ty << "\n";
                            return;
                        }
                        if (is_builtin_(types_, src_ty, parus::ty::Builtin::kBool) && is_integer_(dst_ty)) {
                            os << "    " << dst_name << " = arith.extui " << src_name
                               << " : i1 to " << *dst_mlir_ty << "\n";
                            return;
                        }
                        fail_("unsupported cast combination in M0 MLIR lowering");
                    } else if constexpr (std::is_same_v<T, parus::goir::OpCallDirect>) {
                        const auto& callee = module_.realizations[data.callee];
                        const auto sig_id = module_.computations[callee.computation].sig;
                        const auto& sig = module_.semantic_sigs[sig_id];
                        std::ostringstream args{};
                        std::ostringstream arg_types{};
                        for (size_t i = 0; i < data.args.size(); ++i) {
                            if (i != 0) {
                                args << ", ";
                                arg_types << ", ";
                            }
                            args << value_name_(data.args[i]);
                            const auto mlir_ty = mlir_type_(types_, module_.values[data.args[i]].ty);
                            arg_types << (mlir_ty.has_value() ? *mlir_ty : "<invalid>");
                        }
                        const auto result_ty = mlir_type_(types_, sig.result_type);
                        if (!result_ty.has_value()) {
                            fail_("unsupported call result type in M0 MLIR lowering");
                            return;
                        }
                        if (inst.result != parus::goir::kInvalidId) {
                            os << "    " << value_name_(inst.result) << " = ";
                        } else {
                            os << "    ";
                        }
                        os << "func.call @" << module_.string(callee.name) << "(" << args.str() << ") : ("
                           << arg_types.str() << ") -> " << *result_ty << "\n";
                    } else if constexpr (std::is_same_v<T, parus::goir::OpSemanticInvoke>) {
                        fail_("placed MLIR lowering cannot see semantic invokes");
                    }
                }, inst.data);
            }

            void emit_block_term_(std::ostringstream& os, const parus::goir::Block& block) {
                std::visit([&](const auto& term) {
                    using T = std::decay_t<decltype(term)>;
                    if constexpr (std::is_same_v<T, parus::goir::TermBr>) {
                        os << "    cf.br ^bb" << term.target << "\n";
                    } else if constexpr (std::is_same_v<T, parus::goir::TermCondBr>) {
                        os << "    cf.cond_br " << value_name_(term.cond)
                           << ", ^bb" << term.then_bb << ", ^bb" << term.else_bb << "\n";
                    } else if constexpr (std::is_same_v<T, parus::goir::TermRet>) {
                        if (term.has_value) {
                            const auto mlir_ty = mlir_type_(types_, module_.values[term.value].ty);
                            os << "    func.return " << value_name_(term.value) << " : " << *mlir_ty << "\n";
                        } else {
                            os << "    func.return\n";
                        }
                    }
                }, block.term);
            }

            void emit_realization_(std::ostringstream& os,
                                   parus::goir::RealizationId rid,
                                   const parus::goir::GRealization& real) {
                const auto sig_id = module_.computations[real.computation].sig;
                const auto& sig = module_.semantic_sigs[sig_id];
                const auto result_ty = mlir_type_(types_, sig.result_type);
                if (!result_ty.has_value()) {
                    fail_("unsupported function result type in M0 MLIR lowering");
                    return;
                }

                if (real.entry == parus::goir::kInvalidId || static_cast<size_t>(real.entry) >= module_.blocks.size()) {
                    fail_("realization has invalid entry block");
                    return;
                }
                const auto& entry = module_.blocks[real.entry];
                if (entry.params.size() != sig.param_types.size()) {
                    fail_("entry block param count does not match semantic signature");
                    return;
                }

                os << "  func.func @" << module_.string(real.name) << "(";
                for (size_t i = 0; i < entry.params.size(); ++i) {
                    if (i != 0) os << ", ";
                    const auto mlir_ty = mlir_type_(types_, module_.values[entry.params[i]].ty);
                    if (!mlir_ty.has_value()) {
                        fail_("unsupported entry parameter type in M0 MLIR lowering");
                        return;
                    }
                    value_names_[entry.params[i]] = "%arg" + std::to_string(i);
                    os << value_names_[entry.params[i]] << ": " << *mlir_ty;
                }
                os << ")";
                if (!is_unit_(sig.result_type)) {
                    os << " -> " << *result_ty;
                }
                os << " {\n";

                for (size_t bi = 0; bi < real.blocks.size(); ++bi) {
                    const auto block_id = real.blocks[bi];
                    const auto& block = module_.blocks[block_id];
                    if (block_id != real.entry) {
                        if (!block.params.empty()) {
                            fail_("M0 MLIR lowering does not support non-entry block parameters");
                            return;
                        }
                        os << "  ^bb" << block_id << ":\n";
                    }
                    for (const auto iid : block.insts) {
                        emit_inst_(os, module_.insts[iid]);
                        if (failed_) return;
                    }
                    emit_block_term_(os, block);
                }
                os << "  }\n";
                (void)rid;
            }

        public:
            std::vector<CompileMessage> take_messages() { return std::move(messages_); }
        };

        ::mlir::OwningOpRef<::mlir::ModuleOp> parse_mlir_module_(const std::string& mlir_text,
                                                                 ::mlir::MLIRContext& context) {
            ::mlir::ParserConfig config(&context);
            return ::mlir::parseSourceString<::mlir::ModuleOp>(mlir_text, config, "parus-goir");
        }

    } // namespace

    GOIRLoweringResult lower_goir_to_mlir_text(
        const parus::goir::Module& module,
        const parus::ty::TypePool& types
    ) {
        MlirEmitter emitter(module, types);
        auto out = emitter.emit();
        auto extra = emitter.take_messages();
        out.messages.insert(out.messages.end(), extra.begin(), extra.end());
        out.ok = out.ok && out.messages.empty();
        return out;
    }

    GOIRLLVMIRResult lower_goir_to_llvm_ir_text(
        const parus::goir::Module& module,
        const parus::ty::TypePool& types,
        const GOIRLoweringOptions& options
    ) {
        GOIRLLVMIRResult out{};

        if (options.llvm_lane_major != 20) {
            push_error_(out.messages, "gOIR MLIR lowering requires LLVM lane 20 in this milestone.");
            return out;
        }

        auto mlir = lower_goir_to_mlir_text(module, types);
        out.mlir_text = mlir.mlir_text;
        out.messages = mlir.messages;
        if (!mlir.ok) return out;

        ::mlir::DialectRegistry registry;
        registry.insert<::mlir::func::FuncDialect,
                        ::mlir::arith::ArithDialect,
                        ::mlir::cf::ControlFlowDialect,
                        ::mlir::index::IndexDialect,
                        ::mlir::LLVM::LLVMDialect>();

        ::mlir::MLIRContext context;
        context.appendDialectRegistry(registry);
        context.loadAllAvailableDialects();

        auto parsed = parse_mlir_module_(out.mlir_text, context);
        if (!parsed) {
            push_error_(out.messages, "failed to parse generated MLIR text.");
            return out;
        }

        ::mlir::PassManager pm(&context);
        pm.addPass(::mlir::createArithToLLVMConversionPass());
        pm.addPass(::mlir::createConvertIndexToLLVMPass());
        pm.addPass(::mlir::createConvertControlFlowToLLVMPass());
        pm.addPass(::mlir::createConvertFuncToLLVMPass());
        pm.addPass(::mlir::createReconcileUnrealizedCastsPass());
        if (::mlir::failed(pm.run(parsed.get()))) {
            push_error_(out.messages, "failed to lower MLIR func/arith/cf module to LLVM dialect.");
            return out;
        }

        ::mlir::registerBuiltinDialectTranslation(context);
        ::mlir::registerLLVMDialectTranslation(context);

        llvm::LLVMContext llvm_context;
        auto llvm_module = ::mlir::translateModuleToLLVMIR(parsed.get(), llvm_context, "ParusGOIR");
        if (!llvm_module) {
            push_error_(out.messages, "failed to translate LLVM dialect module to LLVM IR.");
            return out;
        }

        std::string llvm_ir{};
        llvm::raw_string_ostream os(llvm_ir);
        llvm_module->print(os, nullptr);
        os.flush();

        out.ok = true;
        out.llvm_ir = std::move(llvm_ir);
        return out;
    }

    bool run_mlir_smoke(std::string* error_text) {
        ::mlir::DialectRegistry registry;
        registry.insert<::mlir::LLVM::LLVMDialect>();
        ::mlir::MLIRContext context;
        context.appendDialectRegistry(registry);
        context.loadAllAvailableDialects();

        const char* llvm_dialect_ir =
            "module {\n"
            "  llvm.func @main() -> i32 {\n"
            "    %0 = llvm.mlir.constant(0 : i32) : i32\n"
            "    llvm.return %0 : i32\n"
            "  }\n"
            "}\n";

        auto parsed = parse_mlir_module_(llvm_dialect_ir, context);
        if (!parsed) {
            if (error_text) *error_text = "failed to parse LLVM dialect smoke module";
            return false;
        }

        ::mlir::registerBuiltinDialectTranslation(context);
        ::mlir::registerLLVMDialectTranslation(context);

        llvm::LLVMContext llvm_context;
        auto llvm_module = ::mlir::translateModuleToLLVMIR(parsed.get(), llvm_context, "ParusMLIRSmoke");
        if (!llvm_module) {
            if (error_text) *error_text = "failed to translate LLVM dialect smoke module";
            return false;
        }

        std::string llvm_ir{};
        llvm::raw_string_ostream os(llvm_ir);
        llvm_module->print(os, nullptr);
        os.flush();
        const bool ok = llvm_ir.find("define i32 @main()") != std::string::npos;
        if (!ok && error_text) {
            *error_text = "smoke translation output did not contain main definition";
        }
        return ok;
    }

} // namespace parus::backend::mlir
