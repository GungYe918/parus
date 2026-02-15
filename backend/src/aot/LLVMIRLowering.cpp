// backend/src/aot/LLVMIRLowering.cpp
#include <parus/backend/aot/LLVMIRLowering.hpp>

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace parus::backend::aot {

    namespace {

        struct IncomingEdge {
            parus::oir::BlockId pred = parus::oir::kInvalidId;
            std::vector<parus::oir::ValueId> args{};
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
            return "0";
        }

        /// @brief 타입에 맞는 복사/제로 생성 식을 만든다.
        std::string copy_expr_(const std::string& ty, const std::string& src_ref) {
            if (is_int_ty_(ty)) return "add " + ty + " 0, " + src_ref;
            if (is_float_ty_(ty)) return "fadd " + ty + " " + zero_literal_(ty) + ", " + src_ref;
            if (ty == "ptr") return "bitcast ptr " + src_ref + " to ptr";
            return "add i64 0, 0";
        }

        /// @brief 타입 ID를 LLVM 타입 문자열로 변환한다.
        std::string map_type_(const parus::ty::TypePool& types, parus::ty::TypeId tid) {
            if (tid == parus::ty::kInvalidType) return "i64";
            const auto& t = types.get(tid);
            using K = parus::ty::Kind;
            using B = parus::ty::Builtin;

            if (t.kind == K::kBuiltin) {
                switch (t.builtin) {
                    case B::kUnit:  return "void";
                    case B::kNever: return "void";
                    case B::kBool:  return "i1";
                    case B::kChar:  return "i32";
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
            }

            // v0: 사용자/집합/핸들/배열은 우선 i64로 모델링한다.
            return "i64";
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
            const parus::ty::TypePool& types
        ) {
            std::vector<std::string> out(m.values.size(), "i64");
            for (size_t i = 0; i < m.values.size(); ++i) {
                out[i] = map_type_(types, m.values[i].ty);
            }

            for (const auto& inst : m.insts) {
                if (inst.result == parus::oir::kInvalidId || static_cast<size_t>(inst.result) >= out.size()) continue;

                if (std::holds_alternative<parus::oir::InstAllocaLocal>(inst.data)) {
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
                    out[inst.result] = map_type_(types, c.to);
                }
            }
            return out;
        }

        /// @brief OIR 함수 하나를 LLVM-IR 함수 텍스트로 변환한다.
        class FunctionEmitter {
        public:
            FunctionEmitter(
                const parus::oir::Module& m,
                const parus::ty::TypePool& types,
                const parus::oir::Function& fn,
                const std::vector<std::string>& value_types
            ) : m_(m), types_(types), fn_(fn), value_types_(value_types) {
                for (auto bb : fn_.blocks) owned_blocks_.insert(bb);
                build_incomings_();
            }

            /// @brief 함수 본문을 생성한다.
            std::string emit(bool& need_call_stub) {
                std::ostringstream os;
                const std::string ret_ty = map_type_(types_, fn_.ret_ty);
                const std::string sym = sanitize_symbol_(fn_.name);
                os << "define " << ret_ty << " @" << sym << "() {\n";

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

            std::unordered_set<parus::oir::BlockId> owned_blocks_{};
            std::unordered_map<parus::oir::BlockId, std::vector<IncomingEdge>> incomings_{};
            uint32_t temp_seq_ = 0;
            bool need_call_stub_ = false;

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

            /// @brief src 값을 want 타입으로 강제 변환한 SSA ref를 반환한다.
            std::string coerce_value_(
                std::ostringstream& os,
                parus::oir::ValueId src,
                const std::string& want
            ) {
                const std::string cur = value_ty_(src);
                const std::string ref = vref_(src);
                if (cur == want) return ref;

                const std::string tmp = next_tmp_();

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

            /// @brief 블록 인자(phi)를 출력한다.
            void emit_block_params_(
                std::ostringstream& os,
                parus::oir::BlockId bb,
                const parus::oir::Block& block
            ) {
                auto it = incomings_.find(bb);
                for (size_t i = 0; i < block.params.size(); ++i) {
                    const auto p = block.params[i];
                    const std::string pty = value_ty_(p);

                    std::vector<std::string> incoming_texts;
                    if (it != incomings_.end()) {
                        for (const auto& edge : it->second) {
                            if (i >= edge.args.size()) continue;
                            const auto arg = edge.args[i];
                            const auto arg_ref = coerce_value_(os, arg, pty);
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
                        } else if constexpr (std::is_same_v<T, InstCall>) {
                            need_call_stub_ = true;
                            os << "  call void @parus_oir_call_stub()\n";
                            if (inst.result != kInvalidId) {
                                const auto rty = value_ty_(inst.result);
                                if (is_int_ty_(rty)) os << "  " << vref_(inst.result) << " = add " << rty << " 0, 0\n";
                                else if (is_float_ty_(rty)) os << "  " << vref_(inst.result) << " = fadd " << rty << " " << zero_literal_(rty) << ", " << zero_literal_(rty) << "\n";
                                else if (rty == "ptr") os << "  " << vref_(inst.result) << " = getelementptr i8, ptr null, i64 0\n";
                                else os << "  " << vref_(inst.result) << " = add i64 0, 0\n";
                            }
                        } else if constexpr (std::is_same_v<T, InstIndex> || std::is_same_v<T, InstField>) {
                            if (inst.result == kInvalidId) return;
                            const auto rty = value_ty_(inst.result);
                            if (is_int_ty_(rty)) os << "  " << vref_(inst.result) << " = add " << rty << " 0, 0\n";
                            else if (is_float_ty_(rty)) os << "  " << vref_(inst.result) << " = fadd " << rty << " " << zero_literal_(rty) << ", " << zero_literal_(rty) << "\n";
                            else if (rty == "ptr") os << "  " << vref_(inst.result) << " = getelementptr i8, ptr null, i64 0\n";
                            else os << "  " << vref_(inst.result) << " = add i64 0, 0\n";
                        } else if constexpr (std::is_same_v<T, InstAllocaLocal>) {
                            if (inst.result == kInvalidId) return;
                            const auto slot_ty = map_type_(types_, x.slot_ty);
                            os << "  " << vref_(inst.result) << " = alloca " << slot_ty << "\n";
                        } else if constexpr (std::is_same_v<T, InstLoad>) {
                            if (inst.result == kInvalidId) return;
                            const auto rty = value_ty_(inst.result);
                            const auto ptr = coerce_value_(os, x.slot, "ptr");
                            os << "  " << vref_(inst.result) << " = load " << rty << ", ptr " << ptr << "\n";
                        } else if constexpr (std::is_same_v<T, InstStore>) {
                            const auto vty = value_ty_(x.value);
                            const auto ptr = coerce_value_(os, x.slot, "ptr");
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
        os << "; NOTE: this is v0 lowering bootstrap for OIR -> LLVM-IR.\n";
        os << "source_filename = \"parus.oir\"\n\n";

        const auto value_types = build_value_type_table_(oir, types);
        bool need_call_stub = false;

        for (const auto& fn : oir.funcs) {
            FunctionEmitter fe(oir, types, fn, value_types);
            os << fe.emit(need_call_stub) << "\n";
        }

        if (need_call_stub) {
            os << "declare void @parus_oir_call_stub()\n";
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
