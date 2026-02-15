// frontend/src/tyck/type_check_entry.cpp
#include <parus/tyck/TypeCheck.hpp>
#include <parus/syntax/TokenKind.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/diag/DiagCode.hpp>
#include "../common/type_check_literals.hpp"

#include <sstream>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>


namespace parus::tyck {

    using K = parus::syntax::TokenKind;
    using detail::ParsedFloatLiteral;
    using detail::ParsedIntLiteral;
    using detail::parse_float_literal_;
    using detail::parse_int_literal_;

    TyckResult TypeChecker::check_program(ast::StmtId program_stmt) {
        // -----------------------------
        // HARD RESET (매 호출 독립 보장)
        // -----------------------------
        result_ = TyckResult{};
        loop_stack_.clear();
        stmt_loop_depth_ = 0;
        fn_ctx_ = FnCtx{};
        pending_int_sym_.clear();
        pending_int_expr_.clear();
        sym_is_mut_.clear();
        acts_default_operator_map_.clear();

        // sym_ “완전 초기화”
        // (기존에 clear() API가 없으니 재생성하는 게 가장 안전)
        sym_ = sema::SymbolTable{};

        // expr type cache: AST exprs 크기에 맞춰 리셋
        expr_type_cache_.assign(ast_.exprs().size(), ty::kInvalidType);
        expr_overload_target_cache_.assign(ast_.exprs().size(), ast::k_invalid_stmt);
        result_.expr_types = expr_type_cache_; // 결과 벡터도 동일 크기로 시작
        result_.expr_overload_target = expr_overload_target_cache_;

        // string literal 타입(필요 시)
        if (string_type_ == ty::kInvalidType) {
            // 지금은 string builtin이 없으니 placeholder로 error() 사용
            string_type_ = types_.error();
        }

        // ------------------------------------------
        // Sanity: program은 Block이어야 한다 (정책)
        // ------------------------------------------
        if (program_stmt == ast::k_invalid_stmt) {
            result_.ok = false;
            return result_;
        }

        const ast::Stmt& root = ast_.stmt(program_stmt);
        if (root.kind != ast::StmtKind::kBlock) {
            if (diag_bag_) diag_(diag::Code::kTopLevelMustBeBlock, root.span);
            result_.ok = false;
            return result_;
        }

        // ---------------------------------------------------------
        // PASS 1: Top-level decl precollect (mutual recursion 지원)
        // - 전역 스코프에 "함수 시그니처 타입(ty::Kind::kFn)"을 먼저 등록한다.
        // - 기존 check_program() 내부의 "invalid 타입으로 insert"하던 루프가
        //   거의 모든 TypeNotCallable 증상의 원인이었으므로 제거하고,
        //   이미 구현된 first_pass_collect_top_level_()를 정식으로 사용한다.
        // ---------------------------------------------------------
        first_pass_collect_top_level_(program_stmt);

        // ---------------------------------------------------------
        // PASS 2: 실제 타입체크
        // - top-level block은 "scope 생성 없이" 자식만 순회한다.
        //   (중요: check_stmt_block_은 scope를 push하므로,
        //    root를 check_stmt_로 보내면 top-level이 새 스코프가 되어
        //    PASS1에서 등록한 전역 심볼이 가려질 수 있다.)
        // ---------------------------------------------------------
        for (uint32_t i = 0; i < root.stmt_count; ++i) {
            const ast::StmtId child_id = ast_.stmt_children()[root.stmt_begin + i];
            check_stmt_(child_id);
            // 에러가 나도 계속 진행(정책)
        }

        // ----------------------------------------
        // Finalize unresolved deferred integers:
        // - If an inferred integer "{integer}" is never consumed in a way that fixes the type,
        //   pick the smallest signed integer type that fits.
        // - Finalization applies to both symbol-backed and expression-backed pending integers.
        // ----------------------------------------
        auto choose_smallest_signed = [&](const num::BigInt& v) -> ty::TypeId {
            ty::Builtin b = ty::Builtin::kI128;
            if      (v.fits_i8())  b = ty::Builtin::kI8;
            else if (v.fits_i16()) b = ty::Builtin::kI16;
            else if (v.fits_i32()) b = ty::Builtin::kI32;
            else if (v.fits_i64()) b = ty::Builtin::kI64;
            return types_.builtin(b);
        };

        for (auto& kv : pending_int_sym_) {
            const uint32_t sym_id = kv.first;
            PendingInt& pi = kv.second;

            if (!pi.has_value || pi.resolved) continue;
            pi.resolved = true;
            pi.resolved_type = choose_smallest_signed(pi.value);
            sym_.update_declared_type(sym_id, pi.resolved_type);
        }

        for (auto& kv : pending_int_expr_) {
            const uint32_t eid = kv.first;
            PendingInt& pi = kv.second;

            if (!pi.has_value || pi.resolved) continue;
            pi.resolved = true;
            pi.resolved_type = choose_smallest_signed(pi.value);

            if (eid < expr_type_cache_.size()) {
                expr_type_cache_[eid] = pi.resolved_type;
            }
        }

        // 결과 반영
        result_.expr_types = expr_type_cache_;
        result_.expr_overload_target = expr_overload_target_cache_;
        return result_;
    }

