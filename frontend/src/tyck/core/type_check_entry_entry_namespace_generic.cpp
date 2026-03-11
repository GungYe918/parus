// frontend/src/tyck/type_check_entry.cpp
#include <parus/tyck/TypeCheck.hpp>
#include <parus/syntax/TokenKind.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/diag/DiagCode.hpp>
#include "../common/type_check_literals.hpp"

#include <sstream>
#include <cctype>
#include <algorithm>
#include <unordered_map>
#include <unordered_set>


namespace parus::tyck {

    using K = parus::syntax::TokenKind;
    using detail::ParsedFloatLiteral;
    using detail::ParsedIntLiteral;
    using detail::parse_float_literal_;
    using detail::parse_int_literal_;

    TyckResult TypeChecker::check_program(ast::StmtId program_stmt) {
        // -----------------------------
        // HARD RESET (매 호출 독립 보장)
        // -----------------------------
        result_ = TyckResult{};
        loop_stack_.clear();
        stmt_loop_depth_ = 0;
        fn_ctx_ = FnCtx{};
        pending_int_sym_.clear();
        pending_int_expr_.clear();
        fn_sid_stack_.clear();
        sym_is_mut_.clear();
        untyped_catch_binder_symbols_.clear();
        acts_default_operator_map_.clear();
        acts_default_method_map_.clear();
        acts_named_decl_by_owner_and_name_.clear();
        acts_default_decl_by_owner_.clear();
        acts_selection_scope_stack_.clear();
        acts_selection_by_symbol_.clear();
        field_abi_meta_by_type_.clear();
        enum_abi_meta_by_type_.clear();
        generic_fn_template_sid_set_.clear();
        generic_fn_instance_cache_.clear();
        generic_fn_checked_instances_.clear();
        generic_fn_checking_instances_.clear();
        generic_instantiated_fn_sids_.clear();
        pending_generic_instance_queue_.clear();
        pending_generic_instance_enqueued_.clear();
        generic_class_template_sid_set_.clear();
        generic_proto_template_sid_set_.clear();
        generic_acts_template_sid_set_.clear();
        generic_field_template_sid_set_.clear();
        generic_enum_template_sid_set_.clear();
        generic_class_instance_cache_.clear();
        generic_proto_instance_cache_.clear();
        generic_acts_instance_cache_.clear();
        generic_field_instance_cache_.clear();
        generic_enum_instance_cache_.clear();
        generic_decl_checked_instances_.clear();
        generic_decl_checking_instances_.clear();
        pending_generic_decl_instance_queue_.clear();
        pending_generic_decl_instance_enqueued_.clear();
        generic_instantiated_class_sids_.clear();
        generic_instantiated_proto_sids_.clear();
        generic_instantiated_acts_sids_.clear();
        generic_instantiated_field_sids_.clear();
        generic_instantiated_enum_sids_.clear();
        fn_qualified_name_by_stmt_.clear();
        class_effective_method_map_.clear();
        const_symbol_decl_sid_.clear();
        const_symbol_eval_state_.clear();
        const_symbol_runtime_values_.clear();
        const_cycle_diag_emitted_.clear();
        namespace_stack_.clear();
        import_alias_to_path_.clear();
        known_namespace_paths_.clear();
        import_alias_scope_stack_.clear();
        class_decl_by_name_.clear();
        class_decl_by_type_.clear();
        class_qualified_name_by_stmt_.clear();
        proto_decl_by_type_.clear();
        acts_qualified_name_by_stmt_.clear();
        class_member_fn_sid_set_.clear();
        actor_decl_by_name_.clear();
        actor_decl_by_type_.clear();
        actor_method_map_.clear();
        actor_member_fn_sid_set_.clear();
        proto_member_fn_sid_set_.clear();
        enum_decl_by_name_.clear();
        enum_decl_by_type_.clear();
        block_depth_ = 0;
        in_actor_method_ = false;
        in_actor_pub_method_ = false;
        in_actor_sub_method_ = false;

        // sym_ 초기화:
        // - bundle/module prepass 심볼이 있으면 시드 심볼테이블을 그대로 사용한다.
        // - 없으면 빈 심볼테이블로 시작한다.
        if (seed_sym_ != nullptr) {
            sym_ = *seed_sym_;
        } else {
            sym_ = sema::SymbolTable{};
        }

        // expr type cache: AST exprs 크기에 맞춰 리셋
        expr_type_cache_.assign(ast_.exprs().size(), ty::kInvalidType);
        expr_overload_target_cache_.assign(ast_.exprs().size(), ast::k_invalid_stmt);
        expr_ctor_owner_type_cache_.assign(ast_.exprs().size(), ty::kInvalidType);
        expr_enum_ctor_owner_type_cache_.assign(ast_.exprs().size(), ty::kInvalidType);
        expr_enum_ctor_variant_index_cache_.assign(ast_.exprs().size(), 0xFFFF'FFFFu);
        expr_enum_ctor_tag_value_cache_.assign(ast_.exprs().size(), 0);
        expr_resolved_symbol_cache_.assign(ast_.exprs().size(), sema::SymbolTable::kNoScope);
        expr_proto_const_decl_cache_.assign(ast_.exprs().size(), ast::k_invalid_stmt);
        expr_fstring_runtime_expr_cache_.assign(ast_.exprs().size(), ast::k_invalid_expr);
        param_resolved_symbol_cache_.assign(ast_.params().size(), sema::SymbolTable::kNoScope);
        result_.expr_types = expr_type_cache_; // 결과 벡터도 동일 크기로 시작
        result_.expr_overload_target = expr_overload_target_cache_;
        result_.expr_ctor_owner_type = expr_ctor_owner_type_cache_;
        result_.expr_enum_ctor_owner_type = expr_enum_ctor_owner_type_cache_;
        result_.expr_enum_ctor_variant_index = expr_enum_ctor_variant_index_cache_;
        result_.expr_enum_ctor_tag_value = expr_enum_ctor_tag_value_cache_;
        result_.expr_resolved_symbol = expr_resolved_symbol_cache_;
        result_.expr_proto_const_decl = expr_proto_const_decl_cache_;
        result_.expr_fstring_runtime_expr = expr_fstring_runtime_expr_cache_;
        result_.param_resolved_symbol = param_resolved_symbol_cache_;

        // string literal은 builtin text 타입으로 고정한다.
        if (string_type_ == ty::kInvalidType) {
            string_type_ = types_.builtin(ty::Builtin::kText);
        }

        // ------------------------------------------
        // Sanity: program은 Block이어야 한다 (정책)
        // ------------------------------------------
        if (program_stmt == ast::k_invalid_stmt) {
            result_.ok = false;
            return result_;
        }

        const ast::Stmt& root = ast_.stmt(program_stmt);
        if (root.kind != ast::StmtKind::kBlock) {
            if (diag_bag_) diag_(diag::Code::kTopLevelMustBeBlock, root.span);
            result_.ok = false;
            return result_;
        }

        // 파일 기본 nest 지시어를 먼저 반영한다.
        init_file_namespace_(program_stmt);
        collect_known_namespace_paths_(program_stmt);

        // ---------------------------------------------------------
        // PASS 1: Top-level decl precollect (mutual recursion 지원)
        // - 전역 스코프에 "함수 시그니처 타입(ty::Kind::kFn)"을 먼저 등록한다.
        // - 기존 check_program() 내부의 "invalid 타입으로 insert"하던 루프가
        //   거의 모든 TypeNotCallable 증상의 원인이었으므로 제거하고,
        //   이미 구현된 first_pass_collect_top_level_()를 정식으로 사용한다.
        // ---------------------------------------------------------
        first_pass_collect_top_level_(program_stmt);

        // ---------------------------------------------------------
        // PASS 1.5: proto/type/acts dependency cycle check
        // ---------------------------------------------------------
        {
            std::vector<ast::StmtId> node_sids{};
            node_sids.reserve(ast_.stmts().size());

            auto is_cycle_node_kind = [&](ast::StmtKind k) -> bool {
                switch (k) {
                    case ast::StmtKind::kProtoDecl:
                    case ast::StmtKind::kFieldDecl:
                    case ast::StmtKind::kEnumDecl:
                    case ast::StmtKind::kClassDecl:
                    case ast::StmtKind::kActorDecl:
                    case ast::StmtKind::kActsDecl:
                        return true;
                    default:
                        return false;
                }
            };

            for (ast::StmtId sid = 0; (size_t)sid < ast_.stmts().size(); ++sid) {
                if (is_cycle_node_kind(ast_.stmt(sid).kind)) {
                    node_sids.push_back(sid);
                }
            }

            std::unordered_map<ast::StmtId, size_t> node_index{};
            node_index.reserve(node_sids.size());
            for (size_t i = 0; i < node_sids.size(); ++i) {
                node_index.emplace(node_sids[i], i);
            }

            std::vector<std::vector<size_t>> adj(node_sids.size());
            std::vector<std::unordered_set<size_t>> adj_seen(node_sids.size());

            auto add_edge = [&](ast::StmtId from_sid, ast::StmtId to_sid) {
                auto fit = node_index.find(from_sid);
                auto tit = node_index.find(to_sid);
                if (fit == node_index.end() || tit == node_index.end()) return;
                const size_t fi = fit->second;
                const size_t ti = tit->second;
                if (adj_seen[fi].insert(ti).second) {
                    adj[fi].push_back(ti);
                }
            };

            auto resolve_decl_sid_from_type = [&](ty::TypeId t) -> ast::StmtId {
                if (t == ty::kInvalidType) return ast::k_invalid_stmt;
                if (auto it = class_decl_by_type_.find(t); it != class_decl_by_type_.end()) return it->second;
                if (auto it = actor_decl_by_type_.find(t); it != actor_decl_by_type_.end()) return it->second;
                if (auto it = enum_decl_by_type_.find(t); it != enum_decl_by_type_.end()) return it->second;
                if (auto it = field_abi_meta_by_type_.find(t); it != field_abi_meta_by_type_.end()) return it->second.sid;
                return ast::k_invalid_stmt;
            };

            std::unordered_map<std::string, ast::StmtId> acts_decl_by_name{};
            acts_decl_by_name.reserve(acts_qualified_name_by_stmt_.size());
            for (const auto& kv : acts_qualified_name_by_stmt_) {
                acts_decl_by_name[kv.second] = kv.first;
            }

            auto resolve_decl_sid_from_require_target = [&](const ast::Stmt& req_item) -> ast::StmtId {
                if (req_item.proto_require_kind == ast::ProtoRequireKind::kNone) return ast::k_invalid_stmt;
                const std::string raw_path = path_join_(req_item.proto_req_path_begin, req_item.proto_req_path_count);
                if (raw_path.empty()) return ast::k_invalid_stmt;

                std::string lookup = raw_path;
                if (auto rewritten = rewrite_imported_path_(lookup)) {
                    lookup = *rewritten;
                }

                auto sym_sid = lookup_symbol_(lookup);
                if (!sym_sid.has_value()) return ast::k_invalid_stmt;
                const auto& sym_obj = sym_.symbol(*sym_sid);

                switch (req_item.proto_require_kind) {
                    case ast::ProtoRequireKind::kStruct:
                        if (sym_obj.kind == sema::SymbolKind::kField) {
                            return resolve_decl_sid_from_type(sym_obj.declared_type);
                        }
                        break;

                    case ast::ProtoRequireKind::kEnum:
                        if (sym_obj.kind == sema::SymbolKind::kType) {
                            if (auto it = enum_decl_by_name_.find(sym_obj.name); it != enum_decl_by_name_.end()) {
                                return it->second;
                            }
                        }
                        break;

                    case ast::ProtoRequireKind::kClass:
                        if (sym_obj.kind == sema::SymbolKind::kType) {
                            if (auto it = class_decl_by_name_.find(sym_obj.name); it != class_decl_by_name_.end()) {
                                return it->second;
                            }
                        }
                        break;

                    case ast::ProtoRequireKind::kActor:
                        if (sym_obj.kind == sema::SymbolKind::kType) {
                            if (auto it = actor_decl_by_name_.find(sym_obj.name); it != actor_decl_by_name_.end()) {
                                return it->second;
                            }
                        }
                        break;

                    case ast::ProtoRequireKind::kActs:
                        if (sym_obj.kind == sema::SymbolKind::kAct) {
                            if (auto it = acts_decl_by_name.find(sym_obj.name); it != acts_decl_by_name.end()) {
                                return it->second;
                            }
                        }
                        break;

                    case ast::ProtoRequireKind::kNone:
                        break;
                }
                return ast::k_invalid_stmt;
            };

            auto resolve_proto_sid_no_diag = [&](const ast::PathRef& pr) -> std::optional<ast::StmtId> {
                std::string key = path_join_(pr.path_begin, pr.path_count);
                if (key.empty()) return std::nullopt;
                if (auto rewritten = rewrite_imported_path_(key)) {
                    key = *rewritten;
                }

                if (auto it = proto_decl_by_name_.find(key); it != proto_decl_by_name_.end()) {
                    return it->second;
                }

                if (auto sid = lookup_symbol_(key)) {
                    const auto& ss = sym_.symbol(*sid);
                    auto pit = proto_decl_by_name_.find(ss.name);
                    if (pit != proto_decl_by_name_.end()) {
                        return pit->second;
                    }
                }
                return std::nullopt;
            };

            for (const ast::StmtId sid : node_sids) {
                const auto& s = ast_.stmt(sid);
                if (s.kind == ast::StmtKind::kProtoDecl) {
                    const auto& refs = ast_.path_refs();
                    const uint64_t pb = s.decl_path_ref_begin;
                    const uint64_t pe = pb + s.decl_path_ref_count;
                    if (pb <= refs.size() && pe <= refs.size()) {
                        for (uint32_t i = s.decl_path_ref_begin; i < s.decl_path_ref_begin + s.decl_path_ref_count; ++i) {
                            if (auto psid = resolve_proto_sid_no_diag(refs[i])) {
                                add_edge(sid, *psid);
                            }
                        }
                    }

                    const auto& kids = ast_.stmt_children();
                    const uint64_t mb = s.stmt_begin;
                    const uint64_t me = mb + s.stmt_count;
                    if (mb <= kids.size() && me <= kids.size()) {
                        for (uint32_t i = s.stmt_begin; i < s.stmt_begin + s.stmt_count; ++i) {
                            const ast::StmtId msid = kids[i];
                            if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                            const auto& m = ast_.stmt(msid);
                            if (m.kind != ast::StmtKind::kRequire ||
                                m.proto_require_kind == ast::ProtoRequireKind::kNone) {
                                continue;
                            }
                            const ast::StmtId target_sid = resolve_decl_sid_from_require_target(m);
                            if (target_sid != ast::k_invalid_stmt) {
                                add_edge(sid, target_sid);
                            }
                        }
                    }
                    continue;
                }

                if (s.kind == ast::StmtKind::kFieldDecl ||
                    s.kind == ast::StmtKind::kEnumDecl ||
                    s.kind == ast::StmtKind::kClassDecl ||
                    s.kind == ast::StmtKind::kActorDecl) {
                    const auto& refs = ast_.path_refs();
                    const uint64_t pb = s.decl_path_ref_begin;
                    const uint64_t pe = pb + s.decl_path_ref_count;
                    if (pb <= refs.size() && pe <= refs.size()) {
                        for (uint32_t i = s.decl_path_ref_begin; i < s.decl_path_ref_begin + s.decl_path_ref_count; ++i) {
                            if (auto psid = resolve_proto_sid_no_diag(refs[i])) {
                                add_edge(sid, *psid);
                            }
                        }
                    }
                    continue;
                }

                if (s.kind == ast::StmtKind::kActsDecl && s.acts_is_for) {
                    const ty::TypeId owner_t = canonicalize_acts_owner_type_(s.acts_target_type);
                    if (const ast::StmtId owner_sid = resolve_decl_sid_from_type(owner_t);
                        owner_sid != ast::k_invalid_stmt) {
                        add_edge(sid, owner_sid);
                    }
                }
            }

            auto node_label = [&](ast::StmtId sid) -> std::string {
                if (sid == ast::k_invalid_stmt || (size_t)sid >= ast_.stmts().size()) return "<invalid>";
                const auto& s = ast_.stmt(sid);
                std::string kind;
                switch (s.kind) {
                    case ast::StmtKind::kProtoDecl: kind = "proto"; break;
                    case ast::StmtKind::kFieldDecl: kind = "struct"; break;
                    case ast::StmtKind::kEnumDecl: kind = "enum"; break;
                    case ast::StmtKind::kClassDecl: kind = "class"; break;
                    case ast::StmtKind::kActorDecl: kind = "actor"; break;
                    case ast::StmtKind::kActsDecl: kind = "acts"; break;
                    default: kind = "decl"; break;
                }
                return kind + " " + std::string(s.name);
            };

            std::vector<uint8_t> color(node_sids.size(), 0u);
            std::vector<size_t> stack{};
            std::vector<int32_t> stack_pos(node_sids.size(), -1);
            std::unordered_set<std::string> reported_cycles{};

            auto emit_cycle = [&](const std::vector<size_t>& cycle_nodes) {
                if (cycle_nodes.empty()) return;
                std::vector<ast::StmtId> cycle_sids;
                cycle_sids.reserve(cycle_nodes.size());
                for (const size_t idx : cycle_nodes) {
                    cycle_sids.push_back(node_sids[idx]);
                }

                std::vector<ast::StmtId> key_sids = cycle_sids;
                std::sort(key_sids.begin(), key_sids.end());
                key_sids.erase(std::unique(key_sids.begin(), key_sids.end()), key_sids.end());

                std::string key;
                for (const ast::StmtId sid : key_sids) {
                    key += std::to_string(sid);
                    key.push_back('#');
                }
                if (!reported_cycles.insert(key).second) return;

                std::ostringstream oss;
                for (size_t i = 0; i < cycle_sids.size(); ++i) {
                    if (i) oss << " -> ";
                    oss << node_label(cycle_sids[i]);
                }
                if (cycle_sids.size() == 1) {
                    oss << " -> " << node_label(cycle_sids.front());
                }

                const ast::StmtId head_sid = cycle_sids.front();
                const Span sp = (head_sid != ast::k_invalid_stmt && (size_t)head_sid < ast_.stmts().size())
                    ? ast_.stmt(head_sid).span
                    : root.span;
                diag_(diag::Code::kProtoDependencyCycle, sp, oss.str());
                err_(sp, std::string("dependency cycle: ") + oss.str());
            };

            auto dfs = [&](auto&& self, size_t u) -> void {
                color[u] = 1u;
                stack_pos[u] = static_cast<int32_t>(stack.size());
                stack.push_back(u);

                for (const size_t v : adj[u]) {
                    if (color[v] == 0u) {
                        self(self, v);
                    } else if (color[v] == 1u) {
                        const int32_t pos = stack_pos[v];
                        if (pos >= 0 && static_cast<size_t>(pos) < stack.size()) {
                            std::vector<size_t> cyc{};
                            for (size_t i = static_cast<size_t>(pos); i < stack.size(); ++i) {
                                cyc.push_back(stack[i]);
                            }
                            emit_cycle(cyc);
                        }
                    }
                }

                stack.pop_back();
                stack_pos[u] = -1;
                color[u] = 2u;
            };

            for (size_t i = 0; i < node_sids.size(); ++i) {
                if (color[i] == 0u) {
                    dfs(dfs, i);
                }
            }
        }

        // ---------------------------------------------------------
        // PASS 2: 실제 타입체크
        // - top-level block은 "scope 생성 없이" 자식만 순회한다.
        //   (중요: check_stmt_block_은 scope를 push하므로,
        //    root를 check_stmt_로 보내면 top-level이 새 스코프가 되어
        //    PASS1에서 등록한 전역 심볼이 가려질 수 있다.)
        // ---------------------------------------------------------
        push_acts_selection_scope_();
        push_alias_scope_();
        for (uint32_t i = 0; i < root.stmt_count; ++i) {
            const ast::StmtId child_id = ast_.stmt_children()[root.stmt_begin + i];
            check_stmt_(child_id);
            // 에러가 나도 계속 진행(정책)
        }

        // Drain concrete generic instances after top-level walk.
        bool progressed = true;
        while (progressed) {
            progressed = false;

            while (!pending_generic_decl_instance_queue_.empty()) {
                const ast::StmtId inst_sid = pending_generic_decl_instance_queue_.front();
                pending_generic_decl_instance_queue_.pop_front();
                pending_generic_decl_instance_enqueued_.erase(inst_sid);

                if (inst_sid == ast::k_invalid_stmt || (size_t)inst_sid >= ast_.stmts().size()) {
                    continue;
                }
                if (generic_decl_checked_instances_.find(inst_sid) != generic_decl_checked_instances_.end()) {
                    continue;
                }
                if (!generic_decl_checking_instances_.insert(inst_sid).second) {
                    continue;
                }

                check_stmt_(inst_sid);
                generic_decl_checked_instances_.insert(inst_sid);
                generic_decl_checking_instances_.erase(inst_sid);
                progressed = true;
            }

            while (!pending_generic_instance_queue_.empty()) {
                const ast::StmtId inst_sid = pending_generic_instance_queue_.front();
                pending_generic_instance_queue_.pop_front();
                pending_generic_instance_enqueued_.erase(inst_sid);

                if (inst_sid == ast::k_invalid_stmt || (size_t)inst_sid >= ast_.stmts().size()) {
                    continue;
                }
                if (generic_fn_checked_instances_.find(inst_sid) != generic_fn_checked_instances_.end()) {
                    continue;
                }
                if (!generic_fn_checking_instances_.insert(inst_sid).second) {
                    continue;
                }

                const auto& inst = ast_.stmt(inst_sid);
                const bool was_in_actor_method = in_actor_method_;
                const bool was_in_actor_pub = in_actor_pub_method_;
                const bool was_in_actor_sub = in_actor_sub_method_;
                if (actor_member_fn_sid_set_.find(inst_sid) != actor_member_fn_sid_set_.end()) {
                    in_actor_method_ = true;
                    in_actor_pub_method_ = (inst.fn_mode == ast::FnMode::kPub);
                    in_actor_sub_method_ = (inst.fn_mode == ast::FnMode::kSub);
                }
                check_stmt_fn_decl_(inst_sid, inst);
                in_actor_method_ = was_in_actor_method;
                in_actor_pub_method_ = was_in_actor_pub;
                in_actor_sub_method_ = was_in_actor_sub;

                generic_fn_checked_instances_.insert(inst_sid);
                generic_fn_checking_instances_.erase(inst_sid);
                progressed = true;
            }
        }
        pop_alias_scope_();
        pop_acts_selection_scope_();

        // ----------------------------------------
        // Finalize unresolved deferred integers:
        // - If an inferred integer "{integer}" is never consumed in a way that fixes the type,
        //   pick the smallest signed integer type that fits.
        // - Finalization applies to both symbol-backed and expression-backed pending integers.
        // ----------------------------------------
        auto choose_smallest_signed = [&](const num::BigInt& v) -> ty::TypeId {
            ty::Builtin b = ty::Builtin::kI128;
            if      (v.fits_i8())  b = ty::Builtin::kI8;
            else if (v.fits_i16()) b = ty::Builtin::kI16;
            else if (v.fits_i32()) b = ty::Builtin::kI32;
            else if (v.fits_i64()) b = ty::Builtin::kI64;
            return types_.builtin(b);
        };

        for (auto& kv : pending_int_sym_) {
            const uint32_t sym_id = kv.first;
            PendingInt& pi = kv.second;

            if (!pi.has_value || pi.resolved) continue;
            pi.resolved = true;
            pi.resolved_type = choose_smallest_signed(pi.value);
            sym_.update_declared_type(sym_id, pi.resolved_type);
        }

        for (auto& kv : pending_int_expr_) {
            const uint32_t eid = kv.first;
            PendingInt& pi = kv.second;

            if (!pi.has_value || pi.resolved) continue;
            pi.resolved = true;
            pi.resolved_type = choose_smallest_signed(pi.value);

            if (eid < expr_type_cache_.size()) {
                expr_type_cache_[eid] = pi.resolved_type;
            }
        }

        // 결과 반영
        auto sort_unique_sid_vec = [](std::vector<ast::StmtId>& v) {
            std::sort(v.begin(), v.end());
            v.erase(std::unique(v.begin(), v.end()), v.end());
        };
        sort_unique_sid_vec(generic_instantiated_fn_sids_);
        sort_unique_sid_vec(generic_instantiated_class_sids_);
        sort_unique_sid_vec(generic_instantiated_proto_sids_);
        sort_unique_sid_vec(generic_instantiated_acts_sids_);
        sort_unique_sid_vec(generic_instantiated_field_sids_);
        sort_unique_sid_vec(generic_instantiated_enum_sids_);

        result_.expr_types = expr_type_cache_;
        result_.expr_overload_target = expr_overload_target_cache_;
        result_.expr_ctor_owner_type = expr_ctor_owner_type_cache_;
        result_.expr_enum_ctor_owner_type = expr_enum_ctor_owner_type_cache_;
        result_.expr_enum_ctor_variant_index = expr_enum_ctor_variant_index_cache_;
        result_.expr_enum_ctor_tag_value = expr_enum_ctor_tag_value_cache_;
        result_.expr_resolved_symbol = expr_resolved_symbol_cache_;
        result_.expr_proto_const_decl = expr_proto_const_decl_cache_;
        result_.expr_fstring_runtime_expr = expr_fstring_runtime_expr_cache_;
        result_.param_resolved_symbol = param_resolved_symbol_cache_;
        result_.fn_qualified_names = fn_qualified_name_by_stmt_;
        result_.generic_instantiated_fn_sids = generic_instantiated_fn_sids_;
        result_.generic_instantiated_class_sids = generic_instantiated_class_sids_;
        result_.generic_instantiated_proto_sids = generic_instantiated_proto_sids_;
        result_.generic_instantiated_acts_sids = generic_instantiated_acts_sids_;
        result_.generic_instantiated_field_sids = generic_instantiated_field_sids_;
        result_.generic_instantiated_enum_sids = generic_instantiated_enum_sids_;
        result_.generic_acts_template_sids.assign(
            generic_acts_template_sid_set_.begin(),
            generic_acts_template_sid_set_.end()
        );
        std::sort(result_.generic_acts_template_sids.begin(), result_.generic_acts_template_sids.end());
        result_.actor_type_ids.clear();
        result_.actor_type_ids.reserve(actor_decl_by_type_.size());
        for (const auto& kv : actor_decl_by_type_) {
            if (kv.first != ty::kInvalidType) {
                result_.actor_type_ids.push_back(kv.first);
            }
        }
        std::sort(result_.actor_type_ids.begin(), result_.actor_type_ids.end());
        result_.actor_type_ids.erase(
            std::unique(result_.actor_type_ids.begin(), result_.actor_type_ids.end()),
            result_.actor_type_ids.end()
        );
        return result_;
    }

    // --------------------
    // errors
    // --------------------

    void TypeChecker::diag_(diag::Code code, Span sp) {
        if (!diag_bag_) return;
        result_.ok = false;
        diag::Diagnostic d(diag::Severity::kError, code, sp);
        diag_bag_->add(std::move(d));
    }

    void TypeChecker::diag_(diag::Code code, Span sp, std::string_view a0) {
        if (!diag_bag_) return;
        result_.ok = false;
        diag::Diagnostic d(diag::Severity::kError, code, sp);
        d.add_arg(a0);
        diag_bag_->add(std::move(d));
    }

    void TypeChecker::diag_(diag::Code code, Span sp, std::string_view a0, std::string_view a1) {
        if (!diag_bag_) return;
        result_.ok = false;
        diag::Diagnostic d(diag::Severity::kError, code, sp);
        d.add_arg(a0);
        d.add_arg(a1);
        diag_bag_->add(std::move(d));
    }

    void TypeChecker::diag_(diag::Code code, Span sp, std::string_view a0, std::string_view a1, std::string_view a2) {
        if (!diag_bag_) return;
        result_.ok = false;
        diag::Diagnostic d(diag::Severity::kError, code, sp);
        d.add_arg(a0);
        d.add_arg(a1);
        d.add_arg(a2);
        diag_bag_->add(std::move(d));
    }

    void TypeChecker::warn_(diag::Code code, Span sp, std::string_view a0) {
        if (!diag_bag_) return;
        diag::Diagnostic d(diag::Severity::kWarning, code, sp);
        if (!a0.empty()) d.add_arg(a0);
        diag_bag_->add(std::move(d));
    }

    void TypeChecker::warn_(diag::Code code, Span sp, std::string_view a0, std::string_view a1) {
        if (!diag_bag_) return;
        diag::Diagnostic d(diag::Severity::kWarning, code, sp);
        if (!a0.empty()) d.add_arg(a0);
        if (!a1.empty()) d.add_arg(a1);
        diag_bag_->add(std::move(d));
    }

    void TypeChecker::err_(Span sp, std::string msg) {
        // 1) TyckResult(errors)
        TyError e{};
        e.span = sp;
        e.message = msg;
        result_.errors.push_back(std::move(e));

        // NOTE:
        // - err_()는 저장용으로만 사용
        // - 사용자 출력은 항상 diag_(Code, args...)만 사용
    }

    bool TypeChecker::is_c_abi_safe_type_(ty::TypeId t, bool allow_void) const {
        std::unordered_set<ty::TypeId> visiting;
        return is_c_abi_safe_type_impl_(t, allow_void, visiting);
    }

    bool TypeChecker::is_c_abi_safe_type_impl_(
        ty::TypeId t,
        bool allow_void,
        std::unordered_set<ty::TypeId>& visiting
    ) const {
        if (t == ty::kInvalidType) return false;
        const auto& tt = types_.get(t);

        switch (tt.kind) {
            case ty::Kind::kError:
                return false;

            case ty::Kind::kBuiltin: {
                using B = ty::Builtin;
                switch (tt.builtin) {
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
                    case B::kText:
                        return true;
                    case B::kUnit:
                        return allow_void;
                    default:
                        return false;
                }
            }

            case ty::Kind::kPtr:
                // ptr T / ptr mut T: pointee도 FFI-safe여야 한다.
                return is_c_abi_safe_type_impl_(tt.elem, /*allow_void=*/false, visiting);

            case ty::Kind::kBorrow:
            case ty::Kind::kEscape:
            case ty::Kind::kOptional:
            case ty::Kind::kArray:
            case ty::Kind::kFn:
                // v0.0.1 규칙:
                // - borrow/escape/optional/direct function type 금지
                return false;

            case ty::Kind::kNamedUser: {
                const auto it = field_abi_meta_by_type_.find(t);
                if (it == field_abi_meta_by_type_.end()) return false;
                if (it->second.layout != ast::FieldLayout::kC) return false;
                if (it->second.sid == ast::k_invalid_stmt) return false;
                if ((size_t)it->second.sid >= ast_.stmts().size()) return false;

                // 순환 타입이 있더라도 무한 재귀를 피한다.
                if (!visiting.insert(t).second) return true;

                const auto& fs = ast_.stmt(it->second.sid);
                const uint64_t begin = fs.field_member_begin;
                const uint64_t end = begin + fs.field_member_count;
                if (end > ast_.field_members().size()) {
                    visiting.erase(t);
                    return false;
                }

                for (uint32_t i = fs.field_member_begin; i < fs.field_member_begin + fs.field_member_count; ++i) {
                    const auto& m = ast_.field_members()[i];
                    if (!is_c_abi_safe_type_impl_(m.type, /*allow_void=*/false, visiting)) {
                        visiting.erase(t);
                        return false;
                    }
                }

                visiting.erase(t);
                return true;
            }
        }

        return false;
    }

    void TypeChecker::check_c_abi_global_decl_(const ast::Stmt& s) {
        if (s.link_abi != ast::LinkAbi::kC) return;

        (void)ensure_generic_field_instance_from_type_(s.type, s.span);

        if (!s.is_static) {
            diag_(diag::Code::kAbiCGlobalMustBeStatic, s.span, s.name);
            err_(s.span, "C ABI global must be static: " + std::string(s.name));
        }

        if (!is_c_abi_safe_type_(s.type, /*allow_void=*/false)) {
            diag_(diag::Code::kAbiCTypeNotFfiSafe, s.span, std::string("global '") + std::string(s.name) + "'", types_.to_string(s.type));
            err_(s.span, "C ABI global type is not FFI-safe: " + types_.to_string(s.type));
        }
    }

    std::vector<std::string> TypeChecker::collect_generic_param_names_(const ast::Stmt& fn_decl) const {
        std::vector<std::string> out;
        if (fn_decl.fn_generic_param_count == 0) return out;
        out.reserve(fn_decl.fn_generic_param_count);
        for (uint32_t i = 0; i < fn_decl.fn_generic_param_count; ++i) {
            const uint32_t idx = fn_decl.fn_generic_param_begin + i;
            if (idx >= ast_.generic_param_decls().size()) break;
            out.emplace_back(ast_.generic_param_decls()[idx].name);
        }
        return out;
    }

    bool TypeChecker::is_self_named_type_(ty::TypeId t) const {
        std::vector<std::string_view> path{};
        std::vector<ty::TypeId> args{};
        if (!types_.decompose_named_user(t, path, args)) return false;
        return path.size() == 1 && path[0] == "Self" && args.empty();
    }

    bool TypeChecker::decompose_named_user_type_(
        ty::TypeId t,
        std::string& out_base,
        std::vector<ty::TypeId>& out_args
    ) const {
        out_base.clear();
        out_args.clear();
        if (t == ty::kInvalidType) return false;

        std::vector<std::string_view> path{};
        if (!types_.decompose_named_user(t, path, out_args) || path.empty()) {
            out_args.clear();
            return false;
        }

        for (size_t i = 0; i < path.size(); ++i) {
            if (i) out_base += "::";
            out_base.append(path[i].data(), path[i].size());
        }
        return !out_args.empty();
    }

    std::vector<std::string> TypeChecker::collect_decl_generic_param_names_(const ast::Stmt& decl) const {
        std::vector<std::string> out;
        if (decl.decl_generic_param_count == 0) return out;
        out.reserve(decl.decl_generic_param_count);
        for (uint32_t i = 0; i < decl.decl_generic_param_count; ++i) {
            const uint32_t idx = decl.decl_generic_param_begin + i;
            if (idx >= ast_.generic_param_decls().size()) break;
            out.emplace_back(ast_.generic_param_decls()[idx].name);
        }
        return out;
    }

    ty::TypeId TypeChecker::substitute_generic_type_(
        ty::TypeId src,
        const std::unordered_map<std::string, ty::TypeId>& subst
    ) const {
        if (src == ty::kInvalidType) return src;
        const auto& tt = types_.get(src);
        switch (tt.kind) {
            case ty::Kind::kNamedUser: {
                std::vector<std::string_view> path{};
                std::vector<ty::TypeId> args{};
                if (!types_.decompose_named_user(src, path, args) || path.empty()) {
                    return src;
                }

                if (args.empty()) {
                    if (path.size() == 1) {
                        auto it = subst.find(std::string(path.front()));
                        if (it != subst.end()) return it->second;
                    }
                    const std::string full = types_.to_string(src);
                    if (auto it = subst.find(full); it != subst.end()) {
                        return it->second;
                    }
                    if (!path.empty()) {
                        const std::string tail(path.back());
                        if (auto tail_it = subst.find(tail); tail_it != subst.end()) {
                            return tail_it->second;
                        }
                    }
                    return src;
                }

                bool changed = false;
                std::vector<ty::TypeId> sub_args{};
                sub_args.reserve(args.size());
                for (const auto arg : args) {
                    const ty::TypeId sub = substitute_generic_type_(arg, subst);
                    if (sub != arg) changed = true;
                    sub_args.push_back(sub);
                }
                if (!changed) return src;
                return types_.intern_named_path_with_args(
                    path.data(),
                    static_cast<uint32_t>(path.size()),
                    sub_args.data(),
                    static_cast<uint32_t>(sub_args.size())
                );
            }
            case ty::Kind::kBorrow: {
                const ty::TypeId elem = substitute_generic_type_(tt.elem, subst);
                return types_.make_borrow(elem, tt.borrow_is_mut);
            }
            case ty::Kind::kPtr: {
                const ty::TypeId elem = substitute_generic_type_(tt.elem, subst);
                return types_.make_ptr(elem, tt.ptr_is_mut);
            }
            case ty::Kind::kEscape: {
                const ty::TypeId elem = substitute_generic_type_(tt.elem, subst);
                return types_.make_escape(elem);
            }
            case ty::Kind::kOptional: {
                const ty::TypeId elem = substitute_generic_type_(tt.elem, subst);
                return types_.make_optional(elem);
            }
            case ty::Kind::kArray: {
                const ty::TypeId elem = substitute_generic_type_(tt.elem, subst);
                return types_.make_array(elem, tt.array_has_size, tt.array_size);
            }
            case ty::Kind::kFn: {
                const uint32_t pc = tt.param_count;
                std::vector<ty::TypeId> params;
                std::vector<std::string_view> labels;
                std::vector<uint8_t> has_default;
                params.reserve(pc);
                labels.reserve(pc);
                has_default.reserve(pc);
                for (uint32_t i = 0; i < pc; ++i) {
                    params.push_back(substitute_generic_type_(types_.fn_param_at(src, i), subst));
                    labels.push_back(types_.fn_param_label_at(src, i));
                    has_default.push_back(types_.fn_param_has_default_at(src, i) ? 1u : 0u);
                }
                const ty::TypeId ret = substitute_generic_type_(tt.ret, subst);
                return types_.make_fn(
                    ret,
                    params.empty() ? nullptr : params.data(),
                    pc,
                    types_.fn_positional_count(src),
                    labels.empty() ? nullptr : labels.data(),
                    has_default.empty() ? nullptr : has_default.data()
                );
            }
            default:
                return src;
        }
    }

    ast::ExprId TypeChecker::clone_expr_with_type_subst_(
        ast::ExprId src,
        const std::unordered_map<std::string, ty::TypeId>& subst,
        std::unordered_map<ast::ExprId, ast::ExprId>& expr_map,
        std::unordered_map<ast::StmtId, ast::StmtId>& stmt_map
    ) {
        if (src == ast::k_invalid_expr || (size_t)src >= ast_.exprs().size()) {
            return ast::k_invalid_expr;
        }
        if (auto it = expr_map.find(src); it != expr_map.end()) {
            return it->second;
        }

        const ast::Expr old_e = ast_.expr(src);
        ast::Expr e = old_e;

        e.a = clone_expr_with_type_subst_(old_e.a, subst, expr_map, stmt_map);
        e.b = clone_expr_with_type_subst_(old_e.b, subst, expr_map, stmt_map);
        e.c = clone_expr_with_type_subst_(old_e.c, subst, expr_map, stmt_map);
        e.block_tail = clone_expr_with_type_subst_(old_e.block_tail, subst, expr_map, stmt_map);
        e.loop_iter = clone_expr_with_type_subst_(old_e.loop_iter, subst, expr_map, stmt_map);
        e.block_stmt = clone_stmt_with_type_subst_(old_e.block_stmt, subst, expr_map, stmt_map);
        e.loop_body = clone_stmt_with_type_subst_(old_e.loop_body, subst, expr_map, stmt_map);

        if (old_e.arg_count > 0) {
            const auto& args = ast_.args();
            const uint64_t begin = old_e.arg_begin;
            const uint64_t end = begin + old_e.arg_count;
            if (begin <= args.size() && end <= args.size()) {
                std::vector<ast::Arg> src_args;
                src_args.reserve(old_e.arg_count);
                for (uint32_t i = 0; i < old_e.arg_count; ++i) {
                    src_args.push_back(args[old_e.arg_begin + i]);
                }
                e.arg_begin = static_cast<uint32_t>(ast_.args().size());
                e.arg_count = old_e.arg_count;
                for (uint32_t i = 0; i < old_e.arg_count; ++i) {
                    ast::Arg a = src_args[i];
                    a.expr = clone_expr_with_type_subst_(a.expr, subst, expr_map, stmt_map);
                    ast_.add_arg(a);
                }
            } else {
                e.arg_begin = 0;
                e.arg_count = 0;
            }
        }

        if (old_e.field_init_count > 0) {
            const auto& inits = ast_.field_init_entries();
            const uint64_t begin = old_e.field_init_begin;
            const uint64_t end = begin + old_e.field_init_count;
            if (begin <= inits.size() && end <= inits.size()) {
                std::vector<ast::FieldInitEntry> src_inits;
                src_inits.reserve(old_e.field_init_count);
                for (uint32_t i = 0; i < old_e.field_init_count; ++i) {
                    src_inits.push_back(inits[old_e.field_init_begin + i]);
                }
                e.field_init_begin = static_cast<uint32_t>(ast_.field_init_entries().size());
                e.field_init_count = old_e.field_init_count;
                for (uint32_t i = 0; i < old_e.field_init_count; ++i) {
                    ast::FieldInitEntry fe = src_inits[i];
                    fe.expr = clone_expr_with_type_subst_(fe.expr, subst, expr_map, stmt_map);
                    ast_.add_field_init_entry(fe);
                }
            } else {
                e.field_init_begin = 0;
                e.field_init_count = 0;
            }
        }

        if (old_e.string_part_count > 0) {
            const auto& parts = ast_.fstring_parts();
            const uint64_t begin = old_e.string_part_begin;
            const uint64_t end = begin + old_e.string_part_count;
            if (begin <= parts.size() && end <= parts.size()) {
                std::vector<ast::FStringPart> src_parts;
                src_parts.reserve(old_e.string_part_count);
                for (uint32_t i = 0; i < old_e.string_part_count; ++i) {
                    src_parts.push_back(parts[old_e.string_part_begin + i]);
                }
                e.string_part_begin = static_cast<uint32_t>(ast_.fstring_parts().size());
                e.string_part_count = old_e.string_part_count;
                for (uint32_t i = 0; i < old_e.string_part_count; ++i) {
                    ast::FStringPart fp = src_parts[i];
                    if (fp.is_expr) {
                        fp.expr = clone_expr_with_type_subst_(fp.expr, subst, expr_map, stmt_map);
                    }
                    ast_.add_fstring_part(fp);
                }
            } else {
                e.string_part_begin = 0;
                e.string_part_count = 0;
            }
        }

        if (old_e.call_type_arg_count > 0) {
            const auto& targs = ast_.type_args();
            const uint64_t begin = old_e.call_type_arg_begin;
            const uint64_t end = begin + old_e.call_type_arg_count;
            if (begin <= targs.size() && end <= targs.size()) {
                std::vector<ty::TypeId> src_type_args;
                src_type_args.reserve(old_e.call_type_arg_count);
                for (uint32_t i = 0; i < old_e.call_type_arg_count; ++i) {
                    src_type_args.push_back(targs[old_e.call_type_arg_begin + i]);
                }
                e.call_type_arg_begin = static_cast<uint32_t>(ast_.type_args().size());
                e.call_type_arg_count = old_e.call_type_arg_count;
                for (uint32_t i = 0; i < old_e.call_type_arg_count; ++i) {
                    const ty::TypeId t = substitute_generic_type_(src_type_args[i], subst);
                    ast_.add_type_arg(t);
                }
            } else {
                e.call_type_arg_begin = 0;
                e.call_type_arg_count = 0;
            }
        }

        if (old_e.field_init_type_node != ast::k_invalid_type_node &&
            (size_t)old_e.field_init_type_node < ast_.type_nodes().size()) {
            ast::TypeNode tn = ast_.type_node(old_e.field_init_type_node);
            if (tn.resolved_type != ty::kInvalidType) {
                tn.resolved_type = substitute_generic_type_(tn.resolved_type, subst);
            }
            e.field_init_type_node = ast_.add_type_node(tn);
        }

        if (e.cast_type != ty::kInvalidType) {
            e.cast_type = substitute_generic_type_(e.cast_type, subst);
        }
        if (e.target_type != ty::kInvalidType) {
            e.target_type = substitute_generic_type_(e.target_type, subst);
        }

        const ast::ExprId dst = ast_.add_expr(e);
        expr_map[src] = dst;
        return dst;
    }

    ast::StmtId TypeChecker::clone_stmt_with_type_subst_(
        ast::StmtId src,
        const std::unordered_map<std::string, ty::TypeId>& subst,
        std::unordered_map<ast::ExprId, ast::ExprId>& expr_map,
        std::unordered_map<ast::StmtId, ast::StmtId>& stmt_map
    ) {
        if (src == ast::k_invalid_stmt || (size_t)src >= ast_.stmts().size()) {
            return ast::k_invalid_stmt;
        }
        if (auto it = stmt_map.find(src); it != stmt_map.end()) {
            return it->second;
        }

        const ast::Stmt old_s = ast_.stmt(src);
        ast::Stmt s = old_s;

        s.expr = clone_expr_with_type_subst_(old_s.expr, subst, expr_map, stmt_map);
        s.init = clone_expr_with_type_subst_(old_s.init, subst, expr_map, stmt_map);
        s.a = clone_stmt_with_type_subst_(old_s.a, subst, expr_map, stmt_map);
        s.b = clone_stmt_with_type_subst_(old_s.b, subst, expr_map, stmt_map);

        if (s.type != ty::kInvalidType) s.type = substitute_generic_type_(s.type, subst);
        if (s.fn_ret != ty::kInvalidType) s.fn_ret = substitute_generic_type_(s.fn_ret, subst);
        if (s.acts_target_type != ty::kInvalidType) s.acts_target_type = substitute_generic_type_(s.acts_target_type, subst);
        if (s.var_acts_target_type != ty::kInvalidType) s.var_acts_target_type = substitute_generic_type_(s.var_acts_target_type, subst);

        if (old_s.param_count > 0) {
            const auto& params = ast_.params();
            const uint64_t begin = old_s.param_begin;
            const uint64_t end = begin + old_s.param_count;
            if (begin <= params.size() && end <= params.size()) {
                std::vector<ast::Param> src_params;
                src_params.reserve(old_s.param_count);
                for (uint32_t i = 0; i < old_s.param_count; ++i) {
                    src_params.push_back(params[old_s.param_begin + i]);
                }
                s.param_begin = static_cast<uint32_t>(ast_.params().size());
                s.param_count = old_s.param_count;
                for (uint32_t i = 0; i < old_s.param_count; ++i) {
                    ast::Param p = src_params[i];
                    p.type = substitute_generic_type_(p.type, subst);
                    if (p.has_default) {
                        p.default_expr = clone_expr_with_type_subst_(p.default_expr, subst, expr_map, stmt_map);
                    }
                    ast_.add_param(p);
                }
            } else {
                s.param_begin = 0;
                s.param_count = 0;
            }
        }

        if (old_s.stmt_count > 0) {
            const auto& kids = ast_.stmt_children();
            const uint64_t begin = old_s.stmt_begin;
            const uint64_t end = begin + old_s.stmt_count;
            if (begin <= kids.size() && end <= kids.size()) {
                std::vector<ast::StmtId> src_kids;
                src_kids.reserve(old_s.stmt_count);
                for (uint32_t i = 0; i < old_s.stmt_count; ++i) {
                    src_kids.push_back(kids[old_s.stmt_begin + i]);
                }
                std::vector<ast::StmtId> cloned_kids;
                cloned_kids.reserve(old_s.stmt_count);
                for (uint32_t i = 0; i < old_s.stmt_count; ++i) {
                    const ast::StmtId child =
                        clone_stmt_with_type_subst_(src_kids[i], subst, expr_map, stmt_map);
                    cloned_kids.push_back(child);
                }
                s.stmt_begin = static_cast<uint32_t>(ast_.stmt_children().size());
                s.stmt_count = static_cast<uint32_t>(cloned_kids.size());
                for (const auto child : cloned_kids) {
                    ast_.add_stmt_child(child);
                }
            } else {
                s.stmt_begin = 0;
                s.stmt_count = 0;
            }
        }

        if (old_s.case_count > 0) {
            const auto& cases = ast_.switch_cases();
            const uint64_t begin = old_s.case_begin;
            const uint64_t end = begin + old_s.case_count;
            if (begin <= cases.size() && end <= cases.size()) {
                std::vector<ast::SwitchCase> src_cases;
                src_cases.reserve(old_s.case_count);
                for (uint32_t i = 0; i < old_s.case_count; ++i) {
                    src_cases.push_back(cases[old_s.case_begin + i]);
                }
                std::vector<ast::SwitchCase> cloned_cases;
                cloned_cases.reserve(old_s.case_count);
                for (uint32_t i = 0; i < old_s.case_count; ++i) {
                    ast::SwitchCase c = src_cases[i];
                    c.body = clone_stmt_with_type_subst_(c.body, subst, expr_map, stmt_map);
                    if (c.enum_bind_count > 0) {
                        const auto& binds = ast_.switch_enum_binds();
                        const uint64_t bb = c.enum_bind_begin;
                        const uint64_t be = bb + c.enum_bind_count;
                        if (bb <= binds.size() && be <= binds.size()) {
                            std::vector<ast::SwitchEnumBind> src_binds;
                            src_binds.reserve(c.enum_bind_count);
                            for (uint32_t bi = 0; bi < c.enum_bind_count; ++bi) {
                                src_binds.push_back(binds[c.enum_bind_begin + bi]);
                            }
                            c.enum_bind_begin = static_cast<uint32_t>(ast_.switch_enum_binds().size());
                            for (auto b : src_binds) {
                                if (b.bind_type != ty::kInvalidType) {
                                    b.bind_type = substitute_generic_type_(b.bind_type, subst);
                                }
                                ast_.add_switch_enum_bind(b);
                            }
                        } else {
                            c.enum_bind_begin = 0;
                            c.enum_bind_count = 0;
                        }
                    }
                    cloned_cases.push_back(c);
                }
                s.case_begin = static_cast<uint32_t>(ast_.switch_cases().size());
                s.case_count = static_cast<uint32_t>(cloned_cases.size());
                for (const auto& c : cloned_cases) {
                    ast_.add_switch_case(c);
                }
            } else {
                s.case_begin = 0;
                s.case_count = 0;
            }
        }

        if (old_s.catch_clause_count > 0) {
            const auto& clauses = ast_.try_catch_clauses();
            const uint64_t begin = old_s.catch_clause_begin;
            const uint64_t end = begin + old_s.catch_clause_count;
            if (begin <= clauses.size() && end <= clauses.size()) {
                std::vector<ast::TryCatchClause> src_clauses;
                src_clauses.reserve(old_s.catch_clause_count);
                for (uint32_t i = 0; i < old_s.catch_clause_count; ++i) {
                    src_clauses.push_back(clauses[old_s.catch_clause_begin + i]);
                }
                s.catch_clause_begin = static_cast<uint32_t>(ast_.try_catch_clauses().size());
                s.catch_clause_count = old_s.catch_clause_count;
                for (auto c : src_clauses) {
                    if (c.bind_type != ty::kInvalidType) {
                        c.bind_type = substitute_generic_type_(c.bind_type, subst);
                    }
                    c.body = clone_stmt_with_type_subst_(c.body, subst, expr_map, stmt_map);
                    ast_.add_try_catch_clause(c);
                }
            } else {
                s.catch_clause_begin = 0;
                s.catch_clause_count = 0;
            }
        }

        if (old_s.field_member_count > 0) {
            const auto& members = ast_.field_members();
            const uint64_t begin = old_s.field_member_begin;
            const uint64_t end = begin + old_s.field_member_count;
            if (begin <= members.size() && end <= members.size()) {
                std::vector<ast::FieldMember> src_members;
                src_members.reserve(old_s.field_member_count);
                for (uint32_t i = 0; i < old_s.field_member_count; ++i) {
                    src_members.push_back(members[old_s.field_member_begin + i]);
                }
                s.field_member_begin = static_cast<uint32_t>(ast_.field_members().size());
                s.field_member_count = old_s.field_member_count;
                for (uint32_t i = 0; i < old_s.field_member_count; ++i) {
                    ast::FieldMember m = src_members[i];
                    m.type = substitute_generic_type_(m.type, subst);
                    ast_.add_field_member(m);
                }
            } else {
                s.field_member_begin = 0;
                s.field_member_count = 0;
            }
        }

        if (old_s.enum_variant_count > 0) {
            const auto& variants = ast_.enum_variant_decls();
            const uint64_t begin = old_s.enum_variant_begin;
            const uint64_t end = begin + old_s.enum_variant_count;
            if (begin <= variants.size() && end <= variants.size()) {
                std::vector<ast::EnumVariantDecl> src_variants;
                src_variants.reserve(old_s.enum_variant_count);
                for (uint32_t i = 0; i < old_s.enum_variant_count; ++i) {
                    src_variants.push_back(variants[old_s.enum_variant_begin + i]);
                }

                bool has_payload_block = false;
                uint32_t payload_src_begin = 0;
                uint32_t payload_src_end = 0;
                for (const auto& v : src_variants) {
                    if (v.payload_count == 0) continue;
                    const uint32_t vb = v.payload_begin;
                    const uint32_t ve = v.payload_begin + v.payload_count;
                    if (!has_payload_block) {
                        has_payload_block = true;
                        payload_src_begin = vb;
                        payload_src_end = ve;
                    } else {
                        payload_src_begin = std::min(payload_src_begin, vb);
                        payload_src_end = std::max(payload_src_end, ve);
                    }
                }

                uint32_t payload_dst_begin = 0;
                if (has_payload_block) {
                    const auto& members = ast_.field_members();
                    if (payload_src_begin <= members.size() &&
                        payload_src_end <= members.size() &&
                        payload_src_begin <= payload_src_end) {
                        std::vector<ast::FieldMember> src_members;
                        src_members.reserve(payload_src_end - payload_src_begin);
                        for (uint32_t i = payload_src_begin; i < payload_src_end; ++i) {
                            src_members.push_back(members[i]);
                        }
                        payload_dst_begin = static_cast<uint32_t>(ast_.field_members().size());
                        for (auto m : src_members) {
                            m.type = substitute_generic_type_(m.type, subst);
                            ast_.add_field_member(m);
                        }
                    } else {
                        has_payload_block = false;
                    }
                }

                s.enum_variant_begin = static_cast<uint32_t>(ast_.enum_variant_decls().size());
                s.enum_variant_count = old_s.enum_variant_count;
                for (auto v : src_variants) {
                    if (v.payload_count > 0) {
                        if (has_payload_block &&
                            v.payload_begin >= payload_src_begin &&
                            v.payload_begin + v.payload_count <= payload_src_end) {
                            const uint32_t rel = v.payload_begin - payload_src_begin;
                            v.payload_begin = payload_dst_begin + rel;
                        } else {
                            v.payload_begin = 0;
                            v.payload_count = 0;
                        }
                    }
                    ast_.add_enum_variant_decl(v);
                }
            } else {
                s.enum_variant_begin = 0;
                s.enum_variant_count = 0;
            }
        }

        if (old_s.decl_path_ref_count > 0) {
            const auto& refs = ast_.path_refs();
            const uint64_t begin = old_s.decl_path_ref_begin;
            const uint64_t end = begin + old_s.decl_path_ref_count;
            if (begin <= refs.size() && end <= refs.size()) {
                std::vector<ast::PathRef> src_refs;
                src_refs.reserve(old_s.decl_path_ref_count);
                for (uint32_t i = 0; i < old_s.decl_path_ref_count; ++i) {
                    src_refs.push_back(refs[old_s.decl_path_ref_begin + i]);
                }
                s.decl_path_ref_begin = static_cast<uint32_t>(ast_.path_refs().size());
                s.decl_path_ref_count = old_s.decl_path_ref_count;
                for (uint32_t i = 0; i < old_s.decl_path_ref_count; ++i) {
                    ast::PathRef pr = src_refs[i];
                    if (pr.type != ty::kInvalidType) {
                        pr.type = substitute_generic_type_(pr.type, subst);
                    }
                    ast_.add_path_ref(pr);
                }
            } else {
                s.decl_path_ref_begin = 0;
                s.decl_path_ref_count = 0;
            }
        }

        const ast::StmtId dst = ast_.add_stmt(s);
        stmt_map[src] = dst;
        return dst;
    }

    std::optional<ast::StmtId> TypeChecker::ensure_generic_function_instance_(
        ast::StmtId template_sid,
        const std::vector<ty::TypeId>& concrete_args,
        Span call_span
    ) {
        if (template_sid == ast::k_invalid_stmt || (size_t)template_sid >= ast_.stmts().size()) {
            return std::nullopt;
        }
        const ast::Stmt templ = ast_.stmt(template_sid);
        if (templ.kind != ast::StmtKind::kFnDecl || templ.fn_generic_param_count == 0) {
            return template_sid;
        }

        const auto generic_names = collect_generic_param_names_(templ);
        if (generic_names.size() != concrete_args.size()) {
            diag_(diag::Code::kGenericArityMismatch, call_span,
                  std::to_string(generic_names.size()),
                  std::to_string(concrete_args.size()));
            err_(call_span, "generic arity mismatch");
            return std::nullopt;
        }

        std::ostringstream key_oss;
        key_oss << template_sid << "|";
        for (size_t i = 0; i < concrete_args.size(); ++i) {
            if (i) key_oss << ",";
            key_oss << concrete_args[i];
        }
        const std::string cache_key = key_oss.str();

        if (auto it = generic_fn_instance_cache_.find(cache_key);
            it != generic_fn_instance_cache_.end()) {
            return it->second;
        }

        std::unordered_map<std::string, ty::TypeId> subst;
        subst.reserve(generic_names.size());
        for (size_t i = 0; i < generic_names.size(); ++i) {
            subst.emplace(generic_names[i], concrete_args[i]);
        }

        const uint32_t new_param_begin = static_cast<uint32_t>(ast_.params().size());
        std::unordered_map<ast::ExprId, ast::ExprId> expr_clone_map;
        std::unordered_map<ast::StmtId, ast::StmtId> stmt_clone_map;
        for (uint32_t i = 0; i < templ.param_count; ++i) {
            ast::Param p = ast_.params()[templ.param_begin + i];
            p.type = substitute_generic_type_(p.type, subst);
            if (p.has_default) {
                p.default_expr = clone_expr_with_type_subst_(p.default_expr, subst, expr_clone_map, stmt_clone_map);
            }
            ast_.add_param(p);
        }

        ast::Stmt inst = templ;
        inst.param_begin = new_param_begin;
        inst.param_count = templ.param_count;
        inst.fn_ret = substitute_generic_type_(templ.fn_ret, subst);
        inst.a = clone_stmt_with_type_subst_(templ.a, subst, expr_clone_map, stmt_clone_map);
        inst.b = clone_stmt_with_type_subst_(templ.b, subst, expr_clone_map, stmt_clone_map);
        inst.expr = clone_expr_with_type_subst_(templ.expr, subst, expr_clone_map, stmt_clone_map);
        inst.init = clone_expr_with_type_subst_(templ.init, subst, expr_clone_map, stmt_clone_map);
        inst.fn_generic_param_begin = 0;
        inst.fn_generic_param_count = 0;
        inst.fn_constraint_begin = 0;
        inst.fn_constraint_count = 0;

        std::vector<ty::TypeId> params;
        std::vector<std::string_view> labels;
        std::vector<uint8_t> has_default_flags;
        params.reserve(inst.param_count);
        labels.reserve(inst.param_count);
        has_default_flags.reserve(inst.param_count);
        for (uint32_t i = 0; i < inst.param_count; ++i) {
            const auto& p = ast_.params()[inst.param_begin + i];
            params.push_back(p.type == ty::kInvalidType ? types_.error() : p.type);
            labels.push_back(p.name);
            has_default_flags.push_back(p.has_default ? 1u : 0u);
        }
        ty::TypeId ret = inst.fn_ret;
        if (ret == ty::kInvalidType) ret = types_.builtin(ty::Builtin::kUnit);
        inst.type = types_.make_fn(
            ret,
            params.empty() ? nullptr : params.data(),
            static_cast<uint32_t>(params.size()),
            inst.positional_param_count,
            labels.empty() ? nullptr : labels.data(),
            has_default_flags.empty() ? nullptr : has_default_flags.data()
        );

        const ast::StmtId inst_sid = ast_.add_stmt(inst);

        std::string base_qname = std::string(templ.name);
        if (auto it = fn_qualified_name_by_stmt_.find(template_sid);
            it != fn_qualified_name_by_stmt_.end()) {
            base_qname = it->second;
        }
        std::ostringstream qn;
        qn << base_qname << "<";
        for (size_t i = 0; i < concrete_args.size(); ++i) {
            if (i) qn << ",";
            qn << types_.to_string(concrete_args[i]);
        }
        qn << ">";
        const std::string inst_qname = qn.str();
        fn_qualified_name_by_stmt_[inst_sid] = inst_qname;
        fn_decl_by_name_[inst_qname].push_back(inst_sid);
        if (class_member_fn_sid_set_.find(template_sid) != class_member_fn_sid_set_.end()) {
            class_member_fn_sid_set_.insert(inst_sid);
        }
        if (actor_member_fn_sid_set_.find(template_sid) != actor_member_fn_sid_set_.end()) {
            actor_member_fn_sid_set_.insert(inst_sid);
        }
        if (proto_member_fn_sid_set_.find(template_sid) != proto_member_fn_sid_set_.end()) {
            proto_member_fn_sid_set_.insert(inst_sid);
        }

        const size_t expr_size = ast_.exprs().size();
        if (expr_type_cache_.size() < expr_size) {
            expr_type_cache_.resize(expr_size, ty::kInvalidType);
        }
        if (expr_overload_target_cache_.size() < expr_size) {
            expr_overload_target_cache_.resize(expr_size, ast::k_invalid_stmt);
        }
        if (expr_ctor_owner_type_cache_.size() < expr_size) {
            expr_ctor_owner_type_cache_.resize(expr_size, ty::kInvalidType);
        }
        if (expr_enum_ctor_owner_type_cache_.size() < expr_size) {
            expr_enum_ctor_owner_type_cache_.resize(expr_size, ty::kInvalidType);
        }
        if (expr_enum_ctor_variant_index_cache_.size() < expr_size) {
            expr_enum_ctor_variant_index_cache_.resize(expr_size, 0xFFFF'FFFFu);
        }
        if (expr_enum_ctor_tag_value_cache_.size() < expr_size) {
            expr_enum_ctor_tag_value_cache_.resize(expr_size, 0);
        }
        if (expr_resolved_symbol_cache_.size() < expr_size) {
            expr_resolved_symbol_cache_.resize(expr_size, sema::SymbolTable::kNoScope);
        }
        if (expr_proto_const_decl_cache_.size() < expr_size) {
            expr_proto_const_decl_cache_.resize(expr_size, ast::k_invalid_stmt);
        }
        if (expr_fstring_runtime_expr_cache_.size() < expr_size) {
            expr_fstring_runtime_expr_cache_.resize(expr_size, ast::k_invalid_expr);
        }
        const size_t param_size = ast_.params().size();
        if (param_resolved_symbol_cache_.size() < param_size) {
            param_resolved_symbol_cache_.resize(param_size, sema::SymbolTable::kNoScope);
        }

        generic_fn_instance_cache_[cache_key] = inst_sid;
        generic_instantiated_fn_sids_.push_back(inst_sid);

        if (generic_fn_checked_instances_.find(inst_sid) == generic_fn_checked_instances_.end() &&
            pending_generic_instance_enqueued_.insert(inst_sid).second) {
            pending_generic_instance_queue_.push_back(inst_sid);
        }

        return inst_sid;
    }

    std::string TypeChecker::path_ref_display_(const ast::PathRef& pr) const {
        if (pr.type != ty::kInvalidType) {
            return types_.to_string(pr.type);
        }
        return path_join_(pr.path_begin, pr.path_count);
    }

    std::optional<ast::StmtId> TypeChecker::resolve_proto_decl_from_type_(
        ty::TypeId proto_type,
        Span use_span,
        bool* out_typed_path_failure
    ) {
        if (out_typed_path_failure) *out_typed_path_failure = false;
        if (proto_type == ty::kInvalidType) return std::nullopt;

        if (auto it = proto_decl_by_type_.find(proto_type); it != proto_decl_by_type_.end()) {
            return it->second;
        }

        std::string direct_name = types_.to_string(proto_type);
        if (!direct_name.empty()) {
            if (auto rewritten = rewrite_imported_path_(direct_name)) {
                direct_name = *rewritten;
            }
            if (auto it = proto_decl_by_name_.find(direct_name); it != proto_decl_by_name_.end()) {
                return it->second;
            }
            if (auto sym_sid = lookup_symbol_(direct_name)) {
                const auto& ss = sym_.symbol(*sym_sid);
                auto pit = proto_decl_by_name_.find(ss.name);
                if (pit != proto_decl_by_name_.end()) {
                    return pit->second;
                }
            }
        }

        std::string base;
        std::vector<ty::TypeId> args;
        const bool is_generic_applied = decompose_named_user_type_(proto_type, base, args);
        if (!is_generic_applied) {
            return std::nullopt;
        }

        std::string base_key = base;
        if (auto rewritten = rewrite_imported_path_(base_key)) {
            base_key = *rewritten;
        }

        ast::StmtId templ_sid = ast::k_invalid_stmt;
        if (auto it = proto_decl_by_name_.find(base_key); it != proto_decl_by_name_.end()) {
            templ_sid = it->second;
        } else if (auto sym_sid = lookup_symbol_(base_key)) {
            const auto& ss = sym_.symbol(*sym_sid);
            auto pit = proto_decl_by_name_.find(ss.name);
            if (pit != proto_decl_by_name_.end()) {
                templ_sid = pit->second;
            }
        }

        auto kind_name = [](passes::GenericTemplateKind k) -> const char* {
            switch (k) {
                case passes::GenericTemplateKind::kFn: return "fn";
                case passes::GenericTemplateKind::kClass: return "class";
                case passes::GenericTemplateKind::kProto: return "proto";
                case passes::GenericTemplateKind::kActs: return "acts";
                case passes::GenericTemplateKind::kStruct: return "struct";
            }
            return "unknown";
        };

        if (templ_sid == ast::k_invalid_stmt && generic_prep_ != nullptr) {
            if (auto it = generic_prep_->templates.find(base_key); it != generic_prep_->templates.end()) {
                if (it->second.kind == passes::GenericTemplateKind::kProto) {
                    templ_sid = it->second.sid;
                } else {
                    if (out_typed_path_failure) *out_typed_path_failure = true;
                    diag_(diag::Code::kGenericTypePathTemplateKindMismatch, use_span,
                          base_key, "proto", kind_name(it->second.kind));
                    err_(use_span, "generic type path target kind mismatch");
                    return std::nullopt;
                }
            }
        }

        if (templ_sid == ast::k_invalid_stmt || (size_t)templ_sid >= ast_.stmts().size()) {
            if (out_typed_path_failure) *out_typed_path_failure = true;
            diag_(diag::Code::kGenericTypePathTemplateNotFound, use_span, base_key);
            err_(use_span, "generic proto template not found: " + base_key);
            return std::nullopt;
        }

        const auto& templ = ast_.stmt(templ_sid);
        if (templ.kind != ast::StmtKind::kProtoDecl) {
            if (out_typed_path_failure) *out_typed_path_failure = true;
            diag_(diag::Code::kGenericTypePathTemplateKindMismatch, use_span, base_key, "proto", "non-proto");
            err_(use_span, "generic type path target is not proto: " + base_key);
            return std::nullopt;
        }

        const uint32_t expected = templ.decl_generic_param_count;
        const uint32_t got = static_cast<uint32_t>(args.size());
        if (expected != got) {
            if (out_typed_path_failure) *out_typed_path_failure = true;
            diag_(diag::Code::kGenericTypePathArityMismatch, use_span,
                  base_key, std::to_string(expected), std::to_string(got));
            err_(use_span, "generic proto arity mismatch");
            return std::nullopt;
        }
        if (expected == 0) {
            return templ_sid;
        }

        const auto inst = ensure_generic_proto_instance_(templ_sid, args, use_span);
        if (!inst.has_value() && out_typed_path_failure) {
            *out_typed_path_failure = true;
        }
        return inst;
    }

    std::optional<ast::StmtId> TypeChecker::resolve_proto_decl_from_path_ref_(
        const ast::PathRef& pr,
        Span use_span,
        bool* out_typed_path_failure
    ) {
        if (out_typed_path_failure) *out_typed_path_failure = false;
        if (pr.type != ty::kInvalidType) {
            bool typed_path_failure = false;
            if (auto sid = resolve_proto_decl_from_type_(pr.type, use_span, &typed_path_failure)) {
                return sid;
            }
            if (typed_path_failure) {
                if (out_typed_path_failure) *out_typed_path_failure = true;
                return std::nullopt;
            }
        }

        std::string key = path_join_(pr.path_begin, pr.path_count);
        if (key.empty()) return std::nullopt;
        if (auto rewritten = rewrite_imported_path_(key)) {
            key = *rewritten;
        }
        if (auto it = proto_decl_by_name_.find(key); it != proto_decl_by_name_.end()) {
            return it->second;
        }
        if (auto sym_sid = lookup_symbol_(key)) {
            const auto& ss = sym_.symbol(*sym_sid);
            auto pit = proto_decl_by_name_.find(ss.name);
            if (pit != proto_decl_by_name_.end()) {
                return pit->second;
            }
        }
        return std::nullopt;
    }

    std::optional<ast::StmtId> TypeChecker::ensure_generic_field_instance_(
        ast::StmtId template_sid,
        const std::vector<ty::TypeId>& concrete_args,
        Span use_span
    ) {
        if (template_sid == ast::k_invalid_stmt || (size_t)template_sid >= ast_.stmts().size()) {
            return std::nullopt;
        }
        const ast::Stmt templ = ast_.stmt(template_sid);
        if (templ.kind != ast::StmtKind::kFieldDecl || templ.decl_generic_param_count == 0) {
            return template_sid;
        }

        const auto generic_names = collect_decl_generic_param_names_(templ);
        if (generic_names.size() != concrete_args.size()) {
            std::string base_qname = std::string(templ.name);
            if (templ.type != ty::kInvalidType) {
                base_qname = types_.to_string(templ.type);
            }
            diag_(diag::Code::kGenericTypePathArityMismatch, use_span,
                  base_qname,
                  std::to_string(generic_names.size()),
                  std::to_string(concrete_args.size()));
            err_(use_span, "generic struct arity mismatch");
            return std::nullopt;
        }

        std::ostringstream key_oss;
        key_oss << template_sid << "|";
        for (size_t i = 0; i < concrete_args.size(); ++i) {
            if (i) key_oss << ",";
            key_oss << concrete_args[i];
        }
        const std::string cache_key = key_oss.str();
        if (auto it = generic_field_instance_cache_.find(cache_key);
            it != generic_field_instance_cache_.end()) {
            return it->second;
        }

        std::unordered_map<std::string, ty::TypeId> subst;
        subst.reserve(generic_names.size());
        for (size_t i = 0; i < generic_names.size(); ++i) {
            subst.emplace(generic_names[i], concrete_args[i]);
        }

        auto resolve_proto_sid_for_constraint = [&](std::string_view raw) -> std::optional<ast::StmtId> {
            if (raw.empty()) return std::nullopt;
            std::string key(raw);
            if (auto rewritten = rewrite_imported_path_(key)) key = *rewritten;
            auto it = proto_decl_by_name_.find(key);
            if (it != proto_decl_by_name_.end()) return it->second;
            if (auto sym_sid = lookup_symbol_(key)) {
                const auto& ss = sym_.symbol(*sym_sid);
                auto pit = proto_decl_by_name_.find(ss.name);
                if (pit != proto_decl_by_name_.end()) return pit->second;
            }
            return std::nullopt;
        };

        auto fn_sig_same = [&](const ast::Stmt& a, const ast::Stmt& b) -> bool {
            if (a.kind != ast::StmtKind::kFnDecl || b.kind != ast::StmtKind::kFnDecl) return false;
            if (a.name != b.name) return false;
            if (a.param_count != b.param_count) return false;
            if (a.positional_param_count != b.positional_param_count) return false;
            if (a.fn_ret != b.fn_ret) return false;
            for (uint32_t i = 0; i < a.param_count; ++i) {
                const auto& ap = ast_.params()[a.param_begin + i];
                const auto& bp = ast_.params()[b.param_begin + i];
                if (ap.type != bp.type || ap.is_self != bp.is_self || ap.self_kind != bp.self_kind) return false;
            }
            return true;
        };

        auto proto_effective_required_empty = [&](ast::StmtId proto_sid) -> bool {
            if (proto_sid == ast::k_invalid_stmt || (size_t)proto_sid >= ast_.stmts().size()) return false;
            std::vector<ast::StmtId> reqs;
            std::vector<ast::StmtId> provs;
            std::unordered_set<ast::StmtId> visiting;
            auto collect = [&](auto&& self, ast::StmtId cur_sid) -> void {
                if (cur_sid == ast::k_invalid_stmt || (size_t)cur_sid >= ast_.stmts().size()) return;
                if (!visiting.insert(cur_sid).second) return;
                const auto& cur = ast_.stmt(cur_sid);
                if (cur.kind != ast::StmtKind::kProtoDecl) return;
                const auto& refs = ast_.path_refs();
                const uint64_t ib = cur.decl_path_ref_begin;
                const uint64_t ie = ib + cur.decl_path_ref_count;
                if (ib <= refs.size() && ie <= refs.size()) {
                    for (uint32_t i = cur.decl_path_ref_begin; i < cur.decl_path_ref_begin + cur.decl_path_ref_count; ++i) {
                        if (auto base_sid = resolve_proto_decl_from_path_ref_(refs[i], use_span)) {
                            self(self, *base_sid);
                        }
                    }
                }
                const auto& kids = ast_.stmt_children();
                const uint64_t mb = cur.stmt_begin;
                const uint64_t me = mb + cur.stmt_count;
                if (mb <= kids.size() && me <= kids.size()) {
                    for (uint32_t i = cur.stmt_begin; i < cur.stmt_begin + cur.stmt_count; ++i) {
                        const auto msid = kids[i];
                        if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                        const auto& m = ast_.stmt(msid);
                        if (m.kind != ast::StmtKind::kFnDecl) continue;
                        if (m.proto_fn_role == ast::ProtoFnRole::kRequire) reqs.push_back(msid);
                        if (m.proto_fn_role == ast::ProtoFnRole::kProvide && m.a != ast::k_invalid_stmt) provs.push_back(msid);
                    }
                }
            };
            collect(collect, proto_sid);

            for (const auto req_sid : reqs) {
                if (req_sid == ast::k_invalid_stmt || (size_t)req_sid >= ast_.stmts().size()) continue;
                const auto& req = ast_.stmt(req_sid);
                bool satisfied = false;
                for (const auto prov_sid : provs) {
                    if (prov_sid == ast::k_invalid_stmt || (size_t)prov_sid >= ast_.stmts().size()) continue;
                    if (fn_sig_same(req, ast_.stmt(prov_sid))) {
                        satisfied = true;
                        break;
                    }
                }
                if (!satisfied) return false;
            }
            return true;
        };

        auto type_satisfies_proto_constraint = [&](ty::TypeId concrete_t, ast::StmtId proto_sid) -> bool {
            if (proto_sid == ast::k_invalid_stmt) return false;
            if (!evaluate_proto_require_at_apply_(proto_sid, concrete_t, use_span,
                                                  /*emit_unsatisfied_diag=*/false,
                                                  /*emit_shape_diag=*/false)) {
                return false;
            }
            if (proto_effective_required_empty(proto_sid)) return true;

            ast::StmtId owner_sid = ast::k_invalid_stmt;
            if (auto cit = class_decl_by_type_.find(concrete_t); cit != class_decl_by_type_.end()) {
                owner_sid = cit->second;
            } else if (auto fit = field_abi_meta_by_type_.find(concrete_t); fit != field_abi_meta_by_type_.end()) {
                owner_sid = fit->second.sid;
            }
            if (owner_sid == ast::k_invalid_stmt || (size_t)owner_sid >= ast_.stmts().size()) return false;
            const auto& owner = ast_.stmt(owner_sid);
            if (owner.kind != ast::StmtKind::kClassDecl && owner.kind != ast::StmtKind::kFieldDecl) return false;

            const auto& refs = ast_.path_refs();
            const uint64_t begin = owner.decl_path_ref_begin;
            const uint64_t end = begin + owner.decl_path_ref_count;
            if (begin > refs.size() || end > refs.size()) return false;
            for (uint32_t i = owner.decl_path_ref_begin; i < owner.decl_path_ref_begin + owner.decl_path_ref_count; ++i) {
                if (auto psid = resolve_proto_decl_from_path_ref_(refs[i], use_span)) {
                    if (*psid == proto_sid) return true;
                }
            }
            return false;
        };

        for (uint32_t ci = 0; ci < templ.decl_constraint_count; ++ci) {
            const uint32_t idx = templ.decl_constraint_begin + ci;
            if (idx >= ast_.fn_constraint_decls().size()) break;
            const auto& cc = ast_.fn_constraint_decls()[idx];

            auto bit = subst.find(std::string(cc.type_param));
            if (bit == subst.end()) {
                diag_(diag::Code::kGenericUnknownTypeParamInConstraint, cc.span, cc.type_param);
                err_(cc.span, "struct declaration constraint references unknown generic parameter");
                return std::nullopt;
            }

            const std::string proto_path = path_join_(cc.proto_path_begin, cc.proto_path_count);
            auto proto_sid = resolve_proto_sid_for_constraint(proto_path);
            if (!proto_sid.has_value()) {
                diag_(diag::Code::kGenericConstraintProtoNotFound, cc.span, proto_path);
                err_(cc.span, "struct declaration constraint references unknown proto");
                return std::nullopt;
            }
            if (!type_satisfies_proto_constraint(bit->second, *proto_sid)) {
                diag_(diag::Code::kGenericDeclConstraintUnsatisfied, cc.span,
                      cc.type_param, proto_path, types_.to_string(bit->second));
                err_(cc.span, "struct declaration generic constraint unsatisfied");
                return std::nullopt;
            }
        }

        std::unordered_map<ast::ExprId, ast::ExprId> expr_clone_map;
        std::unordered_map<ast::StmtId, ast::StmtId> stmt_clone_map;
        const ast::StmtId inst_sid = clone_stmt_with_type_subst_(
            template_sid, subst, expr_clone_map, stmt_clone_map);
        if (inst_sid == ast::k_invalid_stmt || (size_t)inst_sid >= ast_.stmts().size()) {
            return std::nullopt;
        }
        auto& inst = ast_.stmt_mut(inst_sid);
        inst.decl_generic_param_begin = 0;
        inst.decl_generic_param_count = 0;
        inst.decl_constraint_begin = 0;
        inst.decl_constraint_count = 0;

        std::string base_qname = std::string(templ.name);
        if (templ.type != ty::kInvalidType) {
            base_qname = types_.to_string(templ.type);
        }
        std::ostringstream qn;
        qn << base_qname << "<";
        for (size_t i = 0; i < concrete_args.size(); ++i) {
            if (i) qn << ",";
            qn << types_.to_string(concrete_args[i]);
        }
        qn << ">";
        const std::string inst_qname = qn.str();
        const ty::TypeId inst_type = types_.intern_ident(ast_.add_owned_string(inst_qname));
        inst.name = ast_.add_owned_string(inst_qname);
        inst.type = inst_type;

        if (auto existing = sym_.lookup_in_current(inst_qname)) {
            (void)sym_.update_declared_type(*existing, inst_type);
        } else {
            (void)sym_.insert(sema::SymbolKind::kField, inst_qname, inst_type, inst.span);
        }

        field_abi_meta_by_type_[inst_type] = FieldAbiMeta{
            .sid = inst_sid,
            .layout = inst.field_layout,
            .align = inst.field_align,
        };

        const size_t expr_size = ast_.exprs().size();
        if (expr_type_cache_.size() < expr_size) expr_type_cache_.resize(expr_size, ty::kInvalidType);
        if (expr_overload_target_cache_.size() < expr_size) expr_overload_target_cache_.resize(expr_size, ast::k_invalid_stmt);
        if (expr_ctor_owner_type_cache_.size() < expr_size) expr_ctor_owner_type_cache_.resize(expr_size, ty::kInvalidType);
        if (expr_enum_ctor_owner_type_cache_.size() < expr_size) expr_enum_ctor_owner_type_cache_.resize(expr_size, ty::kInvalidType);
        if (expr_enum_ctor_variant_index_cache_.size() < expr_size) expr_enum_ctor_variant_index_cache_.resize(expr_size, 0xFFFF'FFFFu);
        if (expr_enum_ctor_tag_value_cache_.size() < expr_size) expr_enum_ctor_tag_value_cache_.resize(expr_size, 0);
        if (expr_resolved_symbol_cache_.size() < expr_size) expr_resolved_symbol_cache_.resize(expr_size, sema::SymbolTable::kNoScope);
        if (expr_proto_const_decl_cache_.size() < expr_size) expr_proto_const_decl_cache_.resize(expr_size, ast::k_invalid_stmt);
        if (expr_fstring_runtime_expr_cache_.size() < expr_size) expr_fstring_runtime_expr_cache_.resize(expr_size, ast::k_invalid_expr);
        const size_t param_size = ast_.params().size();
        if (param_resolved_symbol_cache_.size() < param_size) param_resolved_symbol_cache_.resize(param_size, sema::SymbolTable::kNoScope);

        generic_field_instance_cache_[cache_key] = inst_sid;
        generic_instantiated_field_sids_.push_back(inst_sid);
        if (generic_decl_checked_instances_.find(inst_sid) == generic_decl_checked_instances_.end() &&
            pending_generic_decl_instance_enqueued_.insert(inst_sid).second) {
            pending_generic_decl_instance_queue_.push_back(inst_sid);
        }
        return inst_sid;
    }

    std::optional<ast::StmtId> TypeChecker::ensure_generic_field_instance_from_type_(
        ty::TypeId maybe_generic_field_type,
        Span use_span
    ) {
        if (maybe_generic_field_type == ty::kInvalidType) return std::nullopt;

        std::string base;
        std::vector<ty::TypeId> args;
        if (!decompose_named_user_type_(maybe_generic_field_type, base, args) || args.empty()) {
            return std::nullopt;
        }

        std::string base_key = base;
        if (auto rewritten = rewrite_imported_path_(base_key)) {
            base_key = *rewritten;
        }

        auto sym_sid = lookup_symbol_(base_key);
        if (!sym_sid.has_value()) return std::nullopt;
        const auto& ss = sym_.symbol(*sym_sid);
        if (ss.kind != sema::SymbolKind::kField || ss.declared_type == ty::kInvalidType) {
            return std::nullopt;
        }

        auto fit = field_abi_meta_by_type_.find(ss.declared_type);
        if (fit == field_abi_meta_by_type_.end()) {
            return std::nullopt;
        }
        const ast::StmtId templ_sid = fit->second.sid;
        if (templ_sid == ast::k_invalid_stmt || (size_t)templ_sid >= ast_.stmts().size()) {
            return std::nullopt;
        }
        const auto& templ = ast_.stmt(templ_sid);
        if (templ.kind != ast::StmtKind::kFieldDecl) {
            return std::nullopt;
        }
        if (templ.decl_generic_param_count == 0) {
            return templ_sid;
        }
        return ensure_generic_field_instance_(templ_sid, args, use_span);
    }

    std::optional<ast::StmtId> TypeChecker::ensure_generic_enum_instance_(
        ast::StmtId template_sid,
        const std::vector<ty::TypeId>& concrete_args,
        Span use_span
    ) {
        if (template_sid == ast::k_invalid_stmt || (size_t)template_sid >= ast_.stmts().size()) {
            return std::nullopt;
        }
        const ast::Stmt templ = ast_.stmt(template_sid);
        if (templ.kind != ast::StmtKind::kEnumDecl || templ.decl_generic_param_count == 0) {
            return template_sid;
        }

        const auto generic_names = collect_decl_generic_param_names_(templ);
        if (generic_names.size() != concrete_args.size()) {
            std::string base_qname = std::string(templ.name);
            if (templ.type != ty::kInvalidType) {
                base_qname = types_.to_string(templ.type);
            }
            diag_(diag::Code::kGenericTypePathArityMismatch, use_span,
                  base_qname,
                  std::to_string(generic_names.size()),
                  std::to_string(concrete_args.size()));
            err_(use_span, "generic enum arity mismatch");
            return std::nullopt;
        }

        std::ostringstream key_oss;
        key_oss << template_sid << "|";
        for (size_t i = 0; i < concrete_args.size(); ++i) {
            if (i) key_oss << ",";
            key_oss << concrete_args[i];
        }
        const std::string cache_key = key_oss.str();
        if (auto it = generic_enum_instance_cache_.find(cache_key);
            it != generic_enum_instance_cache_.end()) {
            return it->second;
        }

        std::unordered_map<std::string, ty::TypeId> subst;
        subst.reserve(generic_names.size());
        for (size_t i = 0; i < generic_names.size(); ++i) {
            subst.emplace(generic_names[i], concrete_args[i]);
        }

        std::unordered_map<ast::ExprId, ast::ExprId> expr_clone_map;
        std::unordered_map<ast::StmtId, ast::StmtId> stmt_clone_map;
        const ast::StmtId inst_sid = clone_stmt_with_type_subst_(
            template_sid, subst, expr_clone_map, stmt_clone_map);
        if (inst_sid == ast::k_invalid_stmt || (size_t)inst_sid >= ast_.stmts().size()) {
            return std::nullopt;
        }

        auto& inst = ast_.stmt_mut(inst_sid);
        inst.decl_generic_param_begin = 0;
        inst.decl_generic_param_count = 0;
        inst.decl_constraint_begin = 0;
        inst.decl_constraint_count = 0;

        std::string base_qname = std::string(templ.name);
        if (templ.type != ty::kInvalidType) {
            base_qname = types_.to_string(templ.type);
        }
        std::ostringstream qn;
        qn << base_qname << "<";
        for (size_t i = 0; i < concrete_args.size(); ++i) {
            if (i) qn << ",";
            qn << types_.to_string(concrete_args[i]);
        }
        qn << ">";
        const std::string inst_qname = qn.str();
        const ty::TypeId inst_type = types_.intern_ident(ast_.add_owned_string(inst_qname));
        inst.name = ast_.add_owned_string(inst_qname);
        inst.type = inst_type;

        enum_decl_by_name_[inst_qname] = inst_sid;
        enum_decl_by_type_[inst_type] = inst_sid;

        if (auto existing = sym_.lookup_in_current(inst_qname)) {
            (void)sym_.update_declared_type(*existing, inst_type);
        } else {
            (void)sym_.insert(sema::SymbolKind::kType, inst_qname, inst_type, inst.span);
        }

        const size_t expr_size = ast_.exprs().size();
        if (expr_type_cache_.size() < expr_size) expr_type_cache_.resize(expr_size, ty::kInvalidType);
        if (expr_overload_target_cache_.size() < expr_size) expr_overload_target_cache_.resize(expr_size, ast::k_invalid_stmt);
        if (expr_ctor_owner_type_cache_.size() < expr_size) expr_ctor_owner_type_cache_.resize(expr_size, ty::kInvalidType);
        if (expr_enum_ctor_owner_type_cache_.size() < expr_size) expr_enum_ctor_owner_type_cache_.resize(expr_size, ty::kInvalidType);
        if (expr_enum_ctor_variant_index_cache_.size() < expr_size) expr_enum_ctor_variant_index_cache_.resize(expr_size, 0xFFFF'FFFFu);
        if (expr_enum_ctor_tag_value_cache_.size() < expr_size) expr_enum_ctor_tag_value_cache_.resize(expr_size, 0);
        if (expr_resolved_symbol_cache_.size() < expr_size) expr_resolved_symbol_cache_.resize(expr_size, sema::SymbolTable::kNoScope);
        if (expr_proto_const_decl_cache_.size() < expr_size) expr_proto_const_decl_cache_.resize(expr_size, ast::k_invalid_stmt);
        if (expr_fstring_runtime_expr_cache_.size() < expr_size) expr_fstring_runtime_expr_cache_.resize(expr_size, ast::k_invalid_expr);
        const size_t param_size = ast_.params().size();
        if (param_resolved_symbol_cache_.size() < param_size) param_resolved_symbol_cache_.resize(param_size, sema::SymbolTable::kNoScope);

        generic_enum_instance_cache_[cache_key] = inst_sid;
        generic_instantiated_enum_sids_.push_back(inst_sid);
        if (generic_decl_checked_instances_.find(inst_sid) == generic_decl_checked_instances_.end() &&
            pending_generic_decl_instance_enqueued_.insert(inst_sid).second) {
            pending_generic_decl_instance_queue_.push_back(inst_sid);
        }
        return inst_sid;
    }

    std::optional<ast::StmtId> TypeChecker::ensure_generic_enum_instance_from_type_(
        ty::TypeId maybe_generic_enum_type,
        Span use_span
    ) {
        if (maybe_generic_enum_type == ty::kInvalidType) return std::nullopt;

        std::string base;
        std::vector<ty::TypeId> args;
        if (!decompose_named_user_type_(maybe_generic_enum_type, base, args) || args.empty()) {
            auto it = enum_decl_by_type_.find(maybe_generic_enum_type);
            if (it != enum_decl_by_type_.end()) return it->second;
            return std::nullopt;
        }

        std::string base_key = base;
        if (auto rewritten = rewrite_imported_path_(base_key)) {
            base_key = *rewritten;
        }

        auto templ_it = enum_decl_by_name_.find(base_key);
        if (templ_it == enum_decl_by_name_.end()) {
            return std::nullopt;
        }
        const ast::StmtId templ_sid = templ_it->second;
        if (templ_sid == ast::k_invalid_stmt || (size_t)templ_sid >= ast_.stmts().size()) {
            return std::nullopt;
        }
        const auto& templ = ast_.stmt(templ_sid);
        if (templ.kind != ast::StmtKind::kEnumDecl) {
            return std::nullopt;
        }
        if (templ.decl_generic_param_count == 0) {
            return templ_sid;
        }
        return ensure_generic_enum_instance_(templ_sid, args, use_span);
    }

    std::optional<ast::StmtId> TypeChecker::ensure_generic_class_instance_(
        ast::StmtId template_sid,
        const std::vector<ty::TypeId>& concrete_args,
        Span use_span
    ) {
        if (template_sid == ast::k_invalid_stmt || (size_t)template_sid >= ast_.stmts().size()) {
            return std::nullopt;
        }
        const ast::Stmt templ = ast_.stmt(template_sid);
        if (templ.kind != ast::StmtKind::kClassDecl || templ.decl_generic_param_count == 0) {
            return template_sid;
        }

        const auto generic_names = collect_decl_generic_param_names_(templ);
        if (generic_names.size() != concrete_args.size()) {
            const std::string base_qname =
                (class_qualified_name_by_stmt_.find(template_sid) != class_qualified_name_by_stmt_.end())
                    ? class_qualified_name_by_stmt_[template_sid]
                    : std::string(templ.name);
            diag_(diag::Code::kGenericTypePathArityMismatch, use_span,
                  base_qname,
                  std::to_string(generic_names.size()),
                  std::to_string(concrete_args.size()));
            err_(use_span, "generic class arity mismatch");
            return std::nullopt;
        }

        std::ostringstream key_oss;
        key_oss << template_sid << "|";
        for (size_t i = 0; i < concrete_args.size(); ++i) {
            if (i) key_oss << ",";
            key_oss << concrete_args[i];
        }
        const std::string cache_key = key_oss.str();
        if (auto it = generic_class_instance_cache_.find(cache_key);
            it != generic_class_instance_cache_.end()) {
            return it->second;
        }

        std::string base_qname = std::string(templ.name);
        if (auto it = class_qualified_name_by_stmt_.find(template_sid);
            it != class_qualified_name_by_stmt_.end()) {
            base_qname = it->second;
        }
        std::ostringstream qn;
        qn << base_qname << "<";
        for (size_t i = 0; i < concrete_args.size(); ++i) {
            if (i) qn << ",";
            qn << types_.to_string(concrete_args[i]);
        }
        qn << ">";
        const std::string inst_qname = qn.str();
        const ty::TypeId inst_type = types_.intern_ident(ast_.add_owned_string(inst_qname));

        std::unordered_map<std::string, ty::TypeId> subst;
        subst.reserve(generic_names.size() + 8);
        for (size_t i = 0; i < generic_names.size(); ++i) {
            subst.emplace(generic_names[i], concrete_args[i]);
        }
        auto add_subst_alias = [&](const std::string& key, ty::TypeId val) {
            if (key.empty()) return;
            subst.try_emplace(key, val);
            const size_t sep = key.rfind("::");
            if (sep != std::string::npos && sep + 2 < key.size()) {
                subst.try_emplace(key.substr(sep + 2), val);
            }
        };
        add_subst_alias("Self", inst_type);
        add_subst_alias(std::string(templ.name), inst_type);
        add_subst_alias(base_qname, inst_type);
        if (templ.type != ty::kInvalidType) {
            add_subst_alias(types_.to_string(templ.type), inst_type);
        }

        auto resolve_proto_sid_for_constraint = [&](std::string_view raw) -> std::optional<ast::StmtId> {
            if (raw.empty()) return std::nullopt;
            std::string key(raw);
            if (auto rewritten = rewrite_imported_path_(key)) key = *rewritten;
            auto it = proto_decl_by_name_.find(key);
            if (it != proto_decl_by_name_.end()) return it->second;
            if (auto sym_sid = lookup_symbol_(key)) {
                const auto& ss = sym_.symbol(*sym_sid);
                auto pit = proto_decl_by_name_.find(ss.name);
                if (pit != proto_decl_by_name_.end()) return pit->second;
            }
            return std::nullopt;
        };

        auto fn_sig_same = [&](const ast::Stmt& a, const ast::Stmt& b) -> bool {
            if (a.kind != ast::StmtKind::kFnDecl || b.kind != ast::StmtKind::kFnDecl) return false;
            if (a.name != b.name) return false;
            if (a.param_count != b.param_count) return false;
            if (a.positional_param_count != b.positional_param_count) return false;
            if (a.fn_ret != b.fn_ret) return false;
            for (uint32_t i = 0; i < a.param_count; ++i) {
                const auto& ap = ast_.params()[a.param_begin + i];
                const auto& bp = ast_.params()[b.param_begin + i];
                if (ap.type != bp.type || ap.is_self != bp.is_self || ap.self_kind != bp.self_kind) return false;
            }
            return true;
        };

        auto proto_effective_required_empty = [&](ast::StmtId proto_sid) -> bool {
            if (proto_sid == ast::k_invalid_stmt || (size_t)proto_sid >= ast_.stmts().size()) return false;
            std::vector<ast::StmtId> reqs;
            std::vector<ast::StmtId> provs;
            std::unordered_set<ast::StmtId> visiting;
            auto collect = [&](auto&& self, ast::StmtId cur_sid) -> void {
                if (cur_sid == ast::k_invalid_stmt || (size_t)cur_sid >= ast_.stmts().size()) return;
                if (!visiting.insert(cur_sid).second) return;
                const auto& cur = ast_.stmt(cur_sid);
                if (cur.kind != ast::StmtKind::kProtoDecl) return;
                const auto& refs = ast_.path_refs();
                const uint64_t ib = cur.decl_path_ref_begin;
                const uint64_t ie = ib + cur.decl_path_ref_count;
                if (ib <= refs.size() && ie <= refs.size()) {
                    for (uint32_t i = cur.decl_path_ref_begin; i < cur.decl_path_ref_begin + cur.decl_path_ref_count; ++i) {
                        if (auto base_sid = resolve_proto_decl_from_path_ref_(refs[i], use_span)) {
                            self(self, *base_sid);
                        }
                    }
                }
                const auto& kids = ast_.stmt_children();
                const uint64_t mb = cur.stmt_begin;
                const uint64_t me = mb + cur.stmt_count;
                if (mb <= kids.size() && me <= kids.size()) {
                    for (uint32_t i = cur.stmt_begin; i < cur.stmt_begin + cur.stmt_count; ++i) {
                        const auto msid = kids[i];
                        if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                        const auto& m = ast_.stmt(msid);
                        if (m.kind != ast::StmtKind::kFnDecl) continue;
                        if (m.proto_fn_role == ast::ProtoFnRole::kRequire) reqs.push_back(msid);
                        if (m.proto_fn_role == ast::ProtoFnRole::kProvide && m.a != ast::k_invalid_stmt) provs.push_back(msid);
                    }
                }
            };
            collect(collect, proto_sid);
            for (const auto req_sid : reqs) {
                if (req_sid == ast::k_invalid_stmt || (size_t)req_sid >= ast_.stmts().size()) continue;
                const auto& req = ast_.stmt(req_sid);
                bool satisfied = false;
                for (const auto prov_sid : provs) {
                    if (prov_sid == ast::k_invalid_stmt || (size_t)prov_sid >= ast_.stmts().size()) continue;
                    if (fn_sig_same(req, ast_.stmt(prov_sid))) {
                        satisfied = true;
                        break;
                    }
                }
                if (!satisfied) return false;
            }
            return true;
        };

        auto type_satisfies_proto_constraint = [&](ty::TypeId concrete_t, ast::StmtId proto_sid) -> bool {
            if (proto_sid == ast::k_invalid_stmt) return false;
            if (!evaluate_proto_require_at_apply_(proto_sid, concrete_t, use_span,
                                                  /*emit_unsatisfied_diag=*/false,
                                                  /*emit_shape_diag=*/false)) {
                return false;
            }
            if (proto_effective_required_empty(proto_sid)) return true;

            ast::StmtId owner_sid = ast::k_invalid_stmt;
            if (auto cit = class_decl_by_type_.find(concrete_t); cit != class_decl_by_type_.end()) {
                owner_sid = cit->second;
            } else if (auto fit = field_abi_meta_by_type_.find(concrete_t); fit != field_abi_meta_by_type_.end()) {
                owner_sid = fit->second.sid;
            }
            if (owner_sid == ast::k_invalid_stmt || (size_t)owner_sid >= ast_.stmts().size()) return false;
            const auto& owner = ast_.stmt(owner_sid);
            if (owner.kind != ast::StmtKind::kClassDecl && owner.kind != ast::StmtKind::kFieldDecl) return false;

            const auto& refs = ast_.path_refs();
            const uint64_t begin = owner.decl_path_ref_begin;
            const uint64_t end = begin + owner.decl_path_ref_count;
            if (begin > refs.size() || end > refs.size()) return false;
            for (uint32_t i = owner.decl_path_ref_begin; i < owner.decl_path_ref_begin + owner.decl_path_ref_count; ++i) {
                if (auto psid = resolve_proto_decl_from_path_ref_(refs[i], use_span)) {
                    if (*psid == proto_sid) return true;
                }
            }
            return false;
        };

        for (uint32_t ci = 0; ci < templ.decl_constraint_count; ++ci) {
            const uint32_t idx = templ.decl_constraint_begin + ci;
            if (idx >= ast_.fn_constraint_decls().size()) break;
            const auto& cc = ast_.fn_constraint_decls()[idx];

            auto bit = subst.find(std::string(cc.type_param));
            if (bit == subst.end()) {
                diag_(diag::Code::kGenericUnknownTypeParamInConstraint, cc.span, cc.type_param);
                err_(cc.span, "declaration constraint references unknown generic parameter");
                return std::nullopt;
            }

            const std::string proto_path = path_join_(cc.proto_path_begin, cc.proto_path_count);
            auto proto_sid = resolve_proto_sid_for_constraint(proto_path);
            if (!proto_sid.has_value()) {
                diag_(diag::Code::kGenericConstraintProtoNotFound, cc.span, proto_path);
                err_(cc.span, "declaration constraint references unknown proto");
                return std::nullopt;
            }
            if (!type_satisfies_proto_constraint(bit->second, *proto_sid)) {
                diag_(diag::Code::kGenericDeclConstraintUnsatisfied, cc.span,
                      cc.type_param, proto_path, types_.to_string(bit->second));
                err_(cc.span, "declaration generic constraint unsatisfied");
                return std::nullopt;
            }
        }

        std::unordered_map<ast::ExprId, ast::ExprId> expr_clone_map;
        std::unordered_map<ast::StmtId, ast::StmtId> stmt_clone_map;
        const ast::StmtId inst_sid = clone_stmt_with_type_subst_(
            template_sid, subst, expr_clone_map, stmt_clone_map);
        if (inst_sid == ast::k_invalid_stmt || (size_t)inst_sid >= ast_.stmts().size()) {
            return std::nullopt;
        }
        auto& inst = ast_.stmt_mut(inst_sid);
        inst.decl_generic_param_begin = 0;
        inst.decl_generic_param_count = 0;
        inst.decl_constraint_begin = 0;
        inst.decl_constraint_count = 0;

        inst.type = inst_type;

        class_qualified_name_by_stmt_[inst_sid] = inst_qname;
        class_decl_by_name_[inst_qname] = inst_sid;
        class_decl_by_type_[inst_type] = inst_sid;
        field_abi_meta_by_type_[inst_type] = FieldAbiMeta{
            .sid = inst_sid,
            .layout = ast::FieldLayout::kNone,
            .align = 0,
        };

        const auto& kids = ast_.stmt_children();
        const uint64_t begin = inst.stmt_begin;
        const uint64_t end = begin + inst.stmt_count;
        if (begin <= kids.size() && end <= kids.size()) {
            for (uint32_t i = inst.stmt_begin; i < inst.stmt_begin + inst.stmt_count; ++i) {
                const ast::StmtId msid = kids[i];
                if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                const auto& m = ast_.stmt(msid);
                if (m.kind == ast::StmtKind::kFnDecl) {
                    if (m.fn_generic_param_count > 0) {
                        generic_fn_template_sid_set_.insert(msid);
                    }
                    class_member_fn_sid_set_.insert(msid);

                    std::string mqname = inst_qname;
                    if (!mqname.empty()) mqname += "::";
                    mqname += std::string(m.name);
                    fn_qualified_name_by_stmt_[msid] = mqname;
                    fn_decl_by_name_[mqname].push_back(msid);
                    if (!m.is_static) {
                        class_effective_method_map_[inst_type][std::string(m.name)].push_back(msid);
                    }

                    if (auto existing = sym_.lookup_in_current(mqname)) {
                        (void)sym_.update_declared_type(*existing, m.type);
                    } else {
                        (void)sym_.insert(sema::SymbolKind::kFn, mqname, m.type, m.span);
                    }
                } else if (m.kind == ast::StmtKind::kVar && m.is_static) {
                    std::string vqname = inst_qname;
                    if (!vqname.empty()) vqname += "::";
                    vqname += std::string(m.name);
                    const ty::TypeId vt = (m.type == ty::kInvalidType) ? types_.error() : m.type;
                    if (auto existing = sym_.lookup_in_current(vqname)) {
                        (void)sym_.update_declared_type(*existing, vt);
                    } else {
                        (void)sym_.insert(sema::SymbolKind::kVar, vqname, vt, m.span);
                    }
                }
            }
        }

        const size_t expr_size = ast_.exprs().size();
        if (expr_type_cache_.size() < expr_size) expr_type_cache_.resize(expr_size, ty::kInvalidType);
        if (expr_overload_target_cache_.size() < expr_size) expr_overload_target_cache_.resize(expr_size, ast::k_invalid_stmt);
        if (expr_ctor_owner_type_cache_.size() < expr_size) expr_ctor_owner_type_cache_.resize(expr_size, ty::kInvalidType);
        if (expr_enum_ctor_owner_type_cache_.size() < expr_size) expr_enum_ctor_owner_type_cache_.resize(expr_size, ty::kInvalidType);
        if (expr_enum_ctor_variant_index_cache_.size() < expr_size) expr_enum_ctor_variant_index_cache_.resize(expr_size, 0xFFFF'FFFFu);
        if (expr_enum_ctor_tag_value_cache_.size() < expr_size) expr_enum_ctor_tag_value_cache_.resize(expr_size, 0);
        if (expr_resolved_symbol_cache_.size() < expr_size) expr_resolved_symbol_cache_.resize(expr_size, sema::SymbolTable::kNoScope);
        if (expr_proto_const_decl_cache_.size() < expr_size) expr_proto_const_decl_cache_.resize(expr_size, ast::k_invalid_stmt);
        if (expr_fstring_runtime_expr_cache_.size() < expr_size) expr_fstring_runtime_expr_cache_.resize(expr_size, ast::k_invalid_expr);
        const size_t param_size = ast_.params().size();
        if (param_resolved_symbol_cache_.size() < param_size) param_resolved_symbol_cache_.resize(param_size, sema::SymbolTable::kNoScope);

        generic_class_instance_cache_[cache_key] = inst_sid;
        generic_instantiated_class_sids_.push_back(inst_sid);
        if (generic_decl_checked_instances_.find(inst_sid) == generic_decl_checked_instances_.end() &&
            pending_generic_decl_instance_enqueued_.insert(inst_sid).second) {
            pending_generic_decl_instance_queue_.push_back(inst_sid);
        }
        return inst_sid;
    }

    std::optional<ast::StmtId> TypeChecker::ensure_generic_proto_instance_(
        ast::StmtId template_sid,
        const std::vector<ty::TypeId>& concrete_args,
        Span use_span
    ) {
        if (template_sid == ast::k_invalid_stmt || (size_t)template_sid >= ast_.stmts().size()) {
            return std::nullopt;
        }
        const ast::Stmt templ = ast_.stmt(template_sid);
        if (templ.kind != ast::StmtKind::kProtoDecl || templ.decl_generic_param_count == 0) {
            return template_sid;
        }

        const auto generic_names = collect_decl_generic_param_names_(templ);
        if (generic_names.size() != concrete_args.size()) {
            const std::string base_qname =
                (proto_qualified_name_by_stmt_.find(template_sid) != proto_qualified_name_by_stmt_.end())
                    ? proto_qualified_name_by_stmt_[template_sid]
                    : std::string(templ.name);
            diag_(diag::Code::kGenericTypePathArityMismatch, use_span,
                  base_qname,
                  std::to_string(generic_names.size()),
                  std::to_string(concrete_args.size()));
            err_(use_span, "generic proto arity mismatch");
            return std::nullopt;
        }

        std::ostringstream key_oss;
        key_oss << template_sid << "|";
        for (size_t i = 0; i < concrete_args.size(); ++i) {
            if (i) key_oss << ",";
            key_oss << concrete_args[i];
        }
        const std::string cache_key = key_oss.str();
        if (auto it = generic_proto_instance_cache_.find(cache_key);
            it != generic_proto_instance_cache_.end()) {
            return it->second;
        }

        std::unordered_map<std::string, ty::TypeId> subst;
        subst.reserve(generic_names.size());
        for (size_t i = 0; i < generic_names.size(); ++i) {
            subst.emplace(generic_names[i], concrete_args[i]);
        }

        std::unordered_map<ast::ExprId, ast::ExprId> expr_clone_map;
        std::unordered_map<ast::StmtId, ast::StmtId> stmt_clone_map;
        const ast::StmtId inst_sid = clone_stmt_with_type_subst_(
            template_sid, subst, expr_clone_map, stmt_clone_map);
        if (inst_sid == ast::k_invalid_stmt || (size_t)inst_sid >= ast_.stmts().size()) {
            return std::nullopt;
        }
        auto& inst = ast_.stmt_mut(inst_sid);
        inst.decl_generic_param_begin = 0;
        inst.decl_generic_param_count = 0;
        inst.decl_constraint_begin = 0;
        inst.decl_constraint_count = 0;

        std::string base_qname = std::string(templ.name);
        if (auto it = proto_qualified_name_by_stmt_.find(template_sid);
            it != proto_qualified_name_by_stmt_.end()) {
            base_qname = it->second;
        }
        std::ostringstream qn;
        qn << base_qname << "<";
        for (size_t i = 0; i < concrete_args.size(); ++i) {
            if (i) qn << ",";
            qn << types_.to_string(concrete_args[i]);
        }
        qn << ">";
        const std::string inst_qname = qn.str();
        const ty::TypeId inst_type = types_.intern_ident(ast_.add_owned_string(inst_qname));
        inst.type = inst_type;

        proto_qualified_name_by_stmt_[inst_sid] = inst_qname;
        proto_decl_by_name_[inst_qname] = inst_sid;
        proto_decl_by_type_[inst_type] = inst_sid;

        const auto& kids = ast_.stmt_children();
        const uint64_t begin = inst.stmt_begin;
        const uint64_t end = begin + inst.stmt_count;
        if (begin <= kids.size() && end <= kids.size()) {
            for (uint32_t i = inst.stmt_begin; i < inst.stmt_begin + inst.stmt_count; ++i) {
                const ast::StmtId msid = kids[i];
                if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                const auto& m = ast_.stmt(msid);
                if (m.kind != ast::StmtKind::kFnDecl) continue;
                if (m.fn_generic_param_count > 0) {
                    generic_fn_template_sid_set_.insert(msid);
                }
                proto_member_fn_sid_set_.insert(msid);

                std::string mqname = inst_qname;
                if (!mqname.empty()) mqname += "::";
                mqname += std::string(m.name);
                fn_qualified_name_by_stmt_[msid] = mqname;

                if (m.a != ast::k_invalid_stmt) {
                    fn_decl_by_name_[mqname].push_back(msid);
                    if (auto existing = sym_.lookup_in_current(mqname)) {
                        (void)sym_.update_declared_type(*existing, m.type);
                    } else {
                        (void)sym_.insert(sema::SymbolKind::kFn, mqname, m.type, m.span);
                    }
                }
            }
        }

        const size_t expr_size = ast_.exprs().size();
        if (expr_type_cache_.size() < expr_size) expr_type_cache_.resize(expr_size, ty::kInvalidType);
        if (expr_overload_target_cache_.size() < expr_size) expr_overload_target_cache_.resize(expr_size, ast::k_invalid_stmt);
        if (expr_ctor_owner_type_cache_.size() < expr_size) expr_ctor_owner_type_cache_.resize(expr_size, ty::kInvalidType);
        if (expr_enum_ctor_owner_type_cache_.size() < expr_size) expr_enum_ctor_owner_type_cache_.resize(expr_size, ty::kInvalidType);
        if (expr_enum_ctor_variant_index_cache_.size() < expr_size) expr_enum_ctor_variant_index_cache_.resize(expr_size, 0xFFFF'FFFFu);
        if (expr_enum_ctor_tag_value_cache_.size() < expr_size) expr_enum_ctor_tag_value_cache_.resize(expr_size, 0);
        if (expr_resolved_symbol_cache_.size() < expr_size) expr_resolved_symbol_cache_.resize(expr_size, sema::SymbolTable::kNoScope);
        if (expr_proto_const_decl_cache_.size() < expr_size) expr_proto_const_decl_cache_.resize(expr_size, ast::k_invalid_stmt);
        if (expr_fstring_runtime_expr_cache_.size() < expr_size) expr_fstring_runtime_expr_cache_.resize(expr_size, ast::k_invalid_expr);
        const size_t param_size = ast_.params().size();
        if (param_resolved_symbol_cache_.size() < param_size) param_resolved_symbol_cache_.resize(param_size, sema::SymbolTable::kNoScope);

        generic_proto_instance_cache_[cache_key] = inst_sid;
        generic_instantiated_proto_sids_.push_back(inst_sid);
        if (generic_decl_checked_instances_.find(inst_sid) == generic_decl_checked_instances_.end() &&
            pending_generic_decl_instance_enqueued_.insert(inst_sid).second) {
            pending_generic_decl_instance_queue_.push_back(inst_sid);
        }
        return inst_sid;
    }

    std::optional<ast::StmtId> TypeChecker::ensure_generic_acts_instance_(
        ast::StmtId template_sid,
        ty::TypeId concrete_owner_type,
        const std::vector<ty::TypeId>& concrete_args,
        Span use_span
    ) {
        if (template_sid == ast::k_invalid_stmt || (size_t)template_sid >= ast_.stmts().size()) {
            return std::nullopt;
        }
        concrete_owner_type = canonicalize_acts_owner_type_(concrete_owner_type);
        const ast::Stmt templ = ast_.stmt(template_sid);
        if (templ.kind != ast::StmtKind::kActsDecl || !templ.acts_is_for) {
            return std::nullopt;
        }

        std::string owner_base;
        std::vector<ty::TypeId> owner_generic_params;
        if (!decompose_named_user_type_(templ.acts_target_type, owner_base, owner_generic_params)) {
            return template_sid;
        }
        if (owner_generic_params.size() != concrete_args.size()) {
            diag_(diag::Code::kGenericTypePathArityMismatch, use_span,
                  owner_base,
                  std::to_string(owner_generic_params.size()),
                  std::to_string(concrete_args.size()));
            err_(use_span, "generic acts owner arity mismatch");
            return std::nullopt;
        }

        std::vector<std::string> generic_names;
        generic_names.reserve(owner_generic_params.size());
        for (const auto t : owner_generic_params) {
            generic_names.push_back(types_.to_string(t));
        }

        std::ostringstream key_oss;
        key_oss << template_sid << "|" << concrete_owner_type << "|";
        for (size_t i = 0; i < concrete_args.size(); ++i) {
            if (i) key_oss << ",";
            key_oss << concrete_args[i];
        }
        const std::string cache_key = key_oss.str();
        if (auto it = generic_acts_instance_cache_.find(cache_key);
            it != generic_acts_instance_cache_.end()) {
            return it->second;
        }

        std::unordered_map<std::string, ty::TypeId> subst;
        subst.reserve(generic_names.size());
        for (size_t i = 0; i < generic_names.size(); ++i) {
            subst.emplace(generic_names[i], concrete_args[i]);
        }

        auto resolve_proto_sid_for_constraint = [&](std::string_view raw) -> std::optional<ast::StmtId> {
            if (raw.empty()) return std::nullopt;
            std::string key(raw);
            if (auto rewritten = rewrite_imported_path_(key)) key = *rewritten;
            auto it = proto_decl_by_name_.find(key);
            if (it != proto_decl_by_name_.end()) return it->second;
            if (auto sym_sid = lookup_symbol_(key)) {
                const auto& ss = sym_.symbol(*sym_sid);
                auto pit = proto_decl_by_name_.find(ss.name);
                if (pit != proto_decl_by_name_.end()) return pit->second;
            }
            return std::nullopt;
        };

        auto fn_sig_same = [&](const ast::Stmt& a, const ast::Stmt& b) -> bool {
            if (a.kind != ast::StmtKind::kFnDecl || b.kind != ast::StmtKind::kFnDecl) return false;
            if (a.name != b.name) return false;
            if (a.param_count != b.param_count) return false;
            if (a.positional_param_count != b.positional_param_count) return false;
            if (a.fn_ret != b.fn_ret) return false;
            for (uint32_t i = 0; i < a.param_count; ++i) {
                const auto& ap = ast_.params()[a.param_begin + i];
                const auto& bp = ast_.params()[b.param_begin + i];
                if (ap.type != bp.type || ap.is_self != bp.is_self || ap.self_kind != bp.self_kind) return false;
            }
            return true;
        };

        auto proto_effective_required_empty = [&](ast::StmtId proto_sid) -> bool {
            if (proto_sid == ast::k_invalid_stmt || (size_t)proto_sid >= ast_.stmts().size()) return false;
            std::vector<ast::StmtId> reqs;
            std::vector<ast::StmtId> provs;
            std::unordered_set<ast::StmtId> visiting;
            auto collect = [&](auto&& self, ast::StmtId cur_sid) -> void {
                if (cur_sid == ast::k_invalid_stmt || (size_t)cur_sid >= ast_.stmts().size()) return;
                if (!visiting.insert(cur_sid).second) return;
                const auto& cur = ast_.stmt(cur_sid);
                if (cur.kind != ast::StmtKind::kProtoDecl) return;
                const auto& refs = ast_.path_refs();
                const uint64_t ib = cur.decl_path_ref_begin;
                const uint64_t ie = ib + cur.decl_path_ref_count;
                if (ib <= refs.size() && ie <= refs.size()) {
                    for (uint32_t i = cur.decl_path_ref_begin; i < cur.decl_path_ref_begin + cur.decl_path_ref_count; ++i) {
                        if (auto base_sid = resolve_proto_decl_from_path_ref_(refs[i], use_span)) {
                            self(self, *base_sid);
                        }
                    }
                }
                const auto& kids = ast_.stmt_children();
                const uint64_t mb = cur.stmt_begin;
                const uint64_t me = mb + cur.stmt_count;
                if (mb <= kids.size() && me <= kids.size()) {
                    for (uint32_t i = cur.stmt_begin; i < cur.stmt_begin + cur.stmt_count; ++i) {
                        const auto msid = kids[i];
                        if (msid == ast::k_invalid_stmt || (size_t)msid >= ast_.stmts().size()) continue;
                        const auto& m = ast_.stmt(msid);
                        if (m.kind != ast::StmtKind::kFnDecl) continue;
                        if (m.proto_fn_role == ast::ProtoFnRole::kRequire) reqs.push_back(msid);
                        if (m.proto_fn_role == ast::ProtoFnRole::kProvide && m.a != ast::k_invalid_stmt) provs.push_back(msid);
                    }
                }
            };
            collect(collect, proto_sid);
            for (const auto req_sid : reqs) {
                if (req_sid == ast::k_invalid_stmt || (size_t)req_sid >= ast_.stmts().size()) continue;
                const auto& req = ast_.stmt(req_sid);
                bool satisfied = false;
                for (const auto prov_sid : provs) {
                    if (prov_sid == ast::k_invalid_stmt || (size_t)prov_sid >= ast_.stmts().size()) continue;
                    if (fn_sig_same(req, ast_.stmt(prov_sid))) {
                        satisfied = true;
                        break;
                    }
                }
                if (!satisfied) return false;
            }
            return true;
        };

        auto type_satisfies_proto_constraint = [&](ty::TypeId concrete_t, ast::StmtId proto_sid) -> bool {
            if (proto_sid == ast::k_invalid_stmt) return false;
            if (!evaluate_proto_require_at_apply_(proto_sid, concrete_t, use_span,
                                                  /*emit_unsatisfied_diag=*/false,
                                                  /*emit_shape_diag=*/false)) {
                return false;
            }
            if (proto_effective_required_empty(proto_sid)) return true;

            ast::StmtId owner_sid = ast::k_invalid_stmt;
            if (auto cit = class_decl_by_type_.find(concrete_t); cit != class_decl_by_type_.end()) {
                owner_sid = cit->second;
            } else if (auto fit = field_abi_meta_by_type_.find(concrete_t); fit != field_abi_meta_by_type_.end()) {
                owner_sid = fit->second.sid;
            }
            if (owner_sid == ast::k_invalid_stmt || (size_t)owner_sid >= ast_.stmts().size()) return false;
            const auto& owner = ast_.stmt(owner_sid);
            if (owner.kind != ast::StmtKind::kClassDecl && owner.kind != ast::StmtKind::kFieldDecl) return false;

            const auto& refs = ast_.path_refs();
            const uint64_t begin = owner.decl_path_ref_begin;
            const uint64_t end = begin + owner.decl_path_ref_count;
            if (begin > refs.size() || end > refs.size()) return false;
            for (uint32_t i = owner.decl_path_ref_begin; i < owner.decl_path_ref_begin + owner.decl_path_ref_count; ++i) {
                if (auto psid = resolve_proto_decl_from_path_ref_(refs[i], use_span)) {
                    if (*psid == proto_sid) return true;
                }
            }
            return false;
        };

        for (uint32_t ci = 0; ci < templ.decl_constraint_count; ++ci) {
            const uint32_t idx = templ.decl_constraint_begin + ci;
            if (idx >= ast_.fn_constraint_decls().size()) break;
            const auto& cc = ast_.fn_constraint_decls()[idx];

            auto bit = subst.find(std::string(cc.type_param));
            if (bit == subst.end()) {
                diag_(diag::Code::kGenericUnknownTypeParamInConstraint, cc.span, cc.type_param);
                err_(cc.span, "acts declaration constraint references unknown generic parameter");
                return std::nullopt;
            }

            const std::string proto_path = path_join_(cc.proto_path_begin, cc.proto_path_count);
            auto proto_sid = resolve_proto_sid_for_constraint(proto_path);
            if (!proto_sid.has_value()) {
                diag_(diag::Code::kGenericConstraintProtoNotFound, cc.span, proto_path);
                err_(cc.span, "acts declaration constraint references unknown proto");
                return std::nullopt;
            }
            if (!type_satisfies_proto_constraint(bit->second, *proto_sid)) {
                diag_(diag::Code::kGenericDeclConstraintUnsatisfied, cc.span,
                      cc.type_param, proto_path, types_.to_string(bit->second));
                err_(cc.span, "acts declaration generic constraint unsatisfied");
                return std::nullopt;
            }
        }

        std::unordered_map<ast::ExprId, ast::ExprId> expr_clone_map;
        std::unordered_map<ast::StmtId, ast::StmtId> stmt_clone_map;
        const ast::StmtId inst_sid = clone_stmt_with_type_subst_(
            template_sid, subst, expr_clone_map, stmt_clone_map);
        if (inst_sid == ast::k_invalid_stmt || (size_t)inst_sid >= ast_.stmts().size()) {
            return std::nullopt;
        }

        auto& inst = ast_.stmt_mut(inst_sid);
        inst.acts_target_type = concrete_owner_type;
        inst.decl_constraint_begin = 0;
        inst.decl_constraint_count = 0;
        inst.decl_generic_param_begin = 0;
        inst.decl_generic_param_count = 0;

        std::string acts_qname = std::string(templ.name);
        if (auto it = acts_qualified_name_by_stmt_.find(template_sid);
            it != acts_qualified_name_by_stmt_.end()) {
            acts_qname = it->second;
        }
        acts_qualified_name_by_stmt_[inst_sid] = acts_qname;

        if (inst.acts_has_set_name) {
            acts_named_decl_by_owner_and_name_[acts_named_decl_key_(concrete_owner_type, acts_qname)] = inst_sid;
        }
        collect_acts_operator_decl_(inst_sid, inst, /*allow_named_set=*/true);
        collect_acts_method_decl_(inst_sid, inst, /*allow_named_set=*/true);

        const size_t expr_size = ast_.exprs().size();
        if (expr_type_cache_.size() < expr_size) expr_type_cache_.resize(expr_size, ty::kInvalidType);
        if (expr_overload_target_cache_.size() < expr_size) expr_overload_target_cache_.resize(expr_size, ast::k_invalid_stmt);
        if (expr_ctor_owner_type_cache_.size() < expr_size) expr_ctor_owner_type_cache_.resize(expr_size, ty::kInvalidType);
        if (expr_enum_ctor_owner_type_cache_.size() < expr_size) expr_enum_ctor_owner_type_cache_.resize(expr_size, ty::kInvalidType);
        if (expr_enum_ctor_variant_index_cache_.size() < expr_size) expr_enum_ctor_variant_index_cache_.resize(expr_size, 0xFFFF'FFFFu);
        if (expr_enum_ctor_tag_value_cache_.size() < expr_size) expr_enum_ctor_tag_value_cache_.resize(expr_size, 0);
        if (expr_resolved_symbol_cache_.size() < expr_size) expr_resolved_symbol_cache_.resize(expr_size, sema::SymbolTable::kNoScope);
        if (expr_proto_const_decl_cache_.size() < expr_size) expr_proto_const_decl_cache_.resize(expr_size, ast::k_invalid_stmt);
        if (expr_fstring_runtime_expr_cache_.size() < expr_size) expr_fstring_runtime_expr_cache_.resize(expr_size, ast::k_invalid_expr);
        const size_t param_size = ast_.params().size();
        if (param_resolved_symbol_cache_.size() < param_size) param_resolved_symbol_cache_.resize(param_size, sema::SymbolTable::kNoScope);

        generic_acts_instance_cache_[cache_key] = inst_sid;
        generic_instantiated_acts_sids_.push_back(inst_sid);
        if (generic_decl_checked_instances_.find(inst_sid) == generic_decl_checked_instances_.end() &&
            pending_generic_decl_instance_enqueued_.insert(inst_sid).second) {
            pending_generic_decl_instance_queue_.push_back(inst_sid);
        }
        return inst_sid;
    }

    void TypeChecker::ensure_generic_acts_for_owner_(ty::TypeId concrete_owner_type, Span use_span) {
        concrete_owner_type = canonicalize_acts_owner_type_(concrete_owner_type);
        if (concrete_owner_type == ty::kInvalidType) return;

        std::string owner_base;
        std::vector<ty::TypeId> owner_args;
        if (!decompose_named_user_type_(concrete_owner_type, owner_base, owner_args)) {
            return;
        }

        std::vector<ast::StmtId> templates;
        templates.reserve(generic_acts_template_sid_set_.size());
        for (const auto sid : generic_acts_template_sid_set_) {
            templates.push_back(sid);
        }
        if (templates.empty()) {
            for (ast::StmtId sid = 0; sid < static_cast<ast::StmtId>(ast_.stmts().size()); ++sid) {
                const auto& cand = ast_.stmt(sid);
                if (cand.kind != ast::StmtKind::kActsDecl || !cand.acts_is_for) continue;
                std::string tmp_base;
                std::vector<ty::TypeId> tmp_args;
                if (!decompose_named_user_type_(cand.acts_target_type, tmp_base, tmp_args)) continue;
                templates.push_back(sid);
            }
        }
        std::sort(templates.begin(), templates.end());

        for (const auto templ_sid : templates) {
            if (templ_sid == ast::k_invalid_stmt || (size_t)templ_sid >= ast_.stmts().size()) continue;
            const auto& templ = ast_.stmt(templ_sid);
            if (templ.kind != ast::StmtKind::kActsDecl || !templ.acts_is_for) continue;

            std::string templ_base;
            std::vector<ty::TypeId> templ_args;
            if (!decompose_named_user_type_(templ.acts_target_type, templ_base, templ_args)) continue;
            if (templ_base != owner_base) continue;
            if (templ_args.size() != owner_args.size()) continue;

            (void)ensure_generic_acts_instance_(templ_sid, concrete_owner_type, owner_args, use_span);
        }
    }

    std::string TypeChecker::path_join_(uint32_t begin, uint32_t count) const {
        if (count == 0) return {};
        const auto& segs = ast_.path_segs();
        if (begin >= segs.size() || begin + count > segs.size()) return {};

        std::string out;
        for (uint32_t i = 0; i < count; ++i) {
            if (i) out += "::";
            out += std::string(segs[begin + i]);
        }
        return out;
    }

    std::string TypeChecker::current_namespace_prefix_() const {
        if (namespace_stack_.empty()) return {};
        std::string out;
        for (size_t i = 0; i < namespace_stack_.size(); ++i) {
            if (i) out += "::";
            out += namespace_stack_[i];
        }
        return out;
    }

    std::string TypeChecker::qualify_decl_name_(std::string_view base_name) const {
        const std::string ns = current_namespace_prefix_();
        if (ns.empty()) return std::string(base_name);
        if (base_name.empty()) return ns;
        return ns + "::" + std::string(base_name);
    }

    void TypeChecker::init_file_namespace_(ast::StmtId program_stmt) {
        namespace_stack_.clear();
        if (program_stmt == ast::k_invalid_stmt) return;
        const auto& prog = ast_.stmt(program_stmt);
        if (prog.kind != ast::StmtKind::kBlock) return;

        for (uint32_t i = 0; i < prog.stmt_count; ++i) {
            const ast::StmtId cid = ast_.stmt_children()[prog.stmt_begin + i];
            if (cid == ast::k_invalid_stmt || (size_t)cid >= ast_.stmts().size()) continue;
            const auto& s = ast_.stmt(cid);
            if (s.kind != ast::StmtKind::kNestDecl || !s.nest_is_file_directive) continue;

            const std::string file_ns = path_join_(s.nest_path_begin, s.nest_path_count);
            if (file_ns.empty()) return;

            size_t pos = 0;
            while (pos < file_ns.size()) {
                size_t next = file_ns.find("::", pos);
                if (next == std::string::npos) {
                    namespace_stack_.push_back(file_ns.substr(pos));
                    break;
                }
                namespace_stack_.push_back(file_ns.substr(pos, next - pos));
                pos = next + 2;
            }
            return;
        }
    }

    std::optional<std::string> TypeChecker::rewrite_imported_path_(std::string_view path) const {
        if (path.empty()) return std::nullopt;

        auto lookup_alias = [&](std::string_view head) -> std::optional<std::string> {
            if (!import_alias_scope_stack_.empty()) {
                const auto& scope = import_alias_scope_stack_.back();
                auto it = scope.find(std::string(head));
                if (it != scope.end()) return it->second;
                return std::nullopt;
            }
            auto it = import_alias_to_path_.find(std::string(head));
            if (it == import_alias_to_path_.end()) return std::nullopt;
            return it->second;
        };

        const size_t pos = path.find("::");
        if (pos == std::string_view::npos) {
            return lookup_alias(path);
        }

        const std::string first(path.substr(0, pos));
        auto resolved = lookup_alias(first);
        if (!resolved.has_value()) return std::nullopt;

        std::string out = *resolved;
        out += path.substr(pos);
        return out;
    }

    std::optional<uint32_t> TypeChecker::lookup_symbol_(std::string_view name) const {
        if (name.empty()) return std::nullopt;

        std::string key(name);
        if (auto rewritten = rewrite_imported_path_(key)) {
            key = *rewritten;
        }

        if (auto sid = sym_.lookup(key)) {
            return sid;
        }

        // nest 경로 상대 해소:
        // - 식별자(`name`)와 경로(`a::b`) 모두 현재 namespace 기준 상대 해소를 시도한다.
        for (size_t depth = namespace_stack_.size(); depth > 0; --depth) {
            std::string qname;
            for (size_t i = 0; i < depth; ++i) {
                if (i) qname += "::";
                qname += namespace_stack_[i];
            }
            qname += "::";
            qname += key;
            if (auto sid = sym_.lookup(qname)) {
                return sid;
            }
        }

        return std::nullopt;
    }

    ty::TypeId TypeChecker::canonicalize_acts_owner_type_(ty::TypeId owner_type) const {
        if (owner_type == ty::kInvalidType) return owner_type;
        const auto& tt = types_.get(owner_type);
        if (tt.kind != ty::Kind::kNamedUser) return owner_type;

        if (auto sid = lookup_symbol_(types_.to_string(owner_type))) {
            const auto& ss = sym_.symbol(*sid);
            if ((ss.kind == sema::SymbolKind::kField || ss.kind == sema::SymbolKind::kType) &&
                ss.declared_type != ty::kInvalidType) {
                return ss.declared_type;
            }
        }
        return owner_type;
    }

    void TypeChecker::push_alias_scope_() {
        if (import_alias_scope_stack_.empty()) {
            import_alias_scope_stack_.push_back(import_alias_to_path_);
            return;
        }
        import_alias_scope_stack_.push_back(import_alias_scope_stack_.back());
    }

    void TypeChecker::pop_alias_scope_() {
        if (!import_alias_scope_stack_.empty()) {
            import_alias_scope_stack_.pop_back();
        }
    }

    bool TypeChecker::define_alias_(
        std::string_view alias,
        std::string_view path,
        Span diag_span,
        bool warn_use_nest_preferred
    ) {
        if (alias.empty() || path.empty()) return false;
        if (import_alias_scope_stack_.empty()) {
            push_alias_scope_();
        }

        auto& scope = import_alias_scope_stack_.back();
        const std::string alias_s(alias);
        const std::string path_s(path);
        auto it = scope.find(alias_s);
        if (it != scope.end() && it->second != path_s) {
            diag_(diag::Code::kTypeErrorGeneric, diag_span,
                  "conflicting use alias in same lexical scope: " + alias_s);
            err_(diag_span, "conflicting use alias: " + alias_s);
            return false;
        }

        scope[alias_s] = path_s;
        if (import_alias_scope_stack_.size() == 1) {
            import_alias_to_path_[alias_s] = path_s;
        }

        if (warn_use_nest_preferred && is_known_namespace_path_(path_s)) {
            warn_(diag::Code::kUseNestAliasPreferred, diag_span, path_s, alias_s);
        }
        return true;
    }

    bool TypeChecker::is_known_namespace_path_(std::string_view path) const {
        if (path.empty()) return false;
        return known_namespace_paths_.find(std::string(path)) != known_namespace_paths_.end();
    }

    void TypeChecker::collect_known_namespace_paths_(ast::StmtId program_stmt) {
        known_namespace_paths_.clear();
        if (program_stmt == ast::k_invalid_stmt) return;
        if ((size_t)program_stmt >= ast_.stmts().size()) return;

        std::vector<std::string> ns_stack = namespace_stack_;

        auto add_ns_prefixes_of_symbol = [&](std::string_view symbol_path) {
            if (symbol_path.empty()) return;
            size_t pos = symbol_path.find("::");
            while (pos != std::string_view::npos) {
                known_namespace_paths_.insert(std::string(symbol_path.substr(0, pos)));
                pos = symbol_path.find("::", pos + 2);
            }
        };

        auto qualify_with_ns_stack = [&](std::string_view base_name) -> std::string {
            if (base_name.empty()) return {};
            if (ns_stack.empty()) return std::string(base_name);
            std::string out;
            for (size_t i = 0; i < ns_stack.size(); ++i) {
                if (i) out += "::";
                out += ns_stack[i];
            }
            out += "::";
            out += std::string(base_name);
            return out;
        };

        auto collect_stmt = [&](auto&& self, ast::StmtId sid) -> void {
            if (sid == ast::k_invalid_stmt || (size_t)sid >= ast_.stmts().size()) return;
            const auto& s = ast_.stmt(sid);

            if (s.kind == ast::StmtKind::kBlock) {
                const auto& kids = ast_.stmt_children();
                const uint64_t begin = s.stmt_begin;
                const uint64_t end = begin + s.stmt_count;
                if (begin <= kids.size() && end <= kids.size()) {
                    for (uint32_t i = 0; i < s.stmt_count; ++i) {
                        self(self, kids[s.stmt_begin + i]);
                    }
                }
                return;
            }

            if (s.kind == ast::StmtKind::kNestDecl) {
                uint32_t pushed = 0;
                const auto& segs = ast_.path_segs();
                const uint64_t begin = s.nest_path_begin;
                const uint64_t end = begin + s.nest_path_count;
                if (begin <= segs.size() && end <= segs.size()) {
                    for (uint32_t i = 0; i < s.nest_path_count; ++i) {
                        ns_stack.push_back(std::string(segs[s.nest_path_begin + i]));
                        ++pushed;
                    }
                }
                if (!ns_stack.empty()) {
                    std::string ns;
                    for (size_t i = 0; i < ns_stack.size(); ++i) {
                        if (i) ns += "::";
                        ns += ns_stack[i];
                        known_namespace_paths_.insert(ns);
                    }
                }
                if (!s.nest_is_file_directive) {
                    self(self, s.a);
                }
                while (pushed > 0) {
                    ns_stack.pop_back();
                    --pushed;
                }
                return;
            }

            if ((s.kind == ast::StmtKind::kFnDecl ||
                 s.kind == ast::StmtKind::kFieldDecl ||
                 s.kind == ast::StmtKind::kEnumDecl ||
                 s.kind == ast::StmtKind::kProtoDecl ||
                 s.kind == ast::StmtKind::kClassDecl ||
                 s.kind == ast::StmtKind::kActorDecl ||
                 s.kind == ast::StmtKind::kActsDecl) &&
                !s.name.empty()) {
                const std::string qname = qualify_with_ns_stack(s.name);
                add_ns_prefixes_of_symbol(qname);
                return;
            }

            if (s.kind == ast::StmtKind::kVar) {
                const bool is_global_decl =
                    s.is_static || s.is_const || s.is_extern || s.is_export || (s.link_abi == ast::LinkAbi::kC);
                if (!is_global_decl || s.name.empty()) return;
                const std::string qname = qualify_with_ns_stack(s.name);
                add_ns_prefixes_of_symbol(qname);
                return;
            }
        };

        collect_stmt(collect_stmt, program_stmt);
    }

    // --------------------
    // pass 1: collect top-level decls
    // --------------------
