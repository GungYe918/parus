#include <parus/goir/Builder.hpp>
#include <parus/goir/Passes.hpp>
#include <parus/goir/Verify.hpp>

#include <parus/ast/Nodes.hpp>
#include <parus/syntax/TokenKind.hpp>

#include <charconv>
#include <cctype>
#include <optional>
#include <string>
#include <unordered_map>

namespace parus::goir {

    namespace {

        using parus::sir::StmtKind;
        using parus::sir::ValueKind;
        using parus::syntax::TokenKind;

        bool is_supported_scalar_type_(const parus::ty::TypePool& types, TypeId ty) {
            if (ty == kInvalidType) return false;
            const auto& t = types.get(ty);
            if (t.kind != parus::ty::Kind::kBuiltin) return false;
            using B = parus::ty::Builtin;
            switch (t.builtin) {
                case B::kUnit:
                case B::kBool:
                case B::kChar:
                case B::kI8:
                case B::kI16:
                case B::kI32:
                case B::kI64:
                case B::kU8:
                case B::kU16:
                case B::kU32:
                case B::kU64:
                case B::kISize:
                case B::kUSize:
                case B::kF32:
                case B::kF64:
                    return true;
                default:
                    return false;
            }
        }

        bool is_unit_type_(const parus::ty::TypePool& types, TypeId ty) {
            if (ty == kInvalidType) return false;
            const auto& t = types.get(ty);
            return t.kind == parus::ty::Kind::kBuiltin && t.builtin == parus::ty::Builtin::kUnit;
        }

        bool is_text_type_(const parus::ty::TypePool& types, TypeId ty) {
            if (ty == kInvalidType) return false;
            const auto& t = types.get(ty);
            return t.kind == parus::ty::Kind::kBuiltin && t.builtin == parus::ty::Builtin::kText;
        }

        bool starts_with_(std::string_view s, std::string_view pfx) {
            return s.size() >= pfx.size() && s.substr(0, pfx.size()) == pfx;
        }

        bool ends_with_(std::string_view s, std::string_view sfx) {
            return s.size() >= sfx.size() && s.substr(s.size() - sfx.size()) == sfx;
        }

        bool is_hex_digit_(char c) {
            return (c >= '0' && c <= '9') ||
                   (c >= 'a' && c <= 'f') ||
                   (c >= 'A' && c <= 'F');
        }

        uint8_t hex_digit_value_(char c) {
            if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
            if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + (c - 'a'));
            if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(10 + (c - 'A'));
            return 0;
        }

        std::string parse_int_literal_text_(std::string_view text) {
            std::string out{};
            out.reserve(text.size());
            size_t i = 0;
            if (!text.empty() && (text[0] == '+' || text[0] == '-')) {
                out.push_back(text[0]);
                i = 1;
            }
            bool saw_digit = false;
            for (; i < text.size(); ++i) {
                const char c = text[i];
                if (c >= '0' && c <= '9') {
                    out.push_back(c);
                    saw_digit = true;
                    continue;
                }
                if (c == '_') continue;
                break;
            }
            return saw_digit ? out : std::string("0");
        }

        std::optional<int64_t> parse_i64_literal_(std::string_view text) {
            const auto sanitized = parse_int_literal_text_(text);
            int64_t value = 0;
            const char* begin = sanitized.data();
            const char* end = begin + sanitized.size();
            const auto [ptr, ec] = std::from_chars(begin, end, value, 10);
            if (ec != std::errc{} || ptr != end) return std::nullopt;
            return value;
        }

        std::optional<uint32_t> parse_char_literal_code_(std::string_view text) {
            if (text.size() < 3 || text.front() != '\'' || text.back() != '\'') return std::nullopt;
            const auto body = text.substr(1, text.size() - 2);
            if (body.empty()) return std::nullopt;
            if (body[0] != '\\') {
                if (body.size() != 1) return std::nullopt;
                return static_cast<uint32_t>(static_cast<unsigned char>(body[0]));
            }
            if (body.size() == 2) {
                switch (body[1]) {
                    case 'n': return static_cast<uint32_t>('\n');
                    case 'r': return static_cast<uint32_t>('\r');
                    case 't': return static_cast<uint32_t>('\t');
                    case '\\': return static_cast<uint32_t>('\\');
                    case '\'': return static_cast<uint32_t>('\'');
                    case '0': return static_cast<uint32_t>('\0');
                    default: return std::nullopt;
                }
            }
            if (body.size() == 4 && body[1] == 'x' &&
                is_hex_digit_(body[2]) && is_hex_digit_(body[3])) {
                const uint8_t hi = hex_digit_value_(body[2]);
                const uint8_t lo = hex_digit_value_(body[3]);
                return static_cast<uint32_t>((hi << 4) | lo);
            }
            return std::nullopt;
        }

        bool has_attr_(const parus::sir::Module& sir, const parus::sir::Func& fn, std::string_view name) {
            for (uint32_t i = 0; i < fn.attr_count; ++i) {
                const auto aid = fn.attr_begin + i;
                if (static_cast<size_t>(aid) >= sir.attrs.size()) break;
                if (sir.attrs[aid].name == name) return true;
            }
            return false;
        }

        std::optional<BinOp> map_binop_(TokenKind tk) {
            switch (tk) {
                case TokenKind::kPlus: return BinOp::Add;
                case TokenKind::kMinus: return BinOp::Sub;
                case TokenKind::kStar: return BinOp::Mul;
                case TokenKind::kSlash: return BinOp::Div;
                case TokenKind::kPercent: return BinOp::Rem;
                case TokenKind::kLt: return BinOp::Lt;
                case TokenKind::kLtEq: return BinOp::Le;
                case TokenKind::kGt: return BinOp::Gt;
                case TokenKind::kGtEq: return BinOp::Ge;
                case TokenKind::kEqEq: return BinOp::Eq;
                case TokenKind::kBangEq: return BinOp::Ne;
                case TokenKind::kKwAnd: return BinOp::LogicalAnd;
                case TokenKind::kKwOr: return BinOp::LogicalOr;
                default: return std::nullopt;
            }
        }

        std::optional<UnOp> map_unop_(TokenKind tk) {
            switch (tk) {
                case TokenKind::kPlus: return UnOp::Plus;
                case TokenKind::kMinus: return UnOp::Neg;
                case TokenKind::kKwNot: return UnOp::Not;
                case TokenKind::kBang: return UnOp::BitNot;
                default: return std::nullopt;
            }
        }

        std::optional<CastKind> map_cast_(uint32_t raw) {
            switch (static_cast<parus::ast::CastKind>(raw)) {
                case parus::ast::CastKind::kAs: return CastKind::As;
                case parus::ast::CastKind::kAsOptional: return CastKind::AsQ;
                case parus::ast::CastKind::kAsForce: return CastKind::AsB;
            }
            return std::nullopt;
        }

        class Builder {
        public:
            Builder(const parus::sir::Module& sir, const parus::ty::TypePool& types)
                : sir_(sir), types_(types) {}

