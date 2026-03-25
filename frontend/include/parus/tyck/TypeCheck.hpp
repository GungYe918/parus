// frontend/include/parus/tyck/TypeCheck.hpp
#pragma once
#include <parus/ast/Nodes.hpp>
#include <parus/common/LoopSourceKind.hpp>
#include <parus/ty/TypePool.hpp>
#include <parus/sema/SymbolTable.hpp>
#include <parus/text/Span.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/num/BigInt.hpp>
#include <parus/syntax/TokenKind.hpp>
#include <parus/passes/GenericPrep.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>
#include <optional>
#include <deque>
#include <utility>
#include <memory>
#include <unordered_map> 
#include <unordered_set>

namespace parus::type {
    struct TypeResolveResult;
}

namespace parus::tyck {

    // tyck의 에러는 우선 compile-safe한 독립 포맷으로 저장
    // (diag::Bag 연동은 프로젝트 내 Diagnostic API가 확정되면 쉽게 브릿지 가능)
    struct TyError {
        Span span{};
        std::string message{};
    };

    enum class ConstInitKind : uint8_t {
        kNone = 0,
        kInt,
        kFloat,
        kBool,
        kChar,
        kString,
    };

    struct ConstInitData {
        ConstInitKind kind = ConstInitKind::kNone;
        std::string text{};
    };

    struct ExternalCBitfieldAccess {
        bool is_valid = false;
        uint32_t storage_offset_bytes = 0;
        uint32_t bit_offset = 0;
        uint32_t bit_width = 0;
        bool bit_signed = false;
    };

    struct TyckResult {
        bool ok = true;
        std::vector<ty::TypeId> expr_types; // ast.exprs() index에 대응
        std::vector<ast::StmtId> expr_overload_target; // expr index -> selected decl (call/operator), invalid if builtin path
        std::vector<ty::TypeId> expr_ctor_owner_type; // expr index -> class ctor owner type, invalid if not ctor-call expr
        std::vector<ty::TypeId> expr_enum_ctor_owner_type; // expr index -> enum ctor owner type, invalid if not enum-ctor expr
        std::vector<uint32_t> expr_enum_ctor_variant_index; // expr index -> enum variant index, invalid if not enum-ctor expr
        std::vector<int64_t> expr_enum_ctor_tag_value; // expr index -> enum tag value, valid on enum-ctor expr
        std::vector<uint32_t> expr_resolved_symbol; // expr index -> resolved symbol id (tyck fallback for cloned generic nodes)
        std::vector<uint32_t> stmt_resolved_symbol; // ast.stmts() index -> resolved symbol id (tyck fallback for cloned generic decl stmts)
        std::vector<ast::StmtId> expr_proto_const_decl; // expr index -> selected proto provide-const decl stmt id
        std::vector<uint32_t> expr_external_callee_symbol; // expr index -> direct external callee symbol id
        std::vector<ty::TypeId> expr_external_callee_type; // expr index -> concrete selected external callee fn type
        std::vector<ast::ExprId> expr_external_receiver_expr; // expr index -> implicit receiver expr for external dot-call
        std::vector<uint8_t> expr_call_is_c_abi; // expr index -> call lowers with C ABI
        std::vector<uint8_t> expr_call_is_c_variadic; // expr index -> call is C variadic
        std::vector<ty::CCallConv> expr_call_c_callconv; // expr index -> callsite C callconv
        std::vector<uint32_t> expr_call_c_fixed_param_count; // expr index -> fixed parameter count for C calls
        std::vector<uint8_t> expr_loop_source_kind; // expr index -> parus::LoopSourceKind
        std::vector<ty::TypeId> expr_loop_binder_type; // expr index -> loop binder type
        std::vector<ty::TypeId> expr_loop_iterator_type; // expr index -> concrete iterator type for sequence loops
        std::vector<ast::StmtId> expr_loop_iter_decl; // expr index -> selected iter decl, invalid when external/none
        std::vector<uint32_t> expr_loop_iter_external_symbol; // expr index -> selected external iter callee symbol
        std::vector<ty::TypeId> expr_loop_iter_fn_type; // expr index -> concrete iter callee fn type
        std::vector<ast::StmtId> expr_loop_next_decl; // expr index -> selected next decl, invalid when external/none
        std::vector<uint32_t> expr_loop_next_external_symbol; // expr index -> selected external next callee symbol
        std::vector<ty::TypeId> expr_loop_next_fn_type; // expr index -> concrete next callee fn type
        std::vector<uint8_t> stmt_for_source_kind; // stmt index -> parus::LoopSourceKind for `for`
        std::vector<ty::TypeId> stmt_for_binder_type; // stmt index -> `for` binder type
        std::vector<ty::TypeId> stmt_for_iterator_type; // stmt index -> concrete iterator type for protocol-driven `for`
        std::vector<ast::StmtId> stmt_for_iter_decl; // stmt index -> selected iter decl, invalid when external/none
        std::vector<uint32_t> stmt_for_iter_external_symbol; // stmt index -> selected external iter callee symbol
        std::vector<ty::TypeId> stmt_for_iter_fn_type; // stmt index -> concrete iter callee fn type
        std::vector<ast::StmtId> stmt_for_next_decl; // stmt index -> selected next decl, invalid when external/none
        std::vector<uint32_t> stmt_for_next_external_symbol; // stmt index -> selected external next callee symbol
        std::vector<ty::TypeId> stmt_for_next_fn_type; // stmt index -> concrete next callee fn type
        std::vector<ExternalCBitfieldAccess> expr_external_c_bitfield; // expr index -> imported C bitfield access metadata
        std::vector<ast::ExprId> expr_fstring_runtime_expr; // expr index -> runtime passthrough expr for non-folded f-string, invalid otherwise
        std::vector<uint32_t> param_resolved_symbol; // ast.params() index -> resolved symbol id
        std::unordered_map<ast::StmtId, std::string> fn_qualified_names; // def decl stmt -> qualified path name
        std::vector<ast::StmtId> generic_instantiated_fn_sids; // concrete generic fn instantiations created during tyck
        std::vector<ast::StmtId> generic_instantiated_class_sids; // concrete generic class instantiations
        std::vector<ast::StmtId> generic_instantiated_proto_sids; // concrete generic proto instantiations
        std::vector<ast::StmtId> generic_instantiated_acts_sids; // concrete generic acts instantiations
        std::vector<ast::StmtId> generic_instantiated_field_sids; // concrete generic struct instantiations
        std::vector<ast::StmtId> generic_instantiated_enum_sids; // concrete generic enum instantiations
        std::vector<ast::StmtId> generic_acts_template_sids; // generic acts templates (owner-generic)
        std::vector<ty::TypeId> actor_type_ids; // known actor nominal types
        std::unordered_set<ty::TypeId> tag_only_enum_type_ids; // enum types known to lower as tag-only layout
        std::unordered_map<uint32_t, ConstInitData> const_symbol_values; // SymbolId -> const initializer value
        std::unordered_map<ast::ExprId, ConstInitData> expr_external_const_values; // expr id -> imported external const literal payload
        std::vector<TyError> errors;
    };

