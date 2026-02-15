// frontend/src/cap/capability_check.cpp
#include <parus/cap/CapabilityCheck.hpp>

#include <parus/diag/DiagCode.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/syntax/TokenKind.hpp>

#include <optional>
#include <unordered_map>
#include <vector>

namespace parus::cap {

    namespace {

        using SymbolId = uint32_t;

        enum class ExprUse : uint8_t {
            kValue,
            kBorrowOperand,
            kEscapeOperand,
            kAssignLhs,
            kCallArg,
            kReturnValue,
        };

        struct ScopeState {
            std::vector<SymbolId> activated_mut_borrows;
            std::vector<SymbolId> activated_shared_borrows;
        };

        /// @brief `& / &mut / &&` capability 규칙을 함수 단위로 검사한다.
        class CapabilityChecker {
        public:
            CapabilityChecker(
                const ast::AstArena& ast,
                ast::StmtId program_root,
                const passes::NameResolveResult& nres,
                const tyck::TyckResult& tyck,
                const ty::TypePool& types,
                diag::Bag& bag
            )
                : ast_(ast)
                , program_root_(program_root)
                , nres_(nres)
                , tyck_(tyck)
                , types_(types)
                , bag_(bag) {}

            /// @brief capability 체크를 수행하고 결과를 반환한다.
            CapabilityResult run() {
                build_symbol_traits_();
                enter_scope_();
                walk_stmt_(program_root_);
                leave_scope_();

                CapabilityResult out{};
                out.ok = (error_count_ == 0);
                out.error_count = error_count_;
                return out;
            }

        private:
            /// @brief ExprId의 유효 범위를 확인한다.
            bool is_valid_expr_id_(ast::ExprId eid) const {
                return eid != ast::k_invalid_expr && (size_t)eid < ast_.exprs().size();
            }

            /// @brief StmtId의 유효 범위를 확인한다.
            bool is_valid_stmt_id_(ast::StmtId sid) const {
                return sid != ast::k_invalid_stmt && (size_t)sid < ast_.stmts().size();
            }

            /// @brief 심볼 trait 맵(SymbolId -> mut/static)을 구성한다.
            void build_symbol_traits_() {
                symbol_is_mut_.clear();
                symbol_is_static_.clear();

                const auto& stmts = ast_.stmts();
                for (uint32_t sid = 0; sid < (uint32_t)stmts.size(); ++sid) {
                    const auto& s = stmts[sid];
                    if (s.kind != ast::StmtKind::kVar) continue;
                    auto sym = symbol_from_stmt_((ast::StmtId)sid);
                    if (!sym) continue;
                    symbol_is_mut_[*sym] = s.is_mut;
                    symbol_is_static_[*sym] = s.is_static;
                }

                const auto& ps = ast_.params();
                for (uint32_t pid = 0; pid < (uint32_t)ps.size(); ++pid) {
                    auto sym = symbol_from_param_(pid);
                    if (!sym) continue;
                    symbol_is_mut_[*sym] = ps[pid].is_mut;
                    symbol_is_static_[*sym] = false;
                }
            }

            /// @brief 진단을 기록하고 error 카운트를 증가시킨다.
            void report_(diag::Code code, Span sp) {
                diag::Diagnostic d(diag::Severity::kError, code, sp);
                bag_.add(std::move(d));
                ++error_count_;
            }

            /// @brief 진단을 기록하고 error 카운트를 증가시킨다(인자 1개).
            void report_(diag::Code code, Span sp, std::string_view a0) {
                diag::Diagnostic d(diag::Severity::kError, code, sp);
                d.add_arg(a0);
                bag_.add(std::move(d));
                ++error_count_;
            }

            /// @brief 현재 lexical scope를 시작한다.
            void enter_scope_() {
                scopes_.push_back(ScopeState{});
            }

