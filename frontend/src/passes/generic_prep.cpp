#include <parus/passes/GenericPrep.hpp>

#include <vector>

namespace parus::passes {

    namespace {

        static std::string qualify_(const std::vector<std::string>& ns, std::string_view name) {
            if (name.empty()) return {};
            if (ns.empty()) return std::string(name);
            std::string out;
            for (size_t i = 0; i < ns.size(); ++i) {
                if (i) out += "::";
                out += ns[i];
            }
            out += "::";
            out += std::string(name);
            return out;
        }

    } // namespace

    GenericPrepResult run_generic_prep(
        const ast::AstArena& ast,
        ast::StmtId program_root,
        diag::Bag& bag
    ) {
        (void)bag;
        GenericPrepResult out{};
        if (program_root == ast::k_invalid_stmt || (size_t)program_root >= ast.stmts().size()) {
            return out;
        }

        std::vector<std::string> ns_stack{};

        const auto collect_stmt = [&](auto&& self, ast::StmtId sid) -> void {
            if (sid == ast::k_invalid_stmt || (size_t)sid >= ast.stmts().size()) return;
            const auto& s = ast.stmt(sid);

            if (s.kind == ast::StmtKind::kNestDecl) {
                uint32_t pushed = 0;
                if (!s.nest_is_file_directive) {
                    const auto& segs = ast.path_segs();
                    const uint64_t begin = s.nest_path_begin;
                    const uint64_t end = begin + s.nest_path_count;
                    if (begin <= segs.size() && end <= segs.size()) {
                        for (uint32_t i = 0; i < s.nest_path_count; ++i) {
                            ns_stack.push_back(std::string(segs[s.nest_path_begin + i]));
                            ++pushed;
                        }
                    }
                }
                if (s.a != ast::k_invalid_stmt) self(self, s.a);
                while (pushed > 0) {
                    ns_stack.pop_back();
                    --pushed;
                }
                return;
            }

            auto register_template = [&](GenericTemplateKind kind, uint32_t arity) {
                if (arity == 0 || s.name.empty()) return;
                const std::string qn = qualify_(ns_stack, s.name);
                if (qn.empty()) return;
                out.templates.emplace(
                    qn,
                    GenericTemplateInfo{
                        .kind = kind,
                        .arity = arity,
                        .sid = sid,
                        .span = s.span,
                    }
                );
            };

            switch (s.kind) {
                case ast::StmtKind::kFnDecl:
                    register_template(GenericTemplateKind::kFn, s.fn_generic_param_count);
                    break;
                case ast::StmtKind::kClassDecl:
                    register_template(GenericTemplateKind::kClass, s.decl_generic_param_count);
                    break;
                case ast::StmtKind::kProtoDecl:
                    register_template(GenericTemplateKind::kProto, s.decl_generic_param_count);
                    break;
                case ast::StmtKind::kActsDecl:
                    register_template(GenericTemplateKind::kActs, s.decl_generic_param_count);
                    break;
                case ast::StmtKind::kFieldDecl:
                    register_template(GenericTemplateKind::kStruct, s.decl_generic_param_count);
                    break;
                default:
                    break;
            }

            if (s.kind == ast::StmtKind::kBlock) {
                const auto& kids = ast.stmt_children();
                const uint64_t begin = s.stmt_begin;
                const uint64_t end = begin + s.stmt_count;
                if (begin <= kids.size() && end <= kids.size()) {
                    for (uint32_t i = 0; i < s.stmt_count; ++i) {
                        self(self, kids[s.stmt_begin + i]);
                    }
                }
            } else if (s.kind == ast::StmtKind::kClassDecl ||
                       s.kind == ast::StmtKind::kProtoDecl ||
                       s.kind == ast::StmtKind::kActorDecl ||
                       s.kind == ast::StmtKind::kActsDecl) {
                const auto& kids = ast.stmt_children();
                const uint64_t begin = s.stmt_begin;
                const uint64_t end = begin + s.stmt_count;
                if (begin <= kids.size() && end <= kids.size()) {
                    for (uint32_t i = 0; i < s.stmt_count; ++i) {
                        self(self, kids[s.stmt_begin + i]);
                    }
                }
            }
        };

        collect_stmt(collect_stmt, program_root);
        return out;
    }

} // namespace parus::passes