    // --------------------
    // errors
    // --------------------

    void TypeChecker::diag_(diag::Code code, Span sp) {
        if (!diag_bag_) return;
        result_.ok = false;
        diag::Diagnostic d(diag::Severity::kError, code, sp);
        diag_bag_->add(std::move(d));
    }

    void TypeChecker::diag_(diag::Code code, Span sp, std::string_view a0) {
        if (!diag_bag_) return;
        result_.ok = false;
        diag::Diagnostic d(diag::Severity::kError, code, sp);
        d.add_arg(a0);
        diag_bag_->add(std::move(d));
    }

    void TypeChecker::diag_(diag::Code code, Span sp, std::string_view a0, std::string_view a1) {
        if (!diag_bag_) return;
        result_.ok = false;
        diag::Diagnostic d(diag::Severity::kError, code, sp);
        d.add_arg(a0);
        d.add_arg(a1);
        diag_bag_->add(std::move(d));
    }

    void TypeChecker::diag_(diag::Code code, Span sp, std::string_view a0, std::string_view a1, std::string_view a2) {
        if (!diag_bag_) return;
        result_.ok = false;
        diag::Diagnostic d(diag::Severity::kError, code, sp);
        d.add_arg(a0);
        d.add_arg(a1);
        d.add_arg(a2);
        diag_bag_->add(std::move(d));
    }

    void TypeChecker::err_(Span sp, std::string msg) {
        // 1) TyckResult(errors)
        TyError e{};
        e.span = sp;
        e.message = msg;
        result_.errors.push_back(std::move(e));

        // NOTE:
        // - err_()는 저장용으로만 사용
        // - 사용자 출력은 항상 diag_(Code, args...)만 사용
    }