            /// @brief 현재 lexical scope를 종료하고 활성 mut borrow를 해제한다.
            void leave_scope_() {
                if (scopes_.empty()) return;
                const auto& scope = scopes_.back();
                for (auto sym : scope.activated_mut_borrows) {
                    auto it = active_mut_borrow_count_.find(sym);
                    if (it == active_mut_borrow_count_.end()) continue;
                    if (it->second > 0) --it->second;
                    if (it->second == 0) active_mut_borrow_count_.erase(it);
                }
                for (auto sym : scope.activated_shared_borrows) {
                    auto it = active_shared_borrow_count_.find(sym);
                    if (it == active_shared_borrow_count_.end()) continue;
                    if (it->second > 0) --it->second;
                    if (it->second == 0) active_shared_borrow_count_.erase(it);
                }
                scopes_.pop_back();
            }

            /// @brief 특정 심볼의 활성 `&mut` borrow를 현재 스코프에 등록한다.
            void activate_mut_borrow_(SymbolId sym) {
                active_mut_borrow_count_[sym] += 1;
                if (!scopes_.empty()) {
                    scopes_.back().activated_mut_borrows.push_back(sym);
                }
            }

            /// @brief 특정 심볼의 활성 shared borrow를 현재 스코프에 등록한다.
            void activate_shared_borrow_(SymbolId sym) {
                active_shared_borrow_count_[sym] += 1;
                if (!scopes_.empty()) {
                    scopes_.back().activated_shared_borrows.push_back(sym);
                }
            }

            /// @brief 특정 심볼에 활성 `&mut` borrow가 있는지 확인한다.
            bool has_active_mut_borrow_(SymbolId sym) const {
                auto it = active_mut_borrow_count_.find(sym);
                if (it == active_mut_borrow_count_.end()) return false;
                return it->second > 0;
            }

            /// @brief 특정 심볼에 활성 shared borrow가 있는지 확인한다.
            bool has_active_shared_borrow_(SymbolId sym) const {
                auto it = active_shared_borrow_count_.find(sym);
                if (it == active_shared_borrow_count_.end()) return false;
                return it->second > 0;
            }

            /// @brief SymbolId가 `mut`로 선언되었는지 확인한다.
            bool is_symbol_mutable_(SymbolId sym) const {
                auto it = symbol_is_mut_.find(sym);
                if (it == symbol_is_mut_.end()) return false;
                return it->second;
            }

            /// @brief SymbolId가 static 저장소로 선언되었는지 확인한다.
            bool is_symbol_static_(SymbolId sym) const {
                auto it = symbol_is_static_.find(sym);
                if (it == symbol_is_static_.end()) return false;
                return it->second;
            }

            /// @brief escape operand가 경계(return/call-arg)에서 바로 사용되는지 확인한다.
            static bool is_escape_boundary_use_(ExprUse use) {
                return use == ExprUse::kReturnValue || use == ExprUse::kCallArg;
            }

            /// @brief SymbolId가 `&&`로 move-out 되었는지 확인한다.
            bool is_symbol_moved_(SymbolId sym) const {
                auto it = moved_by_escape_.find(sym);
                if (it == moved_by_escape_.end()) return false;
                return it->second;
            }

            /// @brief SymbolId를 move-out 상태로 표시한다.
            void mark_symbol_moved_(SymbolId sym) {
                moved_by_escape_[sym] = true;
            }

            /// @brief SymbolId를 재초기화하여 move-out 상태를 해제한다.
            void clear_symbol_moved_(SymbolId sym) {
                moved_by_escape_[sym] = false;
            }

            /// @brief ExprId의 타입을 tyck 결과에서 조회한다.
            ty::TypeId expr_type_(ast::ExprId eid) const {
                if (eid == ast::k_invalid_expr) return ty::kInvalidType;
                if ((size_t)eid >= tyck_.expr_types.size()) return ty::kInvalidType;
                return tyck_.expr_types[eid];
            }

            /// @brief 타입이 borrow(`&T`/`&mut T`)인지 확인한다.
            bool is_borrow_type_(ty::TypeId t) const {
                if (t == ty::kInvalidType) return false;
                if (t >= types_.count()) return false;
                return types_.get(t).kind == ty::Kind::kBorrow;
            }

