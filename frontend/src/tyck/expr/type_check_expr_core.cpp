// frontend/src/tyck/type_check_expr_core.cpp
#include <parus/tyck/TypeCheck.hpp>
#include <parus/syntax/TokenKind.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/diag/DiagCode.hpp>
#include "../common/type_check_literals.hpp"

#include <sstream>
#include <unordered_map>

namespace parus::tyck {

    namespace {
        /// @brief 타입이 borrow(`&T`/`&mut T`)인지 판정하고, 내부 요소 타입/가변 여부를 반환한다.
        bool borrow_info_(
            const ty::TypePool& types,
            ty::TypeId t,
            ty::TypeId& out_elem,
            bool& out_is_mut
        ) {
            if (t == ty::kInvalidType) return false;
            if (t >= types.count()) return false;
            const auto& tt = types.get(t);
            if (tt.kind != ty::Kind::kBorrow) return false;
            out_elem = tt.elem;
            out_is_mut = tt.borrow_is_mut;
            return true;
        }

        /// @brief 값 읽기 컨텍스트에서 borrow 타입을 요소 타입으로 1단계 디케이한다.
        ty::TypeId read_decay_borrow_(const ty::TypePool& types, ty::TypeId t) {
            ty::TypeId elem = ty::kInvalidType;
            bool is_mut = false;
            if (!borrow_info_(types, t, elem, is_mut)) return t;
            (void)is_mut;
            return elem;
        }
    } // namespace

    
    using K = parus::syntax::TokenKind;
    using detail::ParsedFloatLiteral;
    using detail::ParsedIntLiteral;
    using detail::parse_float_literal_;
    using detail::parse_int_literal_;

    ty::TypeId TypeChecker::check_expr_(ast::ExprId eid, Slot slot) {
        if (eid == ast::k_invalid_expr) return types_.error();
        if (eid >= expr_type_cache_.size()) return types_.error();

        const ast::Expr& e = ast_.expr(eid);
        const ast::ExprId saved_expr_id = current_expr_id_;
        current_expr_id_ = eid;

        // NOTE(slot-sensitive caching)
        // - 일부 expr는 "Value vs Discard" 컨텍스트에 따라 진단/타입 규칙이 달라질 수 있다.
        // - 특히 block-expr는 Slot::kValue에서 tail이 없으면 에러를 내야 한다.
        //   그런데 Discard 컨텍스트에서 먼저 체크되며 캐시에 타입이 박히면,
        //   나중에 Value 컨텍스트에서 재방문해도 진단이 발생하지 않는 버그가 생긴다.
        //
        // 정책:
        // - Slot에 의존하는 expr(kind)가 있다면, 그 expr는 "Value에서만 캐시"하거나
        //   아예 캐시를 우회해서 컨텍스트별로 재검사한다.
        const bool slot_sensitive =
            (e.kind == ast::ExprKind::kBlockExpr); // v0: currently only block-expr depends on slot

        // memoized
        // - slot-sensitive expr는 Value에서만 캐시를 신뢰한다.
        if (!slot_sensitive || slot == Slot::kValue) {
            if (expr_type_cache_[eid] != ty::kInvalidType) {
                current_expr_id_ = saved_expr_id;
                return expr_type_cache_[eid];
            }
        }

        ty::TypeId t = types_.error();

        switch (e.kind) {
            case ast::ExprKind::kError:
                t = types_.error();
                break;

            case ast::ExprKind::kIntLit: {
                const ParsedIntLiteral lit = parse_int_literal_(e.text);
                if (!lit.ok) {
                    diag_(diag::Code::kIntLiteralInvalid, e.span, e.text);
                    err_(e.span, "invalid integer literal");
                    t = types_.error();
                    break;
                }

                num::BigInt v;
                if (!num::BigInt::parse_dec(lit.digits_no_sep, v)) {
                    diag_(diag::Code::kIntLiteralInvalid, e.span, e.text);
                    err_(e.span, "invalid integer literal");
                    t = types_.error();
                    break;
                }

                if (lit.has_suffix) {
                    t = types_.builtin(lit.suffix);
                    if (!fits_builtin_int_big_(v, lit.suffix)) {
                        diag_(diag::Code::kIntLiteralOverflow, e.span, e.text, types_.to_string(t));
                        err_(e.span, "integer literal overflow");
                        t = types_.error();
                    }
                } else {
                    t = types_.builtin(ty::Builtin::kInferInteger);
                    PendingInt pi{};
                    pi.value = v;
                    pi.has_value = true;
                    pi.resolved = false;
                    pi.resolved_type = ty::kInvalidType;
                    pending_int_expr_[(uint32_t)eid] = pi;
                }
                break;
            }

            case ast::ExprKind::kFloatLit: {
                const ParsedFloatLiteral lit = parse_float_literal_(e.text);
                if (!lit.ok) {
                    diag_(diag::Code::kTypeErrorGeneric, e.span, "invalid float literal");
                    err_(e.span, "invalid float literal");
                    t = types_.error();
                    break;
                }

                t = types_.builtin(lit.builtin);
                break;
            }

            case ast::ExprKind::kStringLit:
                t = string_type_;
                break;

            case ast::ExprKind::kCharLit:
                t = types_.builtin(ty::Builtin::kChar);
                break;

            case ast::ExprKind::kBoolLit:
                t = types_.builtin(ty::Builtin::kBool);
                break;

            case ast::ExprKind::kNullLit:
                t = types_.builtin(ty::Builtin::kNull);
                break;

            case ast::ExprKind::kIdent: {
                auto id = lookup_symbol_(e.text);
                if (!id) {
                    diag_(diag::Code::kUndefinedName, e.span, e.text);
                    err_(e.span, "unknown identifier");
                    t = types_.error();
                } else {
                    t = sym_.symbol(*id).declared_type;
                    if (t == ty::kInvalidType) t = types_.error();

                    const auto& tt = types_.get(t);
                    if (tt.kind == ty::Kind::kBuiltin && tt.builtin == ty::Builtin::kInferInteger) {
                        auto it = pending_int_sym_.find(*id);
                        if (it != pending_int_sym_.end() && it->second.has_value) {
                            pending_int_expr_[(uint32_t)eid] = it->second;
                        }
                    }
                }
                break;
            }

            case ast::ExprKind::kHole:
                err_(e.span, "unresolved hole '_' in expression");
                diag_(diag::Code::kTypeUnresolvedHole, e.span);
                t = types_.error();
                break;

            case ast::ExprKind::kUnary:
                t = check_expr_unary_(e);
                break;

            case ast::ExprKind::kPostfixUnary:
                t = check_expr_postfix_unary_(e);
                break;

            case ast::ExprKind::kBinary:
                t = check_expr_binary_(e);
                break;

            case ast::ExprKind::kAssign:
                t = check_expr_assign_(e);
                break;

            case ast::ExprKind::kTernary:
                t = check_expr_ternary_(e);
                break;

            case ast::ExprKind::kCall:
                t = check_expr_call_(e);
                break;

            case ast::ExprKind::kIndex:
                t = check_expr_index_(e);
                break;

            case ast::ExprKind::kArrayLit:
                t = check_expr_array_lit_(e);
                break;

            case ast::ExprKind::kIfExpr:
                t = check_expr_if_(e, slot);
                break;

            case ast::ExprKind::kBlockExpr:
                t = check_expr_block_(e, slot);
                break;

            case ast::ExprKind::kLoop:
                t = check_expr_loop_(e, slot);
                break;

            case ast::ExprKind::kCast:
                t = check_expr_cast_(e);
                break;
        }

        // caching policy:
        // - slot-sensitive expr는 Value 컨텍스트에서만 캐시한다.
        //   (Discard에서의 결과를 캐시하면, 나중에 Value에서 필요한 진단이 누락될 수 있다.)
        if (!slot_sensitive || slot == Slot::kValue) {
            expr_type_cache_[eid] = t;
        }

        current_expr_id_ = saved_expr_id;
        return t;
    }