    // --------------------
    // pass 1: collect top-level decls
    // --------------------
    void TypeChecker::first_pass_collect_top_level_(ast::StmtId program_stmt) {
        const ast::Stmt& prog = ast_.stmt(program_stmt);
        if (prog.kind != ast::StmtKind::kBlock) {
            err_(prog.span, "program root is not a block stmt");
            diag_(diag::Code::kTopLevelMustBeBlock, prog.span);
            return;
        }

        // ensure map is reset for each check_program call
        fn_decl_by_name_.clear();

        // global scope는 SymbolTable이 이미 push되어 있음.
        for (uint32_t i = 0; i < prog.stmt_count; ++i) {
            const ast::StmtId cid = ast_.stmt_children()[prog.stmt_begin + i];
            const ast::Stmt& s = ast_.stmt(cid);

            // ----------------------------
            // top-level fn decl
            // ----------------------------
            if (s.kind == ast::StmtKind::kFnDecl) {
                // overload 집합 구성: 동일 이름 함수는 모두 수집한다.
                if (!s.name.empty()) {
                    fn_decl_by_name_[std::string(s.name)].push_back(cid);
                }

                // 1) 우선 Stmt.type이 fn 시그니처로 이미 채워져 있으면 그대로 사용
                ty::TypeId sig = s.type;

                if (!(sig != ty::kInvalidType && types_.get(sig).kind == ty::Kind::kFn)) {
                    // 2) 아니면 tyck에서 직접 구성
                    ty::TypeId ret = ty::kInvalidType;

                    // (A) s.type가 "반환 타입"으로 들어온 케이스 fallback
                    if (sig != ty::kInvalidType && types_.get(sig).kind != ty::Kind::kFn) {
                        ret = sig;
                    }
                    if (ret == ty::kInvalidType) ret = types_.error();

                    // IMPORTANT:
                    // signature params should be positional-only.
                    // If parser already filled s.positional_param_count, use it.
                    uint32_t pos_cnt = 0;
                    if (s.positional_param_count != 0 || s.param_count == 0) {
                        pos_cnt = s.positional_param_count;
                    } else {
                        // fallback for older AST: assume all are positional
                        pos_cnt = s.param_count;
                    }

                    std::vector<ty::TypeId> params;
                    params.reserve(pos_cnt);

                    for (uint32_t pi = 0; pi < pos_cnt; ++pi) {
                        const auto& p = ast_.params()[s.param_begin + pi];
                        ty::TypeId pt = p.type;

                        if (pt == ty::kInvalidType) {
                            err_(p.span, "parameter requires an explicit type");
                            diag_(diag::Code::kTypeParamTypeRequired, p.span, p.name);
                            pt = types_.error();
                        }
                        params.push_back(pt);
                    }

                    sig = types_.make_fn(ret, params.empty() ? nullptr : params.data(), (uint32_t)params.size());
                }

                // 함수 오버로딩 허용:
                // - 같은 이름의 기존 심볼이 함수면 "오버로드 집합 대표 심볼"을 재사용한다.
                // - 같은 이름의 기존 심볼이 함수가 아니면 중복 선언 에러다.
                if (auto existing = sym_.lookup_in_current(s.name)) {
                    const auto& existing_sym = sym_.symbol(*existing);
                    if (existing_sym.kind != sema::SymbolKind::kFn) {
                        err_(s.span, "duplicate symbol (function): " + std::string(s.name));
                        diag_(diag::Code::kDuplicateDecl, s.span, s.name);
                    }
                } else {
                    auto ins = sym_.insert(sema::SymbolKind::kFn, s.name, sig, s.span);
                    if (!ins.ok && ins.is_duplicate) {
                        err_(s.span, "duplicate symbol (function): " + std::string(s.name));
                        diag_(diag::Code::kDuplicateDecl, s.span, s.name);
                    }
                }
                continue;
            }

            // ----------------------------
            // top-level var decl
            // ----------------------------
            if (s.kind == ast::StmtKind::kVar) {
                // top-level var는 precollect 대상이 아니다.
                // 실제 선언/타입 확정은 PASS2(check_stmt_var_)에서 한 번만 처리한다.
                continue;
            }

            // ----------------------------
            // top-level field decl
            // ----------------------------
            if (s.kind == ast::StmtKind::kFieldDecl) {
                auto ins = sym_.insert(sema::SymbolKind::kField, s.name, ty::kInvalidType, s.span);
                if (!ins.ok && ins.is_duplicate) {
                    err_(s.span, "duplicate symbol (field): " + std::string(s.name));
                    diag_(diag::Code::kDuplicateDecl, s.span, s.name);
                }
                continue;
            }

            // ----------------------------
            // top-level acts decl
            // ----------------------------
            if (s.kind == ast::StmtKind::kActsDecl) {
                auto ins = sym_.insert(sema::SymbolKind::kAct, s.name, ty::kInvalidType, s.span);
                if (!ins.ok && ins.is_duplicate) {
                    err_(s.span, "duplicate symbol (acts): " + std::string(s.name));
                    diag_(diag::Code::kDuplicateDecl, s.span, s.name);
                }
                collect_acts_operator_decl_(s);
                continue;
            }

            // use / unknown / other: pass1에서는 스킵
        }

        struct ParamShape {
            std::string label;
            ty::TypeId type = ty::kInvalidType;
            bool has_default = false;
            Span span{};
        };
        struct DeclShape {
            ast::StmtId sid = ast::k_invalid_stmt;
            std::string_view name{};
            Span span{};
            ty::TypeId ret = ty::kInvalidType;
            std::vector<ParamShape> positional;
            std::vector<ParamShape> named;
        };

        /// @brief 선언 키(Declaration Key)를 canonical 문자열로 만든다.
        const auto make_decl_key = [](const DeclShape& d) -> std::string {
            std::ostringstream oss;
            oss << "P" << d.positional.size();
            for (const auto& p : d.positional) {
                oss << "|" << p.label << ":" << p.type;
            }
            oss << "|N" << d.named.size();
            for (const auto& p : d.named) {
                oss << "|" << p.label << ":" << p.type << ":" << (p.has_default ? "opt" : "req");
            }
            return oss.str();
        };

        /// @brief 위치 호출 관점(타입열만) 비교 키를 만든다.
        const auto make_positional_type_key = [](const DeclShape& d) -> std::string {
            std::ostringstream oss;
            oss << "P" << d.positional.size();
            for (const auto& p : d.positional) {
                oss << "|" << p.type;
            }
            return oss.str();
        };

        /// @brief labeled 호출 관점(라벨+타입 집합) 비교 키를 만든다.
        const auto make_labeled_set_key = [](const DeclShape& d) -> std::string {
            std::vector<std::pair<std::string, ty::TypeId>> elems;
            elems.reserve(d.positional.size());
            for (const auto& p : d.positional) {
                elems.emplace_back(p.label, p.type);
            }
            std::sort(elems.begin(), elems.end(), [](const auto& a, const auto& b) {
                if (a.first != b.first) return a.first < b.first;
                return a.second < b.second;
            });

            std::ostringstream oss;
            oss << "L" << elems.size();
            for (const auto& e : elems) {
                oss << "|" << e.first << ":" << e.second;
            }
            return oss.str();
        };

        // 오버로드 집합 단위 검증
        for (const auto& it : fn_decl_by_name_) {
            const std::string& fn_name = it.first;
            const std::vector<ast::StmtId>& decl_ids = it.second;
            if (decl_ids.empty()) continue;

            std::vector<DeclShape> decls;
            decls.reserve(decl_ids.size());

            for (const ast::StmtId sid : decl_ids) {
                const ast::Stmt& s = ast_.stmt(sid);
                if (s.kind != ast::StmtKind::kFnDecl) continue;

                DeclShape d{};
                d.sid = sid;
                d.name = s.name;
                d.span = s.span;
                d.ret = (s.fn_ret != ty::kInvalidType)
                    ? s.fn_ret
                    : ((s.type != ty::kInvalidType && types_.get(s.type).kind == ty::Kind::kFn)
                        ? types_.get(s.type).ret
                        : types_.error());

                const uint32_t total = s.param_count;
                uint32_t pos_cnt = s.positional_param_count;
                if (pos_cnt > total) pos_cnt = total;

                std::unordered_set<std::string> seen_labels;
                seen_labels.reserve(total);

                for (uint32_t i = 0; i < total; ++i) {
                    const auto& p = ast_.params()[s.param_begin + i];
                    ParamShape ps{};
                    ps.label = std::string(p.name);
                    ps.type = p.type;
                    ps.has_default = p.has_default;
                    ps.span = p.span;

                    // 스펙 6.1.7(C)-5: 함수 내부 파라미터 라벨 중복 금지
                    if (!seen_labels.insert(ps.label).second) {
                        std::string msg = "duplicate parameter label '" + ps.label +
                                        "' in overload declaration of '" + fn_name + "'";
                        diag_(diag::Code::kTypeErrorGeneric, ps.span, msg);
                        err_(ps.span, msg);
                    }

                    const bool is_named = (i >= pos_cnt) || p.is_named_group;
                    if (is_named) d.named.push_back(std::move(ps));
                    else d.positional.push_back(std::move(ps));
                }

                decls.push_back(std::move(d));
            }

            if (decls.size() <= 1) continue;

            // 스펙 6.1.7(C)-1/2: Declaration Key 충돌 금지 (반환 타입만 차이도 포함)
            std::unordered_map<std::string, size_t> decl_key_owner;
            decl_key_owner.reserve(decls.size());
            for (size_t i = 0; i < decls.size(); ++i) {
                const std::string key = make_decl_key(decls[i]);
                auto ins = decl_key_owner.emplace(key, i);
                if (!ins.second) {
                    const auto& prev = decls[ins.first->second];
                    std::string msg;
                    if (prev.ret != decls[i].ret) {
                        msg = "overload conflict in '" + fn_name +
                            "': return-type-only overloading is not allowed";
                    } else {
                        msg = "overload conflict in '" + fn_name +
                            "': declaration key collision";
                    }
                    diag_(diag::Code::kTypeErrorGeneric, decls[i].span, msg);
                    err_(decls[i].span, msg);
                }
            }

            // 스펙 6.1.7(C)-3: positional 호출 관점 충돌 금지 (named-group 없는 함수끼리)
            std::unordered_map<std::string, size_t> pos_view_owner;
            pos_view_owner.reserve(decls.size());
            for (size_t i = 0; i < decls.size(); ++i) {
                if (!decls[i].named.empty()) continue;
                const std::string key = make_positional_type_key(decls[i]);
                auto ins = pos_view_owner.emplace(key, i);
                if (!ins.second) {
                    std::string msg = "overload conflict in '" + fn_name +
                        "': positional-call view is indistinguishable";
                    diag_(diag::Code::kTypeErrorGeneric, decls[i].span, msg);
                    err_(decls[i].span, msg);
                }
            }

            // 스펙 6.1.7(C)-4: labeled 호출 관점 충돌 금지 (named-group 없는 함수끼리)
            std::unordered_map<std::string, size_t> labeled_view_owner;
            labeled_view_owner.reserve(decls.size());
            for (size_t i = 0; i < decls.size(); ++i) {
                if (!decls[i].named.empty()) continue;
                const std::string key = make_labeled_set_key(decls[i]);
                auto ins = labeled_view_owner.emplace(key, i);
                if (!ins.second) {
                    std::string msg = "overload conflict in '" + fn_name +
                        "': labeled-call view is indistinguishable";
                    diag_(diag::Code::kTypeErrorGeneric, decls[i].span, msg);
                    err_(decls[i].span, msg);
                }
            }
        }
    }