            /// @brief 타입이 mutable borrow(`&mut T`)인지 확인한다.
            bool is_mut_borrow_type_(ty::TypeId t) const {
                if (!is_borrow_type_(t)) return false;
                return types_.get(t).borrow_is_mut;
            }

            /// @brief range 식(`a..b`, `a..:b`)인지 확인한다.
            bool is_range_expr_(ast::ExprId eid) const {
                if (eid == ast::k_invalid_expr) return false;
                if ((size_t)eid >= ast_.exprs().size()) return false;

                const auto& e = ast_.expr(eid);
                if (e.kind != ast::ExprKind::kBinary) return false;
                return e.op == syntax::TokenKind::kDotDot || e.op == syntax::TokenKind::kDotDotColon;
            }

            /// @brief expression이 place expression인지 판정한다(v0: ident/index).
            bool is_place_expr_(ast::ExprId eid) const {
                if (eid == ast::k_invalid_expr) return false;
                const auto& e = ast_.expr(eid);
                if (e.kind == ast::ExprKind::kIdent) return true;
                if (e.kind == ast::ExprKind::kIndex) {
                    // range index는 slice view 생성용이므로 일반 place로는 취급하지 않는다.
                    if (is_range_expr_(e.b)) return false;
                    return is_place_expr_(e.a);
                }
                return false;
            }

            /// @brief `&x[a..b]` / `&mut x[a..:b]` 형태의 slice borrow 피연산자인지 확인한다.
            bool is_slice_borrow_operand_(ast::ExprId eid) const {
                if (eid == ast::k_invalid_expr) return false;
                if ((size_t)eid >= ast_.exprs().size()) return false;

                const auto& e = ast_.expr(eid);
                if (e.kind != ast::ExprKind::kIndex) return false;
                if (!is_range_expr_(e.b)) return false;
                return is_place_expr_(e.a);
            }

            /// @brief ident expression의 SymbolId를 name-resolve 결과에서 조회한다.
            std::optional<SymbolId> symbol_from_ident_expr_(ast::ExprId eid) const {
                if (eid == ast::k_invalid_expr) return std::nullopt;
                if ((size_t)eid >= nres_.expr_to_resolved.size()) return std::nullopt;
                const auto rid = nres_.expr_to_resolved[(uint32_t)eid];
                if (rid == passes::NameResolveResult::k_invalid_resolved) return std::nullopt;
                if ((size_t)rid >= nres_.resolved.size()) return std::nullopt;
                return nres_.resolved[rid].sym;
            }

            /// @brief stmt declaration의 SymbolId를 name-resolve 결과에서 조회한다.
            std::optional<SymbolId> symbol_from_stmt_(ast::StmtId sid) const {
                if (sid == ast::k_invalid_stmt) return std::nullopt;
                if ((size_t)sid >= nres_.stmt_to_resolved.size()) return std::nullopt;
                const auto rid = nres_.stmt_to_resolved[(uint32_t)sid];
                if (rid == passes::NameResolveResult::k_invalid_resolved) return std::nullopt;
                if ((size_t)rid >= nres_.resolved.size()) return std::nullopt;
                return nres_.resolved[rid].sym;
            }

            /// @brief param declaration의 SymbolId를 name-resolve 결과에서 조회한다.
            std::optional<SymbolId> symbol_from_param_(uint32_t pid) const {
                if ((size_t)pid >= nres_.param_to_resolved.size()) return std::nullopt;
                const auto rid = nres_.param_to_resolved[pid];
                if (rid == passes::NameResolveResult::k_invalid_resolved) return std::nullopt;
                if ((size_t)rid >= nres_.resolved.size()) return std::nullopt;
                return nres_.resolved[rid].sym;
            }

            /// @brief place expression의 root SymbolId를 조회한다(v0: ident/index(base)).
            std::optional<SymbolId> root_place_symbol_(ast::ExprId eid) const {
                if (eid == ast::k_invalid_expr) return std::nullopt;
                const auto& e = ast_.expr(eid);
                if (e.kind == ast::ExprKind::kIdent) return symbol_from_ident_expr_(eid);
                if (e.kind == ast::ExprKind::kIndex) return root_place_symbol_(e.a);
                return std::nullopt;
            }