    class TypeChecker {
    public:
        TypeChecker(ast::AstArena& ast,
                    ty::TypePool& types,
                    const parus::type::TypeResolveResult* tr = nullptr,
                    const passes::GenericPrepResult* gp = nullptr)
            : ast_(ast), types_(types), type_resolve_(tr), generic_prep_(gp) {}

        TypeChecker(ast::AstArena& ast,
                    ty::TypePool& types,
                    diag::Bag& bag,
                    const parus::type::TypeResolveResult* tr = nullptr,
                    const passes::GenericPrepResult* gp = nullptr)
            : ast_(ast), types_(types), type_resolve_(tr), generic_prep_(gp), diag_bag_(&bag) {}

        void bind_diag(diag::Bag& bag) { diag_bag_ = &bag; }
        void set_seed_symbol_table(const sema::SymbolTable* seed) { seed_sym_ = seed; }
        void set_current_bundle_name(std::string bundle_name) {
            explicit_current_bundle_name_ = std::move(bundle_name);
        }
        void set_core_impl_marker_file_ids(std::unordered_set<uint32_t> file_ids) {
            explicit_core_impl_marker_file_ids_ = std::move(file_ids);
        }
        void set_file_bundle_overrides(std::unordered_map<uint32_t, std::string> file_bundles) {
            explicit_file_bundle_overrides_ = std::move(file_bundles);
        }

        // program(StmtId) 하나를 타입체크
        TyckResult check_program(ast::StmtId program_stmt);

    private:
        enum class AssignSite : uint8_t {
            LetInit = 0,
            SetInit,
            Assign,
            FieldInit,
            CallArg,
            Return,
            DefaultArg,
            NullCoalesceAssign,
        };

        enum class CoercionKind : uint8_t {
            Exact = 0,
            NullToOptionalNone,
            NullToPtrBoundary,
            LiftToOptionalSome,
            InferThenExact,
            InferThenLiftToOptionalSome,
            Reject,
        };

        struct CoercionPlan {
            bool ok = false;
            CoercionKind kind = CoercionKind::Reject;
            ty::TypeId dst = ty::kInvalidType;
            ty::TypeId src_before = ty::kInvalidType;
            ty::TypeId src_after = ty::kInvalidType;
            ty::TypeId optional_elem = ty::kInvalidType;
        };

        struct FStringConstValue {
            enum class Kind : uint8_t {
                kInvalid = 0,
                kInt,
                kFloat,
                kBool,
                kChar,
                kText,
            };

            Kind kind = Kind::kInvalid;
            int64_t i64 = 0;
            double f64 = 0.0;
            bool b = false;
            char ch = '\0';
            std::string text{};
        };

        // --------------------
        // Slot (value/discard)
        // --------------------
        enum class Slot : uint8_t {
            kValue,    // value is required
            kDiscard,  // value can be discarded
        };

        // --------------------
        // Loop context stack (break type collection)
        // --------------------
        enum class BreakTargetKind : uint8_t {
            kLoopExpr = 0,
            kStmtLoop,
        };

        struct LoopCtx {
            bool has_any_break = false;        // break; or break expr;
            bool has_value_break = false;      // break expr; existed
            bool has_null_break = false;       // break; existed
            bool may_natural_end = false;      // iter-loop natural end -> null
            ty::TypeId joined_value = ty::kInvalidType; // join of break expr types
            ty::TypeId break_expected_type = ty::kInvalidType; // typed destination for break expr infer-int
        };

        std::vector<LoopCtx> loop_stack_;
        std::vector<BreakTargetKind> break_target_stack_;
        uint32_t stmt_loop_depth_ = 0; // while/loop-stmt style depth (non-value loops)

        // --------------------
        // core
        // --------------------
        void first_pass_collect_top_level_(ast::StmtId program_stmt);
        void second_pass_check_program_(ast::StmtId program_stmt);

        // stmt
        void check_stmt_(ast::StmtId sid);
        void check_stmt_block_(const ast::Stmt& s);
        void check_stmt_var_(ast::StmtId sid);
        void check_stmt_if_(const ast::Stmt& s);
        void check_stmt_for_(ast::StmtId sid, const ast::Stmt& s);
        void check_stmt_while_(const ast::Stmt& s);
        void check_stmt_do_scope_(const ast::Stmt& s);
        void check_stmt_do_while_(const ast::Stmt& s);
        void check_stmt_manual_(const ast::Stmt& s);
        void check_stmt_return_(const ast::Stmt& s);
        void check_stmt_require_(const ast::Stmt& s);
        void check_stmt_throw_(const ast::Stmt& s);
        void check_stmt_try_catch_(const ast::Stmt& s);
        void check_stmt_switch_(const ast::Stmt& s);
        void check_stmt_fn_decl_(ast::StmtId sid, const ast::Stmt& s);
        void check_stmt_proto_decl_(ast::StmtId sid);
        void check_stmt_class_decl_(ast::StmtId sid);
        void check_stmt_actor_decl_(ast::StmtId sid);
        void check_stmt_field_decl_(ast::StmtId sid);
        void check_stmt_enum_decl_(ast::StmtId sid);
        void check_stmt_acts_decl_(ast::StmtId sid, const ast::Stmt& s);
        bool stmt_diverges_(ast::StmtId sid, bool loop_control_counts) const;

        // expr
        ty::TypeId check_expr_(ast::ExprId eid);
        ty::TypeId check_expr_(ast::ExprId eid, Slot slot);
        ty::TypeId check_expr_unary_(const ast::Expr& e);
        ty::TypeId check_expr_postfix_unary_(const ast::Expr& e);
        ty::TypeId check_expr_binary_(const ast::Expr& e);
        ty::TypeId check_expr_assign_(const ast::Expr& e);
        ty::TypeId check_expr_ternary_(const ast::Expr& e);
        ty::TypeId check_expr_call_(ast::Expr e);
        ty::TypeId check_expr_index_(const ast::Expr& e);
        ty::TypeId check_expr_array_lit_(const ast::Expr& e);
        ty::TypeId check_expr_field_init_(const ast::Expr& e);
        ty::TypeId check_expr_if_(const ast::Expr& e);
        ty::TypeId check_expr_block_(const ast::Expr& e);
        ty::TypeId check_expr_loop_(const ast::Expr& e);
        ty::TypeId check_expr_cast_(const ast::Expr& e);

