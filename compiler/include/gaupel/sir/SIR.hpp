// compiler/include/gaupel/sir/SIR.hpp
#pragma once
#include <gaupel/text/Span.hpp>
#include <gaupel/ty/Type.hpp>

#include <cstdint>
#include <string_view>
#include <vector>


namespace gaupel::sir {

    using TypeId = gaupel::ty::TypeId;
    static constexpr TypeId k_invalid_type = gaupel::ty::kInvalidType;

    // SIR Ids
    using ValueId = uint32_t;
    static constexpr ValueId k_invalid_value = 0xFFFF'FFFFu;

    using BlockId = uint32_t;
    static constexpr BlockId k_invalid_block = 0xFFFF'FFFFu;

    using FuncId = uint32_t;
    static constexpr FuncId k_invalid_func = 0xFFFF'FFFFu;

    using FieldId = uint32_t;
    static constexpr FieldId k_invalid_field = 0xFFFF'FFFFu;

    using ActsId = uint32_t;
    static constexpr ActsId k_invalid_acts = 0xFFFF'FFFFu;

    // SymbolId: sema::SymbolTable uses uint32_t ids (keep as-is)
    using SymbolId = uint32_t;
    static constexpr SymbolId k_invalid_symbol = 0xFFFF'FFFFu;

    // ---------------------------------------------
    // Value kind
    // ---------------------------------------------
    enum class ValueKind : uint8_t {
        kError,

        // literals
        kIntLit,
        kFloatLit,
        kStringLit,
        kCharLit,
        kBoolLit,
        kNullLit,

        // names / references
        kLocal,     // resolved SymbolId (locals + params in v0)
        kGlobal,    // reserved (future)
        kParam,     // reserved (future direct param ref)

        // composite literals (v0 예정)
        kArrayLit,  // [1,2,3]  (future lowering; keep slot)
        kFieldInit, // field{...} / struct literal (future)

        // ops
        kUnary,
        kBinary,
        kAssign,        // place = value (or compound assigns lowered later)
        kPostfixInc,    // place++
        kCall,
        kIndex,
        kField,         // place/value: a.b (future)

        // control expr (kept structured in SIR; CFG may be formed later)
        kIfExpr,
        kBlockExpr,
        kLoopExpr,

        // cast
        kCast,
    };

    // ---------------------------------------------
    // Place / Effect (v0 fixed spec)
    // ---------------------------------------------
    enum class PlaceClass : uint8_t {
        kNotPlace = 0,

        // v0
        kLocal,          // x
        kIndex,          // a[i]

        // future
        kField,          // a.b
        kDeref,          // *p
    };

    enum class EffectClass : uint8_t {
        kPure = 0,       // guaranteed no observable side effects
        kMayWrite,       // may mutate state/memory (assign/++/etc.)
        kUnknown,        // effect unknown (calls/ffi/etc.)
    };

    // ---------------------------------------------
    // Value node
    // ---------------------------------------------
    struct Value {
        ValueKind kind = ValueKind::kError;
        gaupel::Span span{};
        TypeId type = k_invalid_type;     // from tyck (RESULT type)

        // generic slots (interpret by kind)
        uint32_t op = 0;                  // TokenKind or small opcode
        ValueId a = k_invalid_value;
        ValueId b = k_invalid_value;
        ValueId c = k_invalid_value;

        // literals / identifiers (raw text)
        std::string_view text{};

        // resolved symbol (for kLocal)
        SymbolId sym = k_invalid_symbol;

        // meta classification
        PlaceClass place = PlaceClass::kNotPlace;
        EffectClass effect = EffectClass::kPure;

        // call/array args (slice into Module::args)
        uint32_t arg_begin = 0;
        uint32_t arg_count = 0;

        // -----------------------------------------
        // place element type
        //
        // - Value.type: "read/result type" (load 결과 타입)
        // - place_elem_type: place가 가리키는 element 타입 (slot element)
        //   예) future: a[i]에서 결과 타입과 place element 타입이 달라질 수 있음
        // - v0에서는 Local만 place이고 보통 type==place_elem_type 이지만,
        //   OIR lowering을 깔끔하게 하기 위해 분리 필드 제공.
        // -----------------------------------------
        TypeId place_elem_type = k_invalid_type;

        // -----------------------------------------
        // cast target type for kCast
        // - Value.type is the RESULT type (already from tyck)
        // - cast_to is the syntactic "T" in "expr as T / as? T / as! T"
        //   (tyck가 결과 타입을 정규화(T?) 하더라도, 원래 목표 T를 잃지 않게 저장)
        // -----------------------------------------
        TypeId cast_to = k_invalid_type;
    };

    // ---------------------------------------------
    // Call Args (mirrors AST args; named-group preserved)
    // ---------------------------------------------
    enum class ArgKind : uint8_t { kPositional, kLabeled, kNamedGroup };

    struct Arg {
        ArgKind kind = ArgKind::kPositional;

        bool has_label = false;
        bool is_hole = false;
        std::string_view label{};
        ValueId value = k_invalid_value;

        // for NamedGroup: children are stored as adjacent Arg entries
        uint32_t child_begin = 0;
        uint32_t child_count = 0;

        gaupel::Span span{};
    };

    // ---------------------------------------------
    // Attributes (fn-level)
    // ---------------------------------------------
    struct Attr {
        std::string_view name{};
        gaupel::Span span{};
    };