            BuildResult build() {
                mod_.header.stage_kind = StageKind::Open;
                copy_record_layouts_();
                scan_global_switch_lane_rejections_();
                if (!messages_.empty()) return finish_();

                discover_supported_funcs_();
                if (!messages_.empty()) return finish_();

                for (size_t i = 0; i < sir_.funcs.size(); ++i) {
                    const auto& fn = sir_.funcs[i];
                    if (!supported_func_[static_cast<uint32_t>(i)]) continue;
                    lower_func_(static_cast<uint32_t>(i), fn);
                }

                run_open_passes(mod_);
                const auto verrs = verify(mod_);
                messages_.insert(messages_.end(), verrs.begin(), verrs.end());
                return finish_();
            }

        private:
            struct Binding {
                ValueId value = kInvalidId;
                TypeId type = kInvalidType;
                LayoutClass layout = LayoutClass::Unknown;
                bool is_place = false;
                bool is_mutable = false;
            };

            struct FuncState {
                RealizationId realization = kInvalidId;
                BlockId current_block = kInvalidId;
                TypeId return_type = kInvalidType;
                std::unordered_map<parus::sir::SymbolId, Binding> locals{};
            };

            const parus::sir::Module& sir_;
            const parus::ty::TypePool& types_;
            Module mod_{};
            std::vector<Message> messages_{};
            std::unordered_map<parus::sir::SymbolId, ComputationId> computation_by_sym_{};
            std::unordered_map<uint32_t, ComputationId> computation_by_func_index_{};
            std::unordered_map<uint32_t, RealizationId> realization_by_func_index_{};
            std::unordered_map<uint32_t, bool> supported_func_{};

            BuildResult finish_() {
                BuildResult out{};
                out.ok = messages_.empty();
                out.mod = std::move(mod_);
                out.messages = std::move(messages_);
                return out;
            }

            void push_error_(std::string text) {
                messages_.push_back(Message{std::move(text)});
            }

            const RecordLayout* find_record_layout_(TypeId ty) const {
                return mod_.find_record_layout(ty);
            }

            bool is_optional_type_(TypeId ty) const {
                if (ty == kInvalidType) return false;
                return types_.get(ty).kind == parus::ty::Kind::kOptional;
            }

            TypeId optional_elem_(TypeId ty) const {
                if (!is_optional_type_(ty)) return kInvalidType;
                return types_.get(ty).elem;
            }

            bool is_tag_only_enum_type_(TypeId ty) const {
                const auto* layout = find_record_layout_(ty);
                return layout != nullptr &&
                       layout->fields.size() == 1 &&
                       mod_.string(layout->fields[0].name) == "__tag";
            }

            bool is_supported_value_type_(TypeId ty) const {
                const auto layout = classify_layout_(ty);
                return layout == LayoutClass::Scalar ||
                       layout == LayoutClass::TextView ||
                       layout == LayoutClass::OptionalScalar ||
                       layout == LayoutClass::TagEnum;
            }

            LayoutClass classify_layout_(TypeId ty) const {
                if (ty == kInvalidType) return LayoutClass::Unknown;
                const auto& t = types_.get(ty);
                switch (t.kind) {
                    case parus::ty::Kind::kBuiltin:
                        if (t.builtin == parus::ty::Builtin::kUnit) return LayoutClass::Scalar;
                        if (t.builtin == parus::ty::Builtin::kText) return LayoutClass::TextView;
                        return is_supported_scalar_type_(types_, ty) ? LayoutClass::Scalar : LayoutClass::Unknown;
                    case parus::ty::Kind::kOptional: {
                        const auto elem_layout = classify_layout_(t.elem);
                        return (elem_layout == LayoutClass::Scalar ||
                                elem_layout == LayoutClass::TextView ||
                                elem_layout == LayoutClass::TagEnum)
                            ? LayoutClass::OptionalScalar
                            : LayoutClass::Unknown;
                    }
                    case parus::ty::Kind::kArray:
                        return t.array_has_size ? LayoutClass::FixedArray : LayoutClass::SliceView;
                    case parus::ty::Kind::kNamedUser:
                        if (is_tag_only_enum_type_(ty)) return LayoutClass::TagEnum;
                        return find_record_layout_(ty) != nullptr ? LayoutClass::PlainRecord : LayoutClass::Unknown;
                    default:
                        return LayoutClass::Unknown;
                }
            }

            bool is_supported_local_type_(TypeId ty) const {
                const auto layout = classify_layout_(ty);
                return layout == LayoutClass::Scalar ||
                       layout == LayoutClass::FixedArray ||
                       layout == LayoutClass::SliceView ||
                       layout == LayoutClass::PlainRecord ||
                       layout == LayoutClass::TextView ||
                       layout == LayoutClass::OptionalScalar ||
                       layout == LayoutClass::TagEnum;
            }

            bool is_supported_signature_type_(TypeId ty) const {
                return is_unit_type_(types_, ty) || is_supported_value_type_(ty);
            }

            void scan_global_switch_lane_rejections_() {
                for (const auto& sc : sir_.switch_cases) {
                    if (!sc.is_default &&
                        sc.pat_kind == parus::sir::SwitchCasePatKind::kString) {
                        push_error_("gOIR official lane does not support string switch patterns in this round.");
                        return;
                    }
                    if (sc.enum_bind_count != 0) {
                        push_error_("gOIR official lane does not support payload enum switch bindings in this round.");
                        return;
                    }
                }
            }

            void copy_record_layouts_() {
                for (const auto& field : sir_.fields) {
                    RecordLayout layout{};
                    layout.self_type = field.self_type;
                    const uint64_t begin = field.member_begin;
                    const uint64_t end = begin + field.member_count;
                    if (begin > sir_.field_members.size() || end > sir_.field_members.size()) continue;
                    for (uint32_t i = field.member_begin; i < field.member_begin + field.member_count; ++i) {
                        const auto& member = sir_.field_members[i];
                        layout.fields.push_back(RecordField{
                            .name = mod_.add_string(std::string(member.name)),
                            .type = member.type,
                        });
                    }
                    mod_.add_record_layout(layout);
                }
            }

            bool is_supported_signature_(const parus::sir::Func& fn) {
                if (fn.is_extern || fn.is_throwing || fn.abi != parus::sir::FuncAbi::kParus ||
                    fn.is_actor_member || fn.is_actor_init || fn.is_acts_member) {
                    push_error_("gOIR M1 does not support function '" + std::string(fn.name) +
                                "' in the official lane because it is not a pure internal CPU entry.");
                    return false;
                }
                if (!is_supported_signature_type_(fn.ret)) {
                    push_error_("gOIR M1 does not support return type of function '" + std::string(fn.name) + "'.");
                    return false;
                }
                for (uint32_t i = 0; i < fn.param_count; ++i) {
                    const auto& param = sir_.params[fn.param_begin + i];
                    if (!is_supported_signature_type_(param.type)) {
                        push_error_("gOIR M1 does not support parameter type of function '" +
                                    std::string(fn.name) + "'.");
                        return false;
                    }
                }
                return true;
            }