        ty::TypeId check_expr_if_(const ast::Expr& e, Slot slot);    // overload
        ty::TypeId check_expr_block_(const ast::Expr& e, Slot slot); // overload
        ty::TypeId check_expr_loop_(const ast::Expr& e, Slot slot);  // overload
        ty::TypeId check_expr_loop_(const ast::Expr& e, Slot slot, BreakTargetKind break_target_kind);


        // --------------------
        // helpers
        // --------------------

        void err_(Span sp, std::string msg);

        // 코드/args 기반 진단 헬퍼
        void diag_(diag::Code code, Span sp);
        void diag_(diag::Code code, Span sp, std::string_view a0);
        void diag_(diag::Code code, Span sp, std::string_view a0, std::string_view a1);
        void diag_(diag::Code code, Span sp, std::string_view a0, std::string_view a1, std::string_view a2);
        void diag_(diag::Code code, Span sp, std::string_view a0, std::string_view a1, std::string_view a2, std::string_view a3);
        void warn_(diag::Code code, Span sp, std::string_view a0 = {});
        void warn_(diag::Code code, Span sp, std::string_view a0, std::string_view a1);

        std::optional<uint32_t> root_place_symbol_(ast::ExprId place) const;
        bool is_mutable_symbol_(uint32_t sym_id) const;
        bool is_global_like_symbol_(uint32_t sym_id) const;

        bool is_place_expr_(ast::ExprId eid) const;
        bool is_range_expr_(ast::ExprId eid) const;
        bool is_index_int_type_(ty::TypeId t) const;

        bool is_optional_(ty::TypeId t) const;
        ty::TypeId optional_elem_(ty::TypeId opt) const;

        bool is_null_(ty::TypeId t) const;
        bool is_error_(ty::TypeId t) const;
        bool class_has_user_deinit_(ty::TypeId t) const;
        bool type_needs_drop_(ty::TypeId t) const;
        bool is_move_only_type_(ty::TypeId t) const;
        bool is_trivial_copy_clone_type_(ty::TypeId t) const;
        bool can_access_class_member_(ast::StmtId owner_class_sid, ast::FieldMember::Visibility visibility) const;
        bool is_private_class_stmt_member_(ast::StmtId member_sid) const;
        bool is_private_class_field_member_(const ast::FieldMember& member) const;

        bool in_loop_() const { return !break_target_stack_.empty(); }
        bool current_break_target_accepts_value_() const {
            return !break_target_stack_.empty() &&
                   break_target_stack_.back() == BreakTargetKind::kLoopExpr;
        }
        void note_break_(ty::TypeId t, bool is_value_break);
        enum class OwnershipState : uint8_t {
            kInitialized = 0,
            kMovedUninitialized,
            kMaybeUninitialized,
        };
        using OwnershipStateMap = std::unordered_map<uint32_t, OwnershipState>;
        OwnershipState ownership_state_of_(uint32_t sym_id) const;
        bool ensure_symbol_readable_(uint32_t sym_id, Span use_span);
        void mark_symbol_initialized_(uint32_t sym_id);
        void mark_symbol_moved_(uint32_t sym_id);
        void mark_expr_move_consumed_(ast::ExprId expr_id, ty::TypeId expected_type, Span diag_span);
        OwnershipStateMap capture_ownership_state_() const;
        void restore_ownership_state_(const OwnershipStateMap& state);
        void merge_ownership_state_from_branches_(const OwnershipStateMap& before,
                                                 const std::vector<OwnershipStateMap>& branches,
                                                 bool include_before_as_fallthrough);
        ty::TypeId check_expr_place_no_read_(ast::ExprId eid);

        // "대입/초기화" 호환성:
        // - exact match OK
        // - null -> T? OK
        // - T -> T? (암시적 승격은 일단 OFF: 필요하면 ON으로 바꾸기 쉬움)
        bool can_assign_(ty::TypeId dst, ty::TypeId src) const;
        CoercionPlan classify_assign_with_coercion_(
            AssignSite site,
            ty::TypeId dst,
            ast::ExprId src_eid,
            Span diag_span
        );
        std::string type_for_user_diag_(ty::TypeId t, ast::ExprId eid) const;

        // 두 타입을 하나로 합치기(삼항/if-expr 등):
        // - 같으면 그대로
        // - null + T? => T?
        // - null + T  => T? (여기서는 “유틸”로만 제공. 정책은 쉽게 바꿀 수 있음)
        ty::TypeId unify_(ty::TypeId a, ty::TypeId b);
        bool fold_fstring_expr_(ast::ExprId string_eid, std::string& out_bytes);
        bool try_fold_fstring_expr_no_diag_(ast::ExprId string_eid, std::string& out_bytes);
        bool check_fstring_runtime_form_(ast::ExprId string_eid, ast::ExprId& out_runtime_text_expr);
        bool eval_fstring_const_expr_(ast::ExprId expr_eid, FStringConstValue& out);
        bool fstring_const_to_text_(const FStringConstValue& v, std::string& out) const;

        // 함수 컨텍스트
        struct FnCtx {
            bool in_fn = false;
            bool is_pure = false;
            bool is_comptime = false;
            bool is_throwing = false;
            bool has_exception_construct = false;
            ty::TypeId ret = ty::kInvalidType;
        };
        FnCtx fn_ctx_{};
        std::vector<ast::StmtId> fn_sid_stack_;
        std::vector<ast::StmtId> class_visibility_owner_stack_;
        bool in_try_expr_context_ = false;

        // 심볼 테이블
        sema::SymbolTable sym_;
        const sema::SymbolTable* seed_sym_ = nullptr;
        std::string explicit_current_bundle_name_{};

        // 결과 저장소
        TyckResult result_{};

