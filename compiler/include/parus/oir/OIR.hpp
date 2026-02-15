// compiler/include/parus/oir/OIR.hpp
#pragma once
#include <cstdint>
#include <string>
#include <variant>
#include <vector>


namespace parus::oir {

    // ----------------------
    // IDs
    // ----------------------
    using TypeId  = uint32_t;
    using FuncId  = uint32_t;
    using BlockId = uint32_t;
    using InstId  = uint32_t;
    using ValueId = uint32_t;

    static constexpr uint32_t kInvalidId = 0xFFFF'FFFFu;

    // ----------------------
    // Effect model (v0)
    // ----------------------
    enum class Effect : uint8_t {
        Pure,
        MayReadMem,
        MayWriteMem,
        MayTrap,
        Call,
    };

    // ----------------------
    // Value
    // ----------------------
    struct Value {
        TypeId  ty  = kInvalidId;
        Effect  eff = Effect::Pure;

        // def site (optional debug)
        // - inst result: def_a = inst_id
        // - block param: def_a = block_id, def_b = param_index
        uint32_t def_a = kInvalidId;
        uint32_t def_b = kInvalidId;
    };

    // ----------------------
    // Ops / Kinds (v0 minimal)
    // ----------------------
    enum class BinOp : uint8_t {
        Add,
        Sub,
        Mul,
        Div,
        Rem,
        Lt,
        Le,
        Gt,
        Ge,
        Eq,
        Ne,
        NullCoalesce,
    };

    enum class UnOp : uint8_t {
        Plus,
        Neg,
        Not,
        BitNot,
    };

    enum class CastKind : uint8_t {
        As,    // as T
        AsQ,   // as? T
        AsB,   // as! T
    };

    /// @brief escape-handle storage kind(내부 비물질화 토큰의 의미 힌트).
    enum class EscapeHandleKind : uint8_t {
        Trivial = 0,
        StackSlot,
        CallerSlot,
        HeapBox,
    };

    /// @brief escape-handle이 경계에서 소비되는 형태.
    enum class EscapeBoundaryKind : uint8_t {
        None = 0,
        Return,
        CallArg,
        Abi,
        Ffi,
    };

    // ----------------------
    // Inst payloads (v0)
    // ----------------------
    struct InstConstInt   { std::string text; };
    struct InstConstBool  { bool value = false; };
    struct InstConstNull  { };

    struct InstUnary      { UnOp op; ValueId src; };
    struct InstBinOp      { BinOp op; ValueId lhs; ValueId rhs; };
    struct InstCast       { CastKind kind; TypeId to; ValueId src; };
    struct InstCall       { ValueId callee; std::vector<ValueId> args; };
    struct InstIndex      { ValueId base; ValueId index; };
    struct InstField      { ValueId base; std::string field; };

    struct InstAllocaLocal{ TypeId slot_ty; };
    struct InstLoad       { ValueId slot; };
    struct InstStore      { ValueId slot; ValueId value; };

    using InstData = std::variant<
        InstConstInt,
        InstConstBool,
        InstConstNull,
        InstUnary,
        InstBinOp,
        InstCast,
        InstCall,
        InstIndex,
        InstField,
        InstAllocaLocal,
        InstLoad,
        InstStore
    >;

    // ----------------------
    // Inst
    // ----------------------
    struct Inst {
        InstData data{};
        Effect   eff    = Effect::Pure;
        ValueId  result = kInvalidId; // kInvalidId for "no result" (e.g. store)
    };

    // ----------------------
    // Terminators (v0)
    // ----------------------
    struct TermBr {
        BlockId target = kInvalidId;
        std::vector<ValueId> args{};
    };

    struct TermCondBr {
        ValueId cond = kInvalidId;

        BlockId then_bb = kInvalidId;
        std::vector<ValueId> then_args{};

        BlockId else_bb = kInvalidId;
        std::vector<ValueId> else_args{};
    };

    struct TermRet {
        bool    has_value = false;
        ValueId value     = kInvalidId;
    };

    using Terminator = std::variant<TermBr, TermCondBr, TermRet>;

    // ----------------------
    // Block
    // ----------------------
    struct Block {
        // block params (phi-like)
        std::vector<ValueId> params;

        // linear inst list
        std::vector<InstId> insts;     // ★ 반드시 non-const

        // terminator
        Terminator term{};
        bool has_term = false;
    };

    // ----------------------
    // Function
    // ----------------------
    struct Function {
        std::string name;

        // return type (used by builder/dumper)
        TypeId ret_ty = kInvalidId;

        // list of blocks belonging to this function (ids into Module::blocks)
        std::vector<BlockId> blocks;

        BlockId entry = kInvalidId;
    };

    /// @brief OIR에서 추적하는 escape-handle 힌트(런타임 객체가 아닌 최적화 메타).
    struct EscapeHandleHint {
        ValueId value = kInvalidId;
        TypeId pointee_type = kInvalidId;

        EscapeHandleKind kind = EscapeHandleKind::Trivial;
        EscapeBoundaryKind boundary = EscapeBoundaryKind::None;

        bool from_static = false;
        bool has_drop = false;
        bool abi_pack_required = false;
        bool ffi_pack_required = false;
    };

    /// @brief OIR 패스가 누적하는 최적화 통계.
    struct OptStats {
        uint32_t critical_edges_split = 0;
        uint32_t mem2reg_promoted_slots = 0;
        uint32_t mem2reg_phi_params = 0;
        uint32_t escape_pack_elided = 0;
        uint32_t escape_boundary_rewrites = 0;
    };

    // ----------------------
    // Module container
    // ----------------------
    struct Module {
        std::vector<Function> funcs;
        std::vector<Block>    blocks;
        std::vector<Inst>     insts;
        std::vector<Value>    values;
        std::vector<EscapeHandleHint> escape_hints;
        OptStats opt_stats{};

        // ---- add_* helpers (complete types required) ----
        ValueId add_value(const Value& v) {
            values.push_back(v);
            return static_cast<ValueId>(values.size() - 1);
        }

        InstId add_inst(const Inst& i) {
            insts.push_back(i);
            return static_cast<InstId>(insts.size() - 1);
        }

        BlockId add_block(const Block& b) {
            blocks.push_back(b);
            return static_cast<BlockId>(blocks.size() - 1);
        }

        FuncId add_func(const Function& f) {
            funcs.push_back(f);
            return static_cast<FuncId>(funcs.size() - 1);
        }

        uint32_t add_escape_hint(const EscapeHandleHint& h) {
            escape_hints.push_back(h);
            return static_cast<uint32_t>(escape_hints.size() - 1);
        }
    };

} // namespace parus::oir