            void discover_supported_funcs_() {
                for (uint32_t i = 0; i < sir_.funcs.size(); ++i) {
                    const auto& fn = sir_.funcs[i];
                    const bool is_pure = fn.is_pure || has_attr_(sir_, fn, "pure");
                    if (!is_pure) {
                        push_error_("gOIR M1 requires pure functions; unsupported function '" +
                                    std::string(fn.name) + "'.");
                        continue;
                    }
                    if (!is_supported_signature_(fn)) continue;

                    const auto name = mod_.add_string(std::string(fn.name));

                    SemanticSig sig{};
                    sig.name = name;
                    sig.result_type = fn.ret;
                    sig.is_pure = is_pure;
                    sig.is_throwing = fn.is_throwing;
                    for (uint32_t pi = 0; pi < fn.param_count; ++pi) {
                        sig.param_types.push_back(sir_.params[fn.param_begin + pi].type);
                    }
                    const auto sig_id = mod_.add_semantic_sig(sig);

                    const auto policy_id = mod_.add_placement_policy(GPlacementPolicy{});

                    GComputation comp{};
                    comp.name = name;
                    comp.sig = sig_id;
                    comp.placement_policy = policy_id;
                    const auto comp_id = mod_.add_computation(comp);
                    computation_by_func_index_[i] = comp_id;

                    GRealization real{};
                    real.name = name;
                    real.computation = comp_id;
                    real.family = FamilyKind::Core;
                    real.is_entry = !fn.is_extern;
                    real.is_pure = is_pure;
                    real.is_extern = fn.is_extern;
                    real.fn_type = fn.sig;
                    real.source_func = i;
                    const auto real_id = mod_.add_realization(real);
                    realization_by_func_index_[i] = real_id;

                    mod_.computations[comp_id].realizations.push_back(real_id);
                    computation_by_sym_[fn.sym] = comp_id;
                    supported_func_[i] = true;
                }
            }

            ValueId add_block_param_(BlockId bb, TypeId ty) {
                Value value{};
                value.ty = ty;
                value.place_elem_type = ty;
                value.eff = Effect::Pure;
                value.layout = classify_layout_(ty);
                value.def_a = bb;
                value.def_b = static_cast<uint32_t>(mod_.blocks[bb].params.size());
                const auto vid = mod_.add_value(value);
                mod_.blocks[bb].params.push_back(vid);
                return vid;
            }

            ValueId emit_inst_(FuncState& state,
                               TypeId result_ty,
                               Effect eff,
                               OpData data,
                               OwnershipInfo ownership = {},
                               LayoutClass layout = LayoutClass::Unknown,
                               bool is_place = false,
                               TypeId place_elem_type = kInvalidType,
                               PlaceKind place_kind = PlaceKind::None,
                               bool is_mutable = false) {
                ValueId result = kInvalidId;
                if (result_ty != kInvalidType && (!is_unit_type_(types_, result_ty) || is_place)) {
                    Value value{};
                    value.ty = result_ty;
                    value.place_elem_type = (place_elem_type == kInvalidType) ? result_ty : place_elem_type;
                    value.eff = eff;
                    value.ownership = ownership;
                    value.layout = (layout == LayoutClass::Unknown) ? classify_layout_(result_ty) : layout;
                    value.place_kind = place_kind;
                    value.is_place = is_place;
                    value.is_mutable = is_mutable;
                    value.def_a = static_cast<uint32_t>(mod_.insts.size());
                    result = mod_.add_value(value);
                }

                Inst inst{};
                inst.data = std::move(data);
                inst.eff = eff;
                inst.result = result;
                const auto iid = mod_.add_inst(inst);
                if (result != kInvalidId) mod_.values[result].def_a = iid;
                mod_.blocks[state.current_block].insts.push_back(iid);
                return result;
            }

            void ensure_block_term_(FuncState& state, Terminator term) {
                if (state.current_block == kInvalidId) return;
                auto& block = mod_.blocks[state.current_block];
                if (block.has_term) return;
                block.term = std::move(term);
                block.has_term = true;
            }

            std::optional<Binding> lookup_binding_(const FuncState& state, parus::sir::SymbolId sym) const {
                const auto it = state.locals.find(sym);
                if (it == state.locals.end()) return std::nullopt;
                return it->second;
            }

            TypeId lookup_record_field_type_(TypeId record_ty, std::string_view field_name) const {
                const auto* layout = find_record_layout_(record_ty);
                if (layout == nullptr) return kInvalidType;
                for (const auto& field : layout->fields) {
                    if (mod_.string(field.name) == field_name) return field.type;
                }
                return kInvalidType;
            }

            bool binding_place_reads_as_value_(LayoutClass layout) const {
                return layout == LayoutClass::Scalar ||
                       layout == LayoutClass::TextView ||
                       layout == LayoutClass::OptionalScalar ||
                       layout == LayoutClass::TagEnum;
            }

            std::optional<parus::sir::ValueId> labeled_arg_value_(const parus::sir::Value& value,
                                                                  std::string_view label) const {
                const uint64_t begin = value.arg_begin;
                const uint64_t end = begin + value.arg_count;
                if (begin > sir_.args.size() || end > sir_.args.size()) return std::nullopt;
                for (uint32_t i = 0; i < value.arg_count; ++i) {
                    const auto& arg = sir_.args[value.arg_begin + i];
                    if (!arg.has_label || arg.label != label || arg.is_hole) continue;
                    if (arg.value == parus::sir::k_invalid_value) continue;
                    return arg.value;
                }
                return std::nullopt;
            }

            bool is_supported_switch_scrutinee_type_(TypeId ty) const {
                const auto layout = classify_layout_(ty);
                if (layout == LayoutClass::TagEnum) return true;
                if (is_optional_type_(ty)) return true;
                if (ty == kInvalidType) return false;
                const auto& t = types_.get(ty);
                if (t.kind != parus::ty::Kind::kBuiltin) return false;
                using B = parus::ty::Builtin;
                switch (t.builtin) {
                    case B::kBool:
                    case B::kChar:
                    case B::kI8:
                    case B::kI16:
                    case B::kI32:
                    case B::kI64:
                    case B::kU8:
                    case B::kU16:
                    case B::kU32:
                    case B::kU64:
                    case B::kISize:
                    case B::kUSize:
                        return true;
                    default:
                        return false;
                }
            }

            bool looks_like_supported_range_value_(parus::sir::ValueId sid) const {
                if (sid == parus::sir::k_invalid_value || static_cast<size_t>(sid) >= sir_.values.size()) return false;
                const auto& v = sir_.values[sid];
                if (v.kind != ValueKind::kFieldInit) return false;
                if (v.type == parus::sir::k_invalid_type) return false;
                const auto& t = types_.get(v.type);
                if (t.kind != parus::ty::Kind::kNamedUser) return false;
                std::vector<std::string_view> path{};
                std::vector<TypeId> args{};
                if (!types_.decompose_named_user(v.type, path, args) || path.empty()) return false;
                const auto leaf = path.back();
                return leaf == "Range" || leaf == "RangeInclusive";
            }