        // expr_types 캐시
        std::vector<ty::TypeId> expr_type_cache_;
        std::vector<ast::StmtId> expr_overload_target_cache_;
        std::vector<ty::TypeId> expr_ctor_owner_type_cache_;
        std::vector<ty::TypeId> expr_enum_ctor_owner_type_cache_;
        std::vector<uint32_t> expr_enum_ctor_variant_index_cache_;
        std::vector<int64_t> expr_enum_ctor_tag_value_cache_;
        std::vector<uint32_t> expr_resolved_symbol_cache_;
        std::vector<uint32_t> stmt_resolved_symbol_cache_;
        std::vector<ast::StmtId> expr_proto_const_decl_cache_;
        std::vector<uint32_t> expr_external_callee_symbol_cache_;
        std::vector<ty::TypeId> expr_external_callee_type_cache_;
        std::vector<ast::ExprId> expr_external_receiver_expr_cache_;
        std::vector<uint8_t> expr_call_is_c_abi_cache_;
        std::vector<uint8_t> expr_call_is_c_variadic_cache_;
        std::vector<ty::CCallConv> expr_call_c_callconv_cache_;
        std::vector<uint32_t> expr_call_c_fixed_param_count_cache_;
        std::vector<uint8_t> expr_loop_source_kind_cache_;
        std::vector<ty::TypeId> expr_loop_binder_type_cache_;
        std::vector<ty::TypeId> expr_loop_iterator_type_cache_;
        std::vector<ast::StmtId> expr_loop_iter_decl_cache_;
        std::vector<uint32_t> expr_loop_iter_external_symbol_cache_;
        std::vector<ty::TypeId> expr_loop_iter_fn_type_cache_;
        std::vector<ast::StmtId> expr_loop_next_decl_cache_;
        std::vector<uint32_t> expr_loop_next_external_symbol_cache_;
        std::vector<ty::TypeId> expr_loop_next_fn_type_cache_;
        std::vector<uint8_t> stmt_for_source_kind_cache_;
        std::vector<ty::TypeId> stmt_for_binder_type_cache_;
        std::vector<ty::TypeId> stmt_for_iterator_type_cache_;
        std::vector<ast::StmtId> stmt_for_iter_decl_cache_;
        std::vector<uint32_t> stmt_for_iter_external_symbol_cache_;
        std::vector<ty::TypeId> stmt_for_iter_fn_type_cache_;
        std::vector<ast::StmtId> stmt_for_next_decl_cache_;
        std::vector<uint32_t> stmt_for_next_external_symbol_cache_;
        std::vector<ty::TypeId> stmt_for_next_fn_type_cache_;
        std::vector<ExternalCBitfieldAccess> expr_external_c_bitfield_cache_;
        std::vector<ast::ExprId> expr_fstring_runtime_expr_cache_;
        std::unordered_map<ast::ExprId, ConstInitData> expr_external_const_value_cache_;
        std::vector<uint32_t> param_resolved_symbol_cache_;
        ast::ExprId current_expr_id_ = ast::k_invalid_expr;
        ast::StmtId current_for_stmt_id_ = ast::k_invalid_stmt;

        // builtin text type for string literals
        ty::TypeId string_type_ = ty::kInvalidType;


        ast::AstArena& ast_;
        ty::TypePool& types_;
        const parus::type::TypeResolveResult* type_resolve_ = nullptr;
        const passes::GenericPrepResult* generic_prep_ = nullptr;
        diag::Bag* diag_bag_ = nullptr;

        // ----------------------------------------
        // Deferred integer inference
        // ----------------------------------------
        struct PendingInt {
            num::BigInt value{};
            bool has_value = false;     // literal-backed only
            bool resolved = false;
            ty::TypeId resolved_type = ty::kInvalidType;
        };

        // By SymbolId: set x = <int literal> ;  (var-level origin)
        std::unordered_map<uint32_t, PendingInt> pending_int_sym_;
        std::unordered_map<uint32_t, ast::ExprId> pending_int_sym_origin_;

        // By ExprId: any integer literal expression (and optionally propagated)
        std::unordered_map<uint32_t, PendingInt> pending_int_expr_;

        // (TypeChecker private) inferred integer("{integer}")를 expected 정수 타입으로 확정 시도
        bool type_contains_infer_int_(ty::TypeId tid) const;
        ty::TypeId choose_smallest_signed_type_(const num::BigInt& v) const;
        bool collect_infer_int_leaf_values_(ast::ExprId eid, std::vector<num::BigInt>& out) const;
        bool finalize_infer_int_shape_(ast::ExprId origin_eid, ty::TypeId current, ty::TypeId& out) const;
        bool resolve_infer_int_in_context_(ast::ExprId eid, ty::TypeId expected);
        bool infer_int_value_of_expr_(ast::ExprId eid, num::BigInt& out) const;
        static bool fits_builtin_int_big_(const num::BigInt& v, ty::Builtin dst);
        static bool is_field_pod_value_type_(const ty::TypePool& types, ty::TypeId id);
        bool has_manual_permission_(uint8_t perm) const;
        bool parse_external_c_type_with_semantic_(
            std::string_view type_repr,
            std::string_view type_semantic,
            ty::TypeId& out
        ) const;
        void collect_external_c_record_fields_();
        bool parse_external_c_union_payload_(
            std::string_view payload,
            std::unordered_map<std::string, ty::TypeId>& out_fields
        ) const;
        struct ExternalCFieldMeta {
            ty::TypeId type = ty::kInvalidType;
            bool union_origin = false;
            bool is_bitfield = false;
            uint32_t storage_offset_bytes = 0;
            uint32_t bit_offset = 0;
            uint32_t bit_width = 0;
            bool bit_signed = false;
        };
        struct ExternalCGlobalMeta {
            enum class TlsKind : uint8_t {
                kNone = 0,
                kDynamic,
                kStatic,
            };
            bool is_c_import = false;
            bool is_const = false;
            bool is_volatile = false;
            bool is_restrict = false;
            TlsKind tls_kind = TlsKind::kNone;
        };
        bool parse_external_c_struct_payload_(
            std::string_view payload,
            std::unordered_map<std::string, ExternalCFieldMeta>& out_fields
        ) const;
        bool parse_external_c_global_payload_(
            std::string_view payload,
            ExternalCGlobalMeta& out
        ) const;
        bool parse_external_c_const_payload_(
            std::string_view payload,
            ConstInitData& out
        ) const;
        bool parse_external_c_typedef_payload_(
            std::string_view payload,
            bool& out_transparent,
            ty::TypeId& out_target
        ) const;
        bool parse_cimport_type_repr_(std::string_view repr, ty::TypeId& out) const;
        ty::TypeId canonicalize_transparent_external_typedef_(ty::TypeId t) const;
        bool resolve_external_c_union_field_type_(
            ty::TypeId owner_type,
            std::string_view field_name,
            ty::TypeId& out_field_type,
            bool* out_is_union_owner = nullptr
        );
        bool resolve_external_c_struct_field_type_(
            ty::TypeId owner_type,
            std::string_view field_name,
            ty::TypeId& out_field_type,
            bool* out_is_struct_owner = nullptr
        );
        bool resolve_external_c_struct_field_meta_(
            ty::TypeId owner_type,
            std::string_view field_name,
            ExternalCFieldMeta& out_field,
            bool* out_is_struct_owner = nullptr
        );