    ty::TypeId TypeChecker::check_expr_(ast::ExprId eid) {
        return check_expr_(eid, Slot::kValue);
    }

    // --------------------
    // helpers: type predicates
    // --------------------
    bool TypeChecker::is_optional_(ty::TypeId t) const {
        if (t == ty::kInvalidType) return false;
        return types_.get(t).kind == ty::Kind::kOptional;
    }

    ty::TypeId TypeChecker::optional_elem_(ty::TypeId opt) const {
        if (!is_optional_(opt)) return ty::kInvalidType;
        return types_.get(opt).elem;
    }

    bool TypeChecker::is_null_(ty::TypeId t) const {
        return t == types_.builtin(ty::Builtin::kNull);
    }

    bool TypeChecker::is_error_(ty::TypeId t) const {
        return t == types_.error() || types_.get(t).kind == ty::Kind::kError;
    }

    void TypeChecker::note_break_(ty::TypeId t, bool is_value_break) {
        if (loop_stack_.empty()) return;

        LoopCtx& lc = loop_stack_.back();
        lc.has_any_break = true;

        if (!is_value_break) {
            lc.has_null_break = true;
            return;
        }

        lc.has_value_break = true;

        if (lc.joined_value == ty::kInvalidType) {
            lc.joined_value = t;
        } else {
            lc.joined_value = unify_(lc.joined_value, t);
        }
    }

    bool TypeChecker::can_assign_(ty::TypeId dst, ty::TypeId src) const {
        if (is_error_(dst) || is_error_(src)) return true;
        if (dst == src) return true;

        // never -> anything (bottom type)
        if (src == types_.builtin(ty::Builtin::kNever)) return true;
        // only never can be assigned into never
        if (dst == types_.builtin(ty::Builtin::kNever)) return src == dst;

        // null -> T? 허용
        if (is_null_(src) && is_optional_(dst)) return true;

        const auto& dt = types_.get(dst);
        const auto& st = types_.get(src);

        // array assignability (v0)
        // - elem type must be assignable
        // - dst T[] accepts src T[] and src T[N]
        // - dst T[N] accepts only src T[N] with same N
        if (dt.kind == ty::Kind::kArray && st.kind == ty::Kind::kArray) {
            if (dt.array_has_size) {
                if (!st.array_has_size) return false;
                if (dt.array_size != st.array_size) return false;
            }
            return can_assign_(dt.elem, st.elem);
        }

        // -------------------------------------------------
        // "{integer}" placeholder rules (Rust-like)
        // - placeholder can be assigned ONLY into an integer type (signed/unsigned),
        //   and only if the literal value fits (checked at resolution site).
        // - placeholder -> float is NOT allowed (no implicit int->float).
        // -------------------------------------------------
        auto is_int_builtin = [&](ty::Builtin b) -> bool {
            return b == ty::Builtin::kI8 || b == ty::Builtin::kI16 || b == ty::Builtin::kI32 ||
                b == ty::Builtin::kI64 || b == ty::Builtin::kI128 ||
                b == ty::Builtin::kU8 || b == ty::Builtin::kU16 || b == ty::Builtin::kU32 ||
                b == ty::Builtin::kU64 || b == ty::Builtin::kU128 ||
                b == ty::Builtin::kISize || b == ty::Builtin::kUSize;
        };

        auto is_float_builtin = [&](ty::Builtin b) -> bool {
            return b == ty::Builtin::kF32 || b == ty::Builtin::kF64 || b == ty::Builtin::kF128;
        };

        const bool dst_is_builtin = (dt.kind == ty::Kind::kBuiltin);
        const bool src_is_builtin = (st.kind == ty::Kind::kBuiltin);

        if (dst_is_builtin && src_is_builtin &&
            st.builtin == ty::Builtin::kInferInteger) {

            if (is_float_builtin(dt.builtin)) {
                return false;
            }

            if (!is_int_builtin(dt.builtin)) {
                return false;
            }

            // 값 fit 여부는 소비 지점에서 resolve할 때 체크한다.
            return true;
        }

        return false;
    }

    ty::TypeId TypeChecker::unify_(ty::TypeId a, ty::TypeId b) {
        if (is_error_(a) || is_error_(b)) return types_.error();
        if (a == b) return a;

        if (a == types_.builtin(ty::Builtin::kNever)) return b;
        if (b == types_.builtin(ty::Builtin::kNever)) return a;

        // null + T? => T?
        if (is_null_(a) && is_optional_(b)) return b;
        if (is_null_(b) && is_optional_(a)) return a;

        // null + T => T? (정책: 삼항 등에서 null 섞이면 옵셔널로 승격)
        if (is_null_(a)) return types_.make_optional(b);
        if (is_null_(b)) return types_.make_optional(a);

        // v0: 나머지는 error
        return types_.error();
    }

