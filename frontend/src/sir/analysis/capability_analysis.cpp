// frontend/src/sir/analysis/capability_analysis.cpp
#include <parus/sir/CapabilityAnalysis.hpp>

#include <parus/diag/DiagCode.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/syntax/TokenKind.hpp>

#include <charconv>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace parus::sir {

    namespace {

        using SymbolId = parus::sir::SymbolId;

        enum class ValueUse : uint8_t {
            kValue,
            kBorrowOperand,
            kEscapeOperand,
            kAssignLhs,
            kCallArg,
            kReturnValue,
        };

        enum class PlaceProjectionKind : uint8_t {
            kIndex,
            kField,
        };

        struct PlaceProjection {
            PlaceProjectionKind kind = PlaceProjectionKind::kIndex;

            // kIndex
            bool has_const_index = false;
            uint64_t const_index = 0;

            // kField
            std::string_view field_name{};
        };

        struct PlaceRef {
            SymbolId root = k_invalid_symbol;
            std::vector<PlaceProjection> proj;
        };

        struct ActiveBorrow {
            PlaceRef place{};
            bool is_mut = false;
            SymbolId owner_sym = k_invalid_symbol; // borrow 값을 바인딩한 심볼(없으면 임시 borrow)
        };

        struct ScopeState {
            size_t borrow_size_at_entry = 0;
        };

        struct SymbolTraits {
            bool is_mut = false;
            bool is_static = false;
        };

        struct FlowState {
            std::unordered_map<SymbolId, bool> moved_true;
            std::vector<ActiveBorrow> active_borrows;
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

                // Global/class-static initializer expressions are outside function CFG,
                // but they can still contain escape handles (e.g. static p = &&G).
                // Analyze them in value context so metadata is materialized before OIR gate.
                current_fn_is_pure_ = false;
                current_fn_is_comptime_ = false;
                active_borrows_.clear();
                moved_by_escape_.clear();
                scopes_.clear();
                visiting_blocks_.clear();
                for (const auto& g : m_.globals) {
                    if (g.init == k_invalid_value) continue;
                    analyze_value_(g.init, ValueUse::kValue);
                }

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
                for (const auto& b : active_borrows_) {
                    if (b.place.root == k_invalid_symbol) continue;
                    auto& st = out.state_by_symbol[b.place.root];
                    if (b.is_mut) st.active_mut_borrow = true;
                    else st.active_shared_borrows += 1;
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

            /// @brief `T`가 escape(`^&T`)인지 확인한다.
            bool is_escape_type_(TypeId t) const {
                if (t == k_invalid_type || t >= types_.count()) return false;
                return types_.get(t).kind == ty::Kind::kEscape;
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
                    case ty::Kind::kPtr:
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
                       v.place == PlaceClass::kField ||
                       v.kind == ValueKind::kField ||
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

            /// @brief 정수 리터럴에서 상수 인덱스를 추출한다(추출 실패 시 nullopt).
            std::optional<uint64_t> parse_const_index_(ValueId vid) const {
                if (vid == k_invalid_value || (size_t)vid >= m_.values.size()) return std::nullopt;
                const auto& v = m_.values[vid];
                if (v.kind != ValueKind::kIntLit) return std::nullopt;

                std::string_view text = v.text;
                std::string digits;
                digits.reserve(text.size());
                for (char ch : text) {
                    if (ch >= '0' && ch <= '9') {
                        digits.push_back(ch);
                        continue;
                    }
                    if (ch == '_') continue;
                    break; // 접미사(i32/u64 등) 시작
                }
                if (digits.empty()) return std::nullopt;

                uint64_t out = 0;
                const char* begin = digits.data();
                const char* end = begin + digits.size();
                auto [ptr, ec] = std::from_chars(begin, end, out, 10);
                if (ec != std::errc{} || ptr != end) return std::nullopt;
                return out;
            }

            /// @brief 값에서 place 경로(root + projection)를 구성한다.
            bool try_make_place_ref_(ValueId vid, PlaceRef& out) const {
                if (vid == k_invalid_value || (size_t)vid >= m_.values.size()) return false;
                const auto& v = m_.values[vid];

                switch (v.kind) {
                    case ValueKind::kLocal:
                        if (v.sym == k_invalid_symbol) return false;
                        out.root = v.sym;
                        return true;

                    case ValueKind::kGlobal:
                        if (v.sym == k_invalid_symbol) return false;
                        out.root = v.sym;
                        return true;

                    case ValueKind::kIndex: {
                        if (!try_make_place_ref_(v.a, out)) return false;
                        PlaceProjection p{};
                        p.kind = PlaceProjectionKind::kIndex;
                        if (const auto ci = parse_const_index_(v.b)) {
                            p.has_const_index = true;
                            p.const_index = *ci;
                        }
                        out.proj.push_back(p);
                        return true;
                    }

                    case ValueKind::kField: {
                        if (!try_make_place_ref_(v.a, out)) return false;
                        PlaceProjection p{};
                        p.kind = PlaceProjectionKind::kField;
                        p.field_name = v.text;
                        out.proj.push_back(p);
                        return true;
                    }

                    default:
                        return false;
                }
            }

            /// @brief 값에서 place 경로를 추출한다.
            std::optional<PlaceRef> place_ref_(ValueId vid) const {
                PlaceRef p{};
                if (!try_make_place_ref_(vid, p)) return std::nullopt;
                if (p.root == k_invalid_symbol) return std::nullopt;
                return p;
            }

            /// @brief 값에서 root 심볼을 추적한다(local/index/field/global).
            std::optional<SymbolId> root_symbol_(ValueId vid) const {
                if (vid == k_invalid_value || (size_t)vid >= m_.values.size()) return std::nullopt;
                const auto& v = m_.values[vid];
                if (v.origin_sym != k_invalid_symbol) return v.origin_sym;
                if (const auto p = place_ref_(vid)) return p->root;
                return std::nullopt;
            }

            /// @brief 두 place projection이 disjoint인지 판정한다.
            static bool projection_disjoint_(const PlaceProjection& a, const PlaceProjection& b) {
                if (a.kind != b.kind) return false;
                if (a.kind == PlaceProjectionKind::kField) {
                    return !a.field_name.empty() && !b.field_name.empty() && a.field_name != b.field_name;
                }
                if (a.kind == PlaceProjectionKind::kIndex) {
                    return a.has_const_index && b.has_const_index && a.const_index != b.const_index;
                }
                return false;
            }

            /// @brief 두 place가 alias/충돌 가능한지(overlap) 판정한다.
            static bool place_overlap_(const PlaceRef& a, const PlaceRef& b) {
                if (a.root == k_invalid_symbol || b.root == k_invalid_symbol) return false;
                if (a.root != b.root) return false;

                const size_t n = (a.proj.size() < b.proj.size()) ? a.proj.size() : b.proj.size();
                for (size_t i = 0; i < n; ++i) {
                    if (projection_disjoint_(a.proj[i], b.proj[i])) return false;
                }
                // prefix 관계이거나(ancestor), projection이 같거나, 동적 index가 섞인 경우는 overlap으로 본다.
                return true;
            }

            /// @brief projection이 완전히 동일한지 비교한다.
            static bool place_projection_equal_(const PlaceProjection& a, const PlaceProjection& b) {
                if (a.kind != b.kind) return false;
                if (a.kind == PlaceProjectionKind::kIndex) {
                    return a.has_const_index == b.has_const_index &&
                           a.const_index == b.const_index;
                }
                return a.field_name == b.field_name;
            }

            /// @brief place 경로(root + projection)가 완전히 같은지 비교한다.
            static bool place_ref_equal_(const PlaceRef& a, const PlaceRef& b) {
                if (a.root != b.root) return false;
                if (a.proj.size() != b.proj.size()) return false;
                for (size_t i = 0; i < a.proj.size(); ++i) {
                    if (!place_projection_equal_(a.proj[i], b.proj[i])) return false;
                }
                return true;
            }

            /// @brief 활성 borrow 항목이 완전히 같은지 비교한다.
            static bool active_borrow_equal_(const ActiveBorrow& a, const ActiveBorrow& b) {
                return a.is_mut == b.is_mut &&
                       a.owner_sym == b.owner_sym &&
                       place_ref_equal_(a.place, b.place);
            }

            /// @brief borrow 집합에 중복 없이 추가한다. (같은 owner/place에서 shared/mut 충돌은 mut로 승격)
            static void append_unique_borrow_(std::vector<ActiveBorrow>& dst, const ActiveBorrow& item) {
                for (auto& cur : dst) {
                    if (active_borrow_equal_(cur, item)) return;
                    if (cur.owner_sym == item.owner_sym &&
                        place_ref_equal_(cur.place, item.place) &&
                        cur.is_mut != item.is_mut) {
                        // 보수 규칙: 합류 후에는 stronger capability(&mut)를 유지해
                        // false-negative(충돌 누락)를 막는다.
                        cur.is_mut = true;
                        return;
                    }
                }
                dst.push_back(item);
            }

            /// @brief 두 borrow 집합을 CFG 합류점에서 보수적으로 합친다.
            static std::vector<ActiveBorrow> merge_borrow_set_(
                const std::vector<ActiveBorrow>& a,
                const std::vector<ActiveBorrow>& b
            ) {
                std::vector<ActiveBorrow> out;
                out.reserve(a.size() + b.size());
                for (const auto& x : a) append_unique_borrow_(out, x);
                for (const auto& x : b) append_unique_borrow_(out, x);
                return out;
            }

            /// @brief borrow 집합이 같은지 순서 무관(set)으로 비교한다.
            static bool borrow_set_equal_(
                const std::vector<ActiveBorrow>& a,
                const std::vector<ActiveBorrow>& b
            ) {
                if (a.size() != b.size()) return false;
                for (const auto& x : a) {
                    bool found = false;
                    for (const auto& y : b) {
                        if (active_borrow_equal_(x, y)) {
                            found = true;
                            break;
                        }
                    }
                    if (!found) return false;
                }
                return true;
            }

            /// @brief 스코프를 시작한다.
            void enter_scope_() {
                scopes_.push_back(ScopeState{active_borrows_.size()});
            }

            /// @brief 스코프를 끝내고 스코프에서 생성한 borrow를 해제한다.
            void leave_scope_() {
                if (scopes_.empty()) return;
                const size_t keep = scopes_.back().borrow_size_at_entry;
                if (keep < active_borrows_.size()) {
                    active_borrows_.resize(keep);
                }
                scopes_.pop_back();
            }

            /// @brief 현재 스코프에 borrow 활성화를 등록한다.
            void activate_borrow_(const PlaceRef& place, bool is_mut, SymbolId owner_sym) {
                active_borrows_.push_back(ActiveBorrow{place, is_mut, owner_sym});
            }

            /// @brief 특정 심볼이 바인딩한 borrow를 해제한다(재대입/재바인딩 시점).
            void release_owner_borrows_(SymbolId owner_sym) {
                if (owner_sym == k_invalid_symbol) return;
                size_t write = 0;
                for (size_t read = 0; read < active_borrows_.size(); ++read) {
                    if (active_borrows_[read].owner_sym == owner_sym) continue;
                    if (write != read) active_borrows_[write] = active_borrows_[read];
                    ++write;
                }
                active_borrows_.resize(write);
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

            /// @brief 주어진 place와 겹치는 활성 '&mut' borrow 존재 여부를 반환한다.
            bool has_active_mut_(const PlaceRef& place) const {
                for (const auto& b : active_borrows_) {
                    if (!b.is_mut) continue;
                    if (place_overlap_(place, b.place)) return true;
                }
                return false;
            }

            /// @brief 주어진 place와 겹치는 활성 shared borrow 존재 여부를 반환한다.
            bool has_active_shared_(const PlaceRef& place) const {
                for (const auto& b : active_borrows_) {
                    if (b.is_mut) continue;
                    if (place_overlap_(place, b.place)) return true;
                }
                return false;
            }

            /// @brief `^&` move-out된 심볼인지 확인한다.
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

            /// @brief 현재 CFG 지점의 흐름 상태(moved + active borrow)를 캡처한다.
            FlowState capture_flow_state_() const {
                FlowState st{};
                for (const auto& [sym, moved] : moved_by_escape_) {
                    if (moved) st.moved_true[sym] = true;
                }
                for (const auto& b : active_borrows_) {
                    append_unique_borrow_(st.active_borrows, b);
                }
                return st;
            }

            /// @brief 캡처된 흐름 상태로 moved/borrow 집합을 복원한다.
            void restore_flow_state_(const FlowState& st) {
                moved_by_escape_.clear();
                for (const auto& [sym, moved] : st.moved_true) {
                    if (moved) moved_by_escape_[sym] = true;
                }
                active_borrows_ = st.active_borrows;
            }

            /// @brief CFG 합류점에서 두 경로의 moved/borrow 상태를 보수적으로 OR-merge 한다.
            static FlowState merge_flow_state_(const FlowState& a, const FlowState& b) {
                FlowState out = a;
                for (const auto& [sym, moved] : b.moved_true) {
                    if (moved) out.moved_true[sym] = true;
                }
                out.active_borrows = merge_borrow_set_(a.active_borrows, b.active_borrows);
                return out;
            }

            /// @brief 두 흐름 상태가 같은지(논리적으로) 비교한다.
            static bool flow_state_equal_(const FlowState& a, const FlowState& b) {
                if (a.moved_true.size() != b.moved_true.size()) return false;
                for (const auto& [sym, moved] : a.moved_true) {
                    auto it = b.moved_true.find(sym);
                    if (it == b.moved_true.end()) return false;
                    if (moved != it->second) return false;
                }
                return borrow_set_equal_(a.active_borrows, b.active_borrows);
            }

            /// @brief 주어진 입력 흐름 상태로 블록을 분석하고, 블록 종료 상태를 반환한다.
            FlowState analyze_block_with_flow_(BlockId bid, const FlowState& in) {
                const auto saved_borrows = active_borrows_;
                const auto saved_scopes = scopes_;
                const auto saved_moved = moved_by_escape_;
                const auto saved_visiting = visiting_blocks_;

                restore_flow_state_(in);
                analyze_block_(bid);
                const FlowState out = capture_flow_state_();

                active_borrows_ = saved_borrows;
                scopes_ = saved_scopes;
                moved_by_escape_ = saved_moved;
                visiting_blocks_ = saved_visiting;
                return out;
            }

            /// @brief 주어진 입력 흐름 상태로 조건식을 분석하고, 종료 상태를 반환한다.
            FlowState analyze_value_with_flow_(ValueId vid, ValueUse use, const FlowState& in) {
                const auto saved_borrows = active_borrows_;
                const auto saved_scopes = scopes_;
                const auto saved_moved = moved_by_escape_;
                const auto saved_visiting = visiting_blocks_;

                restore_flow_state_(in);
                analyze_value_(vid, use);
                const FlowState out = capture_flow_state_();

                active_borrows_ = saved_borrows;
                scopes_ = saved_scopes;
                moved_by_escape_ = saved_moved;
                visiting_blocks_ = saved_visiting;
                return out;
            }

            /// @brief place 기반 use-after-move를 검사한다.
            void check_use_after_move_(ValueId place_vid, ValueUse use, Span sp) {
                if (use == ValueUse::kAssignLhs) return;
                const auto root = root_symbol_(place_vid);
                if (!root) return;
                if (is_moved_(*root)) {
                    report_(diag::Code::kSirUseAfterEscapeMove, sp);
                }
            }

            /// @brief place 직접 접근/쓰기 시 borrow 충돌을 검사한다.
            void check_place_access_conflicts_(ValueId place_vid, ValueUse use, Span sp) {
                const auto p = place_ref_(place_vid);
                if (!p) return;

                const bool direct_access =
                    use == ValueUse::kValue ||
                    use == ValueUse::kCallArg ||
                    use == ValueUse::kReturnValue ||
                    use == ValueUse::kAssignLhs;

                if (direct_access && has_active_mut_(*p)) {
                    report_(diag::Code::kBorrowMutDirectAccessConflict, sp);
                }
                if (use == ValueUse::kAssignLhs && has_active_shared_(*p)) {
                    report_(diag::Code::kBorrowSharedWriteConflict, sp);
                }
            }

            /// @brief 대입 lhs가 static place인지 판정한다.
            bool is_static_place_(ValueId lhs_vid) const {
                const auto root = root_symbol_(lhs_vid);
                if (!root) return false;
                return is_symbol_static_(*root);
            }

            /// @brief 대입 lhs가 plain local 심볼 바인딩인지 판정한다.
            SymbolId local_binding_symbol_(ValueId lhs_vid) const {
                if (lhs_vid == k_invalid_value || (size_t)lhs_vid >= m_.values.size()) return k_invalid_symbol;
                const auto& lhs = m_.values[lhs_vid];
                if (lhs.kind != ValueKind::kLocal) return k_invalid_symbol;
                return lhs.sym;
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

            /// @brief `^&` 값에 대한 EscapeHandle 메타를 등록/업데이트한다.
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

                // v0: 내부는 비물질화 토큰으로 유지하고 ABI 경계에서만 패킹.
                meta.abi_pack_required = (boundary == EscapeBoundaryKind::kAbi);
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
                            SymbolId init_owner = k_invalid_symbol;
                            if (s.sym != k_invalid_symbol && is_borrow_type_(value_type_(s.init))) {
                                // let/set으로 borrow를 재바인딩하는 경우 기존 owner borrow를 정리한다.
                                release_owner_borrows_(s.sym);
                                init_owner = s.sym;
                            }

                            analyze_value_(s.init, ValueUse::kValue, init_owner);
                            if (is_borrow_type_(value_type_(s.init)) && s.is_static) {
                                report_(diag::Code::kBorrowEscapeToStorage, s.span);
                            }
                        if (is_escape_type_(value_type_(s.init)) && !s.is_static) {
                            report_(diag::Code::kSirEscapeMustNotMaterialize, s.span);
                        }
                        if (s.sym != k_invalid_symbol) {
                            clear_moved_(s.sym);
                        }
                        return;
                    }

                    case StmtKind::kIfStmt:
                        analyze_value_(s.expr, ValueUse::kValue);
                        {
                            // CFG join(then/else)에서 moved 상태를 경로별로 합친다.
                            const FlowState in = capture_flow_state_();
                            const FlowState then_out = analyze_block_with_flow_(s.a, in);
                            const FlowState else_out =
                                (s.b != k_invalid_block) ? analyze_block_with_flow_(s.b, in) : in;
                            restore_flow_state_(merge_flow_state_(then_out, else_out));
                        }
                        return;

                    case StmtKind::kWhileStmt:
                        {
                            // while 정밀 고정점:
                            // - loop head 상태를 고정점으로 수렴시킨다.
                            // - 종료 상태에는 "마지막 조건식 평가 효과"를 포함한다.
                            const FlowState pre = capture_flow_state_();
                            FlowState head = pre;
                            FlowState exit_from_cond = analyze_value_with_flow_(s.expr, ValueUse::kValue, head);

                            for (uint32_t iter = 0; iter < 16; ++iter) {
                                const FlowState cond_out = analyze_value_with_flow_(s.expr, ValueUse::kValue, head);
                                const FlowState body_out = analyze_block_with_flow_(s.a, cond_out);
                                const FlowState next_head = merge_flow_state_(pre, body_out);
                                exit_from_cond = cond_out;
                                if (flow_state_equal_(next_head, head)) break;
                                head = next_head;
                            }

                            restore_flow_state_(merge_flow_state_(pre, exit_from_cond));
                        }
                        return;

                    case StmtKind::kDoScopeStmt:
                        {
                            const FlowState in = capture_flow_state_();
                            const FlowState out = analyze_block_with_flow_(s.a, in);
                            restore_flow_state_(out);
                        }
                        return;

                    case StmtKind::kManualStmt:
                        {
                            // manual은 권한 컨텍스트를 부여하지만 수명/escape 규칙은 그대로 적용한다.
                            const FlowState in = capture_flow_state_();
                            const FlowState out = analyze_block_with_flow_(s.a, in);
                            restore_flow_state_(out);
                        }
                        return;

                    case StmtKind::kDoWhileStmt:
                        {
                            // do-while 정밀 고정점:
                            // - body를 최소 1회 반영한 뒤 head 고정점을 수렴시킨다.
                            // - 종료 상태에는 마지막 조건식 평가 효과를 포함한다.
                            const FlowState pre = capture_flow_state_();
                            const FlowState first_body = analyze_block_with_flow_(s.a, pre);
                            FlowState head = first_body;
                            FlowState exit_from_cond = analyze_value_with_flow_(s.expr, ValueUse::kValue, head);

                            for (uint32_t iter = 0; iter < 16; ++iter) {
                                const FlowState cond_out = analyze_value_with_flow_(s.expr, ValueUse::kValue, head);
                                const FlowState body_out = analyze_block_with_flow_(s.a, cond_out);
                                const FlowState next_head = merge_flow_state_(first_body, body_out);
                                exit_from_cond = cond_out;
                                if (flow_state_equal_(next_head, head)) break;
                                head = next_head;
                            }

                            restore_flow_state_(merge_flow_state_(first_body, exit_from_cond));
                        }
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
                        return;

                    case StmtKind::kCommitStmt:
                    case StmtKind::kRecastStmt:
                        return;

                    case StmtKind::kSwitch: {
                        const FlowState in = capture_flow_state_();
                        analyze_value_(s.expr, ValueUse::kValue);
                        const FlowState after_scrut = capture_flow_state_();

                        bool has_case = false;
                        FlowState merged{};
                        if ((uint64_t)s.case_begin + (uint64_t)s.case_count <= (uint64_t)m_.switch_cases.size()) {
                            for (uint32_t i = 0; i < s.case_count; ++i) {
                                const auto& c = m_.switch_cases[s.case_begin + i];
                                const FlowState arm_out = analyze_block_with_flow_(c.body, after_scrut);
                                if (!has_case) {
                                    merged = arm_out;
                                    has_case = true;
                                } else {
                                    merged = merge_flow_state_(merged, arm_out);
                                }
                            }
                        }

                        if (has_case) restore_flow_state_(merged);
                        else restore_flow_state_(merge_flow_state_(in, after_scrut));
                        return;
                    }

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
            void analyze_value_(
                ValueId vid,
                ValueUse use,
                SymbolId borrow_owner_hint = k_invalid_symbol
            ) {
                if (vid == k_invalid_value || (size_t)vid >= m_.values.size()) return;
                const auto& v = m_.values[vid];

                switch (v.kind) {
                    case ValueKind::kLocal: {
                        if (v.sym == k_invalid_symbol) return;
                        check_use_after_move_(vid, use, v.span);
                        check_place_access_conflicts_(vid, use, v.span);
                        return;
                    }

                    case ValueKind::kBorrow: {
                        analyze_value_(v.a, ValueUse::kBorrowOperand, k_invalid_symbol);

                        const bool place_ok = is_place_value_(v.a) || is_slice_borrow_operand_(v.a);
                        if (!place_ok) {
                            report_(diag::Code::kBorrowOperandMustBePlace, v.span);
                            return;
                        }
                        if (is_borrow_type_(value_type_(v.a)) || is_escape_type_(value_type_(v.a))) {
                            report_(diag::Code::kBorrowOperandMustBeOwnedPlace, v.span);
                            return;
                        }

                        auto place = place_ref_(v.a);
                        if (!place) return;
                        const auto root = (v.origin_sym != k_invalid_symbol)
                            ? std::optional<SymbolId>{v.origin_sym}
                            : std::optional<SymbolId>{place->root};
                        if (!root || *root == k_invalid_symbol) return;

                        if (v.borrow_is_mut && !is_symbol_mutable_(*root)) {
                            report_(diag::Code::kBorrowMutRequiresMutablePlace, v.span);
                        }

                        const bool has_mut_conflict = has_active_mut_(*place);
                        const bool has_shared_conflict = has_active_shared_(*place);

                        if (v.borrow_is_mut) {
                            if (has_mut_conflict) {
                                report_(diag::Code::kBorrowMutConflict, v.span);
                            }
                            if (has_shared_conflict) {
                                report_(diag::Code::kBorrowMutConflictWithShared, v.span);
                            }
                            if (!has_mut_conflict && !has_shared_conflict && is_symbol_mutable_(*root)) {
                                activate_borrow_(*place, /*is_mut=*/true, borrow_owner_hint);
                            }
                        } else {
                            if (has_mut_conflict) {
                                report_(diag::Code::kBorrowSharedConflictWithMut, v.span);
                            } else {
                                activate_borrow_(*place, /*is_mut=*/false, borrow_owner_hint);
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

                        auto place = place_ref_(v.a);
                        const auto root = (v.origin_sym != k_invalid_symbol)
                            ? std::optional<SymbolId>{v.origin_sym}
                            : (place ? std::optional<SymbolId>{place->root} : root_symbol_(v.a));
                        register_escape_handle_(vid, v, use, root);

                        if (root && *root != k_invalid_symbol) {
                            if (place && has_active_mut_(*place)) {
                                report_(diag::Code::kEscapeWhileMutBorrowActive, v.span);
                            }
                            if (place && has_active_shared_(*place)) {
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
                        analyze_value_(v.a, ValueUse::kAssignLhs, k_invalid_symbol);

                        const SymbolId lhs_local_sym = local_binding_symbol_(v.a);
                        const bool binds_borrow_rhs =
                            lhs_local_sym != k_invalid_symbol &&
                            is_borrow_type_(value_type_(v.b));
                        if (lhs_local_sym != k_invalid_symbol) {
                            // 대입 시 기존 바인딩 borrow 수명을 종료한다(재바인딩 모델).
                            release_owner_borrows_(lhs_local_sym);
                        }

                        analyze_value_(
                            v.b,
                            ValueUse::kValue,
                            binds_borrow_rhs ? lhs_local_sym : k_invalid_symbol
                        );

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
                        if (is_escape_type_(value_type_(v.b)) && !is_static_place_(v.a)) {
                            report_(diag::Code::kSirEscapeMustNotMaterialize, v.span);
                        }

                        if (auto root = root_symbol_(v.a)) {
                            clear_moved_(*root);
                        }
                        return;
                    }

                    case ValueKind::kCall: {
                        analyze_value_(v.a, ValueUse::kValue, k_invalid_symbol);

                        // call 인자에서 만들어진 임시 borrow는 call 식 종료와 함께 정리한다.
                        enter_scope_();
                        uint32_t i = 0;
                        while (i < v.arg_count) {
                            const uint32_t aid = v.arg_begin + i;
                            if ((size_t)aid >= m_.args.size()) break;
                            const auto& a = m_.args[aid];

                            if (!a.is_hole) analyze_value_(a.value, ValueUse::kCallArg, k_invalid_symbol);
                            ++i;
                        }
                        leave_scope_();
                        return;
                    }

                    case ValueKind::kIndex:
                        analyze_value_(v.a, ValueUse::kValue, k_invalid_symbol);
                        analyze_value_(v.b, ValueUse::kValue, k_invalid_symbol);
                        check_use_after_move_(vid, use, v.span);
                        check_place_access_conflicts_(vid, use, v.span);
                        return;

                    case ValueKind::kField:
                        analyze_value_(v.a, ValueUse::kValue, k_invalid_symbol);
                        check_use_after_move_(vid, use, v.span);
                        check_place_access_conflicts_(vid, use, v.span);
                        return;

                    case ValueKind::kIfExpr:
                        analyze_value_(v.a, ValueUse::kValue, k_invalid_symbol);
                        analyze_value_(v.b, ValueUse::kValue, k_invalid_symbol);
                        analyze_value_(v.c, ValueUse::kValue, k_invalid_symbol);
                        return;

                    case ValueKind::kBlockExpr: {
                        const BlockId blk = (BlockId)v.a;
                        analyze_block_(blk);
                        if (v.b != k_invalid_value) analyze_value_(v.b, ValueUse::kValue, k_invalid_symbol);
                        return;
                    }

                    case ValueKind::kLoopExpr: {
                        if (v.a != k_invalid_value) analyze_value_(v.a, ValueUse::kValue, k_invalid_symbol);
                        const BlockId body = (BlockId)v.b;
                        analyze_block_(body);
                        return;
                    }

                    case ValueKind::kUnary:
                    case ValueKind::kPostfixInc:
                    case ValueKind::kCast:
                        analyze_value_(v.a, ValueUse::kValue, k_invalid_symbol);
                        return;

                    case ValueKind::kBinary:
                        analyze_value_(v.a, ValueUse::kValue, k_invalid_symbol);
                        analyze_value_(v.b, ValueUse::kValue, k_invalid_symbol);
                        return;

                    case ValueKind::kArrayLit: {
                        const uint64_t end = (uint64_t)v.arg_begin + (uint64_t)v.arg_count;
                        if (end <= (uint64_t)m_.args.size()) {
                            for (uint32_t i = 0; i < v.arg_count; ++i) {
                                const auto& a = m_.args[v.arg_begin + i];
                                if (!a.is_hole) analyze_value_(a.value, ValueUse::kValue, k_invalid_symbol);
                            }
                        }
                        return;
                    }

                    case ValueKind::kFieldInit: {
                        const uint64_t end = (uint64_t)v.arg_begin + (uint64_t)v.arg_count;
                        if (end <= (uint64_t)m_.args.size()) {
                            for (uint32_t i = 0; i < v.arg_count; ++i) {
                                const auto& a = m_.args[v.arg_begin + i];
                                if (!a.is_hole) analyze_value_(a.value, ValueUse::kValue, k_invalid_symbol);
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
            std::vector<ActiveBorrow> active_borrows_;
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
        // Cap 분석은 canonical SIR을 전제로 한다.
        // (호출 순서에 의존하지 않도록 분석 진입점에서 한 번 더 정규화한다.)
        (void)canonicalize_for_capability(m, types);
        CapabilityAnalyzer a(m, types, bag);
        return a.run();
    }

} // namespace parus::sir