        // --------------------
        // namespace / path helpers (nest/import v0)
        // --------------------
        void init_file_namespace_(ast::StmtId program_stmt);
        std::string qualify_decl_name_(std::string_view base_name) const;
        std::optional<uint32_t> lookup_symbol_(std::string_view name) const;
        std::optional<std::string> rewrite_imported_path_(std::string_view path) const;
        bool apply_imported_path_rewrite_(std::string& path) const;
        bool qualified_path_requires_import_(std::string_view raw_path) const;
        std::string current_namespace_prefix_() const;
        std::string current_module_head_() const;
        std::string path_join_(uint32_t begin, uint32_t count) const;
        std::string resolve_import_path_for_alias_(std::string_view raw_path) const;
        ty::TypeId canonicalize_acts_owner_type_(ty::TypeId owner_type) const;
        std::optional<ast::StmtId> ensure_external_proto_stub_from_symbol_(const sema::Symbol& ss);
        void collect_known_namespace_paths_(ast::StmtId program_stmt);
        void collect_file_import_aliases_(ast::StmtId program_stmt);
        bool is_known_namespace_path_(std::string_view path) const;
        void push_alias_scope_();
        void pop_alias_scope_();
        bool define_alias_(std::string_view alias, std::string_view path, Span diag_span, bool warn_use_nest_preferred = false);

        // ----------------------------------------
        // Mut tracking (tyck-level)
        // ----------------------------------------
        std::unordered_map<uint32_t, bool> sym_is_mut_; // SymbolId -> is_mut
        OwnershipStateMap ownership_state_;
        bool suppress_ownership_read_ = false;
        // catch(e) untyped binder symbols:
        // - used to allow `throw e` rethrow in v0.
        std::unordered_set<uint32_t> untyped_catch_binder_symbols_;

        // qualified-name -> overloaded def decl stmt ids
        // NOTE: std::string을 key로 쓰는 이유:
        // - string_view는 AST storage lifetime에 의존하는데,
        //   향후 AST arena의 내부 저장 방식이 바뀌면 위험해질 수 있음.
        std::unordered_map<std::string, std::vector<ast::StmtId>> fn_decl_by_name_;
        std::unordered_map<ast::StmtId, std::string> fn_qualified_name_by_stmt_;
        std::unordered_map<std::string, ast::StmtId> proto_decl_by_name_;
        std::unordered_map<ty::TypeId, ast::StmtId> proto_decl_by_type_;
        std::unordered_map<ast::StmtId, std::string> proto_qualified_name_by_stmt_;
        std::unordered_map<ty::TypeId, std::vector<ast::StmtId>> explicit_impl_proto_sids_by_type_;
        std::unordered_map<ast::StmtId, std::string> class_qualified_name_by_stmt_;
        std::unordered_map<ast::StmtId, std::string> acts_qualified_name_by_stmt_;
        std::unordered_map<std::string, ast::StmtId> class_decl_by_name_;
        std::unordered_map<ty::TypeId, ast::StmtId> class_decl_by_type_;
        std::unordered_map<ty::TypeId, std::unordered_map<std::string, std::vector<ast::StmtId>>> class_effective_method_map_;
        std::unordered_set<ast::StmtId> class_member_fn_sid_set_;
        std::unordered_map<ast::StmtId, ast::StmtId> class_member_owner_by_stmt_;
        std::unordered_map<std::string, ast::StmtId> private_class_member_qname_owner_;
        std::unordered_map<std::string, ast::StmtId> actor_decl_by_name_;
        std::unordered_map<ty::TypeId, ast::StmtId> actor_decl_by_type_;
        std::unordered_map<ty::TypeId, std::unordered_map<std::string, std::vector<ast::StmtId>>> actor_method_map_;
        std::unordered_set<ast::StmtId> actor_member_fn_sid_set_;
        std::unordered_set<ast::StmtId> proto_member_fn_sid_set_;
        std::unordered_map<std::string, ast::StmtId> enum_decl_by_name_;
        std::unordered_map<ty::TypeId, ast::StmtId> enum_decl_by_type_;
        std::vector<std::string> namespace_stack_;
        std::unordered_map<std::string, std::string> import_alias_to_path_;
        std::unordered_set<std::string> known_namespace_paths_;
        std::vector<std::unordered_map<std::string, std::string>> import_alias_scope_stack_;
        uint32_t block_depth_ = 0;
        bool in_actor_method_ = false;
        bool in_actor_pub_method_ = false;
        bool in_actor_sub_method_ = false;
        std::vector<uint8_t> manual_perm_stack_{};

        struct ActsOperatorDecl {
            ast::StmtId fn_sid = ast::k_invalid_stmt;
            ast::StmtId acts_decl_sid = ast::k_invalid_stmt;
            ty::TypeId owner_type = ty::kInvalidType;
            syntax::TokenKind op_token = syntax::TokenKind::kError;
            bool is_postfix = false;
            bool from_named_set = false;
        };
        std::unordered_map<uint64_t, std::vector<ActsOperatorDecl>> acts_default_operator_map_;

        struct ActsMethodDecl {
            ast::StmtId fn_sid = ast::k_invalid_stmt;
            ast::StmtId acts_decl_sid = ast::k_invalid_stmt;
            ty::TypeId owner_type = ty::kInvalidType;
            bool receiver_is_self = false;
            bool from_named_set = false;
        };
        struct ExternalActsMethodDecl {
            uint32_t fn_symbol = sema::SymbolTable::kNoScope;
            ty::TypeId owner_type = ty::kInvalidType;
            bool receiver_is_self = false;
            bool owner_is_generic_template = false;
            uint32_t owner_generic_arity = 0;
            std::string owner_base{};
            std::string external_payload{};
        };
        struct ActsAssocTypeDecl {
            ast::StmtId assoc_sid = ast::k_invalid_stmt;
            ast::StmtId acts_decl_sid = ast::k_invalid_stmt;
            ty::TypeId owner_type = ty::kInvalidType;
            ty::TypeId bound_type = ty::kInvalidType;
            bool from_named_set = false;
        };
        struct ExternalActsAssocTypeDecl {
            ty::TypeId owner_type = ty::kInvalidType;
            ty::TypeId bound_type = ty::kInvalidType;
            bool owner_is_generic_template = false;
            uint32_t owner_generic_arity = 0;
            std::string owner_base{};
            std::string external_payload{};
        };
        std::unordered_map<std::string, std::vector<uint32_t>> external_fn_overload_map_;
        std::unordered_map<ty::TypeId, std::unordered_map<std::string, std::vector<ActsMethodDecl>>> acts_default_method_map_;
        std::unordered_map<ty::TypeId, std::unordered_map<std::string, std::vector<ExternalActsMethodDecl>>> external_acts_default_method_map_;
        std::unordered_map<std::string, std::unordered_map<std::string, std::vector<ExternalActsMethodDecl>>> external_acts_template_method_map_;
        std::unordered_map<ty::TypeId, std::unordered_map<std::string, std::vector<ActsAssocTypeDecl>>> acts_default_assoc_type_map_;
        std::unordered_map<ty::TypeId, std::unordered_map<std::string, std::vector<ExternalActsAssocTypeDecl>>> external_acts_default_assoc_type_map_;
        std::unordered_map<std::string, std::unordered_map<std::string, std::vector<ExternalActsAssocTypeDecl>>> external_acts_template_assoc_type_map_;
        std::unordered_map<std::string, ast::StmtId> acts_named_decl_by_owner_and_name_;
        std::unordered_map<ty::TypeId, std::vector<ast::StmtId>> acts_default_decls_by_owner_;
        std::unordered_set<uint32_t> explicit_core_impl_marker_file_ids_;
        std::unordered_set<uint32_t> core_impl_marker_file_ids_;
        std::unordered_map<uint32_t, std::string> explicit_file_bundle_overrides_;