            ValueId lower_value_as_(FuncState& state, parus::sir::ValueId sid, TypeId target_ty) {
                if (sid == parus::sir::k_invalid_value || static_cast<size_t>(sid) >= sir_.values.size()) {
                    push_error_("gOIR builder saw invalid SIR value id during coercion.");
                    return kInvalidId;
                }
                if (target_ty == kInvalidType) return lower_value_(state, sid);
                const auto& src = sir_.values[sid];
                if (src.type == target_ty) return lower_value_(state, sid);

                if (is_optional_type_(target_ty)) {
                    if (classify_layout_(target_ty) != LayoutClass::OptionalScalar) {
                        push_error_("gOIR official lane only supports optionals over scalar/text/tag-enum payloads.");
                        return kInvalidId;
                    }
                    if (src.kind == ValueKind::kNullLit) {
                        return emit_inst_(
                            state,
                            target_ty,
                            Effect::Pure,
                            OpOptionalNone{},
                            {},
                            LayoutClass::OptionalScalar
                        );
                    }
                    const auto elem_ty = optional_elem_(target_ty);
                    if (src.type == elem_ty) {
                        const auto payload = lower_value_(state, sid);
                        if (payload == kInvalidId) return kInvalidId;
                        return emit_inst_(
                            state,
                            target_ty,
                            Effect::Pure,
                            OpOptionalSome{.value = payload},
                            {},
                            LayoutClass::OptionalScalar
                        );
                    }
                }

                const auto lowered = lower_value_(state, sid);
                if (lowered == kInvalidId) return kInvalidId;
                if (mod_.values[lowered].ty == target_ty) return lowered;

                push_error_("gOIR official lane encountered an unsupported implicit coercion from '" +
                            types_.to_string(src.type) + "' to '" + types_.to_string(target_ty) + "'.");
                return kInvalidId;
            }

            ValueId lower_range_subview_(FuncState& state, const parus::sir::Value& value) {
                if (!looks_like_supported_range_value_(value.b)) {
                    push_error_("gOIR builder expected a range literal for source subview lowering.");
                    return kInvalidId;
                }
                const auto& range = sir_.values[value.b];
                const auto start_sid = labeled_arg_value_(range, "start");
                const auto end_sid = labeled_arg_value_(range, "end");
                if (!start_sid.has_value() || !end_sid.has_value()) {
                    push_error_("gOIR source subview lowering requires Range/RangeInclusive { start, end } fields.");
                    return kInvalidId;
                }

                std::vector<std::string_view> path{};
                std::vector<TypeId> args{};
                bool inclusive = false;
                if (types_.decompose_named_user(range.type, path, args) && !path.empty()) {
                    inclusive = (path.back() == "RangeInclusive");
                }

                const auto base = lower_place_(state, value.a);
                const auto start = lower_value_(state, *start_sid);
                const auto end = lower_value_(state, *end_sid);
                if (base == kInvalidId || start == kInvalidId || end == kInvalidId) return kInvalidId;

                const auto delta = emit_inst_(
                    state,
                    sir_.values[*end_sid].type,
                    Effect::Pure,
                    OpBinary{.op = BinOp::Sub, .lhs = end, .rhs = start},
                    {},
                    LayoutClass::Scalar
                );
                if (delta == kInvalidId) return kInvalidId;

                auto length = delta;
                if (inclusive) {
                    const auto one = emit_inst_(
                        state,
                        mod_.values[delta].ty,
                        Effect::Pure,
                        OpConstInt{"1"},
                        {},
                        LayoutClass::Scalar
                    );
                    if (one == kInvalidId) return kInvalidId;
                    length = emit_inst_(
                        state,
                        mod_.values[delta].ty,
                        Effect::Pure,
                        OpBinary{.op = BinOp::Add, .lhs = delta, .rhs = one},
                        {},
                        LayoutClass::Scalar
                    );
                }

                return emit_inst_(
                    state,
                    value.type,
                    Effect::Pure,
                    OpSubView{.base = base, .offset = start, .length = length},
                    {},
                    LayoutClass::SliceView,
                    true,
                    value.type,
                    PlaceKind::SubView,
                    true
                );
            }

            ValueId lower_optional_coalesce_(FuncState& state, const parus::sir::Value& value) {
                const auto lhs_ty = sir_.values[value.a].type;
                if (!is_optional_type_(lhs_ty)) {
                    push_error_("gOIR builder requires optional lhs for null-coalescing.");
                    return kInvalidId;
                }
                const auto elem_ty = optional_elem_(lhs_ty);
                if (elem_ty == kInvalidType) {
                    push_error_("gOIR builder saw invalid optional element type in null-coalescing.");
                    return kInvalidId;
                }

                const auto lhs = lower_value_as_(state, value.a, lhs_ty);
                if (lhs == kInvalidId) return kInvalidId;

                const auto bool_ty = types_.builtin(parus::ty::Builtin::kBool);
                const auto present = emit_inst_(
                    state,
                    bool_ty,
                    Effect::Pure,
                    OpOptionalIsPresent{.optional = lhs},
                    {},
                    LayoutClass::Scalar
                );
                if (present == kInvalidId) return kInvalidId;

                const auto some_bb = mod_.add_block(Block{});
                const auto none_bb = mod_.add_block(Block{});
                const auto cont_bb = mod_.add_block(Block{});

                auto& real = mod_.realizations[state.realization];
                real.blocks.push_back(some_bb);
                real.blocks.push_back(none_bb);
                real.blocks.push_back(cont_bb);

                const auto joined = add_block_param_(cont_bb, value.type);

                ensure_block_term_(state, TermCondBr{
                    .cond = present,
                    .then_bb = some_bb,
                    .else_bb = none_bb,
                });

                auto some_state = state;
                some_state.current_block = some_bb;
                const auto some_value = emit_inst_(
                    some_state,
                    elem_ty,
                    Effect::Pure,
                    OpOptionalGet{.optional = lhs},
                    {},
                    classify_layout_(elem_ty)
                );
                if (some_value == kInvalidId) return kInvalidId;
                ensure_block_term_(some_state, TermBr{
                    .target = cont_bb,
                    .args = {some_value},
                });

                auto none_state = state;
                none_state.current_block = none_bb;
                const auto fallback = lower_value_as_(none_state, value.b, value.type);
                if (fallback == kInvalidId) return kInvalidId;
                ensure_block_term_(none_state, TermBr{
                    .target = cont_bb,
                    .args = {fallback},
                });

                state.current_block = cont_bb;
                return joined;
            }