    // ---------------------------------------------
    // Function Params (fn decl까지 보존)
    // ---------------------------------------------
    struct Param {
        std::string_view name{};
        TypeId type = k_invalid_type;

        bool is_mut = false;

        bool has_default = false;
        ValueId default_value = k_invalid_value;

        bool is_named_group = false;      // comes from "{ ... }" param section

        // NOTE: resolved symbol for param will be fixed next turn
        SymbolId sym = k_invalid_symbol;

        gaupel::Span span{};
    };

    // ---------------------------------------------
    // Block / Stmt (structured statements kept)
    // ---------------------------------------------
    enum class StmtKind : uint8_t {
        kError,
        kExprStmt,
        kVarDecl,   // let/set
        kIfStmt,
        kWhileStmt,
        kReturn,
        kBreak,
        kContinue,
        kSwitch,    // reserved (future)
    };

    struct Stmt {
        StmtKind kind = StmtKind::kError;
        gaupel::Span span{};

        // common payload
        ValueId expr = k_invalid_value;

        // structured blocks
        BlockId a = k_invalid_block;  // then/body
        BlockId b = k_invalid_block;  // else

        // var decl
        bool is_set = false;   // let=false, set=true
        bool is_mut = false;
        std::string_view name{};
        SymbolId sym = k_invalid_symbol;
        TypeId declared_type = k_invalid_type;
        ValueId init = k_invalid_value;

        // block children slice (optional; if you ever inline blocks as stmts)
        uint32_t stmt_begin = 0;
        uint32_t stmt_count = 0;
    };

    struct Block {
        gaupel::Span span{};
        uint32_t stmt_begin = 0;
        uint32_t stmt_count = 0;
    };

    // ---------------------------------------------
    // Function Decl metadata (fn decl까지)
    // ---------------------------------------------
    enum class FnMode : uint8_t {
        kNone = 0,
        kPub,
        kSub,
    };

    struct Func {
        gaupel::Span span{};
        std::string_view name{};
        SymbolId sym = k_invalid_symbol;

        // signature types
        TypeId sig = k_invalid_type;     // ty::Kind::kFn
        TypeId ret = k_invalid_type;

        // decl qualifiers
        bool is_export = false;
        FnMode fn_mode = FnMode::kNone;

        bool is_pure = false;
        bool is_comptime = false;

        // reserved qualifiers (future)
        bool is_commit = false;
        bool is_recast = false;

        bool is_throwing = false;

        // attrs / params slices
        uint32_t attr_begin = 0;
        uint32_t attr_count = 0;

        uint32_t param_begin = 0;
        uint32_t param_count = 0;

        uint32_t positional_param_count = 0;
        bool has_named_group = false;

        // body
        BlockId entry = k_invalid_block;

        // hint: whether any stmt/value in this func may write
        bool has_any_write = false;

        // acts 소속 함수 여부 (일반 top-level fn이면 false)
        bool is_acts_member = false;
        ActsId owner_acts = k_invalid_acts;
    };

    struct FieldMember {
        std::string_view name{};
        TypeId type = k_invalid_type;
        gaupel::Span span{};
    };

    struct FieldDecl {
        gaupel::Span span{};
        std::string_view name{};
        SymbolId sym = k_invalid_symbol;
        bool is_export = false;

        uint32_t member_begin = 0;
        uint32_t member_count = 0;
    };

    struct ActsDecl {
        gaupel::Span span{};
        std::string_view name{};
        SymbolId sym = k_invalid_symbol;
        bool is_export = false;

        uint32_t func_begin = 0;
        uint32_t func_count = 0;
    };

    class Module {
    public:
        std::vector<Value> values;
        std::vector<Arg> args;

        std::vector<Attr> attrs;
        std::vector<Param> params;

        std::vector<Stmt> stmts;
        std::vector<Block> blocks;
        std::vector<Func> funcs;
        std::vector<FieldMember> field_members;
        std::vector<FieldDecl> fields;
        std::vector<ActsDecl> acts;

        // helpers
        ValueId add_value(const Value& v)   {  values.push_back(v); return (ValueId)values.size() - 1;  }
        uint32_t add_arg(const Arg& a)      {  args.push_back(a); return (uint32_t)args.size() - 1;     }

        uint32_t add_attr(const Attr& a)    {  attrs.push_back(a); return (uint32_t)attrs.size() - 1;   }
        uint32_t add_param(const Param& p)  {  params.push_back(p); return (uint32_t)params.size() - 1; }

        uint32_t add_stmt(const Stmt& s)    {  stmts.push_back(s); return (uint32_t)stmts.size() - 1;   }
        BlockId add_block(const Block& b)   {  blocks.push_back(b); return (BlockId)blocks.size() - 1;  }
        FuncId add_func(const Func& f)      {  funcs.push_back(f); return (FuncId)funcs.size() - 1;     }
        uint32_t add_field_member(const FieldMember& f) { field_members.push_back(f); return (uint32_t)field_members.size() - 1; }
        FieldId add_field(const FieldDecl& f)           { fields.push_back(f); return (FieldId)fields.size() - 1; }
        ActsId add_acts(const ActsDecl& a)              { acts.push_back(a); return (ActsId)acts.size() - 1; }
    };

} // namespace gaupel::sir