    // v0: place(ident/index)에서 "근원 로컬 심볼"을 최대한 뽑는다.
    // - ident      => sym id
    // - index(a,i) => a가 ident면 그 sym id (v0 보수 규칙)
    std::optional<uint32_t> TypeChecker::root_place_symbol_(ast::ExprId place) const {
        if (place == ast::k_invalid_expr) return std::nullopt;
        const ast::Expr& e = ast_.expr(place);

        if (e.kind == ast::ExprKind::kIdent) {
            auto sid = lookup_symbol_(e.text);
            if (!sid) return std::nullopt;
            return *sid;
        }

        if (e.kind == ast::ExprKind::kIndex) {
            return root_place_symbol_(e.a);
        }

        if (e.kind == ast::ExprKind::kBinary && e.op == K::kDot) {
            return root_place_symbol_(e.a);
        }

        return std::nullopt;
    }

    bool TypeChecker::is_mutable_symbol_(uint32_t sym_id) const {
        auto it = sym_is_mut_.find(sym_id);
        if (it == sym_is_mut_.end()) return false;
        return it->second;
    }

    /// @brief range 식(`a..b`, `a..:b`)인지 확인한다.
    bool TypeChecker::is_range_expr_(ast::ExprId eid) const {
        if (eid == ast::k_invalid_expr) return false;
        const auto& e = ast_.expr(eid);
        if (e.kind != ast::ExprKind::kBinary) return false;
        return e.op == K::kDotDot || e.op == K::kDotDotColon;
    }

    /// @brief 인덱스/슬라이스 경계에 허용되는 정수 타입인지 확인한다.
    bool TypeChecker::is_index_int_type_(ty::TypeId t) const {
        if (t == ty::kInvalidType || is_error_(t)) return false;
        const auto& tt = types_.get(t);
        if (tt.kind != ty::Kind::kBuiltin) return false;

        switch (tt.builtin) {
            case ty::Builtin::kI8:
            case ty::Builtin::kI16:
            case ty::Builtin::kI32:
            case ty::Builtin::kI64:
            case ty::Builtin::kI128:
            case ty::Builtin::kU8:
            case ty::Builtin::kU16:
            case ty::Builtin::kU32:
            case ty::Builtin::kU64:
            case ty::Builtin::kU128:
            case ty::Builtin::kISize:
            case ty::Builtin::kUSize:
                return true;
            default:
                return false;
        }
    }

    // place expr (v0: Ident, Index, Field(dot)만 place로 인정)
    bool TypeChecker::is_place_expr_(ast::ExprId eid) const {
        if (eid == ast::k_invalid_expr) return false;
        const auto& e = ast_.expr(eid);
        if (e.kind == ast::ExprKind::kIdent) return true;
        if (e.kind == ast::ExprKind::kIndex) {
            // range index는 slice 생성용 view이므로 v0에서 write/place로 취급하지 않는다.
            if (is_range_expr_(e.b)) return false;
            return is_place_expr_(e.a);
        }
        if (e.kind == ast::ExprKind::kBinary && e.op == K::kDot) {
            if (e.b == ast::k_invalid_expr) return false;
            const auto& rhs = ast_.expr(e.b);
            if (rhs.kind != ast::ExprKind::kIdent) return false;
            return is_place_expr_(e.a);
        }
        return false;
    }

    // --------------------
    // unary / postfix unary
    // --------------------
    ty::TypeId TypeChecker::check_expr_unary_(const ast::Expr& e) {
        // NOTE:
        // - '&' / '&mut' / '&&' 의 의미 규칙(place, escape, conflict 등)은
        //   capability 단계에서 독립적으로 검사한다.
        // - tyck는 여기서 "결과 타입 계산"만 수행한다.
        if (e.op == K::kAmp) {
            // slice borrow: &x[a..b], &mut x[a..:b]
            if (e.a != ast::k_invalid_expr) {
                const auto& opnd = ast_.expr(e.a);
                if (opnd.kind == ast::ExprKind::kIndex && is_range_expr_(opnd.b)) {
                    // index expression 쪽에서 base/경계 타입 검사를 모두 수행한다.
                    ty::TypeId view_t = check_expr_(e.a);
                    const auto& vt = types_.get(view_t);
                    if (vt.kind != ty::Kind::kArray) {
                        diag_(diag::Code::kTypeIndexNonArray, e.span, types_.to_string(view_t));
                        err_(e.span, "slicing is only supported on array types (T[] / T[N]) in v0");
                        return types_.error();
                    }
                    return types_.make_borrow(view_t, /*is_mut=*/e.unary_is_mut);
                }
            }

            ty::TypeId at = check_expr_(e.a);
            return types_.make_borrow(at, /*is_mut=*/e.unary_is_mut);
        }

        if (e.op == K::kAmpAmp) {
            ty::TypeId at = check_expr_(e.a);
            return types_.make_escape(at);
        }

        // e.op, e.a
        ty::TypeId at = check_expr_(e.a);
        at = read_decay_borrow_(types_, at);

        // 기타 unary: v0에서는 최소만
        if (e.op == K::kBang) {
            if (at != types_.builtin(ty::Builtin::kBool) && !is_error_(at)) {
                diag_(diag::Code::kTypeUnaryBangMustBeBool, e.span, types_.to_string(at));
                err_(e.span, "operator '!' requires bool");
            }
            return types_.builtin(ty::Builtin::kBool);
        }

        if (e.op == K::kMinus || e.op == K::kPlus) {
            // 숫자만(간단히 i*/u*/f*를 모두 “numeric”으로 취급)
            return at;
        }

        return types_.error();
    }

    ty::TypeId TypeChecker::check_expr_postfix_unary_(const ast::Expr& e) {
        if (current_expr_id_ != ast::k_invalid_expr &&
            current_expr_id_ < expr_overload_target_cache_.size()) {
            expr_overload_target_cache_[current_expr_id_] = ast::k_invalid_stmt;
        }

        if (!is_place_expr_(e.a)) {
            diag_(diag::Code::kPostfixOperandMustBePlace, e.span);
            err_(e.span, "postfix operator requires a place expression");
            return types_.error();
        }

        ty::TypeId at = check_expr_(e.a);
        ty::TypeId elem = ty::kInvalidType;
        bool is_mut_borrow = false;
        const bool write_through_borrow = borrow_info_(types_, at, elem, is_mut_borrow) && is_mut_borrow;

        // mut check (x++ is a write)
        // - place가 가리키는 심볼이 mut가 아니면 무조건 에러
        if (!write_through_borrow) {
            if (auto sid = root_place_symbol_(e.a)) {
                if (!is_mutable_symbol_(*sid)) {
                    diag_(diag::Code::kWriteToImmutable, e.span);
                    err_(e.span, "cannot apply postfix ++ to an immutable variable (declare it with `mut`)");
                }
            }
        }

        const ty::TypeId receiver_ty = write_through_borrow ? elem : at;
        const ast::StmtId op_sid = resolve_postfix_operator_overload_(e.op, receiver_ty);
        if (op_sid != ast::k_invalid_stmt) {
            if (current_expr_id_ != ast::k_invalid_expr &&
                current_expr_id_ < expr_overload_target_cache_.size()) {
                expr_overload_target_cache_[current_expr_id_] = op_sid;
            }
            return ast_.stmt(op_sid).fn_ret;
        }

        return receiver_ty;
    }