    /// @brief acts operator 조회용 키를 생성한다.
    uint64_t TypeChecker::acts_operator_key_(ty::TypeId owner_type, syntax::TokenKind op_token, bool is_postfix) {
        const uint64_t owner = static_cast<uint64_t>(owner_type);
        const uint64_t op = static_cast<uint64_t>(op_token);
        const uint64_t pf = is_postfix ? 1ull : 0ull;
        return (owner << 32) | (op << 1) | pf;
    }

    /// @brief 실제 파라미터 타입이 acts owner 타입과 호환되는지 판정한다.
    bool TypeChecker::type_matches_acts_owner_(const ty::TypePool& types, ty::TypeId owner, ty::TypeId actual) {
        if (owner == ty::kInvalidType || actual == ty::kInvalidType) return false;
        if (owner == actual) return true;
        const auto& at = types.get(actual);
        if (at.kind == ty::Kind::kBorrow) {
            return at.elem == owner;
        }
        return false;
    }

    /// @brief acts decl 하나에서 기본 acts(`acts for T`) operator 멤버를 인덱싱한다.
    void TypeChecker::collect_acts_operator_decl_(const ast::Stmt& acts_decl) {
        if (!acts_decl.acts_is_for) return;
        if (acts_decl.acts_has_set_name) return; // v0: named-set operator는 추후 활성화 규칙과 함께 처리
        if (acts_decl.acts_target_type == ty::kInvalidType) return;

        const auto& kids = ast_.stmt_children();
        const uint32_t begin = acts_decl.stmt_begin;
        const uint32_t end = acts_decl.stmt_begin + acts_decl.stmt_count;
        if (begin >= kids.size() || end > kids.size()) return;

        for (uint32_t i = begin; i < end; ++i) {
            const ast::StmtId sid = kids[i];
            if (sid == ast::k_invalid_stmt) continue;
            const auto& member = ast_.stmt(sid);
            if (member.kind != ast::StmtKind::kFnDecl || !member.fn_is_operator) continue;

            // 규칙 검증: operator의 첫 파라미터는 self 리시버여야 한다.
            if (member.param_count == 0) {
                diag_(diag::Code::kOperatorSelfFirstParamRequired, member.span);
                err_(member.span, "operator declaration requires a self receiver parameter");
                continue;
            }
            const auto& first = ast_.params()[member.param_begin];
            if (!first.is_self) {
                diag_(diag::Code::kOperatorSelfFirstParamRequired, first.span);
                err_(first.span, "operator declaration requires 'self' on first parameter");
                continue;
            }
            if (!type_matches_acts_owner_(types_, acts_decl.acts_target_type, first.type)) {
                std::string msg = "operator self type must match acts target type";
                diag_(diag::Code::kTypeErrorGeneric, first.span, msg);
                err_(first.span, msg);
                continue;
            }

            const uint64_t key = acts_operator_key_(
                acts_decl.acts_target_type,
                member.fn_operator_token,
                member.fn_operator_is_postfix
            );
            acts_default_operator_map_[key].push_back(ActsOperatorDecl{
                .fn_sid = sid,
                .owner_type = acts_decl.acts_target_type,
                .op_token = member.fn_operator_token,
                .is_postfix = member.fn_operator_is_postfix,
            });
        }
    }

