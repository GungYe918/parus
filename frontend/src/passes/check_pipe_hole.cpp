// frontend/src/passes/check_pipe_hole.cpp
#include <parus/ast/Nodes.hpp>
#include <parus/diag/Diagnostic.hpp>
#include <parus/diag/DiagCode.hpp>
#include <parus/syntax/TokenKind.hpp>

namespace parus::passes {

    static void report(diag::Bag& bag, diag::Code code, Span span, int v0 = -1) {
        diag::Diagnostic d(diag::Severity::kError, code, span);
        if (v0 >= 0) d.add_arg_int(v0);
        bag.add(std::move(d));
    }

    static bool is_call(const ast::Expr& e)      { return e.kind == ast::ExprKind::kCall; }
    static bool is_hole_expr(const ast::Expr& e) { return e.kind == ast::ExprKind::kHole; }

    static void walk_expr(const ast::AstArena& ast, ast::ExprId id, diag::Bag& bag);

    struct PipeScan {
        bool any_labeled = false;
        bool any_positional = false;
        int hole_count = 0;
    };

    static void scan_arg_list_for_pipe(
        const ast::AstArena& ast,
        const std::vector<ast::Arg>& args,
        uint32_t begin,
        uint32_t count,
        diag::Bag& bag,
        PipeScan& scan
    );

    static void scan_named_group_for_pipe(
        const ast::AstArena& ast,
        const ast::Arg& named_group,
        diag::Bag& bag,
        PipeScan& scan
    ) {
        // named-group 자체는 "라벨 인자 그룹"이므로 labeled로 취급
        scan.any_labeled = true;

        const auto& ng = ast.named_group_args();
        scan_arg_list_for_pipe(ast, ng, named_group.child_begin, named_group.child_count, bag, scan);
    }

    static void scan_arg_list_for_pipe(
        const ast::AstArena& ast,
        const std::vector<ast::Arg>& args,
        uint32_t begin,
        uint32_t count,
        diag::Bag& bag,
        PipeScan& scan
    ) {
        for (uint32_t i = 0; i < count; ++i) {
            const auto& a = args[begin + i];

            // named-group: 내부 엔트리들을 재귀적으로 스캔
            if (a.kind == ast::ArgKind::kNamedGroup) {
                scan_named_group_for_pipe(ast, a, bag, scan);
                continue;
            }

            // labeled / positional 분류
            const bool labeled = a.has_label;
            scan.any_labeled |= labeled;
            scan.any_positional |= !labeled;

            // hole 카운트/규칙
            if (a.is_hole) {
                ++scan.hole_count;
                if (!labeled) {
                    report(bag, diag::Code::kPipeHoleMustBeLabeled, a.span);
                }
                continue;
            }

            // expr 쪽에 "_"가 들어온 경우(특히 positional "_" 같은 케이스) 방지
            if (a.expr != ast::k_invalid_expr) {
                const auto& ex = ast.expr(a.expr);
                if (is_hole_expr(ex)) {
                    report(bag, diag::Code::kPipeHolePositionalNotAllowed, ex.span);
                } else {
                    walk_expr(ast, a.expr, bag);
                }
            }
        }
    }

    // call_expr: pipe에 사용되는 "함수 호출" Expr
    static void validate_pipe_call(const ast::AstArena& ast, const ast::Expr& call_expr, diag::Bag& bag) {
        PipeScan scan{};

        // call의 기본 인자 리스트 스캔
        const auto& args = ast.args();
        scan_arg_list_for_pipe(ast, args, call_expr.arg_begin, call_expr.arg_count, bag, scan);

        // labeled/positional 혼용 금지
        if (scan.any_labeled && scan.any_positional) {
            report(bag, diag::Code::kCallArgMixNotAllowed, call_expr.span);
        }

        // hole 개수는 정확히 1개
        if (scan.hole_count != 1) {
            report(bag, diag::Code::kPipeHoleCountMismatch, call_expr.span, scan.hole_count);
        }
    }