    // --------------------
    // binary / assign / ternary
    // --------------------
    ty::TypeId TypeChecker::check_expr_binary_(const ast::Expr& e) {
        if (current_expr_id_ != ast::k_invalid_expr &&
            current_expr_id_ < expr_overload_target_cache_.size()) {
            expr_overload_target_cache_[current_expr_id_] = ast::k_invalid_stmt;
        }

        // value member access (v0): obj.field
        if (e.op == K::kDot) {
            ty::TypeId base_t = check_expr_(e.a);
            base_t = read_decay_borrow_(types_, base_t);

            if (e.b == ast::k_invalid_expr) {
                diag_(diag::Code::kTypeErrorGeneric, e.span, "missing member on '.' access");
                err_(e.span, "missing member on '.' access");
                return types_.error();
            }

            const ast::Expr& rhs = ast_.expr(e.b);
            if (rhs.kind != ast::ExprKind::kIdent) {
                diag_(diag::Code::kTypeErrorGeneric, rhs.span, "member access requires identifier rhs");
                err_(rhs.span, "member access requires identifier rhs");
                return types_.error();
            }

            auto meta_it = field_abi_meta_by_type_.find(base_t);
            if (meta_it == field_abi_meta_by_type_.end()) {
                const auto& bt = types_.get(base_t);
                if (bt.kind == ty::Kind::kNamedUser) {
                    if (auto sid = lookup_symbol_(types_.to_string(base_t))) {
                        const auto& ss = sym_.symbol(*sid);
                        if (ss.kind == sema::SymbolKind::kField) {
                            meta_it = field_abi_meta_by_type_.find(ss.declared_type);
                        }
                    }
                }
            }

            if (meta_it == field_abi_meta_by_type_.end()) {
                diag_(diag::Code::kTypeErrorGeneric, e.span,
                    std::string("member access is only available on field values in v0, got ") + types_.to_string(base_t));
                err_(e.span, "member access on non-field value");
                return types_.error();
            }

            const ast::StmtId fsid = meta_it->second.sid;
            if (fsid == ast::k_invalid_stmt || (size_t)fsid >= ast_.stmts().size()) {
                diag_(diag::Code::kTypeErrorGeneric, e.span, "invalid field metadata while resolving member");
                err_(e.span, "invalid field metadata");
                return types_.error();
            }

            const auto& fs = ast_.stmt(fsid);
            const uint64_t begin = fs.field_member_begin;
            const uint64_t end = begin + fs.field_member_count;
            if (begin > ast_.field_members().size() || end > ast_.field_members().size()) {
                diag_(diag::Code::kTypeErrorGeneric, e.span, "invalid field member range");
                err_(e.span, "invalid field member range");
                return types_.error();
            }

            for (uint32_t i = fs.field_member_begin; i < fs.field_member_begin + fs.field_member_count; ++i) {
                const auto& m = ast_.field_members()[i];
                if (m.name == rhs.text) {
                    return m.type;
                }
            }

            diag_(diag::Code::kTypeErrorGeneric, rhs.span,
                std::string("unknown field member '") + std::string(rhs.text) + "'");
            err_(rhs.span, "unknown field member");
            return types_.error();
        }

        // NOTE:
        // - v0 정책: binary는 기본적으로 "builtin fast-path"만 처리한다.
        // - 추후 operator overloading을 넣을 때도,
        //   여기 구조를 (A) builtin (B) overload fallback 으로 유지하면 된다.

        // ------------------------------------------------------------
        // Null-Coalescing: ??  (Swift/C# 스타일 축약)
        //
        //  a ?? b
        //   - a: Optional(T?) or null
        //   - if a is T? then b must be assignable to T
        //   - result type: T (non-optional)
        //
        // v0 추가 정책:
        //   - lhs가 null literal인 경우 "null ?? x"를 금지하지 않고,
        //     그냥 결과를 rhs 타입으로 둔다. (원하면 경고/에러로 강화 가능)
        // ------------------------------------------------------------
        if (e.op == K::kQuestionQuestion) {
            ty::TypeId lt = check_expr_(e.a);
            ty::TypeId rt = check_expr_(e.b);

            // error short-circuit
            if (is_error_(lt) || is_error_(rt)) return types_.error();

            // lhs가 null이면 rhs로 수렴(정책)
            if (is_null_(lt)) {
                return rt;
            }

            // lhs는 optional 이어야 한다
            if (!is_optional_(lt)) {
                diag_(diag::Code::kTypeNullCoalesceLhsMustBeOptional, e.span, types_.to_string(lt));
                err_(e.span, "operator '?" "?' requires optional lhs");
                return types_.error();
            }

            ty::TypeId elem = optional_elem_(lt);
            if (elem == ty::kInvalidType) {
                // 방어: Optional인데 elem이 invalid인 경우
                err_(e.span, "optional elem type is invalid");
                return types_.error();
            }

            // rhs가 {integer}면 elem 컨텍스트로 해소 시도
            {
                const auto& st = types_.get(rt);
                if (st.kind == ty::Kind::kBuiltin && st.builtin == ty::Builtin::kInferInteger) {
                    (void)resolve_infer_int_in_context_(e.b, elem);
                    rt = check_expr_(e.b);
                }
            }

            // rhs는 elem에 대입 가능해야 함
            if (!can_assign_(elem, rt)) {
                diag_(diag::Code::kTypeNullCoalesceRhsMismatch, e.span,
                    types_.to_string(elem), types_.to_string(rt));
                err_(e.span, "operator '?" "?' rhs mismatch");
                return types_.error();
            }

            // 결과는 non-optional elem
            return elem;
        }

        ty::TypeId lt = check_expr_(e.a);
        ty::TypeId rt = check_expr_(e.b);
        lt = read_decay_borrow_(types_, lt);
        rt = read_decay_borrow_(types_, rt);

        auto is_builtin = [&](ty::TypeId t) -> bool {
            return t != ty::kInvalidType && types_.get(t).kind == ty::Kind::kBuiltin;
        };

        auto builtin_of = [&](ty::TypeId t) -> ty::Builtin {
            return types_.get(t).builtin;
        };

        auto is_infer_int = [&](ty::TypeId t) -> bool {
            return is_builtin(t) && builtin_of(t) == ty::Builtin::kInferInteger;
        };

        auto is_float = [&](ty::TypeId t) -> bool {
            if (!is_builtin(t)) return false;
            auto b = builtin_of(t);
            return b == ty::Builtin::kF32 || b == ty::Builtin::kF64 || b == ty::Builtin::kF128;
        };

        auto is_int = [&](ty::TypeId t) -> bool {
            if (!is_builtin(t)) return false;
            auto b = builtin_of(t);
            return b == ty::Builtin::kI8 || b == ty::Builtin::kI16 || b == ty::Builtin::kI32 ||
                b == ty::Builtin::kI64 || b == ty::Builtin::kI128 ||
                b == ty::Builtin::kU8 || b == ty::Builtin::kU16 || b == ty::Builtin::kU32 ||
                b == ty::Builtin::kU64 || b == ty::Builtin::kU128 ||
                b == ty::Builtin::kISize || b == ty::Builtin::kUSize;
        };

        // ------------------------------------------------------------
        // Equality: == / !=
        // ------------------------------------------------------------
        if (e.op == K::kEqEq || e.op == K::kBangEq) {
            // acts overload 우선 규칙: 오버로드가 존재하면 builtin보다 먼저 채택한다.
            if (!acts_default_operator_map_.empty()) {
                const ast::StmtId op_sid = resolve_binary_operator_overload_(e.op, lt, rt);
                if (op_sid != ast::k_invalid_stmt) {
                    if (current_expr_id_ != ast::k_invalid_expr &&
                        current_expr_id_ < expr_overload_target_cache_.size()) {
                        expr_overload_target_cache_[current_expr_id_] = op_sid;
                    }
                    return ast_.stmt(op_sid).fn_ret;
                }
            }

            const bool both_builtin = is_builtin(lt) && is_builtin(rt);
            if (!both_builtin && !is_null_(lt) && !is_null_(rt)) {
                diag_(diag::Code::kTypeErrorGeneric, e.span, "no matching operator overload for equality");
                err_(e.span, "no matching operator overload for equality");
                return types_.error();
            }

            // null == null : ok
            if (is_null_(lt) && is_null_(rt)) {
                return types_.builtin(ty::Builtin::kBool);
            }

            // null comparison rule: null is only comparable with optional
            if (is_null_(lt) && !is_optional_(rt)) {
                diag_(diag::Code::kTypeCompareOperandsMustMatch, e.span,
                    types_.to_string(lt), types_.to_string(rt));
                err_(e.span, "null comparison is only allowed with optional types (rhs is not optional)");
                return types_.builtin(ty::Builtin::kBool);
            }
            if (is_null_(rt) && !is_optional_(lt)) {
                diag_(diag::Code::kTypeCompareOperandsMustMatch, e.span,
                    types_.to_string(lt), types_.to_string(rt));
                err_(e.span, "null comparison is only allowed with optional types (lhs is not optional)");
                return types_.builtin(ty::Builtin::kBool);
            }

            // v0: other equality just returns bool (strict typing could be enforced later)
            return types_.builtin(ty::Builtin::kBool);
        }

        // ------------------------------------------------------------
        // Arithmetic: + - * / %
        // ------------------------------------------------------------
        if (e.op == K::kPlus || e.op == K::kMinus || e.op == K::kStar || e.op == K::kSlash || e.op == K::kPercent) {
            // acts overload 우선 규칙: 오버로드가 존재하면 builtin보다 먼저 채택한다.
            if (!acts_default_operator_map_.empty()) {
                const ast::StmtId op_sid = resolve_binary_operator_overload_(e.op, lt, rt);
                if (op_sid != ast::k_invalid_stmt) {
                    if (current_expr_id_ != ast::k_invalid_expr &&
                        current_expr_id_ < expr_overload_target_cache_.size()) {
                        expr_overload_target_cache_[current_expr_id_] = op_sid;
                    }
                    return ast_.stmt(op_sid).fn_ret;
                }
            }

            const bool both_builtin = is_builtin(lt) && is_builtin(rt);
            if (!both_builtin) {
                diag_(diag::Code::kTypeErrorGeneric, e.span, "no matching operator overload for arithmetic");
                err_(e.span, "no matching operator overload for arithmetic");
                return types_.error();
            }

            // float + {integer} is forbidden (no implicit int->float)
            if ((is_float(lt) && is_infer_int(rt)) || (is_float(rt) && is_infer_int(lt))) {
                diag_(diag::Code::kIntToFloatNotAllowed, e.span, "float-arithmetic");
                err_(e.span, "cannot use deferred integer '{integer}' in float arithmetic (no implicit int->float)");
                return types_.error();
            }

            // {integer} + concrete int => resolve {integer} to concrete int
            if (is_infer_int(lt) && is_int(rt)) {
                if (!resolve_infer_int_in_context_(e.a, rt)) return types_.error();
                lt = rt;
                return rt;
            }
            if (is_infer_int(rt) && is_int(lt)) {
                if (!resolve_infer_int_in_context_(e.b, lt)) return types_.error();
                rt = lt;
                return lt;
            }

            // {integer} + {integer} => still {integer}
            if (is_infer_int(lt) && is_infer_int(rt)) {
                return types_.builtin(ty::Builtin::kInferInteger);
            }

            // no implicit promotion: operands must match
            if (lt != rt && !is_error_(lt) && !is_error_(rt)) {
                diag_(diag::Code::kTypeBinaryOperandsMustMatch, e.span,
                    types_.to_string(lt), types_.to_string(rt));
                err_(e.span, "binary arithmetic requires both operands to have the same type (no implicit promotion)");
                return types_.error();
            }

            return lt;
        }

        // ------------------------------------------------------------
        // Comparison: < <= > >=
        // ------------------------------------------------------------
        if (e.op == K::kLt || e.op == K::kLtEq || e.op == K::kGt || e.op == K::kGtEq) {
            // acts overload 우선 규칙: 오버로드가 존재하면 builtin보다 먼저 채택한다.
            if (!acts_default_operator_map_.empty()) {
                const ast::StmtId op_sid = resolve_binary_operator_overload_(e.op, lt, rt);
                if (op_sid != ast::k_invalid_stmt) {
                    if (current_expr_id_ != ast::k_invalid_expr &&
                        current_expr_id_ < expr_overload_target_cache_.size()) {
                        expr_overload_target_cache_[current_expr_id_] = op_sid;
                    }
                    return ast_.stmt(op_sid).fn_ret;
                }
            }

            const bool both_builtin = is_builtin(lt) && is_builtin(rt);
            if (!both_builtin) {
                diag_(diag::Code::kTypeErrorGeneric, e.span, "no matching operator overload for comparison");
                err_(e.span, "no matching operator overload for comparison");
                return types_.error();
            }

            // If one side is concrete int and the other is {integer}, resolve it like arithmetic.
            if (is_infer_int(lt) && is_int(rt)) {
                if (!resolve_infer_int_in_context_(e.a, rt)) {
                    // resolve function should have emitted diag if needed, but keep safety:
                    diag_(diag::Code::kIntLiteralNeedsTypeContext, ast_.expr(e.a).span);
                    err_(e.span, "failed to resolve deferred integer on lhs in comparison");
                    return types_.builtin(ty::Builtin::kBool);
                }
                lt = rt;
            } else if (is_infer_int(rt) && is_int(lt)) {
                if (!resolve_infer_int_in_context_(e.b, lt)) {
                    diag_(diag::Code::kIntLiteralNeedsTypeContext, ast_.expr(e.b).span);
                    err_(e.span, "failed to resolve deferred integer on rhs in comparison");
                    return types_.builtin(ty::Builtin::kBool);
                }
                rt = lt;
            } else if (is_infer_int(lt) || is_infer_int(rt)) {
                // {integer} vs {integer} (or vs non-int) => needs explicit context
                diag_(diag::Code::kIntLiteralNeedsTypeContext, e.span);
                err_(e.span, "comparison with deferred integer '{integer}' needs an explicit integer type context");
                return types_.builtin(ty::Builtin::kBool);
            }

            // v0 strict rule: types must match
            if (lt != rt && !is_error_(lt) && !is_error_(rt)) {
                diag_(diag::Code::kTypeCompareOperandsMustMatch, e.span,
                    types_.to_string(lt), types_.to_string(rt));
                err_(e.span, "comparison requires both operands to have the same type (v0 rule)");
            }

            return types_.builtin(ty::Builtin::kBool);
        }

        // ------------------------------------------------------------
        // TODO: logical ops, bitwise ops, pipe, etc.
        // ------------------------------------------------------------
        {
            const ast::StmtId op_sid = resolve_binary_operator_overload_(e.op, lt, rt);
            if (op_sid != ast::k_invalid_stmt) {
                if (current_expr_id_ != ast::k_invalid_expr &&
                    current_expr_id_ < expr_overload_target_cache_.size()) {
                    expr_overload_target_cache_[current_expr_id_] = op_sid;
                }
                return ast_.stmt(op_sid).fn_ret;
            }
        }
        return types_.error();
    }

