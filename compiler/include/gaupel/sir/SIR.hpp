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

    // SymbolId는 sema::SymbolTable이 uint32_t로 관리하므로 그대로 사용
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

        // names
        kLocal,     // local var reference (resolved SymbolId)
        kGlobal,    // (future) module global ref
        kParam,     // (future) direct param ref

        // ops
        kUnary,
        kBinary,
        kAssign,        // place = value
        kPostfixInc,    // place++
        kCall,
        kIndex,

        // control expr (SIR에서는 “expr 형태”만 남기고, stmt CFG는 OIR에서 만들 수도 있음)
        kIfExpr,
        kBlockExpr,
        kLoopExpr,

        // cast
        kCast,
    };

    // place classification (OIR 힌트용)
    enum class PlaceClass : uint8_t {
        kNotPlace = 0,
        kLocal,          // x
        kIndex,          // a[i]
        // future: field, deref, etc.
    };

    // side-effect classification (초기 v0)
    enum class EffectClass : uint8_t {
        kPure = 0,
        kMayWrite,       // assign/postfix++ 등
        kUnknown,        // call 같은 것 (추후 pure/can_write 분석)
    };

    // ---------------------------------------------
    // Value node
    // ---------------------------------------------
    struct Value {
        ValueKind kind = ValueKind::kError;
        gaupel::Span span{};
        TypeId type = k_invalid_type;     // tyck result 기반

        // generic slots
        uint32_t op = 0;                  // TokenKind or small opcode
        ValueId a = k_invalid_value;
        ValueId b = k_invalid_value;
        ValueId c = k_invalid_value;

        // for literals / ident
        std::string_view text{};

        // resolved symbol (for kLocal)
        SymbolId sym = k_invalid_symbol;

        // meta
        PlaceClass place = PlaceClass::kNotPlace;
        EffectClass effect = EffectClass::kPure;

        // call args (slice)
        uint32_t arg_begin = 0;
        uint32_t arg_count = 0;
    };

    enum class ArgKind : uint8_t { kPositional, kLabeled, kNamedGroup };

    struct Arg {
        ArgKind kind = ArgKind::kPositional;
        bool has_label = false;
        bool is_hole = false;
        std::string_view label{};
        ValueId value = k_invalid_value;

        uint32_t child_begin = 0;
        uint32_t child_count = 0;

        gaupel::Span span{};
    };

    // ---------------------------------------------
    // Block (SIR 단계에서는 “stmt 나열”을 그대로 보존)
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
        kSwitch,    // future
    };

    struct Stmt {
        StmtKind kind = StmtKind::kError;
        gaupel::Span span{};

        // common payload
        ValueId expr = k_invalid_value;

        // structured
        BlockId a = k_invalid_block;  // then/body
        BlockId b = k_invalid_block;  // else

        // var decl
        bool is_set = false;   // let=false, set=true
        bool is_mut = false;   // declared mut keyword (AST의 is_mut)
        std::string_view name{};
        SymbolId sym = k_invalid_symbol;
        TypeId declared_type = k_invalid_type; // let의 annotation or set의 inferred
        ValueId init = k_invalid_value;

        // block children slice
        uint32_t stmt_begin = 0;
        uint32_t stmt_count = 0;
    };

    struct Block {
        gaupel::Span span{};
        uint32_t stmt_begin = 0;
        uint32_t stmt_count = 0;
    };

    struct Func {
        gaupel::Span span{};
        std::string_view name{};
        SymbolId sym = k_invalid_symbol;

        TypeId sig = k_invalid_type;     // ty::Kind::kFn
        TypeId ret = k_invalid_type;

        BlockId entry = k_invalid_block;

        // mut analysis 결과 힌트(함수 단위)
        bool has_any_write = false;
    };

    struct Module {
        std::vector<Value> values;
        std::vector<Arg> args;
        std::vector<Stmt> stmts;
        std::vector<Block> blocks;
        std::vector<Func> funcs;

        // helpers
        ValueId add_value(const Value& v) { values.push_back(v); return (ValueId)values.size()-1; }
        uint32_t add_arg(const Arg& a) { args.push_back(a); return (uint32_t)args.size()-1; }
        uint32_t add_stmt(const Stmt& s) { stmts.push_back(s); return (uint32_t)stmts.size()-1; }
        BlockId add_block(const Block& b) { blocks.push_back(b); return (BlockId)blocks.size()-1; }
        FuncId add_func(const Func& f) { funcs.push_back(f); return (FuncId)funcs.size()-1; }
    };

} // gaupel::sir