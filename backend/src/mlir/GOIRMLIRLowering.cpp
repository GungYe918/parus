#include <parus/backend/mlir/GOIRMLIRLowering.hpp>

#include <parus/backend/aot/LLVMIRLowering.hpp>
#include <parus/goir/Verify.hpp>

#include <mlir/Conversion/Passes.h>
#include <mlir/Dialect/Arith/IR/Arith.h>
#include <mlir/Dialect/ControlFlow/IR/ControlFlow.h>
#include <mlir/Dialect/Func/IR/FuncOps.h>
#include <mlir/Dialect/Index/IR/IndexDialect.h>
#include <mlir/Dialect/LLVMIR/LLVMDialect.h>
#include <mlir/Dialect/MemRef/IR/MemRef.h>
#include <mlir/Dialect/MemRef/Transforms/Passes.h>
#include <mlir/IR/BuiltinOps.h>
#include <mlir/IR/DialectRegistry.h>
#include <mlir/Parser/Parser.h>
#include <mlir/Pass/PassManager.h>
#include <mlir/Target/LLVMIR/Dialect/Builtin/BuiltinToLLVMIRTranslation.h>
#include <mlir/Target/LLVMIR/Dialect/LLVMIR/LLVMToLLVMIRTranslation.h>
#include <mlir/Target/LLVMIR/Export.h>

#include <llvm/IR/LLVMContext.h>
#include <llvm/Support/raw_ostream.h>

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#ifndef PARUS_MLIR_SELECTED_MAJOR
#define PARUS_MLIR_SELECTED_MAJOR 22
#endif

namespace parus::backend::mlir {

    namespace {

        void push_error_(std::vector<CompileMessage>& out, std::string text) {
            out.push_back(CompileMessage{
                .is_error = true,
                .text = std::move(text),
            });
        }

        bool is_builtin_(const parus::ty::TypePool& types, parus::ty::TypeId ty, parus::ty::Builtin builtin) {
            if (ty == parus::ty::kInvalidType) return false;
            const auto& t = types.get(ty);
            return t.kind == parus::ty::Kind::kBuiltin && t.builtin == builtin;
        }

