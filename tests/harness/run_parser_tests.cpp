#include <gaupel/lex/Lexer.hpp>
#include <gaupel/parse/Parser.hpp>
#include <gaupel/passes/Passes.hpp>
#include <gaupel/tyck/TypeCheck.hpp>
#include <gaupel/sir/Builder.hpp>

#include <iostream>
#include <string>

namespace {

    struct ParsedProgram {
        gaupel::ast::AstArena ast;
        gaupel::ty::TypePool types;
        gaupel::diag::Bag bag;
        gaupel::ast::StmtId root = gaupel::ast::k_invalid_stmt;
    };

    static ParsedProgram parse_program(const std::string& src) {
        ParsedProgram p{};
        gaupel::Lexer lx(src, /*file_id=*/1, &p.bag);
        const auto tokens = lx.lex_all();

        gaupel::Parser parser(tokens, p.ast, p.types, &p.bag);
        p.root = parser.parse_program();
        return p;
    }

    static gaupel::passes::PassResults run_passes(ParsedProgram& p) {
        gaupel::passes::PassOptions opt{};
        return gaupel::passes::run_on_program(p.ast, p.root, p.bag, opt);
    }

    static gaupel::tyck::TyckResult run_tyck(ParsedProgram& p) {
        gaupel::tyck::TypeChecker tc(p.ast, p.types, p.bag);
        return tc.check_program(p.root);
    }

    static bool require_(bool cond, const char* msg) {
        if (cond) return true;
        std::cerr << "  - " << msg << "\n";
        return false;
    }

    static bool test_suffix_literals_work() {
        // 타입 접미사가 붙은 숫자(정수/실수)가 파싱+타입체크에서 정상 동작해야 함
        const std::string src = R"(
            fn main() -> u32 {
                set x = 1u32;
                let a: f32 = 10.0f;
                let b: f64 = 10lf;
                return x;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        auto ty = run_tyck(p);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "suffix literal program must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "suffix literal program must not emit tyck errors");
        return ok;
    }

    static bool test_null_coalesce_assign_parsed_as_assign() {
        // '??='가 이항식이 아니라 대입식(Assign)으로 파싱되어야 한다.
        const std::string src = R"(
            fn main() -> unit {
                let mut o: i32? = null;
                o ??= 1;
                return;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        auto ty = run_tyck(p);

        bool found_qq_assign = false;
        for (const auto& ex : p.ast.exprs()) {
            if (ex.kind == gaupel::ast::ExprKind::kAssign &&
                ex.op == gaupel::syntax::TokenKind::kQuestionQuestionAssign) {
                found_qq_assign = true;
                break;
            }
        }

        bool ok = true;
        ok &= require_(found_qq_assign, "parser must build Assign node for '?" "?='");
        ok &= require_(!p.bag.has_error(), "valid '?" "?=' program must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "valid '?" "?=' program must not emit tyck errors");
        return ok;
    }

    static bool test_loop_expr_break_value_allowed() {
        // loop 표현식 내부에서는 break 값이 허용되어야 한다.
        const std::string src = R"(
            fn main() -> i32 {
                set x = loop {
                    break 7i32;
                };
                return x;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        auto ty = run_tyck(p);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "loop expression break-value program must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "loop expression break-value program must not emit tyck errors");
        return ok;
    }

    static bool test_while_break_value_rejected() {
        // while 같은 statement-loop에서는 break 값이 금지되어야 한다.
        const std::string src = R"(
            fn main() -> i32 {
                while (true) {
                    break 1i32;
                }
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        auto ty = run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(gaupel::diag::Code::kTypeErrorGeneric),
            "while + break value must emit type error");
        ok &= require_(!ty.errors.empty(),
            "while + break value must produce tyck error entry");
        return ok;
    }

    static bool test_loop_header_var_name_resolved() {
        // loop (v in xs) 에서 v가 body에서 UndefinedName 없이 해석되어야 한다.
        const std::string src = R"(
            fn main(xs: i32[]) -> i32 {
                loop (v in xs) {
                    set tmp = v;
                    break;
                }
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);

        bool ok = true;
        ok &= require_(!p.bag.has_code(gaupel::diag::Code::kUndefinedName),
            "loop header variable must be visible in loop body");
        return ok;
    }

    static bool test_sir_uses_symbol_declared_type_for_set() {
        // SIR var decl의 declared_type은 init expr 타입이 아니라 SymbolTable의 확정 타입을 써야 한다.
        const std::string src = R"(
            fn main() -> i64 {
                set x = 1;
                let y: i64 = x;
                return y;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "SIR declared_type test source must type-check cleanly");
        ok &= require_(ty.errors.empty(), "SIR declared_type test must not emit tyck errors");
        if (!ok) return false;

        gaupel::sir::BuildOptions bopt{};
        const auto mod = gaupel::sir::build_sir_module(
            p.ast, p.root, pres.sym, pres.name_resolve, ty, p.types, bopt
        );

        ok &= require_(!mod.funcs.empty(), "SIR module must contain at least one function");
        if (!ok) return false;

        const auto& fn = mod.funcs.front();
        ok &= require_(fn.entry != gaupel::sir::k_invalid_block, "function entry block must exist");
        if (!ok) return false;

        const auto& entry = mod.blocks[fn.entry];
        const auto i64_ty = p.types.builtin(gaupel::ty::Builtin::kI64);

        bool found_x = false;
        for (uint32_t i = 0; i < entry.stmt_count; ++i) {
            const auto& st = mod.stmts[entry.stmt_begin + i];
            if (st.kind == gaupel::sir::StmtKind::kVarDecl && st.name == "x") {
                found_x = true;
                ok &= require_(st.declared_type == i64_ty, "SIR declared_type for 'set x = 1' must be i64");
                break;
            }
        }

        ok &= require_(found_x, "SIR entry block must contain var decl for 'x'");
        return ok;
    }

} // namespace

int main() {
    struct Case {
        const char* name;
        bool (*fn)();
    };

    const Case cases[] = {
        {"suffix_literals_work", test_suffix_literals_work},
        {"null_coalesce_assign_parsed_as_assign", test_null_coalesce_assign_parsed_as_assign},
        {"loop_expr_break_value_allowed", test_loop_expr_break_value_allowed},
        {"while_break_value_rejected", test_while_break_value_rejected},
        {"loop_header_var_name_resolved", test_loop_header_var_name_resolved},
        {"sir_uses_symbol_declared_type_for_set", test_sir_uses_symbol_declared_type_for_set},
    };

    int failed = 0;
    for (const auto& tc : cases) {
        std::cout << "[TEST] " << tc.name << "\n";
        const bool ok = tc.fn();
        if (!ok) {
            ++failed;
            std::cout << "  -> FAIL\n";
        } else {
            std::cout << "  -> PASS\n";
        }
    }

    if (failed != 0) {
        std::cout << "FAILED: " << failed << " test(s)\n";
        return 1;
    }

    std::cout << "ALL TESTS PASSED\n";
    return 0;
}
