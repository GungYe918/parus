// compiler/parusc/src/p0/P0Compiler.cpp
#include <parusc/p0/P0Compiler.hpp>

#include <parusc/dump/Dump.hpp>

#include <parus/ast/Nodes.hpp>
#include <parus/cap/CapabilityCheck.hpp>
#include <parus/cimport/CHeaderImport.hpp>
#include <parus/cimport/CImportManifest.hpp>
#include <parus/cimport/CImportPayload.hpp>
#include <parus/cimport/ToolchainResolver.hpp>
#include <parus/cimport/TypeReprNormalize.hpp>
#include <parus/cimport/TypeSemantic.hpp>
#include <parus/common/ModulePath.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/diag/Render.hpp>
#include <parus/lex/Lexer.hpp>
#include <parus/oir/Builder.hpp>
#include <parus/oir/Passes.hpp>
#include <parus/oir/Verify.hpp>
#include <parus/macro/Expander.hpp>
#include <parus/parse/Parser.hpp>
#include <parus/passes/Passes.hpp>
#include <parus/os/File.hpp>
#include <parus/sir/Builder.hpp>
#include <parus/sir/CapabilityAnalysis.hpp>
#include <parus/sir/MutAnalysis.hpp>
#include <parus/sir/Verify.hpp>
#include <parus/text/SourceManager.hpp>
#include <parus/type/TypeResolve.hpp>
#include <parus/ty/TypePool.hpp>
#include <parus/tyck/TypeCheck.hpp>

#if PARUSC_HAS_AOT_BACKEND
#include <parus/backend/aot/AOTBackend.hpp>
#include <parus/backend/link/Linker.hpp>
#endif

#include <filesystem>
#include <cctype>
#include <functional>
#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace parusc::p0 {

    namespace {

        /// @brief 진단을 컨텍스트 포함 형태로 출력한다.
        int flush_diags_(
            const parus::diag::Bag& bag,
            parus::diag::Language lang,
            const parus::SourceManager& sm,
            uint32_t context_lines,
            cli::DiagFormat format
        ) {
            if (bag.diags().empty()) return 0;

            if (format == cli::DiagFormat::kText) {
                for (const auto& d : bag.diags()) {
                    std::cerr << parus::diag::render_one_context(d, lang, sm, context_lines) << "\n";
                }
                return bag.has_error() ? 1 : 0;
            }

            auto json_escape_ = [](std::string_view s) -> std::string {
                std::string out;
                out.reserve(s.size() + 8);
                for (const char ch : s) {
                    switch (ch) {
                        case '\"': out += "\\\""; break;
                        case '\\': out += "\\\\"; break;
                        case '\b': out += "\\b"; break;
                        case '\f': out += "\\f"; break;
                        case '\n': out += "\\n"; break;
                        case '\r': out += "\\r"; break;
                        case '\t': out += "\\t"; break;
                        default:
                            if (static_cast<unsigned char>(ch) < 0x20) {
                                std::ostringstream oss;
                                oss << "\\u" << std::hex << std::uppercase
                                    << std::setw(4) << std::setfill('0')
                                    << static_cast<int>(static_cast<unsigned char>(ch));
                                out += oss.str();
                            } else {
                                out.push_back(ch);
                            }
                            break;
                    }
                }
                return out;
            };

            auto severity_name_ = [](parus::diag::Severity sev) -> const char* {
                switch (sev) {
                    case parus::diag::Severity::kWarning: return "warning";
                    case parus::diag::Severity::kFatal: return "fatal";
                    case parus::diag::Severity::kError:
                    default:
                        return "error";
                }
            };

            std::cerr << "[\n";
            bool first = true;
            for (const auto& d : bag.diags()) {
                const auto sp = d.span();
                const uint32_t end_off = (sp.hi >= sp.lo) ? sp.hi : sp.lo;
                const auto begin_lc = sm.line_col(sp.file_id, sp.lo);
                const auto end_lc = sm.line_col(sp.file_id, end_off);

                if (!first) std::cerr << ",\n";
                first = false;

                std::cerr << "  {";
                std::cerr << "\"severity\":\"" << severity_name_(d.severity()) << "\",";
                std::cerr << "\"code\":\"" << json_escape_(parus::diag::code_name(d.code())) << "\",";
                std::cerr << "\"message\":\"" << json_escape_(parus::diag::render_message(d, lang)) << "\",";
                std::cerr << "\"file\":\"" << json_escape_(sm.name(sp.file_id)) << "\",";
                std::cerr << "\"line\":" << begin_lc.line << ",";
                std::cerr << "\"col\":" << begin_lc.col << ",";
                std::cerr << "\"end_line\":" << end_lc.line << ",";
                std::cerr << "\"end_col\":" << end_lc.col << ",";
                std::cerr << "\"args\":[";
                for (size_t i = 0; i < d.args().size(); ++i) {
                    if (i != 0) std::cerr << ",";
                    std::cerr << "\"" << json_escape_(d.args()[i]) << "\"";
                }
                std::cerr << "],";
                std::cerr << "\"labels\":[";
                for (size_t i = 0; i < d.labels().size(); ++i) {
                    const auto& label = d.labels()[i];
                    const auto lsp = label.span;
                    const uint32_t lend_off = (lsp.hi >= lsp.lo) ? lsp.hi : lsp.lo;
                    const auto lbegin_lc = sm.line_col(lsp.file_id, lsp.lo);
                    const auto lend_lc = sm.line_col(lsp.file_id, lend_off);
                    const uint32_t lstart_line0 = (lbegin_lc.line > 0) ? (lbegin_lc.line - 1) : 0;
                    const uint32_t lstart_col0 = (lbegin_lc.col > 0) ? (lbegin_lc.col - 1) : 0;
                    const uint32_t lend_line0 = (lend_lc.line > 0) ? (lend_lc.line - 1) : 0;
                    const uint32_t lend_col0 = (lend_lc.col > 0) ? (lend_lc.col - 1) : 0;
                    if (i != 0) std::cerr << ",";
                    std::cerr << "{";
                    std::cerr << "\"file\":\"" << json_escape_(sm.name(lsp.file_id)) << "\",";
                    std::cerr << "\"line\":" << lbegin_lc.line << ",";
                    std::cerr << "\"col\":" << lbegin_lc.col << ",";
                    std::cerr << "\"end_line\":" << lend_lc.line << ",";
                    std::cerr << "\"end_col\":" << lend_lc.col << ",";
                    std::cerr << "\"message\":\"" << json_escape_(label.message) << "\",";
                    std::cerr << "\"range\":{"
                                 "\"start\":{\"line\":" << lstart_line0 << ",\"character\":" << lstart_col0 << "},"
                                 "\"end\":{\"line\":" << lend_line0 << ",\"character\":" << lend_col0 << "}"
                              << "}";
                    std::cerr << "}";
                }
                std::cerr << "],";
                std::cerr << "\"notes\":[";
                for (size_t i = 0; i < d.notes().size(); ++i) {
                    if (i != 0) std::cerr << ",";
                    std::cerr << "\"" << json_escape_(d.notes()[i]) << "\"";
                }
                std::cerr << "],";
                std::cerr << "\"help\":[";
                for (size_t i = 0; i < d.help().size(); ++i) {
                    if (i != 0) std::cerr << ",";
                    std::cerr << "\"" << json_escape_(d.help()[i]) << "\"";
                }
                std::cerr << "],";

                const uint32_t start_line0 = (begin_lc.line > 0) ? (begin_lc.line - 1) : 0;
                const uint32_t start_col0 = (begin_lc.col > 0) ? (begin_lc.col - 1) : 0;
                const uint32_t end_line0 = (end_lc.line > 0) ? (end_lc.line - 1) : 0;
                const uint32_t end_col0 = (end_lc.col > 0) ? (end_lc.col - 1) : 0;
                std::cerr << "\"range\":{"
                             "\"start\":{\"line\":" << start_line0 << ",\"character\":" << start_col0 << "},"
                             "\"end\":{\"line\":" << end_line0 << ",\"character\":" << end_col0 << "}"
                          << "}";

                std::cerr << "}";
            }
            std::cerr << "\n]\n";
            return bag.has_error() ? 1 : 0;
        }

        void add_template_sidecar_diag_(
            parus::diag::Bag& bag,
            parus::Span span,
            std::string_view detail
        ) {
            using parus::diag::Code;
            Code code = Code::kTemplateSidecarSchema;
            if (detail.starts_with("unsupported helper actor dependency closure") ||
                detail.starts_with("unsupported mutable global dependency closure") ||
                detail.starts_with("unsupported helper class dependency closure")) {
                code = Code::kTemplateSidecarUnsupportedClosure;
            } else if (detail.find("missing valid root stmt") != std::string_view::npos) {
                code = Code::kTemplateSidecarMissingNode;
            } else if (detail.starts_with("conflicting canonical template-sidecar identity")) {
                code = Code::kTemplateSidecarConflictingNode;
            } else if (detail.find("template is unavailable") != std::string_view::npos) {
                code = Code::kTemplateSidecarUnavailable;
            }

            parus::diag::Diagnostic d(parus::diag::Severity::kError, code, span);
            d.add_arg(detail);
            d.add_label(span, "while loading imported generic materialization data");

            if (code == Code::kTemplateSidecarUnsupportedClosure) {
                const size_t colon = detail.find(": ");
                if (colon != std::string_view::npos && colon + 2 < detail.size()) {
                    d.add_note("dependency chain: " + std::string(detail.substr(colon + 2)));
                } else {
                    d.add_note("producer-side closure validation rejected this dependency graph");
                }
                if (detail.starts_with("unsupported mutable global dependency closure")) {
                    d.add_help("remove the mutable global/static dependency from the exported generic closure, or route the behavior through an allowed helper boundary");
                } else {
                    d.add_help("replace the unsupported helper dependency with a free function, struct, enum, or immutable helper class body");
                }
            } else if (code == Code::kTemplateSidecarMissingNode) {
                d.add_note("consumer-side closure loading expected a root or dependency node that was not present in the sidecar");
                d.add_help("rebuild and reinstall the producer bundle so its export-index and adjacent .templates.json stay in sync");
            } else if (code == Code::kTemplateSidecarConflictingNode) {
                d.add_note("sidecar dedup uses canonical template identity, so conflicting payloads indicate a broken merge or stale install");
                d.add_help("clean stale export-index/template-sidecar artifacts and rebuild the producer bundle");
            } else if (code == Code::kTemplateSidecarUnavailable) {
                d.add_note("external generic materialization requires the adjacent typed template-sidecar file");
                d.add_help("load the producer bundle's export-index together with its .templates.json sidecar");
            } else {
                if (detail.find("root kind is unsupported") != std::string_view::npos) {
                    d.add_note("consumer-side validator found a node kind that does not match the expected template declaration kind");
                } else if (detail.find("out of bounds") != std::string_view::npos ||
                           detail.find("failed to reconstruct root decl") != std::string_view::npos) {
                    d.add_note("consumer-side validator found malformed typed payload data while rebuilding the imported template");
                }
                d.add_note("template-sidecar loading failed before monomorphization could begin");
                d.add_help("verify the sidecar schema/version and rebuild the producer bundle if needed");
            }
            bag.add(std::move(d));
        }

        /// @brief SourceManager를 사용해 토큰 스트림을 생성한다.
        std::vector<parus::Token> lex_with_sm_(
            parus::SourceManager& sm,
            uint32_t file_id,
            parus::diag::Bag* bag
        ) {
            parus::Lexer lex(sm.content(file_id), file_id, bag);
            return lex.lex_all();
        }

        struct ExportSurfaceEntry {
            parus::sema::SymbolKind kind = parus::sema::SymbolKind::kVar;
            std::string kind_text{};
            std::string path{};
            std::string link_name{};
            std::string module_head{};
            std::string decl_dir{};
            std::string type_repr{};
            std::string type_semantic{};
            std::string inst_payload{};
            std::string decl_file{};
            uint32_t decl_line = 1;
            uint32_t decl_col = 1;
            std::string decl_bundle{};
            bool is_export = false;
        };

        struct TemplateSidecarArg {
            uint8_t kind = 0;
            bool has_label = false;
            bool is_hole = false;
            std::string label{};
            uint32_t expr = parus::ast::k_invalid_expr;
        };

        struct TemplateSidecarFieldInit {
            std::string name{};
            uint32_t expr = parus::ast::k_invalid_expr;
        };

        struct TemplateSidecarFieldMember {
            std::string name{};
            std::string type_repr{};
            std::string type_semantic{};
            uint8_t visibility = 0;
        };

        struct TemplateSidecarEnumVariant {
            std::string name{};
            uint32_t payload_begin = 0;
            uint32_t payload_count = 0;
            bool has_discriminant = false;
            int64_t discriminant = 0;
        };

        struct TemplateSidecarFStringPart {
            bool is_expr = false;
            std::string text{};
            uint32_t expr = parus::ast::k_invalid_expr;
        };

        struct TemplateSidecarSwitchCase {
            bool is_default = false;
            uint8_t pat_kind = 0;
            std::string pat_text{};
            std::string enum_type_repr{};
            std::string enum_type_semantic{};
            std::string enum_variant_name{};
            uint32_t enum_bind_begin = 0;
            uint32_t enum_bind_count = 0;
            uint32_t body = parus::ast::k_invalid_stmt;
        };

        struct TemplateSidecarSwitchEnumBind {
            std::string field_name{};
            std::string bind_name{};
            std::string bind_type_repr{};
            std::string bind_type_semantic{};
        };

        struct TemplateSidecarGenericParam {
            std::string name{};
        };

        struct TemplateSidecarFnConstraint {
            uint8_t kind = 0;
            std::string type_param{};
            std::string rhs_type_repr{};
            parus::tyck::ImportedProtoIdentity proto{};
        };

        struct TemplateSidecarPathRef {
            std::string path{};
            std::string type_repr{};
            std::string type_semantic{};
        };

        struct TemplateSidecarActsAssocWitness {
            std::string assoc_name{};
            std::string rhs_type_repr{};
            std::string rhs_type_semantic{};
        };

        struct TemplateSidecarParam {
            std::string name{};
            std::string type_repr{};
            std::string type_semantic{};
            bool is_mut = false;
            bool is_self = false;
            uint8_t self_kind = 0;
            bool has_default = false;
            uint32_t default_expr = parus::ast::k_invalid_expr;
            bool is_named_group = false;
        };

        struct TemplateSidecarExpr {
            uint8_t kind = 0;
            uint8_t op = 0;
            uint32_t a = parus::ast::k_invalid_expr;
            uint32_t b = parus::ast::k_invalid_expr;
            uint32_t c = parus::ast::k_invalid_expr;
            bool unary_is_mut = false;
            std::string text{};
            bool string_is_raw = false;
            bool string_is_format = false;
            uint32_t string_part_begin = 0;
            uint32_t string_part_count = 0;
            std::string string_folded_text{};
            uint32_t arg_begin = 0;
            uint32_t arg_count = 0;
            uint32_t call_type_arg_begin = 0;
            uint32_t call_type_arg_count = 0;
            bool call_from_pipe = false;
            uint32_t field_init_begin = 0;
            uint32_t field_init_count = 0;
            std::string field_init_type_repr{};
            std::string field_init_type_semantic{};
            uint32_t block_stmt = parus::ast::k_invalid_stmt;
            uint32_t block_tail = parus::ast::k_invalid_expr;
            bool loop_has_header = false;
            std::string loop_var{};
            uint32_t loop_iter = parus::ast::k_invalid_expr;
            uint32_t loop_body = parus::ast::k_invalid_stmt;
            std::string cast_type_repr{};
            std::string cast_type_semantic{};
            uint8_t cast_kind = 0;
            std::string target_type_repr{};
            std::string target_type_semantic{};
        };

        struct TemplateSidecarStmt {
            uint8_t kind = 0;
            uint32_t expr = parus::ast::k_invalid_expr;
            uint32_t init = parus::ast::k_invalid_expr;
            uint32_t a = parus::ast::k_invalid_stmt;
            uint32_t b = parus::ast::k_invalid_stmt;
            uint32_t stmt_begin = 0;
            uint32_t stmt_count = 0;
            uint32_t case_begin = 0;
            uint32_t case_count = 0;
            bool has_default = false;
            bool is_set = false;
            bool is_mut = false;
            bool is_static = false;
            bool is_const = false;
            bool is_extern = false;
            uint8_t link_abi = 0;
            std::string name{};
            std::string type_repr{};
            std::string type_semantic{};
            bool is_export = false;
            uint8_t fn_mode = 0;
            std::string fn_ret_repr{};
            std::string fn_ret_semantic{};
            uint8_t member_visibility = 0;
            bool is_pure = false;
            bool is_comptime = false;
            bool is_commit = false;
            bool is_recast = false;
            bool is_throwing = false;
            bool fn_is_const = false;
            uint32_t param_begin = 0;
            uint32_t param_count = 0;
            uint32_t positional_param_count = 0;
            bool has_named_group = false;
            bool fn_is_c_variadic = false;
            bool fn_is_proto_sig = false;
            uint32_t fn_generic_param_begin = 0;
            uint32_t fn_generic_param_count = 0;
            uint32_t fn_constraint_begin = 0;
            uint32_t fn_constraint_count = 0;
            uint32_t decl_generic_param_begin = 0;
            uint32_t decl_generic_param_count = 0;
            uint32_t decl_constraint_begin = 0;
            uint32_t decl_constraint_count = 0;
            uint32_t decl_path_ref_begin = 0;
            uint32_t decl_path_ref_count = 0;
            uint8_t field_layout = 0;
            uint32_t field_align = 0;
            uint32_t field_member_begin = 0;
            uint32_t field_member_count = 0;
            uint32_t enum_variant_begin = 0;
            uint32_t enum_variant_count = 0;
            uint8_t proto_fn_role = 0;
            uint8_t proto_require_kind = 0;
            uint8_t assoc_type_role = 0;
            bool var_is_proto_provide = false;
            bool acts_is_for = false;
            bool acts_has_set_name = false;
            std::string acts_target_type_repr{};
            std::string acts_target_type_semantic{};
            uint32_t acts_assoc_witness_begin = 0;
            uint32_t acts_assoc_witness_count = 0;
            uint8_t manual_perm_mask = 0;
            bool var_has_consume_else = false;
        };

        struct TemplateSidecarFunction {
            std::string bundle{};
            std::string module_head{};
            std::string public_path{};
            std::string link_name{};
            std::string lookup_name{};
            std::string decl_file{};
            uint32_t decl_line = 1;
            uint32_t decl_col = 1;
            bool is_public_export = false;
            std::string declared_type_repr{};
            std::string declared_type_semantic{};
            std::vector<TemplateSidecarStmt> stmts{};
            std::vector<uint32_t> stmt_children{};
            std::vector<TemplateSidecarExpr> exprs{};
            std::vector<TemplateSidecarParam> params{};
            std::vector<TemplateSidecarSwitchCase> switch_cases{};
            std::vector<TemplateSidecarSwitchEnumBind> switch_enum_binds{};
            std::vector<TemplateSidecarArg> args{};
            std::vector<TemplateSidecarFieldInit> field_inits{};
            std::vector<TemplateSidecarFieldMember> field_members{};
            std::vector<TemplateSidecarEnumVariant> enum_variants{};
            std::vector<TemplateSidecarFStringPart> fstring_parts{};
            std::vector<std::string> type_args{};
            std::vector<TemplateSidecarGenericParam> generic_params{};
            std::vector<TemplateSidecarFnConstraint> constraints{};
            std::vector<TemplateSidecarPathRef> path_refs{};
            std::vector<TemplateSidecarActsAssocWitness> acts_assoc_witnesses{};
            uint32_t root_stmt = parus::ast::k_invalid_stmt;
        };

        struct LoadedExternalIndex {
            std::string export_index_path{};
            std::string bundle{};
            std::vector<ExportSurfaceEntry> entries{};
            std::vector<TemplateSidecarFunction> sidecars{};
        };

        std::string json_escape_text_(std::string_view s) {
            std::string out;
            out.reserve(s.size() + 8);
            for (const char ch : s) {
                switch (ch) {
                    case '\"': out += "\\\""; break;
                    case '\\': out += "\\\\"; break;
                    case '\b': out += "\\b"; break;
                    case '\f': out += "\\f"; break;
                    case '\n': out += "\\n"; break;
                    case '\r': out += "\\r"; break;
                    case '\t': out += "\\t"; break;
                    default:
                        if (static_cast<unsigned char>(ch) < 0x20) {
                            std::ostringstream oss;
                            oss << "\\u" << std::hex << std::uppercase
                                << std::setw(4) << std::setfill('0')
                                << static_cast<int>(static_cast<unsigned char>(ch));
                            out += oss.str();
                        } else {
                            out.push_back(ch);
                        }
                        break;
                }
            }
            return out;
        }

        std::string template_sidecar_path_(std::string_view export_index_path) {
            std::string out(export_index_path);
            constexpr std::string_view kSuffix = ".exports.json";
            if (out.size() >= kSuffix.size() &&
                std::string_view(out).substr(out.size() - kSuffix.size()) == kSuffix) {
                out.replace(out.size() - kSuffix.size(), kSuffix.size(), ".templates.json");
                return out;
            }
            out += ".templates.json";
            return out;
        }

        std::string build_enum_decl_payload_(
            const parus::ast::AstArena& ast,
            const parus::ast::Stmt& s,
            const parus::ty::TypePool& types
        );

        std::string build_field_decl_payload_(
            const parus::ast::AstArena& ast,
            const parus::ast::Stmt& s,
            const parus::ty::TypePool& types
        );

        bool json_unescape_text_(std::string_view in, std::string& out) {
            auto hex_value_ = [](char c) -> int {
                if (c >= '0' && c <= '9') return c - '0';
                if (c >= 'a' && c <= 'f') return 10 + (c - 'a');
                if (c >= 'A' && c <= 'F') return 10 + (c - 'A');
                return -1;
            };
            auto parse_hex4_ = [&](size_t pos, uint16_t& value) -> bool {
                if (pos + 4 > in.size()) return false;
                int h0 = hex_value_(in[pos + 0]);
                int h1 = hex_value_(in[pos + 1]);
                int h2 = hex_value_(in[pos + 2]);
                int h3 = hex_value_(in[pos + 3]);
                if (h0 < 0 || h1 < 0 || h2 < 0 || h3 < 0) return false;
                value = static_cast<uint16_t>((h0 << 12) | (h1 << 8) | (h2 << 4) | h3);
                return true;
            };
            auto append_utf8_ = [](std::string& dst, uint32_t cp) {
                if (cp <= 0x7F) {
                    dst.push_back(static_cast<char>(cp));
                } else if (cp <= 0x7FF) {
                    dst.push_back(static_cast<char>(0xC0 | ((cp >> 6) & 0x1F)));
                    dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else if (cp <= 0xFFFF) {
                    dst.push_back(static_cast<char>(0xE0 | ((cp >> 12) & 0x0F)));
                    dst.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                } else {
                    dst.push_back(static_cast<char>(0xF0 | ((cp >> 18) & 0x07)));
                    dst.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
                    dst.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
                    dst.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
                }
            };

            out.clear();
            out.reserve(in.size());
            for (size_t i = 0; i < in.size(); ++i) {
                const char c = in[i];
                if (c != '\\') {
                    out.push_back(c);
                    continue;
                }
                if (i + 1 >= in.size()) return false;
                const char n = in[++i];
                switch (n) {
                    case '\"': out.push_back('\"'); break;
                    case '\\': out.push_back('\\'); break;
                    case '/': out.push_back('/'); break;
                    case 'b': out.push_back('\b'); break;
                    case 'f': out.push_back('\f'); break;
                    case 'n': out.push_back('\n'); break;
                    case 'r': out.push_back('\r'); break;
                    case 't': out.push_back('\t'); break;
                    case 'u': {
                        uint16_t hi = 0;
                        if (!parse_hex4_(i + 1, hi)) return false;
                        i += 4;

                        uint32_t cp = hi;
                        if (hi >= 0xD800u && hi <= 0xDBFFu) {
                            if (i + 6 >= in.size()) return false;
                            if (in[i + 1] != '\\' || in[i + 2] != 'u') return false;
                            uint16_t lo = 0;
                            if (!parse_hex4_(i + 3, lo)) return false;
                            if (lo < 0xDC00u || lo > 0xDFFFu) return false;
                            cp = 0x10000u + (((uint32_t)hi - 0xD800u) << 10) + ((uint32_t)lo - 0xDC00u);
                            i += 6;
                        } else if (hi >= 0xDC00u && hi <= 0xDFFFu) {
                            return false;
                        }

                        append_utf8_(out, cp);
                        break;
                    }
                    default:
                        return false;
                }
            }
            return true;
        }

        std::string symbol_kind_to_text_(parus::sema::SymbolKind kind) {
            using K = parus::sema::SymbolKind;
            switch (kind) {
                case K::kFn: return "fn";
                case K::kVar: return "var";
                case K::kField: return "field";
                case K::kAct: return "act";
                case K::kType: return "type";
                case K::kInst: return "inst";
                default: return "var";
            }
        }

        std::optional<parus::sema::SymbolKind> symbol_kind_from_text_(std::string_view s) {
            using K = parus::sema::SymbolKind;
            if (s == "fn") return K::kFn;
            if (s == "var") return K::kVar;
            if (s == "field") return K::kField;
            if (s == "act") return K::kAct;
            if (s == "type") return K::kType;
            if (s == "inst") return K::kInst;
            return std::nullopt;
        }

        std::string qualify_name_(const std::vector<std::string>& ns, std::string_view base) {
            if (base.empty()) return {};
            if (ns.empty()) return std::string(base);
            std::string out{};
            for (size_t i = 0; i < ns.size(); ++i) {
                if (i) out += "::";
                out += ns[i];
            }
            out += "::";
            out += std::string(base);
            return out;
        }

        std::string payload_escape_value_(std::string_view raw) {
            static constexpr char kHex[] = "0123456789ABCDEF";
            std::string out{};
            out.reserve(raw.size());
            for (const unsigned char ch : raw) {
                const bool safe =
                    (ch >= 'a' && ch <= 'z') ||
                    (ch >= 'A' && ch <= 'Z') ||
                    (ch >= '0' && ch <= '9') ||
                    ch == '_' || ch == '-' || ch == '.' || ch == ':' || ch == '<' || ch == '>';
                if (safe) {
                    out.push_back(static_cast<char>(ch));
                } else {
                    out.push_back('%');
                    out.push_back(kHex[(ch >> 4) & 0xF]);
                    out.push_back(kHex[ch & 0xF]);
                }
            }
            return out;
        }

        std::string payload_unescape_value_(std::string_view raw) {
            auto hex_value = [](char ch) -> int {
                if (ch >= '0' && ch <= '9') return ch - '0';
                if (ch >= 'a' && ch <= 'f') return 10 + (ch - 'a');
                if (ch >= 'A' && ch <= 'F') return 10 + (ch - 'A');
                return -1;
            };

            std::string out{};
            out.reserve(raw.size());
            for (size_t i = 0; i < raw.size(); ++i) {
                if (raw[i] == '%' && i + 2 < raw.size()) {
                    const int hi = hex_value(raw[i + 1]);
                    const int lo = hex_value(raw[i + 2]);
                    if (hi >= 0 && lo >= 0) {
                        out.push_back(static_cast<char>((hi << 4) | lo));
                        i += 2;
                        continue;
                    }
                }
                out.push_back(static_cast<char>(raw[i]));
            }
            return out;
        }

        void append_generic_decl_payload_(
            std::string& payload,
            const parus::ast::AstArena& ast,
            const parus::ast::Stmt& s,
            const parus::ty::TypePool& types
        ) {
            std::vector<std::string> owner_generic_params{};
            if (s.kind == parus::ast::StmtKind::kActsDecl &&
                s.acts_is_for &&
                s.acts_target_type != parus::ty::kInvalidType) {
                std::vector<std::string_view> owner_path{};
                std::vector<parus::ty::TypeId> owner_args{};
                if (types.decompose_named_user(s.acts_target_type, owner_path, owner_args) && !owner_args.empty()) {
                    owner_generic_params.reserve(owner_args.size());
                    for (const auto arg_t : owner_args) {
                        if (arg_t == parus::ty::kInvalidType || arg_t >= types.count()) continue;
                        std::vector<std::string_view> arg_path{};
                        std::vector<parus::ty::TypeId> arg_args{};
                        if (!types.decompose_named_user(arg_t, arg_path, arg_args) ||
                            arg_path.size() != 1 ||
                            !arg_args.empty()) {
                            continue;
                        }
                        owner_generic_params.emplace_back(arg_path.front());
                    }
                }
            }

            const uint32_t gp_begin =
                (s.kind == parus::ast::StmtKind::kFnDecl) ? s.fn_generic_param_begin : s.decl_generic_param_begin;
            const uint32_t gp_count =
                (s.kind == parus::ast::StmtKind::kFnDecl) ? s.fn_generic_param_count : s.decl_generic_param_count;
            const uint32_t cc_begin =
                (s.kind == parus::ast::StmtKind::kFnDecl) ? s.fn_constraint_begin : s.decl_constraint_begin;
            const uint32_t cc_count =
                (s.kind == parus::ast::StmtKind::kFnDecl) ? s.fn_constraint_count : s.decl_constraint_count;

            if (gp_count == 0 && cc_count == 0 && owner_generic_params.empty()) return;

            if (payload.empty()) {
                payload = "parus_generic_decl";
            }
            const auto& gps = ast.generic_param_decls();
            const auto& ccs = ast.fn_constraint_decls();
            std::unordered_set<std::string> emitted_params{};

            for (const auto& name : owner_generic_params) {
                if (!emitted_params.insert(name).second) continue;
                payload += "|gparam=";
                payload += payload_escape_value_(name);
            }

            for (uint32_t i = 0; i < gp_count; ++i) {
                const uint32_t idx = gp_begin + i;
                if (idx >= gps.size()) break;
                if (!emitted_params.insert(std::string(gps[idx].name)).second) continue;
                payload += "|gparam=";
                payload += payload_escape_value_(gps[idx].name);
            }

            for (uint32_t i = 0; i < cc_count; ++i) {
                const uint32_t idx = cc_begin + i;
                if (idx >= ccs.size()) break;
                const auto& cc = ccs[idx];
                payload += "|gconstraint=";
                if (cc.kind == parus::ast::FnConstraintKind::kProto) {
                    payload += "proto,";
                    payload += payload_escape_value_(cc.type_param);
                    payload += ",";
                    payload += payload_escape_value_(
                        (cc.rhs_type != parus::ty::kInvalidType)
                            ? types.to_export_string(cc.rhs_type)
                            : std::string("<invalid>")
                    );
                } else {
                    payload += "type_eq,";
                    payload += payload_escape_value_(cc.type_param);
                    payload += ",";
                    payload += payload_escape_value_(
                        (cc.rhs_type != parus::ty::kInvalidType)
                            ? types.to_export_string(cc.rhs_type)
                            : std::string("<invalid>")
                    );
                }
            }
        }

        void append_type_impl_proto_payload_(
            std::string& payload,
            const parus::ast::AstArena& ast,
            const parus::ast::Stmt& s,
            const parus::ty::TypePool& types
        ) {
            const auto& refs = ast.path_refs();
            const uint64_t begin = s.decl_path_ref_begin;
            const uint64_t end = begin + s.decl_path_ref_count;
            if (!(begin <= refs.size() && end <= refs.size())) return;

            for (uint32_t i = s.decl_path_ref_begin; i < s.decl_path_ref_begin + s.decl_path_ref_count; ++i) {
                const auto& pr = refs[i];
                if (pr.type == parus::ty::kInvalidType) continue;
                if (payload.empty()) {
                    payload = "parus_type_decl";
                }
                payload += "|impl_proto=";
                payload += payload_escape_value_(types.to_export_string(pr.type));
                const auto semantic =
                    parus::cimport::serialize_type_semantic_from_type(pr.type, types);
                if (!semantic.empty()) {
                    payload += "@";
                    payload += payload_escape_value_(semantic);
                }
            }
        }

        void append_type_assoc_binding_payload_(
            std::string& payload,
            const parus::ast::AstArena& ast,
            const parus::ast::Stmt& s,
            const parus::ty::TypePool& types
        ) {
            if (s.type == parus::ty::kInvalidType) return;
            const std::string owner_repr = types.to_export_string(s.type);
            if (owner_repr.empty()) return;

            const auto& stmts = ast.stmts();
            std::unordered_set<std::string> emitted{};
            std::unordered_set<uint32_t> emitted_constraint_stmts{};

            const auto append_constraint_payload_only_ = [&](
                const parus::ast::Stmt& decl
            ) {
                const uint32_t cc_begin =
                    (decl.kind == parus::ast::StmtKind::kFnDecl) ? decl.fn_constraint_begin : decl.decl_constraint_begin;
                const uint32_t cc_count =
                    (decl.kind == parus::ast::StmtKind::kFnDecl) ? decl.fn_constraint_count : decl.decl_constraint_count;
                if (cc_count == 0) return;
                const auto& ccs = ast.fn_constraint_decls();
                for (uint32_t i = 0; i < cc_count; ++i) {
                    const uint32_t idx = cc_begin + i;
                    if (idx >= ccs.size()) break;
                    const auto& cc = ccs[idx];
                    payload += "|gconstraint=";
                    if (cc.kind == parus::ast::FnConstraintKind::kProto) {
                        payload += "proto,";
                        payload += payload_escape_value_(cc.type_param);
                        payload += ",";
                        payload += payload_escape_value_(
                            (cc.rhs_type != parus::ty::kInvalidType)
                                ? types.to_export_string(cc.rhs_type)
                                : std::string("<invalid>")
                        );
                    } else {
                        payload += "type_eq,";
                        payload += payload_escape_value_(cc.type_param);
                        payload += ",";
                        payload += payload_escape_value_(
                            (cc.rhs_type != parus::ty::kInvalidType)
                                ? types.to_export_string(cc.rhs_type)
                                : std::string("<invalid>")
                        );
                    }
                }
            };

            const auto acts_owner_matches = [&](const parus::ast::Stmt& acts) -> bool {
                if (acts.acts_target_type == parus::ty::kInvalidType) return false;
                if (types.to_export_string(acts.acts_target_type) == owner_repr) return true;

                std::vector<std::string_view> owner_path{};
                std::vector<parus::ty::TypeId> owner_args{};
                if (!types.decompose_named_user(acts.acts_target_type, owner_path, owner_args) ||
                    owner_path.empty()) {
                    return false;
                }
                return owner_path.back() == s.name;
            };

            for (const auto& acts : stmts) {
                if (acts.kind != parus::ast::StmtKind::kActsDecl || !acts.acts_is_for) continue;
                if (!acts_owner_matches(acts)) continue;

                bool emitted_from_this_acts = false;
                const auto& witnesses = ast.acts_assoc_type_witness_decls();
                const uint64_t wbegin = acts.acts_assoc_witness_begin;
                const uint64_t wend = wbegin + acts.acts_assoc_witness_count;
                if (!(wbegin <= witnesses.size() && wend <= witnesses.size())) continue;

                for (uint32_t i = 0; i < acts.acts_assoc_witness_count; ++i) {
                    const auto& witness = witnesses[acts.acts_assoc_witness_begin + i];
                    if (witness.assoc_name.empty() || witness.rhs_type == parus::ty::kInvalidType) continue;

                    std::string body = std::string(witness.assoc_name);
                    body += ",";
                    body += types.to_export_string(witness.rhs_type);
                    const auto semantic =
                        parus::cimport::serialize_type_semantic_from_type(witness.rhs_type, types);
                    if (!semantic.empty()) {
                        body += "@";
                        body += semantic;
                    }
                    if (!emitted.insert(body).second) continue;

                    if (payload.empty()) {
                        payload = "parus_type_decl";
                    }
                    if (!emitted_from_this_acts &&
                        emitted_constraint_stmts.insert(&acts - stmts.data()).second) {
                        append_constraint_payload_only_(acts);
                    }
                    payload += "|assoc_type=";
                    payload += body;
                    emitted_from_this_acts = true;
                }
            }
        }

        void dedupe_export_surface_(std::vector<ExportSurfaceEntry>& entries) {
            std::sort(entries.begin(), entries.end(), [](const ExportSurfaceEntry& a, const ExportSurfaceEntry& b) {
                return std::tie(a.kind_text, a.path, a.link_name, a.module_head, a.decl_dir,
                                a.type_repr, a.type_semantic, a.inst_payload,
                                a.decl_file, a.decl_line, a.decl_col, a.decl_bundle, a.is_export)
                     < std::tie(b.kind_text, b.path, b.link_name, b.module_head, b.decl_dir,
                                b.type_repr, b.type_semantic, b.inst_payload,
                                b.decl_file, b.decl_line, b.decl_col, b.decl_bundle, b.is_export);
            });
            entries.erase(std::unique(entries.begin(), entries.end(), [](const ExportSurfaceEntry& a, const ExportSurfaceEntry& b) {
                return std::tie(a.kind_text, a.path, a.link_name, a.module_head, a.decl_dir,
                                a.type_repr, a.type_semantic, a.inst_payload,
                                a.decl_file, a.decl_line, a.decl_col, a.decl_bundle, a.is_export)
                    == std::tie(b.kind_text, b.path, b.link_name, b.module_head, b.decl_dir,
                                b.type_repr, b.type_semantic, b.inst_payload,
                                b.decl_file, b.decl_line, b.decl_col, b.decl_bundle, b.is_export);
            }), entries.end());
        }

        bool core_builtin_use_export_(std::string_view module_head,
                                      std::string_view name,
                                      std::string& out_type_repr,
                                      std::string& out_payload) {
            static const std::unordered_set<std::string> kAllowed = {
                "c_void", "c_char", "c_schar", "c_uchar",
                "c_short", "c_ushort", "c_int", "c_uint",
                "c_long", "c_ulong", "c_longlong", "c_ulonglong",
                "c_float", "c_double",
                "c_size", "c_ssize", "c_ptrdiff",
                "vaList"
            };
            static const std::unordered_set<std::string> kConstraintAllowed = {
                "Comparable",
                "BinaryInteger",
                "SignedInteger",
                "UnsignedInteger",
                "BinaryFloatingPoint",
                "Step",
            };

            if (module_head == "ext" || module_head == "core::ext") {
                if (kAllowed.find(std::string(name)) == kAllowed.end()) return false;
                out_type_repr = "core::ext::";
                out_type_repr += std::string(name);
                out_payload = "parus_core_builtin_use|name=" + std::string(name);
                return true;
            }

            if (module_head == "constraints" || module_head == "core::constraints") {
                if (kConstraintAllowed.find(std::string(name)) == kConstraintAllowed.end()) return false;
                out_type_repr = "core::constraints::";
                out_type_repr += std::string(name);
                out_payload = "parus_decl_kind=proto|parus_core_builtin_use|kind=proto|name=" + std::string(name);
                return true;
            }

            return false;
        }

        std::string parent_dir_norm_(std::string_view path) {
            namespace fs = std::filesystem;
            std::error_code ec{};
            fs::path p(path);
            if (p.is_relative()) p = fs::absolute(p, ec);
            if (ec) {
                ec.clear();
                p = fs::path(path);
            }
            fs::path dir = p.parent_path();
            fs::path canon = fs::weakly_canonical(dir, ec);
            if (!ec && !canon.empty()) {
                return parus::normalize_path(canon.string());
            }
            return parus::normalize_path(dir.string());
        }

        std::string compute_module_head_(
            std::string_view bundle_root,
            std::string_view source_path,
            std::string_view bundle_name
        ) {
            return parus::compute_module_head(bundle_root, source_path, bundle_name);
        }

        std::string normalize_import_head_top_(std::string_view import_head) {
            return parus::normalize_import_head_top(import_head);
        }

        std::string normalize_core_public_module_head_(
            std::string_view bundle_name,
            std::string_view module_head
        ) {
            return parus::normalize_core_public_module_head(bundle_name, module_head);
        }

        std::string normalize_symbol_fragment_(std::string_view in) {
            std::string out;
            out.reserve(in.size());
            for (const char c : in) {
                const unsigned char u = static_cast<unsigned char>(c);
                if (std::isalnum(u) || c == '_') out.push_back(c);
                else out.push_back('_');
            }
            if (out.empty()) out = "_";
            return out;
        }

        uint64_t fnv1a64_(std::string_view s) {
            uint64_t h = 1469598103934665603ull;
            for (const char c : s) {
                h ^= static_cast<unsigned char>(c);
                h *= 1099511628211ull;
            }
            return h;
        }

        std::string build_function_link_name_(
            std::string_view bundle_name,
            std::string_view qname,
            parus::ast::FnMode fn_mode,
            std::string_view sig_repr,
            bool is_c_abi
        ) {
            if (qname.empty()) return {};
            if (is_c_abi) {
                const size_t pos = qname.rfind("::");
                return (pos == std::string_view::npos)
                    ? std::string(qname)
                    : std::string(qname.substr(pos + 2));
            }

            std::string path = "_";
            std::string base = std::string(qname);
            if (const size_t pos = base.rfind("::"); pos != std::string::npos) {
                path = base.substr(0, pos);
                base = base.substr(pos + 2);
                size_t p = 0;
                while ((p = path.find("::", p)) != std::string::npos) {
                    path.replace(p, 2, "__");
                    p += 2;
                }
            }

            std::string mode = "none";
            switch (fn_mode) {
                case parus::ast::FnMode::kPub: mode = "pub"; break;
                case parus::ast::FnMode::kSub: mode = "sub"; break;
                case parus::ast::FnMode::kNone: default: mode = "none"; break;
            }

            const std::string bundle = bundle_name.empty() ? std::string("main") : std::string(bundle_name);
            const std::string sig = sig_repr.empty() ? std::string("def(?)") : std::string(sig_repr);
            const std::string canonical =
                "bundle=" + bundle + "|path=" + path +
                "|name=" + base +
                "|mode=" + mode +
                "|recv=none|sig=" + sig;

            std::ostringstream hs;
            hs << std::hex << fnv1a64_(canonical);

            return "p$" + normalize_symbol_fragment_(bundle) + "$" +
                normalize_symbol_fragment_(path) + "$" +
                normalize_symbol_fragment_(base) + "$M" +
                normalize_symbol_fragment_(mode) + "$Rnone$S" +
                normalize_symbol_fragment_(sig) + "$H" + hs.str();
        }

        void collect_exports_stmt_(
            const parus::ast::AstArena& ast,
            parus::ast::StmtId sid,
            const parus::ty::TypePool& types,
            const parus::SourceManager& sm,
            const std::string& decl_file,
            const std::string& bundle_name,
            const std::string& module_head,
            const std::string& decl_dir,
            std::vector<std::string>& ns,
            std::vector<ExportSurfaceEntry>& out,
            std::optional<uint32_t> only_file_id = std::nullopt
        ) {
            if (sid == parus::ast::k_invalid_stmt || static_cast<size_t>(sid) >= ast.stmts().size()) return;
            const auto& s = ast.stmt(sid);
            parus::ty::TypePool export_types = types;

            auto rewrite_builtin_acts_self_type_ = [&](auto&& self,
                                                       parus::ty::TypeId cur,
                                                       parus::ty::TypeId owner_type) -> parus::ty::TypeId {
                using Kind = parus::ty::Kind;
                if (cur == parus::ty::kInvalidType || cur >= export_types.count()) return cur;

                const auto& tt = export_types.get(cur);
                switch (tt.kind) {
                    case Kind::kNamedUser: {
                        std::vector<std::string_view> path{};
                        std::vector<parus::ty::TypeId> args{};
                        if (!export_types.decompose_named_user(cur, path, args)) return cur;
                        if (path.size() == 1 && path[0] == "Self" && args.empty()) {
                            return owner_type;
                        }
                        bool changed = false;
                        for (auto& arg : args) {
                            const auto next = self(self, arg, owner_type);
                            if (next != arg) {
                                arg = next;
                                changed = true;
                            }
                        }
                        if (!changed) return cur;
                        return export_types.intern_named_path_with_args(
                            path.data(),
                            static_cast<uint32_t>(path.size()),
                            args.empty() ? nullptr : args.data(),
                            static_cast<uint32_t>(args.size())
                        );
                    }
                    case Kind::kOptional: {
                        const auto elem = self(self, tt.elem, owner_type);
                        return elem == tt.elem ? cur : export_types.make_optional(elem);
                    }
                    case Kind::kArray: {
                        const auto elem = self(self, tt.elem, owner_type);
                        return elem == tt.elem ? cur : export_types.make_array(elem, tt.array_has_size, tt.array_size);
                    }
                    case Kind::kBorrow: {
                        const auto elem = self(self, tt.elem, owner_type);
                        return elem == tt.elem ? cur : export_types.make_borrow(elem, tt.borrow_is_mut);
                    }
                    case Kind::kEscape: {
                        const auto elem = self(self, tt.elem, owner_type);
                        return elem == tt.elem ? cur : export_types.make_escape(elem);
                    }
                    case Kind::kPtr: {
                        const auto elem = self(self, tt.elem, owner_type);
                        return elem == tt.elem ? cur : export_types.make_ptr(elem, tt.ptr_is_mut);
                    }
                    case Kind::kFn: {
                        std::vector<parus::ty::TypeId> params{};
                        std::vector<std::string_view> labels{};
                        std::vector<uint8_t> defaults{};
                        params.reserve(tt.param_count);
                        labels.reserve(tt.param_count);
                        defaults.reserve(tt.param_count);
                        bool changed = false;
                        for (uint32_t i = 0; i < tt.param_count; ++i) {
                            const auto p = export_types.fn_param_at(cur, i);
                            const auto np = self(self, p, owner_type);
                            if (np != p) changed = true;
                            params.push_back(np);
                            labels.push_back(export_types.fn_param_label_at(cur, i));
                            defaults.push_back(export_types.fn_param_has_default_at(cur, i) ? 1u : 0u);
                        }
                        const auto nr = self(self, tt.ret, owner_type);
                        if (nr != tt.ret) changed = true;
                        if (!changed) return cur;
                        return export_types.make_fn(
                            nr,
                            params.empty() ? nullptr : params.data(),
                            static_cast<uint32_t>(params.size()),
                            tt.positional_param_count,
                            labels.empty() ? nullptr : labels.data(),
                            defaults.empty() ? nullptr : defaults.data(),
                            tt.fn_is_c_abi,
                            tt.fn_is_c_variadic,
                            tt.fn_callconv
                        );
                    }
                    case Kind::kError:
                    case Kind::kBuiltin:
                    default:
                        return cur;
                }
            };

            if (s.kind == parus::ast::StmtKind::kBlock) {
                const auto& kids = ast.stmt_children();
                const uint64_t begin = s.stmt_begin;
                const uint64_t end = begin + s.stmt_count;
                if (begin <= kids.size() && end <= kids.size()) {
                    for (uint32_t i = 0; i < s.stmt_count; ++i) {
                        collect_exports_stmt_(ast,
                                              kids[s.stmt_begin + i],
                                              types,
                                              sm,
                                              decl_file,
                                              bundle_name,
                                              module_head,
                                              decl_dir,
                                              ns,
                                              out,
                                              only_file_id);
                    }
                }
                return;
            }

            if (s.kind == parus::ast::StmtKind::kNestDecl) {
                if (!s.nest_is_file_directive) {
                    const auto& segs = ast.path_segs();
                    const uint64_t begin = s.nest_path_begin;
                    const uint64_t end = begin + s.nest_path_count;
                    uint32_t pushed = 0;
                    if (begin <= segs.size() && end <= segs.size()) {
                        for (uint32_t i = 0; i < s.nest_path_count; ++i) {
                            ns.push_back(std::string(segs[s.nest_path_begin + i]));
                            ++pushed;
                        }
                    }
                    collect_exports_stmt_(ast,
                                          s.a,
                                          types,
                                          sm,
                                          decl_file,
                                          bundle_name,
                                          module_head,
                                          decl_dir,
                                          ns,
                                          out,
                                          only_file_id);
                    while (pushed > 0) {
                        ns.pop_back();
                        --pushed;
                    }
                }
                return;
            }

            auto push_export = [&](parus::sema::SymbolKind kind,
                                   std::string qname,
                                   parus::ty::TypeId tid,
                                   bool is_export,
                                   parus::Span span,
                                   std::string link_name = {},
                                   std::string inst_payload = {}) {
                if (qname.empty()) return;
                if (only_file_id.has_value() && span.file_id != *only_file_id) return;
                ExportSurfaceEntry e{};
                e.kind = kind;
                e.kind_text = symbol_kind_to_text_(kind);
                e.path = std::move(qname);
                e.link_name = std::move(link_name);
                e.module_head = module_head;
                e.decl_dir = decl_dir;
                if (tid != parus::ty::kInvalidType) {
                    e.type_repr = export_types.to_export_string(tid);
                    e.type_semantic = parus::cimport::serialize_type_semantic_from_type(tid, export_types);
                }
                e.inst_payload = std::move(inst_payload);
                e.decl_file = decl_file;
                const auto lc = sm.line_col(span.file_id, span.lo);
                e.decl_line = lc.line;
                e.decl_col = lc.col;
                e.decl_bundle = bundle_name;
                e.is_export = is_export;
                out.push_back(std::move(e));
            };

            if (s.kind == parus::ast::StmtKind::kFnDecl && !s.name.empty()) {
                const std::string qname = qualify_name_(ns, s.name);
                const std::string sig_repr =
                    (s.type != parus::ty::kInvalidType) ? types.to_export_string(s.type) : std::string("def(?)");
                const std::string link_name = build_function_link_name_(
                    bundle_name,
                    qname,
                    s.fn_mode,
                    sig_repr,
                    s.link_abi == parus::ast::LinkAbi::kC
                );
                std::string inst_payload{};
                if (s.directive_target_path_count == 0 && s.directive_key_path_count == 2) {
                    const auto& segs = ast.path_segs();
                    const uint64_t begin = s.directive_key_path_begin;
                    const uint64_t end = begin + s.directive_key_path_count;
                    if (begin <= segs.size() && end <= segs.size() &&
                        segs[s.directive_key_path_begin] == "Impl") {
                        const std::string_view leaf = segs[s.directive_key_path_begin + 1];
                        if (leaf != "Core") {
                            inst_payload = "parus_impl_binding|key=Impl::";
                            inst_payload += leaf;
                            const bool compiler_owned =
                                (s.a == parus::ast::k_invalid_stmt) &&
                                (leaf == "SpinLoop" || leaf == "StepNext" ||
                                 leaf == "SizeOf" || leaf == "AlignOf");
                            inst_payload += compiler_owned ? "|mode=compiler" : "|mode=library";
                        }
                    }
                }
                append_generic_decl_payload_(inst_payload, ast, s, types);
                if (s.is_throwing && s.link_abi != parus::ast::LinkAbi::kC) {
                    if (!inst_payload.empty()) inst_payload += "|";
                    inst_payload += "throwing=1";
                }
                push_export(parus::sema::SymbolKind::kFn, qname, s.type, s.is_export, s.span, link_name, std::move(inst_payload));
                return;
            }

            if (s.kind == parus::ast::StmtKind::kVar && !s.name.empty()) {
                const bool is_global =
                    s.is_static || s.is_extern || s.is_export || (s.link_abi == parus::ast::LinkAbi::kC);
                if (is_global) {
                    push_export(parus::sema::SymbolKind::kVar, qualify_name_(ns, s.name), s.type, s.is_export, s.span);
                }
                return;
            }

            if (s.kind == parus::ast::StmtKind::kFieldDecl && !s.name.empty()) {
                std::string payload = build_field_decl_payload_(ast, s, types);
                push_export(parus::sema::SymbolKind::kField,
                            qualify_name_(ns, s.name),
                            s.type,
                            s.is_export,
                            s.span,
                            /*link_name=*/{},
                            std::move(payload));
                return;
            }

            if ((s.kind == parus::ast::StmtKind::kEnumDecl ||
                 s.kind == parus::ast::StmtKind::kProtoDecl ||
                 s.kind == parus::ast::StmtKind::kClassDecl ||
                 s.kind == parus::ast::StmtKind::kActorDecl) &&
                !s.name.empty()) {
                std::string payload{};
                if (s.kind == parus::ast::StmtKind::kProtoDecl) payload = "parus_decl_kind=proto";
                if (s.kind == parus::ast::StmtKind::kEnumDecl) payload = build_enum_decl_payload_(ast, s, types);
                if (s.kind == parus::ast::StmtKind::kClassDecl) payload = "parus_decl_kind=class";
                if (s.kind == parus::ast::StmtKind::kActorDecl) payload = "parus_decl_kind=actor";
                append_generic_decl_payload_(payload, ast, s, types);
                if (s.kind != parus::ast::StmtKind::kProtoDecl) {
                    append_type_impl_proto_payload_(payload, ast, s, types);
                    append_type_assoc_binding_payload_(payload, ast, s, types);
                }
                push_export(parus::sema::SymbolKind::kType,
                            qualify_name_(ns, s.name),
                            s.type,
                            s.is_export,
                            s.span,
                            /*link_name=*/{},
                            std::move(payload));
                return;
            }

            if (s.kind == parus::ast::StmtKind::kActsDecl && !s.name.empty()) {
                const std::string acts_qname = qualify_name_(ns, s.name);
                const bool export_acts_decl_symbol = !(s.acts_is_for && !s.acts_has_set_name);
                if (export_acts_decl_symbol) {
                    std::string acts_payload{};
                    append_generic_decl_payload_(acts_payload, ast, s, types);
                    push_export(
                        parus::sema::SymbolKind::kAct,
                        acts_qname,
                        s.acts_target_type,
                        s.is_export,
                        s.span,
                        {},
                        std::move(acts_payload)
                    );
                }

                const bool owner_is_builtin =
                    (s.acts_target_type != parus::ty::kInvalidType) &&
                    (types.get(s.acts_target_type).kind == parus::ty::Kind::kBuiltin);
                std::string owner_builtin_text{};
                if (owner_is_builtin) {
                    owner_builtin_text = types.to_string(s.acts_target_type);
                }

                const auto& kids = ast.stmt_children();
                const uint64_t begin = s.stmt_begin;
                const uint64_t end = begin + s.stmt_count;
                if (begin <= kids.size() && end <= kids.size()) {
                    for (uint32_t i = 0; i < s.stmt_count; ++i) {
                        const auto msid = kids[s.stmt_begin + i];
                        if (msid == parus::ast::k_invalid_stmt || static_cast<size_t>(msid) >= ast.stmts().size()) continue;
                        const auto& ms = ast.stmt(msid);
                        if (ms.kind != parus::ast::StmtKind::kFnDecl || ms.name.empty()) continue;
                        if (ms.fn_is_operator) continue;

                        std::string payload{};
                        if (s.acts_is_for && owner_is_builtin) {
                            bool receiver_is_self = false;
                            for (uint32_t pi = 0; pi < ms.param_count; ++pi) {
                                const auto& p = ast.params()[ms.param_begin + pi];
                                if (!p.is_self) continue;
                                receiver_is_self = true;
                                break;
                            }
                            payload = "parus_builtin_acts|owner=" + owner_builtin_text +
                                      "|member=" + std::string(ms.name) +
                                      "|self=" + (receiver_is_self ? "1" : "0");
                        }
                        append_generic_decl_payload_(payload, ast, s, types);
                        append_generic_decl_payload_(payload, ast, ms, types);
                        if (ms.is_throwing && ms.link_abi != parus::ast::LinkAbi::kC) {
                            if (!payload.empty()) payload += "|";
                            payload += "throwing=1";
                        }

                        parus::ty::TypeId export_member_type = ms.type;
                        if (s.acts_is_for && export_member_type != parus::ty::kInvalidType) {
                            export_member_type = rewrite_builtin_acts_self_type_(
                                rewrite_builtin_acts_self_type_,
                                export_member_type,
                                s.acts_target_type
                            );
                        }
                        std::string member_qname = acts_qname;
                        member_qname += "::";
                        member_qname += std::string(ms.name);
                        const std::string sig_repr =
                            (export_member_type != parus::ty::kInvalidType)
                                ? export_types.to_export_string(export_member_type)
                                : std::string("def(?)");
                        const std::string link_name = build_function_link_name_(
                            bundle_name,
                            member_qname,
                            ms.fn_mode,
                            sig_repr,
                            ms.link_abi == parus::ast::LinkAbi::kC
                        );
                        push_export(parus::sema::SymbolKind::kFn,
                                    member_qname,
                                    export_member_type,
                                    s.is_export,
                                    ms.span,
                                    link_name,
                                    std::move(payload));
                    }
                }
                return;
            }

            if (s.kind == parus::ast::StmtKind::kInstDecl && !s.name.empty()) {
                std::string payload{};
                const auto src = sm.content(s.span.file_id);
                if (s.span.lo <= s.span.hi && s.span.hi <= src.size()) {
                    payload.assign(src.substr(s.span.lo, s.span.hi - s.span.lo));
                }
                push_export(parus::sema::SymbolKind::kInst,
                            qualify_name_(ns, s.name),
                            parus::ty::kInvalidType,
                            s.is_export,
                            s.span,
                            /*link_name=*/{},
                            std::move(payload));
                return;
            }

            if (s.kind == parus::ast::StmtKind::kUse &&
                s.use_kind == parus::ast::UseKind::kCoreBuiltinUse &&
                !s.use_name.empty()) {
                std::string type_repr{};
                std::string payload{};
                if (!core_builtin_use_export_(module_head, s.use_name, type_repr, payload)) return;
                ExportSurfaceEntry e{};
                e.kind = parus::sema::SymbolKind::kType;
                e.kind_text = symbol_kind_to_text_(e.kind);
                e.path = qualify_name_(ns, s.use_name);
                e.module_head = module_head;
                e.decl_dir = decl_dir;
                e.type_repr = std::move(type_repr);
                e.inst_payload = std::move(payload);
                e.decl_file = decl_file;
                const auto lc = sm.line_col(s.span.file_id, s.span.lo);
                e.decl_line = lc.line;
                e.decl_col = lc.col;
                e.decl_bundle = bundle_name;
                e.is_export = true;
                out.push_back(std::move(e));
                return;
            }
        }

        std::string qualify_type_semantic_for_bundle_(
            std::string_view type_semantic,
            std::string_view bundle_name,
            std::string_view current_module_head,
            const std::unordered_set<std::string>& dep_module_heads,
            const std::unordered_set<std::string>& current_module_local_types
        ) {
            if (type_semantic.empty() || bundle_name.empty()) return std::string(type_semantic);

            parus::cimport::TypeSemanticNode node{};
            if (!parus::cimport::parse_type_semantic(type_semantic, node)) {
                return std::string(type_semantic);
            }

            auto rewrite = [&](auto&& self, parus::cimport::TypeSemanticNode& cur) -> void {
                if (cur.kind == parus::cimport::TypeSemanticKind::kNamed) {
                    const std::string_view name = cur.name;
                    if (!name.empty()) {
                        const size_t split = name.find("::");
                        if (split == std::string_view::npos) {
                            if (!current_module_head.empty() &&
                                current_module_local_types.find(cur.name) != current_module_local_types.end()) {
                                cur.name = std::string(bundle_name) + "::" +
                                           std::string(current_module_head) + "::" +
                                           cur.name;
                            }
                        } else {
                            const std::string head(name.substr(0, split));
                            if (dep_module_heads.find(head) != dep_module_heads.end()) {
                                cur.name = std::string(bundle_name) + "::" + cur.name;
                            }
                        }
                    }
                }
                for (auto& child : cur.children) self(self, child);
            };

            rewrite(rewrite, node);
            return parus::cimport::serialize_type_semantic(node);
        }

        std::string qualify_type_repr_for_bundle_(
            std::string_view type_repr,
            std::string_view bundle_name,
            std::string_view current_module_head,
            const std::unordered_set<std::string>& dep_module_heads,
            const std::unordered_set<std::string>& current_module_local_types
        ) {
            if (type_repr.empty() || bundle_name.empty()) return std::string(type_repr);

            parus::ty::TypePool types{};
            const auto parsed = parus::cimport::parse_external_type_repr(
                type_repr,
                std::string_view{},
                std::string_view{},
                types
            );
            if (parsed == parus::ty::kInvalidType) {
                return std::string(type_repr);
            }

            auto qualify_named_path = [&](const std::vector<std::string_view>& path) -> std::vector<std::string> {
                std::vector<std::string> out{};
                out.reserve(path.size() + 2u);
                if (path.empty()) return out;

                if (path.size() == 1u &&
                    !current_module_head.empty() &&
                    current_module_local_types.find(std::string(path[0])) != current_module_local_types.end()) {
                    out.emplace_back(bundle_name);
                    size_t pos = 0;
                    while (pos < current_module_head.size()) {
                        const size_t next = current_module_head.find("::", pos);
                        const size_t stop = (next == std::string_view::npos) ? current_module_head.size() : next;
                        out.emplace_back(current_module_head.substr(pos, stop - pos));
                        if (next == std::string_view::npos) break;
                        pos = next + 2;
                    }
                    out.emplace_back(path[0]);
                    return out;
                }

                if (!path.empty() &&
                    dep_module_heads.find(std::string(path[0])) != dep_module_heads.end()) {
                    out.emplace_back(bundle_name);
                    for (const auto seg : path) out.emplace_back(seg);
                    return out;
                }

                for (const auto seg : path) out.emplace_back(seg);
                return out;
            };

            auto render = [&](auto&& self, parus::ty::TypeId cur, std::string& out) -> void {
                if (cur == parus::ty::kInvalidType || cur >= types.count()) {
                    out += "<invalid-type>";
                    return;
                }
                const auto& tt = types.get(cur);
                switch (tt.kind) {
                    case parus::ty::Kind::kError:
                        out += "<error>";
                        return;
                    case parus::ty::Kind::kBuiltin:
                        out += parus::ty::TypePool::builtin_name(tt.builtin);
                        return;
                    case parus::ty::Kind::kNamedUser: {
                        std::vector<std::string_view> path{};
                        std::vector<parus::ty::TypeId> args{};
                        if (!types.decompose_named_user(cur, path, args) || path.empty()) {
                            out += "<user-type?>";
                            return;
                        }
                        const auto qualified = qualify_named_path(path);
                        for (size_t i = 0; i < qualified.size(); ++i) {
                            if (i) out += "::";
                            out += qualified[i];
                        }
                        if (!args.empty()) {
                            out += "<";
                            for (size_t i = 0; i < args.size(); ++i) {
                                if (i) out += ",";
                                self(self, args[i], out);
                            }
                            out += ">";
                        }
                        return;
                    }
                    case parus::ty::Kind::kOptional:
                        self(self, tt.elem, out);
                        out += "?";
                        return;
                    case parus::ty::Kind::kArray:
                        self(self, tt.elem, out);
                        if (tt.array_has_size) {
                            out += "[";
                            out += std::to_string(tt.array_size);
                            out += "]";
                        } else {
                            out += "[]";
                        }
                        return;
                    case parus::ty::Kind::kBorrow:
                        out += "&";
                        if (tt.borrow_is_mut) out += "mut ";
                        self(self, tt.elem, out);
                        return;
                    case parus::ty::Kind::kEscape:
                        out += "~";
                        self(self, tt.elem, out);
                        return;
                    case parus::ty::Kind::kPtr:
                        out += tt.ptr_is_mut ? "*mut " : "*const ";
                        self(self, tt.elem, out);
                        return;
                    case parus::ty::Kind::kFn: {
                        out += "fn(";
                        for (uint32_t i = 0; i < tt.param_count; ++i) {
                            if (i) out += ", ";
                            self(self, types.fn_param_at(cur, i), out);
                        }
                        out += ") -> ";
                        self(self, tt.ret, out);
                        return;
                    }
                }
            };

            std::string out{};
            render(render, parsed, out);
            return out;
        }

        parus::ty::TypeId qualify_type_for_bundle_(
            parus::ty::TypeId tid,
            parus::ty::TypePool& types,
            std::string_view bundle_name,
            std::string_view current_module_head,
            const std::unordered_set<std::string>& dep_module_heads,
            const std::unordered_set<std::string>& current_module_local_types
        ) {
            using TypeId = parus::ty::TypeId;
            if (tid == parus::ty::kInvalidType || bundle_name.empty()) return tid;

            std::unordered_map<TypeId, TypeId> memo{};

            auto qualify_named_path = [&](const std::vector<std::string_view>& path) -> std::vector<std::string> {
                std::vector<std::string> out{};
                out.reserve(path.size() + 2u);
                if (path.empty()) return out;

                if (path.size() == 1u &&
                    !current_module_head.empty() &&
                    current_module_local_types.find(std::string(path[0])) != current_module_local_types.end()) {
                    out.emplace_back(bundle_name);
                    size_t pos = 0;
                    while (pos < current_module_head.size()) {
                        const size_t next = current_module_head.find("::", pos);
                        const size_t stop = (next == std::string_view::npos) ? current_module_head.size() : next;
                        out.emplace_back(current_module_head.substr(pos, stop - pos));
                        if (next == std::string_view::npos) break;
                        pos = next + 2;
                    }
                    out.emplace_back(path[0]);
                    return out;
                }

                if (!path.empty() &&
                    dep_module_heads.find(std::string(path[0])) != dep_module_heads.end()) {
                    out.emplace_back(bundle_name);
                    for (const auto seg : path) out.emplace_back(seg);
                    return out;
                }

                for (const auto seg : path) out.emplace_back(seg);
                return out;
            };

            auto walk = [&](auto&& self, TypeId cur) -> TypeId {
                if (cur == parus::ty::kInvalidType || cur >= types.count()) return cur;
                if (auto it = memo.find(cur); it != memo.end()) return it->second;

                const auto& tt = types.get(cur);
                TypeId out = cur;

                switch (tt.kind) {
                    case parus::ty::Kind::kNamedUser: {
                        std::vector<std::string_view> path{};
                        std::vector<TypeId> args{};
                        if (!types.decompose_named_user(cur, path, args) || path.empty()) {
                            break;
                        }

                        std::vector<TypeId> rewritten_args{};
                        rewritten_args.reserve(args.size());
                        bool changed = false;
                        for (const auto arg : args) {
                            const TypeId sub = self(self, arg);
                            if (sub != arg) changed = true;
                            rewritten_args.push_back(sub);
                        }

                        const auto qualified = qualify_named_path(path);
                        if (qualified.size() != path.size()) {
                            changed = true;
                        } else {
                            for (size_t i = 0; i < path.size(); ++i) {
                                if (qualified[i] != path[i]) {
                                    changed = true;
                                    break;
                                }
                            }
                        }
                        if (!changed) break;

                        std::vector<std::string_view> qualified_views{};
                        qualified_views.reserve(qualified.size());
                        for (const auto& seg : qualified) qualified_views.push_back(seg);
                        out = types.intern_named_path_with_args(
                            qualified_views.data(),
                            static_cast<uint32_t>(qualified_views.size()),
                            rewritten_args.empty() ? nullptr : rewritten_args.data(),
                            static_cast<uint32_t>(rewritten_args.size())
                        );
                        break;
                    }
                    case parus::ty::Kind::kOptional: {
                        const TypeId elem = self(self, tt.elem);
                        if (elem != tt.elem) out = types.make_optional(elem);
                        break;
                    }
                    case parus::ty::Kind::kBorrow: {
                        const TypeId elem = self(self, tt.elem);
                        if (elem != tt.elem) out = types.make_borrow(elem, tt.borrow_is_mut);
                        break;
                    }
                    case parus::ty::Kind::kEscape: {
                        const TypeId elem = self(self, tt.elem);
                        if (elem != tt.elem) out = types.make_escape(elem);
                        break;
                    }
                    case parus::ty::Kind::kPtr: {
                        const TypeId elem = self(self, tt.elem);
                        if (elem != tt.elem) out = types.make_ptr(elem, tt.ptr_is_mut);
                        break;
                    }
                    case parus::ty::Kind::kArray: {
                        const TypeId elem = self(self, tt.elem);
                        if (elem != tt.elem) out = types.make_array(elem, tt.array_has_size, tt.array_size);
                        break;
                    }
                    case parus::ty::Kind::kFn: {
                        std::vector<TypeId> params{};
                        std::vector<std::string_view> labels{};
                        std::vector<uint8_t> defaults{};
                        bool changed = false;
                        params.reserve(tt.param_count);
                        labels.reserve(tt.param_count);
                        defaults.reserve(tt.param_count);
                        for (uint32_t i = 0; i < tt.param_count; ++i) {
                            const auto p = types.fn_param_at(cur, i);
                            const auto np = self(self, p);
                            if (np != p) changed = true;
                            params.push_back(np);
                            labels.push_back(types.fn_param_label_at(cur, i));
                            defaults.push_back(types.fn_param_has_default_at(cur, i) ? 1u : 0u);
                        }
                        const auto ret = self(self, tt.ret);
                        if (ret != tt.ret) changed = true;
                        if (changed) {
                            out = types.make_fn(
                                ret,
                                params.empty() ? nullptr : params.data(),
                                tt.param_count,
                                tt.positional_param_count,
                                labels.empty() ? nullptr : labels.data(),
                                defaults.empty() ? nullptr : defaults.data(),
                                tt.fn_is_c_abi,
                                tt.fn_is_c_variadic,
                                tt.fn_callconv
                            );
                        }
                        break;
                    }
                    default:
                        break;
                }

                memo.emplace(cur, out);
                return out;
            };

            return walk(walk, tid);
        }

        const parus::sema::Symbol* find_exact_symbol_by_name_(
            const parus::sema::SymbolTable& sym,
            std::string_view name
        ) {
            const parus::sema::Symbol* fallback = nullptr;
            for (const auto& ss : sym.symbols()) {
                if (ss.name != name) continue;
                if (ss.name.find("@@extovl$") != std::string::npos) continue;
                if (ss.kind != parus::sema::SymbolKind::kType &&
                    ss.kind != parus::sema::SymbolKind::kField) {
                    continue;
                }
                if (!ss.decl_module_head.empty() || !ss.decl_bundle_name.empty()) {
                    return &ss;
                }
                if (fallback == nullptr) fallback = &ss;
            }
            return fallback;
        }

        std::string canonical_symbolic_path_from_symbol_(const parus::sema::Symbol& ss) {
            if (ss.name.empty()) return {};

            std::string name = ss.name;
            if (ss.decl_module_head.empty()) {
                if (!ss.decl_bundle_name.empty()) {
                    const std::string prefix = ss.decl_bundle_name + "::";
                    if (name != ss.decl_bundle_name && !name.starts_with(prefix) &&
                        name.find("::") == std::string::npos) {
                        return prefix + name;
                    }
                }
                return name;
            }

            std::string module_head = ss.decl_module_head;
            if (!ss.decl_bundle_name.empty()) {
                const std::string prefix = ss.decl_bundle_name + "::";
                if (!(module_head == ss.decl_bundle_name || module_head.starts_with(prefix))) {
                    module_head = prefix + module_head;
                }
            }

            std::string local = parus::strip_module_prefix(name, ss.decl_module_head);
            if (local.empty()) {
                local = parus::strip_module_prefix(name, module_head);
            }
            if (local == name && name.find("::") != std::string::npos) {
                if (const std::string short_head =
                        parus::short_core_module_head(ss.decl_bundle_name, ss.decl_module_head);
                    !short_head.empty()) {
                    const std::string short_local = parus::strip_module_prefix(name, short_head);
                    if (!short_local.empty() && short_local != name) {
                        local = short_local;
                    }
                }
            }
            if (local.empty()) {
                local = name;
            }
            if (local.find("::") != std::string::npos) {
                if (const size_t split = local.rfind("::"); split != std::string::npos) {
                    local = local.substr(split + 2);
                }
            }

            return module_head + "::" + local;
        }

        std::string canonicalize_symbolic_path_with_symbols_(
            std::string_view raw_path,
            const parus::sema::SymbolTable& sym
        ) {
            if (raw_path.empty()) return {};
            if (const auto* ss = find_exact_symbol_by_name_(sym, raw_path)) {
                return canonical_symbolic_path_from_symbol_(*ss);
            }
            return std::string(raw_path);
        }

        parus::ty::TypeId canonicalize_type_with_symbols_(
            parus::ty::TypeId tid,
            parus::ty::TypePool& types,
            const parus::sema::SymbolTable& sym
        ) {
            std::unordered_map<parus::ty::TypeId, parus::ty::TypeId> memo{};

            auto split_path = [](std::string_view raw) -> std::vector<std::string> {
                std::vector<std::string> parts{};
                size_t pos = 0;
                while (pos <= raw.size()) {
                    const size_t next = raw.find("::", pos);
                    if (next == std::string_view::npos) {
                        parts.emplace_back(raw.substr(pos));
                        break;
                    }
                    parts.emplace_back(raw.substr(pos, next - pos));
                    pos = next + 2;
                }
                return parts;
            };

            auto walk = [&](auto&& self, parus::ty::TypeId cur) -> parus::ty::TypeId {
                if (cur == parus::ty::kInvalidType) return cur;
                if (const auto it = memo.find(cur); it != memo.end()) return it->second;

                parus::ty::TypeId out = cur;
                const auto& tt = types.get(cur);
                switch (tt.kind) {
                    case parus::ty::Kind::kNamedUser: {
                        std::vector<std::string_view> path{};
                        std::vector<parus::ty::TypeId> args{};
                        if (!types.decompose_named_user(cur, path, args) || path.empty()) break;

                        std::vector<parus::ty::TypeId> rewritten_args{};
                        rewritten_args.reserve(args.size());
                        bool changed = false;
                        for (const auto arg : args) {
                            const auto sub = self(self, arg);
                            if (sub != arg) changed = true;
                            rewritten_args.push_back(sub);
                        }

                        std::string raw_path{};
                        for (size_t i = 0; i < path.size(); ++i) {
                            if (i != 0) raw_path += "::";
                            raw_path += std::string(path[i]);
                        }
                        const std::string canonical = canonicalize_symbolic_path_with_symbols_(raw_path, sym);
                        if (canonical != raw_path) changed = true;
                        if (!changed) break;

                        const auto parts = split_path(canonical);
                        std::vector<std::string_view> views{};
                        views.reserve(parts.size());
                        for (const auto& part : parts) views.push_back(part);
                        out = types.intern_named_path_with_args(
                            views.data(),
                            static_cast<uint32_t>(views.size()),
                            rewritten_args.empty() ? nullptr : rewritten_args.data(),
                            static_cast<uint32_t>(rewritten_args.size())
                        );
                        break;
                    }
                    case parus::ty::Kind::kOptional: {
                        const auto elem = self(self, tt.elem);
                        if (elem != tt.elem) out = types.make_optional(elem);
                        break;
                    }
                    case parus::ty::Kind::kBorrow: {
                        const auto elem = self(self, tt.elem);
                        if (elem != tt.elem) out = types.make_borrow(elem, tt.borrow_is_mut);
                        break;
                    }
                    case parus::ty::Kind::kEscape: {
                        const auto elem = self(self, tt.elem);
                        if (elem != tt.elem) out = types.make_escape(elem);
                        break;
                    }
                    case parus::ty::Kind::kPtr: {
                        const auto elem = self(self, tt.elem);
                        if (elem != tt.elem) out = types.make_ptr(elem, tt.ptr_is_mut);
                        break;
                    }
                    case parus::ty::Kind::kArray: {
                        const auto elem = self(self, tt.elem);
                        if (elem != tt.elem) out = types.make_array(elem, tt.array_has_size, tt.array_size);
                        break;
                    }
                    case parus::ty::Kind::kFn: {
                        std::vector<parus::ty::TypeId> params{};
                        std::vector<std::string_view> labels{};
                        std::vector<uint8_t> defaults{};
                        bool changed = false;
                        params.reserve(tt.param_count);
                        labels.reserve(tt.param_count);
                        defaults.reserve(tt.param_count);
                        for (uint32_t i = 0; i < tt.param_count; ++i) {
                            const auto p = types.fn_param_at(cur, i);
                            const auto np = self(self, p);
                            if (np != p) changed = true;
                            params.push_back(np);
                            labels.push_back(types.fn_param_label_at(cur, i));
                            defaults.push_back(types.fn_param_has_default_at(cur, i) ? 1u : 0u);
                        }
                        const auto ret = self(self, tt.ret);
                        if (ret != tt.ret) changed = true;
                        if (changed) {
                            out = types.make_fn(
                                ret,
                                params.empty() ? nullptr : params.data(),
                                tt.param_count,
                                tt.positional_param_count,
                                labels.empty() ? nullptr : labels.data(),
                                defaults.empty() ? nullptr : defaults.data(),
                                tt.fn_is_c_abi,
                                tt.fn_is_c_variadic,
                                tt.fn_callconv
                            );
                        }
                        break;
                    }
                    default:
                        break;
                }

                memo.emplace(cur, out);
                return out;
            };

            return walk(walk, tid);
        }

        std::pair<std::string, std::string> canonicalize_payload_type_meta_with_symbols_(
            std::string_view repr,
            std::string_view semantic,
            const parus::sema::SymbolTable& sym
        ) {
            if (repr.empty() && semantic.empty()) {
                return {std::string(repr), std::string(semantic)};
            }

            parus::ty::TypePool sem_pool{};
            parus::ty::TypeId sem_type = parus::ty::kInvalidType;
            std::string sem_repr{};
            if (!semantic.empty()) {
                const auto parsed = parus::cimport::parse_external_type_repr(
                    std::string_view{},
                    semantic,
                    std::string_view{},
                    sem_pool
                );
                if (parsed != parus::ty::kInvalidType) {
                    sem_type = canonicalize_type_with_symbols_(parsed, sem_pool, sym);
                    sem_repr = sem_pool.to_export_string(sem_type);
                }
            }

            parus::ty::TypePool repr_pool{};
            parus::ty::TypeId repr_type = parus::ty::kInvalidType;
            std::string repr_text{};
            if (!repr.empty()) {
                const auto parsed = parus::cimport::parse_external_type_repr(
                    repr,
                    std::string_view{},
                    std::string_view{},
                    repr_pool
                );
                if (parsed != parus::ty::kInvalidType) {
                    repr_type = canonicalize_type_with_symbols_(parsed, repr_pool, sym);
                    repr_text = repr_pool.to_export_string(repr_type);
                }
            }

            if (repr_type != parus::ty::kInvalidType) {
                return {repr_text, parus::cimport::serialize_type_semantic_from_type(repr_type, repr_pool)};
            }
            if (sem_type != parus::ty::kInvalidType) {
                return {sem_repr, parus::cimport::serialize_type_semantic_from_type(sem_type, sem_pool)};
            }
            return {std::string(repr), std::string(semantic)};
        }

        std::string canonicalize_inst_payload_with_symbols_(
            std::string_view payload,
            const parus::sema::SymbolTable& sym
        ) {
            if (payload.empty()) return {};

            std::string out{};
            size_t pos = 0;
            bool first = true;
            while (pos < payload.size()) {
                size_t next = payload.find('|', pos);
                if (next == std::string_view::npos) next = payload.size();
                const std::string_view part = payload.substr(pos, next - pos);
                std::string rewritten = std::string(part);

                if (part.starts_with("gconstraint=")) {
                    const std::string_view body = part.substr(std::string_view("gconstraint=").size());
                    const size_t comma1 = body.find(',');
                    const size_t comma2 =
                        (comma1 == std::string_view::npos) ? std::string_view::npos : body.find(',', comma1 + 1);
                    if (comma1 != std::string_view::npos && comma2 != std::string_view::npos) {
                        const std::string kind = payload_unescape_value_(body.substr(0, comma1));
                        const std::string lhs = payload_unescape_value_(body.substr(comma1 + 1, comma2 - comma1 - 1));
                        std::string rhs = payload_unescape_value_(body.substr(comma2 + 1));
                        if (kind == "proto") {
                            rhs = canonicalize_payload_type_meta_with_symbols_(
                                rhs,
                                std::string_view{},
                                sym
                            ).first;
                        } else if (kind == "type_eq") {
                            rhs = canonicalize_payload_type_meta_with_symbols_(rhs, std::string_view{}, sym).first;
                        }
                        rewritten = "gconstraint=" + payload_escape_value_(kind) + "," +
                                    payload_escape_value_(lhs) + "," +
                                    payload_escape_value_(rhs);
                    }
                }

                if (!first) out += "|";
                out += rewritten;
                first = false;
                if (next == payload.size()) break;
                pos = next + 1;
            }
            return out;
        }

        void canonicalize_export_surface_entries_with_symbols_(
            std::vector<ExportSurfaceEntry>& entries,
            const parus::sema::SymbolTable& sym
        ) {
            for (auto& e : entries) {
                e.inst_payload = canonicalize_inst_payload_with_symbols_(e.inst_payload, sym);
            }
        }

        std::pair<std::string, std::string> qualify_payload_type_meta_for_bundle_(
            std::string_view repr,
            std::string_view semantic,
            std::string_view bundle_name,
            std::string_view current_module_head,
            const std::unordered_set<std::string>& dep_module_heads,
            const std::unordered_set<std::string>& current_module_local_symbols
        ) {
            if ((repr.empty() && semantic.empty()) || bundle_name.empty()) {
                return {std::string(repr), std::string(semantic)};
            }

            parus::ty::TypePool sem_pool{};
            parus::ty::TypeId sem_type = parus::ty::kInvalidType;
            std::string sem_qualified_semantic = std::string(semantic);
            std::string sem_qualified_repr{};
            if (!semantic.empty()) {
                sem_qualified_semantic = qualify_type_semantic_for_bundle_(
                    semantic,
                    bundle_name,
                    current_module_head,
                    dep_module_heads,
                    current_module_local_symbols
                );
                sem_type = parus::cimport::parse_external_type_repr(
                    std::string_view{},
                    sem_qualified_semantic,
                    std::string_view{},
                    sem_pool
                );
                if (sem_type != parus::ty::kInvalidType) {
                    sem_qualified_repr = sem_pool.to_export_string(sem_type);
                }
            }

            parus::ty::TypePool repr_pool{};
            parus::ty::TypeId repr_type = parus::ty::kInvalidType;
            std::string repr_qualified_repr{};
            if (!repr.empty()) {
                const auto parsed = parus::cimport::parse_external_type_repr(
                    repr,
                    std::string_view{},
                    std::string_view{},
                    repr_pool
                );
                if (parsed != parus::ty::kInvalidType) {
                    repr_type = qualify_type_for_bundle_(
                        parsed,
                        repr_pool,
                        bundle_name,
                        current_module_head,
                        dep_module_heads,
                        current_module_local_symbols
                    );
                    if (repr_type != parus::ty::kInvalidType) {
                        repr_qualified_repr = repr_pool.to_export_string(repr_type);
                    }
                }
            }

            if (repr_type != parus::ty::kInvalidType) {
                return {
                    repr_qualified_repr,
                    parus::cimport::serialize_type_semantic_from_type(repr_type, repr_pool)
                };
            }
            if (sem_type != parus::ty::kInvalidType) {
                return {
                    sem_qualified_repr,
                    parus::cimport::serialize_type_semantic_from_type(sem_type, sem_pool)
                };
            }
            return {std::string(repr), std::string(semantic)};
        }

        std::string qualify_inst_payload_for_bundle_(
            std::string_view payload,
            std::string_view bundle_name,
            std::string_view current_module_head,
            const std::unordered_set<std::string>& dep_module_heads,
            const std::unordered_set<std::string>& current_module_local_symbols
        ) {
            if (payload.empty() || bundle_name.empty()) return std::string(payload);

            std::string out{};
            size_t pos = 0;
            bool first = true;
            while (pos < payload.size()) {
                size_t next = payload.find('|', pos);
                if (next == std::string_view::npos) next = payload.size();
                const std::string_view part = payload.substr(pos, next - pos);
                std::string rewritten = std::string(part);

                if (part.starts_with("gconstraint=")) {
                    const std::string_view body = part.substr(std::string_view("gconstraint=").size());
                    const size_t comma1 = body.find(',');
                    const size_t comma2 =
                        (comma1 == std::string_view::npos) ? std::string_view::npos : body.find(',', comma1 + 1);
                    if (comma1 != std::string_view::npos && comma2 != std::string_view::npos) {
                        const std::string kind = payload_unescape_value_(body.substr(0, comma1));
                        const std::string lhs = payload_unescape_value_(body.substr(comma1 + 1, comma2 - comma1 - 1));
                        std::string rhs = payload_unescape_value_(body.substr(comma2 + 1));
                        if (kind == "proto") {
                            rhs = qualify_payload_type_meta_for_bundle_(
                                rhs,
                                std::string_view{},
                                bundle_name,
                                current_module_head,
                                dep_module_heads,
                                current_module_local_symbols
                            ).first;
                        } else if (kind == "type_eq") {
                            rhs = qualify_payload_type_meta_for_bundle_(
                                rhs,
                                std::string_view{},
                                bundle_name,
                                current_module_head,
                                dep_module_heads,
                                current_module_local_symbols
                            ).first;
                        }
                        rewritten = "gconstraint=" + payload_escape_value_(kind) + "," +
                                    payload_escape_value_(lhs) + "," +
                                    payload_escape_value_(rhs);
                    }
                } else if (part.starts_with("impl_proto=")) {
                    const std::string body = payload_unescape_value_(
                        part.substr(std::string_view("impl_proto=").size())
                    );
                    const size_t split = body.find('@');
                    const std::string repr = (split == std::string::npos) ? body : body.substr(0, split);
                    std::string body_out = qualify_type_repr_for_bundle_(
                        repr,
                        bundle_name,
                        current_module_head,
                        dep_module_heads,
                        current_module_local_symbols
                    );
                    if (body_out.empty()) {
                        body_out = repr;
                    }
                    rewritten = "impl_proto=" + payload_escape_value_(body_out);
                } else if (part.starts_with("assoc_type=")) {
                    const std::string body = payload_unescape_value_(
                        part.substr(std::string_view("assoc_type=").size())
                    );
                    const size_t comma = body.find(',');
                    if (comma != std::string::npos) {
                        const std::string name = body.substr(0, comma);
                        const std::string encoded_type = body.substr(comma + 1);
                        if (!name.empty() && !encoded_type.empty()) {
                            const size_t split = encoded_type.find('@');
                            const std::string repr =
                                (split == std::string::npos) ? encoded_type : encoded_type.substr(0, split);
                            const std::string semantic =
                                (split == std::string::npos) ? std::string{} : encoded_type.substr(split + 1);
                            const auto [qrepr, qsemantic] = qualify_payload_type_meta_for_bundle_(
                                repr,
                                semantic,
                                bundle_name,
                                current_module_head,
                                dep_module_heads,
                                current_module_local_symbols
                            );
                            std::string body_out = name;
                            body_out += ",";
                            body_out += qrepr.empty() ? repr : qrepr;
                            if (!qsemantic.empty()) {
                                body_out += "@";
                                body_out += qsemantic;
                            }
                            rewritten = "assoc_type=" + payload_escape_value_(body_out);
                        }
                    }
                } else if (part.starts_with("field=")) {
                    const std::string_view body = part.substr(std::string_view("field=").size());
                    const size_t colon = body.find(':');
                    if (colon != std::string_view::npos) {
                        const std::string name = std::string(body.substr(0, colon));
                        const std::string encoded_type = payload_unescape_value_(body.substr(colon + 1));
                        if (encoded_type != "<invalid>") {
                            const size_t split = encoded_type.find('@');
                            const std::string repr =
                                (split == std::string::npos) ? encoded_type : encoded_type.substr(0, split);
                            const std::string semantic =
                                (split == std::string::npos) ? std::string{} : encoded_type.substr(split + 1);
                            const auto [qrepr, qsemantic] = qualify_payload_type_meta_for_bundle_(
                                repr,
                                semantic,
                                bundle_name,
                                current_module_head,
                                dep_module_heads,
                                current_module_local_symbols
                            );
                            std::string body_out = qrepr;
                            if (!qsemantic.empty()) {
                                body_out += "@";
                                body_out += qsemantic;
                            }
                            rewritten = "field=" + name + ":" + payload_escape_value_(body_out);
                        }
                    }
                }

                if (!first) out += "|";
                out += rewritten;
                first = false;
                if (next == payload.size()) break;
                pos = next + 1;
            }
            return out;
        }

        void qualify_export_surface_entries_for_bundle_(
            std::vector<ExportSurfaceEntry>& entries,
            std::string_view bundle_name
        ) {
            if (entries.empty() || bundle_name.empty()) return;

            auto relative_module_head = [&](std::string_view module_head) -> std::string {
                const std::string prefix = std::string(bundle_name) + "::";
                if (module_head.starts_with(prefix)) {
                    return std::string(module_head.substr(prefix.size()));
                }
                return std::string(module_head);
            };

            std::unordered_set<std::string> bundle_module_heads{};
            std::unordered_map<std::string, std::unordered_set<std::string>> bundle_local_types_by_module{};
            bundle_module_heads.reserve(entries.size());
            bundle_local_types_by_module.reserve(entries.size());
            for (const auto& e : entries) {
                if (!e.module_head.empty()) {
                    bundle_module_heads.insert(relative_module_head(e.module_head));
                }
                if ((e.kind == parus::sema::SymbolKind::kType ||
                     e.kind == parus::sema::SymbolKind::kField) &&
                    !e.module_head.empty() &&
                    !e.path.empty() &&
                    e.path.find("::") == std::string::npos) {
                    bundle_local_types_by_module[relative_module_head(e.module_head)].insert(e.path);
                }
            }

            for (auto& e : entries) {
                if ((e.type_repr.empty() && e.type_semantic.empty()) || e.module_head.empty()) {
                    continue;
                }
                const std::string current_module_head = relative_module_head(e.module_head);
                const auto it = bundle_local_types_by_module.find(current_module_head);
                const std::unordered_set<std::string> empty_names{};
                const auto& local_names = (it != bundle_local_types_by_module.end()) ? it->second : empty_names;
                e.inst_payload = qualify_inst_payload_for_bundle_(
                    e.inst_payload,
                    bundle_name,
                    current_module_head,
                    bundle_module_heads,
                    local_names
                );
                const auto has_bundle_qual = [&](std::string_view type_repr) -> bool {
                    const std::string needle = std::string(bundle_name) + "::";
                    return type_repr.find(needle) != std::string_view::npos;
                };

                parus::ty::TypePool sem_pool{};
                parus::ty::TypeId sem_type = parus::ty::kInvalidType;
                std::string sem_qualified_semantic = e.type_semantic;
                std::string sem_qualified_repr{};
                if (!e.type_semantic.empty()) {
                    sem_qualified_semantic = qualify_type_semantic_for_bundle_(
                        e.type_semantic,
                        bundle_name,
                        current_module_head,
                        bundle_module_heads,
                        local_names
                    );
                    sem_type = parus::cimport::parse_external_type_repr(
                        std::string_view{},
                        sem_qualified_semantic,
                        e.inst_payload,
                        sem_pool
                    );
                    if (sem_type != parus::ty::kInvalidType) {
                        sem_qualified_repr = sem_pool.to_export_string(sem_type);
                    }
                }

                parus::ty::TypePool repr_pool{};
                parus::ty::TypeId repr_type = parus::ty::kInvalidType;
                std::string repr_qualified_repr{};
                if (!e.type_repr.empty()) {
                    const auto parsed_type = parus::cimport::parse_external_type_repr(
                        e.type_repr,
                        std::string_view{},
                        e.inst_payload,
                        repr_pool
                    );
                    if (parsed_type != parus::ty::kInvalidType) {
                        repr_type = qualify_type_for_bundle_(
                            parsed_type,
                            repr_pool,
                            bundle_name,
                            current_module_head,
                            bundle_module_heads,
                            local_names
                        );
                        if (repr_type != parus::ty::kInvalidType) {
                            repr_qualified_repr = repr_pool.to_export_string(repr_type);
                        }
                    }
                }

                if (repr_type != parus::ty::kInvalidType &&
                    (has_bundle_qual(repr_qualified_repr) || sem_type == parus::ty::kInvalidType)) {
                    e.type_repr = std::move(repr_qualified_repr);
                    e.type_semantic = parus::cimport::serialize_type_semantic_from_type(repr_type, repr_pool);
                    continue;
                }
                if (sem_type != parus::ty::kInvalidType) {
                    e.type_repr = std::move(sem_qualified_repr);
                    e.type_semantic = parus::cimport::serialize_type_semantic_from_type(sem_type, sem_pool);
                    continue;
                }
                if (repr_type != parus::ty::kInvalidType) {
                    e.type_repr = std::move(repr_qualified_repr);
                    e.type_semantic = parus::cimport::serialize_type_semantic_from_type(repr_type, repr_pool);
                }
            }
        }

        std::string build_enum_decl_payload_(
            const parus::ast::AstArena& ast,
            const parus::ast::Stmt& s,
            const parus::ty::TypePool& types
        ) {
            std::string payload = "parus_decl_kind=enum";
            if (s.kind != parus::ast::StmtKind::kEnumDecl) return payload;

            const uint64_t begin = s.enum_variant_begin;
            const uint64_t end = begin + s.enum_variant_count;
            if (begin > ast.enum_variant_decls().size() || end > ast.enum_variant_decls().size()) {
                return payload;
            }

            payload += "|layout=";
            payload += (s.field_layout == parus::ast::FieldLayout::kC) ? "c" : "n";

            int64_t next_tag = 0;
            for (uint32_t i = 0; i < s.enum_variant_count; ++i) {
                const auto& v = ast.enum_variant_decls()[s.enum_variant_begin + i];
                const int64_t tag = v.has_discriminant ? v.discriminant : next_tag;
                next_tag = tag + 1;
                payload += "|variant=";
                payload += std::string(v.name);
                payload += ",";
                payload += std::to_string(tag);

                const uint64_t pb = v.payload_begin;
                const uint64_t pe = pb + v.payload_count;
                if (pb > ast.field_members().size() || pe > ast.field_members().size()) {
                    return payload;
                }
                for (uint32_t mi = v.payload_begin; mi < v.payload_begin + v.payload_count; ++mi) {
                    const auto& m = ast.field_members()[mi];
                    payload += "|payload=";
                    payload += std::string(v.name);
                    payload += ",";
                    payload += std::string(m.name);
                    payload += ",";
                    if (m.type != parus::ty::kInvalidType) {
                        payload += payload_escape_value_(types.to_export_string(m.type));
                        const auto semantic =
                            parus::cimport::serialize_type_semantic_from_type(m.type, types);
                        if (!semantic.empty()) {
                            payload += "@";
                            payload += payload_escape_value_(semantic);
                        }
                    } else {
                        payload += "<invalid>";
                    }
                }
            }
            return payload;
        }

        std::string build_field_decl_payload_(
            const parus::ast::AstArena& ast,
            const parus::ast::Stmt& s,
            const parus::ty::TypePool& types
        ) {
            std::string payload = "parus_field_decl";
            if (s.kind != parus::ast::StmtKind::kFieldDecl) return payload;

            payload += "|layout=";
            payload += (s.field_layout == parus::ast::FieldLayout::kC) ? "c" : "n";
            payload += "|align=";
            payload += std::to_string(s.field_align);
            append_generic_decl_payload_(payload, ast, s, types);
            append_type_impl_proto_payload_(payload, ast, s, types);
            append_type_assoc_binding_payload_(payload, ast, s, types);

            const uint64_t begin = s.field_member_begin;
            const uint64_t end = begin + s.field_member_count;
            if (begin > ast.field_members().size() || end > ast.field_members().size()) {
                return payload;
            }

            for (uint32_t i = 0; i < s.field_member_count; ++i) {
                const auto& m = ast.field_members()[s.field_member_begin + i];
                payload += "|field=";
                payload += std::string(m.name);
                payload += ":";
                if (m.type != parus::ty::kInvalidType) {
                    payload += payload_escape_value_(types.to_export_string(m.type));
                    const auto semantic =
                        parus::cimport::serialize_type_semantic_from_type(m.type, types);
                    if (!semantic.empty()) {
                        payload += "@";
                        payload += payload_escape_value_(semantic);
                    }
                } else {
                    payload += payload_escape_value_("<invalid>");
                }
            }

            return payload;
        }

        bool is_tag_only_enum_decl_payload_(std::string_view payload) {
            if (!payload.starts_with("parus_decl_kind=enum")) return false;
            return payload.find("|variant=") != std::string_view::npos;
        }

        bool collect_file_namespace_(const parus::ast::AstArena& ast, parus::ast::StmtId root, std::vector<std::string>& ns) {
            if (root == parus::ast::k_invalid_stmt || static_cast<size_t>(root) >= ast.stmts().size()) return false;
            const auto& rs = ast.stmt(root);
            if (rs.kind != parus::ast::StmtKind::kBlock) return false;
            const auto& kids = ast.stmt_children();
            const uint64_t begin = rs.stmt_begin;
            const uint64_t end = begin + rs.stmt_count;
            if (!(begin <= kids.size() && end <= kids.size())) return false;
            const auto& segs = ast.path_segs();
            for (uint32_t i = 0; i < rs.stmt_count; ++i) {
                const auto sid = kids[rs.stmt_begin + i];
                if (sid == parus::ast::k_invalid_stmt || static_cast<size_t>(sid) >= ast.stmts().size()) continue;
                const auto& s = ast.stmt(sid);
                if (s.kind == parus::ast::StmtKind::kNestDecl &&
                    s.nest_is_file_directive &&
                    s.nest_path_count > 0) {
                    const uint64_t pbegin = s.nest_path_begin;
                    const uint64_t pend = pbegin + s.nest_path_count;
                    if (!(pbegin <= segs.size() && pend <= segs.size())) return false;
                    for (uint32_t j = 0; j < s.nest_path_count; ++j) {
                        ns.push_back(std::string(segs[s.nest_path_begin + j]));
                    }
                    return true;
                }
            }
            return false;
        }

        bool collect_bundle_export_surface_(
            const std::vector<std::string>& bundle_sources,
            std::string_view bundle_root,
            const std::string& bundle_name,
            std::vector<ExportSurfaceEntry>& out,
            std::string& out_err
        ) {
            out.clear();
            out_err.clear();
            if (bundle_sources.empty()) return true;

            for (const auto& src_path : bundle_sources) {
                std::string src{};
                std::string io_err{};
                if (!parus::open_file(src_path, src, io_err)) {
                    out_err = "failed to open bundle source '" + src_path + "': " + io_err;
                    return false;
                }

                parus::SourceManager local_sm{};
                const uint32_t local_fid = local_sm.add(parus::normalize_path(src_path), std::move(src));
                parus::diag::Bag local_bag{};
                auto local_tokens = lex_with_sm_(local_sm, local_fid, &local_bag);
                if (local_bag.has_error()) {
                    out_err = "failed to lex bundle source '" + src_path + "'";
                    return false;
                }

                parus::ast::AstArena local_ast{};
                parus::ty::TypePool local_types{};
                parus::ParserFeatureFlags flags{};
                parus::Parser local_parser(local_tokens, local_ast, local_types, &local_bag, /*max_errors=*/128, flags);
                const auto local_root = local_parser.parse_program();
                if (local_bag.has_error()) {
                    out_err = "failed to parse bundle source '" + src_path + "'";
                    return false;
                }

                std::vector<std::string> ns{};
                (void)collect_file_namespace_(local_ast, local_root, ns);
                const std::string decl_file = parus::normalize_path(src_path);
                const std::string module_head = compute_module_head_(bundle_root, decl_file, bundle_name);
                const std::string decl_dir = parent_dir_norm_(decl_file);
                collect_exports_stmt_(local_ast,
                                      local_root,
                                      local_types,
                                      local_sm,
                                      decl_file,
                                      bundle_name,
                                      module_head,
                                      decl_dir,
                                      ns,
                                      out);
            }

            std::unordered_set<std::string> bundle_module_heads{};
            std::unordered_map<std::string, std::unordered_set<std::string>> bundle_local_types_by_module{};
            bundle_module_heads.reserve(out.size());
            bundle_local_types_by_module.reserve(out.size());
            for (const auto& e : out) {
                if (!e.module_head.empty()) bundle_module_heads.insert(e.module_head);
                if ((e.kind == parus::sema::SymbolKind::kType ||
                     e.kind == parus::sema::SymbolKind::kField) &&
                    !e.module_head.empty() &&
                    !e.path.empty() &&
                    e.path.find("::") == std::string::npos) {
                    bundle_local_types_by_module[e.module_head].insert(e.path);
                }
            }

            for (auto& e : out) {
                if ((e.type_repr.empty() && e.type_semantic.empty()) ||
                    bundle_name.empty() ||
                    e.module_head.empty()) {
                    continue;
                }
                const auto it = bundle_local_types_by_module.find(e.module_head);
                const std::unordered_set<std::string> empty_names{};
                const auto& local_names = (it != bundle_local_types_by_module.end()) ? it->second : empty_names;
                parus::ty::TypePool export_type_pool{};
                parus::ty::TypeId qualified_type = parus::ty::kInvalidType;
                if (!e.type_repr.empty()) {
                    const auto parsed_type = parus::cimport::parse_external_type_repr(
                        e.type_repr,
                        std::string_view{},
                        e.inst_payload,
                        export_type_pool
                    );
                    if (parsed_type != parus::ty::kInvalidType) {
                        qualified_type = qualify_type_for_bundle_(
                            parsed_type,
                            export_type_pool,
                            bundle_name,
                            e.module_head,
                            bundle_module_heads,
                            local_names
                        );
                    }
                }
                if (qualified_type == parus::ty::kInvalidType && !e.type_semantic.empty()) {
                    e.type_semantic = qualify_type_semantic_for_bundle_(
                        e.type_semantic,
                        bundle_name,
                        e.module_head,
                        bundle_module_heads,
                        local_names
                    );
                    qualified_type = parus::cimport::parse_external_type_repr(
                        std::string_view{},
                        e.type_semantic,
                        e.inst_payload,
                        export_type_pool
                    );
                }
                if (qualified_type != parus::ty::kInvalidType) {
                    e.type_repr = export_type_pool.to_export_string(qualified_type);
                    e.type_semantic = parus::cimport::serialize_type_semantic_from_type(qualified_type, export_type_pool);
                }
            }

            std::sort(out.begin(), out.end(), [](const ExportSurfaceEntry& a, const ExportSurfaceEntry& b) {
                if (a.path != b.path) return a.path < b.path;
                if (a.kind_text != b.kind_text) return a.kind_text < b.kind_text;
                return a.decl_file < b.decl_file;
            });
            return true;
        }

        void collect_typed_current_export_surface_(
            const parus::ast::AstArena& ast,
            parus::ast::StmtId root,
            const parus::ty::TypePool& types,
            const parus::sema::SymbolTable& sym,
            const parus::SourceManager& sm,
            uint32_t current_file_id,
            std::string_view decl_file,
            std::string_view bundle_name,
            std::string_view module_head,
            std::string_view decl_dir,
            std::vector<ExportSurfaceEntry>& out
        ) {
            out.clear();
            std::vector<std::string> ns{};
            (void)collect_file_namespace_(ast, root, ns);
            collect_exports_stmt_(ast,
                                  root,
                                  types,
                                  sm,
                                  std::string(decl_file),
                                  std::string(bundle_name),
                                  std::string(module_head),
                                  std::string(decl_dir),
                                  ns,
                                  out,
                                  current_file_id);
            canonicalize_export_surface_entries_with_symbols_(out, sym);
            qualify_export_surface_entries_for_bundle_(out, bundle_name);
            dedupe_export_surface_(out);
        }

        std::string join_path_text_(const std::vector<std::string>& segs) {
            std::ostringstream oss{};
            for (size_t i = 0; i < segs.size(); ++i) {
                if (i) oss << "::";
                oss << segs[i];
            }
            return oss.str();
        }

        std::vector<std::string> split_path_text_(std::string_view text) {
            std::vector<std::string> out{};
            if (text.empty()) return out;
            size_t pos = 0;
            while (pos < text.size()) {
                const size_t next = text.find("::", pos);
                const size_t end = (next == std::string_view::npos) ? text.size() : next;
                if (end > pos) {
                    out.emplace_back(text.substr(pos, end - pos));
                }
                if (next == std::string_view::npos) break;
                pos = next + 2;
            }
            return out;
        }

        std::string build_template_lookup_name_(
            std::string_view bundle_name,
            std::string_view module_head,
            std::string_view public_path,
            std::string_view link_name
        ) {
            const std::string seed =
                std::string(bundle_name) + "|" +
                std::string(module_head) + "|" +
                std::string(public_path) + "|" +
                std::string(link_name);
            std::ostringstream hs{};
            hs << std::hex << fnv1a64_(seed);
            return "__parus_template$" + hs.str();
        }

        parus::tyck::ImportedProtoIdentity build_constraint_proto_identity_(
            const parus::ast::AstArena& ast,
            const parus::ast::FnConstraintDecl& cc,
            const parus::ty::TypePool& types,
            std::string_view bundle_name,
            std::string_view current_module_head
        ) {
            parus::tyck::ImportedProtoIdentity out{};
            if (cc.kind != parus::ast::FnConstraintKind::kProto) return out;

            if (cc.rhs_type != parus::ty::kInvalidType) {
                std::vector<std::string_view> rhs_path{};
                std::vector<parus::ty::TypeId> rhs_args{};
                if (types.decompose_named_user(cc.rhs_type, rhs_path, rhs_args) && !rhs_path.empty()) {
                    std::vector<std::string> segs{};
                    segs.reserve(rhs_path.size());
                    for (const auto seg : rhs_path) segs.emplace_back(seg);
                    const bool bundle_qualified =
                        segs.size() >= 3u ||
                        (segs.size() >= 2u && !bundle_name.empty() && segs.front() == bundle_name);
                    if (bundle_qualified) {
                        out.bundle = segs.front();
                        if (segs.size() >= 3u) {
                            std::vector<std::string> head_segs(segs.begin() + 1, segs.end() - 1);
                            out.module_head = normalize_core_public_module_head_(
                                out.bundle,
                                join_path_text_(head_segs)
                            );
                            std::vector<std::string> public_path_segs(segs.begin() + 1, segs.end());
                            out.path = join_path_text_(public_path_segs);
                        } else {
                            out.module_head.clear();
                            out.path = segs.back();
                        }
                        return out;
                    }
                }
            }

            std::vector<std::string> segs{};
            const auto& path_segs = ast.path_segs();
            const uint64_t begin = cc.proto_path_begin;
            const uint64_t end = begin + cc.proto_path_count;
            if (begin <= path_segs.size() && end <= path_segs.size()) {
                segs.reserve(cc.proto_path_count);
                for (uint32_t i = 0; i < cc.proto_path_count; ++i) {
                    segs.emplace_back(path_segs[cc.proto_path_begin + i]);
                }
            }
            out.path = join_path_text_(segs);
            if (out.path.empty()) return out;

            if (segs.size() > 1u) {
                std::vector<std::string> head_segs(segs.begin(), segs.end() - 1);
                const std::string raw_head = join_path_text_(head_segs);
                out.module_head = normalize_core_public_module_head_(bundle_name, raw_head);
                if (bundle_name == "core" ||
                    raw_head == current_module_head ||
                    (!current_module_head.empty() &&
                     current_module_head.starts_with(raw_head + "::"))) {
                    out.bundle = std::string(bundle_name);
                }
            } else {
                out.module_head = normalize_core_public_module_head_(bundle_name, current_module_head);
                out.bundle = std::string(bundle_name);
            }
            return out;
        }

        bool serialize_top_level_generic_decl_sidecar_(
            const parus::ast::AstArena& ast,
            parus::ast::StmtId root_sid,
            const parus::ty::TypePool& types,
            const parus::SourceManager& sm,
            std::string_view decl_file,
            std::string_view bundle_name,
            std::string_view module_head,
            std::string_view public_path,
            std::string_view link_name_override,
            TemplateSidecarFunction& out,
            std::string& out_err
        ) {
            out = TemplateSidecarFunction{};
            out_err.clear();
            if (root_sid == parus::ast::k_invalid_stmt || static_cast<size_t>(root_sid) >= ast.stmts().size()) {
                out_err = "invalid template sidecar root";
                return false;
            }

            const auto& root_decl = ast.stmt(root_sid);
            const bool is_free_fn =
                root_decl.kind == parus::ast::StmtKind::kFnDecl &&
                root_decl.a != parus::ast::k_invalid_stmt;
            const bool is_proto = root_decl.kind == parus::ast::StmtKind::kProtoDecl;
            const bool is_acts = root_decl.kind == parus::ast::StmtKind::kActsDecl;
            const bool is_class = root_decl.kind == parus::ast::StmtKind::kClassDecl;
            const bool is_field = root_decl.kind == parus::ast::StmtKind::kFieldDecl;
            const bool is_enum = root_decl.kind == parus::ast::StmtKind::kEnumDecl;
            if (!is_free_fn && !is_proto && !is_acts && !is_class && !is_field && !is_enum) {
                out_err = "template sidecar requires top-level fn/proto/acts/class/struct/enum declaration";
                return false;
            }

            out.bundle = std::string(bundle_name);
            out.module_head = normalize_core_public_module_head_(bundle_name, module_head);
            out.public_path = std::string(public_path);
            if (is_free_fn) {
                out.link_name = !link_name_override.empty()
                    ? std::string(link_name_override)
                    : build_function_link_name_(
                        bundle_name,
                        public_path,
                        root_decl.fn_mode,
                        (root_decl.type != parus::ty::kInvalidType) ? types.to_export_string(root_decl.type) : std::string("def(?)"),
                        root_decl.link_abi == parus::ast::LinkAbi::kC
                    );
            }
            out.decl_file = std::string(decl_file);
            const auto lc = sm.line_col(root_decl.span.file_id, root_decl.span.lo);
            out.decl_line = lc.line;
            out.decl_col = lc.col;
            const auto root_type_contains_unresolved_generic_param = [&](const auto& self,
                                                                        parus::ty::TypeId t,
                                                                        bool nested) -> bool {
                if (t == parus::ty::kInvalidType || t >= types.count()) return false;
                const auto& tt = types.get(t);
                switch (tt.kind) {
                    case parus::ty::Kind::kNamedUser: {
                        std::vector<std::string_view> path{};
                        std::vector<parus::ty::TypeId> args{};
                        if (!types.decompose_named_user(t, path, args) || path.empty()) return false;
                        if (nested && args.empty() && path.size() == 1) {
                            return true;
                        }
                        for (const auto arg_t : args) {
                            if (self(self, arg_t, /*nested=*/true)) return true;
                        }
                        return false;
                    }
                    case parus::ty::Kind::kOptional:
                    case parus::ty::Kind::kArray:
                    case parus::ty::Kind::kBorrow:
                    case parus::ty::Kind::kEscape:
                    case parus::ty::Kind::kPtr:
                        return self(self, tt.elem, /*nested=*/true);
                    case parus::ty::Kind::kFn:
                        if (self(self, tt.ret, /*nested=*/true)) return true;
                        for (uint32_t i = 0; i < tt.param_count; ++i) {
                            if (self(self, types.fn_param_at(t, i), /*nested=*/true)) return true;
                        }
                        return false;
                    default:
                        return false;
                }
            };
            const auto root_acts_is_generic_template = [&]() {
                if (!is_acts) return false;
                if (root_decl.decl_generic_param_count > 0) return true;
                if (root_type_contains_unresolved_generic_param(
                        root_type_contains_unresolved_generic_param,
                        root_decl.acts_target_type,
                        /*nested=*/false)) {
                    return true;
                }
                const auto& witnesses = ast.acts_assoc_type_witness_decls();
                const uint64_t begin = root_decl.acts_assoc_witness_begin;
                const uint64_t end = begin + root_decl.acts_assoc_witness_count;
                if (begin > witnesses.size() || end > witnesses.size()) return false;
                for (uint32_t i = 0; i < root_decl.acts_assoc_witness_count; ++i) {
                    const auto& witness = witnesses[root_decl.acts_assoc_witness_begin + i];
                    if (root_type_contains_unresolved_generic_param(
                            root_type_contains_unresolved_generic_param,
                            witness.rhs_type,
                            /*nested=*/false)) {
                        return true;
                    }
                }
                return false;
            };
            const bool root_is_generic_template =
                is_free_fn
                    ? (root_decl.fn_generic_param_count > 0)
                    : (is_acts
                        ? root_acts_is_generic_template()
                        : (root_decl.decl_generic_param_count > 0));
            out.is_public_export =
                root_decl.is_export &&
                (root_is_generic_template || is_free_fn);
            if (root_decl.type != parus::ty::kInvalidType) {
                out.declared_type_repr = types.to_export_string(root_decl.type);
                out.declared_type_semantic =
                    parus::cimport::serialize_type_semantic_from_type(root_decl.type, types);
            }

            const auto type_repr_or_empty = [&](parus::ty::TypeId tid) -> std::string {
                return (tid != parus::ty::kInvalidType) ? types.to_export_string(tid) : std::string{};
            };
            const auto canonical_type_repr_or_empty = [&](parus::ty::TypeId tid) -> std::string {
                std::string repr = type_repr_or_empty(tid);
                if (tid == parus::ty::kInvalidType || repr.empty()) return repr;
                const auto& tt = types.get(tid);
                if (tt.kind != parus::ty::Kind::kNamedUser) return repr;
                std::vector<std::string_view> path{};
                std::vector<parus::ty::TypeId> args{};
                if (!types.decompose_named_user(tid, path, args) || path.empty() || path.size() > 1) {
                    return repr;
                }
                const std::string canonical_head =
                    normalize_core_public_module_head_(bundle_name, module_head);
                if (canonical_head.empty()) return repr;
                return canonical_head + "::" + repr;
            };

            std::string lookup_seed = out.public_path;
            if (lookup_seed.empty()) {
                if (is_acts && root_decl.acts_target_type != parus::ty::kInvalidType) {
                    lookup_seed = "__acts_for$" + canonical_type_repr_or_empty(root_decl.acts_target_type);
                } else if (!root_decl.name.empty()) {
                    lookup_seed = std::string(root_decl.name);
                } else {
                    lookup_seed = "__template_root$" + std::to_string(out.decl_line) + ":" + std::to_string(out.decl_col);
                }
            }
            out.lookup_name = build_template_lookup_name_(out.bundle, out.module_head, lookup_seed, out.link_name);

            const auto& gps = ast.generic_param_decls();
            const auto& constraints = ast.fn_constraint_decls();
            const uint32_t root_gp_begin =
                is_free_fn ? root_decl.fn_generic_param_begin : root_decl.decl_generic_param_begin;
            const uint32_t root_gp_count =
                is_free_fn ? root_decl.fn_generic_param_count : root_decl.decl_generic_param_count;
            const uint32_t root_cc_begin =
                is_free_fn ? root_decl.fn_constraint_begin : root_decl.decl_constraint_begin;
            const uint32_t root_cc_count =
                is_free_fn ? root_decl.fn_constraint_count : root_decl.decl_constraint_count;
            for (uint32_t i = 0; i < root_gp_count; ++i) {
                const uint32_t idx = root_gp_begin + i;
                if (idx >= gps.size()) break;
                TemplateSidecarGenericParam gp{};
                gp.name = std::string(gps[idx].name);
                out.generic_params.push_back(std::move(gp));
            }
            for (uint32_t i = 0; i < root_cc_count; ++i) {
                const uint32_t idx = root_cc_begin + i;
                if (idx >= constraints.size()) break;
                const auto& cc = constraints[idx];
                TemplateSidecarFnConstraint meta{};
                meta.kind = static_cast<uint8_t>(cc.kind);
                meta.type_param = std::string(cc.type_param);
                meta.rhs_type_repr = type_repr_or_empty(cc.rhs_type);
                meta.proto = build_constraint_proto_identity_(ast, cc, types, bundle_name, module_head);
                out.constraints.push_back(std::move(meta));
            }

            if (is_proto || is_class || is_field) {
                const auto& refs = ast.path_refs();
                const uint64_t begin = root_decl.decl_path_ref_begin;
                const uint64_t end = begin + root_decl.decl_path_ref_count;
                if (begin > refs.size() || end > refs.size()) {
                    out_err = "typed template payload v2 type path-ref range out of bounds";
                    return false;
                }
                for (uint32_t i = 0; i < root_decl.decl_path_ref_count; ++i) {
                    const auto& pr = refs[root_decl.decl_path_ref_begin + i];
                    TemplateSidecarPathRef out_ref{};
                    if (pr.path_count > 0) {
                        const auto& segs = ast.path_segs();
                        const uint64_t pbegin = pr.path_begin;
                        const uint64_t pend = pbegin + pr.path_count;
                        if (pbegin <= segs.size() && pend <= segs.size()) {
                            std::vector<std::string> path_text{};
                            path_text.reserve(pr.path_count);
                            for (uint32_t j = 0; j < pr.path_count; ++j) {
                                path_text.emplace_back(segs[pr.path_begin + j]);
                            }
                            out_ref.path = join_path_text_(path_text);
                        }
                    }
                    out_ref.type_repr = canonical_type_repr_or_empty(pr.type);
                    if (pr.type != parus::ty::kInvalidType) {
                        out_ref.type_semantic =
                            parus::cimport::serialize_type_semantic_from_type(pr.type, types);
                    }
                    out.path_refs.push_back(std::move(out_ref));
                }
            }

            if (is_class || is_field) {
                const auto& members = ast.field_members();
                const uint64_t begin = root_decl.field_member_begin;
                const uint64_t end = begin + root_decl.field_member_count;
                if (begin > members.size() || end > members.size()) {
                    out_err = "typed template payload v2 field-member range out of bounds";
                    return false;
                }
                for (uint32_t i = 0; i < root_decl.field_member_count; ++i) {
                    const auto& member = members[root_decl.field_member_begin + i];
                    TemplateSidecarFieldMember out_member{};
                    out_member.name = std::string(member.name);
                    out_member.type_repr = canonical_type_repr_or_empty(member.type);
                    if (member.type != parus::ty::kInvalidType) {
                        out_member.type_semantic =
                            parus::cimport::serialize_type_semantic_from_type(member.type, types);
                    }
                    out_member.visibility = static_cast<uint8_t>(member.visibility);
                    out.field_members.push_back(std::move(out_member));
                }
            }

            if (is_enum) {
                const auto& variants = ast.enum_variant_decls();
                const uint64_t vbegin = root_decl.enum_variant_begin;
                const uint64_t vend = vbegin + root_decl.enum_variant_count;
                if (vbegin > variants.size() || vend > variants.size()) {
                    out_err = "typed template payload v2 enum variant range out of bounds";
                    return false;
                }

                const auto& members = ast.field_members();
                for (uint32_t i = 0; i < root_decl.enum_variant_count; ++i) {
                    const auto& variant = variants[root_decl.enum_variant_begin + i];
                    TemplateSidecarEnumVariant out_variant{};
                    out_variant.name = std::string(variant.name);
                    out_variant.payload_begin = static_cast<uint32_t>(out.field_members.size());
                    out_variant.payload_count = variant.payload_count;
                    out_variant.has_discriminant = variant.has_discriminant;
                    out_variant.discriminant = variant.discriminant;

                    const uint64_t pbegin = variant.payload_begin;
                    const uint64_t pend = pbegin + variant.payload_count;
                    if (pbegin > members.size() || pend > members.size()) {
                        out_err = "typed template payload v2 enum payload field-member range out of bounds";
                        return false;
                    }
                    for (uint32_t mi = 0; mi < variant.payload_count; ++mi) {
                        const auto& member = members[variant.payload_begin + mi];
                        TemplateSidecarFieldMember out_member{};
                        out_member.name = std::string(member.name);
                        out_member.type_repr = canonical_type_repr_or_empty(member.type);
                        if (member.type != parus::ty::kInvalidType) {
                            out_member.type_semantic =
                                parus::cimport::serialize_type_semantic_from_type(member.type, types);
                        }
                        out_member.visibility = static_cast<uint8_t>(member.visibility);
                        out.field_members.push_back(std::move(out_member));
                    }
                    out.enum_variants.push_back(std::move(out_variant));
                }
            }

            if (is_acts) {
                const auto& witnesses = ast.acts_assoc_type_witness_decls();
                const uint64_t begin = root_decl.acts_assoc_witness_begin;
                const uint64_t end = begin + root_decl.acts_assoc_witness_count;
                if (begin > witnesses.size() || end > witnesses.size()) {
                    out_err = "typed template payload v2 acts witness range out of bounds";
                    return false;
                }
                for (uint32_t i = 0; i < root_decl.acts_assoc_witness_count; ++i) {
                    const auto& witness = witnesses[root_decl.acts_assoc_witness_begin + i];
                    TemplateSidecarActsAssocWitness out_w{};
                    out_w.assoc_name = std::string(witness.assoc_name);
                    out_w.rhs_type_repr = canonical_type_repr_or_empty(witness.rhs_type);
                    if (witness.rhs_type != parus::ty::kInvalidType) {
                        out_w.rhs_type_semantic =
                            parus::cimport::serialize_type_semantic_from_type(witness.rhs_type, types);
                    }
                    out.acts_assoc_witnesses.push_back(std::move(out_w));
                }
            }

            std::unordered_map<parus::ast::ExprId, uint32_t> expr_map{};
            std::unordered_map<parus::ast::StmtId, uint32_t> stmt_map{};
            std::function<uint32_t(parus::ast::ExprId)> serialize_expr{};
            std::function<uint32_t(parus::ast::StmtId)> serialize_stmt{};

            serialize_expr = [&](parus::ast::ExprId eid) -> uint32_t {
                if (eid == parus::ast::k_invalid_expr || static_cast<size_t>(eid) >= ast.exprs().size()) {
                    return parus::ast::k_invalid_expr;
                }
                if (auto it = expr_map.find(eid); it != expr_map.end()) return it->second;

                const auto idx = static_cast<uint32_t>(out.exprs.size());
                expr_map[eid] = idx;
                out.exprs.push_back(TemplateSidecarExpr{});
                TemplateSidecarExpr se{};
                const auto& e = ast.expr(eid);
                se.kind = static_cast<uint8_t>(e.kind);
                se.op = static_cast<uint8_t>(e.op);
                se.unary_is_mut = e.unary_is_mut;
                se.text = std::string(e.text);
                se.string_is_raw = e.string_is_raw;
                se.string_is_format = e.string_is_format;
                se.string_folded_text = std::string(e.string_folded_text);
                se.call_from_pipe = e.call_from_pipe;
                se.loop_has_header = e.loop_has_header;
                se.loop_var = std::string(e.loop_var);
                se.cast_kind = static_cast<uint8_t>(e.cast_kind);
                se.cast_type_repr = type_repr_or_empty(e.cast_type);
                if (e.cast_type != parus::ty::kInvalidType) {
                    se.cast_type_semantic =
                        parus::cimport::serialize_type_semantic_from_type(e.cast_type, types);
                }
                se.target_type_repr = type_repr_or_empty(e.target_type);
                if (e.target_type != parus::ty::kInvalidType) {
                    se.target_type_semantic =
                        parus::cimport::serialize_type_semantic_from_type(e.target_type, types);
                }
                if (e.kind == parus::ast::ExprKind::kFieldInit) {
                    if (e.field_init_type_node != parus::ast::k_invalid_type_node &&
                        static_cast<size_t>(e.field_init_type_node) < ast.type_nodes().size()) {
                        const auto& tn = ast.type_node(e.field_init_type_node);
                        se.field_init_type_repr = canonical_type_repr_or_empty(tn.resolved_type);
                        if (tn.resolved_type != parus::ty::kInvalidType) {
                            se.field_init_type_semantic =
                                parus::cimport::serialize_type_semantic_from_type(tn.resolved_type, types);
                        }
                    }
                    if (se.field_init_type_repr.empty()) {
                        se.field_init_type_repr = std::string(e.text);
                    }
                }

                se.a = serialize_expr(e.a);
                se.b = serialize_expr(e.b);
                se.c = serialize_expr(e.c);
                se.block_tail = serialize_expr(e.block_tail);
                se.loop_iter = serialize_expr(e.loop_iter);
                se.block_stmt = serialize_stmt(e.block_stmt);
                se.loop_body = serialize_stmt(e.loop_body);

                const auto& args = ast.args();
                const uint64_t arg_begin = e.arg_begin;
                const uint64_t arg_end = arg_begin + e.arg_count;
                if (arg_begin <= args.size() && arg_end <= args.size()) {
                    se.arg_begin = static_cast<uint32_t>(out.args.size());
                    for (uint32_t i = 0; i < e.arg_count; ++i) {
                        const auto& a = args[e.arg_begin + i];
                        TemplateSidecarArg sa{};
                        sa.kind = static_cast<uint8_t>(a.kind);
                        sa.has_label = a.has_label;
                        sa.is_hole = a.is_hole;
                        sa.label = std::string(a.label);
                        sa.expr = serialize_expr(a.expr);
                        out.args.push_back(std::move(sa));
                    }
                    se.arg_count = e.arg_count;
                }

                const auto& type_args = ast.type_args();
                const uint64_t type_begin = e.call_type_arg_begin;
                const uint64_t type_end = type_begin + e.call_type_arg_count;
                if (type_begin <= type_args.size() && type_end <= type_args.size()) {
                    se.call_type_arg_begin = static_cast<uint32_t>(out.type_args.size());
                    for (uint32_t i = 0; i < e.call_type_arg_count; ++i) {
                        out.type_args.push_back(type_repr_or_empty(type_args[e.call_type_arg_begin + i]));
                    }
                    se.call_type_arg_count = e.call_type_arg_count;
                }

                const auto& inits = ast.field_init_entries();
                const uint64_t init_begin = e.field_init_begin;
                const uint64_t init_end = init_begin + e.field_init_count;
                if (init_begin <= inits.size() && init_end <= inits.size()) {
                    se.field_init_begin = static_cast<uint32_t>(out.field_inits.size());
                    for (uint32_t i = 0; i < e.field_init_count; ++i) {
                        const auto& init = inits[e.field_init_begin + i];
                        TemplateSidecarFieldInit si{};
                        si.name = std::string(init.name);
                        si.expr = serialize_expr(init.expr);
                        out.field_inits.push_back(std::move(si));
                    }
                    se.field_init_count = e.field_init_count;
                }

                const auto& parts = ast.fstring_parts();
                const uint64_t part_begin = e.string_part_begin;
                const uint64_t part_end = part_begin + e.string_part_count;
                if (part_begin <= parts.size() && part_end <= parts.size()) {
                    se.string_part_begin = static_cast<uint32_t>(out.fstring_parts.size());
                    for (uint32_t i = 0; i < e.string_part_count; ++i) {
                        const auto& part = parts[e.string_part_begin + i];
                        TemplateSidecarFStringPart sp{};
                        sp.is_expr = part.is_expr;
                        sp.text = std::string(part.text);
                        sp.expr = serialize_expr(part.expr);
                        out.fstring_parts.push_back(std::move(sp));
                    }
                    se.string_part_count = e.string_part_count;
                }

                out.exprs[idx] = std::move(se);
                return idx;
            };

            serialize_stmt = [&](parus::ast::StmtId sid) -> uint32_t {
                if (sid == parus::ast::k_invalid_stmt || static_cast<size_t>(sid) >= ast.stmts().size()) {
                    return parus::ast::k_invalid_stmt;
                }
                if (auto it = stmt_map.find(sid); it != stmt_map.end()) return it->second;

                const auto idx = static_cast<uint32_t>(out.stmts.size());
                stmt_map[sid] = idx;
                out.stmts.push_back(TemplateSidecarStmt{});
                TemplateSidecarStmt ss{};
                const auto& s = ast.stmt(sid);
                ss.kind = static_cast<uint8_t>(s.kind);
                ss.expr = serialize_expr(s.expr);
                ss.init = serialize_expr(s.init);
                ss.a = serialize_stmt(s.a);
                ss.b = serialize_stmt(s.b);
                ss.case_begin = 0;
                ss.case_count = 0;
                ss.has_default = s.has_default;
                ss.is_set = s.is_set;
                ss.is_mut = s.is_mut;
                ss.is_static = s.is_static;
                ss.is_const = s.is_const;
                ss.is_extern = s.is_extern;
                ss.link_abi = static_cast<uint8_t>(s.link_abi);
                ss.name = std::string(s.name);
                ss.type_repr = type_repr_or_empty(s.type);
                if (s.type != parus::ty::kInvalidType) {
                    ss.type_semantic = parus::cimport::serialize_type_semantic_from_type(s.type, types);
                }
                ss.is_export = s.is_export;
                ss.fn_mode = static_cast<uint8_t>(s.fn_mode);
                ss.fn_ret_repr = type_repr_or_empty(s.fn_ret);
                if (s.fn_ret != parus::ty::kInvalidType) {
                    ss.fn_ret_semantic = parus::cimport::serialize_type_semantic_from_type(s.fn_ret, types);
                }
                ss.member_visibility = static_cast<uint8_t>(s.member_visibility);
                ss.is_pure = s.is_pure;
                ss.is_comptime = s.is_comptime;
                ss.is_commit = s.is_commit;
                ss.is_recast = s.is_recast;
                ss.is_throwing = s.is_throwing;
                ss.fn_is_const = s.fn_is_const;
                ss.param_begin = 0;
                ss.param_count = 0;
                ss.positional_param_count = s.positional_param_count;
                ss.has_named_group = s.has_named_group;
                ss.fn_is_c_variadic = s.fn_is_c_variadic;
                ss.fn_is_proto_sig = s.fn_is_proto_sig;
                ss.fn_generic_param_begin = s.fn_generic_param_begin;
                ss.fn_generic_param_count = s.fn_generic_param_count;
                ss.fn_constraint_begin = s.fn_constraint_begin;
                ss.fn_constraint_count = s.fn_constraint_count;
                ss.decl_generic_param_begin = s.decl_generic_param_begin;
                ss.decl_generic_param_count = s.decl_generic_param_count;
                ss.decl_constraint_begin = s.decl_constraint_begin;
                ss.decl_constraint_count = s.decl_constraint_count;
                ss.decl_path_ref_begin = s.decl_path_ref_begin;
                ss.decl_path_ref_count = s.decl_path_ref_count;
                ss.field_layout = static_cast<uint8_t>(s.field_layout);
                ss.field_align = s.field_align;
                ss.field_member_begin = s.field_member_begin;
                ss.field_member_count = s.field_member_count;
                ss.enum_variant_begin = s.enum_variant_begin;
                ss.enum_variant_count = s.enum_variant_count;
                ss.proto_fn_role = static_cast<uint8_t>(s.proto_fn_role);
                ss.proto_require_kind = static_cast<uint8_t>(s.proto_require_kind);
                ss.assoc_type_role = static_cast<uint8_t>(s.assoc_type_role);
                ss.var_is_proto_provide = s.var_is_proto_provide;
                ss.acts_is_for = s.acts_is_for;
                ss.acts_has_set_name = s.acts_has_set_name;
                ss.acts_target_type_repr = canonical_type_repr_or_empty(s.acts_target_type);
                if (s.acts_target_type != parus::ty::kInvalidType) {
                    ss.acts_target_type_semantic =
                        parus::cimport::serialize_type_semantic_from_type(s.acts_target_type, types);
                }
                ss.acts_assoc_witness_begin = s.acts_assoc_witness_begin;
                ss.acts_assoc_witness_count = s.acts_assoc_witness_count;
                ss.manual_perm_mask = s.manual_perm_mask;
                ss.var_has_consume_else = s.var_has_consume_else;

                const bool has_stmt_children =
                    s.kind == parus::ast::StmtKind::kBlock ||
                    s.kind == parus::ast::StmtKind::kProtoDecl ||
                    s.kind == parus::ast::StmtKind::kActsDecl ||
                    s.kind == parus::ast::StmtKind::kClassDecl;
                if (has_stmt_children) {
                    const auto& kids = ast.stmt_children();
                    const uint64_t begin = s.stmt_begin;
                    const uint64_t end = begin + s.stmt_count;
                    if (begin <= kids.size() && end <= kids.size()) {
                        ss.stmt_begin = static_cast<uint32_t>(out.stmt_children.size());
                        out.stmt_children.resize(out.stmt_children.size() + s.stmt_count,
                                                 parus::ast::k_invalid_stmt);
                        for (uint32_t i = 0; i < s.stmt_count; ++i) {
                            out.stmt_children[ss.stmt_begin + i] =
                                serialize_stmt(kids[s.stmt_begin + i]);
                        }
                        ss.stmt_count = s.stmt_count;
                    }
                    out.stmts[idx] = std::move(ss);
                    return idx;
                }

                if (s.kind == parus::ast::StmtKind::kFnDecl) {
                    if (sid != root_sid && (s.fn_generic_param_count > 0 || s.fn_constraint_count > 0)) {
                        out.stmts[idx] = std::move(ss);
                        out_err = "typed template payload v2 currently does not support generic proto/acts/class member functions";
                        return idx;
                    }
                    ss.param_begin = static_cast<uint32_t>(out.params.size());
                    for (uint32_t i = 0; i < s.param_count; ++i) {
                        const auto& p = ast.params()[s.param_begin + i];
                        TemplateSidecarParam sp{};
                        sp.name = std::string(p.name);
                        sp.type_repr = type_repr_or_empty(p.type);
                        if (p.type != parus::ty::kInvalidType) {
                            sp.type_semantic =
                                parus::cimport::serialize_type_semantic_from_type(p.type, types);
                        }
                        sp.is_mut = p.is_mut;
                        sp.is_self = p.is_self;
                        sp.self_kind = static_cast<uint8_t>(p.self_kind);
                        sp.has_default = p.has_default;
                        sp.default_expr = serialize_expr(p.default_expr);
                        sp.is_named_group = p.is_named_group;
                        out.params.push_back(std::move(sp));
                    }
                    ss.param_count = s.param_count;
                    if (sid == root_sid && is_free_fn) {
                        ss.fn_generic_param_begin = 0;
                        ss.fn_generic_param_count = static_cast<uint32_t>(out.generic_params.size());
                        ss.fn_constraint_begin = 0;
                        ss.fn_constraint_count = static_cast<uint32_t>(out.constraints.size());
                    }
                    out.stmts[idx] = std::move(ss);
                    return idx;
                }

                if (s.kind == parus::ast::StmtKind::kSwitch) {
                    const auto& cases = ast.switch_cases();
                    const uint64_t begin = s.case_begin;
                    const uint64_t end = begin + s.case_count;
                    if (begin <= cases.size() && end <= cases.size()) {
                        ss.case_begin = static_cast<uint32_t>(out.switch_cases.size());
                        ss.case_count = s.case_count;
                        for (uint32_t i = 0; i < s.case_count; ++i) {
                            const auto& sc = cases[s.case_begin + i];
                            TemplateSidecarSwitchCase out_sc{};
                            out_sc.is_default = sc.is_default;
                            out_sc.pat_kind = static_cast<uint8_t>(sc.pat_kind);
                            out_sc.pat_text = std::string(sc.pat_text);
                            if (sc.enum_type != parus::ty::kInvalidType) {
                                out_sc.enum_type_repr = types.to_export_string(sc.enum_type);
                                out_sc.enum_type_semantic =
                                    parus::cimport::serialize_type_semantic_from_type(sc.enum_type, types);
                            }
                            out_sc.enum_variant_name = std::string(sc.enum_variant_name);
                            out_sc.enum_bind_begin = static_cast<uint32_t>(out.switch_enum_binds.size());
                            out_sc.enum_bind_count = sc.enum_bind_count;
                            const auto& binds = ast.switch_enum_binds();
                            const uint64_t bb = sc.enum_bind_begin;
                            const uint64_t be = bb + sc.enum_bind_count;
                            if (bb <= binds.size() && be <= binds.size()) {
                                for (uint32_t bi = sc.enum_bind_begin; bi < sc.enum_bind_begin + sc.enum_bind_count; ++bi) {
                                    const auto& sb = binds[bi];
                                    TemplateSidecarSwitchEnumBind out_bind{};
                                    out_bind.field_name = std::string(sb.field_name);
                                    out_bind.bind_name = std::string(sb.bind_name);
                                    if (sb.bind_type != parus::ty::kInvalidType) {
                                        out_bind.bind_type_repr = types.to_export_string(sb.bind_type);
                                        out_bind.bind_type_semantic =
                                            parus::cimport::serialize_type_semantic_from_type(sb.bind_type, types);
                                    }
                                    out.switch_enum_binds.push_back(std::move(out_bind));
                                }
                            } else {
                                out_sc.enum_bind_begin = 0;
                                out_sc.enum_bind_count = 0;
                            }
                            out_sc.body = serialize_stmt(sc.body);
                            out.switch_cases.push_back(std::move(out_sc));
                        }
                    }
                    out.stmts[idx] = std::move(ss);
                    return idx;
                }

                switch (s.kind) {
                    case parus::ast::StmtKind::kEmpty:
                    case parus::ast::StmtKind::kExprStmt:
                    case parus::ast::StmtKind::kVar:
                    case parus::ast::StmtKind::kIf:
                    case parus::ast::StmtKind::kFor:
                    case parus::ast::StmtKind::kWhile:
                    case parus::ast::StmtKind::kDoScope:
                    case parus::ast::StmtKind::kDoWhile:
                    case parus::ast::StmtKind::kManual:
                    case parus::ast::StmtKind::kReturn:
                    case parus::ast::StmtKind::kRequire:
                    case parus::ast::StmtKind::kThrow:
                    case parus::ast::StmtKind::kBreak:
                    case parus::ast::StmtKind::kContinue:
                    case parus::ast::StmtKind::kAssocTypeDecl:
                    case parus::ast::StmtKind::kFieldDecl:
                    case parus::ast::StmtKind::kEnumDecl:
                        out.stmts[idx] = std::move(ss);
                        return idx;
                    default:
                        out.stmts[idx] = std::move(ss);
                        out_err = "typed template payload v2 currently supports generic fn/proto/acts/class declarations without nested type/control extensions";
                        return idx;
                }
            };

            out.root_stmt = serialize_stmt(root_sid);
            return out_err.empty();
        }

        std::string template_sidecar_identity_key_(const TemplateSidecarFunction& entry) {
            uint32_t kind = 0;
            if (entry.root_stmt != parus::ast::k_invalid_stmt &&
                entry.root_stmt < entry.stmts.size()) {
                kind = entry.stmts[entry.root_stmt].kind;
            }

            const std::string path_key =
                !entry.public_path.empty() ? entry.public_path : entry.lookup_name;
            std::string key = entry.bundle;
            key.push_back('|');
            key.append(std::to_string(kind));
            key.push_back('|');
            key.append(entry.module_head);
            key.push_back('|');
            key.append(path_key);
            key.push_back('|');
            key.append(entry.link_name);
            if (kind == static_cast<uint32_t>(parus::ast::StmtKind::kActsDecl) &&
                entry.root_stmt != parus::ast::k_invalid_stmt &&
                entry.root_stmt < entry.stmts.size()) {
                const auto& root = entry.stmts[entry.root_stmt];
                key.append("|owner=");
                key.append(root.acts_target_type_repr);
                key.append("|w=");
                for (size_t i = 0; i < entry.acts_assoc_witnesses.size(); ++i) {
                    const auto& witness = entry.acts_assoc_witnesses[i];
                    if (i) key.push_back(';');
                    key.append(witness.assoc_name);
                    key.push_back('=');
                    key.append(witness.rhs_type_repr);
                }
                key.append("|c=");
                for (size_t i = 0; i < entry.constraints.size(); ++i) {
                    const auto& cc = entry.constraints[i];
                    if (i) key.push_back(';');
                    key.append(std::to_string(static_cast<uint32_t>(cc.kind)));
                    key.push_back(':');
                    key.append(cc.type_param);
                    key.push_back(':');
                    key.append(cc.rhs_type_repr);
                    key.push_back(':');
                    key.append(cc.proto.bundle);
                    key.push_back(':');
                    key.append(cc.proto.module_head);
                    key.push_back(':');
                    key.append(cc.proto.path);
                }
                key.append("|m=");
                const uint64_t begin = root.stmt_begin;
                const uint64_t end = begin + root.stmt_count;
                if (begin <= entry.stmt_children.size() && end <= entry.stmt_children.size()) {
                    for (uint32_t i = 0; i < root.stmt_count; ++i) {
                        const auto child_sid = entry.stmt_children[root.stmt_begin + i];
                        if (child_sid == parus::ast::k_invalid_stmt ||
                            child_sid >= entry.stmts.size()) {
                            continue;
                        }
                        const auto& member = entry.stmts[child_sid];
                        if (i) key.push_back(';');
                        key.append(std::to_string(member.kind));
                        key.push_back(':');
                        key.append(member.name);
                    }
                }
            }
            return key;
        }

        std::string template_sidecar_payload_fingerprint_(const TemplateSidecarFunction& entry) {
            std::ostringstream oss;
            oss << entry.bundle << "|"
                << entry.module_head << "|"
                << entry.public_path << "|"
                << entry.link_name << "|"
                << entry.lookup_name << "|"
                << entry.declared_type_repr << "|"
                << entry.declared_type_semantic << "|"
                << entry.root_stmt << "|";
            oss << "stmts=" << entry.stmts.size() << ";";
            for (const auto& s : entry.stmts) {
                oss << static_cast<uint32_t>(s.kind) << ":"
                    << s.name << ":"
                    << s.type_repr << ":"
                    << s.fn_ret_repr << ":"
                    << s.acts_target_type_repr << ":"
                    << static_cast<uint32_t>(s.member_visibility) << ":"
                    << static_cast<uint32_t>(s.field_layout) << ":"
                    << s.stmt_count << ";";
            }
            oss << "params=" << entry.params.size() << ";";
            for (const auto& p : entry.params) {
                oss << p.name << ":" << p.type_repr << ":" << p.type_semantic << ":"
                    << p.is_mut << ":" << p.is_self << ":" << static_cast<uint32_t>(p.self_kind) << ";";
            }
            oss << "constraints=" << entry.constraints.size() << ";";
            for (const auto& c : entry.constraints) {
                oss << static_cast<uint32_t>(c.kind) << ":" << c.type_param << ":"
                    << c.rhs_type_repr << ":" << c.proto.bundle << ":"
                    << c.proto.module_head << ":" << c.proto.path << ";";
            }
            oss << "path_refs=" << entry.path_refs.size() << ";";
            for (const auto& pr : entry.path_refs) {
                oss << pr.path << ":" << pr.type_repr << ":" << pr.type_semantic << ";";
            }
            oss << "field_members=" << entry.field_members.size() << ";";
            for (const auto& fm : entry.field_members) {
                oss << fm.name << ":" << fm.type_repr << ":" << fm.type_semantic << ":"
                    << static_cast<uint32_t>(fm.visibility) << ";";
            }
            oss << "enum_variants=" << entry.enum_variants.size() << ";";
            for (const auto& ev : entry.enum_variants) {
                oss << ev.name << ":" << ev.payload_begin << ":" << ev.payload_count << ":"
                    << ev.has_discriminant << ":" << ev.discriminant << ";";
            }
            oss << "witnesses=" << entry.acts_assoc_witnesses.size() << ";";
            for (const auto& w : entry.acts_assoc_witnesses) {
                oss << w.assoc_name << ":" << w.rhs_type_repr << ":" << w.rhs_type_semantic << ";";
            }
            return oss.str();
        }

        bool dedupe_template_sidecars_(std::vector<TemplateSidecarFunction>& entries, std::string* out_err = nullptr) {
            std::unordered_map<std::string, std::string> fingerprint_by_key{};
            std::vector<TemplateSidecarFunction> deduped{};
            deduped.reserve(entries.size());
            for (auto& e : entries) {
                const std::string key = template_sidecar_identity_key_(e);
                const std::string fingerprint = template_sidecar_payload_fingerprint_(e);
                if (auto it = fingerprint_by_key.find(key); it != fingerprint_by_key.end()) {
                    if (it->second != fingerprint) {
                        if (out_err != nullptr) {
                            *out_err = "conflicting canonical template-sidecar identity: " + key;
                        }
                        return false;
                    }
                    continue;
                }
                fingerprint_by_key.emplace(key, fingerprint);
                deduped.push_back(std::move(e));
            }
            entries = std::move(deduped);
            return true;
        }

        bool collect_typed_current_template_sidecars_(
            const parus::ast::AstArena& ast,
            parus::ast::StmtId root,
            const parus::ty::TypePool& types,
            const parus::SourceManager& sm,
            uint32_t current_file_id,
            std::string_view decl_file,
            std::string_view bundle_name,
            std::string_view module_head,
            const std::vector<ExportSurfaceEntry>& current_exports,
            std::vector<TemplateSidecarFunction>& out,
            std::string& out_err
        ) {
            out.clear();
            out_err.clear();

            if (root == parus::ast::k_invalid_stmt || static_cast<size_t>(root) >= ast.stmts().size()) {
                return true;
            }
            const auto& rs = ast.stmt(root);
            if (rs.kind != parus::ast::StmtKind::kBlock) return true;

            const auto type_contains_unresolved_generic_param = [&](const auto& self,
                                                                   parus::ty::TypeId t,
                                                                   bool nested) -> bool {
                if (t == parus::ty::kInvalidType || t >= types.count()) return false;
                const auto& tt = types.get(t);
                switch (tt.kind) {
                    case parus::ty::Kind::kNamedUser: {
                        std::vector<std::string_view> path{};
                        std::vector<parus::ty::TypeId> args{};
                        if (!types.decompose_named_user(t, path, args) || path.empty()) return false;
                        if (nested && args.empty() && path.size() == 1) {
                            return true;
                        }
                        for (const auto arg_t : args) {
                            if (self(self, arg_t, /*nested=*/true)) return true;
                        }
                        return false;
                    }
                    case parus::ty::Kind::kOptional:
                    case parus::ty::Kind::kArray:
                    case parus::ty::Kind::kBorrow:
                    case parus::ty::Kind::kEscape:
                    case parus::ty::Kind::kPtr:
                        return self(self, tt.elem, /*nested=*/true);
                    case parus::ty::Kind::kFn:
                        if (self(self, tt.ret, /*nested=*/true)) return true;
                        for (uint32_t i = 0; i < tt.param_count; ++i) {
                            if (self(self, types.fn_param_at(t, i), /*nested=*/true)) return true;
                        }
                        return false;
                    default:
                        return false;
                }
            };
            const auto acts_has_unresolved_generic = [&](const parus::ast::Stmt& acts_decl) -> bool {
                if (acts_decl.kind != parus::ast::StmtKind::kActsDecl || !acts_decl.acts_is_for) {
                    return false;
                }
                if (acts_decl.decl_generic_param_count > 0) return true;
                if (type_contains_unresolved_generic_param(
                        type_contains_unresolved_generic_param,
                        acts_decl.acts_target_type,
                        /*nested=*/false)) {
                    return true;
                }
                const auto& witnesses = ast.acts_assoc_type_witness_decls();
                const uint64_t begin = acts_decl.acts_assoc_witness_begin;
                const uint64_t end = begin + acts_decl.acts_assoc_witness_count;
                if (begin > witnesses.size() || end > witnesses.size()) return false;
                for (uint32_t i = 0; i < acts_decl.acts_assoc_witness_count; ++i) {
                    const auto& witness = witnesses[acts_decl.acts_assoc_witness_begin + i];
                    if (type_contains_unresolved_generic_param(
                            type_contains_unresolved_generic_param,
                            witness.rhs_type,
                            /*nested=*/false)) {
                        return true;
                    }
                }
                return false;
            };

            std::vector<std::string> ns{};
            const auto collect_stmt = [&](const auto& self, parus::ast::StmtId sid) -> bool {
                if (sid == parus::ast::k_invalid_stmt || static_cast<size_t>(sid) >= ast.stmts().size()) {
                    return true;
                }
                const auto& s = ast.stmt(sid);
                if (s.span.file_id != current_file_id) return true;

                if (s.kind == parus::ast::StmtKind::kBlock) {
                    const auto& kids = ast.stmt_children();
                    const uint64_t begin = s.stmt_begin;
                    const uint64_t end = begin + s.stmt_count;
                    if (begin > kids.size() || end > kids.size()) return true;
                    for (uint32_t i = 0; i < s.stmt_count; ++i) {
                        if (!self(self, kids[s.stmt_begin + i])) return false;
                    }
                    return true;
                }

                if (s.kind == parus::ast::StmtKind::kNestDecl && !s.nest_is_file_directive) {
                    const auto& segs = ast.path_segs();
                    const uint64_t begin = s.nest_path_begin;
                    const uint64_t end = begin + s.nest_path_count;
                    uint32_t pushed = 0;
                    if (begin <= segs.size() && end <= segs.size()) {
                        for (uint32_t i = 0; i < s.nest_path_count; ++i) {
                            ns.push_back(std::string(segs[s.nest_path_begin + i]));
                            ++pushed;
                        }
                    }
                    const bool ok = self(self, s.a);
                    while (pushed > 0) {
                        ns.pop_back();
                        --pushed;
                    }
                    return ok;
                }

                const bool is_generic_free_fn =
                    s.kind == parus::ast::StmtKind::kFnDecl &&
                    !s.name.empty() &&
                    s.a != parus::ast::k_invalid_stmt &&
                    s.fn_generic_param_count > 0;
                const bool is_exported_free_fn =
                    s.kind == parus::ast::StmtKind::kFnDecl &&
                    !s.name.empty() &&
                    s.a != parus::ast::k_invalid_stmt &&
                    s.is_export;
                const bool is_generic_proto =
                    s.kind == parus::ast::StmtKind::kProtoDecl &&
                    !s.name.empty() &&
                    s.is_export &&
                    s.decl_generic_param_count > 0;
                const bool is_generic_acts =
                    s.kind == parus::ast::StmtKind::kActsDecl &&
                    s.is_export &&
                    s.acts_is_for &&
                    acts_has_unresolved_generic(s);
                const bool is_class_decl =
                    s.kind == parus::ast::StmtKind::kClassDecl &&
                    !s.name.empty();
                const bool is_field_decl =
                    s.kind == parus::ast::StmtKind::kFieldDecl &&
                    !s.name.empty();
                const bool is_enum_decl =
                    s.kind == parus::ast::StmtKind::kEnumDecl &&
                    !s.name.empty();
                if (!is_generic_free_fn && !is_exported_free_fn && !is_generic_proto && !is_generic_acts &&
                    !is_class_decl && !is_field_decl && !is_enum_decl) {
                    return true;
                }

                const auto lc = sm.line_col(s.span.file_id, s.span.lo);
                std::string qname = qualify_name_(ns, s.name);
                std::string link_name_override{};
                for (const auto& ex : current_exports) {
                    if ((is_generic_free_fn || is_exported_free_fn) &&
                        ex.kind != parus::sema::SymbolKind::kFn) {
                        continue;
                    }
                    if (is_generic_proto && ex.kind != parus::sema::SymbolKind::kType) continue;
                    if (is_generic_acts && ex.kind != parus::sema::SymbolKind::kAct) continue;
                    if (is_class_decl && ex.kind != parus::sema::SymbolKind::kType) continue;
                    if (is_field_decl && ex.kind != parus::sema::SymbolKind::kField) continue;
                    if (is_enum_decl && ex.kind != parus::sema::SymbolKind::kType) continue;
                    if (ex.decl_file != decl_file) continue;
                    if (ex.decl_line != lc.line || ex.decl_col != lc.col) continue;
                    qname = ex.path;
                    link_name_override = ex.link_name;
                    break;
                }
                TemplateSidecarFunction one{};
                if (!serialize_top_level_generic_decl_sidecar_(
                        ast,
                        sid,
                        types,
                        sm,
                        decl_file,
                        bundle_name,
                        module_head,
                        qname,
                        link_name_override,
                        one,
                        out_err)) {
                    return false;
                }
                out.push_back(std::move(one));
                return true;
            };

            const auto& kids = ast.stmt_children();
            const uint64_t begin = rs.stmt_begin;
            const uint64_t end = begin + rs.stmt_count;
            if (begin > kids.size() || end > kids.size()) return true;
            for (uint32_t i = 0; i < rs.stmt_count; ++i) {
                if (!collect_stmt(collect_stmt, kids[rs.stmt_begin + i])) return false;
            }

            std::unordered_map<std::string, parus::ast::StmtId> local_decl_by_qname{};
            const auto helper_class_has_static_mut_state = [&](parus::ast::StmtId sid) -> bool {
                if (sid == parus::ast::k_invalid_stmt || static_cast<size_t>(sid) >= ast.stmts().size()) {
                    return false;
                }
                const auto& decl = ast.stmt(sid);
                if (decl.kind != parus::ast::StmtKind::kClassDecl) return false;
                const auto& decl_kids = ast.stmt_children();
                const uint64_t begin = decl.stmt_begin;
                const uint64_t end = begin + decl.stmt_count;
                if (begin > decl_kids.size() || end > decl_kids.size()) return false;
                for (uint32_t i = decl.stmt_begin; i < decl.stmt_begin + decl.stmt_count; ++i) {
                    const auto msid = decl_kids[i];
                    if (msid == parus::ast::k_invalid_stmt ||
                        static_cast<size_t>(msid) >= ast.stmts().size()) {
                        continue;
                    }
                    const auto& member = ast.stmt(msid);
                    if (member.kind == parus::ast::StmtKind::kVar &&
                        member.is_static &&
                        !member.is_const) {
                        return true;
                    }
                }
                return false;
            };

            std::vector<std::string> validate_ns{};
            const auto index_decl = [&](const auto& self, parus::ast::StmtId sid) -> void {
                if (sid == parus::ast::k_invalid_stmt || static_cast<size_t>(sid) >= ast.stmts().size()) return;
                const auto& s = ast.stmt(sid);
                if (s.span.file_id != current_file_id) return;
                if (s.kind == parus::ast::StmtKind::kNestDecl && !s.nest_is_file_directive) {
                    const auto& segs = ast.path_segs();
                    const uint64_t begin = s.nest_path_begin;
                    const uint64_t end = begin + s.nest_path_count;
                    uint32_t pushed = 0;
                    if (begin <= segs.size() && end <= segs.size()) {
                        for (uint32_t i = 0; i < s.nest_path_count; ++i) {
                            validate_ns.push_back(std::string(segs[s.nest_path_begin + i]));
                            ++pushed;
                        }
                    }
                    if (s.a != parus::ast::k_invalid_stmt &&
                        static_cast<size_t>(s.a) < ast.stmts().size()) {
                        const auto& body = ast.stmt(s.a);
                        if (body.kind == parus::ast::StmtKind::kBlock) {
                            const auto& blk_kids = ast.stmt_children();
                            const uint64_t body_begin = body.stmt_begin;
                            const uint64_t body_end = body_begin + body.stmt_count;
                            if (body_begin <= blk_kids.size() && body_end <= blk_kids.size()) {
                                for (uint32_t i = 0; i < body.stmt_count; ++i) {
                                    self(self, blk_kids[body.stmt_begin + i]);
                                }
                            }
                        } else {
                            self(self, s.a);
                        }
                    }
                    while (pushed > 0) {
                        validate_ns.pop_back();
                        --pushed;
                    }
                    return;
                }
                if (s.name.empty()) return;
                switch (s.kind) {
                    case parus::ast::StmtKind::kClassDecl:
                    case parus::ast::StmtKind::kFieldDecl:
                    case parus::ast::StmtKind::kEnumDecl:
                    case parus::ast::StmtKind::kActorDecl:
                    case parus::ast::StmtKind::kVar:
                        local_decl_by_qname[qualify_name_(validate_ns, s.name)] = sid;
                        break;
                    default:
                        break;
                }
            };
            for (uint32_t i = 0; i < rs.stmt_count; ++i) {
                index_decl(index_decl, kids[rs.stmt_begin + i]);
            }

            std::unordered_map<std::string, const TemplateSidecarFunction*> sidecar_by_name{};
            const auto register_sidecar_name = [&](const TemplateSidecarFunction& templ,
                                                   std::string_view name) {
                if (name.empty()) return;
                sidecar_by_name.emplace(std::string(name), &templ);
            };
            std::unordered_map<std::string, std::vector<std::string>> sidecar_decl_ref_cache{};
            for (const auto& templ : out) {
                if (!templ.public_path.empty()) {
                    register_sidecar_name(templ, templ.public_path);
                    if (!templ.module_head.empty()) {
                        const std::string prefix = templ.module_head + "::";
                        if (templ.public_path.starts_with(prefix)) {
                            register_sidecar_name(templ, std::string_view(templ.public_path).substr(prefix.size()));
                        }
                    }
                    const size_t last = templ.public_path.rfind("::");
                    if (last != std::string::npos) {
                        register_sidecar_name(templ, std::string_view(templ.public_path).substr(last + 2));
                    }
                }
                if (!templ.lookup_name.empty()) {
                    register_sidecar_name(templ, templ.lookup_name);
                }
            }

            const auto strip_generic_args = [](std::string_view text) -> std::string {
                std::string out{};
                int depth = 0;
                for (const char ch : text) {
                    if (ch == '<') {
                        ++depth;
                        continue;
                    }
                    if (ch == '>') {
                        if (depth > 0) --depth;
                        continue;
                    }
                    if (depth == 0) out.push_back(ch);
                }
                return out;
            };
            const auto collect_sidecar_decl_refs = [&](const TemplateSidecarFunction& templ) {
                const std::string cache_key = template_sidecar_identity_key_(templ);
                if (auto it = sidecar_decl_ref_cache.find(cache_key);
                    it != sidecar_decl_ref_cache.end()) {
                    return it->second;
                }
                std::vector<std::string> refs{};
                std::unordered_set<std::string> seen{};
                std::vector<std::string> namespace_prefixes{};
                const auto add_namespace_prefix = [&](std::string_view path) {
                    const size_t pos = path.rfind("::");
                    if (pos == std::string_view::npos) return;
                    const std::string prefix(path.substr(0, pos));
                    if (!prefix.empty() &&
                        std::find(namespace_prefixes.begin(), namespace_prefixes.end(), prefix) ==
                            namespace_prefixes.end()) {
                        namespace_prefixes.push_back(prefix);
                    }
                };
                add_namespace_prefix(templ.public_path);
                add_namespace_prefix(templ.lookup_name);
                if (!templ.module_head.empty() &&
                    std::find(namespace_prefixes.begin(), namespace_prefixes.end(), templ.module_head) ==
                        namespace_prefixes.end()) {
                    namespace_prefixes.push_back(templ.module_head);
                }
                const auto add_ref_candidate = [&](std::string candidate) {
                    if (candidate.empty()) return;
                    std::vector<std::string> candidates{};
                    candidates.push_back(candidate);
                    if (candidate.find("::") == std::string::npos) {
                        for (const auto& prefix : namespace_prefixes) {
                            candidates.push_back(prefix + "::" + candidate);
                        }
                    }
                    for (auto expanded : candidates) {
                        for (;;) {
                            if (seen.find(expanded) == seen.end() &&
                                local_decl_by_qname.find(expanded) != local_decl_by_qname.end()) {
                                seen.insert(expanded);
                                refs.push_back(expanded);
                                return;
                            }
                            const size_t pos = expanded.rfind("::");
                            if (pos == std::string::npos) break;
                            expanded.erase(pos);
                        }
                    }
                    if (candidate.find("::") == std::string::npos) {
                        const std::string suffix = "::" + candidate;
                        for (const auto& [qname, _sid] : local_decl_by_qname) {
                            if (qname == candidate ||
                                (qname.size() > suffix.size() &&
                                 qname.ends_with(suffix))) {
                                if (seen.find(qname) == seen.end()) {
                                    seen.insert(qname);
                                    refs.push_back(qname);
                                }
                                return;
                            }
                        }
                    }
                };
                const auto add_ref_tokens = [&](std::string_view text) {
                    std::string token{};
                    auto flush = [&]() {
                        if (token.empty()) return;
                        add_ref_candidate(strip_generic_args(token));
                        token.clear();
                    };
                    for (const char ch : text) {
                        const bool is_token_char =
                            (ch >= 'A' && ch <= 'Z') ||
                            (ch >= 'a' && ch <= 'z') ||
                            (ch >= '0' && ch <= '9') ||
                            ch == '_' || ch == ':';
                        if (is_token_char) {
                            token.push_back(ch);
                        } else {
                            flush();
                        }
                    }
                    flush();
                };
                for (const auto& ref : templ.path_refs) {
                    if (!ref.path.empty()) add_ref_candidate(ref.path);
                    if (!ref.type_repr.empty()) add_ref_tokens(ref.type_repr);
                }
                for (const auto& field : templ.field_members) {
                    if (!field.type_repr.empty()) add_ref_tokens(field.type_repr);
                }
                for (const auto& cc : templ.constraints) {
                    if (!cc.rhs_type_repr.empty()) add_ref_tokens(cc.rhs_type_repr);
                    if (!cc.proto.path.empty()) add_ref_candidate(cc.proto.path);
                }
                for (const auto& expr : templ.exprs) {
                    if (!expr.text.empty()) add_ref_tokens(expr.text);
                    if (!expr.cast_type_repr.empty()) add_ref_tokens(expr.cast_type_repr);
                    if (!expr.target_type_repr.empty()) add_ref_tokens(expr.target_type_repr);
                    if (!expr.field_init_type_repr.empty()) add_ref_tokens(expr.field_init_type_repr);
                }
                for (const auto& stmt : templ.stmts) {
                    if (!stmt.type_repr.empty()) add_ref_tokens(stmt.type_repr);
                    if (!stmt.fn_ret_repr.empty()) add_ref_tokens(stmt.fn_ret_repr);
                    if (!stmt.acts_target_type_repr.empty()) add_ref_tokens(stmt.acts_target_type_repr);
                }
                sidecar_decl_ref_cache.emplace(cache_key, refs);
                return refs;
            };

            for (const auto& templ : out) {
                if (!templ.is_public_export) continue;
                const std::string root_name =
                    !templ.public_path.empty() ? templ.public_path : templ.lookup_name;
                std::unordered_set<std::string> visiting{};
                const auto validate_closure_ref = [&](const auto& self,
                                                      const std::string& chain,
                                                      std::string_view ref_path) -> bool {
                    if (ref_path.empty()) return true;
                    const std::string dep_name(ref_path);
                    const std::string dep_chain = chain + " -> " + dep_name;
                    auto it = local_decl_by_qname.find(dep_name);
                    if (it != local_decl_by_qname.end()) {
                        const auto dep_sid = it->second;
                        const auto& dep = ast.stmt(dep_sid);
                        if (dep.kind == parus::ast::StmtKind::kActorDecl) {
                            out_err = "unsupported helper actor dependency closure: " + dep_chain;
                            return false;
                        }
                        if (dep.kind == parus::ast::StmtKind::kVar &&
                            !dep.is_const) {
                            out_err = "unsupported mutable global dependency closure: " + dep_chain;
                            return false;
                        }
                        if (dep.kind == parus::ast::StmtKind::kClassDecl &&
                            helper_class_has_static_mut_state(dep_sid)) {
                            out_err = "unsupported helper class dependency closure with static mutable state: " +
                                      dep_chain;
                            return false;
                        }
                    }
                    const auto sidecar_it = sidecar_by_name.find(dep_name);
                    if (sidecar_it == sidecar_by_name.end()) return true;
                    if (!visiting.insert(dep_name).second) return true;
                    const TemplateSidecarFunction* dep_templ = sidecar_it->second;
                    for (const auto& next_ref : collect_sidecar_decl_refs(*dep_templ)) {
                        if (!self(self, dep_chain, next_ref)) {
                            visiting.erase(dep_name);
                            return false;
                        }
                    }
                    visiting.erase(dep_name);
                    return true;
                };

                for (const auto& ref : collect_sidecar_decl_refs(templ)) {
                    if (!validate_closure_ref(validate_closure_ref, root_name, ref)) {
                        return false;
                    }
                }
            }

            return dedupe_template_sidecars_(out, &out_err);
        }

        void validate_same_folder_export_collisions_(
            const std::vector<ExportSurfaceEntry>& entries,
            parus::diag::Bag& bag,
            const parus::Span& span
        ) {
            std::set<std::tuple<std::string, std::string, std::string, std::string>> seen{};
            for (const auto& e : entries) {
                if (!e.is_export) continue;
                const auto key = std::make_tuple(e.module_head, e.path, e.kind_text, e.type_repr);
                if (seen.insert(key).second) continue;
                parus::diag::Diagnostic d(
                    parus::diag::Severity::kError,
                    parus::diag::Code::kExportCollisionSameFolder,
                    span
                );
                d.add_arg(e.module_head.empty() ? e.path : (e.module_head + "::" + e.path));
                bag.add(std::move(d));
            }
        }

        bool write_export_index_(
            const std::string& out_path,
            const std::string& bundle_name,
            const std::vector<ExportSurfaceEntry>& entries,
            std::string& out_err
        ) {
            namespace fs = std::filesystem;
            out_err.clear();
            std::error_code ec{};
            const fs::path p(out_path);
            const auto dir = p.parent_path();
            if (!dir.empty()) {
                fs::create_directories(dir, ec);
                if (ec) {
                    out_err = "failed to create export-index directory: " + dir.string();
                    return false;
                }
            }

            std::ofstream ofs(out_path, std::ios::binary | std::ios::trunc);
            if (!ofs.is_open()) {
                out_err = "failed to open export-index output: " + out_path;
                return false;
            }

            ofs << "{\n";
            ofs << "  \"version\": 1,\n";
            ofs << "  \"bundle\": \"" << json_escape_text_(bundle_name) << "\",\n";
            ofs << "  \"exports\": [\n";
            for (size_t i = 0; i < entries.size(); ++i) {
                const auto& e = entries[i];
                ofs << "    {\"kind\":\"" << json_escape_text_(e.kind_text)
                    << "\",\"path\":\"" << json_escape_text_(e.path)
                    << "\",\"link_name\":\"" << json_escape_text_(e.link_name)
                    << "\",\"module_head\":\"" << json_escape_text_(e.module_head)
                    << "\",\"decl_dir\":\"" << json_escape_text_(e.decl_dir)
                    << "\",\"type_repr\":\"" << json_escape_text_(e.type_repr)
                    << "\",\"type_semantic\":\"" << json_escape_text_(e.type_semantic)
                    << "\",\"inst_payload\":\"" << json_escape_text_(e.inst_payload)
                    << "\",\"decl_span\":{\"file\":\"" << json_escape_text_(e.decl_file)
                    << "\",\"line\":" << e.decl_line
                    << ",\"col\":" << e.decl_col
                    << "},\"is_export\":" << (e.is_export ? "true" : "false")
                    << "}";
                if (i + 1 != entries.size()) ofs << ",";
                ofs << "\n";
            }
            ofs << "  ]\n";
            ofs << "}\n";

            if (!ofs.good()) {
                out_err = "failed to write export-index output: " + out_path;
                return false;
            }
            return true;
        }

        bool find_json_key_pos_(std::string_view text, std::string_view key, size_t& out_pos) {
            const std::string needle = "\"" + std::string(key) + "\"";
            const size_t at = text.find(needle);
            if (at == std::string_view::npos) return false;
            out_pos = at + needle.size();
            return true;
        }

        bool parse_json_string_field_(std::string_view text, std::string_view key, std::string& out) {
            size_t pos = 0;
            if (!find_json_key_pos_(text, key, pos)) return false;
            pos = text.find(':', pos);
            if (pos == std::string_view::npos) return false;
            ++pos;
            while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
            if (pos >= text.size() || text[pos] != '"') return false;
            ++pos;
            std::string raw{};
            bool escaped = false;
            for (; pos < text.size(); ++pos) {
                const char c = text[pos];
                if (escaped) {
                    raw.push_back(c);
                    escaped = false;
                    continue;
                }
                if (c == '\\') {
                    raw.push_back(c);
                    escaped = true;
                    continue;
                }
                if (c == '"') break;
                raw.push_back(c);
            }
            if (pos >= text.size() || text[pos] != '"') return false;
            return json_unescape_text_(raw, out);
        }

        bool parse_json_string_field_optional_(std::string_view text, std::string_view key, std::string& out) {
            size_t pos = 0;
            if (!find_json_key_pos_(text, key, pos)) {
                out.clear();
                return true;
            }
            return parse_json_string_field_(text, key, out);
        }

        bool parse_json_uint_field_(std::string_view text, std::string_view key, uint32_t& out) {
            size_t pos = 0;
            if (!find_json_key_pos_(text, key, pos)) return false;
            pos = text.find(':', pos);
            if (pos == std::string_view::npos) return false;
            ++pos;
            while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
            size_t end = pos;
            while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
            if (end == pos) return false;
            try {
                out = static_cast<uint32_t>(std::stoul(std::string(text.substr(pos, end - pos))));
                return true;
            } catch (...) {
                return false;
            }
        }

        bool parse_json_uint_field_optional_(std::string_view text, std::string_view key, uint32_t& out) {
            size_t pos = 0;
            if (!find_json_key_pos_(text, key, pos)) {
                out = 0;
                return true;
            }
            return parse_json_uint_field_(text, key, out);
        }

        bool parse_json_i64_field_(std::string_view text, std::string_view key, int64_t& out) {
            size_t pos = 0;
            if (!find_json_key_pos_(text, key, pos)) return false;
            pos = text.find(':', pos);
            if (pos == std::string_view::npos) return false;
            ++pos;
            while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
            size_t end = pos;
            if (end < text.size() && (text[end] == '-' || text[end] == '+')) ++end;
            while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
            if (end == pos || (end == pos + 1 && (text[pos] == '-' || text[pos] == '+'))) return false;
            try {
                out = static_cast<int64_t>(std::stoll(std::string(text.substr(pos, end - pos))));
                return true;
            } catch (...) {
                return false;
            }
        }

        bool parse_json_bool_field_(std::string_view text, std::string_view key, bool& out) {
            size_t pos = 0;
            if (!find_json_key_pos_(text, key, pos)) return false;
            pos = text.find(':', pos);
            if (pos == std::string_view::npos) return false;
            ++pos;
            while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos;
            if (text.substr(pos, 4) == "true") {
                out = true;
                return true;
            }
            if (text.substr(pos, 5) == "false") {
                out = false;
                return true;
            }
            return false;
        }

        bool parse_json_array_object_slices_(std::string_view text, std::string_view key, std::vector<std::string>& out) {
            out.clear();
            size_t pos = 0;
            if (!find_json_key_pos_(text, key, pos)) return false;
            pos = text.find('[', pos);
            if (pos == std::string_view::npos) return false;
            ++pos;

            int depth = 0;
            bool in_string = false;
            bool escaped = false;
            size_t obj_begin = std::string_view::npos;

            for (size_t i = pos; i < text.size(); ++i) {
                const char c = text[i];
                if (in_string) {
                    if (escaped) {
                        escaped = false;
                    } else if (c == '\\') {
                        escaped = true;
                    } else if (c == '"') {
                        in_string = false;
                    }
                    continue;
                }

                if (c == '"') {
                    in_string = true;
                    continue;
                }

                if (c == '{') {
                    if (depth == 0) obj_begin = i;
                    ++depth;
                    continue;
                }
                if (c == '}') {
                    --depth;
                    if (depth == 0 && obj_begin != std::string_view::npos) {
                        out.emplace_back(text.substr(obj_begin, i - obj_begin + 1));
                        obj_begin = std::string_view::npos;
                    }
                    continue;
                }
                if (c == ']' && depth == 0) {
                    return true;
                }
            }
            return false;
        }

        bool parse_json_array_string_field_(
            std::string_view text,
            std::string_view key,
            std::vector<std::string>& out
        ) {
            out.clear();
            size_t pos = 0;
            if (!find_json_key_pos_(text, key, pos)) return false;
            pos = text.find('[', pos);
            if (pos == std::string_view::npos) return false;
            ++pos;

            while (pos < text.size()) {
                while (pos < text.size() &&
                       (std::isspace(static_cast<unsigned char>(text[pos])) || text[pos] == ',')) {
                    ++pos;
                }
                if (pos >= text.size()) return false;
                if (text[pos] == ']') return true;
                if (text[pos] != '"') return false;
                ++pos;
                std::string raw{};
                bool escaped = false;
                for (; pos < text.size(); ++pos) {
                    const char c = text[pos];
                    if (escaped) {
                        raw.push_back(c);
                        escaped = false;
                        continue;
                    }
                    if (c == '\\') {
                        raw.push_back(c);
                        escaped = true;
                        continue;
                    }
                    if (c == '"') break;
                    raw.push_back(c);
                }
                if (pos >= text.size() || text[pos] != '"') return false;
                std::string decoded{};
                if (!json_unescape_text_(raw, decoded)) return false;
                out.push_back(std::move(decoded));
                ++pos;
            }
            return false;
        }

        bool parse_json_array_uint_field_(
            std::string_view text,
            std::string_view key,
            std::vector<uint32_t>& out
        ) {
            out.clear();
            size_t pos = 0;
            if (!find_json_key_pos_(text, key, pos)) return false;
            pos = text.find('[', pos);
            if (pos == std::string_view::npos) return false;
            ++pos;

            while (pos < text.size()) {
                while (pos < text.size() &&
                       (std::isspace(static_cast<unsigned char>(text[pos])) || text[pos] == ',')) {
                    ++pos;
                }
                if (pos >= text.size()) return false;
                if (text[pos] == ']') return true;
                size_t end = pos;
                while (end < text.size() && std::isdigit(static_cast<unsigned char>(text[end]))) ++end;
                if (end == pos) return false;
                try {
                    out.push_back(static_cast<uint32_t>(std::stoul(std::string(text.substr(pos, end - pos)))));
                } catch (...) {
                    return false;
                }
                pos = end;
            }
            return false;
        }

        bool load_export_index_(
            const std::string& path,
            std::string& bundle_name,
            std::vector<ExportSurfaceEntry>& out,
            std::string& out_err
        ) {
            out.clear();
            out_err.clear();

            std::string text{};
            std::string io_err{};
            if (!parus::open_file(path, text, io_err)) {
                out_err = "missing export-index file: " + path;
                return false;
            }

            uint32_t version = 0;
            if (!parse_json_uint_field_(text, "version", version) || version != 1) {
                out_err = "unsupported export-index version (expected v1) in: " + path;
                return false;
            }
            if (!parse_json_string_field_(text, "bundle", bundle_name) || bundle_name.empty()) {
                out_err = "invalid export-index bundle name in: " + path;
                return false;
            }

            std::vector<std::string> objects{};
            if (!parse_json_array_object_slices_(text, "exports", objects)) {
                out_err = "invalid export-index exports array in: " + path;
                return false;
            }

            for (const auto& obj : objects) {
                std::string kind_s{};
                std::string path_s{};
                std::string link_name{};
                std::string module_head_raw{};
                std::string decl_dir{};
                std::string type_repr{};
                std::string type_semantic{};
                std::string inst_payload{};
                std::string decl_file{};
                uint32_t decl_line = 1;
                uint32_t decl_col = 1;
                bool is_export = false;

                if (!parse_json_string_field_(obj, "kind", kind_s)) {
                    out_err = "invalid export-index entry field 'kind' in: " + path;
                    return false;
                }
                if (!parse_json_string_field_(obj, "path", path_s)) {
                    out_err = "invalid export-index entry field 'path' in: " + path;
                    return false;
                }
                if (!parse_json_string_field_(obj, "link_name", link_name)) {
                    out_err = "invalid export-index entry field 'link_name' in: " + path;
                    return false;
                }
                if (!parse_json_string_field_(obj, "module_head", module_head_raw)) {
                    out_err = "invalid export-index entry field 'module_head' in: " + path;
                    return false;
                }
                if (!parse_json_string_field_(obj, "decl_dir", decl_dir)) {
                    out_err = "invalid export-index entry field 'decl_dir' in: " + path;
                    return false;
                }
                if (!parse_json_string_field_(obj, "type_repr", type_repr)) {
                    out_err = "invalid export-index entry field 'type_repr' in: " + path;
                    return false;
                }
                if (!parse_json_string_field_optional_(obj, "type_semantic", type_semantic)) {
                    out_err = "invalid export-index entry field 'type_semantic' in: " + path;
                    return false;
                }
                if (!parse_json_string_field_optional_(obj, "inst_payload", inst_payload)) {
                    out_err = "invalid export-index entry field 'inst_payload' in: " + path;
                    return false;
                }
                if (!parse_json_string_field_(obj, "file", decl_file)) {
                    out_err = "invalid export-index entry field 'file' in: " + path;
                    return false;
                }
                if (!parse_json_uint_field_(obj, "line", decl_line)) {
                    out_err = "invalid export-index entry field 'line' in: " + path;
                    return false;
                }
                if (!parse_json_uint_field_(obj, "col", decl_col)) {
                    out_err = "invalid export-index entry field 'col' in: " + path;
                    return false;
                }
                if (!parse_json_bool_field_(obj, "is_export", is_export)) {
                    out_err = "invalid export-index entry field 'is_export' in: " + path;
                    return false;
                }

                const auto kind = symbol_kind_from_text_(kind_s);
                if (!kind.has_value()) {
                    out_err = "unknown export-index kind '" + kind_s + "' in: " + path;
                    return false;
                }

                ExportSurfaceEntry e{};
                e.kind = *kind;
                e.kind_text = kind_s;
                e.path = std::move(path_s);
                e.link_name = std::move(link_name);
                e.module_head = normalize_core_public_module_head_(bundle_name, module_head_raw);
                e.decl_dir = std::move(decl_dir);
                e.type_repr = std::move(type_repr);
                e.type_semantic = std::move(type_semantic);
                e.inst_payload = std::move(inst_payload);
                e.decl_file = std::move(decl_file);
                e.decl_line = decl_line;
                e.decl_col = decl_col;
                e.decl_bundle = bundle_name;
                e.is_export = is_export;
                out.push_back(std::move(e));
            }
            return true;
        }

        bool write_template_sidecar_(
            const std::string& export_index_path,
            const std::string& bundle_name,
            const std::vector<TemplateSidecarFunction>& entries,
            std::string& out_err
        ) {
            namespace fs = std::filesystem;
            out_err.clear();

            const std::string out_path = template_sidecar_path_(export_index_path);
            std::error_code ec{};
            const fs::path p(out_path);
            const auto dir = p.parent_path();
            if (!dir.empty()) {
                fs::create_directories(dir, ec);
                if (ec) {
                    out_err = "failed to create template-sidecar directory: " + dir.string();
                    return false;
                }
            }

            std::ofstream ofs(out_path, std::ios::binary | std::ios::trunc);
            if (!ofs.is_open()) {
                out_err = "failed to open template-sidecar output: " + out_path;
                return false;
            }

            const auto emit_q = [&](std::string_view v) {
                ofs << "\"" << json_escape_text_(v) << "\"";
            };
            const auto emit_bool = [&](bool v) {
                ofs << (v ? "true" : "false");
            };
            const auto emit_uint = [&](uint32_t v) {
                ofs << v;
            };

            ofs << "{\n";
            ofs << "  \"version\": 2,\n";
            ofs << "  \"bundle\": \"" << json_escape_text_(bundle_name) << "\",\n";
            ofs << "  \"templates\": [\n";
            for (size_t i = 0; i < entries.size(); ++i) {
                const auto& e = entries[i];
                ofs << "    {";
                ofs << "\"bundle\":"; emit_q(e.bundle);
                ofs << ",\"module_head\":"; emit_q(e.module_head);
                ofs << ",\"public_path\":"; emit_q(e.public_path);
                ofs << ",\"link_name\":"; emit_q(e.link_name);
                ofs << ",\"lookup_name\":"; emit_q(e.lookup_name);
                ofs << ",\"decl_file\":"; emit_q(e.decl_file);
                ofs << ",\"decl_line\":"; emit_uint(e.decl_line);
                ofs << ",\"decl_col\":"; emit_uint(e.decl_col);
                ofs << ",\"is_public_export\":"; emit_bool(e.is_public_export);
                ofs << ",\"declared_type_repr\":"; emit_q(e.declared_type_repr);
                ofs << ",\"declared_type_semantic\":"; emit_q(e.declared_type_semantic);
                ofs << ",\"root_stmt\":"; emit_uint(e.root_stmt);

                ofs << ",\"stmts\":[";
                for (size_t j = 0; j < e.stmts.size(); ++j) {
                    const auto& s = e.stmts[j];
                    ofs << "{";
                    ofs << "\"kind\":"; emit_uint(s.kind);
                    ofs << ",\"expr\":"; emit_uint(s.expr);
                    ofs << ",\"init\":"; emit_uint(s.init);
                    ofs << ",\"a\":"; emit_uint(s.a);
                    ofs << ",\"b\":"; emit_uint(s.b);
                    ofs << ",\"stmt_begin\":"; emit_uint(s.stmt_begin);
                    ofs << ",\"stmt_count\":"; emit_uint(s.stmt_count);
                    ofs << ",\"case_begin\":"; emit_uint(s.case_begin);
                    ofs << ",\"case_count\":"; emit_uint(s.case_count);
                    ofs << ",\"has_default\":"; emit_bool(s.has_default);
                    ofs << ",\"is_set\":"; emit_bool(s.is_set);
                    ofs << ",\"is_mut\":"; emit_bool(s.is_mut);
                    ofs << ",\"is_static\":"; emit_bool(s.is_static);
                    ofs << ",\"is_const\":"; emit_bool(s.is_const);
                    ofs << ",\"is_extern\":"; emit_bool(s.is_extern);
                    ofs << ",\"link_abi\":"; emit_uint(s.link_abi);
                    ofs << ",\"name\":"; emit_q(s.name);
                    ofs << ",\"type_repr\":"; emit_q(s.type_repr);
                    ofs << ",\"type_semantic\":"; emit_q(s.type_semantic);
                    ofs << ",\"is_export\":"; emit_bool(s.is_export);
                    ofs << ",\"fn_mode\":"; emit_uint(s.fn_mode);
                    ofs << ",\"fn_ret_repr\":"; emit_q(s.fn_ret_repr);
                    ofs << ",\"fn_ret_semantic\":"; emit_q(s.fn_ret_semantic);
                    ofs << ",\"member_visibility\":"; emit_uint(s.member_visibility);
                    ofs << ",\"is_pure\":"; emit_bool(s.is_pure);
                    ofs << ",\"is_comptime\":"; emit_bool(s.is_comptime);
                    ofs << ",\"is_commit\":"; emit_bool(s.is_commit);
                    ofs << ",\"is_recast\":"; emit_bool(s.is_recast);
                    ofs << ",\"is_throwing\":"; emit_bool(s.is_throwing);
                    ofs << ",\"fn_is_const\":"; emit_bool(s.fn_is_const);
                    ofs << ",\"param_begin\":"; emit_uint(s.param_begin);
                    ofs << ",\"param_count\":"; emit_uint(s.param_count);
                    ofs << ",\"positional_param_count\":"; emit_uint(s.positional_param_count);
                    ofs << ",\"has_named_group\":"; emit_bool(s.has_named_group);
                    ofs << ",\"fn_is_c_variadic\":"; emit_bool(s.fn_is_c_variadic);
                    ofs << ",\"fn_is_proto_sig\":"; emit_bool(s.fn_is_proto_sig);
                    ofs << ",\"fn_generic_param_begin\":"; emit_uint(s.fn_generic_param_begin);
                    ofs << ",\"fn_generic_param_count\":"; emit_uint(s.fn_generic_param_count);
                    ofs << ",\"fn_constraint_begin\":"; emit_uint(s.fn_constraint_begin);
                    ofs << ",\"fn_constraint_count\":"; emit_uint(s.fn_constraint_count);
                    ofs << ",\"decl_generic_param_begin\":"; emit_uint(s.decl_generic_param_begin);
                    ofs << ",\"decl_generic_param_count\":"; emit_uint(s.decl_generic_param_count);
                    ofs << ",\"decl_constraint_begin\":"; emit_uint(s.decl_constraint_begin);
                    ofs << ",\"decl_constraint_count\":"; emit_uint(s.decl_constraint_count);
                    ofs << ",\"decl_path_ref_begin\":"; emit_uint(s.decl_path_ref_begin);
                    ofs << ",\"decl_path_ref_count\":"; emit_uint(s.decl_path_ref_count);
                    ofs << ",\"field_layout\":"; emit_uint(s.field_layout);
                    ofs << ",\"field_align\":"; emit_uint(s.field_align);
                    ofs << ",\"field_member_begin\":"; emit_uint(s.field_member_begin);
                    ofs << ",\"field_member_count\":"; emit_uint(s.field_member_count);
                    ofs << ",\"enum_variant_begin\":"; emit_uint(s.enum_variant_begin);
                    ofs << ",\"enum_variant_count\":"; emit_uint(s.enum_variant_count);
                    ofs << ",\"proto_fn_role\":"; emit_uint(s.proto_fn_role);
                    ofs << ",\"proto_require_kind\":"; emit_uint(s.proto_require_kind);
                    ofs << ",\"assoc_type_role\":"; emit_uint(s.assoc_type_role);
                    ofs << ",\"var_is_proto_provide\":"; emit_bool(s.var_is_proto_provide);
                    ofs << ",\"acts_is_for\":"; emit_bool(s.acts_is_for);
                    ofs << ",\"acts_has_set_name\":"; emit_bool(s.acts_has_set_name);
                    ofs << ",\"acts_target_type_repr\":"; emit_q(s.acts_target_type_repr);
                    ofs << ",\"acts_target_type_semantic\":"; emit_q(s.acts_target_type_semantic);
                    ofs << ",\"acts_assoc_witness_begin\":"; emit_uint(s.acts_assoc_witness_begin);
                    ofs << ",\"acts_assoc_witness_count\":"; emit_uint(s.acts_assoc_witness_count);
                    ofs << ",\"manual_perm_mask\":"; emit_uint(s.manual_perm_mask);
                    ofs << ",\"var_has_consume_else\":"; emit_bool(s.var_has_consume_else);
                    ofs << "}";
                    if (j + 1 != e.stmts.size()) ofs << ",";
                }
                ofs << "]";

                ofs << ",\"stmt_children\":[";
                for (size_t j = 0; j < e.stmt_children.size(); ++j) {
                    emit_uint(e.stmt_children[j]);
                    if (j + 1 != e.stmt_children.size()) ofs << ",";
                }
                ofs << "]";

                ofs << ",\"exprs\":[";
                for (size_t j = 0; j < e.exprs.size(); ++j) {
                    const auto& x = e.exprs[j];
                    ofs << "{";
                    ofs << "\"kind\":"; emit_uint(x.kind);
                    ofs << ",\"op\":"; emit_uint(x.op);
                    ofs << ",\"a\":"; emit_uint(x.a);
                    ofs << ",\"b\":"; emit_uint(x.b);
                    ofs << ",\"c\":"; emit_uint(x.c);
                    ofs << ",\"unary_is_mut\":"; emit_bool(x.unary_is_mut);
                    ofs << ",\"text\":"; emit_q(x.text);
                    ofs << ",\"string_is_raw\":"; emit_bool(x.string_is_raw);
                    ofs << ",\"string_is_format\":"; emit_bool(x.string_is_format);
                    ofs << ",\"string_part_begin\":"; emit_uint(x.string_part_begin);
                    ofs << ",\"string_part_count\":"; emit_uint(x.string_part_count);
                    ofs << ",\"string_folded_text\":"; emit_q(x.string_folded_text);
                    ofs << ",\"arg_begin\":"; emit_uint(x.arg_begin);
                    ofs << ",\"arg_count\":"; emit_uint(x.arg_count);
                    ofs << ",\"call_type_arg_begin\":"; emit_uint(x.call_type_arg_begin);
                    ofs << ",\"call_type_arg_count\":"; emit_uint(x.call_type_arg_count);
                    ofs << ",\"call_from_pipe\":"; emit_bool(x.call_from_pipe);
                    ofs << ",\"field_init_begin\":"; emit_uint(x.field_init_begin);
                    ofs << ",\"field_init_count\":"; emit_uint(x.field_init_count);
                    ofs << ",\"field_init_type_repr\":"; emit_q(x.field_init_type_repr);
                    ofs << ",\"field_init_type_semantic\":"; emit_q(x.field_init_type_semantic);
                    ofs << ",\"block_stmt\":"; emit_uint(x.block_stmt);
                    ofs << ",\"block_tail\":"; emit_uint(x.block_tail);
                    ofs << ",\"loop_has_header\":"; emit_bool(x.loop_has_header);
                    ofs << ",\"loop_var\":"; emit_q(x.loop_var);
                    ofs << ",\"loop_iter\":"; emit_uint(x.loop_iter);
                    ofs << ",\"loop_body\":"; emit_uint(x.loop_body);
                    ofs << ",\"cast_type_repr\":"; emit_q(x.cast_type_repr);
                    ofs << ",\"cast_type_semantic\":"; emit_q(x.cast_type_semantic);
                    ofs << ",\"cast_kind\":"; emit_uint(x.cast_kind);
                    ofs << ",\"target_type_repr\":"; emit_q(x.target_type_repr);
                    ofs << ",\"target_type_semantic\":"; emit_q(x.target_type_semantic);
                    ofs << "}";
                    if (j + 1 != e.exprs.size()) ofs << ",";
                }
                ofs << "]";

                ofs << ",\"params\":[";
                for (size_t j = 0; j < e.params.size(); ++j) {
                    const auto& p = e.params[j];
                    ofs << "{";
                    ofs << "\"name\":"; emit_q(p.name);
                    ofs << ",\"type_repr\":"; emit_q(p.type_repr);
                    ofs << ",\"type_semantic\":"; emit_q(p.type_semantic);
                    ofs << ",\"is_mut\":"; emit_bool(p.is_mut);
                    ofs << ",\"is_self\":"; emit_bool(p.is_self);
                    ofs << ",\"self_kind\":"; emit_uint(p.self_kind);
                    ofs << ",\"has_default\":"; emit_bool(p.has_default);
                    ofs << ",\"default_expr\":"; emit_uint(p.default_expr);
                    ofs << ",\"is_named_group\":"; emit_bool(p.is_named_group);
                    ofs << "}";
                    if (j + 1 != e.params.size()) ofs << ",";
                }
                ofs << "]";

                ofs << ",\"switch_cases\":[";
                for (size_t j = 0; j < e.switch_cases.size(); ++j) {
                    const auto& sc = e.switch_cases[j];
                    ofs << "{";
                    ofs << "\"is_default\":"; emit_bool(sc.is_default);
                    ofs << ",\"pat_kind\":"; emit_uint(sc.pat_kind);
                    ofs << ",\"pat_text\":"; emit_q(sc.pat_text);
                    ofs << ",\"enum_type_repr\":"; emit_q(sc.enum_type_repr);
                    ofs << ",\"enum_type_semantic\":"; emit_q(sc.enum_type_semantic);
                    ofs << ",\"enum_variant_name\":"; emit_q(sc.enum_variant_name);
                    ofs << ",\"enum_bind_begin\":"; emit_uint(sc.enum_bind_begin);
                    ofs << ",\"enum_bind_count\":"; emit_uint(sc.enum_bind_count);
                    ofs << ",\"body\":"; emit_uint(sc.body);
                    ofs << "}";
                    if (j + 1 != e.switch_cases.size()) ofs << ",";
                }
                ofs << "]";

                ofs << ",\"switch_enum_binds\":[";
                for (size_t j = 0; j < e.switch_enum_binds.size(); ++j) {
                    const auto& sb = e.switch_enum_binds[j];
                    ofs << "{";
                    ofs << "\"field_name\":"; emit_q(sb.field_name);
                    ofs << ",\"bind_name\":"; emit_q(sb.bind_name);
                    ofs << ",\"bind_type_repr\":"; emit_q(sb.bind_type_repr);
                    ofs << ",\"bind_type_semantic\":"; emit_q(sb.bind_type_semantic);
                    ofs << "}";
                    if (j + 1 != e.switch_enum_binds.size()) ofs << ",";
                }
                ofs << "]";

                ofs << ",\"args\":[";
                for (size_t j = 0; j < e.args.size(); ++j) {
                    const auto& a = e.args[j];
                    ofs << "{";
                    ofs << "\"kind\":"; emit_uint(a.kind);
                    ofs << ",\"has_label\":"; emit_bool(a.has_label);
                    ofs << ",\"is_hole\":"; emit_bool(a.is_hole);
                    ofs << ",\"label\":"; emit_q(a.label);
                    ofs << ",\"expr\":"; emit_uint(a.expr);
                    ofs << "}";
                    if (j + 1 != e.args.size()) ofs << ",";
                }
                ofs << "]";

                ofs << ",\"field_inits\":[";
                for (size_t j = 0; j < e.field_inits.size(); ++j) {
                    const auto& fi = e.field_inits[j];
                    ofs << "{";
                    ofs << "\"name\":"; emit_q(fi.name);
                    ofs << ",\"expr\":"; emit_uint(fi.expr);
                    ofs << "}";
                    if (j + 1 != e.field_inits.size()) ofs << ",";
                }
                ofs << "]";

                ofs << ",\"field_members\":[";
                for (size_t j = 0; j < e.field_members.size(); ++j) {
                    const auto& fm = e.field_members[j];
                    ofs << "{";
                    ofs << "\"name\":"; emit_q(fm.name);
                    ofs << ",\"type_repr\":"; emit_q(fm.type_repr);
                    ofs << ",\"type_semantic\":"; emit_q(fm.type_semantic);
                    ofs << ",\"visibility\":"; emit_uint(fm.visibility);
                    ofs << "}";
                    if (j + 1 != e.field_members.size()) ofs << ",";
                }
                ofs << "]";

                ofs << ",\"enum_variants\":[";
                for (size_t j = 0; j < e.enum_variants.size(); ++j) {
                    const auto& ev = e.enum_variants[j];
                    ofs << "{";
                    ofs << "\"name\":"; emit_q(ev.name);
                    ofs << ",\"payload_begin\":"; emit_uint(ev.payload_begin);
                    ofs << ",\"payload_count\":"; emit_uint(ev.payload_count);
                    ofs << ",\"has_discriminant\":"; emit_bool(ev.has_discriminant);
                    ofs << ",\"discriminant\":"; ofs << ev.discriminant;
                    ofs << "}";
                    if (j + 1 != e.enum_variants.size()) ofs << ",";
                }
                ofs << "]";

                ofs << ",\"fstring_parts\":[";
                for (size_t j = 0; j < e.fstring_parts.size(); ++j) {
                    const auto& fp = e.fstring_parts[j];
                    ofs << "{";
                    ofs << "\"is_expr\":"; emit_bool(fp.is_expr);
                    ofs << ",\"text\":"; emit_q(fp.text);
                    ofs << ",\"expr\":"; emit_uint(fp.expr);
                    ofs << "}";
                    if (j + 1 != e.fstring_parts.size()) ofs << ",";
                }
                ofs << "]";

                ofs << ",\"type_args\":[";
                for (size_t j = 0; j < e.type_args.size(); ++j) {
                    emit_q(e.type_args[j]);
                    if (j + 1 != e.type_args.size()) ofs << ",";
                }
                ofs << "]";

                ofs << ",\"generic_params\":[";
                for (size_t j = 0; j < e.generic_params.size(); ++j) {
                    ofs << "{\"name\":";
                    emit_q(e.generic_params[j].name);
                    ofs << "}";
                    if (j + 1 != e.generic_params.size()) ofs << ",";
                }
                ofs << "]";

                ofs << ",\"constraints\":[";
                for (size_t j = 0; j < e.constraints.size(); ++j) {
                    const auto& c = e.constraints[j];
                    ofs << "{";
                    ofs << "\"kind\":"; emit_uint(static_cast<uint32_t>(c.kind));
                    ofs << ",\"lhs\":"; emit_q(c.type_param);
                    ofs << ",\"rhs_type_repr\":"; emit_q(c.rhs_type_repr);
                    ofs << ",\"proto_bundle\":"; emit_q(c.proto.bundle);
                    ofs << ",\"proto_module_head\":"; emit_q(c.proto.module_head);
                    ofs << ",\"proto_path\":"; emit_q(c.proto.path);
                    ofs << "}";
                    if (j + 1 != e.constraints.size()) ofs << ",";
                }
                ofs << "]";

                ofs << ",\"path_refs\":[";
                for (size_t j = 0; j < e.path_refs.size(); ++j) {
                    const auto& pr = e.path_refs[j];
                    ofs << "{";
                    ofs << "\"path\":"; emit_q(pr.path);
                    ofs << ",\"type_repr\":"; emit_q(pr.type_repr);
                    ofs << ",\"type_semantic\":"; emit_q(pr.type_semantic);
                    ofs << "}";
                    if (j + 1 != e.path_refs.size()) ofs << ",";
                }
                ofs << "]";

                ofs << ",\"acts_assoc_witnesses\":[";
                for (size_t j = 0; j < e.acts_assoc_witnesses.size(); ++j) {
                    const auto& w = e.acts_assoc_witnesses[j];
                    ofs << "{";
                    ofs << "\"assoc_name\":"; emit_q(w.assoc_name);
                    ofs << ",\"rhs_type_repr\":"; emit_q(w.rhs_type_repr);
                    ofs << ",\"rhs_type_semantic\":"; emit_q(w.rhs_type_semantic);
                    ofs << "}";
                    if (j + 1 != e.acts_assoc_witnesses.size()) ofs << ",";
                }
                ofs << "]";
                ofs << "}";
                if (i + 1 != entries.size()) ofs << ",";
                ofs << "\n";
            }
            ofs << "  ]\n";
            ofs << "}\n";

            if (!ofs.good()) {
                out_err = "failed to write template-sidecar output: " + out_path;
                return false;
            }
            return true;
        }

        bool load_template_sidecar_(
            const std::string& export_index_path,
            std::string_view bundle_name,
            std::vector<TemplateSidecarFunction>& out,
            std::string& out_err
        ) {
            out.clear();
            out_err.clear();

            namespace fs = std::filesystem;
            std::error_code ec{};
            const std::string path = template_sidecar_path_(export_index_path);
            if (!fs::exists(path, ec)) {
                if (ec) {
                    out_err = "failed to stat template-sidecar file: " + path;
                    return false;
                }
                return true;
            }

            std::string text{};
            std::string io_err{};
            if (!parus::open_file(path, text, io_err)) {
                out_err = "failed to open template-sidecar file: " + path;
                return false;
            }

            uint32_t version = 0;
            if (!parse_json_uint_field_(text, "version", version) || version != 2) {
                out_err = "unsupported template-sidecar version (expected v2) in: " + path;
                return false;
            }

            std::string sidecar_bundle{};
            if (!parse_json_string_field_(text, "bundle", sidecar_bundle) || sidecar_bundle.empty()) {
                out_err = "invalid template-sidecar bundle name in: " + path;
                return false;
            }
            if (!bundle_name.empty() && sidecar_bundle != bundle_name) {
                out_err = "template-sidecar bundle mismatch in: " + path;
                return false;
            }

            std::vector<std::string> objects{};
            if (!parse_json_array_object_slices_(text, "templates", objects)) {
                out_err = "invalid template-sidecar templates array in: " + path;
                return false;
            }

            for (const auto& obj : objects) {
                TemplateSidecarFunction entry{};
                if (!parse_json_string_field_(obj, "bundle", entry.bundle) ||
                    !parse_json_string_field_(obj, "module_head", entry.module_head) ||
                    !parse_json_string_field_(obj, "public_path", entry.public_path) ||
                    !parse_json_string_field_(obj, "link_name", entry.link_name) ||
                    !parse_json_string_field_(obj, "lookup_name", entry.lookup_name) ||
                    !parse_json_string_field_(obj, "decl_file", entry.decl_file) ||
                    !parse_json_uint_field_(obj, "decl_line", entry.decl_line) ||
                    !parse_json_uint_field_(obj, "decl_col", entry.decl_col) ||
                    !parse_json_bool_field_(obj, "is_public_export", entry.is_public_export) ||
                    !parse_json_string_field_(obj, "declared_type_repr", entry.declared_type_repr) ||
                    !parse_json_string_field_optional_(obj, "declared_type_semantic", entry.declared_type_semantic) ||
                    !parse_json_uint_field_(obj, "root_stmt", entry.root_stmt)) {
                    out_err = "invalid template-sidecar entry in: " + path;
                    return false;
                }
                entry.module_head = normalize_core_public_module_head_(entry.bundle, entry.module_head);
                entry.decl_file = parus::normalize_path(entry.decl_file);

                std::vector<std::string> stmt_objs{};
                if (!parse_json_array_object_slices_(obj, "stmts", stmt_objs)) {
                    out_err = "invalid template-sidecar stmts array in: " + path;
                    return false;
                }
                for (const auto& stmt_obj : stmt_objs) {
                    TemplateSidecarStmt s{};
                    uint32_t stmt_kind = 0;
                    uint32_t stmt_link_abi = 0;
                    uint32_t stmt_fn_mode = 0;
                    uint32_t stmt_member_visibility = 0;
                    uint32_t stmt_field_layout = 0;
                    uint32_t stmt_proto_fn_role = 0;
                    uint32_t stmt_proto_require_kind = 0;
                    uint32_t stmt_assoc_type_role = 0;
                    uint32_t stmt_perm_mask = 0;
                    if (!parse_json_uint_field_(stmt_obj, "kind", stmt_kind) ||
                        !parse_json_uint_field_(stmt_obj, "expr", s.expr) ||
                        !parse_json_uint_field_(stmt_obj, "init", s.init) ||
                        !parse_json_uint_field_(stmt_obj, "a", s.a) ||
                        !parse_json_uint_field_(stmt_obj, "b", s.b) ||
                        !parse_json_uint_field_(stmt_obj, "stmt_begin", s.stmt_begin) ||
                        !parse_json_uint_field_(stmt_obj, "stmt_count", s.stmt_count) ||
                        !parse_json_uint_field_(stmt_obj, "case_begin", s.case_begin) ||
                        !parse_json_uint_field_(stmt_obj, "case_count", s.case_count) ||
                        !parse_json_bool_field_(stmt_obj, "has_default", s.has_default) ||
                        !parse_json_bool_field_(stmt_obj, "is_set", s.is_set) ||
                        !parse_json_bool_field_(stmt_obj, "is_mut", s.is_mut) ||
                        !parse_json_bool_field_(stmt_obj, "is_static", s.is_static) ||
                        !parse_json_bool_field_(stmt_obj, "is_const", s.is_const) ||
                        !parse_json_bool_field_(stmt_obj, "is_extern", s.is_extern) ||
                        !parse_json_uint_field_(stmt_obj, "link_abi", stmt_link_abi) ||
                        !parse_json_string_field_(stmt_obj, "name", s.name) ||
                        !parse_json_string_field_(stmt_obj, "type_repr", s.type_repr) ||
                        !parse_json_string_field_optional_(stmt_obj, "type_semantic", s.type_semantic) ||
                        !parse_json_bool_field_(stmt_obj, "is_export", s.is_export) ||
                        !parse_json_uint_field_(stmt_obj, "fn_mode", stmt_fn_mode) ||
                        !parse_json_string_field_(stmt_obj, "fn_ret_repr", s.fn_ret_repr) ||
                        !parse_json_string_field_optional_(stmt_obj, "fn_ret_semantic", s.fn_ret_semantic) ||
                        !parse_json_uint_field_(stmt_obj, "member_visibility", stmt_member_visibility) ||
                        !parse_json_bool_field_(stmt_obj, "is_pure", s.is_pure) ||
                        !parse_json_bool_field_(stmt_obj, "is_comptime", s.is_comptime) ||
                        !parse_json_bool_field_(stmt_obj, "is_commit", s.is_commit) ||
                        !parse_json_bool_field_(stmt_obj, "is_recast", s.is_recast) ||
                        !parse_json_bool_field_(stmt_obj, "is_throwing", s.is_throwing) ||
                        !parse_json_bool_field_(stmt_obj, "fn_is_const", s.fn_is_const) ||
                        !parse_json_uint_field_(stmt_obj, "param_begin", s.param_begin) ||
                        !parse_json_uint_field_(stmt_obj, "param_count", s.param_count) ||
                        !parse_json_uint_field_(stmt_obj, "positional_param_count", s.positional_param_count) ||
                        !parse_json_bool_field_(stmt_obj, "has_named_group", s.has_named_group) ||
                        !parse_json_bool_field_(stmt_obj, "fn_is_c_variadic", s.fn_is_c_variadic) ||
                        !parse_json_bool_field_(stmt_obj, "fn_is_proto_sig", s.fn_is_proto_sig) ||
                        !parse_json_uint_field_(stmt_obj, "fn_generic_param_begin", s.fn_generic_param_begin) ||
                        !parse_json_uint_field_(stmt_obj, "fn_generic_param_count", s.fn_generic_param_count) ||
                        !parse_json_uint_field_(stmt_obj, "fn_constraint_begin", s.fn_constraint_begin) ||
                        !parse_json_uint_field_(stmt_obj, "fn_constraint_count", s.fn_constraint_count) ||
                        !parse_json_uint_field_(stmt_obj, "decl_generic_param_begin", s.decl_generic_param_begin) ||
                        !parse_json_uint_field_(stmt_obj, "decl_generic_param_count", s.decl_generic_param_count) ||
                        !parse_json_uint_field_(stmt_obj, "decl_constraint_begin", s.decl_constraint_begin) ||
                        !parse_json_uint_field_(stmt_obj, "decl_constraint_count", s.decl_constraint_count) ||
                        !parse_json_uint_field_(stmt_obj, "decl_path_ref_begin", s.decl_path_ref_begin) ||
                        !parse_json_uint_field_(stmt_obj, "decl_path_ref_count", s.decl_path_ref_count) ||
                        !parse_json_uint_field_(stmt_obj, "field_layout", stmt_field_layout) ||
                        !parse_json_uint_field_(stmt_obj, "field_align", s.field_align) ||
                        !parse_json_uint_field_(stmt_obj, "field_member_begin", s.field_member_begin) ||
                        !parse_json_uint_field_(stmt_obj, "field_member_count", s.field_member_count) ||
                        !parse_json_uint_field_(stmt_obj, "enum_variant_begin", s.enum_variant_begin) ||
                        !parse_json_uint_field_(stmt_obj, "enum_variant_count", s.enum_variant_count) ||
                        !parse_json_uint_field_(stmt_obj, "proto_fn_role", stmt_proto_fn_role) ||
                        !parse_json_uint_field_(stmt_obj, "proto_require_kind", stmt_proto_require_kind) ||
                        !parse_json_uint_field_(stmt_obj, "assoc_type_role", stmt_assoc_type_role) ||
                        !parse_json_bool_field_(stmt_obj, "var_is_proto_provide", s.var_is_proto_provide) ||
                        !parse_json_bool_field_(stmt_obj, "acts_is_for", s.acts_is_for) ||
                        !parse_json_bool_field_(stmt_obj, "acts_has_set_name", s.acts_has_set_name) ||
                        !parse_json_string_field_(stmt_obj, "acts_target_type_repr", s.acts_target_type_repr) ||
                        !parse_json_string_field_optional_(stmt_obj, "acts_target_type_semantic", s.acts_target_type_semantic) ||
                        !parse_json_uint_field_(stmt_obj, "acts_assoc_witness_begin", s.acts_assoc_witness_begin) ||
                        !parse_json_uint_field_(stmt_obj, "acts_assoc_witness_count", s.acts_assoc_witness_count) ||
                        !parse_json_uint_field_(stmt_obj, "manual_perm_mask", stmt_perm_mask) ||
                        !parse_json_bool_field_(stmt_obj, "var_has_consume_else", s.var_has_consume_else)) {
                        out_err = "invalid template-sidecar stmt entry in: " + path;
                        return false;
                    }
                    s.kind = static_cast<uint8_t>(stmt_kind);
                    s.link_abi = static_cast<uint8_t>(stmt_link_abi);
                    s.fn_mode = static_cast<uint8_t>(stmt_fn_mode);
                    s.member_visibility = static_cast<uint8_t>(stmt_member_visibility);
                    s.field_layout = static_cast<uint8_t>(stmt_field_layout);
                    s.proto_fn_role = static_cast<uint8_t>(stmt_proto_fn_role);
                    s.proto_require_kind = static_cast<uint8_t>(stmt_proto_require_kind);
                    s.assoc_type_role = static_cast<uint8_t>(stmt_assoc_type_role);
                    s.manual_perm_mask = static_cast<uint8_t>(stmt_perm_mask);
                    entry.stmts.push_back(std::move(s));
                }

                if (!parse_json_array_uint_field_(obj, "stmt_children", entry.stmt_children)) {
                    out_err = "invalid template-sidecar stmt_children array in: " + path;
                    return false;
                }

                std::vector<std::string> expr_objs{};
                if (!parse_json_array_object_slices_(obj, "exprs", expr_objs)) {
                    out_err = "invalid template-sidecar exprs array in: " + path;
                    return false;
                }
                for (const auto& expr_obj : expr_objs) {
                    TemplateSidecarExpr x{};
                    uint32_t expr_kind = 0;
                    uint32_t expr_op = 0;
                    uint32_t expr_cast_kind = 0;
                    if (!parse_json_uint_field_(expr_obj, "kind", expr_kind) ||
                        !parse_json_uint_field_(expr_obj, "op", expr_op) ||
                        !parse_json_uint_field_(expr_obj, "a", x.a) ||
                        !parse_json_uint_field_(expr_obj, "b", x.b) ||
                        !parse_json_uint_field_(expr_obj, "c", x.c) ||
                        !parse_json_bool_field_(expr_obj, "unary_is_mut", x.unary_is_mut) ||
                        !parse_json_string_field_(expr_obj, "text", x.text) ||
                        !parse_json_bool_field_(expr_obj, "string_is_raw", x.string_is_raw) ||
                        !parse_json_bool_field_(expr_obj, "string_is_format", x.string_is_format) ||
                        !parse_json_uint_field_(expr_obj, "string_part_begin", x.string_part_begin) ||
                        !parse_json_uint_field_(expr_obj, "string_part_count", x.string_part_count) ||
                        !parse_json_string_field_(expr_obj, "string_folded_text", x.string_folded_text) ||
                        !parse_json_uint_field_(expr_obj, "arg_begin", x.arg_begin) ||
                        !parse_json_uint_field_(expr_obj, "arg_count", x.arg_count) ||
                        !parse_json_uint_field_(expr_obj, "call_type_arg_begin", x.call_type_arg_begin) ||
                        !parse_json_uint_field_(expr_obj, "call_type_arg_count", x.call_type_arg_count) ||
                        !parse_json_bool_field_(expr_obj, "call_from_pipe", x.call_from_pipe) ||
                        !parse_json_uint_field_(expr_obj, "field_init_begin", x.field_init_begin) ||
                        !parse_json_uint_field_(expr_obj, "field_init_count", x.field_init_count) ||
                        !parse_json_string_field_(expr_obj, "field_init_type_repr", x.field_init_type_repr) ||
                        !parse_json_string_field_optional_(expr_obj, "field_init_type_semantic", x.field_init_type_semantic) ||
                        !parse_json_uint_field_(expr_obj, "block_stmt", x.block_stmt) ||
                        !parse_json_uint_field_(expr_obj, "block_tail", x.block_tail) ||
                        !parse_json_bool_field_(expr_obj, "loop_has_header", x.loop_has_header) ||
                        !parse_json_string_field_(expr_obj, "loop_var", x.loop_var) ||
                        !parse_json_uint_field_(expr_obj, "loop_iter", x.loop_iter) ||
                        !parse_json_uint_field_(expr_obj, "loop_body", x.loop_body) ||
                        !parse_json_string_field_(expr_obj, "cast_type_repr", x.cast_type_repr) ||
                        !parse_json_string_field_optional_(expr_obj, "cast_type_semantic", x.cast_type_semantic) ||
                        !parse_json_uint_field_(expr_obj, "cast_kind", expr_cast_kind) ||
                        !parse_json_string_field_(expr_obj, "target_type_repr", x.target_type_repr) ||
                        !parse_json_string_field_optional_(expr_obj, "target_type_semantic", x.target_type_semantic)) {
                        out_err = "invalid template-sidecar expr entry in: " + path;
                        return false;
                    }
                    x.kind = static_cast<uint8_t>(expr_kind);
                    x.op = static_cast<uint8_t>(expr_op);
                    x.cast_kind = static_cast<uint8_t>(expr_cast_kind);
                    entry.exprs.push_back(std::move(x));
                }

                std::vector<std::string> switch_case_objs{};
                if (!parse_json_array_object_slices_(obj, "switch_cases", switch_case_objs)) {
                    out_err = "invalid template-sidecar switch_cases array in: " + path;
                    return false;
                }
                for (const auto& switch_case_obj : switch_case_objs) {
                    TemplateSidecarSwitchCase sc{};
                    uint32_t pat_kind = 0;
                    if (!parse_json_bool_field_(switch_case_obj, "is_default", sc.is_default) ||
                        !parse_json_uint_field_(switch_case_obj, "pat_kind", pat_kind) ||
                        !parse_json_string_field_(switch_case_obj, "pat_text", sc.pat_text) ||
                        !parse_json_string_field_optional_(switch_case_obj, "enum_type_repr", sc.enum_type_repr) ||
                        !parse_json_string_field_optional_(switch_case_obj, "enum_type_semantic", sc.enum_type_semantic) ||
                        !parse_json_string_field_optional_(switch_case_obj, "enum_variant_name", sc.enum_variant_name) ||
                        !parse_json_uint_field_optional_(switch_case_obj, "enum_bind_begin", sc.enum_bind_begin) ||
                        !parse_json_uint_field_optional_(switch_case_obj, "enum_bind_count", sc.enum_bind_count) ||
                        !parse_json_uint_field_(switch_case_obj, "body", sc.body)) {
                        out_err = "invalid template-sidecar switch case entry in: " + path;
                        return false;
                    }
                    sc.pat_kind = static_cast<uint8_t>(pat_kind);
                    entry.switch_cases.push_back(std::move(sc));
                }

                std::vector<std::string> switch_enum_bind_objs{};
                if (!parse_json_array_object_slices_(obj, "switch_enum_binds", switch_enum_bind_objs)) {
                    out_err = "invalid template-sidecar switch_enum_binds array in: " + path;
                    return false;
                }
                for (const auto& switch_enum_bind_obj : switch_enum_bind_objs) {
                    TemplateSidecarSwitchEnumBind sb{};
                    if (!parse_json_string_field_(switch_enum_bind_obj, "field_name", sb.field_name) ||
                        !parse_json_string_field_(switch_enum_bind_obj, "bind_name", sb.bind_name) ||
                        !parse_json_string_field_optional_(switch_enum_bind_obj, "bind_type_repr", sb.bind_type_repr) ||
                        !parse_json_string_field_optional_(switch_enum_bind_obj, "bind_type_semantic", sb.bind_type_semantic)) {
                        out_err = "invalid template-sidecar switch enum bind entry in: " + path;
                        return false;
                    }
                    entry.switch_enum_binds.push_back(std::move(sb));
                }

                std::vector<std::string> param_objs{};
                if (!parse_json_array_object_slices_(obj, "params", param_objs)) {
                    out_err = "invalid template-sidecar params array in: " + path;
                    return false;
                }
                for (const auto& param_obj : param_objs) {
                    TemplateSidecarParam p{};
                    uint32_t param_self_kind = 0;
                    if (!parse_json_string_field_(param_obj, "name", p.name) ||
                        !parse_json_string_field_(param_obj, "type_repr", p.type_repr) ||
                        !parse_json_string_field_optional_(param_obj, "type_semantic", p.type_semantic) ||
                        !parse_json_bool_field_(param_obj, "is_mut", p.is_mut) ||
                        !parse_json_bool_field_(param_obj, "is_self", p.is_self) ||
                        !parse_json_uint_field_(param_obj, "self_kind", param_self_kind) ||
                        !parse_json_bool_field_(param_obj, "has_default", p.has_default) ||
                        !parse_json_uint_field_(param_obj, "default_expr", p.default_expr) ||
                        !parse_json_bool_field_(param_obj, "is_named_group", p.is_named_group)) {
                        out_err = "invalid template-sidecar param entry in: " + path;
                        return false;
                    }
                    p.self_kind = static_cast<uint8_t>(param_self_kind);
                    entry.params.push_back(std::move(p));
                }

                std::vector<std::string> arg_objs{};
                if (!parse_json_array_object_slices_(obj, "args", arg_objs)) {
                    out_err = "invalid template-sidecar args array in: " + path;
                    return false;
                }
                for (const auto& arg_obj : arg_objs) {
                    TemplateSidecarArg a{};
                    uint32_t arg_kind = 0;
                    if (!parse_json_uint_field_(arg_obj, "kind", arg_kind) ||
                        !parse_json_bool_field_(arg_obj, "has_label", a.has_label) ||
                        !parse_json_bool_field_(arg_obj, "is_hole", a.is_hole) ||
                        !parse_json_string_field_(arg_obj, "label", a.label) ||
                        !parse_json_uint_field_(arg_obj, "expr", a.expr)) {
                        out_err = "invalid template-sidecar arg entry in: " + path;
                        return false;
                    }
                    a.kind = static_cast<uint8_t>(arg_kind);
                    entry.args.push_back(std::move(a));
                }

                std::vector<std::string> init_objs{};
                if (!parse_json_array_object_slices_(obj, "field_inits", init_objs)) {
                    out_err = "invalid template-sidecar field_inits array in: " + path;
                    return false;
                }
                for (const auto& init_obj : init_objs) {
                    TemplateSidecarFieldInit fi{};
                    if (!parse_json_string_field_(init_obj, "name", fi.name) ||
                        !parse_json_uint_field_(init_obj, "expr", fi.expr)) {
                        out_err = "invalid template-sidecar field_init entry in: " + path;
                        return false;
                    }
                    entry.field_inits.push_back(std::move(fi));
                }

                std::vector<std::string> field_member_objs{};
                if (!parse_json_array_object_slices_(obj, "field_members", field_member_objs)) {
                    out_err = "invalid template-sidecar field_members array in: " + path;
                    return false;
                }
                for (const auto& field_member_obj : field_member_objs) {
                    TemplateSidecarFieldMember fm{};
                    uint32_t visibility = 0;
                    if (!parse_json_string_field_(field_member_obj, "name", fm.name) ||
                        !parse_json_string_field_(field_member_obj, "type_repr", fm.type_repr) ||
                        !parse_json_string_field_optional_(field_member_obj, "type_semantic", fm.type_semantic) ||
                        !parse_json_uint_field_(field_member_obj, "visibility", visibility)) {
                        out_err = "invalid template-sidecar field member entry in: " + path;
                        return false;
                    }
                    fm.visibility = static_cast<uint8_t>(visibility);
                    entry.field_members.push_back(std::move(fm));
                }

                std::vector<std::string> enum_variant_objs{};
                if (!parse_json_array_object_slices_(obj, "enum_variants", enum_variant_objs)) {
                    out_err = "invalid template-sidecar enum_variants array in: " + path;
                    return false;
                }
                for (const auto& enum_variant_obj : enum_variant_objs) {
                    TemplateSidecarEnumVariant ev{};
                    if (!parse_json_string_field_(enum_variant_obj, "name", ev.name) ||
                        !parse_json_uint_field_(enum_variant_obj, "payload_begin", ev.payload_begin) ||
                        !parse_json_uint_field_(enum_variant_obj, "payload_count", ev.payload_count) ||
                        !parse_json_bool_field_(enum_variant_obj, "has_discriminant", ev.has_discriminant) ||
                        !parse_json_i64_field_(enum_variant_obj, "discriminant", ev.discriminant)) {
                        out_err = "invalid template-sidecar enum variant entry in: " + path;
                        return false;
                    }
                    entry.enum_variants.push_back(std::move(ev));
                }

                std::vector<std::string> fpart_objs{};
                if (!parse_json_array_object_slices_(obj, "fstring_parts", fpart_objs)) {
                    out_err = "invalid template-sidecar fstring_parts array in: " + path;
                    return false;
                }
                for (const auto& part_obj : fpart_objs) {
                    TemplateSidecarFStringPart fp{};
                    if (!parse_json_bool_field_(part_obj, "is_expr", fp.is_expr) ||
                        !parse_json_string_field_(part_obj, "text", fp.text) ||
                        !parse_json_uint_field_(part_obj, "expr", fp.expr)) {
                        out_err = "invalid template-sidecar fstring part entry in: " + path;
                        return false;
                    }
                    entry.fstring_parts.push_back(std::move(fp));
                }

                if (!parse_json_array_string_field_(obj, "type_args", entry.type_args)) {
                    out_err = "invalid template-sidecar type_args array in: " + path;
                    return false;
                }

                std::vector<std::string> gp_objs{};
                if (!parse_json_array_object_slices_(obj, "generic_params", gp_objs)) {
                    out_err = "invalid template-sidecar generic_params array in: " + path;
                    return false;
                }
                for (const auto& gp_obj : gp_objs) {
                    TemplateSidecarGenericParam gp{};
                    if (!parse_json_string_field_(gp_obj, "name", gp.name)) {
                        out_err = "invalid template-sidecar generic param entry in: " + path;
                        return false;
                    }
                    entry.generic_params.push_back(std::move(gp));
                }

                std::vector<std::string> constraint_objs{};
                if (!parse_json_array_object_slices_(obj, "constraints", constraint_objs)) {
                    out_err = "invalid template-sidecar constraints array in: " + path;
                    return false;
                }
                for (const auto& constraint_obj : constraint_objs) {
                    TemplateSidecarFnConstraint c{};
                    uint32_t constraint_kind = 0;
                    if (!parse_json_uint_field_(constraint_obj, "kind", constraint_kind) ||
                        !parse_json_string_field_(constraint_obj, "lhs", c.type_param) ||
                        !parse_json_string_field_(constraint_obj, "rhs_type_repr", c.rhs_type_repr) ||
                        !parse_json_string_field_optional_(constraint_obj, "proto_bundle", c.proto.bundle) ||
                        !parse_json_string_field_optional_(constraint_obj, "proto_module_head", c.proto.module_head) ||
                        !parse_json_string_field_optional_(constraint_obj, "proto_path", c.proto.path)) {
                        out_err = "invalid template-sidecar constraint entry in: " + path;
                        return false;
                    }
                    c.kind = static_cast<uint8_t>(constraint_kind);
                    entry.constraints.push_back(std::move(c));
                }

                std::vector<std::string> path_ref_objs{};
                if (!parse_json_array_object_slices_(obj, "path_refs", path_ref_objs)) {
                    out_err = "invalid template-sidecar path_refs array in: " + path;
                    return false;
                }
                for (const auto& path_ref_obj : path_ref_objs) {
                    TemplateSidecarPathRef pr{};
                    if (!parse_json_string_field_(path_ref_obj, "path", pr.path) ||
                        !parse_json_string_field_(path_ref_obj, "type_repr", pr.type_repr) ||
                        !parse_json_string_field_optional_(path_ref_obj, "type_semantic", pr.type_semantic)) {
                        out_err = "invalid template-sidecar path_ref entry in: " + path;
                        return false;
                    }
                    entry.path_refs.push_back(std::move(pr));
                }

                std::vector<std::string> witness_objs{};
                if (!parse_json_array_object_slices_(obj, "acts_assoc_witnesses", witness_objs)) {
                    out_err = "invalid template-sidecar acts_assoc_witnesses array in: " + path;
                    return false;
                }
                for (const auto& witness_obj : witness_objs) {
                    TemplateSidecarActsAssocWitness witness{};
                    if (!parse_json_string_field_(witness_obj, "assoc_name", witness.assoc_name) ||
                        !parse_json_string_field_(witness_obj, "rhs_type_repr", witness.rhs_type_repr) ||
                        !parse_json_string_field_optional_(witness_obj, "rhs_type_semantic", witness.rhs_type_semantic)) {
                        out_err = "invalid template-sidecar acts assoc witness entry in: " + path;
                        return false;
                    }
                    entry.acts_assoc_witnesses.push_back(std::move(witness));
                }

                out.push_back(std::move(entry));
            }
            return true;
        }

        bool load_external_index_(
            const std::string& path,
            LoadedExternalIndex& out,
            std::string& out_err
        ) {
            out = LoadedExternalIndex{};
            out.export_index_path = path;
            if (!load_export_index_(path, out.bundle, out.entries, out_err)) {
                return false;
            }
            if (!load_template_sidecar_(path, out.bundle, out.sidecars, out_err)) {
                return false;
            }
            return true;
        }

        parus::ty::TypeId parse_type_repr_into_(
            std::string_view type_repr,
            std::string_view type_semantic,
            std::string_view inst_payload,
            parus::ty::TypePool& types
        ) {
            return parus::cimport::parse_external_type_repr(type_repr, type_semantic, inst_payload, types);
        }

        std::string_view clone_sv_into_ast_(parus::ast::AstArena& dst, std::string_view s);

        bool load_imported_templates_into_ast_(
            const std::vector<LoadedExternalIndex>& loaded,
            std::string_view current_norm,
            parus::SourceManager& sm,
            parus::ast::AstArena& ast,
            parus::ty::TypePool& types,
            std::vector<parus::tyck::ImportedFnTemplate>& out_fn_templates,
            std::vector<parus::tyck::ImportedProtoTemplate>& out_proto_templates,
            std::vector<parus::tyck::ImportedActsTemplate>& out_acts_templates,
            std::vector<parus::tyck::ImportedClassTemplate>& out_class_templates,
            std::vector<parus::tyck::ImportedFieldTemplate>& out_field_templates,
            std::vector<parus::tyck::ImportedEnumTemplate>& out_enum_templates,
            std::unordered_map<uint32_t, std::string>& out_file_bundle_overrides,
            std::unordered_map<uint32_t, std::string>& out_file_module_head_overrides,
            std::string& out_err
        ) {
            out_fn_templates.clear();
            out_proto_templates.clear();
            out_acts_templates.clear();
            out_class_templates.clear();
            out_field_templates.clear();
            out_enum_templates.clear();
            out_file_bundle_overrides.clear();
            out_file_module_head_overrides.clear();
            out_err.clear();
            std::unordered_map<std::string, parus::ty::TypeId> imported_type_cache{};

            const auto make_anchor_span = [&](const TemplateSidecarFunction& templ) -> parus::Span {
                std::string fake{};
                if (templ.decl_line > 1) fake.append(templ.decl_line - 1, '\n');
                if (templ.decl_col > 1) fake.append(templ.decl_col - 1, ' ');
                const uint32_t lo = static_cast<uint32_t>(fake.size());
                fake.push_back('x');
                const std::string fake_name =
                    templ.decl_file + "#template:" +
                    (!templ.lookup_name.empty() ? templ.lookup_name : templ.link_name);
                const uint32_t fid = sm.add(fake_name, std::move(fake));
                out_file_bundle_overrides[fid] = templ.bundle;
                out_file_module_head_overrides[fid] = templ.module_head;
                return parus::Span{fid, lo, lo + 1};
            };

            const auto add_type_node_for = [&](parus::ty::TypeId tid, const parus::Span& sp) -> parus::ast::TypeNodeId {
                if (tid == parus::ty::kInvalidType) return parus::ast::k_invalid_type_node;
                parus::ast::TypeNode tn{};
                tn.kind = parus::ast::TypeNodeKind::kError;
                tn.span = sp;
                tn.resolved_type = tid;
                return ast.add_type_node(tn);
            };

            std::unordered_map<std::string, std::string> seen_sidecar_keys{};
            for (const auto& index : loaded) {
                auto relative_module_head = [&](std::string_view module_head) -> std::string {
                    const std::string prefix = index.bundle + "::";
                    if (module_head.starts_with(prefix)) {
                        return std::string(module_head.substr(prefix.size()));
                    }
                    return std::string(module_head);
                };

                std::unordered_set<std::string> bundle_module_heads{};
                std::unordered_map<std::string, std::unordered_set<std::string>> bundle_local_types_by_module{};
                bundle_module_heads.reserve(index.entries.size());
                bundle_local_types_by_module.reserve(index.entries.size());
                for (const auto& e : index.entries) {
                    if (!e.module_head.empty()) {
                        bundle_module_heads.insert(relative_module_head(e.module_head));
                    }
                    if ((e.kind == parus::sema::SymbolKind::kType ||
                         e.kind == parus::sema::SymbolKind::kField) &&
                        !e.module_head.empty() &&
                        !e.path.empty() &&
                        e.path.find("::") == std::string::npos) {
                        bundle_local_types_by_module[relative_module_head(e.module_head)].insert(e.path);
                    }
                }
                for (const auto& templ : index.sidecars) {
                    if (templ.root_stmt == parus::ast::k_invalid_stmt ||
                        templ.root_stmt >= templ.stmts.size()) {
                        continue;
                    }
                    const auto& root_stmt = templ.stmts[templ.root_stmt];
                    if (root_stmt.kind != static_cast<uint8_t>(parus::ast::StmtKind::kFieldDecl) &&
                        root_stmt.kind != static_cast<uint8_t>(parus::ast::StmtKind::kEnumDecl) &&
                        root_stmt.kind != static_cast<uint8_t>(parus::ast::StmtKind::kClassDecl) &&
                        root_stmt.kind != static_cast<uint8_t>(parus::ast::StmtKind::kProtoDecl)) {
                        continue;
                    }
                    if (root_stmt.name.empty()) continue;
                    const std::string module_head = relative_module_head(templ.module_head);
                    bundle_module_heads.insert(module_head);
                    if (root_stmt.name.find("::") == std::string::npos) {
                        bundle_local_types_by_module[module_head].insert(root_stmt.name);
                    }
                }
                const std::unordered_set<std::string> empty_local_names{};

                for (const auto& templ : index.sidecars) {
                    if (!templ.decl_file.empty() && templ.decl_file == current_norm) continue;
                    const std::string sidecar_key = template_sidecar_identity_key_(templ);
                    const std::string sidecar_fingerprint = template_sidecar_payload_fingerprint_(templ);
                    if (auto it = seen_sidecar_keys.find(sidecar_key); it != seen_sidecar_keys.end()) {
                        if (it->second != sidecar_fingerprint) {
                            out_err = "conflicting canonical template-sidecar identity while loading: " + sidecar_key;
                            return false;
                        }
                        continue;
                    }
                    seen_sidecar_keys.emplace(sidecar_key, sidecar_fingerprint);
                    if (templ.root_stmt == parus::ast::k_invalid_stmt ||
                        templ.root_stmt >= templ.stmts.size()) {
                        out_err = "typed template sidecar missing valid root stmt: " + templ.lookup_name;
                        return false;
                    }

                    const parus::Span anchor = make_anchor_span(templ);

                    std::unordered_map<uint32_t, parus::ast::ExprId> expr_map{};
                    std::unordered_map<uint32_t, parus::ast::StmtId> stmt_map{};

                    std::function<parus::ast::ExprId(uint32_t)> clone_expr{};
                    std::function<parus::ast::StmtId(uint32_t)> clone_stmt{};
                    auto parse_imported_type_repr_into_ = [&](std::string_view repr,
                                                             std::string_view semantic,
                                                             std::string_view inst_payload)
                        -> parus::ty::TypeId {
                        std::string cache_key = index.bundle;
                        cache_key.push_back('|');
                        cache_key.append(templ.module_head);
                        cache_key.push_back('|');
                        cache_key.append(repr);
                        cache_key.push_back('|');
                        cache_key.append(semantic);
                        cache_key.push_back('|');
                        cache_key.append(inst_payload);
                        if (auto it = imported_type_cache.find(cache_key);
                            it != imported_type_cache.end()) {
                            return it->second;
                        }
                        const std::string current_module_head = relative_module_head(templ.module_head);
                        const auto it = bundle_local_types_by_module.find(current_module_head);
                        const auto& local_names =
                            (it != bundle_local_types_by_module.end()) ? it->second : empty_local_names;
                        const auto qualified = qualify_payload_type_meta_for_bundle_(
                            repr,
                            semantic,
                            index.bundle,
                            current_module_head,
                            bundle_module_heads,
                            local_names
                        );
                        const auto parsed =
                            parse_type_repr_into_(qualified.first, qualified.second, inst_payload, types);
                        imported_type_cache.emplace(std::move(cache_key), parsed);
                        return parsed;
                    };

                    clone_expr = [&](uint32_t src_idx) -> parus::ast::ExprId {
                        if (src_idx == parus::ast::k_invalid_expr || src_idx >= templ.exprs.size()) {
                            return parus::ast::k_invalid_expr;
                        }
                        if (auto it = expr_map.find(src_idx); it != expr_map.end()) return it->second;

                        const auto& src = templ.exprs[src_idx];
                        parus::ast::Expr e{};
                        e.kind = static_cast<parus::ast::ExprKind>(src.kind);
                        e.span = anchor;
                        e.op = static_cast<parus::syntax::TokenKind>(src.op);
                        e.unary_is_mut = src.unary_is_mut;
                        e.text = clone_sv_into_ast_(ast, src.text);
                        e.string_is_raw = src.string_is_raw;
                        e.string_is_format = src.string_is_format;
                        e.string_folded_text = clone_sv_into_ast_(ast, src.string_folded_text);
                        e.call_from_pipe = src.call_from_pipe;
                        e.loop_has_header = src.loop_has_header;
                        e.loop_var = clone_sv_into_ast_(ast, src.loop_var);
                        e.cast_kind = static_cast<parus::ast::CastKind>(src.cast_kind);

                        const parus::ast::ExprId eid = ast.add_expr(e);
                        expr_map[src_idx] = eid;
                        parus::ast::Expr dst = ast.expr(eid);

                        dst.a = clone_expr(src.a);
                        dst.b = clone_expr(src.b);
                        dst.c = clone_expr(src.c);
                        dst.block_tail = clone_expr(src.block_tail);
                        dst.loop_iter = clone_expr(src.loop_iter);
                        dst.block_stmt = clone_stmt(src.block_stmt);
                        dst.loop_body = clone_stmt(src.loop_body);

                        const uint64_t arg_begin = src.arg_begin;
                        const uint64_t arg_end = arg_begin + src.arg_count;
                        if (arg_begin <= templ.args.size() && arg_end <= templ.args.size()) {
                            dst.arg_begin = static_cast<uint32_t>(ast.args().size());
                            dst.arg_count = src.arg_count;
                            for (uint32_t i = 0; i < src.arg_count; ++i) {
                                const auto& a = templ.args[src.arg_begin + i];
                                parus::ast::Arg out_a{};
                                out_a.kind = static_cast<parus::ast::ArgKind>(a.kind);
                                out_a.has_label = a.has_label;
                                out_a.is_hole = a.is_hole;
                                out_a.label = clone_sv_into_ast_(ast, a.label);
                                out_a.expr = clone_expr(a.expr);
                                out_a.span = anchor;
                                ast.add_arg(out_a);
                            }
                        }

                        const uint64_t type_begin = src.call_type_arg_begin;
                        const uint64_t type_end = type_begin + src.call_type_arg_count;
                        if (type_begin <= templ.type_args.size() && type_end <= templ.type_args.size()) {
                            dst.call_type_arg_begin = static_cast<uint32_t>(ast.type_args().size());
                            dst.call_type_arg_count = src.call_type_arg_count;
                            for (uint32_t i = 0; i < src.call_type_arg_count; ++i) {
                                const auto tid = parse_imported_type_repr_into_(
                                    templ.type_args[src.call_type_arg_begin + i],
                                    std::string_view{},
                                    std::string_view{}
                                );
                                ast.add_type_arg(tid);
                            }
                        }

                        const uint64_t init_begin = src.field_init_begin;
                        const uint64_t init_end = init_begin + src.field_init_count;
                        if (init_begin <= templ.field_inits.size() && init_end <= templ.field_inits.size()) {
                            dst.field_init_begin = static_cast<uint32_t>(ast.field_init_entries().size());
                            dst.field_init_count = src.field_init_count;
                            for (uint32_t i = 0; i < src.field_init_count; ++i) {
                                const auto& fi = templ.field_inits[src.field_init_begin + i];
                                parus::ast::FieldInitEntry out_fi{};
                                out_fi.name = clone_sv_into_ast_(ast, fi.name);
                                out_fi.expr = clone_expr(fi.expr);
                                out_fi.span = anchor;
                                ast.add_field_init_entry(out_fi);
                            }
                        }

                        const uint64_t part_begin = src.string_part_begin;
                        const uint64_t part_end = part_begin + src.string_part_count;
                        if (part_begin <= templ.fstring_parts.size() && part_end <= templ.fstring_parts.size()) {
                            dst.string_part_begin = static_cast<uint32_t>(ast.fstring_parts().size());
                            dst.string_part_count = src.string_part_count;
                            for (uint32_t i = 0; i < src.string_part_count; ++i) {
                                const auto& fp = templ.fstring_parts[src.string_part_begin + i];
                                parus::ast::FStringPart out_fp{};
                                out_fp.is_expr = fp.is_expr;
                                out_fp.text = clone_sv_into_ast_(ast, fp.text);
                                out_fp.expr = clone_expr(fp.expr);
                                out_fp.span = anchor;
                                ast.add_fstring_part(out_fp);
                            }
                        }

                        if (!src.field_init_type_repr.empty()) {
                            const auto tid = parse_imported_type_repr_into_(
                                src.field_init_type_repr,
                                src.field_init_type_semantic,
                                std::string_view{}
                            );
                            dst.field_init_type_node = add_type_node_for(tid, anchor);
                        }
                        if (!src.cast_type_repr.empty()) {
                            dst.cast_type = parse_imported_type_repr_into_(
                                src.cast_type_repr,
                                src.cast_type_semantic,
                                std::string_view{}
                            );
                            dst.cast_type_node = add_type_node_for(dst.cast_type, anchor);
                        }
                        if (!src.target_type_repr.empty()) {
                            dst.target_type = parse_imported_type_repr_into_(
                                src.target_type_repr,
                                src.target_type_semantic,
                                std::string_view{}
                            );
                        }

                        ast.expr_mut(eid) = std::move(dst);
                        return eid;
                    };

                    clone_stmt = [&](uint32_t src_idx) -> parus::ast::StmtId {
                        if (src_idx == parus::ast::k_invalid_stmt || src_idx >= templ.stmts.size()) {
                            return parus::ast::k_invalid_stmt;
                        }
                        if (auto it = stmt_map.find(src_idx); it != stmt_map.end()) return it->second;

                        const auto& src = templ.stmts[src_idx];
                        parus::ast::Stmt s{};
                        s.kind = static_cast<parus::ast::StmtKind>(src.kind);
                        s.span = anchor;
                        s.expr = clone_expr(src.expr);
                        s.init = clone_expr(src.init);
                        s.is_set = src.is_set;
                        s.is_mut = src.is_mut;
                        s.is_static = src.is_static;
                        s.is_const = src.is_const;
                        s.is_extern = src.is_extern;
                        s.link_abi = static_cast<parus::ast::LinkAbi>(src.link_abi);
                        s.name = clone_sv_into_ast_(ast, src.name);
                        s.is_export = src.is_export;
                        s.fn_mode = static_cast<parus::ast::FnMode>(src.fn_mode);
                        s.member_visibility =
                            static_cast<parus::ast::FieldMember::Visibility>(src.member_visibility);
                        s.is_pure = src.is_pure;
                        s.is_comptime = src.is_comptime;
                        s.is_commit = src.is_commit;
                        s.is_recast = src.is_recast;
                        s.is_throwing = src.is_throwing;
                        s.fn_is_const = src.fn_is_const;
                        s.positional_param_count = src.positional_param_count;
                        s.has_named_group = src.has_named_group;
                        s.fn_is_c_variadic = src.fn_is_c_variadic;
                        s.fn_is_proto_sig = src.fn_is_proto_sig;
                        s.fn_generic_param_begin = src.fn_generic_param_begin;
                        s.fn_generic_param_count = src.fn_generic_param_count;
                        s.fn_constraint_begin = src.fn_constraint_begin;
                        s.fn_constraint_count = src.fn_constraint_count;
                        s.decl_generic_param_begin = src.decl_generic_param_begin;
                        s.decl_generic_param_count = src.decl_generic_param_count;
                        s.decl_constraint_begin = src.decl_constraint_begin;
                        s.decl_constraint_count = src.decl_constraint_count;
                        s.decl_path_ref_begin = src.decl_path_ref_begin;
                        s.decl_path_ref_count = src.decl_path_ref_count;
                        s.field_layout = static_cast<parus::ast::FieldLayout>(src.field_layout);
                        s.field_align = src.field_align;
                        s.field_member_begin = src.field_member_begin;
                        s.field_member_count = src.field_member_count;
                        s.proto_fn_role = static_cast<parus::ast::ProtoFnRole>(src.proto_fn_role);
                        s.proto_require_kind = static_cast<parus::ast::ProtoRequireKind>(src.proto_require_kind);
                        s.assoc_type_role = static_cast<parus::ast::AssocTypeRole>(src.assoc_type_role);
                        s.var_is_proto_provide = src.var_is_proto_provide;
                        s.acts_is_for = src.acts_is_for;
                        s.acts_has_set_name = src.acts_has_set_name;
                        s.acts_assoc_witness_begin = src.acts_assoc_witness_begin;
                        s.acts_assoc_witness_count = src.acts_assoc_witness_count;
                        s.manual_perm_mask = src.manual_perm_mask;
                        s.var_has_consume_else = src.var_has_consume_else;

                        if (!src.type_repr.empty()) {
                            s.type = parse_imported_type_repr_into_(
                                src.type_repr,
                                src.type_semantic,
                                std::string_view{}
                            );
                            s.type_node = add_type_node_for(s.type, anchor);
                        }
                        if (!src.fn_ret_repr.empty()) {
                            s.fn_ret = parse_imported_type_repr_into_(
                                src.fn_ret_repr,
                                src.fn_ret_semantic,
                                std::string_view{}
                            );
                            s.fn_ret_type_node = add_type_node_for(s.fn_ret, anchor);
                        }
                        if (!src.acts_target_type_repr.empty()) {
                            s.acts_target_type = parse_imported_type_repr_into_(
                                src.acts_target_type_repr,
                                src.acts_target_type_semantic,
                                std::string_view{}
                            );
                            s.acts_target_type_node = add_type_node_for(s.acts_target_type, anchor);
                        }
                        const parus::ast::StmtId sid = ast.add_stmt(s);
                        stmt_map[src_idx] = sid;
                        parus::ast::Stmt dst = ast.stmt(sid);

                        dst.a = clone_stmt(src.a);
                        dst.b = clone_stmt(src.b);

                        if (src.kind == static_cast<uint8_t>(parus::ast::StmtKind::kBlock) ||
                            src.kind == static_cast<uint8_t>(parus::ast::StmtKind::kProtoDecl) ||
                            src.kind == static_cast<uint8_t>(parus::ast::StmtKind::kActsDecl) ||
                            src.kind == static_cast<uint8_t>(parus::ast::StmtKind::kClassDecl)) {
                            const uint64_t begin = src.stmt_begin;
                            const uint64_t end = begin + src.stmt_count;
                            if (begin <= templ.stmt_children.size() && end <= templ.stmt_children.size()) {
                                dst.stmt_begin = static_cast<uint32_t>(ast.stmt_children().size());
                                dst.stmt_count = src.stmt_count;
                                for (uint32_t i = 0; i < src.stmt_count; ++i) {
                                    ast.add_stmt_child(parus::ast::k_invalid_stmt);
                                }
                                for (uint32_t i = 0; i < src.stmt_count; ++i) {
                                    ast.stmt_children_mut()[dst.stmt_begin + i] =
                                        clone_stmt(templ.stmt_children[src.stmt_begin + i]);
                                }
                            }
                            if (src.kind == static_cast<uint8_t>(parus::ast::StmtKind::kClassDecl)) {
                                dst.field_member_begin = static_cast<uint32_t>(ast.field_members().size());
                                dst.field_member_count = static_cast<uint32_t>(templ.field_members.size());
                                for (const auto& fm : templ.field_members) {
                                    parus::ast::FieldMember out_fm{};
                                    out_fm.name = clone_sv_into_ast_(ast, fm.name);
                                    out_fm.visibility =
                                        static_cast<parus::ast::FieldMember::Visibility>(fm.visibility);
                                    out_fm.span = anchor;
                                    if (!fm.type_repr.empty()) {
                                        out_fm.type = parse_imported_type_repr_into_(
                                            fm.type_repr,
                                            fm.type_semantic,
                                            std::string_view{}
                                        );
                                        out_fm.type_node = add_type_node_for(out_fm.type, anchor);
                                    }
                                    ast.add_field_member(out_fm);
                                }
                            }
                        } else if (src.kind == static_cast<uint8_t>(parus::ast::StmtKind::kFieldDecl)) {
                            dst.field_member_begin = static_cast<uint32_t>(ast.field_members().size());
                            dst.field_member_count = static_cast<uint32_t>(templ.field_members.size());
                            for (const auto& fm : templ.field_members) {
                                parus::ast::FieldMember out_fm{};
                                out_fm.name = clone_sv_into_ast_(ast, fm.name);
                                out_fm.visibility =
                                    static_cast<parus::ast::FieldMember::Visibility>(fm.visibility);
                                out_fm.span = anchor;
                                if (!fm.type_repr.empty()) {
                                    out_fm.type = parse_imported_type_repr_into_(
                                        fm.type_repr,
                                        fm.type_semantic,
                                        std::string_view{}
                                    );
                                    out_fm.type_node = add_type_node_for(out_fm.type, anchor);
                                }
                                ast.add_field_member(out_fm);
                            }
                        } else if (src.kind == static_cast<uint8_t>(parus::ast::StmtKind::kEnumDecl)) {
                            dst.enum_variant_begin = static_cast<uint32_t>(ast.enum_variant_decls().size());
                            dst.enum_variant_count = static_cast<uint32_t>(templ.enum_variants.size());
                            for (const auto& ev : templ.enum_variants) {
                                parus::ast::EnumVariantDecl out_ev{};
                                out_ev.name = clone_sv_into_ast_(ast, ev.name);
                                out_ev.payload_begin = static_cast<uint32_t>(ast.field_members().size());
                                out_ev.payload_count = ev.payload_count;
                                out_ev.has_discriminant = ev.has_discriminant;
                                out_ev.discriminant = ev.discriminant;
                                out_ev.span = anchor;
                                const uint64_t pbegin = ev.payload_begin;
                                const uint64_t pend = pbegin + ev.payload_count;
                                if (pbegin > templ.field_members.size() || pend > templ.field_members.size()) {
                                    out_err = "typed template sidecar enum payload field-member range out of bounds: " + templ.lookup_name;
                                    ast.stmt_mut(sid) = std::move(dst);
                                    return sid;
                                }
                                for (uint32_t mi = 0; mi < ev.payload_count; ++mi) {
                                    const auto& fm = templ.field_members[ev.payload_begin + mi];
                                    parus::ast::FieldMember out_fm{};
                                    out_fm.name = clone_sv_into_ast_(ast, fm.name);
                                    out_fm.visibility =
                                        static_cast<parus::ast::FieldMember::Visibility>(fm.visibility);
                                    out_fm.span = anchor;
                                    if (!fm.type_repr.empty()) {
                                        out_fm.type = parse_imported_type_repr_into_(
                                            fm.type_repr,
                                            fm.type_semantic,
                                            std::string_view{}
                                        );
                                        out_fm.type_node = add_type_node_for(out_fm.type, anchor);
                                    }
                                    ast.add_field_member(out_fm);
                                }
                                ast.add_enum_variant_decl(out_ev);
                            }
                        } else if (src.kind == static_cast<uint8_t>(parus::ast::StmtKind::kSwitch)) {
                            const uint64_t begin = src.case_begin;
                            const uint64_t end = begin + src.case_count;
                            if (begin > templ.switch_cases.size() || end > templ.switch_cases.size()) {
                                out_err = "typed template sidecar switch case range out of bounds: " + templ.lookup_name;
                                ast.stmt_mut(sid) = std::move(dst);
                                return sid;
                            }
                            dst.case_begin = static_cast<uint32_t>(ast.switch_cases().size());
                            dst.case_count = src.case_count;
                            dst.has_default = src.has_default;
                            for (uint32_t i = 0; i < src.case_count; ++i) {
                                const auto& sc = templ.switch_cases[src.case_begin + i];
                                parus::ast::SwitchCase out_sc{};
                                out_sc.is_default = sc.is_default;
                                out_sc.pat_kind = static_cast<parus::ast::CasePatKind>(sc.pat_kind);
                                out_sc.pat_text = clone_sv_into_ast_(ast, sc.pat_text);
                                out_sc.enum_variant_name = clone_sv_into_ast_(ast, sc.enum_variant_name);
                                if (!sc.enum_type_repr.empty()) {
                                    out_sc.enum_type = parse_imported_type_repr_into_(
                                        sc.enum_type_repr,
                                        sc.enum_type_semantic,
                                        std::string_view{}
                                    );
                                    out_sc.enum_type_node = add_type_node_for(out_sc.enum_type, anchor);
                                }
                                const uint64_t ebb = sc.enum_bind_begin;
                                const uint64_t ebe = ebb + sc.enum_bind_count;
                                if (ebb > templ.switch_enum_binds.size() || ebe > templ.switch_enum_binds.size()) {
                                    out_err = "typed template sidecar switch enum bind range out of bounds: " + templ.lookup_name;
                                    ast.stmt_mut(sid) = std::move(dst);
                                    return sid;
                                }
                                out_sc.enum_bind_begin = static_cast<uint32_t>(ast.switch_enum_binds().size());
                                out_sc.enum_bind_count = sc.enum_bind_count;
                                for (uint32_t bi = 0; bi < sc.enum_bind_count; ++bi) {
                                    const auto& sb = templ.switch_enum_binds[sc.enum_bind_begin + bi];
                                    parus::ast::SwitchEnumBind out_sb{};
                                    out_sb.field_name = clone_sv_into_ast_(ast, sb.field_name);
                                    out_sb.bind_name = clone_sv_into_ast_(ast, sb.bind_name);
                                    out_sb.span = anchor;
                                    if (!sb.bind_type_repr.empty()) {
                                        out_sb.bind_type = parse_imported_type_repr_into_(
                                            sb.bind_type_repr,
                                            sb.bind_type_semantic,
                                            std::string_view{}
                                        );
                                    }
                                    ast.add_switch_enum_bind(out_sb);
                                }
                                out_sc.body = clone_stmt(sc.body);
                                out_sc.span = anchor;
                                ast.add_switch_case(out_sc);
                            }
                        } else if (src.kind == static_cast<uint8_t>(parus::ast::StmtKind::kFnDecl)) {
                            dst.param_begin = static_cast<uint32_t>(ast.params().size());
                            dst.param_count = src.param_count;
                            const uint64_t p_begin = src.param_begin;
                            const uint64_t p_end = p_begin + src.param_count;
                            if (p_begin > templ.params.size() || p_end > templ.params.size()) {
                                out_err = "typed template sidecar param range out of bounds: " + templ.lookup_name;
                                return sid;
                            }
                            for (uint32_t i = 0; i < src.param_count; ++i) {
                                const auto& p = templ.params[src.param_begin + i];
                                parus::ast::Param out_p{};
                                out_p.name = clone_sv_into_ast_(ast, p.name);
                                out_p.is_mut = p.is_mut;
                                out_p.is_self = p.is_self;
                                out_p.self_kind = static_cast<parus::ast::SelfReceiverKind>(p.self_kind);
                                out_p.has_default = p.has_default;
                                out_p.default_expr = clone_expr(p.default_expr);
                                out_p.is_named_group = p.is_named_group;
                                out_p.span = anchor;
                                if (dst.type != parus::ty::kInvalidType &&
                                    dst.type < types.count() &&
                                    types.get(dst.type).kind == parus::ty::Kind::kFn &&
                                    i < types.get(dst.type).param_count) {
                                    out_p.type = types.fn_param_at(dst.type, i);
                                    out_p.type_node = add_type_node_for(out_p.type, anchor);
                                } else if (!p.type_repr.empty()) {
                                    out_p.type = parse_imported_type_repr_into_(
                                        p.type_repr,
                                        p.type_semantic,
                                        std::string_view{}
                                    );
                                    out_p.type_node = add_type_node_for(out_p.type, anchor);
                                }
                                ast.add_param(out_p);
                            }
                        }
                        ast.stmt_mut(sid) = std::move(dst);
                        return sid;
                    };

                    const uint32_t gp_begin = static_cast<uint32_t>(ast.generic_param_decls().size());
                    for (const auto& gp : templ.generic_params) {
                        parus::ast::GenericParamDecl out_gp{};
                        out_gp.name = clone_sv_into_ast_(ast, gp.name);
                        out_gp.span = anchor;
                        ast.add_generic_param_decl(out_gp);
                    }

                    const uint32_t cc_begin = static_cast<uint32_t>(ast.fn_constraint_decls().size());
                    for (const auto& cc : templ.constraints) {
                        parus::ast::FnConstraintDecl out_cc{};
                        out_cc.kind = static_cast<parus::ast::FnConstraintKind>(cc.kind);
                        out_cc.type_param = clone_sv_into_ast_(ast, cc.type_param);
                        out_cc.span = anchor;
                        if (!cc.rhs_type_repr.empty()) {
                            out_cc.rhs_type = parse_imported_type_repr_into_(
                                cc.rhs_type_repr,
                                std::string_view{},
                                std::string_view{}
                            );
                        }
                        if (!cc.proto.path.empty()) {
                            const auto segs = split_path_text_(cc.proto.path);
                            out_cc.proto_path_begin = static_cast<uint32_t>(ast.path_segs().size());
                            out_cc.proto_path_count = static_cast<uint32_t>(segs.size());
                            for (const auto& seg : segs) {
                                ast.add_path_seg(seg);
                            }
                        }
                        ast.add_fn_constraint_decl(out_cc);
                    }

                    const auto root_sid = clone_stmt(templ.root_stmt);
                    if (!out_err.empty()) return false;
                    if (root_sid == parus::ast::k_invalid_stmt ||
                        static_cast<size_t>(root_sid) >= ast.stmts().size()) {
                        out_err = "typed template sidecar failed to reconstruct root decl: " + templ.lookup_name;
                        return false;
                    }
                    auto& root_stmt = ast.stmt_mut(root_sid);
                    if (root_stmt.kind != parus::ast::StmtKind::kFnDecl &&
                        root_stmt.kind != parus::ast::StmtKind::kProtoDecl &&
                        root_stmt.kind != parus::ast::StmtKind::kActsDecl &&
                        root_stmt.kind != parus::ast::StmtKind::kClassDecl &&
                        root_stmt.kind != parus::ast::StmtKind::kFieldDecl &&
                        root_stmt.kind != parus::ast::StmtKind::kEnumDecl) {
                        out_err = "typed template sidecar root kind is unsupported: " + templ.lookup_name;
                        return false;
                    }
                    if (root_stmt.kind == parus::ast::StmtKind::kFnDecl) {
                        root_stmt.fn_generic_param_begin = gp_begin;
                        root_stmt.fn_generic_param_count = static_cast<uint32_t>(templ.generic_params.size());
                        root_stmt.fn_constraint_begin = cc_begin;
                        root_stmt.fn_constraint_count = static_cast<uint32_t>(templ.constraints.size());
                    } else {
                        root_stmt.decl_generic_param_begin = gp_begin;
                        root_stmt.decl_generic_param_count = static_cast<uint32_t>(templ.generic_params.size());
                        root_stmt.decl_constraint_begin = cc_begin;
                        root_stmt.decl_constraint_count = static_cast<uint32_t>(templ.constraints.size());
                    }
                    if (!templ.declared_type_repr.empty()) {
                        root_stmt.type = parse_imported_type_repr_into_(
                            templ.declared_type_repr,
                            templ.declared_type_semantic,
                            std::string_view{}
                        );
                    }
                    if (root_stmt.kind == parus::ast::StmtKind::kProtoDecl ||
                        root_stmt.kind == parus::ast::StmtKind::kClassDecl ||
                        root_stmt.kind == parus::ast::StmtKind::kFieldDecl) {
                        root_stmt.decl_path_ref_begin = static_cast<uint32_t>(ast.path_refs().size());
                        root_stmt.decl_path_ref_count = static_cast<uint32_t>(templ.path_refs.size());
                        for (const auto& pr : templ.path_refs) {
                            parus::ast::PathRef out_pr{};
                            out_pr.span = anchor;
                            if (!pr.path.empty()) {
                                const auto segs = split_path_text_(pr.path);
                                out_pr.path_begin = static_cast<uint32_t>(ast.path_segs().size());
                                out_pr.path_count = static_cast<uint32_t>(segs.size());
                                for (const auto& seg : segs) {
                                    ast.add_path_seg(seg);
                                }
                            }
                            if (!pr.type_repr.empty()) {
                                out_pr.type = parse_imported_type_repr_into_(
                                    pr.type_repr,
                                    pr.type_semantic,
                                    std::string_view{}
                                );
                                out_pr.type_node = add_type_node_for(out_pr.type, anchor);
                            }
                            ast.add_path_ref(out_pr);
                        }
                    }
                    if (root_stmt.kind == parus::ast::StmtKind::kActsDecl) {
                        root_stmt.acts_assoc_witness_begin = static_cast<uint32_t>(ast.acts_assoc_type_witness_decls().size());
                        root_stmt.acts_assoc_witness_count = static_cast<uint32_t>(templ.acts_assoc_witnesses.size());
                        for (const auto& witness : templ.acts_assoc_witnesses) {
                            parus::ast::ActsAssocTypeWitnessDecl out_w{};
                            out_w.assoc_name = clone_sv_into_ast_(ast, witness.assoc_name);
                            out_w.span = anchor;
                            if (!witness.rhs_type_repr.empty()) {
                                out_w.rhs_type = parse_imported_type_repr_into_(
                                    witness.rhs_type_repr,
                                    witness.rhs_type_semantic,
                                    std::string_view{}
                                );
                                out_w.rhs_type_node = add_type_node_for(out_w.rhs_type, anchor);
                            }
                            ast.add_acts_assoc_type_witness_decl(out_w);
                        }
                    }

                    const auto copy_constraints = [&]() {
                        std::vector<parus::tyck::ImportedFnConstraintMeta> out{};
                        out.reserve(templ.constraints.size());
                        for (const auto& cc : templ.constraints) {
                            parus::tyck::ImportedFnConstraintMeta meta{};
                            meta.kind = static_cast<parus::ast::FnConstraintKind>(cc.kind);
                            meta.lhs = cc.type_param;
                            meta.rhs_type_repr = cc.rhs_type_repr;
                            meta.proto = cc.proto;
                            out.push_back(std::move(meta));
                        }
                        return out;
                    };

                    if (root_stmt.kind == parus::ast::StmtKind::kFnDecl) {
                        parus::tyck::ImportedFnTemplate out_t{};
                        out_t.template_sid = root_sid;
                        out_t.producer_bundle = templ.bundle.empty() ? index.bundle : templ.bundle;
                        out_t.module_head = templ.module_head;
                        out_t.public_path = templ.public_path;
                        out_t.link_name = templ.link_name;
                        out_t.lookup_name = templ.lookup_name;
                        out_t.decl_file = templ.decl_file;
                        out_t.decl_line = templ.decl_line;
                        out_t.decl_col = templ.decl_col;
                        out_t.is_public_export = templ.is_public_export;
                        out_t.declared_type = root_stmt.type;
                        out_t.constraints = copy_constraints();
                        out_fn_templates.push_back(std::move(out_t));
                    } else if (root_stmt.kind == parus::ast::StmtKind::kProtoDecl) {
                        parus::tyck::ImportedProtoTemplate out_t{};
                        out_t.template_sid = root_sid;
                        out_t.producer_bundle = templ.bundle.empty() ? index.bundle : templ.bundle;
                        out_t.module_head = templ.module_head;
                        out_t.public_path = templ.public_path;
                        out_t.lookup_name = templ.lookup_name;
                        out_t.decl_file = templ.decl_file;
                        out_t.decl_line = templ.decl_line;
                        out_t.decl_col = templ.decl_col;
                        out_t.is_public_export = templ.is_public_export;
                        out_t.declared_type = root_stmt.type;
                        out_t.constraints = copy_constraints();
                        out_proto_templates.push_back(std::move(out_t));
                    } else if (root_stmt.kind == parus::ast::StmtKind::kActsDecl) {
                        parus::tyck::ImportedActsTemplate out_t{};
                        out_t.template_sid = root_sid;
                        out_t.producer_bundle = templ.bundle.empty() ? index.bundle : templ.bundle;
                        out_t.module_head = templ.module_head;
                        out_t.public_path = templ.public_path;
                        out_t.lookup_name = templ.lookup_name;
                        out_t.decl_file = templ.decl_file;
                        out_t.decl_line = templ.decl_line;
                        out_t.decl_col = templ.decl_col;
                        out_t.is_public_export = templ.is_public_export;
                        out_t.constraints = copy_constraints();
                        out_acts_templates.push_back(std::move(out_t));
                    } else if (root_stmt.kind == parus::ast::StmtKind::kClassDecl) {
                        parus::tyck::ImportedClassTemplate out_t{};
                        out_t.template_sid = root_sid;
                        out_t.producer_bundle = templ.bundle.empty() ? index.bundle : templ.bundle;
                        out_t.module_head = templ.module_head;
                        out_t.public_path = templ.public_path;
                        out_t.lookup_name = templ.lookup_name;
                        out_t.decl_file = templ.decl_file;
                        out_t.decl_line = templ.decl_line;
                        out_t.decl_col = templ.decl_col;
                        out_t.is_public_export = templ.is_public_export;
                        out_t.declared_type = root_stmt.type;
                        out_t.constraints = copy_constraints();
                        out_class_templates.push_back(std::move(out_t));
                    } else if (root_stmt.kind == parus::ast::StmtKind::kFieldDecl) {
                        parus::tyck::ImportedFieldTemplate out_t{};
                        out_t.template_sid = root_sid;
                        out_t.producer_bundle = templ.bundle.empty() ? index.bundle : templ.bundle;
                        out_t.module_head = templ.module_head;
                        out_t.public_path = templ.public_path;
                        out_t.lookup_name = templ.lookup_name;
                        out_t.decl_file = templ.decl_file;
                        out_t.decl_line = templ.decl_line;
                        out_t.decl_col = templ.decl_col;
                        out_t.is_public_export = templ.is_public_export;
                        out_t.declared_type = root_stmt.type;
                        out_t.constraints = copy_constraints();
                        out_field_templates.push_back(std::move(out_t));
                    } else {
                        parus::tyck::ImportedEnumTemplate out_t{};
                        out_t.template_sid = root_sid;
                        out_t.producer_bundle = templ.bundle.empty() ? index.bundle : templ.bundle;
                        out_t.module_head = templ.module_head;
                        out_t.public_path = templ.public_path;
                        out_t.lookup_name = templ.lookup_name;
                        out_t.decl_file = templ.decl_file;
                        out_t.decl_line = templ.decl_line;
                        out_t.decl_col = templ.decl_col;
                        out_t.is_public_export = templ.is_public_export;
                        out_t.declared_type = root_stmt.type;
                        out_t.constraints = copy_constraints();
                        out_enum_templates.push_back(std::move(out_t));
                    }
                }
            }

            return true;
        }

#if PARUSC_HAS_AOT_BACKEND
        /// @brief 드라이버 실행 경로를 기준으로 PARUS_LLD 환경 변수를 자동 설정한다.
        std::string getenv_string_(const char* key) {
            if (key == nullptr) return {};
            const char* p = std::getenv(key);
            if (p == nullptr) return {};
            return std::string(p);
        }

        std::optional<uint64_t> parse_u64_(const std::string& s) {
            if (s.empty()) return std::nullopt;
            try {
                size_t idx = 0;
                const uint64_t v = std::stoull(s, &idx, 0);
                if (idx != s.size()) return std::nullopt;
                return v;
            } catch (...) {
                return std::nullopt;
            }
        }

        void seed_parus_toolchain_env_from_driver_(const Invocation& inv) {
            namespace fs = std::filesystem;
            if (inv.driver_executable_path.empty()) {
                return;
            }
            std::error_code ec{};

            fs::path driver_path(inv.driver_executable_path);
            fs::path resolved_driver = fs::weakly_canonical(driver_path, ec);
            if (ec || resolved_driver.empty()) {
                ec.clear();
                resolved_driver = driver_path;
            }
            if (resolved_driver.empty()) return;

            const fs::path driver_bin_dir = resolved_driver.parent_path();
            fs::path toolchain_root = driver_bin_dir;
            if (driver_bin_dir.filename() == "bin") {
                toolchain_root = driver_bin_dir.parent_path();
            }

            if (std::getenv("PARUS_TOOLCHAIN_ROOT") == nullptr) {
#if defined(_WIN32)
                _putenv_s("PARUS_TOOLCHAIN_ROOT", toolchain_root.string().c_str());
#else
                setenv("PARUS_TOOLCHAIN_ROOT", toolchain_root.string().c_str(), 0);
#endif
            }

            if (std::getenv("PARUS_LLD") == nullptr) {
                const fs::path candidate_lld = toolchain_root / "bin" / "parus-lld";
                if (fs::exists(candidate_lld, ec) && !ec) {
#if defined(_WIN32)
                    _putenv_s("PARUS_LLD", candidate_lld.string().c_str());
#else
                    setenv("PARUS_LLD", candidate_lld.string().c_str(), 0);
#endif
                }
            }

            if (std::getenv("PARUS_SYSROOT") == nullptr) {
                const fs::path candidate_sysroot = toolchain_root / "sysroot";
                if (fs::exists(candidate_sysroot, ec) && !ec) {
#if defined(_WIN32)
                    _putenv_s("PARUS_SYSROOT", candidate_sysroot.string().c_str());
#else
                    setenv("PARUS_SYSROOT", candidate_sysroot.string().c_str(), 0);
#endif
                }
            }
        }

        std::optional<uint64_t> expected_hash_from_env_(const char* key) {
            const auto s = getenv_string_(key);
            return parse_u64_(s);
        }

        std::string select_sysroot_(const cli::Options& opt) {
            if (!opt.sysroot_path.empty()) return opt.sysroot_path;
            return getenv_string_("PARUS_SYSROOT");
        }

        std::string resolve_active_toolchain_sysroot_() {
            namespace fs = std::filesystem;
            std::error_code ec{};

            const std::string env_toolchain = getenv_string_("PARUS_TOOLCHAIN_ROOT");
            if (!env_toolchain.empty()) {
                const fs::path candidate = fs::path(env_toolchain) / "sysroot";
                const fs::path canonical = fs::weakly_canonical(candidate, ec);
                if (!ec && !canonical.empty()) return canonical.string();
                ec.clear();
                if (fs::exists(candidate, ec) && !ec) return candidate.lexically_normal().string();
            }

            const std::string home = getenv_string_("HOME");
            if (home.empty()) return {};

            const fs::path active = fs::path(home) / ".local" / "share" / "parus" / "active-toolchain";
            const fs::path candidate = active / "sysroot";
            const fs::path canonical = fs::weakly_canonical(candidate, ec);
            if (!ec && !canonical.empty()) return canonical.string();
            ec.clear();
            if (fs::exists(candidate, ec) && !ec) return candidate.lexically_normal().string();
            return {};
        }

        std::string read_manifest_default_target_triple_(const std::string& sysroot) {
            namespace fs = std::filesystem;
            if (sysroot.empty()) return {};
            const fs::path manifest = fs::path(sysroot) / "manifest.json";
            std::string text{};
            std::string io_err{};
            if (!parus::open_file(manifest.string(), text, io_err)) return {};

            const std::string key = "\"default_target_triple\"";
            const size_t key_pos = text.find(key);
            if (key_pos == std::string::npos) return {};
            const size_t colon = text.find(':', key_pos + key.size());
            if (colon == std::string::npos) return {};
            const size_t quote0 = text.find('"', colon + 1);
            if (quote0 == std::string::npos) return {};
            const size_t quote1 = text.find('"', quote0 + 1);
            if (quote1 == std::string::npos || quote1 <= quote0 + 1) return {};
            return text.substr(quote0 + 1, quote1 - quote0 - 1);
        }

        std::string read_sysroot_apple_sdk_ref_(const std::string& sysroot, const std::string& target) {
            namespace fs = std::filesystem;
            if (sysroot.empty() || target.empty()) return {};

            const fs::path ref = fs::path(sysroot) / "targets" / target / "native" / "apple-sdk.ref";
            std::string text{};
            std::string io_err{};
            if (!parus::open_file(ref.string(), text, io_err)) return {};

            std::string sdk_root{};
            if (!parse_json_string_field_(text, "sdk_root", sdk_root)) return {};
            return sdk_root;
        }

        std::string effective_target_triple_(const cli::Options& opt) {
            if (!opt.target_triple.empty()) return opt.target_triple;
            return read_manifest_default_target_triple_(select_sysroot_(opt));
        }

        std::string resolve_prt_archive_path_(const cli::Options& opt) {
            namespace fs = std::filesystem;
            const std::string sysroot = select_sysroot_(opt);
            const std::string target = effective_target_triple_(opt);
            if (sysroot.empty() || target.empty()) return {};
            const fs::path libdir = fs::path(sysroot) / "targets" / target / "lib";
            const fs::path archive = libdir / (opt.freestanding ? "libprt_freestanding.a" : "libprt_hosted.a");
            std::error_code ec{};
            if (fs::exists(archive, ec) && !ec && fs::is_regular_file(archive, ec)) {
                return archive.string();
            }
            return {};
        }

        std::string resolve_core_ext_archive_path_(const cli::Options& opt) {
            namespace fs = std::filesystem;
            const std::string sysroot = select_sysroot_(opt);
            const std::string target = effective_target_triple_(opt);
            if (sysroot.empty() || target.empty()) return {};
            const fs::path libdir = fs::path(sysroot) / "targets" / target / "lib";
            const fs::path archive = libdir / "libcore_ext.a";
            std::error_code ec{};
            if (fs::exists(archive, ec) && !ec && fs::is_regular_file(archive, ec)) {
                return archive.string();
            }
            return {};
        }

        bool program_uses_actor_runtime_(const parus::tyck::TyckResult& tyck_res) {
            std::unordered_set<parus::ty::TypeId> actor_types(
                tyck_res.actor_type_ids.begin(),
                tyck_res.actor_type_ids.end()
            );
            if (!actor_types.empty()) return true;
            auto has_actor_type = [&](const auto& vec) {
                for (const auto ty : vec) {
                    if (actor_types.find(ty) != actor_types.end()) return true;
                }
                return false;
            };
            return has_actor_type(tyck_res.expr_ctor_owner_type) ||
                   has_actor_type(tyck_res.expr_types);
        }

        bool env_flag_truthy_(std::string_view s) {
            if (s.empty()) return false;
            std::string v(s);
            std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
                return static_cast<char>(std::tolower(c));
            });
            return v == "1" || v == "true" || v == "yes" || v == "on";
        }

        std::string resolve_core_export_index_path_(const cli::Options& opt) {
            namespace fs = std::filesystem;
            std::error_code ec{};

            const std::string sysroot = select_sysroot_(opt);
            if (sysroot.empty()) return {};

            const fs::path idx = fs::path(sysroot) / ".cache" / "exports" / "core.exports.json";
            const fs::path normalized = fs::weakly_canonical(idx, ec);
            if (!ec && fs::exists(normalized, ec) && !ec && fs::is_regular_file(normalized, ec)) {
                return parus::normalize_path(normalized.string());
            }
            ec.clear();
            if (fs::exists(idx, ec) && !ec && fs::is_regular_file(idx, ec)) {
                return parus::normalize_path(idx.string());
            }
            return {};
        }

        std::string resolve_core_macro_prelude_path_(const cli::Options& opt) {
            namespace fs = std::filesystem;
            std::error_code ec{};

            const std::string sysroot = select_sysroot_(opt);
            if (sysroot.empty()) return {};

            const fs::path p = fs::path(sysroot) / "core" / "ext" / "cstr.pr";
            const fs::path normalized = fs::weakly_canonical(p, ec);
            if (!ec && fs::exists(normalized, ec) && !ec && fs::is_regular_file(normalized, ec)) {
                return parus::normalize_path(normalized.string());
            }
            ec.clear();
            if (fs::exists(p, ec) && !ec && fs::is_regular_file(p, ec)) {
                return parus::normalize_path(p.string());
            }
            return {};
        }

        std::string_view clone_sv_into_ast_(parus::ast::AstArena& dst, std::string_view s) {
            if (s.empty()) return {};
            return dst.add_owned_string(std::string(s));
        }

        parus::Token clone_token_into_ast_(parus::ast::AstArena& dst, const parus::Token& src) {
            parus::Token out = src;
            out.lexeme = clone_sv_into_ast_(dst, src.lexeme);
            return out;
        }

        void copy_macro_token_range_into_ast_(
            const parus::ast::AstArena& src,
            parus::ast::AstArena& dst,
            uint32_t in_begin,
            uint32_t in_count,
            uint32_t& out_begin,
            uint32_t& out_count
        ) {
            out_begin = static_cast<uint32_t>(dst.macro_tokens().size());
            out_count = 0;

            const auto& toks = src.macro_tokens();
            const uint64_t begin = in_begin;
            const uint64_t end = begin + in_count;
            if (begin > toks.size() || end > toks.size()) return;
            for (uint32_t i = 0; i < in_count; ++i) {
                const auto& t = toks[in_begin + i];
                (void)dst.add_macro_token(clone_token_into_ast_(dst, t));
                ++out_count;
            }
        }

        void append_top_level_macro_decls_(
            const parus::ast::AstArena& src,
            parus::ast::AstArena& dst
        ) {
            const auto& src_decls = src.macro_decls();
            const auto& src_groups = src.macro_groups();
            const auto& src_arms = src.macro_arms();
            const auto& src_caps = src.macro_captures();

            for (const auto& d : src_decls) {
                if (d.scope_depth != 0) continue; // top-level only
                if (!d.is_export) continue;       // external prelude only exports public macros

                parus::ast::MacroDecl nd{};
                nd.name = clone_sv_into_ast_(dst, d.name);
                nd.scope_depth = 0;
                nd.is_export = true;
                nd.span = d.span;
                nd.group_begin = static_cast<uint32_t>(dst.macro_groups().size());
                nd.group_count = 0;

                const uint64_t g_begin = d.group_begin;
                const uint64_t g_end = g_begin + d.group_count;
                if (g_begin > src_groups.size() || g_end > src_groups.size()) continue;

                for (uint32_t gi = 0; gi < d.group_count; ++gi) {
                    const auto& g = src_groups[d.group_begin + gi];
                    parus::ast::MacroGroup ng{};
                    ng.match_kind = g.match_kind;
                    ng.span = g.span;
                    ng.arm_begin = static_cast<uint32_t>(dst.macro_arms().size());
                    ng.arm_count = 0;

                    const uint64_t a_begin = g.arm_begin;
                    const uint64_t a_end = a_begin + g.arm_count;
                    if (a_begin > src_arms.size() || a_end > src_arms.size()) continue;

                    for (uint32_t ai = 0; ai < g.arm_count; ++ai) {
                        const auto& a = src_arms[g.arm_begin + ai];
                        parus::ast::MacroArm na{};
                        na.out_kind = a.out_kind;
                        na.span = a.span;
                        na.capture_begin = static_cast<uint32_t>(dst.macro_captures().size());
                        na.capture_count = 0;

                        const uint64_t c_begin = a.capture_begin;
                        const uint64_t c_end = c_begin + a.capture_count;
                        if (c_begin <= src_caps.size() && c_end <= src_caps.size()) {
                            for (uint32_t ci = 0; ci < a.capture_count; ++ci) {
                                auto cap = src_caps[a.capture_begin + ci];
                                cap.name = clone_sv_into_ast_(dst, cap.name);
                                (void)dst.add_macro_capture(cap);
                                ++na.capture_count;
                            }
                        }

                        copy_macro_token_range_into_ast_(
                            src, dst, a.pattern_token_begin, a.pattern_token_count,
                            na.pattern_token_begin, na.pattern_token_count
                        );
                        copy_macro_token_range_into_ast_(
                            src, dst, a.template_token_begin, a.template_token_count,
                            na.template_token_begin, na.template_token_count
                        );

                        (void)dst.add_macro_arm(na);
                        ++ng.arm_count;
                    }

                    (void)dst.add_macro_group(ng);
                    ++nd.group_count;
                }

                (void)dst.add_macro_decl(nd);
            }
        }

        bool load_core_macro_prelude_into_ast_(
            const cli::Options& opt,
            parus::ast::AstArena& dst_ast,
            parus::SourceManager& sm,
            parus::diag::Bag& bag,
            std::string& out_err
        ) {
            out_err.clear();
            const std::string prelude_path = resolve_core_macro_prelude_path_(opt);
            if (prelude_path.empty()) {
                return true; // core macro prelude is optional unless user actually uses those macros
            }

            std::string text{};
            std::string io_err{};
            if (!parus::open_file(prelude_path, text, io_err)) {
                out_err = "failed to read core macro prelude '" + prelude_path + "': " + io_err;
                return false;
            }

            const uint32_t fid = sm.add(prelude_path, text);
            auto tokens = lex_with_sm_(sm, fid, &bag);
            if (bag.has_error()) {
                out_err = "failed to lex core macro prelude: " + prelude_path;
                return false;
            }

            parus::ast::AstArena src_ast{};
            parus::ty::TypePool src_types{};
            parus::diag::Bag local_bag{};
            parus::ParserFeatureFlags flags{};
            parus::Parser p(tokens, src_ast, src_types, &local_bag, 128, flags);
            (void)p.parse_program();
            if (local_bag.has_error()) {
                for (const auto& d : local_bag.diags()) bag.add(d);
                out_err = "failed to parse core macro prelude: " + prelude_path;
                return false;
            }

            append_top_level_macro_decls_(src_ast, dst_ast);
            return true;
        }

        bool is_core_impl_marker_stmt_(const parus::ast::AstArena& ast, const parus::ast::Stmt& s) {
            if (s.kind != parus::ast::StmtKind::kCompilerIntrinsicDirective) return false;
            if (s.directive_target_path_count != 0) return false; // tag form only
            if (s.directive_key_path_count != 2) return false;
            const auto& segs = ast.path_segs();
            const uint64_t begin = s.directive_key_path_begin;
            const uint64_t end = begin + s.directive_key_path_count;
            if (begin > segs.size() || end > segs.size()) return false;
            return segs[s.directive_key_path_begin] == "Impl" &&
                   segs[s.directive_key_path_begin + 1] == "Core";
        }

        void collect_core_impl_marker_file_ids_(const parus::ast::AstArena& ast,
                                                parus::ast::StmtId root_sid,
                                                std::unordered_set<uint32_t>& out) {
            if (root_sid == parus::ast::k_invalid_stmt || static_cast<size_t>(root_sid) >= ast.stmts().size()) {
                return;
            }
            const auto& root = ast.stmt(root_sid);
            if (root.kind != parus::ast::StmtKind::kBlock) return;
            const auto& kids = ast.stmt_children();
            const uint64_t begin = root.stmt_begin;
            const uint64_t end = begin + root.stmt_count;
            if (begin > kids.size() || end > kids.size()) return;
            for (uint32_t i = 0; i < root.stmt_count; ++i) {
                const auto sid = kids[root.stmt_begin + i];
                if (sid == parus::ast::k_invalid_stmt || static_cast<size_t>(sid) >= ast.stmts().size()) continue;
                const auto& s = ast.stmt(sid);
                if (!is_core_impl_marker_stmt_(ast, s)) continue;
                out.insert(s.span.file_id);
            }
        }

        struct CHeaderImportSpec {
            std::string header{};
            std::string alias{};
            parus::Span span{};
        };

        void collect_c_header_imports_(const parus::ast::AstArena& ast,
                                       parus::ast::StmtId root_sid,
                                       std::vector<CHeaderImportSpec>& out) {
            if (root_sid == parus::ast::k_invalid_stmt || static_cast<size_t>(root_sid) >= ast.stmts().size()) {
                return;
            }
            const auto& root = ast.stmt(root_sid);
            if (root.kind != parus::ast::StmtKind::kBlock) return;
            const auto& kids = ast.stmt_children();
            const uint64_t begin = root.stmt_begin;
            const uint64_t end = begin + root.stmt_count;
            if (begin > kids.size() || end > kids.size()) return;

            for (uint32_t i = 0; i < root.stmt_count; ++i) {
                const auto sid = kids[root.stmt_begin + i];
                if (sid == parus::ast::k_invalid_stmt || static_cast<size_t>(sid) >= ast.stmts().size()) continue;
                const auto& s = ast.stmt(sid);
                if (s.kind != parus::ast::StmtKind::kUse) continue;
                if (s.use_kind != parus::ast::UseKind::kImportCHeader) continue;
                if (s.use_rhs_ident.empty()) continue;

                CHeaderImportSpec one{};
                if (s.use_path_count > 0) {
                    const auto& segs = ast.path_segs();
                    if (s.use_path_begin + s.use_path_count > segs.size()) continue;
                    one.header = std::string(segs[s.use_path_begin]);
                } else if (!s.use_name.empty()) {
                    one.header = std::string(s.use_name);
                } else {
                    continue;
                }
                one.alias = std::string(s.use_rhs_ident);
                one.span = s.span;
                out.push_back(std::move(one));
            }
        }


        std::string query_macos_sdk_root_from_xcrun_() {
#if defined(__APPLE__) && !defined(_WIN32)
            FILE* pipe = popen("xcrun --sdk macosx --show-sdk-path 2>/dev/null", "r");
            if (pipe == nullptr) return {};

            std::string out{};
            char buf[512];
            while (fgets(buf, static_cast<int>(sizeof(buf)), pipe) != nullptr) {
                out.append(buf);
            }
            (void)pclose(pipe);

            while (!out.empty() &&
                   (out.back() == '\n' || out.back() == '\r' ||
                    std::isspace(static_cast<unsigned char>(out.back())))) {
                out.pop_back();
            }
            return out;
#else
            return {};
#endif
        }

        std::string select_apple_sdk_root_(const cli::Options& opt) {
            if (!opt.apple_sdk_root.empty()) return opt.apple_sdk_root;

            const std::string env_sdk = getenv_string_("PARUS_APPLE_SDK_ROOT");
            if (!env_sdk.empty()) return env_sdk;

            auto resolve_from_sysroot = [&](const std::string& sysroot) -> std::string {
                if (sysroot.empty()) return {};
                const std::string target = !opt.target_triple.empty()
                    ? opt.target_triple
                    : read_manifest_default_target_triple_(sysroot);
                return read_sysroot_apple_sdk_ref_(sysroot, target);
            };

            if (const std::string from_explicit = resolve_from_sysroot(select_sysroot_(opt));
                !from_explicit.empty()) {
                return from_explicit;
            }
            if (const std::string from_active = resolve_from_sysroot(resolve_active_toolchain_sysroot_());
                !from_active.empty()) {
                return from_active;
            }

            const std::string sdkroot_env = getenv_string_("SDKROOT");
            if (!sdkroot_env.empty()) return sdkroot_env;

            return query_macos_sdk_root_from_xcrun_();
        }

        /// @brief 백엔드 메시지를 표준 에러로 출력하고 실패 여부를 반환한다.
        bool print_backend_messages_(const parus::backend::CompileResult& r) {
            bool has_error = false;
            for (const auto& m : r.messages) {
                if (m.is_error) {
                    has_error = true;
                    std::cerr << "error: " << m.text << "\n";
                } else {
                    std::cout << m.text << "\n";
                }
            }
            return has_error;
        }

        /// @brief CLI 링커 모드를 backend 링크 API 모드로 매핑한다.
        parus::backend::link::LinkerMode to_backend_linker_mode_(cli::LinkerMode mode) {
            using In = cli::LinkerMode;
            using Out = parus::backend::link::LinkerMode;
            switch (mode) {
                case In::kParusLld: return Out::kParusLld;
                case In::kAuto:
                default:
                    return Out::kAuto;
            }
        }

        /// @brief backend 링크 메시지를 표준 입출력으로 출력하고 오류 여부를 반환한다.
        bool print_link_messages_(const parus::backend::link::LinkResult& r) {
            bool has_error = false;
            for (const auto& m : r.messages) {
                if (m.is_error) {
                    if (r.ok) {
                        std::cerr << "note: " << m.text << "\n";
                    } else {
                        has_error = true;
                        std::cerr << "error: " << m.text << "\n";
                    }
                } else {
                    std::cout << m.text << "\n";
                }
            }
            return has_error;
        }
#endif

    } // namespace

    int run(const Invocation& inv) {
        if (inv.options == nullptr) {
            std::cerr << "error: internal compiler invocation has null options.\n";
            return 1;
        }
        const auto& opt = *inv.options;
        const std::string current_norm = parus::normalize_path(inv.normalized_input_path);

        parus::SourceManager sm;
        const uint32_t file_id = sm.add(inv.normalized_input_path, inv.source_text);
        const parus::Span root_span{file_id, 0, 0};

        parus::diag::Bag bag;
        auto tokens = lex_with_sm_(sm, file_id, &bag);

        if (opt.has_xparus && opt.internal.token_dump) {
            parusc::dump::dump_tokens(tokens);
        }

        parus::ast::AstArena ast;
        parus::ty::TypePool types;
        parus::ParserFeatureFlags parser_flags{};
        parus::Parser parser(tokens, ast, types, &bag, opt.max_errors, parser_flags);
        auto root = parser.parse_program();
        std::unordered_set<uint32_t> core_impl_marker_file_ids{};
        collect_core_impl_marker_file_ids_(ast, root, core_impl_marker_file_ids);

        if (opt.has_xparus && opt.internal.ast_dump) {
            parusc::dump::dump_stmt(ast, types, root, 0);
            std::cout << "\nTYPES:\n";
            types.dump(std::cout);
        }

        // 파싱/렉싱 단계에서 오류가 있으면 이후 단계 진단 폭주를 막기 위해
        // name-resolve/tyck/cap으로 진행하지 않는다.
        if (bag.has_error()) {
            const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
            return (diag_rc != 0) ? 1 : 0;
        }

        std::vector<CHeaderImportSpec> c_header_imports{};
        collect_c_header_imports_(ast, root, c_header_imports);

        const bool env_no_core = env_flag_truthy_(getenv_string_("PARUS_NO_CORE"));
        const bool disable_auto_core = opt.no_core || env_no_core;
        const bool auto_core_injection = !disable_auto_core && (opt.bundle.bundle_name != "core");
        std::string auto_core_export_index_path{};
        if (auto_core_injection) {
            auto_core_export_index_path = resolve_core_export_index_path_(opt);
        }
        if (!c_header_imports.empty() && disable_auto_core) {
            parus::diag::Diagnostic d(
                parus::diag::Severity::kError,
                parus::diag::Code::kTypeErrorGeneric,
                root_span
            );
            d.add_arg("c-import requires implicit core injection; disable '-fno-core'/PARUS_NO_CORE or avoid C header import");
            bag.add(std::move(d));
            const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
            return (diag_rc != 0) ? 1 : 0;
        }

        if (auto_core_injection) {
            std::string macro_prelude_err{};
            if (!load_core_macro_prelude_into_ast_(opt, ast, sm, bag, macro_prelude_err)) {
                parus::diag::Diagnostic d(
                    parus::diag::Severity::kError,
                    parus::diag::Code::kTypeErrorGeneric,
                    root_span
                );
                d.add_arg(macro_prelude_err.empty()
                    ? std::string("failed to load core macro prelude")
                    : macro_prelude_err);
                bag.add(std::move(d));
                const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
                return (diag_rc != 0) ? 1 : 0;
            }
        }

        const bool macro_ok = parus::macro::expand_program(ast, types, root, bag, opt.macro_budget);
        if (bag.has_error() || !macro_ok) {
            const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
            return (diag_rc != 0 || !macro_ok) ? 1 : 0;
        }

        const std::string current_dir = parent_dir_norm_(current_norm);
        std::vector<ExportSurfaceEntry> cimport_surface{};
        std::vector<std::string> cimport_include_dirs_norm{};
        std::vector<std::string> cimport_isystem_dirs_norm{};
        std::vector<std::string> cimport_defines{};
        std::vector<std::string> cimport_undefines{};
        std::vector<std::string> cimport_forced_includes_norm{};
        std::vector<std::string> cimport_imacros_norm{};

        if (!c_header_imports.empty()) {
            const bool cimport_report_enabled =
                (std::getenv("PARUS_CIMPORT_REPORT") != nullptr);
            if (!current_dir.empty()) cimport_include_dirs_norm.push_back(parus::normalize_path(current_dir));
            for (const auto& d : opt.cimport_include_dirs) {
                if (!d.empty()) cimport_include_dirs_norm.push_back(parus::normalize_path(d));
            }
            parus::cimport::append_unique_normalized_paths(cimport_include_dirs_norm, {});
            for (const auto& d : opt.cimport_isystem_dirs) {
                if (!d.empty()) cimport_isystem_dirs_norm.push_back(parus::normalize_path(d));
            }
            parus::cimport::append_unique_normalized_paths(cimport_isystem_dirs_norm, {});
            for (const auto& d : opt.cimport_defines) {
                if (!d.empty()) cimport_defines.push_back(d);
            }
            for (const auto& d : opt.cimport_undefines) {
                if (!d.empty()) cimport_undefines.push_back(d);
            }
            for (const auto& d : opt.cimport_forced_includes) {
                if (!d.empty()) cimport_forced_includes_norm.push_back(parus::normalize_path(d));
            }
            parus::cimport::append_unique_normalized_paths(cimport_forced_includes_norm, {});
            for (const auto& d : opt.cimport_imacros) {
                if (!d.empty()) cimport_imacros_norm.push_back(parus::normalize_path(d));
            }
            parus::cimport::append_unique_normalized_paths(cimport_imacros_norm, {});

            std::unordered_set<std::string> cimport_seen{};
            for (const auto& spec : c_header_imports) {
                const std::string cimport_cache_root =
                    ((!inv.bundle_root.empty()
                        ? parus::normalize_path(inv.bundle_root)
                        : current_dir)
                     + "/.parus-cache/cimport");
                const auto cached = parus::cimport::load_or_translate_c_header_cached({
                    .importer_source_path = current_norm,
                    .header_path = spec.header,
                    .cache_root = cimport_cache_root,
                    .target_triple = effective_target_triple_(opt),
                    .sysroot_path = select_sysroot_(opt),
                    .apple_sdk_root = select_apple_sdk_root_(opt),
                    .include_dirs = cimport_include_dirs_norm,
                    .isystem_dirs = cimport_isystem_dirs_norm,
                    .defines = cimport_defines,
                    .undefines = cimport_undefines,
                    .forced_includes = cimport_forced_includes_norm,
                    .imacros = cimport_imacros_norm,
                });
                const auto& imported = cached.import;
                if (imported.error != parus::cimport::ImportErrorKind::kNone) {
                    if (imported.error == parus::cimport::ImportErrorKind::kLibClangUnavailable) {
                        parus::diag::Diagnostic d(parus::diag::Severity::kError, parus::diag::Code::kCImportLibClangUnavailable, spec.span);
                        bag.add(std::move(d));
                        if (!imported.error_text.empty()) {
                            parus::diag::Diagnostic d2(
                                parus::diag::Severity::kError,
                                parus::diag::Code::kTypeErrorGeneric,
                                spec.span
                            );
                            d2.add_arg("c-import libclang detail: " + imported.error_text);
                            bag.add(std::move(d2));
                        }
                    } else {
                        parus::diag::Diagnostic d(parus::diag::Severity::kError, parus::diag::Code::kTypeErrorGeneric, spec.span);
                        std::string msg = "failed to import C header '" + spec.header + "'";
                        if (!imported.error_text.empty()) {
                            msg += ": " + imported.error_text;
                        }
                        d.add_arg(msg);
                        bag.add(std::move(d));
                    }
                    continue;
                }

                if (cimport_report_enabled) {
                    const auto& cov = imported.coverage;
                    std::cerr
                        << "[cimport-report] header=" << spec.header
                        << " fn=" << cov.imported_function_decls << "/" << cov.total_function_decls
                        << " global=" << cov.imported_global_decls << "/" << cov.total_global_decls
                        << " type=" << cov.imported_type_decls << "/" << cov.total_type_decls
                        << " const=" << cov.imported_const_decls << "/" << cov.total_const_decls
                        << " fn_macro=" << cov.promoted_function_macros << "/" << cov.total_function_macros
                        << " dropped_decl=" << cov.dropped_decl_reasons.size()
                        << " skipped_fn_macro=" << cov.skipped_function_macros
                        << "\n";
                    constexpr size_t kMaxReasons = 8;
                    const size_t reason_n = std::min(cov.skipped_reasons.size(), kMaxReasons);
                    for (size_t i = 0; i < reason_n; ++i) {
                        std::cerr << "  - " << cov.skipped_reasons[i] << "\n";
                    }
                    const size_t code_n = std::min(cov.skipped_reason_codes.size(), kMaxReasons);
                    for (size_t i = 0; i < code_n; ++i) {
                        std::cerr << "  - code: " << cov.skipped_reason_codes[i] << "\n";
                    }
                    const size_t dropped_reason_n = std::min(cov.dropped_decl_reasons.size(), kMaxReasons);
                    for (size_t i = 0; i < dropped_reason_n; ++i) {
                        std::cerr << "  - dropped: " << cov.dropped_decl_reasons[i] << "\n";
                    }
                    const size_t dropped_code_n = std::min(cov.dropped_decl_reason_codes.size(), kMaxReasons);
                    for (size_t i = 0; i < dropped_code_n; ++i) {
                        std::cerr << "  - dropped-code: " << cov.dropped_decl_reason_codes[i] << "\n";
                    }
                    if (cov.skipped_reasons.size() > kMaxReasons) {
                        std::cerr << "  - ... (" << (cov.skipped_reasons.size() - kMaxReasons)
                                  << " more skipped reasons)\n";
                    }
                    if (cov.dropped_decl_reasons.size() > kMaxReasons) {
                        std::cerr << "  - ... (" << (cov.dropped_decl_reasons.size() - kMaxReasons)
                                  << " more dropped decl reasons)\n";
                    }
                }
                if (imported.functions.empty() &&
                    imported.globals.empty() &&
                    imported.unions.empty() &&
                    imported.typedefs.empty() &&
                    imported.structs.empty() &&
                    imported.enums.empty() &&
                    imported.macros.empty()) {
                    parus::diag::Diagnostic d(parus::diag::Severity::kError, parus::diag::Code::kTypeErrorGeneric, spec.span);
                    d.add_arg("no supported C declarations were imported from header '" + spec.header + "'");
                    bag.add(std::move(d));
                    continue;
                }

                std::unordered_map<std::string, const parus::cimport::ImportedFunctionDecl*> imported_fn_by_name{};
                imported_fn_by_name.reserve(imported.functions.size() * 2u + 1u);

                std::unordered_set<std::string> known_type_names{};
                known_type_names.reserve(
                    imported.unions.size() +
                    imported.structs.size() +
                    imported.enums.size() +
                    imported.typedefs.size()
                );
                for (const auto& un : imported.unions) {
                    if (!un.name.empty()) known_type_names.insert(un.name);
                }
                for (const auto& st : imported.structs) {
                    if (!st.name.empty()) known_type_names.insert(st.name);
                }
                for (const auto& en : imported.enums) {
                    if (!en.name.empty()) known_type_names.insert(en.name);
                }
                for (const auto& td : imported.typedefs) {
                    if (!td.name.empty()) known_type_names.insert(td.name);
                }

                for (const auto& fn : imported.functions) {
                    imported_fn_by_name.emplace(fn.name, &fn);
                    if (fn.name.empty() || fn.type_repr.empty()) continue;
                    if (fn.link_name.empty()) continue;
                    const std::string path = spec.alias + "::" + fn.name;
                    if (!cimport_seen.insert("fn|" + path).second) continue;

                    ExportSurfaceEntry e{};
                    e.kind = parus::sema::SymbolKind::kFn;
                    e.kind_text = "fn";
                    e.path = path;
                    e.link_name = fn.link_name;
                    e.module_head.clear();
                    e.decl_dir = current_dir;
                    e.type_repr = parus::cimport::rewrite_cimport_type_with_alias(
                        fn.type_repr, spec.alias, known_type_names);
                    e.type_semantic = parus::cimport::rewrite_cimport_type_semantic_with_alias(
                        fn.type_semantic, spec.alias, known_type_names);
                    e.inst_payload = parus::cimport::make_c_import_payload(spec.header, spec.alias, fn);
                    e.decl_file = fn.decl_file.empty() ? current_norm : parus::normalize_path(fn.decl_file);
                    e.decl_line = fn.decl_line;
                    e.decl_col = fn.decl_col;
                    e.decl_bundle = "__cimport__";
                    e.is_export = true;
                    cimport_surface.push_back(std::move(e));
                }

                for (const auto& gv : imported.globals) {
                    if (gv.name.empty() || gv.type_repr.empty()) continue;
                    const std::string path = spec.alias + "::" + gv.name;
                    if (!cimport_seen.insert("var|" + path).second) continue;

                    ExportSurfaceEntry e{};
                    e.kind = parus::sema::SymbolKind::kVar;
                    e.kind_text = "var";
                    e.path = path;
                    e.link_name = gv.link_name.empty() ? gv.name : gv.link_name;
                    e.module_head.clear();
                    e.decl_dir = current_dir;
                    e.type_repr = parus::cimport::rewrite_cimport_type_with_alias(
                        gv.type_repr, spec.alias, known_type_names);
                    e.type_semantic = parus::cimport::rewrite_cimport_type_semantic_with_alias(
                        gv.type_semantic, spec.alias, known_type_names);
                    e.inst_payload = parus::cimport::make_c_import_global_payload(spec.header, gv);
                    e.decl_file = gv.decl_file.empty() ? current_norm : parus::normalize_path(gv.decl_file);
                    e.decl_line = gv.decl_line;
                    e.decl_col = gv.decl_col;
                    e.decl_bundle = "__cimport__";
                    e.is_export = true;
                    cimport_surface.push_back(std::move(e));
                }

                for (const auto& un : imported.unions) {
                    if (un.name.empty()) continue;
                    const std::string path = spec.alias + "::" + un.name;
                    if (!cimport_seen.insert("type|" + path).second) continue;

                    ExportSurfaceEntry e{};
                    e.kind = parus::sema::SymbolKind::kType;
                    e.kind_text = "type";
                    e.path = path;
                    e.link_name.clear();
                    e.module_head.clear();
                    e.decl_dir = current_dir;
                    e.type_repr = path;
                    e.inst_payload = parus::cimport::make_c_import_union_payload(
                        spec.header, spec.alias, known_type_names, un);
                    e.decl_file = un.decl_file.empty() ? current_norm : parus::normalize_path(un.decl_file);
                    e.decl_line = un.decl_line;
                    e.decl_col = un.decl_col;
                    e.decl_bundle = "__cimport__";
                    e.is_export = true;
                    cimport_surface.push_back(std::move(e));
                }

                for (const auto& st_src : imported.structs) {
                    if (st_src.name.empty()) continue;
                    const auto& st = st_src;
                    const std::string type_path = spec.alias + "::" + st.name;

                    if (!cimport_seen.insert("type|" + type_path).second) continue;

                    ExportSurfaceEntry e{};
                    e.kind = parus::sema::SymbolKind::kType;
                    e.kind_text = "type";
                    e.path = type_path;
                    e.link_name.clear();
                    e.module_head.clear();
                    e.decl_dir = current_dir;
                    e.type_repr = type_path;
                    e.inst_payload = parus::cimport::make_c_import_struct_payload(
                        spec.header, spec.alias, known_type_names, st);
                    e.decl_file = st_src.decl_file.empty() ? current_norm : parus::normalize_path(st_src.decl_file);
                    e.decl_line = st_src.decl_line;
                    e.decl_col = st_src.decl_col;
                    e.decl_bundle = "__cimport__";
                    e.is_export = true;
                    cimport_surface.push_back(std::move(e));
                }

                for (const auto& td : imported.typedefs) {
                    if (td.name.empty() || td.type_repr.empty()) continue;
                    const std::string path = spec.alias + "::" + td.name;
                    if (!cimport_seen.insert("type|" + path).second) continue;

                    ExportSurfaceEntry e{};
                    e.kind = parus::sema::SymbolKind::kType;
                    e.kind_text = "type";
                    e.path = path;
                    e.link_name.clear();
                    e.module_head.clear();
                    e.decl_dir = current_dir;
                    e.type_repr = parus::cimport::rewrite_cimport_type_with_alias(
                        td.type_repr, spec.alias, known_type_names);
                    e.type_semantic = parus::cimport::rewrite_cimport_type_semantic_with_alias(
                        td.type_semantic, spec.alias, known_type_names);
                    e.inst_payload = parus::cimport::make_c_import_typedef_payload(
                        spec.header, spec.alias, known_type_names, td);
                    e.decl_file = td.decl_file.empty() ? current_norm : parus::normalize_path(td.decl_file);
                    e.decl_line = td.decl_line;
                    e.decl_col = td.decl_col;
                    e.decl_bundle = "__cimport__";
                    e.is_export = true;
                    cimport_surface.push_back(std::move(e));
                }

                for (const auto& en : imported.enums) {
                    if (en.name.empty()) continue;
                    const std::string enum_path = spec.alias + "::" + en.name;
                    if (cimport_seen.insert("type|" + enum_path).second) {
                        ExportSurfaceEntry e{};
                        e.kind = parus::sema::SymbolKind::kType;
                        e.kind_text = "type";
                        e.path = enum_path;
                        e.link_name.clear();
                        e.module_head.clear();
                        e.decl_dir = current_dir;
                        e.type_repr = enum_path;
                        e.inst_payload.clear();
                        e.decl_file = en.decl_file.empty() ? current_norm : parus::normalize_path(en.decl_file);
                        e.decl_line = en.decl_line;
                        e.decl_col = en.decl_col;
                        e.decl_bundle = "__cimport__";
                        e.is_export = true;
                        cimport_surface.push_back(std::move(e));
                    }

                    const std::string const_ty =
                        parus::cimport::rewrite_cimport_type_with_alias(
                            en.underlying_type_repr.empty() ? std::string_view("i32") : std::string_view(en.underlying_type_repr),
                            spec.alias,
                            known_type_names
                        );

                    for (const auto& cst : en.constants) {
                        if (cst.name.empty() || cst.value_text.empty()) continue;
                        const std::string const_path = spec.alias + "::" + cst.name;
                        if (!cimport_seen.insert("var|" + const_path).second) continue;

                        ExportSurfaceEntry ce{};
                        ce.kind = parus::sema::SymbolKind::kVar;
                        ce.kind_text = "var";
                        ce.path = const_path;
                        ce.link_name.clear();
                        ce.module_head.clear();
                        ce.decl_dir = current_dir;
                        ce.type_repr = const_ty;
                        ce.inst_payload = parus::cimport::make_c_import_const_payload("int", cst.value_text);
                        ce.decl_file = cst.decl_file.empty() ? current_norm : parus::normalize_path(cst.decl_file);
                        ce.decl_line = cst.decl_line;
                        ce.decl_col = cst.decl_col;
                        ce.decl_bundle = "__cimport__";
                        ce.is_export = true;
                        cimport_surface.push_back(std::move(ce));
                    }
                }

                for (const auto& mc : imported.macros) {
                    if (mc.name.empty() || mc.is_function_like) continue;
                    if (mc.const_kind == parus::cimport::ImportedConstKind::kNone) continue;

                    const std::string const_path = spec.alias + "::" + mc.name;
                    if (!cimport_seen.insert("var|" + const_path).second) continue;

                    ExportSurfaceEntry ce{};
                    ce.kind = parus::sema::SymbolKind::kVar;
                    ce.kind_text = "var";
                    ce.path = const_path;
                    ce.link_name.clear();
                    ce.module_head.clear();
                    ce.decl_dir = current_dir;
                    ce.decl_file = mc.decl_file.empty() ? current_norm : parus::normalize_path(mc.decl_file);
                    ce.decl_line = mc.decl_line;
                    ce.decl_col = mc.decl_col;
                    ce.decl_bundle = "__cimport__";
                    ce.is_export = true;

                    switch (mc.const_kind) {
                        case parus::cimport::ImportedConstKind::kInt:
                            ce.type_repr = "i64";
                            ce.inst_payload = parus::cimport::make_c_import_const_payload("int", mc.value_text);
                            break;
                        case parus::cimport::ImportedConstKind::kFloat:
                            ce.type_repr = "f64";
                            ce.inst_payload = parus::cimport::make_c_import_const_payload("float", mc.value_text);
                            break;
                        case parus::cimport::ImportedConstKind::kBool:
                            ce.type_repr = "bool";
                            ce.inst_payload = parus::cimport::make_c_import_const_payload("bool", mc.value_text);
                            break;
                        case parus::cimport::ImportedConstKind::kChar:
                            ce.type_repr = "char";
                            ce.inst_payload = parus::cimport::make_c_import_const_payload("char", mc.value_text);
                            break;
                        case parus::cimport::ImportedConstKind::kString:
                            ce.type_repr = "*const i8";
                            ce.inst_payload = parus::cimport::make_c_import_const_payload("string", mc.value_text);
                            break;
                        case parus::cimport::ImportedConstKind::kNone:
                        default:
                            continue;
                    }

                    cimport_surface.push_back(std::move(ce));
                }

                std::vector<std::string> macro_skip_reasons{};
                for (const auto& mc : imported.macros) {
                    if (mc.name.empty() || !mc.is_function_like) continue;

                    const std::string macro_path = spec.alias + "::" + mc.name;
                    if (mc.promote_kind == parus::cimport::ImportedMacroPromoteKind::kDirectAlias) {
                        const auto it_fn = imported_fn_by_name.find(mc.promote_callee_name);
                        if (it_fn == imported_fn_by_name.end() || it_fn->second == nullptr) {
                            macro_skip_reasons.push_back(mc.name + ": unresolved direct alias target");
                            continue;
                        }
                        if (!cimport_seen.insert("fn|" + macro_path).second) {
                            macro_skip_reasons.push_back(mc.name + ": symbol collision on alias path");
                            continue;
                        }
                        const auto* callee = it_fn->second;
                        ExportSurfaceEntry e{};
                        e.kind = parus::sema::SymbolKind::kFn;
                        e.kind_text = "fn";
                        e.path = macro_path;
                        e.link_name = callee->link_name;
                        e.module_head.clear();
                        e.decl_dir = current_dir;
                        e.type_repr = parus::cimport::rewrite_cimport_type_with_alias(
                            callee->type_repr, spec.alias, known_type_names);
                        e.type_semantic = parus::cimport::rewrite_cimport_type_semantic_with_alias(
                            callee->type_semantic, spec.alias, known_type_names);
                        e.inst_payload = parus::cimport::make_c_import_payload(spec.header, spec.alias, *callee);
                        e.decl_file = mc.decl_file.empty() ? current_norm : parus::normalize_path(mc.decl_file);
                        e.decl_line = mc.decl_line;
                        e.decl_col = mc.decl_col;
                        e.decl_bundle = "__cimport__";
                        e.is_export = true;
                        cimport_surface.push_back(std::move(e));
                        continue;
                    }

                    if (mc.promote_kind == parus::cimport::ImportedMacroPromoteKind::kIRWrapperCall) {
                        if (mc.promote_type_repr.empty() ||
                            mc.promote_call_args.empty() ||
                            mc.promote_callee_link_name.empty()) {
                            macro_skip_reasons.push_back(mc.name + ": incomplete wrapper metadata");
                            continue;
                        }
                        if (!cimport_seen.insert("fn|" + macro_path).second) {
                            macro_skip_reasons.push_back(mc.name + ": symbol collision on alias path");
                            continue;
                        }
                        const std::string wrapper_symbol = parus::cimport::make_c_import_wrapper_symbol(
                            spec.header, spec.alias, mc.name);

                        ExportSurfaceEntry e{};
                        e.kind = parus::sema::SymbolKind::kFn;
                        e.kind_text = "fn";
                        e.path = macro_path;
                        e.link_name = wrapper_symbol;
                        e.module_head.clear();
                        e.decl_dir = current_dir;
                        e.type_repr = parus::cimport::rewrite_cimport_type_with_alias(
                            mc.promote_type_repr, spec.alias, known_type_names);
                        e.type_semantic = parus::cimport::rewrite_cimport_type_semantic_with_alias(
                            mc.promote_type_semantic, spec.alias, known_type_names);
                        e.inst_payload = parus::cimport::make_c_import_wrapper_payload(spec.header, spec.alias, mc);
                        e.decl_file = mc.decl_file.empty() ? current_norm : parus::normalize_path(mc.decl_file);
                        e.decl_line = mc.decl_line;
                        e.decl_col = mc.decl_col;
                        e.decl_bundle = "__cimport__";
                        e.is_export = true;
                        cimport_surface.push_back(std::move(e));
                        continue;
                    }

                    if (!mc.skip_reason.empty()) {
                        macro_skip_reasons.push_back(
                            mc.name + "[" + parus::cimport::imported_macro_skip_code_text(mc.skip_kind) + "]: " + mc.skip_reason);
                    } else {
                        macro_skip_reasons.push_back(mc.name + ": unsupported function-like macro");
                    }
                }

                if (!macro_skip_reasons.empty()) {
                    std::string msg = "some function-like C macros were skipped: ";
                    constexpr size_t kPreview = 3;
                    for (size_t i = 0; i < macro_skip_reasons.size() && i < kPreview; ++i) {
                        if (i) msg += "; ";
                        msg += macro_skip_reasons[i];
                    }
                    if (macro_skip_reasons.size() > kPreview) {
                        msg += "; ...";
                    }
                    parus::diag::Diagnostic d(
                        parus::diag::Severity::kWarning,
                        parus::diag::Code::kCImportFnMacroSkipped,
                        spec.span
                    );
                    d.add_arg(msg);
                    bag.add(std::move(d));
                }
                if (!imported.coverage.dropped_decl_reasons.empty() ||
                    !imported.coverage.dropped_decl_reason_codes.empty()) {
                    std::string msg = "some C declarations were dropped during import: ";
                    constexpr size_t kPreview = 3;
                    size_t printed = 0;
                    for (size_t i = 0;
                         i < imported.coverage.dropped_decl_reasons.size() && printed < kPreview;
                         ++i, ++printed) {
                        if (printed) msg += "; ";
                        msg += imported.coverage.dropped_decl_reasons[i];
                    }
                    if (printed == 0) {
                        for (size_t i = 0;
                             i < imported.coverage.dropped_decl_reason_codes.size() && printed < kPreview;
                             ++i, ++printed) {
                            if (printed) msg += "; ";
                            msg += imported.coverage.dropped_decl_reason_codes[i];
                        }
                    }
                    const size_t dropped_n = std::max(
                        imported.coverage.dropped_decl_reasons.size(),
                        imported.coverage.dropped_decl_reason_codes.size()
                    );
                    if (dropped_n > printed) {
                        msg += "; ...";
                    }
                    parus::diag::Diagnostic d(
                        parus::diag::Severity::kWarning,
                        parus::diag::Code::kTypeErrorGeneric,
                        spec.span
                    );
                    d.add_arg(msg);
                    bag.add(std::move(d));
                }
            }
        }

        std::vector<std::string> external_index_paths{};
        external_index_paths.reserve(
            inv.load_export_index_paths.size() + (auto_core_export_index_path.empty() ? 0u : 1u)
        );
        if (!auto_core_export_index_path.empty()) {
            external_index_paths.push_back(auto_core_export_index_path);
        }
        for (const auto& p : inv.load_export_index_paths) {
            const std::string norm = parus::normalize_path(p);
            if (std::find(external_index_paths.begin(), external_index_paths.end(), norm) == external_index_paths.end()) {
                external_index_paths.push_back(norm);
            }
        }

        std::vector<LoadedExternalIndex> loaded_external_indices{};
        loaded_external_indices.reserve(external_index_paths.size());
        for (const auto& idx_path : external_index_paths) {
            LoadedExternalIndex loaded{};
            std::string load_err{};
            if (!load_external_index_(idx_path, loaded, load_err)) {
                if (load_err.starts_with("missing export-index file")) {
                    parus::diag::Diagnostic d(
                        parus::diag::Severity::kError,
                        parus::diag::Code::kExportIndexMissing,
                        root_span
                    );
                    d.add_arg(load_err);
                    bag.add(std::move(d));
                } else if (load_err.find("export-index") != std::string::npos) {
                    parus::diag::Diagnostic d(
                        parus::diag::Severity::kError,
                        parus::diag::Code::kExportIndexSchema,
                        root_span
                    );
                    d.add_arg(load_err);
                    bag.add(std::move(d));
                } else {
                    add_template_sidecar_diag_(bag, root_span, load_err);
                }
                continue;
            }
            qualify_export_surface_entries_for_bundle_(loaded.entries, loaded.bundle);
            loaded_external_indices.push_back(std::move(loaded));
        }
        if (bag.has_error()) {
            const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
            return (diag_rc != 0) ? 1 : 0;
        }

        auto type_resolve = parus::type::resolve_program_types(ast, types, root, bag);
        if (bag.has_error() || !type_resolve.ok) {
            const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
            return (diag_rc != 0 || !type_resolve.ok) ? 1 : 0;
        }

        std::vector<ExportSurfaceEntry> bundle_surface{};
        if (opt.bundle.enabled || !opt.bundle.emit_export_index_path.empty() || !inv.load_export_index_paths.empty()) {
            std::vector<std::string> sources = inv.bundle_sources;
            if (sources.empty()) {
                sources.push_back(current_norm);
            }
            for (auto& s : sources) s = parus::normalize_path(s);
            if (std::find(sources.begin(), sources.end(), current_norm) == sources.end()) {
                sources.push_back(current_norm);
            }

            std::string collect_err{};
            if (!collect_bundle_export_surface_(sources, inv.bundle_root, opt.bundle.bundle_name, bundle_surface, collect_err)) {
                parus::diag::Diagnostic d(parus::diag::Severity::kError, parus::diag::Code::kExportIndexSchema, root_span);
                d.add_arg(collect_err);
                bag.add(std::move(d));
            }
            if (!bag.has_error()) {
                validate_same_folder_export_collisions_(bundle_surface, bag, root_span);
            }
            if (bag.has_error()) {
                const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
                return (diag_rc != 0) ? 1 : 0;
            }
        }

        auto pass_opt = opt.pass_opt;
        pass_opt.name_resolve.current_file_id = file_id;
        pass_opt.name_resolve.current_bundle_name = opt.bundle.bundle_name;
        pass_opt.name_resolve.current_module_head = !inv.module_head.empty()
            ? inv.module_head
            : compute_module_head_(inv.bundle_root, current_norm, opt.bundle.bundle_name);
        pass_opt.name_resolve.current_source_dir_norm = parent_dir_norm_(current_norm);
        pass_opt.name_resolve.warn_core_path_when_std = auto_core_injection && !opt.no_std;
        pass_opt.name_resolve.allowed_import_heads.clear();
        if (opt.bundle.enabled) {
            for (const auto& head : inv.module_imports) {
                const auto top = normalize_import_head_top_(head);
                if (!top.empty()) pass_opt.name_resolve.allowed_import_heads.insert(top);
            }
            if (auto_core_injection) {
                pass_opt.name_resolve.allowed_import_heads.insert("core");
            }
        }
        if (auto_core_injection) {
            pass_opt.name_resolve.implicit_import_aliases["core"] = "core";
        }

        std::set<std::tuple<std::string, std::string, std::string>> external_seen{};
        auto add_external = [&](const ExportSurfaceEntry& e, bool same_bundle) {
            std::vector<std::string> lookup_paths{};
            lookup_paths.reserve(3);

            std::string full_path = e.path;
            std::string local_path = e.path;
            if (!e.module_head.empty()) {
                const std::string prefix = e.module_head + "::";
                const bool already_prefixed =
                    full_path == e.module_head ||
                    full_path.starts_with(prefix);
                if (!already_prefixed) {
                    full_path = prefix + full_path;
                }

                if (full_path.starts_with(prefix)) {
                    local_path = std::string(full_path.substr(prefix.size()));
                }

                const bool same_module = same_bundle &&
                    (e.module_head == pass_opt.name_resolve.current_module_head ||
                     (e.decl_bundle == "core" &&
                      e.module_head.starts_with("core::") &&
                      pass_opt.name_resolve.current_module_head ==
                          std::string(e.module_head.substr(std::string_view("core::").size()))));

                if (same_module && !local_path.empty()) {
                    lookup_paths.push_back(local_path);
                }
                lookup_paths.push_back(full_path);

                if (e.decl_bundle == "core" &&
                    e.module_head.starts_with("core::") &&
                    !local_path.empty()) {
                    const std::string short_head =
                        std::string(e.module_head.substr(std::string_view("core::").size()));
                    if (!short_head.empty()) {
                        lookup_paths.push_back(short_head + "::" + local_path);
                    }
                }
            } else if (!full_path.empty()) {
                lookup_paths.push_back(full_path);
            }

            std::sort(lookup_paths.begin(), lookup_paths.end());
            lookup_paths.erase(std::unique(lookup_paths.begin(), lookup_paths.end()), lookup_paths.end());

            for (const auto& lookup_path : lookup_paths) {
                const std::string overload_key =
                    (e.kind == parus::sema::SymbolKind::kFn) ? e.type_repr : std::string{};
                const auto key = std::make_tuple(e.kind_text, lookup_path, overload_key);
                if (!external_seen.insert(key).second) continue;

                parus::passes::NameResolveOptions::ExternalExport x{};
                x.kind = e.kind;
                x.path = lookup_path;
                x.link_name = e.link_name;
                x.declared_type = parse_type_repr_into_(e.type_repr, e.type_semantic, e.inst_payload, types);
                x.declared_type_repr = e.type_repr;
                x.declared_type_semantic = e.type_semantic;
                x.decl_span = root_span;
                x.decl_bundle_name = e.decl_bundle;
                x.module_head = e.module_head;
                x.decl_source_dir_norm = e.decl_dir;
                x.is_export = e.is_export;
                x.inst_payload = e.inst_payload;
                pass_opt.name_resolve.external_exports.push_back(std::move(x));
            }
        };

        if (opt.bundle.enabled) {
            for (const auto& e : bundle_surface) {
                if (e.decl_file == current_norm) continue;
                add_external(e, /*same_bundle=*/true);
            }
        }

        for (const auto& e : cimport_surface) {
            add_external(e, /*same_bundle=*/false);
        }

        for (const auto& loaded : loaded_external_indices) {
            for (auto e : loaded.entries) {
                if (e.decl_bundle.empty()) e.decl_bundle = loaded.bundle;
                const bool same_bundle = opt.bundle.enabled && loaded.bundle == opt.bundle.bundle_name;
                if (same_bundle && e.decl_file == current_norm) continue;
                add_external(e, same_bundle);
            }
        }

        for (const auto& ex : pass_opt.name_resolve.external_exports) {
            if (ex.module_head.empty()) continue;

            const auto top = normalize_import_head_top_(ex.module_head);
            if (!top.empty()) {
                pass_opt.name_resolve.allowed_import_heads.insert(top);
            }

            if (ex.decl_bundle_name == "core" && ex.module_head.starts_with("core::")) {
                const std::string short_head =
                    std::string(ex.module_head.substr(std::string_view("core::").size()));
                const auto short_top = normalize_import_head_top_(short_head);
                if (!short_top.empty()) {
                    pass_opt.name_resolve.allowed_import_heads.insert(short_top);
                }
            }
        }

        if (bag.has_error()) {
            const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
            return (diag_rc != 0) ? 1 : 0;
        }

        auto pres = parus::passes::run_on_program(ast, root, bag, pass_opt);
        if (bag.has_error()) {
            const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
            return (diag_rc != 0) ? 1 : 0;
        }

        std::vector<parus::tyck::ImportedFnTemplate> imported_fn_templates{};
        std::vector<parus::tyck::ImportedProtoTemplate> imported_proto_templates{};
        std::vector<parus::tyck::ImportedActsTemplate> imported_acts_templates{};
        std::vector<parus::tyck::ImportedClassTemplate> imported_class_templates{};
        std::vector<parus::tyck::ImportedFieldTemplate> imported_field_templates{};
        std::vector<parus::tyck::ImportedEnumTemplate> imported_enum_templates{};
        std::unordered_map<uint32_t, std::string> sidecar_file_bundle_overrides{};
        std::unordered_map<uint32_t, std::string> sidecar_file_module_head_overrides{};
        std::string sidecar_load_err{};
        if (!load_imported_templates_into_ast_(
                loaded_external_indices,
                current_norm,
                sm,
                ast,
                types,
                imported_fn_templates,
                imported_proto_templates,
                imported_acts_templates,
                imported_class_templates,
                imported_field_templates,
                imported_enum_templates,
                sidecar_file_bundle_overrides,
                sidecar_file_module_head_overrides,
                sidecar_load_err)) {
            if (!sidecar_load_err.empty()) {
                add_template_sidecar_diag_(bag, root_span, sidecar_load_err);
            }
            const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
            return (diag_rc != 0) ? 1 : 0;
        }

        parus::tyck::TyckResult tyck_res;
        {
            parus::tyck::TypeChecker tc(ast, types, bag, &type_resolve, &pres.generic_prep);
            tc.set_seed_symbol_table(&pres.sym);
            if (!opt.bundle.bundle_name.empty()) {
                tc.set_current_bundle_name(opt.bundle.bundle_name);
            }
            if (!imported_fn_templates.empty()) {
                tc.set_imported_fn_templates(std::move(imported_fn_templates));
            }
            if (!imported_proto_templates.empty()) {
                tc.set_imported_proto_templates(std::move(imported_proto_templates));
            }
            if (!imported_acts_templates.empty()) {
                tc.set_imported_acts_templates(std::move(imported_acts_templates));
            }
            if (!imported_class_templates.empty()) {
                tc.set_imported_class_templates(std::move(imported_class_templates));
            }
            if (!imported_field_templates.empty()) {
                tc.set_imported_field_templates(std::move(imported_field_templates));
            }
            if (!imported_enum_templates.empty()) {
                tc.set_imported_enum_templates(std::move(imported_enum_templates));
            }
            if (!core_impl_marker_file_ids.empty()) {
                tc.set_core_impl_marker_file_ids(std::move(core_impl_marker_file_ids));
            }
            if (!sidecar_file_bundle_overrides.empty()) {
                tc.set_file_bundle_overrides(std::move(sidecar_file_bundle_overrides));
            }
            if (!sidecar_file_module_head_overrides.empty()) {
                tc.set_file_module_head_overrides(std::move(sidecar_file_module_head_overrides));
            }
            tyck_res = tc.check_program(root);
        }
        if (bag.has_error() || !tyck_res.errors.empty()) {
            const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
            if (!tyck_res.errors.empty()) {
                for (const auto& err : tyck_res.errors) {
                    std::cerr << "error: type-check: " << err.message << "\n";
                }
            }
            return (diag_rc != 0 || !tyck_res.errors.empty()) ? 1 : 0;
        }

        const bool actor_runtime_used = program_uses_actor_runtime_(tyck_res);
        if (opt.no_std && actor_runtime_used) {
            parus::diag::Diagnostic d(
                parus::diag::Severity::kError,
                parus::diag::Code::kActorNotAvailableInNoStd,
                root_span
            );
            bag.add(std::move(d));
            const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
            return (diag_rc != 0) ? 1 : 0;
        }

        const auto ast_cap_res = parus::cap::run_capability_check(
            ast, root, pres.name_resolve, tyck_res, types, bag
        );
        if (bag.has_error() || !ast_cap_res.ok) {
            const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
            return (diag_rc != 0 || !ast_cap_res.ok) ? 1 : 0;
        }

        if (!opt.bundle.emit_export_index_path.empty()) {
            std::vector<ExportSurfaceEntry> typed_exports{};
            collect_typed_current_export_surface_(
                ast,
                root,
                types,
                pres.sym,
                sm,
                file_id,
                current_norm,
                opt.bundle.bundle_name,
                pass_opt.name_resolve.current_module_head,
                current_dir,
                typed_exports
            );
            std::vector<TemplateSidecarFunction> current_sidecars{};
            std::string sidecar_err{};
            if (!collect_typed_current_template_sidecars_(
                    ast,
                    root,
                    types,
                    sm,
                    file_id,
                    current_norm,
                    opt.bundle.bundle_name,
                    pass_opt.name_resolve.current_module_head,
                    typed_exports,
                    current_sidecars,
                    sidecar_err)) {
                add_template_sidecar_diag_(
                    bag,
                    root_span,
                    sidecar_err.empty()
                        ? std::string("failed to collect template sidecar")
                        : sidecar_err
                );
                const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
                return (diag_rc != 0) ? 1 : 0;
            }
            std::vector<ExportSurfaceEntry> merged_exports = typed_exports;
            std::vector<TemplateSidecarFunction> merged_sidecars = current_sidecars;
            std::string write_err{};
            for (const auto& loaded : loaded_external_indices) {
                if (loaded.bundle != opt.bundle.bundle_name) continue;
                merged_exports.insert(merged_exports.end(), loaded.entries.begin(), loaded.entries.end());
                merged_sidecars.insert(
                    merged_sidecars.end(), loaded.sidecars.begin(), loaded.sidecars.end());
            }

            qualify_export_surface_entries_for_bundle_(merged_exports, opt.bundle.bundle_name);
            dedupe_export_surface_(merged_exports);
            if (!dedupe_template_sidecars_(merged_sidecars, &write_err)) {
                add_template_sidecar_diag_(bag, root_span, write_err);
                const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
                return (diag_rc != 0) ? 1 : 0;
            }
            validate_same_folder_export_collisions_(merged_exports, bag, root_span);
            if (bag.has_error()) {
                const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
                return (diag_rc != 0) ? 1 : 0;
            }

            if (!write_export_index_(opt.bundle.emit_export_index_path,
                                     opt.bundle.bundle_name,
                                     merged_exports,
                                     write_err)) {
                parus::diag::Diagnostic d(
                    parus::diag::Severity::kError,
                    parus::diag::Code::kExportIndexSchema,
                    root_span
                );
                d.add_arg(write_err);
                bag.add(std::move(d));
                const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
                return (diag_rc != 0) ? 1 : 0;
            }
            if (!write_template_sidecar_(
                    opt.bundle.emit_export_index_path,
                    opt.bundle.bundle_name,
                    merged_sidecars,
                    write_err)) {
                parus::diag::Diagnostic d(
                    parus::diag::Severity::kError,
                    parus::diag::Code::kExportIndexSchema,
                    root_span
                );
                d.add_arg(write_err);
                bag.add(std::move(d));
                const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
                return (diag_rc != 0) ? 1 : 0;
            }
            return 0;
        }

        if (opt.syntax_only) {
            if (!bag.diags().empty()) {
                (void)flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
            }
            return bag.has_error() ? 1 : 0;
        }

        parus::sir::BuildOptions bopt{};
        bopt.bundle_enabled = opt.bundle.enabled;
        bopt.bundle_name = opt.bundle.bundle_name;
        bopt.current_source_norm = current_norm;
        bopt.bundle_sources_norm = inv.bundle_sources;
        std::sort(bopt.bundle_sources_norm.begin(), bopt.bundle_sources_norm.end());
        bopt.bundle_sources_norm.erase(
            std::unique(bopt.bundle_sources_norm.begin(), bopt.bundle_sources_norm.end()),
            bopt.bundle_sources_norm.end()
        );
        auto sir_mod = parus::sir::build_sir_module(
            ast, root, pres.sym, pres.name_resolve, tyck_res, types, bopt
        );

        (void)parus::sir::canonicalize_for_capability(sir_mod, types);

        const auto sir_verrs = parus::sir::verify_module(sir_mod);
        if (!sir_verrs.empty()) {
            for (const auto& e : sir_verrs) {
                std::cerr << "error: SIR verify: " << e.msg << "\n";
            }
            return 1;
        }

        (void)parus::sir::analyze_mut(sir_mod, types, bag);
        const auto sir_cap = parus::sir::analyze_capabilities(sir_mod, types, bag);
        if (!sir_cap.ok) {
            const bool had_diags = !bag.diags().empty();
            const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
            if (!had_diags) {
                std::cerr << "error: SIR capability analysis failed (" << sir_cap.error_count << ")\n";
            }
            return (diag_rc != 0 || !sir_cap.ok) ? 1 : 0;
        }

        const auto sir_handle_verrs = parus::sir::verify_escape_handles(sir_mod);
        if (!sir_handle_verrs.empty()) {
            for (const auto& e : sir_handle_verrs) {
                std::cerr << "error: SIR escape-handle verify: " << e.msg << "\n";
            }
            return 1;
        }

        if (opt.has_xparus && opt.internal.sir_dump) {
            parusc::dump::dump_sir_module(sir_mod, types);
        }

        std::unordered_set<parus::ty::TypeId> oir_tag_only_enum_type_ids = tyck_res.tag_only_enum_type_ids;
        auto type_name_keys = [&](parus::ty::TypeId tid) {
            std::unordered_set<std::string> keys{};
            if (tid == parus::ty::kInvalidType) return keys;

            std::vector<std::string_view> path{};
            std::vector<parus::ty::TypeId> args{};
            if (!types.decompose_named_user(tid, path, args) || path.empty()) {
                keys.insert(types.to_string(tid));
                return keys;
            }

            std::string full{};
            for (size_t i = 0; i < path.size(); ++i) {
                if (i != 0) full += "::";
                full += std::string(path[i]);
            }
            keys.insert(full);

            if (path.size() >= 2) {
                std::string dropped_first{};
                for (size_t i = 1; i < path.size(); ++i) {
                    if (i != 1) dropped_first += "::";
                    dropped_first += std::string(path[i]);
                }
                keys.insert(std::move(dropped_first));
            }
            return keys;
        };

        auto add_matching_named_types = [&](const std::unordered_set<std::string>& want_keys) {
            for (uint32_t i = 0; i < types.count(); ++i) {
                const auto cur = static_cast<parus::ty::TypeId>(i);
                const auto cur_keys = type_name_keys(cur);
                for (const auto& key : cur_keys) {
                    if (want_keys.find(key) != want_keys.end()) {
                        oir_tag_only_enum_type_ids.insert(cur);
                        break;
                    }
                }
            }
        };

        for (const auto& ext : pass_opt.name_resolve.external_exports) {
            if (!is_tag_only_enum_decl_payload_(ext.inst_payload)) continue;
            std::unordered_set<std::string> want_keys{};
            if (ext.kind == parus::sema::SymbolKind::kType && ext.declared_type != parus::ty::kInvalidType) {
                const auto type_keys = type_name_keys(ext.declared_type);
                want_keys.insert(type_keys.begin(), type_keys.end());
            }

            if (!ext.path.empty()) {
                want_keys.insert(ext.path);
                if (!ext.module_head.empty()) {
                    const std::string module_prefix = ext.module_head + "::";
                    if (!ext.path.starts_with(module_prefix)) {
                        want_keys.insert(module_prefix + ext.path);
                    }
                    if (!ext.decl_bundle_name.empty()) {
                        std::string local = ext.path;
                        if (local.starts_with(module_prefix)) {
                            local.erase(0, module_prefix.size());
                        }
                        want_keys.insert(ext.decl_bundle_name + "::" + ext.module_head + "::" + local);
                    }
                } else if (!ext.decl_bundle_name.empty()) {
                    want_keys.insert(ext.decl_bundle_name + "::" + ext.path);
                }
            }

            add_matching_named_types(want_keys);
        }

        parus::oir::Builder ob(sir_mod, types, &oir_tag_only_enum_type_ids, &pres.sym);
        auto oir_res = ob.build();
        if (!oir_res.gate_passed) {
            for (const auto& e : oir_res.gate_errors) {
                std::cerr << "error: OIR gate: " << e.msg << "\n";
            }
            return 1;
        }

        parus::oir::run_passes(oir_res.mod);

        const auto oir_verrs = parus::oir::verify(oir_res.mod);
        if (!oir_verrs.empty()) {
            for (const auto& e : oir_verrs) {
                std::cerr << "error: OIR verify: " << e.msg << "\n";
            }
            return 1;
        }

        if (opt.has_xparus && opt.internal.oir_dump) {
            parusc::dump::dump_oir_module(oir_res.mod, types);
        }

#if PARUSC_HAS_AOT_BACKEND
        seed_parus_toolchain_env_from_driver_(inv);

        parus::backend::CompileOptions backend_opt{};
        backend_opt.opt_level = opt.opt_level;
        backend_opt.aot_engine = parus::backend::AOTEngine::kLlvm;
        backend_opt.target_triple = effective_target_triple_(opt);

        const bool emit_object = opt.emit_object || (opt.has_xparus && opt.internal.emit_object);
        const bool emit_llvm_ir = (opt.has_xparus && opt.internal.emit_llvm_ir);
        const bool emit_executable = (!emit_object && !emit_llvm_ir);

        std::string object_for_link = opt.output_path;
        std::string final_exe_output = opt.output_path;
        if (emit_executable) {
            object_for_link = opt.output_path + ".tmp.o";
            backend_opt.output_path = object_for_link;
            backend_opt.emit_object = true;
            backend_opt.emit_llvm_ir = false;
        } else if (emit_object) {
            backend_opt.output_path = opt.output_path;
            backend_opt.emit_object = true;
            backend_opt.emit_llvm_ir = false;
        } else {
            backend_opt.output_path = opt.output_path;
            backend_opt.emit_object = false;
            backend_opt.emit_llvm_ir = true;
        }

        parus::backend::aot::AOTBackend backend;
        auto cr = backend.compile(oir_res.mod, types, backend_opt);
        const bool has_backend_error = print_backend_messages_(cr);
        if (!cr.ok || has_backend_error) return 1;

        if (emit_executable) {
            parus::backend::link::LinkOptions link_opt{};
            link_opt.object_paths = {object_for_link};
            link_opt.output_path = final_exe_output;
            link_opt.target_triple = effective_target_triple_(opt);
            link_opt.sysroot_path = select_sysroot_(opt);
            link_opt.apple_sdk_root = select_apple_sdk_root_(opt);
            if (const auto h = expected_hash_from_env_("PARUS_EXPECTED_TOOLCHAIN_HASH"); h.has_value()) {
                link_opt.expected_toolchain_hash = *h;
            }
            if (const auto h = expected_hash_from_env_("PARUS_EXPECTED_TARGET_HASH"); h.has_value()) {
                link_opt.expected_target_hash = *h;
            }
            link_opt.mode = to_backend_linker_mode_(opt.linker_mode);
            link_opt.allow_fallback = opt.allow_link_fallback;

            if (auto_core_injection) {
                const std::string core_ext_archive = resolve_core_ext_archive_path_(opt);
                if (core_ext_archive.empty()) {
                    parus::diag::Diagnostic d(
                        parus::diag::Severity::kError,
                        parus::diag::Code::kTypeErrorGeneric,
                        root_span
                    );
                    d.add_arg(
                        "missing core runtime archive 'libcore_ext.a' in sysroot target lib dir; "
                        "run ./install.sh to build/install core artifacts"
                    );
                    bag.add(std::move(d));
                    const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
                    return (diag_rc != 0) ? 1 : 0;
                }
                link_opt.object_paths.push_back(core_ext_archive);
            }

            if (actor_runtime_used) {
                const std::string prt_archive = resolve_prt_archive_path_(opt);
                if (prt_archive.empty()) {
                    std::cerr << "error: actor runtime archive is missing for target '"
                              << link_opt.target_triple << "' under sysroot '"
                              << link_opt.sysroot_path << "'.\n";
                    return 1;
                }
                link_opt.object_paths.push_back(prt_archive);
            }

            const auto link_res = parus::backend::link::link_executable(link_opt);
            const bool has_link_error = print_link_messages_(link_res);
            if (!link_res.ok || has_link_error) {
                return 1;
            }
            std::error_code ec;
            std::filesystem::remove(object_for_link, ec);
            std::cout << "linked executable to " << final_exe_output;
            if (!link_res.linker_used.empty()) {
                std::cout << " (via " << link_res.linker_used << ")";
            }
            std::cout << "\n";
        }
        return 0;
#else
        (void)oir_res;
        (void)types;
        std::cerr << "error: parusc was built without AOT backend support.\n";
        return 1;
#endif
    }

} // namespace parusc::p0