            ValueId lower_place_(FuncState& state, parus::sir::ValueId sid) {
                if (sid == parus::sir::k_invalid_value || static_cast<size_t>(sid) >= sir_.values.size()) {
                    push_error_("gOIR builder saw invalid SIR place id.");
                    return kInvalidId;
                }
                const auto& value = sir_.values[sid];
                switch (value.kind) {
                    case ValueKind::kLocal: {
                        const auto binding = lookup_binding_(state, value.sym);
                        if (!binding.has_value()) {
                            push_error_("gOIR builder could not resolve local symbol in SIR place.");
                            return kInvalidId;
                        }
                        if (!binding->is_place) {
                            push_error_("gOIR official lane cannot take a place from an immutable value binding.");
                            return kInvalidId;
                        }
                        return binding->value;
                    }
                    case ValueKind::kField: {
                        const auto base = lower_place_(state, value.a);
                        if (base == kInvalidId) return kInvalidId;
                        const auto field_ty =
                            (value.place_elem_type != parus::sir::k_invalid_type) ? value.place_elem_type : value.type;
                        return emit_inst_(
                            state,
                            value.type,
                            Effect::Pure,
                            OpFieldPlace{.base = base, .field_name = mod_.add_string(std::string(value.text))},
                            {},
                            classify_layout_(field_ty),
                            true,
                            field_ty,
                            PlaceKind::FieldPath,
                            true
                        );
                    }
                    case ValueKind::kIndex: {
                        if (looks_like_supported_range_value_(value.b)) {
                            return lower_range_subview_(state, value);
                        }
                        const auto base = lower_place_(state, value.a);
                        const auto index = lower_value_(state, value.b);
                        if (base == kInvalidId || index == kInvalidId) return kInvalidId;
                        const auto elem_ty =
                            (value.place_elem_type != parus::sir::k_invalid_type) ? value.place_elem_type : value.type;
                        return emit_inst_(
                            state,
                            value.type,
                            Effect::Pure,
                            OpIndexPlace{.base = base, .index = index},
                            {},
                            classify_layout_(elem_ty),
                            true,
                            elem_ty,
                            PlaceKind::IndexPath,
                            true
                        );
                    }
                    default:
                        push_error_("gOIR M1 encountered an unsupported SIR place form.");
                        return kInvalidId;
                }
            }

            ValueId lower_record_make_(FuncState& state, const parus::sir::Value& value) {
                if (find_record_layout_(value.type) == nullptr) {
                    push_error_("gOIR M1 field initializer requires a plain internal record layout.");
                    return kInvalidId;
                }
                OpRecordMake make{};
                const uint64_t arg_end = static_cast<uint64_t>(value.arg_begin) + static_cast<uint64_t>(value.arg_count);
                if (arg_end > sir_.args.size()) {
                    push_error_("gOIR builder saw invalid field-init arg slice.");
                    return kInvalidId;
                }
                for (uint32_t i = 0; i < value.arg_count; ++i) {
                    const auto& arg = sir_.args[value.arg_begin + i];
                    if (arg.is_hole || arg.value == parus::sir::k_invalid_value || !arg.has_label) continue;
                    const auto field_ty = lookup_record_field_type_(value.type, arg.label);
                    make.fields.push_back(RecordValueField{
                        .name = mod_.add_string(std::string(arg.label)),
                        .value = (field_ty == kInvalidType)
                            ? lower_value_(state, arg.value)
                            : lower_value_as_(state, arg.value, field_ty),
                    });
                }
                return emit_inst_(state, value.type, Effect::Pure, std::move(make), {}, LayoutClass::PlainRecord);
            }