    /// @brief binary operator에 대응되는 기본 acts overload를 찾는다.
    ast::StmtId TypeChecker::resolve_binary_operator_overload_(syntax::TokenKind op, ty::TypeId lhs, ty::TypeId rhs) const {
        const uint64_t key = acts_operator_key_(lhs, op, /*is_postfix=*/false);
        auto it = acts_default_operator_map_.find(key);
        if (it == acts_default_operator_map_.end()) return ast::k_invalid_stmt;

        ast::StmtId selected = ast::k_invalid_stmt;
        for (const auto& decl : it->second) {
            const auto& fn = ast_.stmt(decl.fn_sid);
            if (fn.kind != ast::StmtKind::kFnDecl) continue;
            if (fn.param_count < 2) continue;

            const auto& p0 = ast_.params()[fn.param_begin + 0];
            const auto& p1 = ast_.params()[fn.param_begin + 1];
            if (!can_assign_(p0.type, lhs)) continue;
            if (!can_assign_(p1.type, rhs)) continue;

            if (selected != ast::k_invalid_stmt) {
                // 중복 후보는 모호성으로 보고 해소 실패 처리
                return ast::k_invalid_stmt;
            }
            selected = decl.fn_sid;
        }
        return selected;
    }

    /// @brief postfix operator(++ 등)에 대응되는 기본 acts overload를 찾는다.
    ast::StmtId TypeChecker::resolve_postfix_operator_overload_(syntax::TokenKind op, ty::TypeId lhs) const {
        const uint64_t key = acts_operator_key_(lhs, op, /*is_postfix=*/true);
        auto it = acts_default_operator_map_.find(key);
        if (it == acts_default_operator_map_.end()) return ast::k_invalid_stmt;

        ast::StmtId selected = ast::k_invalid_stmt;
        for (const auto& decl : it->second) {
            const auto& fn = ast_.stmt(decl.fn_sid);
            if (fn.kind != ast::StmtKind::kFnDecl) continue;
            if (fn.param_count < 1) continue;

            const auto& p0 = ast_.params()[fn.param_begin + 0];
            if (!can_assign_(p0.type, lhs)) continue;

            if (selected != ast::k_invalid_stmt) {
                return ast::k_invalid_stmt;
            }
            selected = decl.fn_sid;
        }
        return selected;
    }