        enum class BuiltinActsApiGroup : uint8_t {
            IntLike = 0,
            FloatLike,
            BoolLike,
            CharLike,
            TextLike,
            Unsupported,
        };

        struct BuiltinActsPolicy {
            bool allow_default_acts = false;
            bool allow_named_acts = false;
            BuiltinActsApiGroup api_group = BuiltinActsApiGroup::Unsupported;
            std::string_view reserved_bundle = "core";
        };

        enum class ActiveActsSelectionKind : uint8_t {
            kDefaultOnly = 0,
            kNamed,
        };

        struct ActiveActsSelection {
            ActiveActsSelectionKind kind = ActiveActsSelectionKind::kDefaultOnly;
            ast::StmtId named_decl_sid = ast::k_invalid_stmt;
            std::string set_name{};
            Span span{};
        };

        enum class ImplBindingKind : uint8_t {
            kNone = 0,
            kSpinLoop,
            kStepNext,
            kSizeOf,
            kAlignOf,
        };

        struct GenericConstraintFailure {
            enum class Kind : uint8_t {
                kNone = 0,
                kUnknownTypeParam,
                kProtoNotFound,
                kProtoUnsatisfied,
                kTypeMismatch,
            };

            Kind kind = Kind::kNone;
            std::string lhs_type_param{};
            std::string rhs_proto{};
            std::string rhs_type_repr{};
            std::string concrete_lhs{};
            std::string concrete_rhs{};
        };

        std::vector<std::unordered_map<ty::TypeId, ActiveActsSelection>> acts_selection_scope_stack_;
        std::unordered_map<uint32_t, ActiveActsSelection> acts_selection_by_symbol_;

