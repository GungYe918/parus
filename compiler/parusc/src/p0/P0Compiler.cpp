// compiler/parusc/src/p0/P0Compiler.cpp
#include <parusc/p0/P0Compiler.hpp>

#include <parusc/dump/Dump.hpp>

#include <parus/ast/Nodes.hpp>
#include <parus/cap/CapabilityCheck.hpp>
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
            std::string inst_payload{};
            std::string decl_file{};
            uint32_t decl_line = 1;
            uint32_t decl_col = 1;
            std::string decl_bundle{};
            bool is_export = false;
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
            namespace fs = std::filesystem;
            std::error_code ec{};
            fs::path root(bundle_root);
            if (root.is_relative()) root = fs::absolute(root, ec);
            if (ec) {
                ec.clear();
                root = fs::path(bundle_root);
            }
            fs::path root_norm = fs::weakly_canonical(root, ec);
            if (ec || root_norm.empty()) {
                ec.clear();
                root_norm = root.lexically_normal();
            }

            fs::path src(source_path);
            if (src.is_relative()) src = fs::absolute(src, ec);
            if (ec) {
                ec.clear();
                src = fs::path(source_path);
            }
            fs::path src_norm = fs::weakly_canonical(src, ec);
            if (ec || src_norm.empty()) {
                ec.clear();
                src_norm = src.lexically_normal();
            }

            fs::path rel = src_norm.lexically_relative(root_norm);
            const std::string rel_s = rel.generic_string();
            if (rel.empty() || rel_s.empty() || rel_s == "." || rel_s.starts_with("..")) {
                rel = src_norm.filename();
            }

            fs::path dir = rel.parent_path();
            std::vector<std::string> segs{};
            bool stripped_src = false;
            for (const auto& seg : dir) {
                const std::string s = seg.string();
                if (s.empty() || s == ".") continue;
                if (!stripped_src && s == "src") {
                    stripped_src = true;
                    continue;
                }
                segs.push_back(s);
            }

            if (segs.empty()) {
                return std::string(bundle_name);
            }

            std::string out{};
            for (size_t i = 0; i < segs.size(); ++i) {
                if (i) out += "::";
                out += segs[i];
            }
            return out;
        }

        std::string normalize_import_head_top_(std::string_view import_head) {
            if (import_head.empty()) return {};
            std::string_view s = import_head;
            if (s.starts_with("::")) s.remove_prefix(2);
            if (s.empty()) return {};
            const size_t pos = s.find("::");
            std::string_view top = (pos == std::string_view::npos) ? s : s.substr(0, pos);
            if (top.empty() || top.find(':') != std::string_view::npos) return {};
            return std::string(top);
        }

        std::string normalize_core_public_module_head_(
            std::string_view bundle_name,
            std::string_view module_head
        ) {
            if (bundle_name != "core") {
                return std::string(module_head);
            }
            if (module_head.empty()) {
                return std::string("core");
            }
            if (module_head == "core" || module_head.starts_with("core::")) {
                return std::string(module_head);
            }
            std::string out = "core::";
            out += module_head;
            return out;
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
            std::vector<ExportSurfaceEntry>& out
        ) {
            if (sid == parus::ast::k_invalid_stmt || static_cast<size_t>(sid) >= ast.stmts().size()) return;
            const auto& s = ast.stmt(sid);

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
                                              out);
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
                                          out);
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
                ExportSurfaceEntry e{};
                e.kind = kind;
                e.kind_text = symbol_kind_to_text_(kind);
                e.path = std::move(qname);
                e.link_name = std::move(link_name);
                e.module_head = module_head;
                e.decl_dir = decl_dir;
                if (tid != parus::ty::kInvalidType) {
                    e.type_repr = types.to_export_string(tid);
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
                    (s.type != parus::ty::kInvalidType) ? types.to_string(s.type) : std::string("def(?)");
                const std::string link_name = build_function_link_name_(
                    bundle_name,
                    qname,
                    s.fn_mode,
                    sig_repr,
                    s.link_abi == parus::ast::LinkAbi::kC
                );
                push_export(parus::sema::SymbolKind::kFn, qname, s.type, s.is_export, s.span, link_name);
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
                push_export(parus::sema::SymbolKind::kField, qualify_name_(ns, s.name), s.type, s.is_export, s.span);
                return;
            }

            if ((s.kind == parus::ast::StmtKind::kEnumDecl ||
                 s.kind == parus::ast::StmtKind::kProtoDecl ||
                 s.kind == parus::ast::StmtKind::kClassDecl ||
                 s.kind == parus::ast::StmtKind::kActorDecl) &&
                !s.name.empty()) {
                std::string payload{};
                if (s.kind == parus::ast::StmtKind::kProtoDecl) payload = "parus_decl_kind=proto";
                if (s.kind == parus::ast::StmtKind::kEnumDecl) payload = "parus_decl_kind=enum";
                if (s.kind == parus::ast::StmtKind::kClassDecl) payload = "parus_decl_kind=class";
                if (s.kind == parus::ast::StmtKind::kActorDecl) payload = "parus_decl_kind=actor";
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
                push_export(parus::sema::SymbolKind::kAct, acts_qname, s.acts_target_type, s.is_export, s.span);

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

                        std::string member_qname = acts_qname;
                        member_qname += "::";
                        member_qname += std::string(ms.name);
                        const std::string sig_repr =
                            (ms.type != parus::ty::kInvalidType) ? types.to_string(ms.type) : std::string("def(?)");
                        const std::string link_name = build_function_link_name_(
                            bundle_name,
                            member_qname,
                            ms.fn_mode,
                            sig_repr,
                            ms.link_abi == parus::ast::LinkAbi::kC
                        );
                        push_export(parus::sema::SymbolKind::kFn,
                                    member_qname,
                                    ms.type,
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

            std::sort(out.begin(), out.end(), [](const ExportSurfaceEntry& a, const ExportSurfaceEntry& b) {
                if (a.path != b.path) return a.path < b.path;
                if (a.kind_text != b.kind_text) return a.kind_text < b.kind_text;
                return a.decl_file < b.decl_file;
            });
            return true;
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
            ofs << "  \"version\": 5,\n";
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
            if (!parse_json_uint_field_(text, "version", version) || version != 5) {
                out_err = "unsupported export-index version in: " + path;
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

        parus::ty::TypeId parse_type_repr_into_(
            std::string_view type_repr,
            parus::ty::TypePool& types
        ) {
            if (type_repr.empty()) return parus::ty::kInvalidType;

            parus::SourceManager sm{};
            parus::diag::Bag bag{};
            const uint32_t fid = sm.add("<export-index:type>", std::string(type_repr));
            auto toks = lex_with_sm_(sm, fid, &bag);
            if (bag.has_error()) return parus::ty::kInvalidType;

            parus::ast::AstArena ast{};
            parus::ParserFeatureFlags flags{};
            parus::Parser p(toks, ast, types, &bag, /*max_errors=*/16, flags);
            parus::ty::TypeId out = parus::ty::kInvalidType;
            (void)p.parse_type_full_for_macro(&out);
            if (bag.has_error()) return parus::ty::kInvalidType;
            return out;
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

        std::string select_apple_sdk_root_(const cli::Options& opt) {
            if (!opt.apple_sdk_root.empty()) return opt.apple_sdk_root;
            return getenv_string_("PARUS_APPLE_SDK_ROOT");
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
                case In::kSystemLld: return Out::kSystemLld;
                case In::kSystemClang: return Out::kSystemClang;
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

        const bool env_no_core = env_flag_truthy_(getenv_string_("PARUS_NO_CORE"));
        const bool disable_auto_core = opt.no_core || env_no_core;
        const bool auto_core_injection = !disable_auto_core && (opt.bundle.bundle_name != "core");
        std::string auto_core_export_index_path{};
        if (auto_core_injection) {
            auto_core_export_index_path = resolve_core_export_index_path_(opt);
        }

        const bool macro_ok = parus::macro::expand_program(ast, types, root, bag, opt.macro_budget);
        if (bag.has_error() || !macro_ok) {
            const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
            return (diag_rc != 0 || !macro_ok) ? 1 : 0;
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

        if (!opt.bundle.emit_export_index_path.empty()) {
            std::string write_err{};
            if (!write_export_index_(opt.bundle.emit_export_index_path, opt.bundle.bundle_name, bundle_surface, write_err)) {
                parus::diag::Diagnostic d(parus::diag::Severity::kError, parus::diag::Code::kExportIndexSchema, root_span);
                d.add_arg(write_err);
                bag.add(std::move(d));
                const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
                return (diag_rc != 0) ? 1 : 0;
            }
            return 0;
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

        std::set<std::pair<std::string, std::string>> external_seen{};
        auto add_external = [&](const ExportSurfaceEntry& e, bool same_bundle) {
            std::string lookup_path = e.path;
            if (!e.module_head.empty()) {
                const std::string prefix = e.module_head + "::";
                const bool already_prefixed =
                    lookup_path == e.module_head ||
                    lookup_path.starts_with(prefix);
                const bool same_module = same_bundle && e.module_head == pass_opt.name_resolve.current_module_head;
                if (!same_module && !already_prefixed) {
                    lookup_path = prefix + lookup_path;
                }
            }

            const auto key = std::make_pair(e.kind_text, lookup_path);
            if (!external_seen.insert(key).second) return;

            parus::passes::NameResolveOptions::ExternalExport x{};
            x.kind = e.kind;
            x.path = std::move(lookup_path);
            x.link_name = e.link_name;
            x.declared_type = parse_type_repr_into_(e.type_repr, types);
            x.declared_type_repr = e.type_repr;
            x.decl_span = root_span;
            x.decl_bundle_name = e.decl_bundle;
            x.module_head = e.module_head;
            x.decl_source_dir_norm = e.decl_dir;
            x.is_export = e.is_export;
            x.inst_payload = e.inst_payload;
            pass_opt.name_resolve.external_exports.push_back(std::move(x));
        };

        if (opt.bundle.enabled) {
            for (const auto& e : bundle_surface) {
                if (e.decl_file == current_norm) continue;
                add_external(e, /*same_bundle=*/true);
            }
        }

        std::vector<std::string> external_index_paths{};
        external_index_paths.reserve(inv.load_export_index_paths.size() + (auto_core_export_index_path.empty() ? 0u : 1u));
        if (!auto_core_export_index_path.empty()) {
            external_index_paths.push_back(auto_core_export_index_path);
        }
        for (const auto& p : inv.load_export_index_paths) {
            const std::string norm = parus::normalize_path(p);
            if (std::find(external_index_paths.begin(), external_index_paths.end(), norm) == external_index_paths.end()) {
                external_index_paths.push_back(norm);
            }
        }

        for (const auto& idx_path : external_index_paths) {
            std::string dep_bundle{};
            std::vector<ExportSurfaceEntry> dep_entries{};
            std::string load_err{};
            if (!load_export_index_(idx_path, dep_bundle, dep_entries, load_err)) {
                const parus::diag::Code code = load_err.starts_with("missing export-index file")
                    ? parus::diag::Code::kExportIndexMissing
                    : parus::diag::Code::kExportIndexSchema;
                parus::diag::Diagnostic d(parus::diag::Severity::kError, code, root_span);
                d.add_arg(load_err);
                bag.add(std::move(d));
                continue;
            }
            for (auto& e : dep_entries) {
                if (e.decl_bundle.empty()) e.decl_bundle = dep_bundle;
                add_external(e, /*same_bundle=*/false);
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

        parus::tyck::TyckResult tyck_res;
        {
            parus::tyck::TypeChecker tc(ast, types, bag, &type_resolve, &pres.generic_prep);
            if (opt.bundle.enabled || !external_index_paths.empty() || !inv.bundle_sources.empty()) {
                tc.set_seed_symbol_table(&pres.sym);
            }
            if (!core_impl_marker_file_ids.empty()) {
                tc.set_core_impl_marker_file_ids(std::move(core_impl_marker_file_ids));
            }
            tyck_res = tc.check_program(root);
        }
        if (bag.has_error() || !tyck_res.errors.empty()) {
            const int diag_rc = flush_diags_(bag, opt.lang, sm, opt.context_lines, opt.diag_format);
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

        parus::oir::Builder ob(sir_mod, types);
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

            if (actor_runtime_used) {
                const std::string prt_archive = resolve_prt_archive_path_(opt);
                if (prt_archive.empty()) {
                    std::cerr << "error: actor runtime archive is missing for target '"
                              << link_opt.target_triple << "' under sysroot '"
                              << link_opt.sysroot_path << "'.\n";
                    return 1;
                }
                link_opt.object_paths.push_back(prt_archive);
                link_opt.requires_cpp_runtime = !opt.freestanding;
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
