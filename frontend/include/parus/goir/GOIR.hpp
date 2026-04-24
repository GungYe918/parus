#pragma once

#include <parus/sir/SIR.hpp>
#include <parus/ty/Type.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace parus::goir {

    using TypeId = parus::ty::TypeId;
    inline constexpr TypeId kInvalidType = parus::ty::kInvalidType;

    using StringId = uint32_t;
    using SemanticSigId = uint32_t;
    using ComputationId = uint32_t;
    using RealizationId = uint32_t;
    using PlacementPolicyId = uint32_t;
    using ServiceId = uint32_t;
    using BlockId = uint32_t;
    using InstId = uint32_t;
    using ValueId = uint32_t;
    using RecordLayoutId = uint32_t;

    inline constexpr uint32_t kInvalidId = 0xFFFF'FFFFu;

    enum class StageKind : uint8_t {
        Open = 0,
        Placed,
    };

    enum class FamilyKind : uint8_t {
        None = 0,
        Core,
        Cpu,
        Gpu,
        HwStruct,
        HwFlow,
        Bridge,
    };

    enum class PlacementPolicyKind : uint8_t {
        FixedCpu = 0,
        Unsupported,
    };

    enum class Effect : uint8_t {
        Pure = 0,
        Control,
        Call,
        MayTrap,
        MayRead,
        MayWrite,
        MayReadWrite,
    };

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
        LogicalAnd,
        LogicalOr,
    };

    enum class UnOp : uint8_t {
        Plus,
        Neg,
        Not,
        BitNot,
    };

    enum class CastKind : uint8_t {
        As,
        AsQ,
        AsB,
    };

    enum class OwnershipKind : uint8_t {
        Plain = 0,
        BorrowShared,
        BorrowMut,
        Escape,
    };

    enum class LayoutClass : uint8_t {
        Unknown = 0,
        Scalar,
        FixedArray,
        SliceView,
        PlainRecord,
        TextView,
        OptionalScalar,
        TagEnum,
    };

    enum class PlaceKind : uint8_t {
        None = 0,
        LocalSlot,
        FieldPath,
        IndexPath,
        SubView,
    };

    struct OwnershipInfo {
        OwnershipKind kind = OwnershipKind::Plain;
        parus::sir::EscapeHandleKind escape_kind = parus::sir::EscapeHandleKind::kTrivial;
        parus::sir::EscapeBoundaryKind escape_boundary = parus::sir::EscapeBoundaryKind::kNone;
        bool requires_runtime_lowering = false;
        bool from_static = false;
    };

    struct GHeader {
        uint32_t goir_version_major = 1;
        uint32_t goir_version_minor = 1;
        uint32_t internal_abi_rev = 2;
        StageKind stage_kind = StageKind::Open;
    };

    struct Value {
        TypeId ty = kInvalidType;
        TypeId place_elem_type = kInvalidType;
        Effect eff = Effect::Pure;
        OwnershipInfo ownership{};
        LayoutClass layout = LayoutClass::Unknown;
        PlaceKind place_kind = PlaceKind::None;
        bool is_place = false;
        bool is_mutable = false;
        uint32_t def_a = kInvalidId;
        uint32_t def_b = kInvalidId;
    };

    struct RecordField {
        StringId name = kInvalidId;
        TypeId type = kInvalidType;
    };

    struct RecordLayout {
        TypeId self_type = kInvalidType;
        std::vector<RecordField> fields{};
    };

    struct RecordValueField {
        StringId name = kInvalidId;
        ValueId value = kInvalidId;
    };

    struct OpConstInt {
        std::string text{};
    };

    struct OpConstFloat {
        std::string text{};
    };

    struct OpConstBool {
        bool value = false;
    };

    struct OpConstNull { };

    struct OpTextLit {
        std::string quoted_text{};
    };

    struct OpUnary {
        UnOp op = UnOp::Plus;
        ValueId src = kInvalidId;
    };

    struct OpBinary {
        BinOp op = BinOp::Add;
        ValueId lhs = kInvalidId;
        ValueId rhs = kInvalidId;
    };

    struct OpCast {
        CastKind kind = CastKind::As;
        TypeId to = kInvalidType;
        ValueId src = kInvalidId;
    };

    struct OpArrayMake {
        std::vector<ValueId> elems{};
    };

    struct OpArrayGet {
        ValueId base = kInvalidId;
        ValueId index = kInvalidId;
    };

    struct OpArrayLen {
        ValueId base = kInvalidId;
    };

    struct OpRecordMake {
        std::vector<RecordValueField> fields{};
    };

    struct OpLocalSlot {
        StringId debug_name = kInvalidId;
    };

    struct OpFieldPlace {
        ValueId base = kInvalidId;
        StringId field_name = kInvalidId;
    };

    struct OpIndexPlace {
        ValueId base = kInvalidId;
        ValueId index = kInvalidId;
    };

    struct OpSubView {
        ValueId base = kInvalidId;
        ValueId offset = kInvalidId;
        ValueId length = kInvalidId;
    };

    struct OpLoad {
        ValueId place = kInvalidId;
    };

    struct OpStore {
        ValueId place = kInvalidId;
        ValueId value = kInvalidId;
    };

    struct OpBorrowView {
        ValueId source_place = kInvalidId;
    };

    struct OpEscapeView {
        ValueId source_place = kInvalidId;
    };

    struct OpOptionalSome {
        ValueId value = kInvalidId;
    };

    struct OpOptionalNone { };

    struct OpOptionalIsPresent {
        ValueId optional = kInvalidId;
    };

    struct OpOptionalGet {
        ValueId optional = kInvalidId;
    };

    struct OpEnumTag {
        int64_t tag = 0;
    };

    struct OpSemanticInvoke {
        ComputationId computation = kInvalidId;
        std::vector<ValueId> args{};
    };

    struct OpCallDirect {
        RealizationId callee = kInvalidId;
        std::vector<ValueId> args{};
    };

    using OpData = std::variant<
        OpConstInt,
        OpConstFloat,
        OpConstBool,
        OpConstNull,
        OpTextLit,
        OpUnary,
        OpBinary,
        OpCast,
        OpArrayMake,
        OpArrayGet,
        OpArrayLen,
        OpRecordMake,
        OpLocalSlot,
        OpFieldPlace,
        OpIndexPlace,
        OpSubView,
        OpLoad,
        OpStore,
        OpBorrowView,
        OpEscapeView,
        OpOptionalSome,
        OpOptionalNone,
        OpOptionalIsPresent,
        OpOptionalGet,
        OpEnumTag,
        OpSemanticInvoke,
        OpCallDirect
    >;

    struct Inst {
        OpData data{};
        Effect eff = Effect::Pure;
        ValueId result = kInvalidId;
    };

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

    struct SwitchArm {
        int64_t match_value = 0;
        BlockId target = kInvalidId;
        std::vector<ValueId> args{};
    };

    struct TermSwitch {
        ValueId scrutinee = kInvalidId;
        BlockId default_bb = kInvalidId;
        std::vector<ValueId> default_args{};
        std::vector<SwitchArm> arms{};
    };

    struct TermRet {
        bool has_value = false;
        ValueId value = kInvalidId;
    };

    using Terminator = std::variant<TermBr, TermCondBr, TermSwitch, TermRet>;

    struct Block {
        std::vector<ValueId> params{};
        std::vector<InstId> insts{};
        Terminator term{};
        bool has_term = false;
    };

    struct SemanticSig {
        StringId name = kInvalidId;
        std::vector<TypeId> param_types{};
        TypeId result_type = kInvalidType;
        bool is_pure = true;
        bool is_throwing = false;
    };

    struct GPlacementPolicy {
        PlacementPolicyKind kind = PlacementPolicyKind::FixedCpu;
    };

    struct GService {
        StringId name = kInvalidId;
    };

    struct GComputation {
        StringId name = kInvalidId;
        SemanticSigId sig = kInvalidId;
        PlacementPolicyId placement_policy = kInvalidId;
        std::vector<RealizationId> realizations{};
    };

    struct GRealization {
        StringId name = kInvalidId;
        ComputationId computation = kInvalidId;
        FamilyKind family = FamilyKind::None;
        bool is_entry = false;
        bool is_pure = true;
        bool is_extern = false;
        TypeId fn_type = kInvalidType;
        std::vector<BlockId> blocks{};
        BlockId entry = kInvalidId;
        uint32_t source_func = kInvalidId;
    };

    class Module {
    public:
        GHeader header{};
        std::vector<std::string> strings{};
        std::vector<Value> values{};
        std::vector<Inst> insts{};
        std::vector<Block> blocks{};
        std::vector<RecordLayout> record_layouts{};
        std::vector<SemanticSig> semantic_sigs{};
        std::vector<GPlacementPolicy> placement_policies{};
        std::vector<GService> services{};
        std::vector<GComputation> computations{};
        std::vector<GRealization> realizations{};

        StringId add_string(std::string text) {
            for (StringId i = 0; i < strings.size(); ++i) {
                if (strings[i] == text) return i;
            }
            strings.push_back(std::move(text));
            return static_cast<StringId>(strings.size() - 1);
        }

        std::string_view string(StringId id) const {
            if (id == kInvalidId || static_cast<size_t>(id) >= strings.size()) return {};
            return strings[id];
        }

        ValueId add_value(const Value& value) {
            values.push_back(value);
            return static_cast<ValueId>(values.size() - 1);
        }

        InstId add_inst(const Inst& inst) {
            insts.push_back(inst);
            return static_cast<InstId>(insts.size() - 1);
        }

        BlockId add_block(const Block& block) {
            blocks.push_back(block);
            return static_cast<BlockId>(blocks.size() - 1);
        }

        RecordLayoutId add_record_layout(const RecordLayout& layout) {
            record_layouts.push_back(layout);
            return static_cast<RecordLayoutId>(record_layouts.size() - 1);
        }

        const RecordLayout* find_record_layout(TypeId type) const {
            for (const auto& layout : record_layouts) {
                if (layout.self_type == type) return &layout;
            }
            return nullptr;
        }

        SemanticSigId add_semantic_sig(const SemanticSig& sig) {
            semantic_sigs.push_back(sig);
            return static_cast<SemanticSigId>(semantic_sigs.size() - 1);
        }

        PlacementPolicyId add_placement_policy(const GPlacementPolicy& policy) {
            placement_policies.push_back(policy);
            return static_cast<PlacementPolicyId>(placement_policies.size() - 1);
        }

        ServiceId add_service(const GService& service) {
            services.push_back(service);
            return static_cast<ServiceId>(services.size() - 1);
        }

        ComputationId add_computation(const GComputation& computation) {
            computations.push_back(computation);
            return static_cast<ComputationId>(computations.size() - 1);
        }

        RealizationId add_realization(const GRealization& realization) {
            realizations.push_back(realization);
            return static_cast<RealizationId>(realizations.size() - 1);
        }
    };

} // namespace parus::goir