            ValueId lower_value_(FuncState& state, parus::sir::ValueId sid) {
                if (sid == parus::sir::k_invalid_value || static_cast<size_t>(sid) >= sir_.values.size()) {
                    push_error_("gOIR builder saw invalid SIR value id.");
                    return kInvalidId;
                }
                const auto& value = sir_.values[sid];
                switch (value.kind) {
                    case ValueKind::kIntLit:
                        return emit_inst_(state, value.type, Effect::Pure, OpConstInt{std::string(value.text)});
                    case ValueKind::kFloatLit:
                        return emit_inst_(state, value.type, Effect::Pure, OpConstFloat{std::string(value.text)});
                    case ValueKind::kStringLit:
                        return emit_inst_(
                            state,
                            value.type,
                            Effect::Pure,
                            OpTextLit{.quoted_text = std::string(value.text)},
                            {},
                            LayoutClass::TextView
                        );
                    case ValueKind::kCharLit: {
                        const auto code = parse_char_literal_code_(value.text);
                        if (!code.has_value()) {
                            push_error_("gOIR builder could not decode char literal.");
                            return kInvalidId;
                        }
                        return emit_inst_(
                            state,
                            value.type,
                            Effect::Pure,
                            OpConstInt{std::to_string(*code)},
                            {},
                            LayoutClass::Scalar
                        );
                    }
                    case ValueKind::kBoolLit:
                        return emit_inst_(state, value.type, Effect::Pure,
                                          OpConstBool{value.text == "true" || value.text == "1"});
                    case ValueKind::kNullLit:
                        if (is_optional_type_(value.type) &&
                            classify_layout_(value.type) == LayoutClass::OptionalScalar) {
                            return emit_inst_(
                                state,
                                value.type,
                                Effect::Pure,
                                OpOptionalNone{},
                                {},
                                LayoutClass::OptionalScalar
                            );
                        }
                        return emit_inst_(state, value.type, Effect::Pure, OpConstNull{});
                    case ValueKind::kLocal: {
                        const auto binding = lookup_binding_(state, value.sym);
                        if (!binding.has_value()) {
                            push_error_("gOIR builder could not resolve local symbol in SIR.");
                            return kInvalidId;
                        }
                        if (!binding->is_place) return binding->value;
                        if (binding_place_reads_as_value_(binding->layout)) {
                            return emit_inst_(
                                state,
                                binding->type,
                                Effect::MayRead,
                                OpLoad{.place = binding->value},
                                {},
                                classify_layout_(binding->type)
                            );
                        }
                        return binding->value;
                    }
                    case ValueKind::kUnary: {
                        const auto src = lower_value_(state, value.a);
                        const auto op = map_unop_(static_cast<TokenKind>(value.op));
                        if (!op.has_value()) {
                            push_error_("unsupported unary operator in gOIR M1 builder.");
                            return kInvalidId;
                        }
                        return emit_inst_(state, value.type, Effect::Pure, OpUnary{*op, src});
                    }
                    case ValueKind::kBinary: {
                        if (static_cast<TokenKind>(value.op) == TokenKind::kQuestionQuestion) {
                            return lower_optional_coalesce_(state, value);
                        }
                        const auto lhs = lower_value_(state, value.a);
                        const auto rhs = lower_value_(state, value.b);
                        const auto op = map_binop_(static_cast<TokenKind>(value.op));
                        if (!op.has_value()) {
                            push_error_("unsupported binary operator in gOIR M1 builder.");
                            return kInvalidId;
                        }
                        return emit_inst_(state, value.type, Effect::Pure, OpBinary{*op, lhs, rhs});
                    }
                    case ValueKind::kCast: {
                        const auto src = lower_value_(state, value.a);
                        const auto cast = map_cast_(value.op);
                        if (!cast.has_value()) {
                            push_error_("unsupported cast operator in gOIR M1 builder.");
                            return kInvalidId;
                        }
                        return emit_inst_(state, value.type,
                                          (*cast == CastKind::AsB) ? Effect::MayTrap : Effect::Pure,
                                          OpCast{*cast, value.cast_to, src});
                    }
                    case ValueKind::kCall:
                    case ValueKind::kPipeCall: {
                        if (value.core_call_kind != parus::sir::CoreCallKind::kNone) {
                            push_error_("gOIR M1 does not support core runtime helper calls yet.");
                            return kInvalidId;
                        }
                        if (value.call_is_throwing || value.call_is_c_abi) {
                            push_error_("gOIR M1 does not support throwing or C ABI calls.");
                            return kInvalidId;
                        }
                        const auto cit = computation_by_sym_.find(value.callee_sym);
                        if (cit == computation_by_sym_.end()) {
                            push_error_("gOIR M1 only supports direct pure/internal calls.");
                            return kInvalidId;
                        }
                        OpSemanticInvoke invoke{};
                        invoke.computation = cit->second;
                        const auto sig_id = mod_.computations[cit->second].sig;
                        const auto& sig = mod_.semantic_sigs[sig_id];
                        for (uint32_t i = 0; i < value.arg_count; ++i) {
                            const auto aid = value.arg_begin + i;
                            if (static_cast<size_t>(aid) >= sir_.args.size()) {
                                push_error_("gOIR builder saw invalid SIR arg slice.");
                                return kInvalidId;
                            }
                            const auto& arg = sir_.args[aid];
                            if (arg.is_hole) {
                                push_error_("gOIR M1 does not support hole arguments.");
                                return kInvalidId;
                            }
                            const auto param_ty =
                                (i < sig.param_types.size()) ? sig.param_types[i] : sir_.values[arg.value].type;
                            invoke.args.push_back(lower_value_as_(state, arg.value, param_ty));
                        }
                        return emit_inst_(state, value.type, Effect::Call, std::move(invoke));
                    }
                    case ValueKind::kBorrow: {
                        OwnershipInfo ownership{};
                        ownership.kind = value.borrow_is_mut ? OwnershipKind::BorrowMut : OwnershipKind::BorrowShared;
                        ownership.requires_runtime_lowering = true;
                        const auto place = lower_place_(state, value.a);
                        return emit_inst_(
                            state,
                            value.type,
                            Effect::Pure,
                            OpBorrowView{.source_place = place},
                            ownership,
                            classify_layout_(value.type)
                        );
                    }
                    case ValueKind::kEscape: {
                        OwnershipInfo ownership{};
                        ownership.kind = OwnershipKind::Escape;
                        ownership.requires_runtime_lowering = true;
                        for (const auto& handle : sir_.escape_handles) {
                            if (handle.escape_value == sid) {
                                ownership.escape_kind = handle.kind;
                                ownership.escape_boundary = handle.boundary;
                                ownership.from_static = handle.from_static;
                                break;
                            }
                        }
                        const auto place = lower_place_(state, value.a);
                        return emit_inst_(
                            state,
                            value.type,
                            Effect::MayTrap,
                            OpEscapeView{.source_place = place},
                            ownership,
                            classify_layout_(value.type)
                        );
                    }
                    case ValueKind::kEnumCtor: {
                        if (value.arg_count != 0 || !is_tag_only_enum_type_(value.type)) {
                            push_error_("gOIR official lane only supports tag-only enum constructors in this round.");
                            return kInvalidId;
                        }
                        return emit_inst_(
                            state,
                            value.type,
                            Effect::Pure,
                            OpEnumTag{.tag = value.enum_ctor_tag_value},
                            {},
                            LayoutClass::TagEnum
                        );
                    }
                    case ValueKind::kAssign: {
                        const auto place = lower_place_(state, value.a);
                        const auto elem_ty =
                            (place != kInvalidId) ? mod_.values[place].place_elem_type : kInvalidType;
                        const auto rhs = lower_value_as_(state, value.b, elem_ty);
                        if (place == kInvalidId || rhs == kInvalidId) return kInvalidId;
                        emit_inst_(state, kInvalidType, Effect::MayWrite, OpStore{.place = place, .value = rhs});
                        return rhs;
                    }
                    case ValueKind::kArrayLit: {
                        OpArrayMake make{};
                        const uint64_t arg_end = static_cast<uint64_t>(value.arg_begin) + static_cast<uint64_t>(value.arg_count);
                        if (arg_end > sir_.args.size()) {
                            push_error_("gOIR builder saw invalid array-literal arg slice.");
                            return kInvalidId;
                        }
                        TypeId elem_ty = kInvalidType;
                        if (value.type != kInvalidType) {
                            const auto& arr = types_.get(value.type);
                            if (arr.kind == parus::ty::Kind::kArray) elem_ty = arr.elem;
                        }
                        for (uint32_t i = 0; i < value.arg_count; ++i) {
                            const auto& arg = sir_.args[value.arg_begin + i];
                            if (arg.is_hole || arg.value == parus::sir::k_invalid_value) continue;
                            make.elems.push_back(
                                (elem_ty == kInvalidType)
                                    ? lower_value_(state, arg.value)
                                    : lower_value_as_(state, arg.value, elem_ty)
                            );
                        }
                        return emit_inst_(state, value.type, Effect::Pure, std::move(make), {}, LayoutClass::FixedArray);
                    }
                    case ValueKind::kFieldInit:
                        return lower_record_make_(state, value);
                    case ValueKind::kIndex: {
                        if (looks_like_supported_range_value_(value.b)) {
                            return lower_place_(state, sid);
                        }
                        const auto place = lower_place_(state, sid);
                        if (place == kInvalidId) return kInvalidId;
                        return emit_inst_(state, value.type, Effect::MayRead, OpLoad{.place = place});
                    }
                    case ValueKind::kField: {
                        const auto base = lower_value_(state, value.a);
                        if (base == kInvalidId) return kInvalidId;
                        if (value.text == "len") {
                            const auto base_ty = sir_.values[value.a].type;
                            if (base_ty != parus::sir::k_invalid_type) {
                                const auto& bt = types_.get(base_ty);
                                if (bt.kind == parus::ty::Kind::kArray && bt.array_has_size) {
                                    return emit_inst_(state, value.type, Effect::Pure,
                                                      OpConstInt{std::to_string(bt.array_size)});
                                }
                            }
                            return emit_inst_(state, value.type, Effect::Pure, OpArrayLen{.base = base});
                        }
                        const auto place = lower_place_(state, sid);
                        if (place == kInvalidId) return kInvalidId;
                        return emit_inst_(state, value.type, Effect::MayRead, OpLoad{.place = place});
                    }
                    default:
                        push_error_("gOIR M1 builder encountered unsupported SIR value kind.");
                        return kInvalidId;
                }
            }

            void lower_stmt_range_(FuncState& state, uint32_t begin, uint32_t count) {
                for (uint32_t i = 0; i < count; ++i) {
                    if (state.current_block == kInvalidId) return;
                    const auto sid = begin + i;
                    if (static_cast<size_t>(sid) >= sir_.stmts.size()) {
                        push_error_("gOIR builder saw invalid SIR stmt id.");
                        return;
                    }
                    lower_stmt_(state, sir_.stmts[sid]);
                }
            }