        static uint64_t acts_operator_key_(ty::TypeId owner_type, syntax::TokenKind op_token, bool is_postfix);
        static std::string acts_named_decl_key_(ty::TypeId owner_type, std::string_view set_qname);
        static BuiltinActsPolicy builtin_acts_policy_(ty::Builtin b);
        static bool is_intlike_builtin_(ty::Builtin b);
        static bool is_float_builtin_(ty::Builtin b);
        static bool is_char_builtin_(ty::Builtin b);
        static bool is_text_builtin_(ty::Builtin b);
        static bool is_bool_builtin_(ty::Builtin b);
        void collect_external_fn_overloads_();
        bool is_self_named_type_(ty::TypeId t) const;
        bool is_builtin_owner_type_(ty::TypeId t, ty::Builtin* out_builtin = nullptr) const;
        std::optional<ty::TypeId> parse_builtin_owner_type_from_text_(std::string_view s) const;
        bool parse_external_builtin_acts_payload_(
            std::string_view payload,
            ty::TypeId& out_owner_type,
            std::string& out_member_name,
            bool& out_receiver_is_self
        ) const;
        bool parse_external_acts_assoc_type_payload_(
            std::string_view payload,
            ty::TypeId& out_owner_type,
            std::string& out_assoc_name,
            ty::TypeId& out_bound_type
        ) const;
        void collect_external_builtin_acts_methods_();
        void collect_external_proto_stubs_();
        void ensure_builtin_family_proto_aliases_();
        std::string current_bundle_name_() const;
        std::string bundle_name_for_file_(uint32_t file_id) const;
        void collect_core_impl_marker_file_ids_(ast::StmtId program_stmt);
        bool is_core_impl_marker_stmt_(const ast::Stmt& s) const;
        ImplBindingKind parse_impl_binding_payload_(std::string_view payload) const;
        bool stmt_impl_binding_key_(const ast::Stmt& s, std::string& out_key) const;
        bool stmt_impl_binding_kind_(const ast::Stmt& s, ImplBindingKind& out_kind) const;
        std::string make_impl_binding_payload_(ImplBindingKind kind) const;
        std::string make_impl_binding_payload_(std::string_view key, bool compiler_owned) const;
        bool validate_constraint_clause_decl_(
            uint32_t begin,
            uint32_t count,
            const std::unordered_set<std::string>& generic_params,
            Span owner_span
        );
        void collect_unresolved_generic_param_names_in_type_(
            ty::TypeId t,
            std::unordered_set<std::string>& out
        ) const;
        void collect_unresolved_generic_param_names_in_proto_target_(
            ty::TypeId t,
            std::unordered_set<std::string>& out
        ) const;
        std::optional<ast::StmtId> resolve_proto_sid_for_constraint_(std::string_view raw) const;
        bool is_builtin_family_proto_(ast::StmtId proto_sid) const;
        bool builtin_family_proto_satisfied_by_primitive_name_(ty::TypeId concrete_t, std::string_view proto_name) const;
        bool builtin_family_proto_satisfied_by_primitive_(ty::TypeId concrete_t, ast::StmtId proto_sid) const;
        bool proto_decl_matches_constraint_sid_(ast::StmtId candidate_sid, ast::StmtId expected_sid) const;
        bool type_satisfies_proto_constraint_(ty::TypeId concrete_t, ast::StmtId proto_sid, Span use_span);
        bool evaluate_generic_constraint_(
            const ast::FnConstraintDecl& cc,
            const std::unordered_map<std::string, ty::TypeId>& bindings,
            Span use_span,
            GenericConstraintFailure& out
        );
        bool enforce_builtin_acts_policy_(const ast::Stmt& acts_decl, ty::TypeId owner_type);
        bool decompose_named_user_type_(
            ty::TypeId t,
            std::string& out_base,
            std::vector<ty::TypeId>& out_args
        ) const;
        bool type_contains_unresolved_generic_param_(ty::TypeId t) const;
        void push_acts_selection_scope_();
        void pop_acts_selection_scope_();
        const ActiveActsSelection* lookup_active_acts_selection_(ty::TypeId owner_type) const;
        const ActiveActsSelection* lookup_symbol_acts_selection_(uint32_t symbol_id) const;
        bool bind_symbol_acts_selection_(uint32_t symbol_id, ty::TypeId owner_type, const ast::Stmt& var_stmt, Span diag_span);
        std::optional<ast::StmtId> resolve_named_acts_decl_sid_(ty::TypeId owner_type, std::string_view raw_set_path) const;
        bool apply_use_acts_selection_(const ast::Stmt& use_stmt);
        void collect_acts_operator_decl_(ast::StmtId acts_decl_sid, const ast::Stmt& acts_decl, bool allow_named_set = false);
        void collect_acts_method_decl_(ast::StmtId acts_decl_sid, const ast::Stmt& acts_decl, bool allow_named_set = false);
        void collect_acts_assoc_type_decl_(ast::StmtId acts_decl_sid, const ast::Stmt& acts_decl, bool allow_named_set = false);
        ast::StmtId resolve_binary_operator_overload_(syntax::TokenKind op, ty::TypeId lhs, ty::TypeId rhs,
                                                      const ActiveActsSelection* forced_selection = nullptr) const;
        ast::StmtId resolve_prefix_operator_overload_(syntax::TokenKind op, ty::TypeId lhs,
                                                      const ActiveActsSelection* forced_selection = nullptr) const;
        ast::StmtId resolve_postfix_operator_overload_(syntax::TokenKind op, ty::TypeId lhs,
                                                       const ActiveActsSelection* forced_selection = nullptr) const;
        std::vector<ActsMethodDecl> lookup_acts_methods_for_call_(ty::TypeId owner_type, std::string_view name,
                                                                  const ActiveActsSelection* forced_selection = nullptr) const;
        std::vector<ExternalActsMethodDecl> lookup_external_acts_methods_for_call_(
            ty::TypeId concrete_owner_type,
            std::string_view member_name
        ) const;
        std::optional<ty::TypeId> lookup_acts_assoc_type_binding_(ty::TypeId owner_type, std::string_view name,
                                                                  const ActiveActsSelection* forced_selection = nullptr) const;
        bool is_self_assoc_named_type_(ty::TypeId t, std::string* out_name = nullptr) const;
        ty::TypeId substitute_self_and_assoc_type_(
            ty::TypeId t,
            ty::TypeId owner_type,
            const std::unordered_map<std::string, ty::TypeId>* assoc_bindings = nullptr
        ) const;
        bool collect_assoc_type_bindings_for_owner_(
            ty::TypeId owner_type,
            std::unordered_map<std::string, ty::TypeId>& out,
            const ActiveActsSelection* forced_selection = nullptr
        ) const;
        static bool type_matches_acts_owner_(const ty::TypePool& types, ty::TypeId owner, ty::TypeId actual);
        std::vector<std::string> collect_decl_generic_param_names_(const ast::Stmt& decl) const;
        std::optional<ast::StmtId> ensure_generic_class_instance_(
            ast::StmtId template_sid,
            const std::vector<ty::TypeId>& concrete_args,
            Span use_span
        );
        std::optional<ast::StmtId> ensure_generic_field_instance_(
            ast::StmtId template_sid,
            const std::vector<ty::TypeId>& concrete_args,
            Span use_span
        );
        std::optional<ast::StmtId> ensure_generic_enum_instance_(
            ast::StmtId template_sid,
            const std::vector<ty::TypeId>& concrete_args,
            Span use_span
        );
        std::optional<ast::StmtId> ensure_generic_field_instance_from_type_(
            ty::TypeId maybe_generic_field_type,
            Span use_span
        );
        std::optional<ast::StmtId> ensure_generic_enum_instance_from_type_(
            ty::TypeId maybe_generic_enum_type,
            Span use_span
        );
        std::optional<ast::StmtId> ensure_generic_proto_instance_(
            ast::StmtId template_sid,
            const std::vector<ty::TypeId>& concrete_args,
            Span use_span
        );
        std::optional<ast::StmtId> ensure_generic_acts_instance_(
            ast::StmtId template_sid,
            ty::TypeId concrete_owner_type,
            const std::vector<ty::TypeId>& concrete_args,
            Span use_span
        );
        void ensure_generic_acts_for_owner_(ty::TypeId concrete_owner_type, Span use_span);
        std::optional<ast::StmtId> resolve_proto_decl_from_type_(
            ty::TypeId proto_type,
            Span use_span,
            bool* out_typed_path_failure = nullptr,
            bool emit_diag = true
        );
        std::optional<ast::StmtId> resolve_proto_decl_from_path_ref_(
            const ast::PathRef& pr,
            Span use_span,
            bool* out_typed_path_failure = nullptr
        );
        std::string path_ref_display_(const ast::PathRef& pr) const;
        enum class ProtoRequireEvalResult : uint8_t {
            kTrue = 0,
            kFalse,
            kTypeNotBool,
            kTooComplex,
        };
        ProtoRequireEvalResult eval_proto_require_const_bool_(ast::ExprId expr_id) const;
        bool evaluate_proto_require_at_apply_(
            ast::StmtId proto_sid,
            ty::TypeId owner_type,
            Span apply_span,
            bool emit_unsatisfied_diag = true,
            bool emit_shape_diag = true
        );
        bool proto_member_fn_sig_matches_impl_(
            const ast::Stmt& req,
            const ast::Stmt& impl,
            ty::TypeId owner_type
        ) const;
        bool proto_requirement_satisfied_by_default_acts_(
            ast::StmtId req_sid,
            ty::TypeId owner_type
        ) const;
        bool proto_assoc_requirement_satisfied_by_default_acts_(
            const ast::Stmt& req,
            ty::TypeId owner_type
        ) const;
        struct ConstObject;
        struct ConstValue {
            enum class Kind : uint8_t {
                kInvalid = 0,
                kInt,
                kFloat,
                kBool,
                kChar,
                kStruct,
            };

            Kind kind = Kind::kInvalid;
            ty::TypeId type = ty::kInvalidType;
            int64_t i64 = 0;
            double f64 = 0.0;
            bool b = false;
            uint32_t ch = 0;
            std::shared_ptr<ConstObject> object{};
        };
        struct ConstObject {
            ty::TypeId type = ty::kInvalidType;
            std::vector<std::string> field_names{};
            std::vector<ConstValue> field_values{};
        };
        struct ConstBinding {
            ConstValue value{};
            bool is_mut = false;
        };
        struct ConstEvalContext {
            uint32_t step_budget = 100000;
            uint32_t step_count = 0;
            uint32_t call_depth_budget = 128;
            std::vector<ast::StmtId> call_stack{};
        };