            /// @brief statement 트리를 순회하며 capability 규칙을 검사한다.
            void walk_stmt_(ast::StmtId sid) {
                if (!is_valid_stmt_id_(sid)) return;
                const auto& s = ast_.stmt(sid);

                switch (s.kind) {
                    case ast::StmtKind::kEmpty:
                        return;

                    case ast::StmtKind::kExprStmt:
                        walk_expr_(s.expr, ExprUse::kValue);
                        return;

                    case ast::StmtKind::kBlock:
                        walk_block_stmt_(s);
                        return;

                    case ast::StmtKind::kVar: {
                        if (s.init != ast::k_invalid_expr) {
                            walk_expr_(s.init, ExprUse::kValue);
                        }
                        return;
                    }

                    case ast::StmtKind::kIf:
                        walk_expr_(s.expr, ExprUse::kValue);
                        walk_stmt_(s.a);
                        walk_stmt_(s.b);
                        return;

                    case ast::StmtKind::kWhile:
                        walk_expr_(s.expr, ExprUse::kValue);
                        walk_stmt_(s.a);
                        return;
                    case ast::StmtKind::kDoScope:
                        walk_stmt_(s.a);
                        return;
                    case ast::StmtKind::kDoWhile:
                        walk_stmt_(s.a);
                        walk_expr_(s.expr, ExprUse::kValue);
                        return;

                    case ast::StmtKind::kReturn: {
                        if (s.expr != ast::k_invalid_expr) {
                            walk_expr_(s.expr, ExprUse::kReturnValue);
                        }
                        return;
                    }

                    case ast::StmtKind::kBreak:
                        if (s.expr != ast::k_invalid_expr) walk_expr_(s.expr, ExprUse::kValue);
                        return;

                    case ast::StmtKind::kContinue:
                        return;

                    case ast::StmtKind::kFnDecl:
                        walk_fn_decl_(s);
                        return;

                    case ast::StmtKind::kActsDecl: {
                        const auto& kids = ast_.stmt_children();
                        for (uint32_t i = 0; i < s.stmt_count; ++i) {
                            walk_stmt_(kids[s.stmt_begin + i]);
                        }
                        return;
                    }

                    case ast::StmtKind::kSwitch: {
                        walk_expr_(s.expr, ExprUse::kValue);
                        const auto& cs = ast_.switch_cases();
                        for (uint32_t i = 0; i < s.case_count; ++i) {
                            walk_stmt_(cs[s.case_begin + i].body);
                        }
                        return;
                    }

                    case ast::StmtKind::kFieldDecl:
                    case ast::StmtKind::kUse:
                    case ast::StmtKind::kNestDecl:
                    case ast::StmtKind::kError:
                        return;
                }
            }

            /// @brief block statement를 lexical scope로 순회한다.
            void walk_block_stmt_(const ast::Stmt& s) {
                enter_scope_();
                const auto& kids = ast_.stmt_children();
                for (uint32_t i = 0; i < s.stmt_count; ++i) {
                    walk_stmt_(kids[s.stmt_begin + i]);
                }
                leave_scope_();
            }