    ty::TypeId TypeChecker::check_expr_assign_(const ast::Expr& e) {
        // NOTE:
        // - v0: assign expr는 (1) place 체크, (2) rhs 체크, (3) can_assign 검사로 끝낸다.
        // - compound-assign(+= 등)도 현재는 "단순 대입 호환"만 보는 형태.
        // - NEW: ??= 는 제어흐름 의미가 있으므로 별도 규칙을 강제한다.

        // ------------------------------------------------------------
        // Null-Coalescing Assign: ??=
        //
        //  x ??= y
        //   - lhs must be place
        //   - lhs type must be Optional(T?)
        //   - rhs must be assignable to T
        //   - expression result type: lhs type (T?)  (IR lowering/일관성에 유리)
        //
        // 이 연산도 "write" 이므로 mut 검사 대상이다.
        // ------------------------------------------------------------
        if (e.op == K::kQuestionQuestionAssign) {
            // e.a = lhs, e.b = rhs
            if (!is_place_expr_(e.a)) {
                diag_(diag::Code::kAssignLhsMustBePlace, e.span);
                err_(e.span, "assignment lhs must be a place expression (ident/index)");
                (void)check_expr_(e.b);
                return types_.error();
            }

            ty::TypeId lt = check_expr_(e.a);
            ty::TypeId lhs_target = lt;
            {
                ty::TypeId elem = ty::kInvalidType;
                bool is_mut_borrow = false;
                if (borrow_info_(types_, lt, elem, is_mut_borrow) && is_mut_borrow) {
                    lhs_target = elem; // &mut T place에는 T를 대입한다.
                }
            }

            // mut check
            {
                ty::TypeId elem = ty::kInvalidType;
                bool is_mut_borrow = false;
                const bool write_through_borrow = borrow_info_(types_, lt, elem, is_mut_borrow) && is_mut_borrow;
                if (!write_through_borrow) {
                    if (auto sid = root_place_symbol_(e.a)) {
                        if (!is_mutable_symbol_(*sid)) {
                            diag_(diag::Code::kWriteToImmutable, e.span, "assignment");
                            err_(e.span, "cannot assign to an immutable variable (declare it with `mut`)");
                        }
                    }
                }
            }

            ty::TypeId rt = check_expr_(e.b);

            if (is_error_(lt) || is_error_(rt)) return types_.error();

            if (!is_optional_(lhs_target)) {
                diag_(diag::Code::kTypeNullCoalesceAssignLhsMustBeOptional, e.span, types_.to_string(lhs_target));
                err_(e.span, "operator '?" "?=' requires optional lhs");
                return types_.error();
            }

            ty::TypeId elem = optional_elem_(lhs_target);
            if (elem == ty::kInvalidType) {
                err_(e.span, "optional elem type is invalid");
                return types_.error();
            }

            // RHS가 {integer}면, elem 타입 컨텍스트로 해소 시도
            {
                const auto& st = types_.get(rt);
                if (st.kind == ty::Kind::kBuiltin && st.builtin == ty::Builtin::kInferInteger) {
                    (void)resolve_infer_int_in_context_(e.b, elem);
                    rt = check_expr_(e.b);
                }
            }

            if (!can_assign_(elem, rt)) {
                diag_(diag::Code::kTypeNullCoalesceAssignRhsMismatch, e.span,
                    types_.to_string(elem), types_.to_string(rt));
                err_(e.span, "operator '?" "?=' rhs mismatch");
                return types_.error();
            }

            return lhs_target;
        }

        // ------------------------------------------------------------
        // 기존 '=' / 기타 대입류 (현 로직 유지)
        // ------------------------------------------------------------
        // e.a = lhs, e.b = rhs
        if (!is_place_expr_(e.a)) {
            diag_(diag::Code::kAssignLhsMustBePlace, e.span);
            err_(e.span, "assignment lhs must be a place expression (ident/index)");
        }

        ty::TypeId lt = check_expr_(e.a);
        ty::TypeId lhs_target = lt;
        {
            ty::TypeId elem = ty::kInvalidType;
            bool is_mut_borrow = false;
            if (borrow_info_(types_, lt, elem, is_mut_borrow) && is_mut_borrow) {
                lhs_target = elem; // &mut T place에는 T를 대입한다.
            }
        }

        if (is_place_expr_(e.a)) {
            ty::TypeId elem = ty::kInvalidType;
            bool is_mut_borrow = false;
            const bool write_through_borrow = borrow_info_(types_, lt, elem, is_mut_borrow) && is_mut_borrow;
            if (!write_through_borrow) {
                if (auto sid = root_place_symbol_(e.a)) {
                    if (!is_mutable_symbol_(*sid)) {
                        diag_(diag::Code::kWriteToImmutable, e.span, "assignment");
                        err_(e.span, "cannot assign to an immutable variable (declare it with `mut`)");
                    }
                }
            }
        }

        ty::TypeId rt = check_expr_(e.b);

        // RHS가 {integer}면, LHS 타입 컨텍스트로 해소 시도
        {
            const auto& st = types_.get(rt);
            if (st.kind == ty::Kind::kBuiltin && st.builtin == ty::Builtin::kInferInteger) {
                (void)resolve_infer_int_in_context_(e.b, lhs_target);
                rt = check_expr_(e.b);
            }
        }

        if (!can_assign_(lhs_target, rt)) {
            diag_(
                diag::Code::kTypeAssignMismatch, e.span,
                types_.to_string(lhs_target), types_.to_string(rt)
            );
            err_(e.span, "assign mismatch");
        }
        return lhs_target;
    }

