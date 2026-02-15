#include <gaupel/sir/CapabilityAnalysis.hpp>

#include <gaupel/diag/DiagCode.hpp>
#include <gaupel/diag/Diagnostic.hpp>
#include <gaupel/syntax/TokenKind.hpp>

#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace gaupel::sir {

    namespace {

        using SymbolId = gaupel::sir::SymbolId;

        enum class ValueUse : uint8_t {
            kValue,
            kBorrowOperand,
            kEscapeOperand,
            kAssignLhs,
            kCallArg,
            kReturnValue,
        };

        struct ScopeState {
            std::vector<std::pair<SymbolId, bool>> activated_borrows; // (sym, is_mut)
        };

        struct BorrowState {
            uint32_t shared_count = 0;
            uint32_t mut_count = 0;
        };

        struct SymbolTraits {
            bool is_mut = false;
            bool is_static = false;
        };

        /// @brief SIR 값/문장을 순회하며 capability 충돌, UAF, escape 경계를 정밀 검증한다.
        class CapabilityAnalyzer final {
        public:
            CapabilityAnalyzer(
                Module& m,
                const ty::TypePool& types,
                diag::Bag& bag
            )
                : m_(m), types_(types), bag_(bag) {}

            /// @brief 전체 모듈에 대해 capability 분석을 수행한다.
            CapabilityAnalysisResult run() {
                m_.escape_handles.clear();
                escape_meta_by_value_.clear();
                collect_symbol_traits_();

                for (uint32_t fid = 0; fid < (uint32_t)m_.funcs.size(); ++fid) {
                    analyze_func_(fid);
                }

                CapabilityAnalysisResult out{};
                out.ok = (error_count_ == 0);
                out.error_count = error_count_;

                for (const auto& [sym, moved] : moved_by_escape_) {
                    auto& st = out.state_by_symbol[sym];
                    st.moved_by_escape = moved;
                }
                for (const auto& [sym, bs] : active_borrows_) {
                    auto& st = out.state_by_symbol[sym];
                    st.active_shared_borrows = bs.shared_count;
                    st.active_mut_borrow = (bs.mut_count > 0);
                }
                out.escape_handle_count = (uint32_t)m_.escape_handles.size();
                for (const auto& h : m_.escape_handles) {
                    out.materialized_handle_count += h.materialize_count;
                }
                return out;
            }

        private:
            /// @brief 오류 진단을 누적한다.
            void report_(diag::Code code, Span sp) {
                diag::Diagnostic d(diag::Severity::kError, code, sp);
                bag_.add(std::move(d));
                ++error_count_;
            }

            /// @brief `T`가 borrow(`&T`/`&mut T`)인지 확인한다.
            bool is_borrow_type_(TypeId t) const {
                if (t == k_invalid_type || t >= types_.count()) return false;
                return types_.get(t).kind == ty::Kind::kBorrow;
            }

            /// @brief 타입이 drop을 요구할 수 있는지 보수적으로 판정한다.
            bool type_needs_drop_(TypeId t) const {
                if (t == k_invalid_type || t >= types_.count()) return false;
                const auto& tt = types_.get(t);
                switch (tt.kind) {
                    case ty::Kind::kError:
                        return false;
                    case ty::Kind::kBuiltin:
                        return false;
                    case ty::Kind::kBorrow:
                        return false;
                    case ty::Kind::kEscape:
                        return false;
                    case ty::Kind::kFn:
                        return false;
                    case ty::Kind::kOptional:
                    case ty::Kind::kArray:
                        return type_needs_drop_(tt.elem);
                    case ty::Kind::kNamedUser:
                        // 사용자 정의 타입은 보수적으로 drop 필요로 본다.
                        return true;
                }
                return true;
            }

            /// @brief SIR `ValueId`가 place(local/index)인지 확인한다.
            bool is_place_value_(ValueId vid) const {
                if (vid == k_invalid_value || (size_t)vid >= m_.values.size()) return false;
                const auto& v = m_.values[vid];
                return v.place == PlaceClass::kLocal ||
                       v.place == PlaceClass::kIndex ||
                       v.kind == ValueKind::kGlobal;
            }

            /// @brief `Index + Range` 형태인지 확인한다. (`&x[a..b]`, `&mut x[a..:b]`)
            bool is_slice_borrow_operand_(ValueId vid) const {
                if (vid == k_invalid_value || (size_t)vid >= m_.values.size()) return false;
                const auto& v = m_.values[vid];
                if (v.kind != ValueKind::kIndex) return false;
                if (v.b == k_invalid_value || (size_t)v.b >= m_.values.size()) return false;

                const auto& idx = m_.values[v.b];
                if (idx.kind != ValueKind::kBinary) return false;

                const uint32_t op = idx.op;
                return op == (uint32_t)syntax::TokenKind::kDotDot ||
                       op == (uint32_t)syntax::TokenKind::kDotDotColon;
            }

            /// @brief 값에서 root 심볼을 추적한다(local/index/borrow/escape).
            std::optional<SymbolId> root_symbol_(ValueId vid) const {
                if (vid == k_invalid_value || (size_t)vid >= m_.values.size()) return std::nullopt;
                const auto& v = m_.values[vid];

                if (v.origin_sym != k_invalid_symbol) return v.origin_sym;
                if (v.kind == ValueKind::kLocal && v.sym != k_invalid_symbol) return v.sym;
                if (v.kind == ValueKind::kIndex) return root_symbol_(v.a);
                return std::nullopt;
            }

            /// @brief 스코프를 시작한다.
            void enter_scope_() {
                scopes_.push_back(ScopeState{});
            }

            /// @brief 스코프를 끝내고 스코프 내 활성 borrow를 해제한다.
            void leave_scope_() {
                if (scopes_.empty()) return;
                const auto& scope = scopes_.back();
                for (const auto& [sym, is_mut] : scope.activated_borrows) {
                    auto it = active_borrows_.find(sym);
                    if (it == active_borrows_.end()) continue;
                    if (is_mut) {
                        if (it->second.mut_count > 0) --it->second.mut_count;
                    } else {
                        if (it->second.shared_count > 0) --it->second.shared_count;
                    }
                    if (it->second.mut_count == 0 && it->second.shared_count == 0) {
                        active_borrows_.erase(it);
                    }
                }
                scopes_.pop_back();
            }

            /// @brief 현재 스코프에 borrow 활성화를 등록한다.
            void activate_borrow_(SymbolId sym, bool is_mut) {
                auto& st = active_borrows_[sym];
                if (is_mut) {
                    st.mut_count += 1;
                } else {
                    st.shared_count += 1;
                }
                if (!scopes_.empty()) {
                    scopes_.back().activated_borrows.push_back({sym, is_mut});
                }
            }

            /// @brief 심볼이 mutable 선언인지 확인한다.
            bool is_symbol_mutable_(SymbolId sym) const {
                auto it = symbol_traits_.find(sym);
                if (it == symbol_traits_.end()) return false;
                return it->second.is_mut;
            }

            /// @brief 심볼이 static 저장소인지 확인한다.
            bool is_symbol_static_(SymbolId sym) const {
                auto it = symbol_traits_.find(sym);
                if (it == symbol_traits_.end()) return false;
                return it->second.is_static;
            }

            /// @brief 활성 `&mut` borrow 존재 여부를 반환한다.
            bool has_active_mut_(SymbolId sym) const {
                auto it = active_borrows_.find(sym);
                if (it == active_borrows_.end()) return false;
                return it->second.mut_count > 0;
            }

            /// @brief 활성 shared borrow 존재 여부를 반환한다.
            bool has_active_shared_(SymbolId sym) const {
                auto it = active_borrows_.find(sym);
                if (it == active_borrows_.end()) return false;
                return it->second.shared_count > 0;
            }

            /// @brief `&&` move-out된 심볼인지 확인한다.
            bool is_moved_(SymbolId sym) const {
                auto it = moved_by_escape_.find(sym);
                if (it == moved_by_escape_.end()) return false;
                return it->second;
            }

            /// @brief 심볼을 move-out 상태로 표시한다.
            void mark_moved_(SymbolId sym) {
                moved_by_escape_[sym] = true;
            }

            /// @brief 심볼 move-out 상태를 해제한다(재초기화/재대입).
            void clear_moved_(SymbolId sym) {
                moved_by_escape_[sym] = false;
            }

            /// @brief escape 값이 허용된 경계 문맥인지 반환한다.
            static bool is_escape_boundary_use_(ValueUse use) {
                return use == ValueUse::kReturnValue || use == ValueUse::kCallArg;
            }

            /// @brief 값 사용 문맥을 EscapeBoundaryKind로 변환한다.
            static EscapeBoundaryKind boundary_from_use_(ValueUse use) {
                switch (use) {
                    case ValueUse::kReturnValue: return EscapeBoundaryKind::kReturn;
                    case ValueUse::kCallArg: return EscapeBoundaryKind::kCallArg;
                    default: return EscapeBoundaryKind::kNone;
                }
            }

            /// @brief `&&` 값에 대한 EscapeHandle 메타를 등록/업데이트한다.
            void register_escape_handle_(
                ValueId escape_vid,
                const Value& escape_value,
                ValueUse use,
                std::optional<SymbolId> root
            ) {
                if (escape_vid == k_invalid_value || (size_t)escape_vid >= m_.values.size()) return;

                const bool from_static = root && is_symbol_static_(*root);
                const EscapeBoundaryKind boundary = boundary_from_use_(use);

                EscapeHandleMeta meta{};
                meta.escape_value = escape_vid;
                meta.span = escape_value.span;
                meta.origin_sym = root ? *root : k_invalid_symbol;
                meta.pointee_type = value_type_(escape_value.a);
                meta.from_static = from_static;
                meta.has_drop = type_needs_drop_(meta.pointee_type);
                meta.boundary = boundary;

                if (boundary == EscapeBoundaryKind::kReturn || boundary == EscapeBoundaryKind::kCallArg) {
                    meta.kind = EscapeHandleKind::kCallerSlot;
                } else if (from_static) {
                    meta.kind = EscapeHandleKind::kTrivial;
                } else {
                    meta.kind = EscapeHandleKind::kStackSlot;
                }

                // v0: 내부는 비물질화 토큰으로 유지하고 ABI/FFI 경계에서만 패킹.
                meta.abi_pack_required = (boundary == EscapeBoundaryKind::kAbi);
                meta.ffi_pack_required = (boundary == EscapeBoundaryKind::kFfi);
                meta.materialize_count = 0;

                auto it = escape_meta_by_value_.find(escape_vid);
                if (it == escape_meta_by_value_.end()) {
                    const uint32_t idx = m_.add_escape_handle(meta);
                    escape_meta_by_value_[escape_vid] = idx;
                } else if ((size_t)it->second < m_.escape_handles.size()) {
                    auto& dst = m_.escape_handles[it->second];
                    dst = meta;
                }
            }

            /// @brief 심볼 trait(is_mut/is_static)를 수집한다.
            void collect_symbol_traits_() {
                symbol_traits_.clear();

                for (const auto& p : m_.params) {
                    if (p.sym == k_invalid_symbol) continue;
                    auto& t = symbol_traits_[p.sym];
                    t.is_mut = p.is_mut;
                    t.is_static = false;
                }
                for (const auto& s : m_.stmts) {
                    if (s.kind != StmtKind::kVarDecl || s.sym == k_invalid_symbol) continue;
                    auto& t = symbol_traits_[s.sym];
                    t.is_mut = s.is_mut;
                    t.is_static = s.is_static;
                }
                for (const auto& g : m_.globals) {
                    if (g.sym == k_invalid_symbol) continue;
                    auto& t = symbol_traits_[g.sym];
                    t.is_mut = g.is_mut;
                    t.is_static = g.is_static;
                }
            }

            /// @brief 함수 단위 컨텍스트를 초기화하고 entry block을 분석한다.
            void analyze_func_(uint32_t fid) {
                if (fid >= m_.funcs.size()) return;

                active_borrows_.clear();
                moved_by_escape_.clear();
                scopes_.clear();
                visiting_blocks_.clear();

                const auto& f = m_.funcs[fid];
                current_fn_is_pure_ = f.is_pure;
                current_fn_is_comptime_ = f.is_comptime;

                if (f.entry != k_invalid_block) {
                    analyze_block_(f.entry);
                }
            }

            /// @brief block 내 stmt를 순회 분석한다.
            void analyze_block_(BlockId bid) {
                if (bid == k_invalid_block || (size_t)bid >= m_.blocks.size()) return;
                if (!visiting_blocks_.insert(bid).second) return;

                enter_scope_();
                const auto& b = m_.blocks[bid];
                const uint64_t end = (uint64_t)b.stmt_begin + (uint64_t)b.stmt_count;
                if (end <= (uint64_t)m_.stmts.size()) {
                    for (uint32_t i = 0; i < b.stmt_count; ++i) {
                        analyze_stmt_(b.stmt_begin + i);
                    }
                }
                leave_scope_();

                visiting_blocks_.erase(bid);
            }

            /// @brief SIR statement를 분석한다.
            void analyze_stmt_(uint32_t sid) {
                if ((size_t)sid >= m_.stmts.size()) return;
                const auto& s = m_.stmts[sid];

                switch (s.kind) {
                    case StmtKind::kExprStmt:
                        analyze_value_(s.expr, ValueUse::kValue);
                        return;

                    case StmtKind::kVarDecl: {
                        analyze_value_(s.init, ValueUse::kValue);
                        if (is_borrow_type_(value_type_(s.init)) && s.is_static) {
                            report_(diag::Code::kBorrowEscapeToStorage, s.span);
                        }
                        if (s.sym != k_invalid_symbol) {
                            clear_moved_(s.sym);
                        }
                        return;
                    }

                    case StmtKind::kIfStmt:
                        analyze_value_(s.expr, ValueUse::kValue);
                        analyze_block_(s.a);
                        if (s.b != k_invalid_block) analyze_block_(s.b);
                        return;

                    case StmtKind::kWhileStmt:
                        analyze_value_(s.expr, ValueUse::kValue);
                        analyze_block_(s.a);
                        return;

                    case StmtKind::kReturn:
                        analyze_value_(s.expr, ValueUse::kReturnValue);
                        if (is_borrow_type_(value_type_(s.expr))) {
                            report_(diag::Code::kBorrowEscapeFromReturn, s.span);
                        }
                        return;

                    case StmtKind::kBreak:
                        if (s.expr != k_invalid_value) analyze_value_(s.expr, ValueUse::kValue);
                        return;

                    case StmtKind::kContinue:
                    case StmtKind::kSwitch:
                    case StmtKind::kError:
                        return;
                }
            }

            /// @brief 값 타입을 조회한다.
            TypeId value_type_(ValueId vid) const {
                if (vid == k_invalid_value || (size_t)vid >= m_.values.size()) return k_invalid_type;
                return m_.values[vid].type;
            }

            /// @brief SIR value를 재귀 순회하며 capability 규칙을 검증한다.
            void analyze_value_(ValueId vid, ValueUse use) {
                if (vid == k_invalid_value || (size_t)vid >= m_.values.size()) return;
                const auto& v = m_.values[vid];

                switch (v.kind) {
                    case ValueKind::kLocal: {
                        if (v.sym == k_invalid_symbol) return;

                        if (use != ValueUse::kAssignLhs && is_moved_(v.sym)) {
                            report_(diag::Code::kSirUseAfterEscapeMove, v.span);
                        }

                        const bool direct_access =
                            use == ValueUse::kValue ||
                            use == ValueUse::kCallArg ||
                            use == ValueUse::kReturnValue ||
                            use == ValueUse::kAssignLhs;

                        if (direct_access && has_active_mut_(v.sym)) {
                            report_(diag::Code::kBorrowMutDirectAccessConflict, v.span);
                        }
                        if (use == ValueUse::kAssignLhs && has_active_shared_(v.sym)) {
                            report_(diag::Code::kBorrowSharedWriteConflict, v.span);
                        }
                        return;
                    }

                    case ValueKind::kBorrow: {
                        analyze_value_(v.a, ValueUse::kBorrowOperand);

                        const bool place_ok = is_place_value_(v.a) || is_slice_borrow_operand_(v.a);
                        if (!place_ok) {
                            report_(diag::Code::kBorrowOperandMustBePlace, v.span);
                            return;
                        }

                        const auto root = (v.origin_sym != k_invalid_symbol)
                            ? std::optional<SymbolId>{v.origin_sym}
                            : root_symbol_(v.a);
                        if (!root) return;

                        if (v.borrow_is_mut && !is_symbol_mutable_(*root)) {
                            report_(diag::Code::kBorrowMutRequiresMutablePlace, v.span);
                        }

                        const bool has_mut_conflict = has_active_mut_(*root);
                        const bool has_shared_conflict = has_active_shared_(*root);

                        if (v.borrow_is_mut) {
                            if (has_mut_conflict) {
                                report_(diag::Code::kBorrowMutConflict, v.span);
                            }
                            if (has_shared_conflict) {
                                report_(diag::Code::kBorrowMutConflictWithShared, v.span);
                            }
                            if (!has_mut_conflict && !has_shared_conflict && is_symbol_mutable_(*root)) {
                                activate_borrow_(*root, /*is_mut=*/true);
                            }
                        } else {
                            if (has_mut_conflict) {
                                report_(diag::Code::kBorrowSharedConflictWithMut, v.span);
                            } else {
                                activate_borrow_(*root, /*is_mut=*/false);
                            }
                        }
                        return;
                    }

                    case ValueKind::kEscape: {
                        analyze_value_(v.a, ValueUse::kEscapeOperand);

                        if (!is_place_value_(v.a)) {
                            report_(diag::Code::kEscapeOperandMustBePlace, v.span);
                        }
                        if (current_fn_is_pure_ || current_fn_is_comptime_) {
                            report_(diag::Code::kTypeEscapeNotAllowedInPureComptime, v.span);
                        }
                        if (is_borrow_type_(value_type_(v.a))) {
                            report_(diag::Code::kEscapeOperandMustNotBeBorrow, v.span);
                        }

                        const auto root = (v.origin_sym != k_invalid_symbol)
                            ? std::optional<SymbolId>{v.origin_sym}
                            : root_symbol_(v.a);
                        register_escape_handle_(vid, v, use, root);

                        if (root) {
                            if (has_active_mut_(*root)) {
                                report_(diag::Code::kEscapeWhileMutBorrowActive, v.span);
                            }
                            if (has_active_shared_(*root)) {
                                report_(diag::Code::kEscapeWhileBorrowActive, v.span);
                            }

                            if (!is_escape_boundary_use_(use) && !is_symbol_static_(*root)) {
                                report_(diag::Code::kSirEscapeBoundaryViolation, v.span);
                            }
                            mark_moved_(*root);
                        } else if (!is_escape_boundary_use_(use)) {
                            report_(diag::Code::kSirEscapeBoundaryViolation, v.span);
                        }
                        return;
                    }

                    case ValueKind::kAssign: {
                        analyze_value_(v.a, ValueUse::kAssignLhs);
                        analyze_value_(v.b, ValueUse::kValue);

                        if (is_borrow_type_(value_type_(v.b))) {
                            bool lhs_plain_local = false;
                            if (v.a != k_invalid_value && (size_t)v.a < m_.values.size()) {
                                const auto& lhs = m_.values[v.a];
                                if (lhs.kind == ValueKind::kLocal && lhs.sym != k_invalid_symbol &&
                                    !is_symbol_static_(lhs.sym)) {
                                    lhs_plain_local = true;
                                }
                            }
                            if (!lhs_plain_local) {
                                report_(diag::Code::kBorrowEscapeToStorage, v.span);
                            }
                        }

                        if (auto root = root_symbol_(v.a)) {
                            clear_moved_(*root);
                        }
                        return;
                    }

                    case ValueKind::kCall: {
                        analyze_value_(v.a, ValueUse::kValue);

                        // call 인자에서 만들어진 임시 borrow는 call 식 종료와 함께 정리한다.
                        enter_scope_();
                        uint32_t i = 0;
                        while (i < v.arg_count) {
                            const uint32_t aid = v.arg_begin + i;
                            if ((size_t)aid >= m_.args.size()) break;
                            const auto& a = m_.args[aid];

                            if (a.kind == ArgKind::kNamedGroup) {
                                for (uint32_t j = 0; j < a.child_count; ++j) {
                                    const uint32_t cid = a.child_begin + j;
                                    if ((size_t)cid >= m_.args.size()) break;
                                    const auto& ca = m_.args[cid];
                                    if (!ca.is_hole) analyze_value_(ca.value, ValueUse::kCallArg);
                                }
                                i += 1 + a.child_count;
                                continue;
                            }

                            if (!a.is_hole) analyze_value_(a.value, ValueUse::kCallArg);
                            ++i;
                        }
                        leave_scope_();
                        return;
                    }

                    case ValueKind::kIndex:
                        analyze_value_(v.a, use == ValueUse::kAssignLhs ? ValueUse::kAssignLhs : ValueUse::kValue);
                        analyze_value_(v.b, ValueUse::kValue);
                        return;

                    case ValueKind::kIfExpr:
                        analyze_value_(v.a, ValueUse::kValue);
                        analyze_value_(v.b, ValueUse::kValue);
                        analyze_value_(v.c, ValueUse::kValue);
                        return;

                    case ValueKind::kBlockExpr: {
                        const BlockId blk = (BlockId)v.a;
                        analyze_block_(blk);
                        if (v.b != k_invalid_value) analyze_value_(v.b, ValueUse::kValue);
                        return;
                    }

                    case ValueKind::kLoopExpr: {
                        if (v.a != k_invalid_value) analyze_value_(v.a, ValueUse::kValue);
                        const BlockId body = (BlockId)v.b;
                        analyze_block_(body);
                        return;
                    }

                    case ValueKind::kUnary:
                    case ValueKind::kPostfixInc:
                    case ValueKind::kCast:
                        analyze_value_(v.a, ValueUse::kValue);
                        return;

                    case ValueKind::kBinary:
                        analyze_value_(v.a, ValueUse::kValue);
                        analyze_value_(v.b, ValueUse::kValue);
                        return;

                    case ValueKind::kArrayLit: {
                        const uint64_t end = (uint64_t)v.arg_begin + (uint64_t)v.arg_count;
                        if (end <= (uint64_t)m_.args.size()) {
                            for (uint32_t i = 0; i < v.arg_count; ++i) {
                                const auto& a = m_.args[v.arg_begin + i];
                                if (!a.is_hole) analyze_value_(a.value, ValueUse::kValue);
                            }
                        }
                        return;
                    }

                    case ValueKind::kError:
                    case ValueKind::kIntLit:
                    case ValueKind::kFloatLit:
                    case ValueKind::kStringLit:
                    case ValueKind::kCharLit:
                    case ValueKind::kBoolLit:
                    case ValueKind::kNullLit:
                    case ValueKind::kGlobal:
                    case ValueKind::kParam:
                    case ValueKind::kFieldInit:
                    case ValueKind::kField:
                        return;
                }
            }

            Module& m_;
            const ty::TypePool& types_;
            diag::Bag& bag_;

            uint32_t error_count_ = 0;
            bool current_fn_is_pure_ = false;
            bool current_fn_is_comptime_ = false;

            std::unordered_map<SymbolId, SymbolTraits> symbol_traits_;
            std::unordered_map<SymbolId, BorrowState> active_borrows_;
            std::unordered_map<SymbolId, bool> moved_by_escape_;
            std::unordered_map<ValueId, uint32_t> escape_meta_by_value_;
            std::vector<ScopeState> scopes_;
            std::unordered_set<BlockId> visiting_blocks_;
        };

    } // namespace

    CapabilityAnalysisResult analyze_capabilities(
        Module& m,
        const ty::TypePool& types,
        diag::Bag& bag
    ) {
        CapabilityAnalyzer a(m, types, bag);
        return a.run();
    }

} // namespace gaupel::sir