            void lower_block_(FuncState& state, parus::sir::BlockId block_id) {
                if (block_id == parus::sir::k_invalid_block || static_cast<size_t>(block_id) >= sir_.blocks.size()) {
                    push_error_("gOIR builder saw invalid SIR block id.");
                    return;
                }
                const auto& block = sir_.blocks[block_id];
                lower_stmt_range_(state, block.stmt_begin, block.stmt_count);
            }

            void lower_if_stmt_(FuncState& state, const parus::sir::Stmt& stmt) {
                const auto cond = lower_value_(state, stmt.expr);
                const auto then_bb = mod_.add_block(Block{});
                const auto else_bb = mod_.add_block(Block{});
                const auto cont_bb = mod_.add_block(Block{});

                auto& real = mod_.realizations[state.realization];
                real.blocks.push_back(then_bb);
                real.blocks.push_back(else_bb);
                real.blocks.push_back(cont_bb);

                ensure_block_term_(state, TermCondBr{
                    .cond = cond,
                    .then_bb = then_bb,
                    .else_bb = else_bb,
                });

                auto then_state = state;
                then_state.current_block = then_bb;
                lower_block_(then_state, stmt.a);
                if (then_state.current_block != kInvalidId &&
                    !mod_.blocks[then_state.current_block].has_term) {
                    ensure_block_term_(then_state, TermBr{.target = cont_bb});
                }

                auto else_state = state;
                else_state.current_block = else_bb;
                if (stmt.b != parus::sir::k_invalid_block) {
                    lower_block_(else_state, stmt.b);
                }
                if (else_state.current_block != kInvalidId &&
                    !mod_.blocks[else_state.current_block].has_term) {
                    ensure_block_term_(else_state, TermBr{.target = cont_bb});
                }

                state.current_block = cont_bb;
            }

            void lower_while_stmt_(FuncState& state, const parus::sir::Stmt& stmt) {
                const auto cond_bb = mod_.add_block(Block{});
                const auto body_bb = mod_.add_block(Block{});
                const auto after_bb = mod_.add_block(Block{});

                auto& real = mod_.realizations[state.realization];
                real.blocks.push_back(cond_bb);
                real.blocks.push_back(body_bb);
                real.blocks.push_back(after_bb);

                ensure_block_term_(state, TermBr{.target = cond_bb});

                auto cond_state = state;
                cond_state.current_block = cond_bb;
                const auto cond = lower_value_(cond_state, stmt.expr);
                ensure_block_term_(cond_state, TermCondBr{
                    .cond = cond,
                    .then_bb = body_bb,
                    .else_bb = after_bb,
                });

                auto body_state = state;
                body_state.current_block = body_bb;
                lower_block_(body_state, stmt.a);
                if (body_state.current_block != kInvalidId &&
                    !mod_.blocks[body_state.current_block].has_term) {
                    ensure_block_term_(body_state, TermBr{.target = cond_bb});
                }

                state.current_block = after_bb;
            }

            std::optional<int64_t> switch_case_match_value_(TypeId scrut_ty,
                                                            const parus::sir::SwitchCase& sc,
                                                            bool optional_switch) {
                using Pat = parus::sir::SwitchCasePatKind;
                if (sc.is_default) return std::nullopt;

                if (optional_switch) {
                    if (sc.pat_kind == Pat::kNull) return int64_t{0};
                    push_error_("gOIR official lane only supports optional switch patterns for `null` and `default`.");
                    return std::nullopt;
                }

                switch (sc.pat_kind) {
                    case Pat::kInt:
                        if (const auto value = parse_i64_literal_(sc.pat_text); value.has_value()) {
                            return value;
                        }
                        push_error_("gOIR builder could not parse integer switch case literal.");
                        return std::nullopt;
                    case Pat::kBool:
                        return (sc.pat_text == "true") ? int64_t{1} : int64_t{0};
                    case Pat::kChar: {
                        const auto code = parse_char_literal_code_(sc.pat_text);
                        if (code.has_value()) return static_cast<int64_t>(*code);
                        push_error_("gOIR builder could not decode char switch case literal.");
                        return std::nullopt;
                    }
                    case Pat::kEnumVariant:
                        if (!is_tag_only_enum_type_(scrut_ty)) {
                            push_error_("gOIR official lane only supports enum switch on tag-only enums in this round.");
                            return std::nullopt;
                        }
                        if (sc.enum_bind_count != 0) {
                            push_error_("gOIR official lane does not support payload enum switch bindings in this round.");
                            return std::nullopt;
                        }
                        return sc.enum_tag_value;
                    case Pat::kNull:
                        push_error_("gOIR official lane only supports `case null` on optional scrutinees.");
                        return std::nullopt;
                    case Pat::kString:
                        push_error_("gOIR official lane does not support string switch patterns in this round.");
                        return std::nullopt;
                    case Pat::kIdent:
                        push_error_("gOIR official lane does not support identifier switch patterns in this round.");
                        return std::nullopt;
                    case Pat::kError:
                    default:
                        push_error_("gOIR builder saw an unsupported switch case pattern.");
                        return std::nullopt;
                }
            }

