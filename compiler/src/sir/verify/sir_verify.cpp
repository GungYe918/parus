// compiler/src/sir/sir_verify.cpp
#include <gaupel/sir/Verify.hpp>

#include <optional>
#include <sstream>
#include <string>
#include <vector>


namespace gaupel::sir {

    static bool valid_value_id_(const Module& m, ValueId id) {
        return id != k_invalid_value && (size_t)id < m.values.size();
    }

    static bool valid_block_id_(const Module& m, BlockId id) {
        return id != k_invalid_block && (size_t)id < m.blocks.size();
    }

    static void push_error_(std::vector<VerifyError>& out, const std::string& msg) {
        out.push_back(VerifyError{msg});
    }

    std::vector<VerifyError> verify_module(const Module& m) {
        std::vector<VerifyError> errs;

        // 1) block stmt range bounds + overlap
        std::vector<int32_t> stmt_owner(m.stmts.size(), -1);
        for (uint32_t bid = 0; bid < (uint32_t)m.blocks.size(); ++bid) {
            const auto& b = m.blocks[bid];
            const uint64_t end = (uint64_t)b.stmt_begin + (uint64_t)b.stmt_count;
            if (end > (uint64_t)m.stmts.size()) {
                std::ostringstream oss;
                oss << "block #" << bid << " has out-of-range stmt slice: begin="
                    << b.stmt_begin << " count=" << b.stmt_count
                    << " (stmts.size=" << m.stmts.size() << ")";
                push_error_(errs, oss.str());
                continue;
            }
            for (uint32_t i = 0; i < b.stmt_count; ++i) {
                const uint32_t sid = b.stmt_begin + i;
                if (stmt_owner[sid] != -1) {
                    std::ostringstream oss;
                    oss << "stmt #" << sid << " belongs to multiple blocks ("
                        << stmt_owner[sid] << ", " << bid << ")";
                    push_error_(errs, oss.str());
                } else {
                    stmt_owner[sid] = (int32_t)bid;
                }
            }
        }

        // 2) function slices / entry block
        for (uint32_t fid = 0; fid < (uint32_t)m.funcs.size(); ++fid) {
            const auto& f = m.funcs[fid];
            if (f.entry != k_invalid_block && !valid_block_id_(m, f.entry)) {
                std::ostringstream oss;
                oss << "func #" << fid << " has invalid entry block id " << f.entry;
                push_error_(errs, oss.str());
            }

            const uint64_t attr_end = (uint64_t)f.attr_begin + (uint64_t)f.attr_count;
            if (attr_end > (uint64_t)m.attrs.size()) {
                std::ostringstream oss;
                oss << "func #" << fid << " has out-of-range attrs slice";
                push_error_(errs, oss.str());
            }

            const uint64_t param_end = (uint64_t)f.param_begin + (uint64_t)f.param_count;
            if (param_end > (uint64_t)m.params.size()) {
                std::ostringstream oss;
                oss << "func #" << fid << " has out-of-range params slice";
                push_error_(errs, oss.str());
            }

            if (f.is_acts_member) {
                if (f.owner_acts == k_invalid_acts || (size_t)f.owner_acts >= m.acts.size()) {
                    std::ostringstream oss;
                    oss << "func #" << fid << " is acts member but owner_acts is invalid";
                    push_error_(errs, oss.str());
                }
            }
        }

        // 2.5) field/acts slices
        for (uint32_t i = 0; i < (uint32_t)m.fields.size(); ++i) {
            const auto& f = m.fields[i];
            const uint64_t mend = (uint64_t)f.member_begin + (uint64_t)f.member_count;
            if (mend > (uint64_t)m.field_members.size()) {
                std::ostringstream oss;
                oss << "field #" << i << " has out-of-range member slice";
                push_error_(errs, oss.str());
            }
        }

        for (uint32_t i = 0; i < (uint32_t)m.acts.size(); ++i) {
            const auto& a = m.acts[i];
            const uint64_t fend = (uint64_t)a.func_begin + (uint64_t)a.func_count;
            if (fend > (uint64_t)m.funcs.size()) {
                std::ostringstream oss;
                oss << "acts #" << i << " has out-of-range function slice";
                push_error_(errs, oss.str());
                continue;
            }

            for (uint32_t k = 0; k < a.func_count; ++k) {
                const uint32_t fid = a.func_begin + k;
                if ((size_t)fid >= m.funcs.size()) break;
                const auto& f = m.funcs[fid];
                if (!f.is_acts_member || f.owner_acts != i) {
                    std::ostringstream oss;
                    oss << "acts #" << i << " function #" << fid << " ownership metadata mismatch";
                    push_error_(errs, oss.str());
                }
            }
        }

        // 3) stmt references
        for (uint32_t sid = 0; sid < (uint32_t)m.stmts.size(); ++sid) {
            const auto& s = m.stmts[sid];

            auto need_value = [&](ValueId v, const char* what) {
                if (v == k_invalid_value) {
                    std::ostringstream oss;
                    oss << "stmt #" << sid << " requires " << what << " value but got invalid id";
                    push_error_(errs, oss.str());
                    return;
                }
                if (!valid_value_id_(m, v)) {
                    std::ostringstream oss;
                    oss << "stmt #" << sid << " has invalid " << what << " value id " << v;
                    push_error_(errs, oss.str());
                }
            };

            switch (s.kind) {
                case StmtKind::kExprStmt:
                    need_value(s.expr, "expr");
                    break;
                case StmtKind::kVarDecl:
                    need_value(s.init, "init");
                    break;
                case StmtKind::kIfStmt:
                    need_value(s.expr, "cond");
                    if (!valid_block_id_(m, s.a)) {
                        std::ostringstream oss;
                        oss << "stmt #" << sid << " if-then has invalid block id " << s.a;
                        push_error_(errs, oss.str());
                    }
                    if (s.b != k_invalid_block && !valid_block_id_(m, s.b)) {
                        std::ostringstream oss;
                        oss << "stmt #" << sid << " if-else has invalid block id " << s.b;
                        push_error_(errs, oss.str());
                    }
                    break;
                case StmtKind::kWhileStmt:
                    need_value(s.expr, "cond");
                    if (!valid_block_id_(m, s.a)) {
                        std::ostringstream oss;
                        oss << "stmt #" << sid << " while-body has invalid block id " << s.a;
                        push_error_(errs, oss.str());
                    }
                    break;
                case StmtKind::kReturn:
                case StmtKind::kBreak:
                    if (s.expr != k_invalid_value && !valid_value_id_(m, s.expr)) {
                        std::ostringstream oss;
                        oss << "stmt #" << sid << " has invalid optional expr value id " << s.expr;
                        push_error_(errs, oss.str());
                    }
                    break;
                default:
                    break;
            }
        }

        // 4) value references
        for (uint32_t vid = 0; vid < (uint32_t)m.values.size(); ++vid) {
            const auto& v = m.values[vid];

            auto need_child = [&](ValueId cid, const char* what) {
                if (!valid_value_id_(m, cid)) {
                    std::ostringstream oss;
                    oss << "value #" << vid << " has invalid " << what << " child value id " << cid;
                    push_error_(errs, oss.str());
                }
            };

            switch (v.kind) {
                case ValueKind::kUnary:
                case ValueKind::kBorrow:
                case ValueKind::kEscape:
                case ValueKind::kPostfixInc:
                case ValueKind::kCast:
                    need_child(v.a, "a");
                    break;

                case ValueKind::kBinary:
                case ValueKind::kAssign:
                case ValueKind::kIndex:
                    need_child(v.a, "a");
                    need_child(v.b, "b");
                    break;

                case ValueKind::kIfExpr:
                    need_child(v.a, "a");
                    need_child(v.b, "b");
                    need_child(v.c, "c");
                    break;

                case ValueKind::kLoopExpr: {
                    if (v.a != k_invalid_value && !valid_value_id_(m, v.a)) {
                        std::ostringstream oss;
                        oss << "value #" << vid << " loop has invalid iter value id " << v.a;
                        push_error_(errs, oss.str());
                    }
                    const BlockId body = (BlockId)v.b;
                    if (!valid_block_id_(m, body)) {
                        std::ostringstream oss;
                        oss << "value #" << vid << " loop has invalid body block id " << body;
                        push_error_(errs, oss.str());
                    }
                    break;
                }

                case ValueKind::kBlockExpr: {
                    const BlockId blk = (BlockId)v.a;
                    if (!valid_block_id_(m, blk)) {
                        std::ostringstream oss;
                        oss << "value #" << vid << " block-expr has invalid block id " << blk;
                        push_error_(errs, oss.str());
                    }
                    if (v.b != k_invalid_value && !valid_value_id_(m, v.b)) {
                        std::ostringstream oss;
                        oss << "value #" << vid << " block-expr has invalid tail value id " << v.b;
                        push_error_(errs, oss.str());
                    }
                    break;
                }

                case ValueKind::kCall: {
                    need_child(v.a, "callee");
                    const uint64_t arg_end = (uint64_t)v.arg_begin + (uint64_t)v.arg_count;
                    if (arg_end > (uint64_t)m.args.size()) {
                        std::ostringstream oss;
                        oss << "value #" << vid << " call has out-of-range args slice";
                        push_error_(errs, oss.str());
                        break;
                    }

                    for (uint32_t i = 0; i < v.arg_count; ++i) {
                        const auto& a = m.args[v.arg_begin + i];
                        if (a.kind == ArgKind::kNamedGroup) {
                            const uint64_t child_end = (uint64_t)a.child_begin + (uint64_t)a.child_count;
                            if (child_end > (uint64_t)m.args.size()) {
                                std::ostringstream oss;
                                oss << "value #" << vid << " call has named-group arg with out-of-range children";
                                push_error_(errs, oss.str());
                            }
                            continue;
                        }
                        if (a.value != k_invalid_value && !valid_value_id_(m, a.value)) {
                            std::ostringstream oss;
                            oss << "value #" << vid << " call arg has invalid value id " << a.value;
                            push_error_(errs, oss.str());
                        }
                    }
                    break;
                }

                case ValueKind::kArrayLit: {
                    const uint64_t arg_end = (uint64_t)v.arg_begin + (uint64_t)v.arg_count;
                    if (arg_end > (uint64_t)m.args.size()) {
                        std::ostringstream oss;
                        oss << "value #" << vid << " array literal has out-of-range args slice";
                        push_error_(errs, oss.str());
                        break;
                    }

                    for (uint32_t i = 0; i < v.arg_count; ++i) {
                        const auto& a = m.args[v.arg_begin + i];
                        if (a.value != k_invalid_value && !valid_value_id_(m, a.value)) {
                            std::ostringstream oss;
                            oss << "value #" << vid << " array literal element has invalid value id " << a.value;
                            push_error_(errs, oss.str());
                        }
                    }
                    break;
                }

                default:
                    break;
            }
        }

        return errs;
    }

} // namespace gaupel::sir