        bool is_integer_builtin_(parus::ty::Builtin builtin) {
            using B = parus::ty::Builtin;
            switch (builtin) {
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

        bool is_signed_builtin_(parus::ty::Builtin builtin) {
            using B = parus::ty::Builtin;
            switch (builtin) {
                case B::kChar:
                case B::kI8:
                case B::kI16:
                case B::kI32:
                case B::kI64:
                case B::kISize:
                    return true;
                default:
                    return false;
            }
        }

        bool is_float_builtin_(parus::ty::Builtin builtin) {
            return builtin == parus::ty::Builtin::kF32 || builtin == parus::ty::Builtin::kF64;
        }

        std::optional<parus::ty::Builtin> builtin_of_(const parus::ty::TypePool& types, parus::ty::TypeId ty) {
            if (ty == parus::ty::kInvalidType) return std::nullopt;
            const auto& t = types.get(ty);
            if (t.kind != parus::ty::Kind::kBuiltin) return std::nullopt;
            return t.builtin;
        }

        std::optional<std::string> mlir_scalar_type_(const parus::ty::TypePool& types, parus::ty::TypeId ty) {
            const auto builtin = builtin_of_(types, ty);
            if (!builtin.has_value()) return std::nullopt;
            using B = parus::ty::Builtin;
            switch (*builtin) {
                case B::kUnit: return std::string("()");
                case B::kBool: return std::string("i1");
                case B::kChar: return std::string("i32");
                case B::kI8:
                case B::kU8: return std::string("i8");
                case B::kI16:
                case B::kU16: return std::string("i16");
                case B::kI32:
                case B::kU32: return std::string("i32");
                case B::kI64:
                case B::kU64:
                case B::kISize:
                case B::kUSize: return std::string("i64");
                case B::kF32: return std::string("f32");
                case B::kF64: return std::string("f64");
                default: return std::nullopt;
            }
        }

        std::optional<std::string> memref_slot_type_(const parus::ty::TypePool& types, parus::ty::TypeId ty) {
            const auto scalar = mlir_scalar_type_(types, ty);
            if (scalar.has_value()) return "memref<" + *scalar + ">";
            if (ty == parus::ty::kInvalidType) return std::nullopt;
            const auto& t = types.get(ty);
            if (t.kind != parus::ty::Kind::kArray || !t.array_has_size) return std::nullopt;
            const auto elem = mlir_scalar_type_(types, t.elem);
            if (!elem.has_value()) return std::nullopt;
            return "memref<" + std::to_string(t.array_size) + "x" + *elem + ">";
        }

        std::optional<std::string> slice_memref_type_(const parus::ty::TypePool& types, parus::ty::TypeId ty) {
            if (ty == parus::ty::kInvalidType) return std::nullopt;
            const auto& t = types.get(ty);
            if (t.kind != parus::ty::Kind::kArray || t.array_has_size) return std::nullopt;
            const auto elem = mlir_scalar_type_(types, t.elem);
            if (!elem.has_value()) return std::nullopt;
            return "memref<?x" + *elem + ", strided<[1], offset: ?>>";
        }

        std::string sanitize_int_literal_(std::string_view text) {
            std::string out{};
            out.reserve(text.size());
            size_t i = 0;
            if (!text.empty() && (text[0] == '+' || text[0] == '-')) {
                out.push_back(text[0]);
                i = 1;
            }
            for (; i < text.size(); ++i) {
                const char ch = text[i];
                if (ch >= '0' && ch <= '9') out.push_back(ch);
                else if (ch == '_') continue;
                else break;
            }
            return out.empty() ? std::string("0") : out;
        }

        std::string sanitize_float_literal_(std::string_view text) {
            std::string out{};
            out.reserve(text.size());
            bool in_exp = false;
            size_t i = 0;
            if (!text.empty() && (text[0] == '+' || text[0] == '-')) {
                out.push_back(text[0]);
                i = 1;
            }
            for (; i < text.size(); ++i) {
                const char ch = text[i];
                if ((ch >= '0' && ch <= '9') || ch == '.') {
                    out.push_back(ch);
                    continue;
                }
                if (ch == '_') continue;
                if ((ch == 'e' || ch == 'E') && !in_exp) {
                    out.push_back(ch);
                    in_exp = true;
                    continue;
                }
                if ((ch == '+' || ch == '-') && in_exp &&
                    !out.empty() && (out.back() == 'e' || out.back() == 'E')) {
                    out.push_back(ch);
                    continue;
                }
                break;
            }
            return out.empty() ? std::string("0.0") : out;
        }

        std::optional<uint32_t> parse_char_literal_code_(std::string_view text) {
            auto is_hex_digit_local = [](char c) {
                return (c >= '0' && c <= '9') ||
                       (c >= 'a' && c <= 'f') ||
                       (c >= 'A' && c <= 'F');
            };
            auto hex_digit_value_local = [](char c) -> uint8_t {
                if (c >= '0' && c <= '9') return static_cast<uint8_t>(c - '0');
                if (c >= 'a' && c <= 'f') return static_cast<uint8_t>(10 + (c - 'a'));
                if (c >= 'A' && c <= 'F') return static_cast<uint8_t>(10 + (c - 'A'));
                return 0;
            };
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
                is_hex_digit_local(body[2]) && is_hex_digit_local(body[3])) {
                const uint8_t hi = hex_digit_value_local(body[2]);
                const uint8_t lo = hex_digit_value_local(body[3]);
                return static_cast<uint32_t>((hi << 4) | lo);
            }
            return std::nullopt;
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

        std::string decode_escaped_string_body_(std::string_view body) {
            std::string out{};
            out.reserve(body.size());

            for (size_t i = 0; i < body.size(); ++i) {
                const char c = body[i];
                if (c != '\\') {
                    out.push_back(c);
                    continue;
                }
                if (i + 1 >= body.size()) {
                    out.push_back('\\');
                    break;
                }

                const char esc = body[++i];
                switch (esc) {
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case '\\': out.push_back('\\'); break;
                    case '"': out.push_back('"'); break;
                    case '\'': out.push_back('\''); break;
                    case '0': out.push_back('\0'); break;
                    case 'x': {
                        if (i + 2 < body.size() &&
                            is_hex_digit_(body[i + 1]) &&
                            is_hex_digit_(body[i + 2])) {
                            const uint8_t hi = hex_digit_value_(body[i + 1]);
                            const uint8_t lo = hex_digit_value_(body[i + 2]);
                            out.push_back(static_cast<char>((hi << 4) | lo));
                            i += 2;
                            break;
                        }
                        out.push_back('x');
                        break;
                    }
                    default:
                        out.push_back(esc);
                        break;
                }
            }
            return out;
        }

        std::string parse_string_literal_bytes_(std::string_view text) {
            if (starts_with_(text, "c\"") && text.size() >= 3 && text.back() == '"') {
                return decode_escaped_string_body_(text.substr(2, text.size() - 3));
            }
            if (starts_with_(text, "cr\"") && text.size() >= 4 && text.back() == '"') {
                return std::string(text.substr(3, text.size() - 4));
            }
            if (text.size() >= 2 && text.front() == '"' && text.back() == '"') {
                return decode_escaped_string_body_(text.substr(1, text.size() - 2));
            }
            if (starts_with_(text, "$\"") && text.size() >= 3 && text.back() == '"') {
                return decode_escaped_string_body_(text.substr(2, text.size() - 3));
            }
            if (starts_with_(text, "R\"\"\"") && ends_with_(text, "\"\"\"") && text.size() >= 7) {
                return std::string(text.substr(4, text.size() - 7));
            }
            if (starts_with_(text, "F\"\"\"") && ends_with_(text, "\"\"\"") && text.size() >= 7) {
                return std::string(text.substr(4, text.size() - 7));
            }
            return std::string(text);
        }

        std::string escape_mlir_string_bytes_(std::string_view bytes) {
            static constexpr char kHex[] = "0123456789ABCDEF";
            std::string out{};
            out.reserve(bytes.size() * 2);
            for (const unsigned char c : bytes) {
                switch (c) {
                    case '\\': out += "\\\\"; break;
                    case '"': out += "\\\""; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    case '\0': out += "\\00"; break;
                    default:
                        if (std::isprint(c) != 0) {
                            out.push_back(static_cast<char>(c));
                        } else {
                            out.push_back('\\');
                            out.push_back(kHex[(c >> 4) & 0xF]);
                            out.push_back(kHex[c & 0xF]);
                        }
                        break;
                }
            }
            return out;
        }

        class MlirEmitter {
        public:
            MlirEmitter(const parus::goir::Module& module, const parus::ty::TypePool& types)
                : module_(module),
                  types_(types),
                  value_names_(module.values.size()),
                  place_infos_(module.values.size()) {}

            GOIRLoweringResult emit() {
                GOIRLoweringResult out{};
                if (module_.header.stage_kind != parus::goir::StageKind::Placed) {
                    push_error_(out.messages, "MLIR lowering expects a gOIR-placed module.");
                    return out;
                }
                const auto verrs = parus::goir::verify(module_);
                for (const auto& err : verrs) push_error_(out.messages, err.msg);
                if (!out.messages.empty()) return out;

                std::string mlir_text{};
                llvm::raw_string_ostream os(mlir_text);
                os << "module {\n";
                collect_text_globals_();
                emit_text_globals_(os);
                emit_globals_(os);
                for (size_t i = 0; i < module_.realizations.size(); ++i) {
                    const auto& real = module_.realizations[i];
                    if (real.family != parus::goir::FamilyKind::Cpu &&
                        real.family != parus::goir::FamilyKind::Core) {
                        push_error_(out.messages, "MLIR lowering only supports CPU/core placed realizations in M1.");
                        continue;
                    }
                    if (failed_) break;
                    emit_realization_(os, real);
                }
                os << "}\n";
                os.flush();

                auto extra = take_messages();
                out.messages.insert(out.messages.end(), extra.begin(), extra.end());
                if (!out.messages.empty()) return out;
                out.ok = true;
                out.mlir_text = std::move(mlir_text);
                return out;
            }

            std::vector<CompileMessage> take_messages() { return std::move(messages_); }

        private:
            struct TextGlobalInfo {
                std::string symbol{};
                std::string escaped_bytes{};
                size_t byte_count = 0;
                bool uses_sentinel_byte = false;
            };

            struct PlaceInfo {
                enum class Kind : uint8_t {
                    None = 0,
                    MemRef,
                    LlvmPtr,
                };

                Kind kind = Kind::None;
                std::string ref{};
                std::string container_type{};
                std::string element_type{};
                parus::goir::LayoutClass layout = parus::goir::LayoutClass::Unknown;
                parus::ty::TypeId storage_type = parus::ty::kInvalidType;
                bool host_abi_bytes = false;
                std::vector<std::string> indices{};
            };

            const parus::goir::Module& module_;
            const parus::ty::TypePool& types_;
            std::vector<std::string> value_names_;
            std::vector<std::optional<PlaceInfo>> place_infos_;
            uint32_t temp_counter_ = 0;
            uint32_t text_global_counter_ = 0;
            bool failed_ = false;
            std::vector<CompileMessage> messages_{};
            std::unordered_map<parus::goir::ValueId, TextGlobalInfo> text_globals_{};

            void fail_(std::string text) {
                failed_ = true;
                messages_.push_back(CompileMessage{.is_error = true, .text = std::move(text)});
            }

            std::string value_name_(parus::goir::ValueId id) {
                if (id == parus::goir::kInvalidId || static_cast<size_t>(id) >= value_names_.size()) return "%invalid";
                if (!value_names_[id].empty()) return value_names_[id];
                value_names_[id] = "%v" + std::to_string(id);
                return value_names_[id];
            }

            std::string make_temp_() {
                return "%tmp" + std::to_string(temp_counter_++);
            }

            std::string make_block_arg_(parus::goir::BlockId block_id, size_t index) {
                return "%bb" + std::to_string(block_id) + "_arg" + std::to_string(index);
            }

            bool is_unit_(parus::ty::TypeId ty) const {
                return is_builtin_(types_, ty, parus::ty::Builtin::kUnit);
            }

            bool is_integer_(parus::ty::TypeId ty) const {
                const auto builtin = builtin_of_(types_, ty);
                return builtin.has_value() && is_integer_builtin_(*builtin);
            }

            bool is_float_(parus::ty::TypeId ty) const {
                const auto builtin = builtin_of_(types_, ty);
                return builtin.has_value() && is_float_builtin_(*builtin);
            }

            bool is_signed_(parus::ty::TypeId ty) const {
                const auto builtin = builtin_of_(types_, ty);
                return builtin.has_value() && is_signed_builtin_(*builtin);
            }

            const parus::goir::RecordLayout* record_layout_(parus::ty::TypeId ty) const {
                return module_.find_record_layout(ty);
            }

            bool is_layout_c_record_(parus::ty::TypeId ty) const {
                const auto* layout = record_layout_(ty);
                return layout != nullptr && layout->layout == parus::sir::FieldLayout::kC;
            }

            bool is_text_(parus::ty::TypeId ty) const {
                return is_builtin_(types_, ty, parus::ty::Builtin::kText);
            }

            bool is_optional_(parus::ty::TypeId ty) const {
                if (ty == parus::ty::kInvalidType) return false;
                return types_.get(ty).kind == parus::ty::Kind::kOptional;
            }

            parus::ty::TypeId optional_elem_(parus::ty::TypeId ty) const {
                return is_optional_(ty) ? types_.get(ty).elem : parus::ty::kInvalidType;
            }

            std::optional<parus::ty::TypeId> tag_enum_storage_type_(parus::ty::TypeId ty) const {
                const auto* layout = record_layout_(ty);
                if (layout == nullptr || layout->fields.size() != 1) return std::nullopt;
                if (module_.string(layout->fields[0].name) != "__tag") return std::nullopt;
                return layout->fields[0].type;
            }

            bool is_tag_enum_(parus::ty::TypeId ty) const {
                return tag_enum_storage_type_(ty).has_value();
            }

            std::optional<uint32_t> c_abi_align_(parus::ty::TypeId ty) const {
                if (ty == parus::ty::kInvalidType) return std::nullopt;
                if (const auto builtin = builtin_of_(types_, ty); builtin.has_value()) {
                    using B = parus::ty::Builtin;
                    switch (*builtin) {
                        case B::kBool:
                        case B::kI8:
                        case B::kU8:
                            return uint32_t{1};
                        case B::kI16:
                        case B::kU16:
                            return uint32_t{2};
                        case B::kChar:
                        case B::kI32:
                        case B::kU32:
                        case B::kF32:
                            return uint32_t{4};
                        case B::kI64:
                        case B::kU64:
                        case B::kISize:
                        case B::kUSize:
                        case B::kF64:
                            return uint32_t{8};
                        case B::kUnit:
                            return uint32_t{1};
                        default:
                            return std::nullopt;
                    }
                }
                if (const auto tag_ty = tag_enum_storage_type_(ty); tag_ty.has_value()) {
                    return c_abi_align_(*tag_ty);
                }
                const auto* layout = record_layout_(ty);
                if (layout == nullptr || layout->layout != parus::sir::FieldLayout::kC) return std::nullopt;
                uint32_t max_align = 1;
                for (const auto& field : layout->fields) {
                    const auto field_align = c_abi_align_(field.type);
                    if (!field_align.has_value()) return std::nullopt;
                    max_align = std::max(max_align, *field_align);
                }
                if (layout->align != 0) max_align = std::max(max_align, layout->align);
                return max_align;
            }

            std::optional<size_t> c_abi_size_(parus::ty::TypeId ty) const {
                if (ty == parus::ty::kInvalidType) return std::nullopt;
                if (const auto builtin = builtin_of_(types_, ty); builtin.has_value()) {
                    using B = parus::ty::Builtin;
                    switch (*builtin) {
                        case B::kBool:
                        case B::kI8:
                        case B::kU8:
                            return size_t{1};
                        case B::kI16:
                        case B::kU16:
                            return size_t{2};
                        case B::kChar:
                        case B::kI32:
                        case B::kU32:
                        case B::kF32:
                            return size_t{4};
                        case B::kI64:
                        case B::kU64:
                        case B::kISize:
                        case B::kUSize:
                        case B::kF64:
                            return size_t{8};
                        case B::kUnit:
                            return size_t{0};
                        default:
                            return std::nullopt;
                    }
                }
                if (const auto tag_ty = tag_enum_storage_type_(ty); tag_ty.has_value()) {
                    return c_abi_size_(*tag_ty);
                }
                const auto* layout = record_layout_(ty);
                if (layout == nullptr || layout->layout != parus::sir::FieldLayout::kC) return std::nullopt;
                size_t size = 0;
                uint32_t struct_align = 1;
                for (const auto& field : layout->fields) {
                    const auto field_align = c_abi_align_(field.type);
                    const auto field_size = c_abi_size_(field.type);
                    if (!field_align.has_value() || !field_size.has_value()) return std::nullopt;
                    struct_align = std::max(struct_align, *field_align);
                    const size_t rem = size % *field_align;
                    if (rem != 0) size += (*field_align - rem);
                    size += *field_size;
                }
                if (layout->align != 0) struct_align = std::max(struct_align, layout->align);
                const size_t rem = size % struct_align;
                if (rem != 0) size += (struct_align - rem);
                return size;
            }

            std::optional<std::pair<size_t, parus::ty::TypeId>> c_abi_field_offset_(parus::ty::TypeId record_ty,
                                                                                      parus::goir::StringId field_name) const {
                const auto* layout = record_layout_(record_ty);
                if (layout == nullptr || layout->layout != parus::sir::FieldLayout::kC) return std::nullopt;
                size_t offset = 0;
                for (const auto& field : layout->fields) {
                    const auto field_align = c_abi_align_(field.type);
                    const auto field_size = c_abi_size_(field.type);
                    if (!field_align.has_value() || !field_size.has_value()) return std::nullopt;
                    const size_t rem = offset % *field_align;
                    if (rem != 0) offset += (*field_align - rem);
                    if (field.name == field_name) {
                        return std::make_pair(offset, field.type);
                    }
                    offset += *field_size;
                }
                return std::nullopt;
            }

            std::optional<std::string> scalar_like_type_(parus::ty::TypeId ty) const {
                if (const auto scalar = mlir_scalar_type_(types_, ty); scalar.has_value()) {
                    return scalar;
                }
                if (const auto tag_ty = tag_enum_storage_type_(ty); tag_ty.has_value()) {
                    return mlir_scalar_type_(types_, *tag_ty);
                }
                return std::nullopt;
            }

            parus::goir::LayoutClass classify_layout_(parus::ty::TypeId ty) const {
                if (ty == parus::ty::kInvalidType) return parus::goir::LayoutClass::Unknown;
                const auto& t = types_.get(ty);
                switch (t.kind) {
                    case parus::ty::Kind::kBuiltin:
                        if (t.builtin == parus::ty::Builtin::kText) return parus::goir::LayoutClass::TextView;
                        return mlir_scalar_type_(types_, ty).has_value()
                            ? parus::goir::LayoutClass::Scalar
                            : parus::goir::LayoutClass::Unknown;
                    case parus::ty::Kind::kOptional:
                        return parus::goir::LayoutClass::OptionalScalar;
                    case parus::ty::Kind::kArray:
                        return t.array_has_size ? parus::goir::LayoutClass::FixedArray
                                                : parus::goir::LayoutClass::SliceView;
                    case parus::ty::Kind::kNamedUser:
                        if (is_tag_enum_(ty)) return parus::goir::LayoutClass::TagEnum;
                        return record_layout_(ty) != nullptr ? parus::goir::LayoutClass::PlainRecord
                                                             : parus::goir::LayoutClass::Unknown;
                    default:
                        return parus::goir::LayoutClass::Unknown;
                }
            }

            bool is_integer_like_(parus::ty::TypeId ty) const {
                if (const auto builtin = builtin_of_(types_, ty); builtin.has_value()) {
                    return is_integer_builtin_(*builtin);
                }
                if (const auto tag_ty = tag_enum_storage_type_(ty); tag_ty.has_value()) {
                    const auto builtin = builtin_of_(types_, *tag_ty);
                    return builtin.has_value() && is_integer_builtin_(*builtin);
                }
                return false;
            }

            bool is_signed_like_(parus::ty::TypeId ty) const {
                if (const auto builtin = builtin_of_(types_, ty); builtin.has_value()) {
                    return is_signed_builtin_(*builtin);
                }
                if (const auto tag_ty = tag_enum_storage_type_(ty); tag_ty.has_value()) {
                    const auto builtin = builtin_of_(types_, *tag_ty);
                    return builtin.has_value() && is_signed_builtin_(*builtin);
                }
                return false;
            }

            std::string llvm_text_view_type_() const {
                return "!llvm.struct<(!llvm.ptr, i64)>";
            }

            std::optional<std::string> mlir_value_type_(parus::ty::TypeId ty) const {
                if (ty == parus::ty::kInvalidType) return std::nullopt;
                if (is_unit_(ty)) return std::string("()");
                if (const auto scalar = scalar_like_type_(ty); scalar.has_value()) {
                    return scalar;
                }
                return llvm_type_(ty);
            }

            std::optional<std::string> llvm_type_(parus::ty::TypeId ty) const {
                if (const auto scalar = scalar_like_type_(ty); scalar.has_value()) {
                    return scalar;
                }
                if (ty == parus::ty::kInvalidType) return std::nullopt;
                const auto& t = types_.get(ty);
                if (t.kind == parus::ty::Kind::kBuiltin && t.builtin == parus::ty::Builtin::kText) {
                    return llvm_text_view_type_();
                }
                if (t.kind == parus::ty::Kind::kOptional) {
                    const auto payload_ty = llvm_type_(t.elem);
                    if (!payload_ty.has_value()) return std::nullopt;
                    return "!llvm.struct<(i1, " + *payload_ty + ")>";
                }
                if (t.kind == parus::ty::Kind::kArray && t.array_has_size) {
                    const auto elem = llvm_type_(t.elem);
                    if (!elem.has_value()) return std::nullopt;
                    return "!llvm.array<" + std::to_string(t.array_size) + " x " + *elem + ">";
                }
                if (const auto* layout = record_layout_(ty); layout != nullptr) {
                    if (layout->layout == parus::sir::FieldLayout::kC) {
                        const auto size = c_abi_size_(ty);
                        if (!size.has_value()) return std::nullopt;
                        return "!llvm.array<" + std::to_string(*size) + " x i8>";
                    }
                    std::string out = "!llvm.struct<(";
                    for (size_t i = 0; i < layout->fields.size(); ++i) {
                        if (i != 0) out += ", ";
                        const auto field_ty = llvm_type_(layout->fields[i].type);
                        if (!field_ty.has_value()) return std::nullopt;
                        out += *field_ty;
                    }
                    out += ")>";
                    return out;
                }
                return std::nullopt;
            }

            void collect_text_globals_() {
                for (size_t iid = 0; iid < module_.insts.size(); ++iid) {
                    const auto& inst = module_.insts[iid];
                    if (inst.result == parus::goir::kInvalidId) continue;
                    const auto* text = std::get_if<parus::goir::OpTextLit>(&inst.data);
                    if (text == nullptr) continue;

                    auto bytes = parse_string_literal_bytes_(text->quoted_text);
                    bool uses_sentinel = false;
                    if (bytes.empty()) {
                        bytes.push_back('\0');
                        uses_sentinel = true;
                    }
                    text_globals_[inst.result] = TextGlobalInfo{
                        .symbol = "__parus_text_" + std::to_string(text_global_counter_++),
                        .escaped_bytes = escape_mlir_string_bytes_(bytes),
                        .byte_count = uses_sentinel ? size_t{0} : bytes.size(),
                        .uses_sentinel_byte = uses_sentinel,
                    };
                }
            }

            void emit_text_globals_(llvm::raw_ostream& os) {
                for (const auto& [_, global] : text_globals_) {
                    const size_t storage_len = global.uses_sentinel_byte ? size_t{1} : global.byte_count;
                    os << "  llvm.mlir.global internal constant @" << global.symbol
                       << "(\"" << global.escaped_bytes << "\") : !llvm.array<"
                       << storage_len << " x i8>\n";
                }
            }

            std::string global_symbol_(const parus::goir::GGlobal& global) const {
                if (global.link_name != parus::goir::kInvalidId) {
                    return std::string(module_.string(global.link_name));
                }
                return std::string(module_.string(global.name));
            }

            void emit_zero_global_(llvm::raw_ostream& os,
                                   std::string_view symbol,
                                   std::string_view storage_ty,
                                   bool constant) {
                os << "  llvm.mlir.global internal ";
                if (constant) os << "constant ";
                os << "@" << symbol << "() : " << storage_ty << " {\n";
                os << "    %0 = llvm.mlir.zero : " << storage_ty << "\n";
                os << "    llvm.return %0 : " << storage_ty << "\n";
                os << "  }\n";
            }

            void emit_const_global_(llvm::raw_ostream& os, const parus::goir::GGlobal& global) {
                const auto storage_ty = llvm_type_(global.type);
                if (!storage_ty.has_value()) {
                    fail_("constant global lowering requires an LLVM-compatible storage type");
                    return;
                }

                if (global.folded_const_init.kind == parus::sir::ConstInitKind::kNone) {
                    fail_("constant global lowering currently requires folded scalar init metadata");
                    return;
                }

                os << "  llvm.mlir.global internal constant @" << global_symbol_(global)
                   << "() : " << *storage_ty << " {\n";
                switch (global.folded_const_init.kind) {
                    case parus::sir::ConstInitKind::kInt:
                        os << "    %0 = llvm.mlir.constant(" << sanitize_int_literal_(global.folded_const_init.text)
                           << " : " << *storage_ty << ") : " << *storage_ty << "\n";
                        break;
                    case parus::sir::ConstInitKind::kFloat:
                        os << "    %0 = llvm.mlir.constant(" << sanitize_float_literal_(global.folded_const_init.text)
                           << " : " << *storage_ty << ") : " << *storage_ty << "\n";
                        break;
                    case parus::sir::ConstInitKind::kBool:
                        os << "    %0 = llvm.mlir.constant(" << ((global.folded_const_init.text == "true" || global.folded_const_init.text == "1") ? "1" : "0")
                           << " : " << *storage_ty << ") : " << *storage_ty << "\n";
                        break;
                    case parus::sir::ConstInitKind::kChar: {
                        const auto ch = parse_char_literal_code_(global.folded_const_init.text);
                        if (!ch.has_value()) {
                            fail_("constant global lowering could not decode char literal");
                            return;
                        }
                        os << "    %0 = llvm.mlir.constant(" << *ch
                           << " : " << *storage_ty << ") : " << *storage_ty << "\n";
                        break;
                    }
                    case parus::sir::ConstInitKind::kNone:
                    default:
                        fail_("unsupported folded constant global kind");
                        return;
                }
                os << "    llvm.return %0 : " << *storage_ty << "\n";
                os << "  }\n";
            }

            void emit_globals_(llvm::raw_ostream& os) {
                for (const auto& global : module_.globals) {
                    const auto storage_ty = llvm_type_(global.type);
                    if (!storage_ty.has_value()) {
                        fail_("global lowering requires an LLVM-compatible storage type");
                        return;
                    }

                    if (global.linkage == parus::goir::LinkageKind::External) {
                        os << "  llvm.mlir.global external @" << global_symbol_(global)
                           << "() : " << *storage_ty << "\n";
                        continue;
                    }

                    if (global.is_const) {
                        emit_const_global_(os, global);
                        if (failed_) return;
                        continue;
                    }

                    emit_zero_global_(os, global_symbol_(global), *storage_ty, /*constant=*/false);
                    if (failed_) return;
                }
            }

            const parus::goir::Inst* producer_inst_(parus::goir::ValueId id) const {
                if (id == parus::goir::kInvalidId || static_cast<size_t>(id) >= module_.values.size()) return nullptr;
                const auto& value = module_.values[id];
                if (value.def_b != parus::goir::kInvalidId) return nullptr;
                if (value.def_a == parus::goir::kInvalidId || static_cast<size_t>(value.def_a) >= module_.insts.size()) {
                    return nullptr;
                }
                const auto& inst = module_.insts[value.def_a];
                return inst.result == id ? &inst : nullptr;
            }

            template <typename T>
            const T* producer_as_(parus::goir::ValueId id) const {
                const auto* inst = producer_inst_(id);
                if (inst == nullptr) return nullptr;
                return std::get_if<T>(&inst->data);
            }

            std::optional<std::pair<size_t, parus::ty::TypeId>> record_field_index_(parus::ty::TypeId record_ty,
                                                                                     parus::goir::StringId field_name) const {
                const auto* layout = record_layout_(record_ty);
                if (layout == nullptr) return std::nullopt;
                for (size_t i = 0; i < layout->fields.size(); ++i) {
                    if (layout->fields[i].name == field_name) {
                        return std::make_pair(i, layout->fields[i].type);
                    }
                }
                return std::nullopt;
            }

            std::string const_index_(llvm::raw_ostream& os, std::string_view value) {
                const auto name = make_temp_();
                os << "    " << name << " = arith.constant " << value << " : index\n";
                return name;
            }

            std::string const_i64_(llvm::raw_ostream& os, int64_t value) {
                const auto name = make_temp_();
                os << "    " << name << " = arith.constant " << value << " : i64\n";
                return name;
            }

            std::string llvm_const_i64_(llvm::raw_ostream& os, int64_t value) {
                const auto name = make_temp_();
                os << "    " << name << " = llvm.mlir.constant(" << value << " : i64) : i64\n";
                return name;
            }

            std::string const_zero_(llvm::raw_ostream& os, parus::ty::TypeId ty) {
                const auto mlir_ty = scalar_like_type_(ty);
                const auto name = make_temp_();
                if (is_float_(ty)) {
                    os << "    " << name << " = arith.constant 0.0 : " << *mlir_ty << "\n";
                } else {
                    os << "    " << name << " = arith.constant 0 : " << *mlir_ty << "\n";
                }
                return name;
            }

            std::string const_all_ones_(llvm::raw_ostream& os, parus::ty::TypeId ty) {
                const auto mlir_ty = scalar_like_type_(ty);
                const auto name = make_temp_();
                os << "    " << name << " = arith.constant -1 : " << *mlir_ty << "\n";
                return name;
            }

            std::string const_bool_(llvm::raw_ostream& os, bool value) {
                const auto name = make_temp_();
                os << "    " << name << " = arith.constant " << (value ? "true" : "false") << "\n";
                return name;
            }

            std::optional<PlaceInfo> place_info_(parus::goir::ValueId id) const {
                if (id == parus::goir::kInvalidId || static_cast<size_t>(id) >= place_infos_.size()) return std::nullopt;
                return place_infos_[id];
            }

            void set_place_info_(parus::goir::ValueId id, PlaceInfo info) {
                if (id == parus::goir::kInvalidId || static_cast<size_t>(id) >= place_infos_.size()) return;
                place_infos_[id] = std::move(info);
            }

            std::string ensure_index_(llvm::raw_ostream& os, parus::goir::ValueId value) {
                if (value == parus::goir::kInvalidId || static_cast<size_t>(value) >= module_.values.size()) {
                    fail_("expected a valid scalar value for index conversion");
                    return "%invalid";
                }
                const auto& gv = module_.values[value];
                const auto name = value_name_(value);
                if (gv.ty == parus::ty::kInvalidType) {
                    fail_("index conversion saw invalid type");
                    return name;
                }
                const auto ty = scalar_like_type_(gv.ty);
                if (!ty.has_value()) {
                    fail_("index conversion only supports scalar integer values");
                    return name;
                }
                if (*ty == "index") return name;
                const auto casted = make_temp_();
                os << "    " << casted << " = arith.index_cast " << name
                   << " : " << *ty << " to index\n";
                return casted;
            }

            std::string cast_index_to_result_(llvm::raw_ostream& os,
                                              std::string_view index_value,
                                              parus::ty::TypeId result_ty) {
                const auto result_mlir_ty = scalar_like_type_(result_ty);
                if (!result_mlir_ty.has_value()) {
                    fail_("array.len result type must be a scalar integer");
                    return "%invalid";
                }
                if (*result_mlir_ty == "index") return std::string(index_value);
                const auto casted = make_temp_();
                os << "    " << casted << " = arith.index_cast " << index_value
                   << " : index to " << *result_mlir_ty << "\n";
                return casted;
            }

            std::optional<PlaceInfo> materialize_array_value_to_memref_(llvm::raw_ostream& os,
                                                                        parus::goir::ValueId value) {
                const auto* make = producer_as_<parus::goir::OpArrayMake>(value);
                if (make == nullptr) {
                    fail_("MLIR lowering only supports array values that are still represented as array.make in M1.");
                    return std::nullopt;
                }

                const auto memref_ty = memref_slot_type_(types_, module_.values[value].ty);
                if (!memref_ty.has_value()) {
                    fail_("array.make materialization requires a fixed-size scalar array type.");
                    return std::nullopt;
                }
                const auto& arr_ty = types_.get(module_.values[value].ty);
                const auto elem_ty = scalar_like_type_(arr_ty.elem);
                if (!elem_ty.has_value()) {
                    fail_("array.make materialization requires scalar element types.");
                    return std::nullopt;
                }

                const auto slot = make_temp_();
                os << "    " << slot << " = memref.alloca() : " << *memref_ty << "\n";
                for (size_t i = 0; i < make->elems.size(); ++i) {
                    const auto idx = const_index_(os, std::to_string(i));
                    os << "    memref.store " << value_name_(make->elems[i]) << ", " << slot
                       << "[" << idx << "] : " << *memref_ty << "\n";
                }

                return PlaceInfo{
                    .kind = PlaceInfo::Kind::MemRef,
                    .ref = slot,
                    .container_type = *memref_ty,
                    .element_type = *elem_ty,
                    .layout = parus::goir::LayoutClass::FixedArray,
                    .indices = {},
                };
            }

            std::optional<PlaceInfo> record_field_place_(llvm::raw_ostream& os,
                                                         const PlaceInfo& base,
                                                         parus::ty::TypeId record_ty,
                                                         parus::goir::StringId field_name,
                                                         parus::goir::LayoutClass field_layout) {
                const auto field_info = record_field_index_(record_ty, field_name);
                if (!field_info.has_value()) {
                    fail_("field.place requires a known frozen record field.");
                    return std::nullopt;
                }
                const auto field_ty = llvm_type_(field_info->second);
                if (!field_ty.has_value()) {
                    fail_("field.place requires an LLVM-compatible field type.");
                    return std::nullopt;
                }

                if (base.host_abi_bytes || is_layout_c_record_(record_ty)) {
                    const auto offset = c_abi_field_offset_(record_ty, field_name);
                    const auto storage_ty = llvm_type_(record_ty);
                    if (!offset.has_value() || !storage_ty.has_value()) {
                        fail_("layout(c) field.place requires ABI size/offset metadata.");
                        return std::nullopt;
                    }
                    const auto ptr = make_temp_();
                    os << "    " << ptr << " = llvm.getelementptr " << base.ref << "[0, "
                       << offset->first << "] : (!llvm.ptr) -> !llvm.ptr, " << *storage_ty << "\n";
                    return PlaceInfo{
                        .kind = PlaceInfo::Kind::LlvmPtr,
                        .ref = ptr,
                        .container_type = *storage_ty,
                        .element_type = *field_ty,
                        .layout = field_layout,
                        .storage_type = field_info->second,
                        .host_abi_bytes = is_layout_c_record_(field_info->second),
                        .indices = {},
                    };
                }

                const auto llvm_record_ty = llvm_type_(record_ty);
                if (!llvm_record_ty.has_value()) {
                    fail_("field.place requires a known record LLVM type.");
                    return std::nullopt;
                }
                const auto ptr = make_temp_();
                os << "    " << ptr << " = llvm.getelementptr " << base.ref << "[0, " << field_info->first
                   << "] : (!llvm.ptr) -> !llvm.ptr, " << *llvm_record_ty << "\n";
                return PlaceInfo{
                    .kind = PlaceInfo::Kind::LlvmPtr,
                    .ref = ptr,
                    .container_type = "!llvm.ptr",
                    .element_type = *field_ty,
                    .layout = field_layout,
                    .storage_type = field_info->second,
                    .host_abi_bytes = false,
                    .indices = {},
                };
            }

            void emit_record_literal_store_(llvm::raw_ostream& os,
                                            const PlaceInfo& place,
                                            parus::goir::ValueId value,
                                            parus::ty::TypeId record_ty) {
                const auto* make = producer_as_<parus::goir::OpRecordMake>(value);
                if (make == nullptr) {
                    fail_("record store requires a record.make producer in M1.");
                    return;
                }
                const auto llvm_record_ty = llvm_type_(record_ty);
                const auto* layout = record_layout_(record_ty);
                if (!llvm_record_ty.has_value() || layout == nullptr) {
                    fail_("record store requires a known record layout.");
                    return;
                }

                for (size_t i = 0; i < layout->fields.size(); ++i) {
                    const auto& field = layout->fields[i];
                    const auto it = std::find_if(
                        make->fields.begin(),
                        make->fields.end(),
                        [&](const parus::goir::RecordValueField& member) {
                            return member.name == field.name;
                        }
                    );
                    if (it == make->fields.end()) {
                        fail_("record.make is missing a field required by the frozen layout.");
                        return;
                    }
                    const auto field_place = record_field_place_(
                        os,
                        place,
                        record_ty,
                        field.name,
                        classify_layout_(field.type)
                    );
                    if (!field_place.has_value()) return;
                    if (classify_layout_(field.type) == parus::goir::LayoutClass::PlainRecord) {
                        emit_record_literal_store_(os, *field_place, it->value, field.type);
                        if (failed_) return;
                        continue;
                    }
                    os << "    llvm.store " << value_name_(it->value) << ", " << field_place->ref
                       << " : " << field_place->element_type << ", !llvm.ptr\n";
                }
            }

            void emit_store_(llvm::raw_ostream& os, const parus::goir::OpStore& store) {
                const auto place = place_info_(store.place);
                if (!place.has_value()) {
                    fail_("store target does not have a lowered place representation");
                    return;
                }

                if (place->kind == PlaceInfo::Kind::MemRef) {
                    if (place->layout == parus::goir::LayoutClass::Scalar) {
                        os << "    memref.store " << value_name_(store.value) << ", " << place->ref;
                        if (place->indices.empty()) {
                            os << "[]";
                        } else {
                            os << "[";
                            for (size_t i = 0; i < place->indices.size(); ++i) {
                                if (i != 0) os << ", ";
                                os << place->indices[i];
                            }
                            os << "]";
                        }
                        os << " : " << place->container_type << "\n";
                        return;
                    }

                    if (place->layout == parus::goir::LayoutClass::FixedArray && place->indices.empty()) {
                        const auto* make = producer_as_<parus::goir::OpArrayMake>(store.value);
                        if (make == nullptr) {
                            fail_("full array store currently requires an array.make source in M1.");
                            return;
                        }
                        for (size_t i = 0; i < make->elems.size(); ++i) {
                            const auto idx = const_index_(os, std::to_string(i));
                            os << "    memref.store " << value_name_(make->elems[i]) << ", " << place->ref
                               << "[" << idx << "] : " << place->container_type << "\n";
                        }
                        return;
                    }

                    fail_("unsupported memref store shape in M1.");
                    return;
                }

                if (place->kind == PlaceInfo::Kind::LlvmPtr) {
                    if (place->layout == parus::goir::LayoutClass::PlainRecord) {
                        emit_record_literal_store_(os, *place, store.value, module_.values[store.place].place_elem_type);
                        return;
                    }
                    os << "    llvm.store " << value_name_(store.value) << ", " << place->ref
                       << " : " << place->element_type << ", !llvm.ptr\n";
                    return;
                }

                fail_("unsupported store target kind");
            }

            void emit_load_(llvm::raw_ostream& os,
                            parus::goir::ValueId result,
                            const parus::goir::OpLoad& load) {
                const auto place = place_info_(load.place);
                if (!place.has_value()) {
                    fail_("load source does not have a lowered place representation");
                    return;
                }

                if (place->kind == PlaceInfo::Kind::MemRef) {
                    if (place->layout != parus::goir::LayoutClass::Scalar) {
                        fail_("M1 MLIR lowering only supports scalar memref loads directly.");
                        return;
                    }
                    os << "    " << value_name_(result) << " = memref.load " << place->ref;
                    if (place->indices.empty()) {
                        os << "[]";
                    } else {
                        os << "[";
                        for (size_t i = 0; i < place->indices.size(); ++i) {
                            if (i != 0) os << ", ";
                            os << place->indices[i];
                        }
                        os << "]";
                    }
                    os << " : " << place->container_type << "\n";
                    return;
                }

                if (place->kind == PlaceInfo::Kind::LlvmPtr) {
                    os << "    " << value_name_(result) << " = llvm.load " << place->ref
                       << " : !llvm.ptr -> " << place->element_type << "\n";
                    return;
                }

                fail_("unsupported load source kind");
            }

            void emit_array_get_(llvm::raw_ostream& os,
                                 parus::goir::ValueId result,
                                 const parus::goir::OpArrayGet& get) {
                auto temp = materialize_array_value_to_memref_(os, get.base);
                if (!temp.has_value()) return;
                const auto idx = ensure_index_(os, get.index);
                os << "    " << value_name_(result) << " = memref.load " << temp->ref
                   << "[" << idx << "] : " << temp->container_type << "\n";
            }

            void emit_array_len_(llvm::raw_ostream& os,
                                 parus::goir::ValueId result,
                                 const parus::goir::OpArrayLen& len) {
                const auto& base_value = module_.values[len.base];
                const auto base_ty = base_value.place_elem_type == parus::ty::kInvalidType
                    ? base_value.ty
                    : base_value.place_elem_type;
                if (base_ty != parus::ty::kInvalidType) {
                    const auto& t = types_.get(base_ty);
                    if (t.kind == parus::ty::Kind::kArray && t.array_has_size) {
                        const auto result_mlir_ty = scalar_like_type_(module_.values[result].ty);
                        if (!result_mlir_ty.has_value()) {
                            fail_("array.len result type must be an integer scalar");
                            return;
                        }
                        os << "    " << value_name_(result) << " = arith.constant " << t.array_size
                           << " : " << *result_mlir_ty << "\n";
                        return;
                    }
                }

                const auto place = place_info_(len.base);
                if (!place.has_value() || place->kind != PlaceInfo::Kind::MemRef) {
                    fail_("array.len requires a lowered memref slice/base");
                    return;
                }
                const auto zero = const_index_(os, "0");
                const auto dim = make_temp_();
                os << "    " << dim << " = memref.dim " << place->ref << ", " << zero
                   << " : " << place->container_type << "\n";
                const auto casted = cast_index_to_result_(os, dim, module_.values[result].ty);
                value_names_[result] = casted;
            }

            void emit_local_slot_(llvm::raw_ostream& os,
                                  parus::goir::ValueId result,
                                  const parus::goir::LayoutClass layout) {
                const auto ty = module_.values[result].place_elem_type;
                if (layout == parus::goir::LayoutClass::Scalar || layout == parus::goir::LayoutClass::FixedArray) {
                    const auto memref_ty = memref_slot_type_(types_, ty);
                    if (!memref_ty.has_value()) {
                        fail_("local slot requires a scalar or fixed-size scalar-array memref type.");
                        return;
                    }
                    std::string elem_ty{};
                    if (layout == parus::goir::LayoutClass::Scalar) {
                        const auto scalar = scalar_like_type_(ty);
                        elem_ty = scalar.has_value() ? *scalar : std::string{};
                    } else {
                        const auto& arr_ty = types_.get(ty);
                        const auto elem = scalar_like_type_(arr_ty.elem);
                        elem_ty = elem.has_value() ? *elem : std::string{};
                    }
                    const auto slot = make_temp_();
                    os << "    " << slot << " = memref.alloca() : " << *memref_ty << "\n";
                    set_place_info_(result, PlaceInfo{
                        .kind = PlaceInfo::Kind::MemRef,
                        .ref = slot,
                        .container_type = *memref_ty,
                        .element_type = elem_ty,
                        .layout = layout,
                        .storage_type = ty,
                        .indices = {},
                    });
                    return;
                }

                if (layout == parus::goir::LayoutClass::PlainRecord ||
                    layout == parus::goir::LayoutClass::TextView ||
                    layout == parus::goir::LayoutClass::OptionalScalar ||
                    layout == parus::goir::LayoutClass::TagEnum) {
                    const auto llvm_ty = llvm_type_(ty);
                    if (!llvm_ty.has_value()) {
                        fail_("LLVM-backed local slot requires a compatible LLVM type.");
                        return;
                    }
                    const auto count = llvm_const_i64_(os, 1);
                    const auto slot = make_temp_();
                    os << "    " << slot << " = llvm.alloca " << count << " x " << *llvm_ty
                       << " : (i64) -> !llvm.ptr\n";
                    set_place_info_(result, PlaceInfo{
                        .kind = PlaceInfo::Kind::LlvmPtr,
                        .ref = slot,
                        .container_type = "!llvm.ptr",
                        .element_type = *llvm_ty,
                        .layout = layout,
                        .storage_type = ty,
                        .host_abi_bytes = is_layout_c_record_(ty),
                        .indices = {},
                    });
                    return;
                }

                fail_("unsupported local-slot layout in M1");
            }

            void emit_inst_(llvm::raw_ostream& os, const parus::goir::Inst& inst) {
                std::visit([&](const auto& data) {
                    using T = std::decay_t<decltype(data)>;
                    if constexpr (std::is_same_v<T, parus::goir::OpConstInt>) {
                        const auto ty = module_.values[inst.result].ty;
                        const auto mlir_ty = scalar_like_type_(ty);
                        if (!mlir_ty.has_value()) return fail_("unsupported integer constant type for MLIR lowering");
                        os << "    " << value_name_(inst.result) << " = arith.constant "
                           << sanitize_int_literal_(data.text) << " : " << *mlir_ty << "\n";
                    } else if constexpr (std::is_same_v<T, parus::goir::OpConstFloat>) {
                        const auto ty = module_.values[inst.result].ty;
                        const auto mlir_ty = scalar_like_type_(ty);
                        if (!mlir_ty.has_value()) return fail_("unsupported float constant type for MLIR lowering");
                        os << "    " << value_name_(inst.result) << " = arith.constant "
                           << sanitize_float_literal_(data.text) << " : " << *mlir_ty << "\n";
                    } else if constexpr (std::is_same_v<T, parus::goir::OpConstBool>) {
                        os << "    " << value_name_(inst.result) << " = arith.constant "
                           << (data.value ? "true" : "false") << "\n";
                    } else if constexpr (std::is_same_v<T, parus::goir::OpConstNull>) {
                        fail_("null constants are outside the M1 MLIR lowering subset");
                    } else if constexpr (std::is_same_v<T, parus::goir::OpTextLit>) {
                        const auto it = text_globals_.find(inst.result);
                        if (it == text_globals_.end()) {
                            fail_("text literal lowering could not find a prepared global string");
                            return;
                        }
                        const auto text_ty = llvm_type_(module_.values[inst.result].ty);
                        if (!text_ty.has_value()) {
                            fail_("text literal lowering requires a text-view LLVM type");
                            return;
                        }
                        const size_t storage_len = it->second.uses_sentinel_byte ? size_t{1} : it->second.byte_count;
                        const auto addr = make_temp_();
                        os << "    " << addr << " = llvm.mlir.addressof @" << it->second.symbol << " : !llvm.ptr\n";
                        const auto ptr = make_temp_();
                        os << "    " << ptr << " = llvm.getelementptr " << addr
                           << "[0, 0] : (!llvm.ptr) -> !llvm.ptr, !llvm.array<"
                           << storage_len << " x i8>\n";
                        const auto len = llvm_const_i64_(os, static_cast<int64_t>(it->second.byte_count));
                        const auto zero = make_temp_();
                        os << "    " << zero << " = llvm.mlir.zero : " << *text_ty << "\n";
                        const auto with_ptr = make_temp_();
                        os << "    " << with_ptr << " = llvm.insertvalue " << ptr << ", " << zero
                           << "[0] : " << *text_ty << "\n";
                        os << "    " << value_name_(inst.result) << " = llvm.insertvalue " << len << ", " << with_ptr
                           << "[1] : " << *text_ty << "\n";
                    } else if constexpr (std::is_same_v<T, parus::goir::OpOptionalSome>) {
                        const auto opt_ty = llvm_type_(module_.values[inst.result].ty);
                        if (!opt_ty.has_value()) {
                            fail_("optional.some lowering requires an LLVM-compatible optional type");
                            return;
                        }
                        const auto present = const_bool_(os, true);
                        const auto zero = make_temp_();
                        os << "    " << zero << " = llvm.mlir.zero : " << *opt_ty << "\n";
                        const auto with_present = make_temp_();
                        os << "    " << with_present << " = llvm.insertvalue " << present << ", " << zero
                           << "[0] : " << *opt_ty << "\n";
                        os << "    " << value_name_(inst.result) << " = llvm.insertvalue "
                           << value_name_(data.value) << ", " << with_present
                           << "[1] : " << *opt_ty << "\n";
                    } else if constexpr (std::is_same_v<T, parus::goir::OpOptionalNone>) {
                        const auto opt_ty = llvm_type_(module_.values[inst.result].ty);
                        if (!opt_ty.has_value()) {
                            fail_("optional.none lowering requires an LLVM-compatible optional type");
                            return;
                        }
                        os << "    " << value_name_(inst.result) << " = llvm.mlir.zero : " << *opt_ty << "\n";
                    } else if constexpr (std::is_same_v<T, parus::goir::OpOptionalIsPresent>) {
                        const auto opt_ty = llvm_type_(module_.values[data.optional].ty);
                        if (!opt_ty.has_value()) {
                            fail_("optional.is_present lowering requires an LLVM-compatible optional type");
                            return;
                        }
                        os << "    " << value_name_(inst.result) << " = llvm.extractvalue "
                           << value_name_(data.optional) << "[0] : " << *opt_ty << "\n";
                    } else if constexpr (std::is_same_v<T, parus::goir::OpOptionalGet>) {
                        const auto opt_ty = llvm_type_(module_.values[data.optional].ty);
                        if (!opt_ty.has_value()) {
                            fail_("optional.get lowering requires an LLVM-compatible optional type");
                            return;
                        }
                        os << "    " << value_name_(inst.result) << " = llvm.extractvalue "
                           << value_name_(data.optional) << "[1] : " << *opt_ty << "\n";
                    } else if constexpr (std::is_same_v<T, parus::goir::OpEnumTag>) {
                        const auto tag_ty = scalar_like_type_(module_.values[inst.result].ty);
                        if (!tag_ty.has_value()) {
                            fail_("enum.tag lowering requires a scalar tag storage type");
                            return;
                        }
                        os << "    " << value_name_(inst.result) << " = arith.constant "
                           << data.tag << " : " << *tag_ty << "\n";
                    } else if constexpr (std::is_same_v<T, parus::goir::OpUnary>) {
                        const auto ty = module_.values[inst.result].ty;
                        const auto result = value_name_(inst.result);
                        const auto src = value_name_(data.src);
                        const auto mlir_ty = scalar_like_type_(ty);
                        if (!mlir_ty.has_value()) return fail_("unsupported unary type for MLIR lowering");
                        switch (data.op) {
                            case parus::goir::UnOp::Plus:
                                value_names_[inst.result] = src;
                                break;
                            case parus::goir::UnOp::Neg: {
                                const auto zero = const_zero_(os, ty);
                                os << "    " << result << " = "
                                   << (is_float_(ty) ? "arith.subf " : "arith.subi ")
                                   << zero << ", " << src << " : " << *mlir_ty << "\n";
                                break;
                            }
                            case parus::goir::UnOp::Not: {
                                if (!is_builtin_(types_, ty, parus::ty::Builtin::kBool)) {
                                    fail_("logical not requires bool in M1 MLIR lowering");
                                    return;
                                }
                                const auto t = const_bool_(os, true);
                                os << "    " << result << " = arith.xori " << src << ", " << t << " : i1\n";
                                break;
                            }
                            case parus::goir::UnOp::BitNot: {
                                if (!is_integer_like_(ty)) {
                                    fail_("bitnot requires integer in M1 MLIR lowering");
                                    return;
                                }
                                const auto all_ones = const_all_ones_(os, ty);
                                os << "    " << result << " = arith.xori " << src << ", "
                                   << all_ones << " : " << *mlir_ty << "\n";
                                break;
                            }
                        }
                    } else if constexpr (std::is_same_v<T, parus::goir::OpBinary>) {
                        const auto result_ty_id = module_.values[inst.result].ty;
                        const auto operand_ty_id = module_.values[data.lhs].ty;
                        const auto result = value_name_(inst.result);
                        const auto lhs = value_name_(data.lhs);
                        const auto rhs = value_name_(data.rhs);
                        const auto result_mlir_ty = scalar_like_type_(result_ty_id);
                        const auto operand_mlir_ty = scalar_like_type_(operand_ty_id);
                        if (!result_mlir_ty.has_value() || !operand_mlir_ty.has_value()) {
                            return fail_("unsupported binary type for MLIR lowering");
                        }

                        const bool is_float = is_float_(operand_ty_id);
                        const bool is_int = is_integer_like_(operand_ty_id);
                        switch (data.op) {
                            case parus::goir::BinOp::Add:
                                os << "    " << result << " = " << (is_float ? "arith.addf " : "arith.addi ")
                                   << lhs << ", " << rhs << " : " << *result_mlir_ty << "\n";
                                break;
                            case parus::goir::BinOp::Sub:
                                os << "    " << result << " = " << (is_float ? "arith.subf " : "arith.subi ")
                                   << lhs << ", " << rhs << " : " << *result_mlir_ty << "\n";
                                break;
                            case parus::goir::BinOp::Mul:
                                os << "    " << result << " = " << (is_float ? "arith.mulf " : "arith.muli ")
                                   << lhs << ", " << rhs << " : " << *result_mlir_ty << "\n";
                                break;
                            case parus::goir::BinOp::Div:
                                if (is_float) {
                                    os << "    " << result << " = arith.divf " << lhs << ", " << rhs
                                       << " : " << *result_mlir_ty << "\n";
                                } else {
                                    os << "    " << result << " = "
                                       << (is_signed_like_(operand_ty_id) ? "arith.divsi " : "arith.divui ")
                                       << lhs << ", " << rhs << " : " << *result_mlir_ty << "\n";
                                }
                                break;
                            case parus::goir::BinOp::Rem:
                                if (is_float) {
                                    os << "    " << result << " = arith.remf " << lhs << ", " << rhs
                                       << " : " << *result_mlir_ty << "\n";
                                } else {
                                    os << "    " << result << " = "
                                       << (is_signed_like_(operand_ty_id) ? "arith.remsi " : "arith.remui ")
                                       << lhs << ", " << rhs << " : " << *result_mlir_ty << "\n";
                                }
                                break;
                            case parus::goir::BinOp::Lt:
                            case parus::goir::BinOp::Le:
                            case parus::goir::BinOp::Gt:
                            case parus::goir::BinOp::Ge:
                            case parus::goir::BinOp::Eq:
                            case parus::goir::BinOp::Ne: {
                                if (is_float) {
                                    const char* pred = "oeq";
                                    switch (data.op) {
                                        case parus::goir::BinOp::Lt: pred = "olt"; break;
                                        case parus::goir::BinOp::Le: pred = "ole"; break;
                                        case parus::goir::BinOp::Gt: pred = "ogt"; break;
                                        case parus::goir::BinOp::Ge: pred = "oge"; break;
                                        case parus::goir::BinOp::Eq: pred = "oeq"; break;
                                        case parus::goir::BinOp::Ne: pred = "one"; break;
                                        default: break;
                                    }
                                    os << "    " << result << " = arith.cmpf " << pred << ", "
                                       << lhs << ", " << rhs << " : " << *operand_mlir_ty << "\n";
                                } else if (is_int) {
                                    const char* pred = "eq";
                                    switch (data.op) {
                                        case parus::goir::BinOp::Lt: pred = is_signed_like_(operand_ty_id) ? "slt" : "ult"; break;
                                        case parus::goir::BinOp::Le: pred = is_signed_like_(operand_ty_id) ? "sle" : "ule"; break;
                                        case parus::goir::BinOp::Gt: pred = is_signed_like_(operand_ty_id) ? "sgt" : "ugt"; break;
                                        case parus::goir::BinOp::Ge: pred = is_signed_like_(operand_ty_id) ? "sge" : "uge"; break;
                                        case parus::goir::BinOp::Eq: pred = "eq"; break;
                                        case parus::goir::BinOp::Ne: pred = "ne"; break;
                                        default: break;
                                    }
                                    os << "    " << result << " = arith.cmpi " << pred << ", "
                                       << lhs << ", " << rhs << " : " << *operand_mlir_ty << "\n";
                                } else {
                                    fail_("comparison requires scalar integer/float operands in M1");
                                }
                                break;
                            }
                            case parus::goir::BinOp::LogicalAnd:
                                os << "    " << result << " = arith.andi " << lhs << ", " << rhs
                                   << " : " << *result_mlir_ty << "\n";
                                break;
                            case parus::goir::BinOp::LogicalOr:
                                os << "    " << result << " = arith.ori " << lhs << ", " << rhs
                                   << " : " << *result_mlir_ty << "\n";
                                break;
                        }
                    } else if constexpr (std::is_same_v<T, parus::goir::OpCast>) {
                        const auto src_ty = module_.values[data.src].ty;
                        const auto dst_ty = module_.values[inst.result].ty;
                        const auto src_name = value_name_(data.src);
                        const auto dst_name = value_name_(inst.result);
                        const auto src_mlir_ty = scalar_like_type_(src_ty);
                        const auto dst_mlir_ty = scalar_like_type_(dst_ty);
                        if (!src_mlir_ty.has_value() || !dst_mlir_ty.has_value()) {
                            fail_("unsupported cast type in M1 MLIR lowering");
                            return;
                        }
                        if (*src_mlir_ty == *dst_mlir_ty) {
                            value_names_[inst.result] = src_name;
                            return;
                        }
                        if (is_integer_like_(src_ty) && is_integer_like_(dst_ty)) {
                            const auto src_width = src_mlir_ty->substr(1);
                            const auto dst_width = dst_mlir_ty->substr(1);
                            if (src_width == dst_width) {
                                value_names_[inst.result] = src_name;
                            } else if (std::stoi(std::string(dst_width)) > std::stoi(std::string(src_width))) {
                                os << "    " << dst_name << " = "
                                   << (is_signed_like_(src_ty) ? "arith.extsi " : "arith.extui ")
                                   << src_name << " : " << *src_mlir_ty << " to " << *dst_mlir_ty << "\n";
                            } else {
                                os << "    " << dst_name << " = arith.trunci " << src_name
                                   << " : " << *src_mlir_ty << " to " << *dst_mlir_ty << "\n";
                            }
                            return;
                        }
                        if (is_integer_like_(src_ty) && is_float_(dst_ty)) {
                            os << "    " << dst_name << " = "
                               << (is_signed_like_(src_ty) ? "arith.sitofp " : "arith.uitofp ")
                               << src_name << " : " << *src_mlir_ty << " to " << *dst_mlir_ty << "\n";
                            return;
                        }
                        if (is_float_(src_ty) && is_integer_like_(dst_ty)) {
                            os << "    " << dst_name << " = "
                               << (is_signed_like_(dst_ty) ? "arith.fptosi " : "arith.fptoui ")
                               << src_name << " : " << *src_mlir_ty << " to " << *dst_mlir_ty << "\n";
                            return;
                        }
                        if (is_float_(src_ty) && is_float_(dst_ty)) {
                            const auto src_kind = builtin_of_(types_, src_ty);
                            const auto dst_kind = builtin_of_(types_, dst_ty);
                            os << "    " << dst_name << " = "
                               << ((*dst_kind == parus::ty::Builtin::kF64 && *src_kind == parus::ty::Builtin::kF32)
                                        ? "arith.extf " : "arith.truncf ")
                               << src_name << " : " << *src_mlir_ty << " to " << *dst_mlir_ty << "\n";
                            return;
                        }
                        if (is_builtin_(types_, src_ty, parus::ty::Builtin::kBool) && is_integer_like_(dst_ty)) {
                            os << "    " << dst_name << " = arith.extui " << src_name
                               << " : i1 to " << *dst_mlir_ty << "\n";
                            return;
                        }
                        fail_("unsupported cast combination in M1 MLIR lowering");
                    } else if constexpr (std::is_same_v<T, parus::goir::OpArrayMake>) {
                        // Array values remain semantic until consumed by store/array.get.
                    } else if constexpr (std::is_same_v<T, parus::goir::OpArrayGet>) {
                        emit_array_get_(os, inst.result, data);
                    } else if constexpr (std::is_same_v<T, parus::goir::OpArrayLen>) {
                        emit_array_len_(os, inst.result, data);
                    } else if constexpr (std::is_same_v<T, parus::goir::OpRecordMake>) {
                        // Record values remain semantic until consumed by store.
                    } else if constexpr (std::is_same_v<T, parus::goir::OpLocalSlot>) {
                        emit_local_slot_(os, inst.result, module_.values[inst.result].layout);
                    } else if constexpr (std::is_same_v<T, parus::goir::OpGlobalSlot>) {
                        const auto& global = module_.globals[data.global];
                        const auto storage_ty = llvm_type_(global.type);
                        if (!storage_ty.has_value()) {
                            fail_("global.slot lowering requires an LLVM-compatible storage type.");
                            return;
                        }
                        const auto addr = make_temp_();
                        os << "    " << addr << " = llvm.mlir.addressof @" << global_symbol_(global)
                           << " : !llvm.ptr\n";
                        set_place_info_(inst.result, PlaceInfo{
                            .kind = PlaceInfo::Kind::LlvmPtr,
                            .ref = addr,
                            .container_type = *storage_ty,
                            .element_type = *storage_ty,
                            .layout = module_.values[inst.result].layout,
                            .storage_type = global.type,
                            .host_abi_bytes = is_layout_c_record_(global.type),
                            .indices = {},
                        });
                    } else if constexpr (std::is_same_v<T, parus::goir::OpFieldPlace>) {
                        const auto base = place_info_(data.base);
                        if (!base.has_value() || base->kind != PlaceInfo::Kind::LlvmPtr) {
                            fail_("field.place requires an LLVM aggregate-backed base place in M1.");
                            return;
                        }
                        const auto base_record_ty = module_.values[data.base].place_elem_type;
                        const auto field_place = record_field_place_(
                            os,
                            *base,
                            base_record_ty,
                            data.field_name,
                            module_.values[inst.result].layout
                        );
                        if (!field_place.has_value()) return;
                        set_place_info_(inst.result, *field_place);
                    } else if constexpr (std::is_same_v<T, parus::goir::OpIndexPlace>) {
                        const auto base = place_info_(data.base);
                        if (!base.has_value() || base->kind != PlaceInfo::Kind::MemRef) {
                            fail_("index.place requires a memref-backed base place in M1.");
                            return;
                        }
                        auto next = *base;
                        next.layout = module_.values[inst.result].layout;
                        next.indices.push_back(ensure_index_(os, data.index));
                        set_place_info_(inst.result, std::move(next));
                    } else if constexpr (std::is_same_v<T, parus::goir::OpSubView>) {
                        const auto base = place_info_(data.base);
                        if (!base.has_value() || base->kind != PlaceInfo::Kind::MemRef) {
                            fail_("subview requires a memref-backed base place in M1.");
                            return;
                        }
                        const auto offset = ensure_index_(os, data.offset);
                        const auto length = ensure_index_(os, data.length);
                        const auto result_ty = slice_memref_type_(types_, module_.values[inst.result].place_elem_type);
                        if (!result_ty.has_value()) {
                            fail_("subview lowering requires a 1-D scalar slice view type.");
                            return;
                        }
                        const auto sub = make_temp_();
                        os << "    " << sub << " = memref.subview " << base->ref
                           << "[" << offset << "] [" << length << "] [1] : "
                           << base->container_type << " to " << *result_ty << "\n";
                        set_place_info_(inst.result, PlaceInfo{
                            .kind = PlaceInfo::Kind::MemRef,
                            .ref = sub,
                            .container_type = *result_ty,
                            .element_type = base->element_type,
                            .layout = parus::goir::LayoutClass::SliceView,
                            .indices = {},
                        });
                    } else if constexpr (std::is_same_v<T, parus::goir::OpLoad>) {
                        emit_load_(os, inst.result, data);
                    } else if constexpr (std::is_same_v<T, parus::goir::OpStore>) {
                        emit_store_(os, data);
                    } else if constexpr (std::is_same_v<T, parus::goir::OpBorrowView>) {
                        fail_("placed MLIR lowering cannot see borrow.view markers");
                    } else if constexpr (std::is_same_v<T, parus::goir::OpEscapeView>) {
                        fail_("placed MLIR lowering cannot see escape.view markers");
                    } else if constexpr (std::is_same_v<T, parus::goir::OpCallDirect>) {
                        const auto& callee = module_.realizations[data.callee];
                        const auto sig_id = module_.computations[callee.computation].sig;
                        const auto& sig = module_.semantic_sigs[sig_id];
                        std::string args{};
                        std::string arg_types{};
                        for (size_t i = 0; i < data.args.size(); ++i) {
                            if (i != 0) {
                                args += ", ";
                                arg_types += ", ";
                            }
                            args += value_name_(data.args[i]);
                            const auto mlir_ty = mlir_value_type_(module_.values[data.args[i]].ty);
                            if (!mlir_ty.has_value()) {
                                fail_("M1 direct calls only support LLVM-compatible value arguments.");
                                return;
                            }
                            arg_types += *mlir_ty;
                        }
                        if (inst.result != parus::goir::kInvalidId) {
                            const auto result_ty = mlir_value_type_(sig.result_type);
                            if (!result_ty.has_value()) {
                                fail_("M1 direct calls only support LLVM-compatible value results.");
                                return;
                            }
                            os << "    " << value_name_(inst.result) << " = func.call @"
                               << module_.string(callee.name) << "(" << args << ") : ("
                               << arg_types << ") -> " << *result_ty << "\n";
                        } else {
                            os << "    func.call @" << module_.string(callee.name) << "(" << args << ") : ("
                               << arg_types << ") -> ()\n";
                        }
                    } else if constexpr (std::is_same_v<T, parus::goir::OpSemanticInvoke>) {
                        fail_("placed MLIR lowering cannot see semantic invokes");
                    } else if constexpr (std::is_same_v<T, parus::goir::OpCallExtern>) {
                        const auto& callee = module_.realizations[data.callee];
                        const auto sig_id = module_.computations[callee.computation].sig;
                        const auto& sig = module_.semantic_sigs[sig_id];
                        std::string args{};
                        std::string arg_types{};
                        for (size_t i = 0; i < data.args.size(); ++i) {
                            if (i != 0) {
                                args += ", ";
                                arg_types += ", ";
                            }
                            args += value_name_(data.args[i]);
                            const auto arg_ty = (i < sig.param_types.size()) ? sig.param_types[i] : module_.values[data.args[i]].ty;
                            const auto mlir_ty = llvm_type_(arg_ty);
                            if (!mlir_ty.has_value()) {
                                fail_("extern call lowering requires LLVM-compatible host ABI arguments.");
                                return;
                            }
                            arg_types += *mlir_ty;
                        }
                        const auto callee_name = std::string(
                            callee.link_name != parus::goir::kInvalidId
                                ? module_.string(callee.link_name)
                                : module_.string(callee.name)
                        );
                        if (inst.result != parus::goir::kInvalidId) {
                            const auto result_ty = llvm_type_(sig.result_type);
                            if (!result_ty.has_value()) {
                                fail_("extern call lowering requires an LLVM-compatible host ABI result.");
                                return;
                            }
                            os << "    " << value_name_(inst.result) << " = llvm.call @" << callee_name
                               << "(" << args << ") : (" << arg_types << ") -> " << *result_ty << "\n";
                        } else {
                            os << "    llvm.call @" << callee_name << "(" << args << ") : ("
                               << arg_types << ") -> ()\n";
                        }
                    }
                }, inst.data);
            }

            void emit_branch_target_(llvm::raw_ostream& os,
                                     parus::goir::BlockId target,
                                     const std::vector<parus::goir::ValueId>& args) {
                os << "^bb" << target;
                if (args.empty()) return;
                os << "(";
                for (size_t i = 0; i < args.size(); ++i) {
                    if (i != 0) os << ", ";
                    const auto ty = mlir_value_type_(module_.values[args[i]].ty);
                    if (!ty.has_value()) {
                        fail_("block arguments currently require LLVM-compatible value types in M1.");
                        return;
                    }
                    os << value_name_(args[i]) << " : " << *ty;
                }
                os << ")";
            }

            void emit_block_term_(llvm::raw_ostream& os, const parus::goir::Block& block) {
                std::visit([&](const auto& term) {
                    using T = std::decay_t<decltype(term)>;
                    if constexpr (std::is_same_v<T, parus::goir::TermBr>) {
                        os << "    cf.br ";
                        emit_branch_target_(os, term.target, term.args);
                        os << "\n";
                    } else if constexpr (std::is_same_v<T, parus::goir::TermCondBr>) {
                        os << "    cf.cond_br " << value_name_(term.cond) << ", ";
                        emit_branch_target_(os, term.then_bb, term.then_args);
                        os << ", ";
                        emit_branch_target_(os, term.else_bb, term.else_args);
                        os << "\n";
                    } else if constexpr (std::is_same_v<T, parus::goir::TermSwitch>) {
                        const auto scrutinee_ty = scalar_like_type_(module_.values[term.scrutinee].ty);
                        if (!scrutinee_ty.has_value()) {
                            fail_("cf.switch lowering requires an integer-like scrutinee type.");
                            return;
                        }
                        os << "    cf.switch " << value_name_(term.scrutinee) << " : " << *scrutinee_ty << ", [\n";
                        os << "      default: ";
                        emit_branch_target_(os, term.default_bb, term.default_args);
                        if (failed_) return;
                        if (!term.arms.empty()) os << ",";
                        os << "\n";
                        for (size_t i = 0; i < term.arms.size(); ++i) {
                            os << "      " << term.arms[i].match_value << ": ";
                            emit_branch_target_(os, term.arms[i].target, term.arms[i].args);
                            if (failed_) return;
                            if (i + 1 != term.arms.size()) os << ",";
                            os << "\n";
                        }
                        os << "    ]\n";
                    } else if constexpr (std::is_same_v<T, parus::goir::TermRet>) {
                        if (term.has_value) {
                            const auto ty = mlir_value_type_(module_.values[term.value].ty);
                            if (!ty.has_value()) {
                                fail_("func.return currently requires an LLVM-compatible result type in M1.");
                                return;
                            }
                            os << "    func.return " << value_name_(term.value) << " : " << *ty << "\n";
                        } else {
                            os << "    func.return\n";
                        }
                    }
                }, block.term);
            }

            void emit_realization_(llvm::raw_ostream& os,
                                   const parus::goir::GRealization& real) {
                const auto sig_id = module_.computations[real.computation].sig;
                const auto& sig = module_.semantic_sigs[sig_id];
                const auto result_ty = mlir_value_type_(sig.result_type);
                if (!result_ty.has_value() && !is_unit_(sig.result_type)) {
                    fail_("unsupported function result type in M1 MLIR lowering");
                    return;
                }

                const auto symbol = std::string(
                    real.link_name != parus::goir::kInvalidId
                        ? module_.string(real.link_name)
                        : module_.string(real.name)
                );

                if (real.linkage == parus::goir::LinkageKind::External) {
                    os << "  llvm.func @" << symbol << "(";
                    for (size_t i = 0; i < sig.param_types.size(); ++i) {
                        if (i != 0) os << ", ";
                        const auto llvm_ty = llvm_type_(sig.param_types[i]);
                        if (!llvm_ty.has_value()) {
                            fail_("external host ABI declaration requires LLVM-compatible parameter types");
                            return;
                        }
                        os << *llvm_ty;
                    }
                    os << ")";
                    if (!is_unit_(sig.result_type)) {
                        const auto llvm_ret_ty = llvm_type_(sig.result_type);
                        if (!llvm_ret_ty.has_value()) {
                            fail_("external host ABI declaration requires an LLVM-compatible result type");
                            return;
                        }
                        os << " -> " << *llvm_ret_ty;
                    }
                    os << "\n";
                    return;
                }

                if (real.entry == parus::goir::kInvalidId || static_cast<size_t>(real.entry) >= module_.blocks.size()) {
                    fail_("realization has invalid entry block");
                    return;
                }
                const auto& entry = module_.blocks[real.entry];
                if (entry.params.size() != sig.param_types.size()) {
                    fail_("entry block param count does not match semantic signature");
                    return;
                }

                for (const auto block_id : real.blocks) {
                    const auto& block = module_.blocks[block_id];
                    if (block_id == real.entry) {
                        for (size_t i = 0; i < block.params.size(); ++i) {
                            value_names_[block.params[i]] = "%arg" + std::to_string(i);
                        }
                    } else {
                        for (size_t i = 0; i < block.params.size(); ++i) {
                            value_names_[block.params[i]] = make_block_arg_(block_id, i);
                        }
                    }
                }

                os << "  func.func @" << symbol << "(";
                for (size_t i = 0; i < entry.params.size(); ++i) {
                    if (i != 0) os << ", ";
                    const auto mlir_ty = mlir_value_type_(module_.values[entry.params[i]].ty);
                    if (!mlir_ty.has_value()) {
                        fail_("unsupported entry parameter type in M1 MLIR lowering");
                        return;
                    }
                    os << value_names_[entry.params[i]] << ": " << *mlir_ty;
                }
                os << ")";
                if (!is_unit_(sig.result_type)) {
                    os << " -> " << *result_ty;
                }
                os << " {\n";

                for (size_t bi = 0; bi < real.blocks.size(); ++bi) {
                    const auto block_id = real.blocks[bi];
                    const auto& block = module_.blocks[block_id];
                    if (block_id != real.entry) {
                        os << "  ^bb" << block_id;
                        if (!block.params.empty()) {
                            os << "(";
                            for (size_t i = 0; i < block.params.size(); ++i) {
                                if (i != 0) os << ", ";
                                const auto mlir_ty = mlir_value_type_(module_.values[block.params[i]].ty);
                                if (!mlir_ty.has_value()) {
                                    fail_("unsupported non-entry block parameter type in M1 MLIR lowering");
                                    return;
                                }
                                os << value_names_[block.params[i]] << ": " << *mlir_ty;
                            }
                            os << ")";
                        }
                        os << ":\n";
                    }
                    for (const auto iid : block.insts) {
                        emit_inst_(os, module_.insts[iid]);
                        if (failed_) return;
                    }
                    emit_block_term_(os, block);
                    if (failed_) return;
                }
                os << "  }\n";
            }
        };

        ::mlir::OwningOpRef<::mlir::ModuleOp> parse_mlir_module_(const std::string& mlir_text,
                                                                 ::mlir::MLIRContext& context) {
            ::mlir::ParserConfig config(&context);
            return ::mlir::parseSourceString<::mlir::ModuleOp>(mlir_text, config, "parus-goir");
        }

        const parus::goir::RecordLayout* find_record_layout_(const parus::goir::Module& module,
                                                             parus::ty::TypeId ty) {
            return module.find_record_layout(ty);
        }

        std::optional<uint32_t> host_abi_align_(const parus::goir::Module& module,
                                                const parus::ty::TypePool& types,
                                                parus::ty::TypeId ty) {
            if (ty == parus::ty::kInvalidType) return std::nullopt;
            const auto& t = types.get(ty);
            using B = parus::ty::Builtin;
            if (t.kind == parus::ty::Kind::kBuiltin) {
                switch (t.builtin) {
                    case B::kBool:
                    case B::kI8:
                    case B::kU8:
                        return uint32_t{1};
                    case B::kI16:
                    case B::kU16:
                        return uint32_t{2};
                    case B::kChar:
                    case B::kI32:
                    case B::kU32:
                    case B::kF32:
                        return uint32_t{4};
                    case B::kI64:
                    case B::kU64:
                    case B::kISize:
                    case B::kUSize:
                    case B::kF64:
                        return uint32_t{8};
                    case B::kUnit:
                        return uint32_t{1};
                    default:
                        return std::nullopt;
                }
            }
            if (t.kind == parus::ty::Kind::kNamedUser) {
                const auto* layout = find_record_layout_(module, ty);
                if (layout != nullptr && layout->fields.size() == 1 &&
                    module.string(layout->fields[0].name) == "__tag") {
                    return host_abi_align_(module, types, layout->fields[0].type);
                }
                if (layout == nullptr || layout->layout != parus::sir::FieldLayout::kC) return std::nullopt;
                uint32_t max_align = 1;
                for (const auto& field : layout->fields) {
                    const auto field_align = host_abi_align_(module, types, field.type);
                    if (!field_align.has_value()) return std::nullopt;
                    max_align = std::max(max_align, *field_align);
                }
                if (layout->align != 0) max_align = std::max(max_align, layout->align);
                return max_align;
            }
            return std::nullopt;
        }

        std::string llvm_callconv_prefix_(parus::sir::CCallConv cc) {
            using CC = parus::sir::CCallConv;
            switch (cc) {
                case CC::kStdCall: return "x86_stdcallcc ";
                case CC::kFastCall: return "x86_fastcallcc ";
                case CC::kVectorCall: return "x86_vectorcallcc ";
                case CC::kWin64: return "win64cc ";
                case CC::kSysV: return "x86_64_sysvcc ";
                case CC::kCdecl:
                case CC::kDefault:
                default:
                    return "";
            }
        }

        std::string patch_host_abi_llvm_ir_(const parus::goir::Module& module,
                                            const parus::ty::TypePool& types,
                                            std::string llvm_ir) {
            auto patch_prefixed_symbol = [&](const std::string& symbol, std::string_view prefix) {
                if (prefix.empty()) return;
                const std::string decl = "declare ";
                const std::string def = "define ";
                const std::string call = "call ";
                const std::string needle = " @" + symbol + "(";

                size_t pos = 0;
                while (pos < llvm_ir.size()) {
                    const size_t line_end = llvm_ir.find('\n', pos);
                    const size_t end = (line_end == std::string::npos) ? llvm_ir.size() : line_end;
                    std::string_view line(llvm_ir.data() + pos, end - pos);
                    if (line.starts_with(decl) && line.find(needle) != std::string_view::npos &&
                        line.find(prefix) == std::string_view::npos) {
                        llvm_ir.insert(pos + decl.size(), prefix);
                        pos += prefix.size();
                    } else if (line.starts_with(def) && line.find(needle) != std::string_view::npos &&
                               line.find(prefix) == std::string_view::npos) {
                        llvm_ir.insert(pos + def.size(), prefix);
                        pos += prefix.size();
                    } else {
                        const auto call_at = line.find(call);
                        const auto sym_at = line.find("@" + symbol + "(");
                        if (call_at != std::string_view::npos &&
                            sym_at != std::string_view::npos &&
                            line.find(prefix, call_at) == std::string_view::npos) {
                            llvm_ir.insert(pos + call_at + call.size(), prefix);
                            pos += prefix.size();
                        }
                    }
                    pos = (line_end == std::string::npos) ? llvm_ir.size() : (line_end + 1);
                }
            };

            auto patch_global_align = [&](const std::string& symbol, uint32_t align) {
                if (align <= 1) return;
                const std::string needle = "@" + symbol + " = ";
                size_t pos = 0;
                while ((pos = llvm_ir.find(needle, pos)) != std::string::npos) {
                    const size_t line_end = llvm_ir.find('\n', pos);
                    const size_t end = (line_end == std::string::npos) ? llvm_ir.size() : line_end;
                    std::string_view line(llvm_ir.data() + pos, end - pos);
                    if (line.find(" align ") == std::string_view::npos) {
                        llvm_ir.insert(end, ", align " + std::to_string(align));
                    }
                    pos = (line_end == std::string::npos) ? llvm_ir.size() : (line_end + 1);
                }
            };

            for (const auto& real : module.realizations) {
                if (real.abi_kind != parus::goir::AbiKind::C) continue;
                const auto prefix = llvm_callconv_prefix_(real.c_callconv);
                if (prefix.empty()) continue;
                const auto symbol = std::string(
                    real.link_name != parus::goir::kInvalidId
                        ? module.string(real.link_name)
                        : module.string(real.name)
                );
                patch_prefixed_symbol(symbol, prefix);
            }

            for (const auto& global : module.globals) {
                if (global.abi_kind != parus::goir::AbiKind::C) continue;
                const auto align = host_abi_align_(module, types, global.type);
                if (!align.has_value()) continue;
                const auto symbol = std::string(
                    global.link_name != parus::goir::kInvalidId
                        ? module.string(global.link_name)
                        : module.string(global.name)
                );
                patch_global_align(symbol, *align);
            }

            return llvm_ir;
        }

    } // namespace

    GOIRLoweringResult lower_goir_to_mlir_text(
        const parus::goir::Module& module,
        const parus::ty::TypePool& types
    ) {
        MlirEmitter emitter(module, types);
        return emitter.emit();
    }

    GOIRLLVMIRResult lower_goir_to_llvm_ir_text(
        const parus::goir::Module& module,
        const parus::ty::TypePool& types,
        const GOIRLoweringOptions& options
    ) {
        GOIRLLVMIRResult out{};

        if (options.llvm_lane_major != PARUS_MLIR_SELECTED_MAJOR) {
            push_error_(out.messages, "gOIR MLIR lowering requires LLVM/MLIR lane " + std::to_string(PARUS_MLIR_SELECTED_MAJOR) + ".");
            return out;
        }

        auto mlir = lower_goir_to_mlir_text(module, types);
        out.mlir_text = mlir.mlir_text;
        out.messages = mlir.messages;
        if (!mlir.ok) return out;

        ::mlir::DialectRegistry registry;
        registry.insert<::mlir::func::FuncDialect,
                        ::mlir::arith::ArithDialect,
                        ::mlir::cf::ControlFlowDialect,
                        ::mlir::index::IndexDialect,
                        ::mlir::memref::MemRefDialect,
                        ::mlir::LLVM::LLVMDialect>();

        ::mlir::MLIRContext context;
        context.appendDialectRegistry(registry);
        context.loadAllAvailableDialects();

        auto parsed = parse_mlir_module_(out.mlir_text, context);
        if (!parsed) {
            push_error_(out.messages, "failed to parse generated MLIR text.");
            return out;
        }

        ::mlir::PassManager pm(&context);
        pm.addPass(::mlir::createArithToLLVMConversionPass());
        pm.addPass(::mlir::createConvertIndexToLLVMPass());
        pm.addPass(::mlir::createConvertControlFlowToLLVMPass());
        pm.addPass(::mlir::memref::createExpandStridedMetadataPass());
        pm.addPass(::mlir::createFinalizeMemRefToLLVMConversionPass());
        pm.addPass(::mlir::createConvertFuncToLLVMPass());
        pm.addPass(::mlir::createReconcileUnrealizedCastsPass());
        if (::mlir::failed(pm.run(parsed.get()))) {
            push_error_(out.messages, "failed to lower mixed func/arith/cf/memref/LLVM module to LLVM dialect.");
            return out;
        }

        ::mlir::registerBuiltinDialectTranslation(context);
        ::mlir::registerLLVMDialectTranslation(context);

        llvm::LLVMContext llvm_context;
        auto llvm_module = ::mlir::translateModuleToLLVMIR(parsed.get(), llvm_context, "ParusGOIR");
        if (!llvm_module) {
            push_error_(out.messages, "failed to translate LLVM dialect module to LLVM IR.");
            return out;
        }

        std::string llvm_ir{};
        llvm::raw_string_ostream os(llvm_ir);
        llvm_module->print(os, nullptr);
        os.flush();

        out.ok = true;
        out.llvm_ir = patch_host_abi_llvm_ir_(module, types, std::move(llvm_ir));
        return out;
    }

    GOIRObjectEmissionResult emit_object_from_goir_via_mlir(
        const parus::goir::Module& module,
        const parus::ty::TypePool& types,
        const GOIRLoweringOptions& lowering_options,
        const std::string& output_path,
        const std::string& target_triple,
        const std::string& cpu,
        uint8_t opt_level
    ) {
        GOIRObjectEmissionResult out{};

        auto lowered = lower_goir_to_llvm_ir_text(module, types, lowering_options);
        out.mlir_text = std::move(lowered.mlir_text);
        out.llvm_ir = std::move(lowered.llvm_ir);
        out.messages = std::move(lowered.messages);
        if (!lowered.ok) return out;

        const auto emitted = parus::backend::aot::emit_object_from_llvm_ir_text(
            out.llvm_ir,
            output_path,
            parus::backend::aot::LLVMObjectEmissionOptions{
                .llvm_lane_major = lowering_options.llvm_lane_major,
                .target_triple = target_triple,
                .cpu = cpu,
                .opt_level = opt_level,
            }
        );
        out.messages.insert(out.messages.end(), emitted.messages.begin(), emitted.messages.end());
        out.ok = emitted.ok;
        return out;
    }

    bool run_mlir_smoke(std::string* error_text) {
        ::mlir::DialectRegistry registry;
        registry.insert<::mlir::LLVM::LLVMDialect>();
        ::mlir::MLIRContext context;
        context.appendDialectRegistry(registry);
        context.loadAllAvailableDialects();

        const char* llvm_dialect_ir =
            "module {\n"
            "  llvm.func @main() -> i32 {\n"
            "    %0 = llvm.mlir.constant(0 : i32) : i32\n"
            "    llvm.return %0 : i32\n"
            "  }\n"
            "}\n";

        auto parsed = parse_mlir_module_(llvm_dialect_ir, context);
        if (!parsed) {
            if (error_text) *error_text = "failed to parse LLVM dialect smoke module";
            return false;
        }

        ::mlir::registerBuiltinDialectTranslation(context);
        ::mlir::registerLLVMDialectTranslation(context);

        llvm::LLVMContext llvm_context;
        auto llvm_module = ::mlir::translateModuleToLLVMIR(parsed.get(), llvm_context, "ParusMLIRSmoke");
        if (!llvm_module) {
            if (error_text) *error_text = "failed to translate LLVM dialect smoke module";
            return false;
        }

        std::string llvm_ir{};
        llvm::raw_string_ostream os(llvm_ir);
        llvm_module->print(os, nullptr);
        os.flush();
        const bool ok = llvm_ir.find("define i32 @main()") != std::string::npos;
        if (!ok && error_text) {
            *error_text = "smoke translation output did not contain main definition";
        }
        return ok;
    }

} // namespace parus::backend::mlir