    bool TypeChecker::fits_builtin_int_big_(const parus::num::BigInt& v, parus::ty::Builtin dst) {
        using B = parus::ty::Builtin;
        switch (dst) {
            case B::kI8:   return v.fits_i8();
            case B::kI16:  return v.fits_i16();
            case B::kI32:  return v.fits_i32();
            case B::kI64:  return v.fits_i64();
            case B::kI128: return v.fits_i128();

            case B::kU8:   return v.fits_u8();
            case B::kU16:  return v.fits_u16();
            case B::kU32:  return v.fits_u32();
            case B::kU64:  return v.fits_u64();
            case B::kU128: return v.fits_u128();

            // isize/usize는 타겟 포인터폭에 의존.
            // v0: 우선 64-bit로 가정하거나(네 프로젝트가 x86_64 우선이니까),
            // 추후 TargetConfig로 분리.
            case B::kISize: return v.fits_i64();
            case B::kUSize: return v.fits_u64();

            default: return false;
        }
    }

    /// @brief field 멤버로 허용할 POD 값 내장 타입인지 판정한다.
    bool TypeChecker::is_field_pod_value_type_(const ty::TypePool& types, ty::TypeId id) {
        if (id == ty::kInvalidType) return false;
        const auto& t = types.get(id);
        if (t.kind != ty::Kind::kBuiltin) return false;

        using B = ty::Builtin;
        switch (t.builtin) {
            case B::kBool:
            case B::kChar:
            case B::kI8:
            case B::kI16:
            case B::kI32:
            case B::kI64:
            case B::kI128:
            case B::kU8:
            case B::kU16:
            case B::kU32:
            case B::kU64:
            case B::kU128:
            case B::kISize:
            case B::kUSize:
            case B::kF32:
            case B::kF64:
            case B::kF128:
                return true;

            case B::kNull:
            case B::kUnit:
            case B::kNever:
            case B::kInferInteger:
                return false;
        }

        return false;
    }

    bool TypeChecker::infer_int_value_of_expr_(ast::ExprId eid, num::BigInt& out) const {
        auto it = pending_int_expr_.find((uint32_t)eid);
        if (it != pending_int_expr_.end() && it->second.has_value) {
            out = it->second.value;
            return true;
        }

        const ast::Expr& e = ast_.expr(eid);
        if (e.kind == ast::ExprKind::kIntLit) {
            const ParsedIntLiteral lit = parse_int_literal_(e.text);
            if (!lit.ok) return false;
            return num::BigInt::parse_dec(lit.digits_no_sep, out);
        }

        // ident의 경우: sym pending에서 찾아온다
        if (e.kind == ast::ExprKind::kIdent) {
            auto sid = sym_.lookup(e.text);
            if (!sid) return false;
            auto it2 = pending_int_sym_.find(*sid);
            if (it2 != pending_int_sym_.end() && it2->second.has_value) {
                out = it2->second.value;
                return true;
            }
        }

        return false;
    }