    ty::TypeId TypeChecker::check_expr_ternary_(const ast::Expr& e) {
        // a ? b : c
        ty::TypeId ct = check_expr_(e.a);
        if (ct != types_.builtin(ty::Builtin::kBool) && !is_error_(ct)) {
            diag_(diag::Code::kTypeTernaryCondMustBeBool, e.span, types_.to_string(ct));
            err_(e.span, "ternary condition must be bool");
        }
        ty::TypeId t1 = check_expr_(e.b);
        ty::TypeId t2 = check_expr_(e.c);
        return unify_(t1, t2);
    }

    // --------------------
    // call / array / index
    // --------------------
    ty::TypeId TypeChecker::check_expr_array_lit_(const ast::Expr& e) {
        // Array literal uses ast.args() slice: e.arg_begin..e.arg_begin+e.arg_count
        if (e.arg_count == 0) {
            diag_(diag::Code::kTypeArrayLiteralEmptyNeedsContext, e.span);
            err_(e.span, "empty array literal requires a contextual type (v0)");
            return types_.make_array(types_.error(), /*has_size=*/true, /*size=*/0);
        }

        const auto& args = ast_.args();
        const uint32_t end = e.arg_begin + e.arg_count;
        if (e.arg_begin >= args.size() || end > args.size()) {
            err_(e.span, "array literal element range is out of AST bounds");
            return types_.error();
        }

        ty::TypeId elem = ty::kInvalidType;
        bool has_error = false;

        for (uint32_t i = 0; i < e.arg_count; ++i) {
            const auto& a = args[e.arg_begin + i];
            if (a.expr == ast::k_invalid_expr) {
                has_error = true;
                continue;
            }

            ty::TypeId t = check_expr_(a.expr);
            if (is_error_(t)) {
                has_error = true;
                continue;
            }

            if (elem == ty::kInvalidType) {
                elem = t;
                continue;
            }

            if (elem == t) continue;

            const auto& et = types_.get(elem);
            const auto& tt = types_.get(t);
            const bool elem_is_infer = (et.kind == ty::Kind::kBuiltin && et.builtin == ty::Builtin::kInferInteger);
            const bool t_is_infer = (tt.kind == ty::Kind::kBuiltin && tt.builtin == ty::Builtin::kInferInteger);

            if (elem_is_infer && is_index_int_type_(t)) {
                (void)resolve_infer_int_in_context_(args[e.arg_begin].expr, t);
                elem = t;
                continue;
            }

            if (t_is_infer && is_index_int_type_(elem)) {
                (void)resolve_infer_int_in_context_(a.expr, elem);
                continue;
            }

            diag_(diag::Code::kTypeBinaryOperandsMustMatch, a.span, types_.to_string(elem), types_.to_string(t));
            err_(a.span, "array literal elements must have one unified type");
            has_error = true;
        }

        if (elem == ty::kInvalidType) elem = types_.error();
        if (has_error) elem = types_.error();

        return types_.make_array(elem, /*has_size=*/true, e.arg_count);
    }

