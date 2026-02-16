// backend/src/aot/LLVMIRLowering.cpp
#include <parus/backend/aot/LLVMIRLowering.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace parus::backend::aot {

    namespace {

        struct IncomingEdge {
            parus::oir::BlockId pred = parus::oir::kInvalidId;
            std::vector<parus::oir::ValueId> args{};
        };

        struct ValueUseInfo {
            bool as_value = false;
            bool as_slot = false;
        };

        struct NamedLayoutInfo {
            uint32_t size = 32;
            uint32_t align = 8;
        };

        struct TextConstantInfo {
            std::string symbol{};
            uint64_t len = 0;
            uint64_t storage_len = 0;
        };

        /// @brief 함수 이름을 LLVM 심볼 이름으로 정규화한다.
        std::string sanitize_symbol_(std::string_view in) {
            std::string out;
            out.reserve(in.size() + 4);
            for (char c : in) {
                const unsigned char u = static_cast<unsigned char>(c);
                if (std::isalnum(u) || c == '_' || c == '$' || c == '.') out.push_back(c);
                else out.push_back('_');
            }
            if (out.empty()) return "anon_fn";
            if (std::isdigit(static_cast<unsigned char>(out[0]))) out.insert(out.begin(), '_');
            return out;
        }

        /// @brief OIR 값 참조를 LLVM SSA 이름으로 변환한다.
        std::string vref_(parus::oir::ValueId v) {
            return "%v" + std::to_string(v);
        }

        /// @brief OIR 블록 참조를 LLVM 라벨 이름으로 변환한다.
        std::string bref_(parus::oir::BlockId b) {
            return "bb" + std::to_string(b);
        }

        /// @brief 문자열이 `iN` 정수 타입인지 검사한다.
        bool is_int_ty_(const std::string& ty) {
            return ty.size() >= 2 && ty[0] == 'i' &&
                   std::all_of(ty.begin() + 1, ty.end(), [](char c) { return std::isdigit(static_cast<unsigned char>(c)); });
        }

        /// @brief 정수 타입의 비트 폭을 반환한다. 실패 시 0.
        uint32_t int_bits_(const std::string& ty) {
            if (!is_int_ty_(ty)) return 0;
            try {
                return static_cast<uint32_t>(std::stoul(ty.substr(1)));
            } catch (...) {
                return 0;
            }
        }

        /// @brief 문자열이 부동소수 타입인지 검사한다.
        bool is_float_ty_(const std::string& ty) {
            return ty == "float" || ty == "double" || ty == "fp128";
        }

        /// @brief 부동소수 타입 비트 폭을 반환한다.
        uint32_t float_bits_(const std::string& ty) {
            if (ty == "float") return 32;
            if (ty == "double") return 64;
            if (ty == "fp128") return 128;
            return 0;
        }

        /// @brief 타입에 맞는 0 리터럴을 반환한다.
        std::string zero_literal_(const std::string& ty) {
            if (ty == "i1") return "false";
            if (is_int_ty_(ty)) return "0";
            if (ty == "float" || ty == "double") return "0.0";
            if (ty == "fp128") return "0xL00000000000000000000000000000000";
            if (ty == "ptr") return "null";
            if (!ty.empty() && (ty.front() == '[' || ty.front() == '{')) return "zeroinitializer";
            return "0";
        }

        /// @brief 타입에 맞는 복사/제로 생성 식을 만든다.
        std::string copy_expr_(const std::string& ty, const std::string& src_ref) {
            if (is_int_ty_(ty)) return "add " + ty + " 0, " + src_ref;
            if (is_float_ty_(ty)) return "fadd " + ty + " " + zero_literal_(ty) + ", " + src_ref;
            if (ty == "ptr") return "bitcast ptr " + src_ref + " to ptr";
            return "add i64 0, 0";
        }

        /// @brief LLVM 타입 문자열이 aggregate(구조체/배열)인지 검사한다.
        bool is_aggregate_llvm_ty_(const std::string& ty) {
            if (ty.empty()) return false;
            return ty.front() == '[' || ty.front() == '{';
        }

        /// @brief raw bytes를 LLVM c"..." 상수 리터럴 본문으로 이스케이프한다.
        std::string llvm_escape_c_bytes_(std::string_view bytes) {
            static constexpr char kHex[] = "0123456789ABCDEF";
            std::string out;
            out.reserve(bytes.size() * 4 + 4);
            for (unsigned char b : bytes) {
                if (b >= 0x20 && b <= 0x7E && b != '\\' && b != '"') {
                    out.push_back(static_cast<char>(b));
                    continue;
                }
                out.push_back('\\');
                out.push_back(kHex[(b >> 4) & 0x0F]);
                out.push_back(kHex[b & 0x0F]);
            }
            return out;
        }

        /// @brief 타입을 LLVM 타입 문자열로 재귀 변환한다.
        std::string map_type_rec_(
            const parus::ty::TypePool& types,
            parus::ty::TypeId tid,
            uint32_t depth,
            const std::unordered_map<parus::ty::TypeId, NamedLayoutInfo>* named_layouts
        ) {
            if (tid == parus::ty::kInvalidType) return "i64";
            if (depth > 8) return "i64";

            const auto& t = types.get(tid);
            using K = parus::ty::Kind;
            using B = parus::ty::Builtin;

            switch (t.kind) {
                case K::kError:
                    return "i64";

                case K::kBuiltin:
                    switch (t.builtin) {
                        case B::kUnit:  return "void";
                        case B::kNever: return "void";
                        case B::kBool:  return "i1";
                        case B::kChar:  return "i32";
                        case B::kText:  return "{ ptr, i64 }";
                        case B::kI8:    return "i8";
                        case B::kI16:   return "i16";
                        case B::kI32:   return "i32";
                        case B::kI64:   return "i64";
                        case B::kI128:  return "i128";
                        case B::kU8:    return "i8";
                        case B::kU16:   return "i16";
                        case B::kU32:   return "i32";
                        case B::kU64:   return "i64";
                        case B::kU128:  return "i128";
                        case B::kISize: return "i64";
                        case B::kUSize: return "i64";
                        case B::kF32:   return "float";
                        case B::kF64:   return "double";
                        case B::kF128:  return "fp128";
                        case B::kNull:  return "ptr";
                        case B::kInferInteger: return "i64";
                    }
                    return "i64";

                case K::kOptional: {
                    std::string elem = map_type_rec_(types, t.elem, depth + 1, named_layouts);
                    if (elem == "void") elem = "i8";
                    return "{ i1, " + elem + " }";
                }

                case K::kArray: {
                    std::string elem = map_type_rec_(types, t.elem, depth + 1, named_layouts);
                    if (elem == "void") elem = "i8";
                    if (t.array_has_size) {
                        return "[" + std::to_string(t.array_size) + " x " + elem + "]";
                    }
                    // unsized array(T[])는 런타임 view로 취급한다.
                    return "{ ptr, i64 }";
                }

                case K::kBorrow:
                case K::kEscape:
                case K::kPtr:
                case K::kFn:
                    return "ptr";

                case K::kNamedUser:
                    if (named_layouts != nullptr) {
                        auto it = named_layouts->find(tid);
                        if (it != named_layouts->end()) {
                            const uint32_t sz = std::max<uint32_t>(1u, it->second.size);
                            return "[" + std::to_string(sz) + " x i8]";
                        }
                    }
                    return "[32 x i8]";
            }

            return "i64";
        }

        /// @brief 타입 ID를 LLVM 타입 문자열로 변환한다.
        std::string map_type_(
            const parus::ty::TypePool& types,
            parus::ty::TypeId tid,
            const std::unordered_map<parus::ty::TypeId, NamedLayoutInfo>* named_layouts = nullptr
        ) {
            return map_type_rec_(types, tid, 0, named_layouts);
        }

        /// @brief 타입의 대략적 바이트 크기를 계산한다.
        uint64_t type_size_bytes_rec_(
            const parus::ty::TypePool& types,
            parus::ty::TypeId tid,
            uint32_t depth,
            const std::unordered_map<parus::ty::TypeId, NamedLayoutInfo>* named_layouts
        ) {
            if (tid == parus::ty::kInvalidType) return 8;
            if (depth > 8) return 8;

            const auto& t = types.get(tid);
            using K = parus::ty::Kind;
            using B = parus::ty::Builtin;

            switch (t.kind) {
                case K::kError:
                    return 8;
                case K::kBuiltin:
                    switch (t.builtin) {
                        case B::kBool: return 1;
                        case B::kI8:
                        case B::kU8: return 1;
                        case B::kI16:
                        case B::kU16: return 2;
                        case B::kI32:
                        case B::kU32:
                        case B::kF32:
                        case B::kChar: return 4;
                        case B::kText: return 16;
                        case B::kI64:
                        case B::kU64:
                        case B::kF64:
                        case B::kISize:
                        case B::kUSize:
                        case B::kNull:
                        case B::kInferInteger:
                        case B::kUnit:
                        case B::kNever:
                            return 8;
                        case B::kI128:
                        case B::kU128:
                        case B::kF128:
                            return 16;
                    }
                    return 8;
                case K::kOptional:
                    return std::max<uint64_t>(2, 1 + type_size_bytes_rec_(types, t.elem, depth + 1, named_layouts));
                case K::kArray: {
                    const uint64_t elem = std::max<uint64_t>(1, type_size_bytes_rec_(types, t.elem, depth + 1, named_layouts));
                    if (t.array_has_size) return elem * std::max<uint64_t>(1, t.array_size);
                    return 16; // {ptr,len}
                }
                case K::kBorrow:
                case K::kEscape:
                case K::kPtr:
                case K::kFn:
                    return 8;
                case K::kNamedUser: {
                    if (named_layouts != nullptr) {
                        auto it = named_layouts->find(tid);
                        if (it != named_layouts->end()) {
                            return std::max<uint64_t>(1u, it->second.size);
                        }
                    }
                    return 32;
                }
            }
            return 8;
        }

        /// @brief 타입의 대략적 바이트 크기를 계산한다.
        uint64_t type_size_bytes_(
            const parus::ty::TypePool& types,
            parus::ty::TypeId tid,
            const std::unordered_map<parus::ty::TypeId, NamedLayoutInfo>* named_layouts = nullptr
        ) {
            return type_size_bytes_rec_(types, tid, 0, named_layouts);
        }

        /// @brief OIR 모듈에서 value 사용 문맥(값/슬롯)을 수집한다.
        std::vector<ValueUseInfo> build_value_use_table_(const parus::oir::Module& m) {
            std::vector<ValueUseInfo> uses(m.values.size());
            auto as_value = [&](parus::oir::ValueId v) {
                if (v == parus::oir::kInvalidId || static_cast<size_t>(v) >= uses.size()) return;
                uses[v].as_value = true;
            };
            auto as_slot = [&](parus::oir::ValueId v) {
                if (v == parus::oir::kInvalidId || static_cast<size_t>(v) >= uses.size()) return;
                uses[v].as_slot = true;
            };

            for (const auto& inst : m.insts) {
                std::visit([&](auto&& x) {
                    using T = std::decay_t<decltype(x)>;
                    if constexpr (std::is_same_v<T, parus::oir::InstUnary>) {
                        as_value(x.src);
                    } else if constexpr (std::is_same_v<T, parus::oir::InstBinOp>) {
                        as_value(x.lhs);
                        as_value(x.rhs);
                    } else if constexpr (std::is_same_v<T, parus::oir::InstCast>) {
                        as_value(x.src);
                    } else if constexpr (std::is_same_v<T, parus::oir::InstCall>) {
                        // callee는 direct-call(direct_callee)로 소거될 수 있으므로
                        // 간접 경로일 때만 일반 값 사용(as_value)으로 표시한다.
                        if (x.direct_callee == parus::oir::kInvalidId) {
                            as_value(x.callee);
                        }
                        for (auto a : x.args) as_value(a);
                    } else if constexpr (std::is_same_v<T, parus::oir::InstIndex>) {
                        as_value(x.base);
                        as_value(x.index);
                    } else if constexpr (std::is_same_v<T, parus::oir::InstField>) {
                        as_value(x.base);
                    } else if constexpr (std::is_same_v<T, parus::oir::InstLoad>) {
                        as_slot(x.slot);
                    } else if constexpr (std::is_same_v<T, parus::oir::InstStore>) {
                        as_slot(x.slot);
                        as_value(x.value);
                    } else if constexpr (std::is_same_v<T, parus::oir::InstConstInt> ||
                                         std::is_same_v<T, parus::oir::InstConstBool> ||
                                         std::is_same_v<T, parus::oir::InstConstText> ||
                                         std::is_same_v<T, parus::oir::InstConstNull> ||
                                         std::is_same_v<T, parus::oir::InstGlobalRef> ||
                                         std::is_same_v<T, parus::oir::InstAllocaLocal>) {
                        // no operand
                    }
                }, inst.data);
            }

            for (const auto& b : m.blocks) {
                if (!b.has_term) continue;
                std::visit([&](auto&& t) {
                    using T = std::decay_t<decltype(t)>;
                    if constexpr (std::is_same_v<T, parus::oir::TermRet>) {
                        if (t.has_value) as_value(t.value);
                    } else if constexpr (std::is_same_v<T, parus::oir::TermBr>) {
                        for (auto a : t.args) as_value(a);
                    } else if constexpr (std::is_same_v<T, parus::oir::TermCondBr>) {
                        as_value(t.cond);
                        for (auto a : t.then_args) as_value(a);
                        for (auto a : t.else_args) as_value(a);
                    }
                }, b.term);
            }

            return uses;
        }

        /// @brief 정수 리터럴 텍스트에서 숫자 부분만 추출한다.
        std::string parse_int_literal_(std::string_view text) {
            std::string out;
            out.reserve(text.size());

            size_t i = 0;
            if (!text.empty() && (text[0] == '+' || text[0] == '-')) {
                out.push_back(text[0]);
                i = 1;
            }

            bool saw_digit = false;
            for (; i < text.size(); ++i) {
                const char c = text[i];
                if (std::isdigit(static_cast<unsigned char>(c))) {
                    out.push_back(c);
                    saw_digit = true;
                    continue;
                }
                if (c == '_') continue;
                break;
            }

            if (!saw_digit) return "0";
            return out;
        }

        /// @brief Value 타입 테이블을 만든다.
        std::vector<std::string> build_value_type_table_(
            const parus::oir::Module& m,
            const parus::ty::TypePool& types,
            const std::unordered_map<parus::ty::TypeId, NamedLayoutInfo>& named_layouts
        ) {
            std::vector<std::string> out(m.values.size(), "i64");
            for (size_t i = 0; i < m.values.size(); ++i) {
                out[i] = map_type_(types, m.values[i].ty, &named_layouts);
                if (out[i] == "void") {
                    // LLVM SSA value는 void 타입을 가질 수 없다.
                    out[i] = "i8";
                }
                if (is_aggregate_llvm_ty_(out[i])) {
                    // SSA value 레벨에서는 aggregate 직접 연산을 피하고 주소/핸들(ptr)로 표현한다.
                    out[i] = "ptr";
                }
            }

            for (const auto& inst : m.insts) {
                if (inst.result == parus::oir::kInvalidId || static_cast<size_t>(inst.result) >= out.size()) continue;

                if (std::holds_alternative<parus::oir::InstAllocaLocal>(inst.data)) {
                    out[inst.result] = "ptr";
                    continue;
                }
                if (std::holds_alternative<parus::oir::InstGlobalRef>(inst.data)) {
                    out[inst.result] = "ptr";
                    continue;
                }
                if (std::holds_alternative<parus::oir::InstConstBool>(inst.data)) {
                    out[inst.result] = "i1";
                    continue;
                }
                if (std::holds_alternative<parus::oir::InstBinOp>(inst.data)) {
                    const auto& bo = std::get<parus::oir::InstBinOp>(inst.data);
                    using O = parus::oir::BinOp;
                    if (bo.op == O::Lt || bo.op == O::Le || bo.op == O::Gt ||
                        bo.op == O::Ge || bo.op == O::Eq || bo.op == O::Ne) {
                        out[inst.result] = "i1";
                    }
                    continue;
                }
                if (std::holds_alternative<parus::oir::InstCast>(inst.data)) {
                    const auto& c = std::get<parus::oir::InstCast>(inst.data);
                    out[inst.result] = map_type_(types, c.to, &named_layouts);
                    if (is_aggregate_llvm_ty_(out[inst.result])) out[inst.result] = "ptr";
                }
            }
            return out;
        }

        /// @brief lowering 전제: edge incoming 타입/개수가 block param과 일치하는지 검사한다.
        std::vector<std::string> verify_phi_incoming_contract_(
            const parus::oir::Module& m,
            const std::vector<std::string>& value_types
        ) {
            using namespace parus::oir;
            std::vector<std::string> errs;

            for (const auto& fn : m.funcs) {
                std::unordered_set<BlockId> owned;
                for (auto bb : fn.blocks) owned.insert(bb);

                auto check_edge = [&](BlockId pred, BlockId target, const std::vector<ValueId>& args) {
                    if (target == kInvalidId || static_cast<size_t>(target) >= m.blocks.size()) return;
                    if (!owned.contains(target)) return;
                    const auto& tb = m.blocks[target];

                    if (args.size() != tb.params.size()) {
                        errs.push_back(
                            "phi incoming arity mismatch in function '" + fn.name +
                            "': pred bb" + std::to_string(pred) +
                            " -> bb" + std::to_string(target) +
                            " has " + std::to_string(args.size()) +
                            " args, but target expects " + std::to_string(tb.params.size())
                        );
                    }

                    const uint32_t n = std::min<uint32_t>(
                        static_cast<uint32_t>(args.size()),
                        static_cast<uint32_t>(tb.params.size())
                    );
                    for (uint32_t i = 0; i < n; ++i) {
                        const auto arg = args[i];
                        const auto param = tb.params[i];
                        if (arg == kInvalidId || param == kInvalidId) continue;
                        if (static_cast<size_t>(arg) >= value_types.size()) continue;
                        if (static_cast<size_t>(param) >= value_types.size()) continue;
                        if (value_types[arg] == value_types[param]) continue;

                        errs.push_back(
                            "phi incoming type mismatch in function '" + fn.name +
                            "': pred bb" + std::to_string(pred) +
                            " -> bb" + std::to_string(target) +
                            ", idx " + std::to_string(i) +
                            ", arg type '" + value_types[arg] +
                            "' != param type '" + value_types[param] + "'"
                        );
                    }
                };

                for (auto bb : fn.blocks) {
                    if (bb == kInvalidId || static_cast<size_t>(bb) >= m.blocks.size()) continue;
                    const auto& b = m.blocks[bb];
                    if (!b.has_term) continue;

                    std::visit([&](auto&& t) {
                        using T = std::decay_t<decltype(t)>;
                        if constexpr (std::is_same_v<T, TermBr>) {
                            check_edge(bb, t.target, t.args);
                        } else if constexpr (std::is_same_v<T, TermCondBr>) {
                            check_edge(bb, t.then_bb, t.then_args);
                            check_edge(bb, t.else_bb, t.else_args);
                        } else if constexpr (std::is_same_v<T, TermRet>) {
                            // no edge
                        }
                    }, b.term);
                }
            }

            return errs;
        }

        /// @brief OIR 함수 하나를 LLVM-IR 함수 텍스트로 변환한다.
        class FunctionEmitter {
        public:
            FunctionEmitter(
                const parus::oir::Module& m,
                const parus::ty::TypePool& types,
                const parus::oir::Function& fn,
                const std::vector<std::string>& value_types,
                const std::vector<ValueUseInfo>& value_uses,
                const std::unordered_map<parus::ty::TypeId, NamedLayoutInfo>& named_layouts,
                const std::unordered_map<parus::ty::TypeId, std::unordered_map<std::string, uint32_t>>& field_offsets,
                const std::unordered_map<parus::oir::InstId, TextConstantInfo>& text_constants
            ) : m_(m),
                types_(types),
                fn_(fn),
                value_types_(value_types),
                value_uses_(value_uses),
                named_layouts_(named_layouts),
                field_offsets_(field_offsets),
                text_constants_(text_constants) {
                for (auto bb : fn_.blocks) owned_blocks_.insert(bb);
                build_incomings_();
            }

            /// @brief 함수 본문을 생성한다.
            std::string emit(bool& need_call_stub) {
                std::ostringstream os;
                const std::string ret_ty = map_type_(types_, fn_.ret_ty, &named_layouts_);
                const std::string sym = sanitize_symbol_(fn_.name);

                // extern 함수는 본문 없이 선언으로만 내린다.
                if (fn_.is_extern) {
                    os << "declare " << ret_ty << " @" << sym << "(";
                    if (fn_.entry != parus::oir::kInvalidId &&
                        static_cast<size_t>(fn_.entry) < m_.blocks.size()) {
                        const auto& entry = m_.blocks[fn_.entry];
                        for (size_t i = 0; i < entry.params.size(); ++i) {
                            if (i) os << ", ";
                            os << abi_value_ty_(entry.params[i], fn_.abi);
                        }
                    }
                    os << ")\n";
                    need_call_stub = need_call_stub || need_call_stub_;
                    return os.str();
                }

                os << "define " << ret_ty << " @" << sym << "(";
                if (fn_.entry != parus::oir::kInvalidId &&
                    static_cast<size_t>(fn_.entry) < m_.blocks.size()) {
                    const auto& entry = m_.blocks[fn_.entry];
                    for (size_t i = 0; i < entry.params.size(); ++i) {
                        if (i) os << ", ";
                        os << abi_value_ty_(entry.params[i], fn_.abi) << " %arg" << i;
                    }
                }
                os << ")";
                if (fn_.is_pure || fn_.is_comptime) {
                    os << " nounwind";
                }
                if (fn_.is_pure) {
                    os << " willreturn";
                }
                os << " {\n";

                for (auto bb : fn_.blocks) {
                    if (bb == parus::oir::kInvalidId || static_cast<size_t>(bb) >= m_.blocks.size()) continue;
                    const auto& block = m_.blocks[bb];

                    os << bref_(bb) << ":\n";
                    emit_block_params_(os, bb, block);
                    emit_insts_(os, block);
                    emit_term_(os, ret_ty, block);
                    os << "\n";
                }

                os << "}\n";
                need_call_stub = need_call_stub || need_call_stub_;
                return os.str();
            }

        private:
            const parus::oir::Module& m_;
            const parus::ty::TypePool& types_;
            const parus::oir::Function& fn_;
            const std::vector<std::string>& value_types_;
            const std::vector<ValueUseInfo>& value_uses_;
            const std::unordered_map<parus::ty::TypeId, NamedLayoutInfo>& named_layouts_;
            const std::unordered_map<parus::ty::TypeId, std::unordered_map<std::string, uint32_t>>& field_offsets_;
            const std::unordered_map<parus::oir::InstId, TextConstantInfo>& text_constants_;

            std::unordered_set<parus::oir::BlockId> owned_blocks_{};
            std::unordered_map<parus::oir::BlockId, std::vector<IncomingEdge>> incomings_{};
            std::unordered_map<parus::oir::ValueId, std::string> address_ref_by_value_{};
            std::unordered_map<uint64_t, uint64_t> field_offset_cache_{};
            std::unordered_map<parus::ty::TypeId, uint64_t> next_field_offset_{};
            uint32_t temp_seq_ = 0;
            bool need_call_stub_ = false;

            struct DirectCalleeInfo {
                std::string symbol{};
                std::string ret_ty{};
                std::vector<std::string> param_tys{};
                parus::oir::FunctionAbi abi = parus::oir::FunctionAbi::Parus;
            };

            /// @brief 새 임시 SSA 이름을 생성한다.
            std::string next_tmp_() {
                return "%tmp" + std::to_string(temp_seq_++);
            }

            /// @brief 특정 값의 LLVM 타입 문자열을 조회한다.
            std::string value_ty_(parus::oir::ValueId v) const {
                if (v == parus::oir::kInvalidId) return "i64";
                if (static_cast<size_t>(v) >= value_types_.size()) return "i64";
                return value_types_[v];
            }

            /// @brief 값의 함수 ABI 관점 LLVM 타입을 계산한다.
            std::string abi_value_ty_(parus::oir::ValueId v, parus::oir::FunctionAbi abi) const {
                if (abi != parus::oir::FunctionAbi::C) {
                    return value_ty_(v);
                }

                const auto tid = value_type_id_(v);
                std::string ty = map_type_(types_, tid, &named_layouts_);
                if (ty == "void") ty = "i8";
                return ty;
            }

            /// @brief ValueId의 타입 ID를 반환한다.
            parus::ty::TypeId value_type_id_(parus::oir::ValueId v) const {
                if (v == parus::oir::kInvalidId || static_cast<size_t>(v) >= m_.values.size()) {
                    return parus::ty::kInvalidType;
                }
                return m_.values[v].ty;
            }

            /// @brief field base 값의 타입 ID를 보수적으로 추론한다.
            parus::ty::TypeId field_base_type_id_(parus::oir::ValueId base) const {
                using namespace parus::oir;

                const auto tid = value_type_id_(base);
                if (tid != parus::ty::kInvalidType) {
                    const auto& t = types_.get(tid);
                    if (!(t.kind == parus::ty::Kind::kBuiltin &&
                          t.builtin == parus::ty::Builtin::kNull)) {
                        return tid;
                    }
                }

                if (base == kInvalidId || static_cast<size_t>(base) >= m_.values.size()) {
                    return tid;
                }
                const auto& bv = m_.values[base];
                if (bv.def_b != kInvalidId) {
                    return tid;
                }
                if (bv.def_a == kInvalidId || static_cast<size_t>(bv.def_a) >= m_.insts.size()) {
                    return tid;
                }
                const auto& def_inst = m_.insts[bv.def_a];
                const auto* gr = std::get_if<InstGlobalRef>(&def_inst.data);
                if (gr == nullptr) {
                    return tid;
                }
                if (static_cast<size_t>(gr->global) >= m_.globals.size()) {
                    return tid;
                }
                return m_.globals[gr->global].type;
            }

            /// @brief 값이 일반 값 문맥에서 읽히는지 검사한다.
            bool is_value_read_(parus::oir::ValueId v) const {
                if (v == parus::oir::kInvalidId || static_cast<size_t>(v) >= value_uses_.size()) return false;
                return value_uses_[v].as_value;
            }

            /// @brief 슬롯 주소 문맥에서 사용되는지 검사한다.
            bool is_value_slot_(parus::oir::ValueId v) const {
                if (v == parus::oir::kInvalidId || static_cast<size_t>(v) >= value_uses_.size()) return false;
                return value_uses_[v].as_slot;
            }

            /// @brief InstCall이 direct callee를 가지면 함수 ID를 우선으로 direct-call 메타를 추출한다.
            std::optional<DirectCalleeInfo> resolve_direct_callee_(const parus::oir::InstCall& call) const {
                using namespace parus::oir;
                FuncId target_fid = call.direct_callee;
                std::string forced_symbol{};

                if (target_fid == kInvalidId) {
                    const auto callee = call.callee;
                    if (callee == kInvalidId || static_cast<size_t>(callee) >= m_.values.size()) {
                        return std::nullopt;
                    }
                    const auto& cv = m_.values[callee];
                    if (cv.def_a == kInvalidId || static_cast<size_t>(cv.def_a) >= m_.insts.size()) {
                        return std::nullopt;
                    }

                    const auto& def_inst = m_.insts[cv.def_a];
                    const auto* fr = std::get_if<InstFuncRef>(&def_inst.data);
                    if (fr == nullptr) {
                        return std::nullopt;
                    }
                    target_fid = fr->func;
                    forced_symbol = fr->name;
                }

                if (target_fid == kInvalidId || static_cast<size_t>(target_fid) >= m_.funcs.size()) {
                    return std::nullopt;
                }

                const auto& target = m_.funcs[target_fid];
                DirectCalleeInfo info{};
                info.symbol = sanitize_symbol_(forced_symbol.empty() ? target.name : forced_symbol);
                info.ret_ty = map_type_(types_, target.ret_ty, &named_layouts_);
                info.abi = target.abi;

                if (target.entry != kInvalidId && static_cast<size_t>(target.entry) < m_.blocks.size()) {
                    const auto& entry = m_.blocks[target.entry];
                    info.param_tys.reserve(entry.params.size());
                    for (auto p : entry.params) {
                        info.param_tys.push_back(abi_value_ty_(p, target.abi));
                    }
                }

                return info;
            }

            /// @brief slot operand를 ptr SSA ref로 정규화한다.
            std::string slot_ptr_ref_(std::ostringstream& os, parus::oir::ValueId slot) {
                auto it = address_ref_by_value_.find(slot);
                if (it != address_ref_by_value_.end()) return it->second;
                return coerce_value_(os, slot, "ptr");
            }

            /// @brief field 오프셋(바이트)을 type+field 조합 기준으로 결정한다.
            uint64_t field_offset_bytes_(parus::ty::TypeId base_ty, std::string_view field) {
                parus::ty::TypeId lookup_ty = base_ty;
                if (lookup_ty != parus::ty::kInvalidType) {
                    const auto& t = types_.get(lookup_ty);
                    if ((t.kind == parus::ty::Kind::kPtr ||
                         t.kind == parus::ty::Kind::kBorrow ||
                         t.kind == parus::ty::Kind::kEscape) &&
                        t.elem != parus::ty::kInvalidType) {
                        lookup_ty = t.elem;
                    }
                }

                auto fit = field_offsets_.find(lookup_ty);
                if (fit != field_offsets_.end()) {
                    auto oit = fit->second.find(std::string(field));
                    if (oit != fit->second.end()) {
                        return static_cast<uint64_t>(oit->second);
                    }
                }

                const uint64_t h = std::hash<std::string_view>{}(field);
                const uint64_t key = (uint64_t(lookup_ty) << 32u) ^ (h & 0xFFFF'FFFFu);

                auto it = field_offset_cache_.find(key);
                if (it != field_offset_cache_.end()) return it->second;

                uint64_t& next = next_field_offset_[lookup_ty];
                const uint64_t off = next;
                next += 8; // v0: 필드 정렬을 8바이트 단위로 고정
                field_offset_cache_[key] = off;
                return off;
            }

            /// @brief SSA 참조(ref, cur_ty)를 want 타입으로 강제 변환한다.
            std::string coerce_ref_(
                std::ostringstream& os,
                const std::string& ref,
                const std::string& cur,
                const std::string& want
            ) {
                if (cur == want) return ref;

                const std::string tmp = next_tmp_();
                const bool cur_is_agg = is_aggregate_llvm_ty_(cur);
                const bool want_is_agg = is_aggregate_llvm_ty_(want);

                // ABI bridge:
                // - aggregate value <-> ptr 변환은 alloca/store/load로 물질화한다.
                if (cur_is_agg && want == "ptr") {
                    const std::string agg_slot = next_tmp_();
                    os << "  " << agg_slot << " = alloca " << cur << "\n";
                    os << "  store " << cur << " " << ref << ", ptr " << agg_slot << "\n";
                    os << "  " << tmp << " = bitcast ptr " << agg_slot << " to ptr\n";
                    return tmp;
                }
                if (want_is_agg && cur == "ptr") {
                    os << "  " << tmp << " = load " << want << ", ptr " << ref << "\n";
                    return tmp;
                }
                if (want_is_agg) {
                    // 보수적 fallback: 원하는 aggregate zero-init value를 생성한다.
                    const std::string agg_slot = next_tmp_();
                    os << "  " << agg_slot << " = alloca " << want << "\n";
                    os << "  store " << want << " zeroinitializer, ptr " << agg_slot << "\n";
                    os << "  " << tmp << " = load " << want << ", ptr " << agg_slot << "\n";
                    return tmp;
                }

                if (want == "i1") {
                    if (cur == "ptr") {
                        os << "  " << tmp << " = icmp ne ptr " << ref << ", null\n";
                        return tmp;
                    }
                    if (is_int_ty_(cur)) {
                        os << "  " << tmp << " = icmp ne " << cur << " " << ref << ", 0\n";
                        return tmp;
                    }
                    if (is_float_ty_(cur)) {
                        os << "  " << tmp << " = fcmp une " << cur << " " << ref << ", " << zero_literal_(cur) << "\n";
                        return tmp;
                    }
                }

                if (is_int_ty_(want) && is_int_ty_(cur)) {
                    const uint32_t wb = int_bits_(want);
                    const uint32_t cb = int_bits_(cur);
                    if (cb < wb) os << "  " << tmp << " = zext " << cur << " " << ref << " to " << want << "\n";
                    else if (cb > wb) os << "  " << tmp << " = trunc " << cur << " " << ref << " to " << want << "\n";
                    else os << "  " << tmp << " = add " << want << " 0, " << ref << "\n";
                    return tmp;
                }

                if (cur == "ptr" && is_int_ty_(want)) {
                    os << "  " << tmp << " = ptrtoint ptr " << ref << " to " << want << "\n";
                    return tmp;
                }
                if (is_int_ty_(cur) && want == "ptr") {
                    os << "  " << tmp << " = inttoptr " << cur << " " << ref << " to ptr\n";
                    return tmp;
                }

                if (is_float_ty_(want) && is_float_ty_(cur)) {
                    const uint32_t wb = float_bits_(want);
                    const uint32_t cb = float_bits_(cur);
                    if (cb < wb) os << "  " << tmp << " = fpext " << cur << " " << ref << " to " << want << "\n";
                    else if (cb > wb) os << "  " << tmp << " = fptrunc " << cur << " " << ref << " to " << want << "\n";
                    else os << "  " << tmp << " = fadd " << want << " " << zero_literal_(want) << ", " << ref << "\n";
                    return tmp;
                }

                if (is_float_ty_(want) && is_int_ty_(cur)) {
                    os << "  " << tmp << " = sitofp " << cur << " " << ref << " to " << want << "\n";
                    return tmp;
                }
                if (is_int_ty_(want) && is_float_ty_(cur)) {
                    os << "  " << tmp << " = fptosi " << cur << " " << ref << " to " << want << "\n";
                    return tmp;
                }

                if (want == "ptr" && cur == "ptr") {
                    os << "  " << tmp << " = bitcast ptr " << ref << " to ptr\n";
                    return tmp;
                }

                // 보수적 fallback: 원하는 타입의 zero 값을 만든다.
                if (is_int_ty_(want)) {
                    os << "  " << tmp << " = add " << want << " 0, 0\n";
                    return tmp;
                }
                if (is_float_ty_(want)) {
                    os << "  " << tmp << " = fadd " << want << " " << zero_literal_(want) << ", " << zero_literal_(want) << "\n";
                    return tmp;
                }
                if (want == "ptr") {
                    os << "  " << tmp << " = getelementptr i8, ptr null, i64 0\n";
                    return tmp;
                }

                os << "  " << tmp << " = add i64 0, 0\n";
                return tmp;
            }

            /// @brief src 값을 want 타입으로 강제 변환한 SSA ref를 반환한다.
            std::string coerce_value_(
                std::ostringstream& os,
                parus::oir::ValueId src,
                const std::string& want
            ) {
                const std::string cur = value_ty_(src);
                const std::string ref = vref_(src);
                return coerce_ref_(os, ref, cur, want);
            }

            /// @brief 블록 인자(phi)를 출력한다.
            void emit_block_params_(
                std::ostringstream& os,
                parus::oir::BlockId bb,
                const parus::oir::Block& block
            ) {
                // 함수 entry 블록 파라미터는 LLVM 함수 인자로 직접 seed한다.
                if (bb == fn_.entry) {
                    for (size_t i = 0; i < block.params.size(); ++i) {
                        const auto p = block.params[i];
                        const std::string pty = value_ty_(p);
                        const std::string aty = abi_value_ty_(p, fn_.abi);
                        const std::string arg = "%arg" + std::to_string(i);

                        if (pty == aty) {
                            os << "  " << vref_(p) << " = " << copy_expr_(pty, arg) << "\n";
                            continue;
                        }

                        const std::string seeded = coerce_ref_(os, arg, aty, pty);
                        if (seeded == vref_(p)) continue;
                        os << "  " << vref_(p) << " = " << copy_expr_(pty, seeded) << "\n";
                    }
                    return;
                }

                auto it = incomings_.find(bb);
                for (size_t i = 0; i < block.params.size(); ++i) {
                    const auto p = block.params[i];
                    const std::string pty = value_ty_(p);

                    std::vector<std::string> incoming_texts;
                    if (it != incomings_.end()) {
                        for (const auto& edge : it->second) {
                            if (i >= edge.args.size()) continue;
                            const auto arg = edge.args[i];
                            // phi는 블록 맨 앞에 연속으로 위치해야 하므로
                            // edge-cast 정규화된 입력만 사용한다.
                            std::string arg_ref = vref_(arg);
                            incoming_texts.push_back("[ " + arg_ref + ", %" + bref_(edge.pred) + " ]");
                        }
                    }

                    if (!incoming_texts.empty()) {
                        os << "  " << vref_(p) << " = phi " << pty << " ";
                        for (size_t k = 0; k < incoming_texts.size(); ++k) {
                            if (k) os << ", ";
                            os << incoming_texts[k];
                        }
                        os << "\n";
                    } else {
                        if (is_int_ty_(pty)) os << "  " << vref_(p) << " = add " << pty << " 0, 0\n";
                        else if (is_float_ty_(pty)) os << "  " << vref_(p) << " = fadd " << pty << " " << zero_literal_(pty) << ", " << zero_literal_(pty) << "\n";
                        else if (pty == "ptr") os << "  " << vref_(p) << " = getelementptr i8, ptr null, i64 0\n";
                        else os << "  " << vref_(p) << " = add i64 0, 0\n";
                    }
                }
            }

            /// @brief index 연산을 실제 주소 계산 + load/store 재사용 모델로 낮춘다.
            void emit_index_(
                std::ostringstream& os,
                const parus::oir::Inst& inst,
                const parus::oir::InstIndex& x
            ) {
                using namespace parus::oir;
                if (inst.result == kInvalidId) return;

                const auto base_ptr = slot_ptr_ref_(os, x.base);
                const auto idx64 = coerce_value_(os, x.index, "i64");

                const auto elem_ty_id = value_type_id_(inst.result);
                const uint64_t elem_size = std::max<uint64_t>(1, type_size_bytes_(types_, elem_ty_id, &named_layouts_));

                std::string byte_off = idx64;
                if (elem_size != 1) {
                    const std::string mul_tmp = next_tmp_();
                    os << "  " << mul_tmp << " = mul i64 " << idx64 << ", " << elem_size << "\n";
                    byte_off = mul_tmp;
                }

                const std::string byte_ptr = next_tmp_();
                os << "  " << byte_ptr << " = getelementptr i8, ptr " << base_ptr << ", i64 " << byte_off << "\n";

                const std::string typed_ptr = next_tmp_();
                os << "  " << typed_ptr << " = bitcast ptr " << byte_ptr << " to ptr\n";
                address_ref_by_value_[inst.result] = typed_ptr;

                const std::string rty = value_ty_(inst.result);
                if (rty == "ptr") {
                    os << "  " << vref_(inst.result) << " = bitcast ptr " << typed_ptr << " to ptr\n";
                    return;
                }

                if (is_value_read_(inst.result)) {
                    os << "  " << vref_(inst.result) << " = load " << rty << ", ptr " << typed_ptr << "\n";
                    return;
                }

                if (is_value_slot_(inst.result)) {
                    // 슬롯 문맥 전용 결과는 주소 맵(address_ref_by_value_)만 있으면 충분하다.
                    // 불필요한 ptrtoint 물질화를 피해서 hot-path IR 노이즈를 줄인다.
                    if (is_int_ty_(rty)) {
                        os << "  " << vref_(inst.result) << " = add " << rty << " 0, 0\n";
                    } else if (is_float_ty_(rty)) {
                        os << "  " << vref_(inst.result) << " = fadd " << rty << " " << zero_literal_(rty)
                           << ", " << zero_literal_(rty) << "\n";
                    } else {
                        os << "  " << vref_(inst.result) << " = add i64 0, 0\n";
                    }
                    return;
                }

                if (is_int_ty_(rty)) {
                    os << "  " << vref_(inst.result) << " = ptrtoint ptr " << typed_ptr << " to " << rty << "\n";
                } else if (is_float_ty_(rty)) {
                    os << "  " << vref_(inst.result) << " = fadd " << rty << " " << zero_literal_(rty)
                       << ", " << zero_literal_(rty) << "\n";
                } else {
                    os << "  " << vref_(inst.result) << " = add i64 0, 0\n";
                }
            }

            /// @brief field 연산을 실제 주소 계산 + load/store 재사용 모델로 낮춘다.
            void emit_field_(
                std::ostringstream& os,
                const parus::oir::Inst& inst,
                const parus::oir::InstField& x
            ) {
                using namespace parus::oir;
                if (inst.result == kInvalidId) return;

                const auto base_ptr = slot_ptr_ref_(os, x.base);
                const auto base_ty_id = field_base_type_id_(x.base);
                const uint64_t field_off = field_offset_bytes_(base_ty_id, x.field);

                const std::string byte_ptr = next_tmp_();
                os << "  " << byte_ptr << " = getelementptr i8, ptr " << base_ptr << ", i64 " << field_off << "\n";

                const std::string typed_ptr = next_tmp_();
                os << "  " << typed_ptr << " = bitcast ptr " << byte_ptr << " to ptr\n";
                address_ref_by_value_[inst.result] = typed_ptr;

                const std::string rty = value_ty_(inst.result);
                if (rty == "ptr") {
                    os << "  " << vref_(inst.result) << " = bitcast ptr " << typed_ptr << " to ptr\n";
                    return;
                }

                if (is_value_read_(inst.result)) {
                    os << "  " << vref_(inst.result) << " = load " << rty << ", ptr " << typed_ptr << "\n";
                    return;
                }

                if (is_value_slot_(inst.result)) {
                    // 슬롯 문맥 전용 결과는 주소 맵(address_ref_by_value_)만 있으면 충분하다.
                    if (is_int_ty_(rty)) {
                        os << "  " << vref_(inst.result) << " = add " << rty << " 0, 0\n";
                    } else if (is_float_ty_(rty)) {
                        os << "  " << vref_(inst.result) << " = fadd " << rty << " " << zero_literal_(rty)
                           << ", " << zero_literal_(rty) << "\n";
                    } else {
                        os << "  " << vref_(inst.result) << " = add i64 0, 0\n";
                    }
                    return;
                }

                if (is_int_ty_(rty)) {
                    os << "  " << vref_(inst.result) << " = ptrtoint ptr " << typed_ptr << " to " << rty << "\n";
                } else if (is_float_ty_(rty)) {
                    os << "  " << vref_(inst.result) << " = fadd " << rty << " " << zero_literal_(rty)
                       << ", " << zero_literal_(rty) << "\n";
                } else {
                    os << "  " << vref_(inst.result) << " = add i64 0, 0\n";
                }
            }

            /// @brief text 상수를 `{ptr,len}` 헤더 슬롯으로 물질화한다.
            void emit_const_text_(
                std::ostringstream& os,
                parus::oir::InstId iid,
                const parus::oir::Inst& inst
            ) {
                using namespace parus::oir;
                if (inst.result == kInvalidId) return;

                const auto it = text_constants_.find(iid);
                const auto tid = value_type_id_(inst.result);
                const std::string text_ty = map_type_(types_, tid, &named_layouts_);

                const std::string slot = next_tmp_();
                os << "  " << slot << " = alloca " << text_ty << "\n";

                const std::string data_gep = next_tmp_();
                os << "  " << data_gep << " = getelementptr " << text_ty
                   << ", ptr " << slot << ", i32 0, i32 0\n";

                if (it != text_constants_.end()) {
                    const auto& info = it->second;
                    const std::string data_ptr = next_tmp_();
                    os << "  " << data_ptr << " = getelementptr ["
                       << info.storage_len << " x i8], ptr @" << info.symbol
                       << ", i32 0, i32 0\n";
                    os << "  store ptr " << data_ptr << ", ptr " << data_gep << "\n";

                    const std::string len_gep = next_tmp_();
                    os << "  " << len_gep << " = getelementptr " << text_ty
                       << ", ptr " << slot << ", i32 0, i32 1\n";
                    os << "  store i64 " << info.len << ", ptr " << len_gep << "\n";
                } else {
                    os << "  store ptr null, ptr " << data_gep << "\n";
                    const std::string len_gep = next_tmp_();
                    os << "  " << len_gep << " = getelementptr " << text_ty
                       << ", ptr " << slot << ", i32 0, i32 1\n";
                    os << "  store i64 0, ptr " << len_gep << "\n";
                }

                address_ref_by_value_[inst.result] = slot;

                const std::string rty = value_ty_(inst.result);
                if (rty == "ptr") {
                    os << "  " << vref_(inst.result) << " = bitcast ptr " << slot << " to ptr\n";
                    return;
                }
                if (is_value_read_(inst.result)) {
                    os << "  " << vref_(inst.result) << " = load " << rty << ", ptr " << slot << "\n";
                    return;
                }
                if (is_int_ty_(rty)) {
                    os << "  " << vref_(inst.result) << " = add " << rty << " 0, 0\n";
                } else if (is_float_ty_(rty)) {
                    os << "  " << vref_(inst.result) << " = fadd " << rty << " " << zero_literal_(rty)
                       << ", " << zero_literal_(rty) << "\n";
                } else {
                    os << "  " << vref_(inst.result) << " = add i64 0, 0\n";
                }
            }

            /// @brief 명령들을 LLVM-IR 문장으로 출력한다.
            void emit_insts_(std::ostringstream& os, const parus::oir::Block& block) {
                using namespace parus::oir;

                for (const auto iid : block.insts) {
                    if (static_cast<size_t>(iid) >= m_.insts.size()) continue;
                    const auto& inst = m_.insts[iid];

                    std::visit([&](auto&& x) {
                        using T = std::decay_t<decltype(x)>;

                        if constexpr (std::is_same_v<T, InstConstInt>) {
                            if (inst.result == kInvalidId) return;
                            const auto rty = value_ty_(inst.result);
                            const auto lit = parse_int_literal_(x.text);
                            if (is_int_ty_(rty)) {
                                os << "  " << vref_(inst.result) << " = add " << rty << " 0, " << lit << "\n";
                            } else if (is_float_ty_(rty)) {
                                os << "  " << vref_(inst.result) << " = fadd " << rty << " " << zero_literal_(rty) << ", " << lit << ".0\n";
                            } else if (rty == "ptr") {
                                os << "  " << vref_(inst.result) << " = inttoptr i64 " << lit << " to ptr\n";
                            } else {
                                os << "  " << vref_(inst.result) << " = add i64 0, " << lit << "\n";
                            }
                        } else if constexpr (std::is_same_v<T, InstConstBool>) {
                            if (inst.result == kInvalidId) return;
                            os << "  " << vref_(inst.result) << " = add i1 0, " << (x.value ? "1" : "0") << "\n";
                        } else if constexpr (std::is_same_v<T, InstConstText>) {
                            emit_const_text_(os, iid, inst);
                        } else if constexpr (std::is_same_v<T, InstConstNull>) {
                            if (inst.result == kInvalidId) return;
                            const auto rty = value_ty_(inst.result);
                            if (rty == "ptr") {
                                os << "  " << vref_(inst.result) << " = getelementptr i8, ptr null, i64 0\n";
                            } else if (is_float_ty_(rty)) {
                                os << "  " << vref_(inst.result) << " = fadd " << rty << " " << zero_literal_(rty) << ", " << zero_literal_(rty) << "\n";
                            } else {
                                os << "  " << vref_(inst.result) << " = add " << rty << " 0, 0\n";
                            }
                        } else if constexpr (std::is_same_v<T, InstUnary>) {
                            if (inst.result == kInvalidId) return;
                            const auto rty = value_ty_(inst.result);
                            const auto src = coerce_value_(os, x.src, rty);
                            using U = UnOp;
                            switch (x.op) {
                                case U::Plus:
                                    os << "  " << vref_(inst.result) << " = " << copy_expr_(rty, src) << "\n";
                                    break;
                                case U::Neg:
                                    if (is_float_ty_(rty)) os << "  " << vref_(inst.result) << " = fsub " << rty << " " << zero_literal_(rty) << ", " << src << "\n";
                                    else os << "  " << vref_(inst.result) << " = sub " << rty << " 0, " << src << "\n";
                                    break;
                                case U::Not:
                                    if (rty == "i1") os << "  " << vref_(inst.result) << " = xor i1 " << src << ", true\n";
                                    else if (is_int_ty_(rty)) os << "  " << vref_(inst.result) << " = xor " << rty << " " << src << ", -1\n";
                                    else os << "  " << vref_(inst.result) << " = add i64 0, 0\n";
                                    break;
                                case U::BitNot:
                                    if (is_int_ty_(rty)) os << "  " << vref_(inst.result) << " = xor " << rty << " " << src << ", -1\n";
                                    else os << "  " << vref_(inst.result) << " = add i64 0, 0\n";
                                    break;
                            }
                        } else if constexpr (std::is_same_v<T, InstBinOp>) {
                            if (inst.result == kInvalidId) return;
                            const auto rty = value_ty_(inst.result);
                            using B = BinOp;
                            if (x.op == B::Lt || x.op == B::Le || x.op == B::Gt ||
                                x.op == B::Ge || x.op == B::Eq || x.op == B::Ne) {
                                const auto lty = value_ty_(x.lhs);
                                const auto cty = lty;
                                const auto lhs = coerce_value_(os, x.lhs, cty);
                                const auto rhs = coerce_value_(os, x.rhs, cty);
                                const bool is_fp = is_float_ty_(cty);

                                std::string op;
                                switch (x.op) {
                                    case B::Lt: op = is_fp ? "fcmp olt" : "icmp slt"; break;
                                    case B::Le: op = is_fp ? "fcmp ole" : "icmp sle"; break;
                                    case B::Gt: op = is_fp ? "fcmp ogt" : "icmp sgt"; break;
                                    case B::Ge: op = is_fp ? "fcmp oge" : "icmp sge"; break;
                                    case B::Eq: op = is_fp ? "fcmp oeq" : "icmp eq";  break;
                                    case B::Ne: op = is_fp ? "fcmp one" : "icmp ne";  break;
                                    default: break;
                                }
                                os << "  " << vref_(inst.result) << " = " << op << " " << cty << " " << lhs << ", " << rhs << "\n";
                            } else {
                                const auto aty = rty;
                                const auto lhs = coerce_value_(os, x.lhs, aty);
                                const auto rhs = coerce_value_(os, x.rhs, aty);
                                const bool is_fp = is_float_ty_(aty);
                                std::string op = is_fp ? "fadd" : "add";
                                switch (x.op) {
                                    case B::Add: op = is_fp ? "fadd" : "add"; break;
                                    case B::Sub: op = is_fp ? "fsub" : "sub"; break;
                                    case B::Mul: op = is_fp ? "fmul" : "mul"; break;
                                    case B::Div: op = is_fp ? "fdiv" : "sdiv"; break;
                                    case B::Rem: op = is_fp ? "frem" : "srem"; break;
                                    case B::NullCoalesce:
                                        // v0 초기 구현: null 병합은 lhs 전달로 낮춘다.
                                        os << "  " << vref_(inst.result) << " = " << copy_expr_(aty, lhs) << "\n";
                                        return;
                                    default:
                                        break;
                                }
                                os << "  " << vref_(inst.result) << " = " << op << " " << aty << " " << lhs << ", " << rhs << "\n";
                            }
                        } else if constexpr (std::is_same_v<T, InstCast>) {
                            if (inst.result == kInvalidId) return;
                            const auto rty = value_ty_(inst.result);
                            const auto src = coerce_value_(os, x.src, rty);
                            os << "  " << vref_(inst.result) << " = " << copy_expr_(rty, src) << "\n";
                        } else if constexpr (std::is_same_v<T, InstFuncRef>) {
                            // InstFuncRef는 런타임 값으로 물질화하지 않고, call 시점에만 사용한다.
                            // 단, result가 값 문맥에서 읽히는 경우를 대비해 ptr 표현을 남겨둔다.
                            if (inst.result == kInvalidId) return;
                            if (!is_value_read_(inst.result)) return;
                            const std::string sym = sanitize_symbol_(x.name);
                            const auto rty = value_ty_(inst.result);
                            if (rty == "ptr") {
                                os << "  " << vref_(inst.result) << " = bitcast ptr @" << sym << " to ptr\n";
                            } else if (is_int_ty_(rty)) {
                                os << "  " << vref_(inst.result) << " = ptrtoint ptr @" << sym << " to " << rty << "\n";
                            } else {
                                os << "  " << vref_(inst.result) << " = add i64 0, 0\n";
                            }
                        } else if constexpr (std::is_same_v<T, InstGlobalRef>) {
                            if (inst.result == kInvalidId) return;
                            const std::string sym = sanitize_symbol_(x.name);
                            address_ref_by_value_[inst.result] = "@" + sym;
                            if (!is_value_read_(inst.result)) return;
                            const auto rty = value_ty_(inst.result);
                            if (rty == "ptr") {
                                os << "  " << vref_(inst.result) << " = bitcast ptr @" << sym << " to ptr\n";
                            } else if (is_int_ty_(rty)) {
                                os << "  " << vref_(inst.result) << " = ptrtoint ptr @" << sym << " to " << rty << "\n";
                            } else {
                                os << "  " << vref_(inst.result) << " = add i64 0, 0\n";
                            }
                        } else if constexpr (std::is_same_v<T, InstCall>) {
                            std::vector<std::string> arg_tys{};
                            std::vector<std::string> arg_vals{};
                            arg_tys.reserve(x.args.size());
                            arg_vals.reserve(x.args.size());

                            const auto direct = resolve_direct_callee_(x);
                            for (size_t ai = 0; ai < x.args.size(); ++ai) {
                                std::string want = value_ty_(x.args[ai]);
                                if (direct.has_value() && ai < direct->param_tys.size()) {
                                    want = direct->param_tys[ai];
                                }
                                arg_tys.push_back(want);
                                arg_vals.push_back(coerce_value_(os, x.args[ai], want));
                            }

                            const auto emit_default_result = [&]() {
                                if (inst.result == kInvalidId) return;
                                const auto rty = value_ty_(inst.result);
                                if (is_int_ty_(rty)) os << "  " << vref_(inst.result) << " = add " << rty << " 0, 0\n";
                                else if (is_float_ty_(rty)) os << "  " << vref_(inst.result) << " = fadd " << rty << " " << zero_literal_(rty) << ", " << zero_literal_(rty) << "\n";
                                else if (rty == "ptr") os << "  " << vref_(inst.result) << " = getelementptr i8, ptr null, i64 0\n";
                                else os << "  " << vref_(inst.result) << " = add i64 0, 0\n";
                            };

                            auto emit_arg_list = [&](std::ostringstream& out) {
                                for (size_t i = 0; i < arg_vals.size(); ++i) {
                                    if (i) out << ", ";
                                    out << arg_tys[i] << " " << arg_vals[i];
                                }
                            };

                            if (direct.has_value() && direct->param_tys.size() == arg_vals.size()) {
                                if (direct->ret_ty == "void") {
                                    os << "  call void @" << direct->symbol << "(";
                                    emit_arg_list(os);
                                    os << ")\n";
                                    emit_default_result();
                                } else if (inst.result != kInvalidId) {
                                    const std::string want_ty = value_ty_(inst.result);
                                    if (want_ty == direct->ret_ty) {
                                        os << "  " << vref_(inst.result) << " = call " << direct->ret_ty
                                           << " @" << direct->symbol << "(";
                                        emit_arg_list(os);
                                        os << ")\n";
                                    } else {
                                        // 오버로드/직접 호출 해소가 기대 타입과 어긋나더라도
                                        // SSA 타입 일관성을 보존하도록 결과를 강제 변환한다.
                                        const std::string call_tmp = next_tmp_();
                                        os << "  " << call_tmp << " = call " << direct->ret_ty
                                           << " @" << direct->symbol << "(";
                                        emit_arg_list(os);
                                        os << ")\n";
                                        const std::string coerced = coerce_ref_(os, call_tmp, direct->ret_ty, want_ty);
                                        if (coerced == vref_(inst.result)) {
                                            // no-op
                                        } else if (coerced.size() > 0 && coerced[0] == '%') {
                                            os << "  " << vref_(inst.result) << " = " << copy_expr_(want_ty, coerced) << "\n";
                                        } else {
                                            os << "  " << vref_(inst.result) << " = " << copy_expr_(want_ty, coerced) << "\n";
                                        }
                                    }
                                } else {
                                    os << "  call " << direct->ret_ty << " @" << direct->symbol << "(";
                                    emit_arg_list(os);
                                    os << ")\n";
                                }
                                return;
                            }

                            std::string callee_ptr;
                            if (direct.has_value()) {
                                // direct 메타를 얻었지만 시그니처가 맞지 않아 indirect 경로로 내려가는 경우,
                                // InstFuncRef 값이 소거되어도 동작하도록 심볼에서 즉시 ptr을 만든다.
                                callee_ptr = next_tmp_();
                                os << "  " << callee_ptr << " = bitcast ptr @" << direct->symbol << " to ptr\n";
                            } else {
                                callee_ptr = coerce_value_(os, x.callee, "ptr");
                            }
                            const std::string rty = (inst.result == kInvalidId) ? "void" : value_ty_(inst.result);
                            if (rty == "void") {
                                os << "  call void " << callee_ptr << "(";
                                emit_arg_list(os);
                                os << ")\n";
                            } else {
                                os << "  " << vref_(inst.result) << " = call " << rty << " " << callee_ptr << "(";
                                emit_arg_list(os);
                                os << ")\n";
                            }
                        } else if constexpr (std::is_same_v<T, InstIndex>) {
                            emit_index_(os, inst, x);
                        } else if constexpr (std::is_same_v<T, InstField>) {
                            emit_field_(os, inst, x);
                        } else if constexpr (std::is_same_v<T, InstAllocaLocal>) {
                            if (inst.result == kInvalidId) return;
                            auto slot_ty = map_type_(types_, x.slot_ty, &named_layouts_);
                            if (slot_ty == "void") slot_ty = "i8";
                            os << "  " << vref_(inst.result) << " = alloca " << slot_ty << "\n";
                            address_ref_by_value_[inst.result] = vref_(inst.result);
                        } else if constexpr (std::is_same_v<T, InstLoad>) {
                            if (inst.result == kInvalidId) return;
                            const auto rty = value_ty_(inst.result);
                            const auto ptr = slot_ptr_ref_(os, x.slot);
                            os << "  " << vref_(inst.result) << " = load " << rty << ", ptr " << ptr << "\n";
                        } else if constexpr (std::is_same_v<T, InstStore>) {
                            const auto vty = value_ty_(x.value);
                            const auto ptr = slot_ptr_ref_(os, x.slot);
                            const auto val = coerce_value_(os, x.value, vty);
                            os << "  store " << vty << " " << val << ", ptr " << ptr << "\n";
                        }
                    }, inst.data);
                }
            }

            /// @brief terminator를 LLVM-IR 분기로 출력한다.
            void emit_term_(
                std::ostringstream& os,
                const std::string& ret_ty,
                const parus::oir::Block& block
            ) {
                using namespace parus::oir;
                if (!block.has_term) {
                    os << "  unreachable\n";
                    return;
                }

                std::visit([&](auto&& t) {
                    using T = std::decay_t<decltype(t)>;
                    if constexpr (std::is_same_v<T, TermBr>) {
                        os << "  br label %" << bref_(t.target) << "\n";
                    } else if constexpr (std::is_same_v<T, TermCondBr>) {
                        const auto cond = coerce_value_(os, t.cond, "i1");
                        os << "  br i1 " << cond
                           << ", label %" << bref_(t.then_bb)
                           << ", label %" << bref_(t.else_bb) << "\n";
                    } else if constexpr (std::is_same_v<T, TermRet>) {
                        if (ret_ty == "void") {
                            os << "  ret void\n";
                            return;
                        }
                        if (!t.has_value) {
                            os << "  ret " << ret_ty << " " << zero_literal_(ret_ty) << "\n";
                            return;
                        }
                        const auto v = coerce_value_(os, t.value, ret_ty);
                        os << "  ret " << ret_ty << " " << v << "\n";
                    }
                }, block.term);
            }

            /// @brief CFG edge에서 블록 인자 유입 정보를 수집한다.
            void build_incomings_() {
                using namespace parus::oir;
                for (auto bb : fn_.blocks) {
                    if (bb == kInvalidId || static_cast<size_t>(bb) >= m_.blocks.size()) continue;
                    const auto& block = m_.blocks[bb];
                    if (!block.has_term) continue;

                    std::visit([&](auto&& t) {
                        using T = std::decay_t<decltype(t)>;
                        if constexpr (std::is_same_v<T, TermBr>) {
                            if (!owned_blocks_.contains(t.target)) return;
                            incomings_[t.target].push_back(IncomingEdge{bb, t.args});
                        } else if constexpr (std::is_same_v<T, TermCondBr>) {
                            if (owned_blocks_.contains(t.then_bb)) {
                                incomings_[t.then_bb].push_back(IncomingEdge{bb, t.then_args});
                            }
                            if (owned_blocks_.contains(t.else_bb)) {
                                incomings_[t.else_bb].push_back(IncomingEdge{bb, t.else_args});
                            }
                        } else if constexpr (std::is_same_v<T, TermRet>) {
                            // no edge
                        }
                    }, block.term);
                }
            }
        };

    } // namespace

    LLVMIRLoweringResult lower_oir_to_llvm_ir_text(
        const parus::oir::Module& oir,
        const parus::ty::TypePool& types,
        const LLVMIRLoweringOptions& opt
    ) {
        LLVMIRLoweringResult out{};

        std::ostringstream os;
        os << "; Generated by parusc AOT LLVM lane v" << opt.llvm_lane_major << "\n";
        os << "; NOTE: OIR->LLVM lowering with index/field/aggregate memory model bootstrap.\n";
        os << "source_filename = \"parus.oir\"\n\n";

        std::unordered_map<parus::ty::TypeId, NamedLayoutInfo> named_layouts;
        std::unordered_map<parus::ty::TypeId, std::unordered_map<std::string, uint32_t>> field_offsets;
        for (const auto& f : oir.fields) {
            if (f.self_type == parus::ty::kInvalidType) continue;
            NamedLayoutInfo li{};
            li.size = std::max<uint32_t>(1u, f.size);
            li.align = std::max<uint32_t>(1u, f.align);
            named_layouts[f.self_type] = li;

            auto& om = field_offsets[f.self_type];
            for (const auto& m : f.members) {
                om[m.name] = m.offset;
            }
        }

        std::unordered_map<parus::oir::InstId, TextConstantInfo> text_constants;
        uint32_t text_const_seq = 0;
        for (size_t i = 0; i < oir.insts.size(); ++i) {
            const auto* ct = std::get_if<parus::oir::InstConstText>(&oir.insts[i].data);
            if (ct == nullptr) continue;

            TextConstantInfo info{};
            info.symbol = ".parus_text." + std::to_string(text_const_seq++);
            info.len = static_cast<uint64_t>(ct->bytes.size());
            info.storage_len = info.len + 1; // trailing NUL for C interop friendliness
            text_constants[static_cast<parus::oir::InstId>(i)] = info;

            std::string bytes_with_nul = ct->bytes;
            bytes_with_nul.push_back('\0');
            os << "@" << info.symbol
               << " = private unnamed_addr constant ["
               << info.storage_len
               << " x i8] c\""
               << llvm_escape_c_bytes_(bytes_with_nul)
               << "\", align 1\n";
        }
        if (!text_constants.empty()) {
            os << "\n";
        }

        if (!oir.globals.empty()) {
            for (const auto& g : oir.globals) {
                const std::string sym = sanitize_symbol_(g.name);
                const std::string gty = map_type_(types, g.type, &named_layouts);

                bool is_internal = false;
                if (!g.is_extern && g.abi == parus::oir::FunctionAbi::Parus && !g.is_export) {
                    is_internal = true;
                }

                if (g.is_extern) {
                    os << "@" << sym << " = external global " << gty;
                } else {
                    const char* kind = g.is_mut ? "global" : "constant";
                    os << "@" << sym << " = ";
                    if (is_internal) os << "internal ";
                    os << kind << " " << gty << " zeroinitializer";
                }

                if (g.type != parus::ty::kInvalidType && types.get(g.type).kind == parus::ty::Kind::kNamedUser) {
                    auto it = named_layouts.find(g.type);
                    if (it != named_layouts.end() && it->second.align > 0) {
                        os << ", align " << it->second.align;
                    }
                }
                os << "\n";
            }
            os << "\n";
        }

        const auto value_types = build_value_type_table_(oir, types, named_layouts);
        const auto value_uses = build_value_use_table_(oir);
        const auto phi_contract_errors = verify_phi_incoming_contract_(oir, value_types);
        if (!phi_contract_errors.empty()) {
            out.ok = false;
            for (const auto& e : phi_contract_errors) {
                out.messages.push_back(CompileMessage{true, e});
            }
            out.messages.push_back(CompileMessage{
                true,
                "OIR->LLVM lowering aborted: phi incoming contract violation. Run OIR edge-cast normalization first."
            });
            return out;
        }
        bool need_call_stub = false;
        bool has_raw_main_symbol = false;
        bool has_ambiguous_main_entry = false;
        struct MainEntryCandidate {
            std::string symbol{};
            std::string ret_ty{};
        };
        std::optional<MainEntryCandidate> main_entry_candidate{};

        /// @brief 함수가 사용자 엔트리(main) 후보인지 판정한다.
        auto is_main_entry_candidate_name = [](const parus::oir::Function& fn) -> bool {
            // 신규 경로: OIR Function이 맹글링 전 이름을 함께 보존한 경우.
            if (!fn.source_name.empty()) {
                return fn.source_name == "main";
            }
            // 구버전 OIR과의 호환: 맹글링된 main 패턴을 허용한다.
            return fn.name == "main" || fn.name.rfind("main_fn", 0) == 0;
        };

        for (const auto& fn : oir.funcs) {
            const std::string fn_sym = sanitize_symbol_(fn.name);
            if (fn_sym == "main") {
                has_raw_main_symbol = true;
            }

            if (!fn.is_extern && is_main_entry_candidate_name(fn)) {
                bool is_zero_arity = false;
                if (fn.entry != parus::oir::kInvalidId &&
                    static_cast<size_t>(fn.entry) < oir.blocks.size()) {
                    is_zero_arity = oir.blocks[fn.entry].params.empty();
                }
                const std::string ret_ty = map_type_(types, fn.ret_ty, &named_layouts);
                if (is_zero_arity && (ret_ty == "i32" || ret_ty == "void")) {
                    if (!main_entry_candidate.has_value()) {
                        main_entry_candidate = MainEntryCandidate{fn_sym, ret_ty};
                    } else if (main_entry_candidate->symbol != fn_sym) {
                        has_ambiguous_main_entry = true;
                    }
                }
            }

            FunctionEmitter fe(
                oir,
                types,
                fn,
                value_types,
                value_uses,
                named_layouts,
                field_offsets,
                text_constants
            );
            os << fe.emit(need_call_stub) << "\n";
        }

        if (!has_raw_main_symbol && main_entry_candidate.has_value() && !has_ambiguous_main_entry) {
            // 실행 파일 링크를 위해 C 엔트리 심볼(main)을 자동 브릿지한다.
            os << "define i32 @main() {\n";
            os << "entry:\n";
            if (main_entry_candidate->ret_ty == "i32") {
                os << "  %main_ret = call i32 @" << main_entry_candidate->symbol << "()\n";
                os << "  ret i32 %main_ret\n";
            } else {
                os << "  call void @" << main_entry_candidate->symbol << "()\n";
                os << "  ret i32 0\n";
            }
            os << "}\n\n";
            out.messages.push_back(CompileMessage{
                false,
                "emitted main entry wrapper for executable link."
            });
        }

        if (need_call_stub) {
            // 링크 단계에서 unresolved 심볼이 생기지 않도록 내부 no-op 스텁을 함께 생성한다.
            os << "define internal void @parus_oir_call_stub() {\n";
            os << "entry:\n";
            os << "  ret void\n";
            os << "}\n";
        }

        out.ok = true;
        out.llvm_ir = os.str();
        out.messages.push_back(CompileMessage{
            false,
            "lowered OIR to LLVM-IR text successfully."
        });
        return out;
    }

} // namespace parus::backend::aot