    bool TypeChecker::resolve_infer_int_in_context_(ast::ExprId eid, ty::TypeId expected) {
        if (eid == ast::k_invalid_expr) return false;

        const auto& et = types_.get(expected);

        // ------------------------------------------------------------
        // (0) aggregate context: array
        // - expected가 array인 경우, array literal의 각 원소로 컨텍스트를 내려준다.
        // - 원소 타입에 {integer}가 포함된 경우에만 재귀 해소를 시도한다.
        // ------------------------------------------------------------
        if (et.kind == ty::Kind::kArray) {
            const ast::Expr& e = ast_.expr(eid);
            if (e.kind != ast::ExprKind::kArrayLit) return false;
            if (et.array_has_size && e.arg_count != et.array_size) return false;

            auto type_contains_infer_int = [&](ty::TypeId tid, const auto& self) -> bool {
                if (tid == ty::kInvalidType) return false;
                const auto& tt = types_.get(tid);
                switch (tt.kind) {
                    case ty::Kind::kBuiltin:
                        return tt.builtin == ty::Builtin::kInferInteger;
                    case ty::Kind::kOptional:
                    case ty::Kind::kArray:
                    case ty::Kind::kBorrow:
                    case ty::Kind::kEscape:
                        return self(tt.elem, self);
                    default:
                        return false;
                }
            };

            bool ok_all = true;
            const auto& args = ast_.args();
            const uint32_t end = e.arg_begin + e.arg_count;
            if (e.arg_begin >= args.size() || end > args.size()) return false;

            for (uint32_t i = 0; i < e.arg_count; ++i) {
                const auto& a = args[e.arg_begin + i];
                if (a.expr == ast::k_invalid_expr) continue;

                ty::TypeId elem_t = check_expr_(a.expr);
                if (!type_contains_infer_int(elem_t, type_contains_infer_int)) continue;

                if (!resolve_infer_int_in_context_(a.expr, et.elem)) {
                    ok_all = false;
                }
            }

            if (ok_all) {
                if ((size_t)eid < expr_type_cache_.size()) {
                    expr_type_cache_[eid] = expected;
                }
                return true;
            }
            return false;
        }

        // expected는 builtin int여야 한다.
        if (et.kind != ty::Kind::kBuiltin) return false;

        // float 컨텍스트면 즉시 에러 (암시적 int->float 금지)
        if (et.builtin == ty::Builtin::kF32 || et.builtin == ty::Builtin::kF64 || et.builtin == ty::Builtin::kF128) {
            diag_(diag::Code::kIntToFloatNotAllowed, ast_.expr(eid).span, types_.to_string(expected));
            return false;
        }

        auto is_int_builtin = [&](ty::Builtin b) -> bool {
            return b == ty::Builtin::kI8 || b == ty::Builtin::kI16 || b == ty::Builtin::kI32 ||
                b == ty::Builtin::kI64 || b == ty::Builtin::kI128 ||
                b == ty::Builtin::kU8 || b == ty::Builtin::kU16 || b == ty::Builtin::kU32 ||
                b == ty::Builtin::kU64 || b == ty::Builtin::kU128 ||
                b == ty::Builtin::kISize || b == ty::Builtin::kUSize;
        };
        if (!is_int_builtin(et.builtin)) return false;

        const ast::Expr& e = ast_.expr(eid);

        // ------------------------------------------------------------
        // (1) 합성 표현식: expected를 "아래로" 전파해서 내부 {integer}를 확정한다.
        //     - if-expr: then/else로 전파
        //     - ternary: b/c로 전파
        //     - block-expr: tail로 전파
        //
        // 여기서 중요한 점:
        // - 이 expr 자체에서 "정수 literal 값"을 뽑으려고 하면 안 된다.
        // - 내부 리터럴들이 fit+resolve만 되면 상위 expr은 자연히 expected 타입으로 수렴한다.
        // ------------------------------------------------------------
        auto mark_resolved_here = [&](bool has_value, const num::BigInt& v) {
            auto& pe = pending_int_expr_[(uint32_t)eid];
            if (has_value) {
                pe.value = v;
                pe.has_value = true;
            }
            pe.resolved = true;
            pe.resolved_type = expected;
            if ((size_t)eid < expr_type_cache_.size()) {
                expr_type_cache_[eid] = expected;
            }
        };

        switch (e.kind) {
            case ast::ExprKind::kIfExpr: {
                bool ok_then = (e.b != ast::k_invalid_expr) ? resolve_infer_int_in_context_(e.b, expected) : true;
                bool ok_else = (e.c != ast::k_invalid_expr) ? resolve_infer_int_in_context_(e.c, expected) : true;

                if (ok_then && ok_else) {
                    // if-expr 자체는 "값"을 직접 가지지 않으므로 value는 기록하지 않는다.
                    mark_resolved_here(/*has_value=*/false, num::BigInt{});
                    return true;
                }
                // branch 중 하나라도 해소 실패하면: 여기서 "컨텍스트 없음" 진단은 내지 말고 그냥 실패 리턴.
                // (실제 원인은 내부에서 fit 실패/unknown 등의 진단으로 이미 찍힌다.)
                return false;
            }

            case ast::ExprKind::kTernary: {
                bool ok_b = (e.b != ast::k_invalid_expr) ? resolve_infer_int_in_context_(e.b, expected) : true;
                bool ok_c = (e.c != ast::k_invalid_expr) ? resolve_infer_int_in_context_(e.c, expected) : true;

                if (ok_b && ok_c) {
                    mark_resolved_here(/*has_value=*/false, num::BigInt{});
                    return true;
                }
                return false;
            }

            case ast::ExprKind::kBlockExpr: {
                // mapping assumption in your code:
                // - e.a: StmtId of block
                // - e.b: tail ExprId (optional)
                if (e.b != ast::k_invalid_expr) {
                    bool ok_tail = resolve_infer_int_in_context_(e.b, expected);
                    if (ok_tail) {
                        mark_resolved_here(/*has_value=*/false, num::BigInt{});
                        return true;
                    }
                    return false;
                }
                // tail이 없으면 null로 수렴하므로 integer expected로는 해소 불가
                return false;
            }

            default:
                break;
        }

        // ------------------------------------------------------------
        // (2) 리프/값 추적 가능한 케이스: IntLit / Ident({integer})
        // ------------------------------------------------------------
        num::BigInt v;
        if (!infer_int_value_of_expr_(eid, v)) {
            // 값이 없으면(예: 연산을 거쳐 값 추적이 불가) 컨텍스트만으로는 확정 불가
            // 단, 위의 합성 expr들은 여기로 오지 않게 했으니, 이 진단은 "진짜 리프 해소 실패"에만 뜬다.
            diag_(diag::Code::kIntLiteralNeedsTypeContext, e.span);
            return false;
        }

        if (!fits_builtin_int_big_(v, et.builtin)) {
            diag_(diag::Code::kIntLiteralDoesNotFit, e.span,
                types_.to_string(expected), v.to_string(64));
            return false;
        }

        // ident라면 심볼 타입 확정 반영
        if (e.kind == ast::ExprKind::kIdent) {
            auto sid = sym_.lookup(e.text);
            if (sid) {
                const auto& st = types_.get(sym_.symbol(*sid).declared_type);
                if (st.kind == ty::Kind::kBuiltin && st.builtin == ty::Builtin::kInferInteger) {
                    sym_.update_declared_type(*sid, expected);
                    auto it = pending_int_sym_.find(*sid);
                    if (it != pending_int_sym_.end()) {
                        it->second.resolved = true;
                        it->second.resolved_type = expected;
                    }
                }
            }
        }

        // expr pending resolved 표시
        {
            auto& pe = pending_int_expr_[(uint32_t)eid];
            pe.value = v;
            pe.has_value = true;
            pe.resolved = true;
            pe.resolved_type = expected;
        }

        if ((size_t)eid < expr_type_cache_.size()) {
            expr_type_cache_[eid] = expected;
        }

        return true;
    }