    ty::TypeId TypeChecker::check_expr_index_(const ast::Expr& e) {
        // e.a = base, e.b = index expr
        ty::TypeId base_t = check_expr_(e.a);
        ty::TypeId arr_t = base_t;

        const auto& bt = types_.get(base_t);
        if (bt.kind == ty::Kind::kBorrow) {
            const auto& inner = types_.get(bt.elem);
            if (inner.kind == ty::Kind::kArray) {
                arr_t = bt.elem;
            }
        }

        const auto& t = types_.get(arr_t);
        if (t.kind != ty::Kind::kArray) {
            diag_(diag::Code::kTypeIndexNonArray, e.span, types_.to_string(base_t));
            err_(e.span, "indexing is only supported on array types (T[] / T[N]) in v0");
            return types_.error();
        }

        // slice range: x[a..b], x[a..:b]
        if (is_range_expr_(e.b)) {
            const auto& r = ast_.expr(e.b);
            auto check_bound = [&](ast::ExprId bid) {
                if (bid == ast::k_invalid_expr) return;
                ty::TypeId bt = check_expr_(bid);
                if (is_error_(bt)) return;

                const auto& btt = types_.get(bt);
                if (btt.kind == ty::Kind::kBuiltin && btt.builtin == ty::Builtin::kInferInteger) {
                    (void)resolve_infer_int_in_context_(bid, types_.builtin(ty::Builtin::kUSize));
                    bt = check_expr_(bid);
                }

                if (!is_index_int_type_(bt)) {
                    diag_(diag::Code::kTypeIndexMustBeUSize, ast_.expr(bid).span, types_.to_string(bt));
                    err_(e.span, "slice bounds must be integer type in v0");
                }
            };

            check_bound(r.a);
            check_bound(r.b);

            // slicing result is unsized element view (T[])
            return types_.make_array(t.elem);
        }

        ty::TypeId it = check_expr_(e.b);
        if (!is_error_(it)) {
            const auto& itt = types_.get(it);
            if (itt.kind == ty::Kind::kBuiltin && itt.builtin == ty::Builtin::kInferInteger) {
                (void)resolve_infer_int_in_context_(e.b, types_.builtin(ty::Builtin::kUSize));
                it = check_expr_(e.b);
            }
        }

        // index는 정수 타입만 허용(v0)
        if (!is_error_(it) && !is_index_int_type_(it)) {
            diag_(diag::Code::kTypeIndexMustBeUSize, ast_.expr(e.b).span, types_.to_string(it));
            err_(e.span, "index expression must be integer type in v0");
        }

        return t.elem;
    }