            void lower_switch_stmt_(FuncState& state, const parus::sir::Stmt& stmt) {
                const uint64_t case_end =
                    static_cast<uint64_t>(stmt.case_begin) + static_cast<uint64_t>(stmt.case_count);
                if (case_end > sir_.switch_cases.size()) {
                    push_error_("gOIR builder saw invalid switch case slice.");
                    return;
                }
                for (uint32_t i = 0; i < stmt.case_count; ++i) {
                    const auto& sc = sir_.switch_cases[stmt.case_begin + i];
                    if (!sc.is_default && sc.pat_kind == parus::sir::SwitchCasePatKind::kString) {
                        push_error_("gOIR official lane does not support string switch patterns in this round.");
                        return;
                    }
                    if (sc.enum_bind_count != 0) {
                        push_error_("gOIR official lane does not support payload enum switch bindings in this round.");
                        return;
                    }
                }

                if (stmt.expr == parus::sir::k_invalid_value ||
                    static_cast<size_t>(stmt.expr) >= sir_.values.size()) {
                    push_error_("gOIR builder saw invalid switch scrutinee.");
                    return;
                }
                const auto scrut_ty = sir_.values[stmt.expr].type;
                if (!is_supported_switch_scrutinee_type_(scrut_ty)) {
                    push_error_("gOIR official lane switch scrutinee is outside the supported int/char/bool/tag-enum/null subset.");
                    return;
                }

                const bool optional_switch = is_optional_type_(scrut_ty);
                const auto lowered_scrutinee =
                    optional_switch ? lower_value_as_(state, stmt.expr, scrut_ty) : lower_value_(state, stmt.expr);
                if (lowered_scrutinee == kInvalidId) return;

                ValueId switch_value = lowered_scrutinee;
                if (optional_switch) {
                    const auto bool_ty = types_.builtin(parus::ty::Builtin::kBool);
                    switch_value = emit_inst_(
                        state,
                        bool_ty,
                        Effect::Pure,
                        OpOptionalIsPresent{.optional = lowered_scrutinee},
                        {},
                        LayoutClass::Scalar
                    );
                    if (switch_value == kInvalidId) return;
                }

                const auto after_bb = mod_.add_block(Block{});
                auto& real = mod_.realizations[state.realization];
                real.blocks.push_back(after_bb);

                TermSwitch term{};
                term.scrutinee = switch_value;
                term.default_bb = after_bb;

                struct PendingCase {
                    BlockId block = kInvalidId;
                    parus::sir::BlockId body = parus::sir::k_invalid_block;
                };
                std::vector<PendingCase> pending{};

                for (uint32_t i = 0; i < stmt.case_count; ++i) {
                    const auto& sc = sir_.switch_cases[stmt.case_begin + i];
                    const auto case_bb = mod_.add_block(Block{});
                    real.blocks.push_back(case_bb);
                    pending.push_back(PendingCase{
                        .block = case_bb,
                        .body = sc.body,
                    });

                    if (sc.is_default) {
                        term.default_bb = case_bb;
                        continue;
                    }

                    const auto match = switch_case_match_value_(scrut_ty, sc, optional_switch);
                    if (!match.has_value()) return;
                    term.arms.push_back(SwitchArm{
                        .match_value = *match,
                        .target = case_bb,
                        .args = {},
                    });
                }

                ensure_block_term_(state, std::move(term));

                for (const auto& pending_case : pending) {
                    auto case_state = state;
                    case_state.current_block = pending_case.block;
                    if (pending_case.body != parus::sir::k_invalid_block) {
                        lower_block_(case_state, pending_case.body);
                    }
                    if (case_state.current_block != kInvalidId &&
                        !mod_.blocks[case_state.current_block].has_term) {
                        ensure_block_term_(case_state, TermBr{.target = after_bb});
                    }
                }

                state.current_block = after_bb;
            }

            void bind_local_(FuncState& state,
                             const parus::sir::Stmt& stmt,
                             TypeId binding_ty,
                             ValueId init_value) {
                const auto layout = classify_layout_(binding_ty);
                const bool needs_slot =
                    stmt.is_set || stmt.is_mut ||
                    layout == LayoutClass::FixedArray ||
                    layout == LayoutClass::PlainRecord;
                if (needs_slot) {
                    const auto slot = emit_inst_(
                        state,
                        binding_ty,
                        Effect::Pure,
                        OpLocalSlot{.debug_name = mod_.add_string(std::string(stmt.name))},
                        {},
                        layout,
                        true,
                        binding_ty,
                        PlaceKind::LocalSlot,
                        stmt.is_set || stmt.is_mut
                    );
                    if (init_value != kInvalidId) {
                        emit_inst_(state, kInvalidType, Effect::MayWrite, OpStore{.place = slot, .value = init_value});
                    }
                    state.locals[stmt.sym] = Binding{
                        .value = slot,
                        .type = binding_ty,
                        .layout = layout,
                        .is_place = true,
                        .is_mutable = stmt.is_set || stmt.is_mut,
                    };
                    return;
                }

                const bool init_is_place =
                    init_value != kInvalidId &&
                    static_cast<size_t>(init_value) < mod_.values.size() &&
                    mod_.values[init_value].is_place;
                state.locals[stmt.sym] = Binding{
                    .value = init_value,
                    .type = binding_ty,
                    .layout = layout,
                    .is_place = init_is_place,
                    .is_mutable = false,
                };
            }

            void lower_stmt_(FuncState& state, const parus::sir::Stmt& stmt) {
                switch (stmt.kind) {
                    case StmtKind::kExprStmt:
                        if (stmt.expr != parus::sir::k_invalid_value) {
                            (void)lower_value_(state, stmt.expr);
                        }
                        return;
                    case StmtKind::kVarDecl: {
                        if (stmt.is_static) {
                            push_error_("gOIR M1 does not support static local bindings.");
                            return;
                        }
                        const TypeId binding_ty =
                            (stmt.declared_type != parus::sir::k_invalid_type) ? stmt.declared_type :
                            ((stmt.init != parus::sir::k_invalid_value) ? sir_.values[stmt.init].type : kInvalidType);
                        if (!is_supported_local_type_(binding_ty)) {
                            push_error_("gOIR M1 local binding type is outside the supported memory/aggregate subset.");
                            return;
                        }
                        if (stmt.init == parus::sir::k_invalid_value) {
                            push_error_("gOIR M1 requires initialized local bindings.");
                            return;
                        }
                        const auto init = lower_value_as_(state, stmt.init, binding_ty);
                        bind_local_(state, stmt, binding_ty, init);
                        return;
                    }
                    case StmtKind::kIfStmt:
                        lower_if_stmt_(state, stmt);
                        return;
                    case StmtKind::kWhileStmt:
                        lower_while_stmt_(state, stmt);
                        return;
                    case StmtKind::kSwitch:
                        lower_switch_stmt_(state, stmt);
                        return;
                    case StmtKind::kReturn: {
                        if (stmt.expr == parus::sir::k_invalid_value) {
                            ensure_block_term_(state, TermRet{});
                        } else {
                            const auto value = lower_value_as_(state, stmt.expr, state.return_type);
                            ensure_block_term_(state, TermRet{
                                .has_value = true,
                                .value = value,
                            });
                        }
                        state.current_block = kInvalidId;
                        return;
                    }
                    default:
                        push_error_("gOIR M1 builder encountered unsupported SIR stmt kind.");
                        return;
                }
            }

            void lower_func_(uint32_t func_index, const parus::sir::Func& fn) {
                FuncState state{};
                state.realization = realization_by_func_index_[func_index];
                state.current_block = mod_.add_block(Block{});
                state.return_type = fn.ret;

                auto& real = mod_.realizations[state.realization];
                real.entry = state.current_block;
                real.blocks.push_back(state.current_block);

                for (uint32_t i = 0; i < fn.param_count; ++i) {
                    const auto& param = sir_.params[fn.param_begin + i];
                    const auto vid = add_block_param_(state.current_block, param.type);
                    state.locals[param.sym] = Binding{
                        .value = vid,
                        .type = param.type,
                        .layout = classify_layout_(param.type),
                        .is_place = false,
                        .is_mutable = false,
                    };
                }

                lower_block_(state, fn.entry);

                if (state.current_block != kInvalidId &&
                    !mod_.blocks[state.current_block].has_term) {
                    if (is_unit_type_(types_, fn.ret)) {
                        ensure_block_term_(state, TermRet{});
                    } else {
                        push_error_("gOIR M1 function '" + std::string(fn.name) + "' falls off the end without return.");
                    }
                }
            }
        };

    } // namespace

    BuildResult build_from_sir(
        const parus::sir::Module& sir,
        const parus::ty::TypePool& types
    ) {
        Builder builder(sir, types);
        return builder.build();
    }

} // namespace parus::goir