    // --------------------
    // pass 2: check
    // --------------------
    void TypeChecker::second_pass_check_program_(ast::StmtId program_stmt) {
        check_stmt_(program_stmt);

        // ----------------------------------------
        // Finalize unresolved deferred integers:
        // - If an inferred integer "{integer}" is never consumed in a way that fixes the type,
        //   we pick the smallest signed integer type that fits (i8..i128).
        // - This keeps DX friendly and avoids leaving IR in an unresolved state.
        // ----------------------------------------
        for (auto& kv : pending_int_sym_) {
            const uint32_t sym_id = kv.first;
            PendingInt& pi = kv.second;

            if (!pi.has_value) continue;
            if (pi.resolved) continue;

            // pick smallest signed type
            ty::Builtin b = ty::Builtin::kI128;
            if      (pi.value.fits_i8())   b = ty::Builtin::kI8;
            else if (pi.value.fits_i16())  b = ty::Builtin::kI16;
            else if (pi.value.fits_i32())  b = ty::Builtin::kI32;
            else if (pi.value.fits_i64())  b = ty::Builtin::kI64;
            else                           b = ty::Builtin::kI128;

            pi.resolved = true;
            pi.resolved_type = types_.builtin(b);

            // NEW: SymbolTable에 확정 타입 반영
            sym_.update_declared_type(sym_id, pi.resolved_type);
        }
    }

    // --------------------
    // stmt dispatch
    // --------------------

} // namespace parus::tyck