    // --------------------
    // if-expr / block-expr / loop-expr
    // --------------------
    ty::TypeId TypeChecker::check_expr_if_(const ast::Expr& e) {
        return check_expr_if_(e, Slot::kValue);
    }

    ty::TypeId TypeChecker::check_expr_if_(const ast::Expr& e, Slot slot) {
        ty::TypeId ct = check_expr_(e.a, Slot::kValue);
        if (ct != types_.builtin(ty::Builtin::kBool) && !is_error_(ct)) {
            diag_(diag::Code::kTypeCondMustBeBool, ast_.expr(e.a).span, types_.to_string(ct));
            err_(e.span, "if-expr condition must be bool");
        }

        // branches are always value-checked as expressions
        ty::TypeId t_then = check_expr_(e.b, Slot::kValue);
        ty::TypeId t_else = check_expr_(e.c, Slot::kValue);

        (void)slot; // currently result type doesn't depend on slot
        return unify_(t_then, t_else);
    }

    ty::TypeId TypeChecker::check_expr_block_(const ast::Expr& e) {
        return check_expr_block_(e, Slot::kValue);
    }

    ty::TypeId TypeChecker::check_expr_block_(const ast::Expr& e, Slot slot) {
        // Mapping assumption:
        //  - e.a: StmtId of block stmt
        //  - e.b: tail ExprId (optional)
        const ast::StmtId block_sid = (ast::StmtId)e.a;
        if (block_sid == ast::k_invalid_stmt) {
            err_(e.span, "block-expr has no block stmt id");
            return types_.error();
        }

        const ast::Stmt& bs = ast_.stmt(block_sid);
        if (bs.kind != ast::StmtKind::kBlock) {
            err_(e.span, "block-expr target is not a block stmt");
            return types_.error();
        }

        // block expr introduces a scope (like block stmt)
        sym_.push_scope();

        // all child statements are checked in statement context
        for (uint32_t i = 0; i < bs.stmt_count; ++i) {
            const ast::StmtId cid = ast_.stmt_children()[bs.stmt_begin + i];
            check_stmt_(cid);
        }

        // tail
        ty::TypeId out = types_.builtin(ty::Builtin::kNull);
        if (e.b != ast::k_invalid_expr) {
            out = check_expr_(e.b, Slot::kValue);
        } else {
            // tail absent => null
            out = types_.builtin(ty::Builtin::kNull);

            // Slot::Value에서는 tail 요구 (v0 안전 정책)
            if (slot == Slot::kValue) {
                diag_(diag::Code::kBlockExprValueExpected, e.span);
                err_(e.span, "value expected: block-expr in value context must have a tail expression");
            }
        }

        sym_.pop_scope();
        return out;
    }

    ty::TypeId TypeChecker::check_expr_loop_(const ast::Expr& e) {
        return check_expr_loop_(e, Slot::kValue);
    }

    ty::TypeId TypeChecker::check_expr_loop_(const ast::Expr& e, Slot /*slot*/) {
        // loop result type comes ONLY from breaks, plus optional null if:
        // - break; exists, or
        // - iter-loop can naturally end

        LoopCtx lc{};
        lc.may_natural_end = e.loop_has_header; // iter loop => natural end => null
        lc.joined_value = ty::kInvalidType;

        // loop scope: variable binding + body scope
        sym_.push_scope();

        // header: loop (v in xs) { ... }
        if (e.loop_has_header) {
            // v0: loop var type unknown => error (until iter protocol exists)
            if (!e.loop_var.empty()) {
                sym_.insert(sema::SymbolKind::kVar, e.loop_var, types_.error(), e.span);
            }
            if (e.loop_iter != ast::k_invalid_expr) {
                (void)check_expr_(e.loop_iter, Slot::kValue);
            }
        }

        // push loop ctx
        loop_stack_.push_back(lc);

        // body is a block stmt
        if (e.loop_body != ast::k_invalid_stmt) {
            ++stmt_loop_depth_;
            check_stmt_(e.loop_body);
            if (stmt_loop_depth_ > 0) --stmt_loop_depth_;
        } else {
            err_(e.span, "loop has no body");
        }

        // pop loop ctx
        LoopCtx done = loop_stack_.back();
        loop_stack_.pop_back();

        sym_.pop_scope();

        // Decide loop type:
        // 1) no breaks:
        //   - iter loop: natural end => null
        //   - infinite loop: never
        if (!done.has_any_break) {
            if (done.may_natural_end) {
                return types_.builtin(ty::Builtin::kNull);
            }
            return types_.builtin(ty::Builtin::kNever);
        }

        // 2) breaks exist:
        // 2-a) no value breaks => only break; (and/or natural end) => null
        if (!done.has_value_break) {
            return types_.builtin(ty::Builtin::kNull);
        }

        // 2-b) value breaks exist => base type = joined_value
        ty::TypeId base = done.joined_value;
        if (base == ty::kInvalidType) base = types_.error();

        // If null is mixed in (break; or natural end), result becomes optional
        const bool has_null = done.has_null_break || done.may_natural_end;

        if (!has_null) {
            return base;
        }

        // base already optional? keep it. if base is null, keep null.
        if (is_null_(base)) return base;
        if (is_optional_(base)) return base;

        return types_.make_optional(base);
    }

    // --------------------
    // cast
    // --------------------

} // namespace parus::tyck