    static void check_pipe(const ast::AstArena& ast, const ast::Expr& pipe_expr, diag::Bag& bag) {
        const auto& lhs = ast.expr(pipe_expr.a);
        const auto& rhs = ast.expr(pipe_expr.b);

        // |> : RHS가 call 이어야 함
        if (pipe_expr.op == syntax::TokenKind::kPipeFwd) {
            if (!is_call(rhs)) {
                report(bag, diag::Code::kPipeFwdRhsMustBeCall, pipe_expr.span);
                return;
            }
            validate_pipe_call(ast, rhs, bag);
            return;
        }

        // <| : LHS가 call 이어야 함
        if (pipe_expr.op == syntax::TokenKind::kPipeRev) {
            if (!is_call(lhs)) {
                report(bag, diag::Code::kPipeRevLhsMustBeCall, pipe_expr.span);
                return;
            }
            validate_pipe_call(ast, lhs, bag);
            return;
        }
    }

    static void walk_expr(const ast::AstArena& ast, ast::ExprId id, diag::Bag& bag) {
        if (id == ast::k_invalid_expr) return;

        const auto& e = ast.expr(id);

        switch (e.kind) {
            case ast::ExprKind::kUnary:
            case ast::ExprKind::kPostfixUnary:
                walk_expr(ast, e.a, bag);
                break;

            case ast::ExprKind::kBinary:
                // pipe operators: |>  and  <|
                if (e.op == syntax::TokenKind::kPipeFwd || e.op == syntax::TokenKind::kPipeRev) {
                    check_pipe(ast, e, bag);
                }
                walk_expr(ast, e.a, bag);
                walk_expr(ast, e.b, bag);
                break;

            case ast::ExprKind::kTernary:
                walk_expr(ast, e.a, bag);
                walk_expr(ast, e.b, bag);
                walk_expr(ast, e.c, bag);
                break;

            case ast::ExprKind::kCast:
                // cast operand만 순회하면 됨 (type은 Expr 트리가 아님)
                walk_expr(ast, e.a, bag);
                break;

            case ast::ExprKind::kCall: {
                walk_expr(ast, e.a, bag);

                const auto& args = ast.args();
                for (uint32_t i = 0; i < e.arg_count; ++i) {
                    const auto& a = args[e.arg_begin + i];

                    if (a.kind == ast::ArgKind::kNamedGroup) {
                        const auto& ng = ast.named_group_args();
                        for (uint32_t j = 0; j < a.child_count; ++j) {
                            const auto& entry = ng[a.child_begin + j];
                            if (!entry.is_hole && entry.expr != ast::k_invalid_expr) {
                                walk_expr(ast, entry.expr, bag);
                            }
                        }
                        continue;
                    }

                    if (!a.is_hole && a.expr != ast::k_invalid_expr) {
                        walk_expr(ast, a.expr, bag);
                    }
                }
                break;
            }

            case ast::ExprKind::kIndex:
                walk_expr(ast, e.a, bag);
                walk_expr(ast, e.b, bag);
                break;

            case ast::ExprKind::kFieldInit: {
                const auto& inits = ast.field_init_entries();
                const uint64_t begin = e.field_init_begin;
                const uint64_t end = begin + e.field_init_count;
                if (begin <= inits.size() && end <= inits.size()) {
                    for (uint32_t i = 0; i < e.field_init_count; ++i) {
                        const auto& ent = inits[e.field_init_begin + i];
                        if (ent.expr != ast::k_invalid_expr) {
                            walk_expr(ast, ent.expr, bag);
                        }
                    }
                }
                break;
            }

            default:
                break;
        }
    }

    void check_pipe_hole(const ast::AstArena& ast, ast::ExprId root, diag::Bag& bag) {
        walk_expr(ast, root, bag);
    }

} // namespace parus::passes