            /// @brief 함수 컨텍스트를 분리하여 body를 검사한다.
            void walk_fn_decl_(const ast::Stmt& s) {
                auto saved_active = std::move(active_mut_borrow_count_);
                auto saved_active_shared = std::move(active_shared_borrow_count_);
                auto saved_moved = std::move(moved_by_escape_);
                auto saved_scopes = std::move(scopes_);
                const bool saved_pure = fn_is_pure_;
                const bool saved_comptime = fn_is_comptime_;

                active_mut_borrow_count_.clear();
                active_shared_borrow_count_.clear();
                moved_by_escape_.clear();
                scopes_.clear();
                fn_is_pure_ = s.is_pure;
                fn_is_comptime_ = s.is_comptime;
                fn_depth_ += 1;

                enter_scope_();
                const auto& ps = ast_.params();
                for (uint32_t i = 0; i < s.param_count; ++i) {
                    const auto& p = ps[s.param_begin + i];
                    if (p.has_default) {
                        walk_expr_(p.default_expr, ExprUse::kValue);
                    }
                }
                walk_stmt_(s.a);
                leave_scope_();

                if (fn_depth_ > 0) --fn_depth_;
                fn_is_pure_ = saved_pure;
                fn_is_comptime_ = saved_comptime;
                active_mut_borrow_count_ = std::move(saved_active);
                active_shared_borrow_count_ = std::move(saved_active_shared);
                moved_by_escape_ = std::move(saved_moved);
                scopes_ = std::move(saved_scopes);
            }

            /// @brief expression 트리를 순회하며 capability 규칙을 검사한다.
            void walk_expr_(ast::ExprId eid, ExprUse use) {
                if (!is_valid_expr_id_(eid)) return;
                const auto& e = ast_.expr(eid);

                switch (e.kind) {
                    case ast::ExprKind::kIdent: {
                        (void)use; // AST pass는 식별자 충돌/UAF 정밀 검증을 SIR 단계로 위임한다.
                        return;
                    }

                    case ast::ExprKind::kUnary: {
                        if (e.op == syntax::TokenKind::kAmp) {
                            walk_expr_(e.a, ExprUse::kBorrowOperand);

                            const bool place_ok = is_place_expr_(e.a) || is_slice_borrow_operand_(e.a);
                            if (!place_ok) {
                                report_(diag::Code::kBorrowOperandMustBePlace, e.span);
                            }

                            auto sid = root_place_symbol_(e.a);
                            if (sid) {
                                if (e.unary_is_mut && !is_symbol_mutable_(*sid)) {
                                    report_(diag::Code::kBorrowMutRequiresMutablePlace, e.span);
                                }
                            }
                            return;
                        }

                        if (e.op == syntax::TokenKind::kAmpAmp) {
                            walk_expr_(e.a, ExprUse::kEscapeOperand);

                            if (!is_place_expr_(e.a)) {
                                report_(diag::Code::kEscapeOperandMustBePlace, e.span);
                            }

                            if (fn_is_pure_ || fn_is_comptime_) {
                                report_(diag::Code::kTypeEscapeNotAllowedInPureComptime, e.span);
                            }

                            (void)use; // escape 경계/충돌/UAF 정밀 검증은 SIR capability 분석에서 수행한다.
                            return;
                        }

                        walk_expr_(e.a, ExprUse::kValue);
                        return;
                    }

                    case ast::ExprKind::kPostfixUnary:
                        walk_expr_(e.a, ExprUse::kAssignLhs);
                        return;

                    case ast::ExprKind::kBinary:
                        walk_expr_(e.a, ExprUse::kValue);
                        walk_expr_(e.b, ExprUse::kValue);
                        return;

                    case ast::ExprKind::kAssign: {
                        walk_expr_(e.a, ExprUse::kAssignLhs);
                        walk_expr_(e.b, ExprUse::kValue);
                        return;
                    }

                    case ast::ExprKind::kTernary:
                        walk_expr_(e.a, ExprUse::kValue);
                        walk_expr_(e.b, ExprUse::kValue);
                        walk_expr_(e.c, ExprUse::kValue);
                        return;

                    case ast::ExprKind::kCall: {
                        // call 인자에서 생성된 임시 borrow는 call 식 범위에서 정리한다.
                        enter_scope_();
                        walk_expr_(e.a, ExprUse::kValue);
                        const auto& args = ast_.args();
                        const auto& ngs = ast_.named_group_args();

                        for (uint32_t i = 0; i < e.arg_count; ++i) {
                            const auto& a = args[e.arg_begin + i];
                            if (a.kind == ast::ArgKind::kNamedGroup) {
                                for (uint32_t j = 0; j < a.child_count; ++j) {
                                    const auto& na = ngs[a.child_begin + j];
                                    if (!na.is_hole) walk_expr_(na.expr, ExprUse::kCallArg);
                                }
                                continue;
                            }
                            if (!a.is_hole) walk_expr_(a.expr, ExprUse::kCallArg);
                        }
                        leave_scope_();
                        return;
                    }

                    case ast::ExprKind::kArrayLit: {
                        const auto& args = ast_.args();
                        const uint32_t end = e.arg_begin + e.arg_count;
                        if (e.arg_begin < args.size() && end <= args.size()) {
                            for (uint32_t i = 0; i < e.arg_count; ++i) {
                                const auto& a = args[e.arg_begin + i];
                                if (!a.is_hole && a.expr != ast::k_invalid_expr) {
                                    walk_expr_(a.expr, ExprUse::kValue);
                                }
                            }
                        }
                        return;
                    }

                    case ast::ExprKind::kIndex:
                        walk_expr_(e.a, use == ExprUse::kAssignLhs ? ExprUse::kAssignLhs : ExprUse::kValue);
                        walk_expr_(e.b, ExprUse::kValue);
                        return;

                    case ast::ExprKind::kIfExpr:
                        walk_expr_(e.a, ExprUse::kValue);
                        if (is_valid_expr_id_(e.b)) {
                            walk_expr_(e.b, ExprUse::kValue);
                        } else {
                            walk_stmt_((ast::StmtId)e.b);
                        }
                        if (is_valid_expr_id_(e.c)) {
                            walk_expr_(e.c, ExprUse::kValue);
                        } else {
                            walk_stmt_((ast::StmtId)e.c);
                        }
                        return;

                    case ast::ExprKind::kBlockExpr: {
                        const ast::StmtId bs = (ast::StmtId)e.a;
                        walk_stmt_(bs);
                        if (is_valid_expr_id_(e.b)) walk_expr_(e.b, ExprUse::kValue);
                        return;
                    }

                    case ast::ExprKind::kLoop:
                        if (e.loop_iter != ast::k_invalid_expr) walk_expr_(e.loop_iter, ExprUse::kValue);
                        walk_stmt_(e.loop_body);
                        return;

                    case ast::ExprKind::kCast:
                        walk_expr_(e.a, ExprUse::kValue);
                        return;

                    case ast::ExprKind::kIntLit:
                    case ast::ExprKind::kFloatLit:
                    case ast::ExprKind::kStringLit:
                    case ast::ExprKind::kCharLit:
                    case ast::ExprKind::kBoolLit:
                    case ast::ExprKind::kNullLit:
                    case ast::ExprKind::kHole:
                    case ast::ExprKind::kError:
                        return;
                }
            }