        bool eval_const_expr_value_(ast::ExprId expr_id, ConstValue& out, Span diag_span);
        bool eval_const_symbol_value_(uint32_t symbol_id, ConstValue& out, Span diag_span);
        bool eval_const_expr_(ast::ExprId expr_id, ConstInitData& out, Span diag_span);
        bool eval_const_symbol_(uint32_t symbol_id, ConstInitData& out, Span diag_span);
        bool eval_const_expr_value_impl_(
            ast::ExprId expr_id,
            ConstValue& out,
            Span diag_span,
            ConstEvalContext& ctx,
            std::unordered_map<std::string, ConstBinding>* local_env = nullptr
        );
        bool eval_const_symbol_value_impl_(
            uint32_t symbol_id,
            ConstValue& out,
            Span diag_span,
            ConstEvalContext& ctx
        );
        bool eval_const_fn_call_impl_(
            ast::StmtId fn_sid,
            const std::vector<ConstValue>& args,
            ConstValue& out,
            Span diag_span,
            ConstEvalContext& ctx
        );
        bool const_value_to_scalar_init_(const ConstValue& in, ConstInitData& out) const;
        bool const_value_type_matches_(const ConstValue& v, ty::TypeId expected) const;
        bool const_value_is_composite_(const ConstValue& v) const;

        bool is_c_abi_safe_type_(ty::TypeId t, bool allow_void) const;
        bool is_c_abi_safe_type_impl_(ty::TypeId t, bool allow_void, std::unordered_set<ty::TypeId>& visiting) const;
        bool is_va_list_type_(ty::TypeId t) const;
        void check_c_abi_global_decl_(const ast::Stmt& s);
        std::vector<std::string> collect_generic_param_names_(const ast::Stmt& fn_decl) const;
        ty::TypeId substitute_generic_type_(
            ty::TypeId src,
            const std::unordered_map<std::string, ty::TypeId>& subst
        ) const;
        ast::ExprId clone_expr_with_type_subst_(
            ast::ExprId src,
            const std::unordered_map<std::string, ty::TypeId>& subst,
            std::unordered_map<ast::ExprId, ast::ExprId>& expr_map,
            std::unordered_map<ast::StmtId, ast::StmtId>& stmt_map
        );
        ast::StmtId clone_stmt_with_type_subst_(
            ast::StmtId src,
            const std::unordered_map<std::string, ty::TypeId>& subst,
            std::unordered_map<ast::ExprId, ast::ExprId>& expr_map,
            std::unordered_map<ast::StmtId, ast::StmtId>& stmt_map
        );
        std::optional<ast::StmtId> ensure_generic_function_instance_(
            ast::StmtId template_sid,
            const std::vector<ty::TypeId>& concrete_args,
            Span call_span
        );

        struct FieldAbiMeta {
            ast::StmtId sid = ast::k_invalid_stmt;
            ast::FieldLayout layout = ast::FieldLayout::kNone;
            uint32_t align = 0;
        };

        struct EnumVariantFieldMeta {
            std::string name{};
            std::string storage_name{};
            ty::TypeId type = ty::kInvalidType;
            Span span{};
        };

        struct EnumVariantMeta {
            std::string name{};
            uint32_t index = 0;
            int64_t tag = 0;
            bool has_discriminant = false;
            std::vector<EnumVariantFieldMeta> fields{};
            std::unordered_map<std::string, uint32_t> field_index_by_name{};
        };

        struct EnumAbiMeta {
            ast::StmtId sid = ast::k_invalid_stmt;
            ast::FieldLayout layout = ast::FieldLayout::kNone;
            bool is_layout_c = false;
            std::vector<EnumVariantMeta> variants{};
            std::unordered_map<std::string, uint32_t> variant_index_by_name{};
        };

        std::unordered_map<ty::TypeId, FieldAbiMeta> field_abi_meta_by_type_;
        std::unordered_map<ty::TypeId, EnumAbiMeta> enum_abi_meta_by_type_;
        bool parse_external_enum_decl_payload_(
            std::string_view payload,
            EnumAbiMeta& out_meta
        ) const;
        void collect_external_enum_metadata_();
        std::unordered_set<ast::StmtId> generic_fn_template_sid_set_;
        std::unordered_map<std::string, ast::StmtId> generic_fn_instance_cache_;
        std::unordered_set<ast::StmtId> generic_fn_checked_instances_;
        std::unordered_set<ast::StmtId> generic_fn_checking_instances_;
        std::vector<ast::StmtId> generic_instantiated_fn_sids_;
        std::deque<ast::StmtId> pending_generic_instance_queue_;
        std::unordered_set<ast::StmtId> pending_generic_instance_enqueued_;
        std::unordered_set<ast::StmtId> generic_class_template_sid_set_;
        std::unordered_set<ast::StmtId> generic_proto_template_sid_set_;
        std::unordered_set<ast::StmtId> generic_acts_template_sid_set_;
        std::unordered_set<ast::StmtId> generic_field_template_sid_set_;
        std::unordered_set<ast::StmtId> generic_enum_template_sid_set_;
        std::unordered_map<std::string, ast::StmtId> generic_class_instance_cache_;
        std::unordered_map<std::string, ast::StmtId> generic_proto_instance_cache_;
        std::unordered_map<std::string, ast::StmtId> generic_acts_instance_cache_;
        std::unordered_map<std::string, ast::StmtId> generic_field_instance_cache_;
        std::unordered_map<std::string, ast::StmtId> generic_enum_instance_cache_;
        std::unordered_set<ast::StmtId> generic_decl_checked_instances_;
        std::unordered_set<ast::StmtId> generic_decl_checking_instances_;
        std::deque<ast::StmtId> pending_generic_decl_instance_queue_;
        std::unordered_set<ast::StmtId> pending_generic_decl_instance_enqueued_;
        std::vector<ast::StmtId> generic_instantiated_class_sids_;
        std::vector<ast::StmtId> generic_instantiated_proto_sids_;
        std::vector<ast::StmtId> generic_instantiated_acts_sids_;
        std::vector<ast::StmtId> generic_instantiated_field_sids_;
        std::vector<ast::StmtId> generic_instantiated_enum_sids_;
        std::unordered_set<ast::ExprId> proto_require_type_diag_emitted_;
        std::unordered_set<ast::ExprId> proto_require_complex_diag_emitted_;
        std::unordered_map<uint32_t, ast::StmtId> const_symbol_decl_sid_;
        std::unordered_map<uint32_t, uint8_t> const_symbol_eval_state_;
        std::unordered_map<uint32_t, ConstValue> const_symbol_runtime_values_;
        std::unordered_set<uint32_t> const_cycle_diag_emitted_;
        bool external_c_record_fields_collected_ = false;
        std::unordered_map<ty::TypeId, std::unordered_map<std::string, ty::TypeId>> external_c_union_fields_by_type_;
        std::unordered_map<std::string, std::unordered_map<std::string, ty::TypeId>> external_c_union_fields_by_name_;
        std::unordered_map<ty::TypeId, std::unordered_map<std::string, ExternalCFieldMeta>> external_c_struct_fields_by_type_;
        std::unordered_map<std::string, std::unordered_map<std::string, ExternalCFieldMeta>> external_c_struct_fields_by_name_;
        bool core_context_invalid_ = false;

    };

} // namespace parus::tyck