            const ast::AstArena& ast_;
            ast::StmtId program_root_ = ast::k_invalid_stmt;
            const passes::NameResolveResult& nres_;
            const tyck::TyckResult& tyck_;
            const ty::TypePool& types_;
            diag::Bag& bag_;

            uint32_t error_count_ = 0;
            uint32_t fn_depth_ = 0;
            bool fn_is_pure_ = false;
            bool fn_is_comptime_ = false;

            std::unordered_map<SymbolId, bool> symbol_is_mut_;
            std::unordered_map<SymbolId, bool> symbol_is_static_;
            std::unordered_map<SymbolId, uint32_t> active_mut_borrow_count_;
            std::unordered_map<SymbolId, uint32_t> active_shared_borrow_count_;
            std::unordered_map<SymbolId, bool> moved_by_escape_;
            std::vector<ScopeState> scopes_;
        };

    } // namespace

    CapabilityResult run_capability_check(
        const ast::AstArena& ast,
        ast::StmtId program_root,
        const passes::NameResolveResult& nres,
        const tyck::TyckResult& tyck,
        const ty::TypePool& types,
        diag::Bag& bag
    ) {
        CapabilityChecker checker(ast, program_root, nres, tyck, types, bag);
        return checker.run();
    }

} // namespace parus::cap
