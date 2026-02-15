#include <parus/lex/Lexer.hpp>
#include <parus/parse/Parser.hpp>
#include <parus/passes/Passes.hpp>
#include <parus/cap/CapabilityCheck.hpp>
#include <parus/tyck/TypeCheck.hpp>
#include <parus/sir/Builder.hpp>
#include <parus/sir/CapabilityAnalysis.hpp>
#include <parus/sir/MutAnalysis.hpp>
#include <parus/sir/Verify.hpp>
#include <parus/oir/Builder.hpp>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

namespace {

    struct ParsedProgram {
        parus::ast::AstArena ast;
        parus::ty::TypePool types;
        parus::diag::Bag bag;
        parus::ast::StmtId root = parus::ast::k_invalid_stmt;
    };

    static ParsedProgram parse_program(const std::string& src) {
        ParsedProgram p{};
        parus::Lexer lx(src, /*file_id=*/1, &p.bag);
        const auto tokens = lx.lex_all();

        parus::Parser parser(tokens, p.ast, p.types, &p.bag);
        p.root = parser.parse_program();
        return p;
    }

    static parus::passes::PassResults run_passes(ParsedProgram& p) {
        parus::passes::PassOptions opt{};
        return parus::passes::run_on_program(p.ast, p.root, p.bag, opt);
    }

    static parus::tyck::TyckResult run_tyck(ParsedProgram& p) {
        parus::tyck::TypeChecker tc(p.ast, p.types, p.bag);
        return tc.check_program(p.root);
    }

    static parus::cap::CapabilityResult run_cap(
        ParsedProgram& p,
        const parus::passes::PassResults& pres,
        const parus::tyck::TyckResult& ty
    ) {
        return parus::cap::run_capability_check(
            p.ast, p.root, pres.name_resolve, ty, p.types, p.bag
        );
    }

    struct SirRun {
        parus::sir::Module mod;
        std::vector<parus::sir::VerifyError> verify_errors;
        std::vector<parus::sir::VerifyError> handle_verify_errors;
        parus::sir::CapabilityAnalysisResult cap;
    };

    static SirRun run_sir(
        ParsedProgram& p,
        const parus::passes::PassResults& pres,
        const parus::tyck::TyckResult& ty
    ) {
        SirRun out{};
        parus::sir::BuildOptions bopt{};
        out.mod = parus::sir::build_sir_module(
            p.ast, p.root, pres.sym, pres.name_resolve, ty, p.types, bopt
        );
        (void)parus::sir::canonicalize_for_capability(out.mod, p.types);
        out.verify_errors = parus::sir::verify_module(out.mod);
        out.cap = parus::sir::analyze_capabilities(out.mod, p.types, p.bag);
        out.handle_verify_errors = parus::sir::verify_escape_handles(out.mod);
        return out;
    }

    static bool require_(bool cond, const char* msg) {
        if (cond) return true;
        std::cerr << "  - " << msg << "\n";
        return false;
    }

    static bool read_text_file_(const std::filesystem::path& p, std::string& out) {
        std::ifstream ifs(p, std::ios::in | std::ios::binary);
        if (!ifs) return false;
        out.assign(std::istreambuf_iterator<char>(ifs), std::istreambuf_iterator<char>());
        return true;
    }

    static bool run_file_case_(const std::filesystem::path& p) {
        std::string src;
        if (!read_text_file_(p, src)) {
            std::cerr << "  - failed to read case file: " << p.string() << "\n";
            return false;
        }

        auto prog = parse_program(src);
        auto pres = run_passes(prog);
        auto ty = run_tyck(prog);
        auto cap = run_cap(prog, pres, ty);
        auto sir = run_sir(prog, pres, ty);

        const std::string file_name = p.filename().string();
        const bool expect_error = (file_name.rfind("err_", 0) == 0);

        if (expect_error) {
            const bool has_any_error =
                prog.bag.has_error() ||
                !ty.errors.empty() ||
                !cap.ok ||
                !sir.verify_errors.empty() ||
                !sir.handle_verify_errors.empty() ||
                !sir.cap.ok;
            bool ok = true;
            ok &= require_(has_any_error, "expected diagnostics for err_ case, but none were emitted");
            if (!ok) {
                std::cerr << "    file: " << p.filename().string() << "\n";
            }
            return ok;
        }

        bool ok = true;
        ok &= require_(!prog.bag.has_error(), "file case emitted parser/sema diagnostics");
        ok &= require_(ty.errors.empty(), "file case emitted tyck errors");
        ok &= require_(cap.ok, "file case emitted AST capability errors");
        ok &= require_(sir.cap.ok, "file case emitted SIR capability errors");
        ok &= require_(sir.handle_verify_errors.empty(), "file case failed SIR escape-handle verification");
        if (!ok) {
            std::cerr << "    file: " << p.filename().string() << "\n";
            return false;
        }

        ok &= require_(!sir.mod.funcs.empty(), "file case must lower at least one function to SIR");
        ok &= require_(sir.verify_errors.empty(), "SIR verifier failed for file case");
        if (!ok) {
            std::cerr << "    file: " << p.filename().string() << "\n";
        }
        return ok;
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
            fn main() -> void {
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
            if (ex.kind == parus::ast::ExprKind::kAssign &&
                ex.op == parus::syntax::TokenKind::kQuestionQuestionAssign) {
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
        ok &= require_(p.bag.has_code(parus::diag::Code::kTypeBreakValueOnlyInLoopExpr),
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
        ok &= require_(!p.bag.has_code(parus::diag::Code::kUndefinedName),
            "loop header variable must be visible in loop body");
        return ok;
    }

    static bool test_diag_ambiguous_amp_prefix_chain() {
        // &&&x 는 모호한 접두사 체인으로 진단되어야 한다.
        const std::string src = R"(
            fn main() -> i32 {
                set x = 1i32;
                set y = &&&x;
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kAmbiguousAmpPrefixChain),
            "&&& chain must emit AmbiguousAmpPrefixChain");
        return ok;
    }

    static bool test_diag_call_no_args_after_named_group() {
        // named-group 뒤에 추가 인자가 오면 전용 진단이 나와야 한다.
        const std::string src = R"(
            fn sub(a: i32, b: i32, { clamp: i32 = 0 }) -> i32 {
                return a - b + clamp;
            }
            fn main() -> i32 {
                return sub(1, 2, { clamp: 1 }, 3);
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kCallNoArgsAfterNamedGroup),
            "args after named-group must emit CallNoArgsAfterNamedGroup");
        return ok;
    }

    static bool test_diag_var_decl_name_expected() {
        // 변수 선언에서 이름이 빠지면 전용 진단이 나와야 한다.
        const std::string src = R"(
            fn main() -> i32 {
                let : i32 = 1i32;
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kVarDeclNameExpected),
            "missing var name must emit VarDeclNameExpected");
        return ok;
    }

    static bool test_diag_set_initializer_required() {
        // set 선언은 '=' 초기화식이 반드시 필요하다.
        const std::string src = R"(
            fn main() -> i32 {
                set x;
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kSetInitializerRequired),
            "set without initializer must emit SetInitializerRequired");
        return ok;
    }

    static bool test_diag_var_initializer_expected() {
        // '=' 뒤에 초기화식이 없으면 전용 진단이 나와야 한다.
        const std::string src = R"(
            fn main() -> i32 {
                let x: i32 = ;
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kVarDeclInitializerExpected),
            "missing initializer expression must emit VarDeclInitializerExpected");
        return ok;
    }

    static bool test_diag_cast_target_type_expected() {
        // as/as?/as! 뒤에는 타입이 필요하다.
        const std::string src = R"(
            fn main() -> i32 {
                set x = 1i32 as ;
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kCastTargetTypeExpected),
            "missing cast target type must emit CastTargetTypeExpected");
        return ok;
    }

    static bool test_diag_fn_name_expected() {
        // fn 선언에는 함수 이름 식별자가 필요하다.
        const std::string src = R"(
            fn (x: i32) -> i32 {
                return x;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kFnNameExpected),
            "missing function name must emit FnNameExpected");
        return ok;
    }

    static bool test_diag_field_member_name_expected() {
        // field 멤버 선언은 이름 식별자가 필요하다.
        const std::string src = R"(
            field P {
                i32;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kFieldMemberNameExpected),
            "missing field member name must emit FieldMemberNameExpected");
        return ok;
    }

    static bool test_diag_acts_for_not_supported() {
        // acts for 구문은 현재 미지원 진단이 나와야 한다.
        const std::string src = R"(
            acts for Logger {
                fn write(msg: i32) -> void { return; }
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kActsForNotSupported),
            "acts for syntax must emit ActsForNotSupported");
        return ok;
    }

    static bool test_diag_block_tail_expr_required() {
        // value-required block(if expr branch)에서 tail 식이 없으면 전용 진단이 나와야 한다.
        const std::string src = R"(
            fn main() -> i32 {
                set x = if (true) {
                    set y = 1i32;
                } else {
                    2i32
                };
                return x;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kBlockTailExprRequired),
            "missing tail expr in value-required block must emit BlockTailExprRequired");
        return ok;
    }

    static bool test_cap_escape_on_slice_borrow_rejected() {
        // slice borrow 값에 &&를 적용하면 금지되어야 한다.
        const std::string src = R"(
            fn main() -> i32 {
                let arr: i32[3] = [1, 2, 3];
                set s = &arr[0..:1];
                set h = &&s;
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);
        auto cap = run_cap(p, pres, ty);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!sir.cap.ok, "&& on slice borrow must fail SIR capability check");
        ok &= require_(p.bag.has_code(parus::diag::Code::kEscapeOperandMustNotBeBorrow),
            "&& on slice borrow must emit EscapeOperandMustNotBeBorrow");
        ok &= require_(cap.ok, "AST capability pass should stay as lightweight filter for this case");
        return ok;
    }

    static bool test_borrow_read_in_arithmetic_ok() {
        // &i32 파라미터를 산술식에서 읽기 값으로 사용할 수 있어야 한다.
        const std::string src = R"(
            fn sum2(a: &i32, b: &i32) -> i32 {
                return a + b;
            }
            fn main() -> i32 {
                let x: i32 = 10;
                let y: i32 = 20;
                set s = sum2(a: &x, b: &y);
                return s;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);
        auto cap = run_cap(p, pres, ty);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "borrow arithmetic source must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "borrow arithmetic source must not emit tyck errors");
        ok &= require_(cap.ok, "borrow arithmetic source must pass capability check");
        ok &= require_(sir.cap.ok, "borrow arithmetic source must pass SIR capability check");
        return ok;
    }

    static bool test_mut_borrow_write_through_assignment_ok() {
        // &mut T 바인딩은 대입을 통해 pointee 쓰기가 가능해야 한다.
        const std::string src = R"(
            fn inc(x: &mut i32) -> void {
                x = x + 1;
                return;
            }
            fn main() -> i32 {
                set mut a = 1i32;
                inc(x: &mut a);
                return a;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);
        auto cap = run_cap(p, pres, ty);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "mut-borrow write-through source must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "mut-borrow write-through source must not emit tyck errors");
        ok &= require_(cap.ok, "mut-borrow write-through source must pass capability check");
        ok &= require_(sir.cap.ok, "mut-borrow write-through source must pass SIR capability check");
        return ok;
    }

    static bool test_cap_shared_conflict_with_mut() {
        // 활성 &mut borrow가 있으면 shared borrow(&)를 추가로 만들 수 없어야 한다.
        const std::string src = R"(
            fn main() -> i32 {
                set mut x = 1i32;
                set m = &mut x;
                set r = &x;
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!sir.cap.ok, "shared borrow under active &mut must fail SIR capability check");
        ok &= require_(p.bag.has_code(parus::diag::Code::kBorrowSharedConflictWithMut),
            "shared borrow under active &mut must emit BorrowSharedConflictWithMut");
        return ok;
    }

    static bool test_cap_mut_conflict_with_shared() {
        // 활성 shared borrow가 있으면 &mut borrow를 만들 수 없어야 한다.
        const std::string src = R"(
            fn main() -> i32 {
                set mut x = 1i32;
                set r = &x;
                set m = &mut x;
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!sir.cap.ok, "&mut under active shared borrow must fail SIR capability check");
        ok &= require_(p.bag.has_code(parus::diag::Code::kBorrowMutConflictWithShared),
            "&mut under active shared borrow must emit BorrowMutConflictWithShared");
        return ok;
    }

    static bool test_cap_shared_write_conflict() {
        // 활성 shared borrow가 있는 동안에는 해당 place에 쓰기를 할 수 없어야 한다.
        const std::string src = R"(
            fn main() -> i32 {
                set mut x = 1i32;
                set r = &x;
                x = 2i32;
                return x;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!sir.cap.ok, "write under active shared borrow must fail SIR capability check");
        ok &= require_(p.bag.has_code(parus::diag::Code::kBorrowSharedWriteConflict),
            "write under active shared borrow must emit BorrowSharedWriteConflict");
        return ok;
    }

    static bool test_escape_requires_static_or_boundary() {
        // &&는 static place이거나 return/call-arg 경계에서만 허용되어야 한다.
        const std::string src = R"(
            fn main() -> i32 {
                set x = 1i32;
                set h = &&x;
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);
        auto cap = run_cap(p, pres, ty);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!sir.cap.ok, "non-boundary && on non-static place must fail SIR capability check");
        ok &= require_(p.bag.has_code(parus::diag::Code::kSirEscapeBoundaryViolation),
            "non-boundary && on non-static place must emit SirEscapeBoundaryViolation");
        ok &= require_(cap.ok, "AST capability pass should keep lightweight behavior for boundary checks");
        return ok;
    }

    static bool test_static_allows_escape_storage() {
        // static place는 non-boundary 문맥에서도 &&를 허용해야 한다.
        const std::string src = R"(
            static let G: i32 = 7i32;
            static mut set HG = &&G;
            fn main() -> i32 {
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);
        auto cap = run_cap(p, pres, ty);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "static + && source must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "static + && source must not emit tyck errors");
        ok &= require_(cap.ok, "static + && source must pass capability check");
        ok &= require_(sir.cap.ok, "static + && source must pass SIR capability check");
        return ok;
    }

    static bool test_sir_handle_verify_rejects_materialized_handle() {
        // OIR 이전 단계에서는 handle 물질화 카운트가 0이어야 하며, 0이 아니면 verify가 실패해야 한다.
        const std::string src = R"(
            static let G: i32 = 7i32;
            fn sink(h: &&i32) -> i32 {
                return 0i32;
            }
            fn main() -> i32 {
                return sink(h: &&G);
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "materialize-count verify seed must parse/type-check cleanly");
        ok &= require_(ty.errors.empty(), "materialize-count verify seed must not emit tyck errors");
        ok &= require_(sir.cap.ok, "materialize-count verify seed must pass SIR capability check");
        ok &= require_(sir.handle_verify_errors.empty(), "materialize-count verify seed must pass handle verify initially");
        ok &= require_(!sir.mod.escape_handles.empty(), "materialize-count verify seed must produce at least one escape handle");
        if (!ok) return false;

        sir.mod.escape_handles[0].materialize_count = 1;
        const auto verrs = parus::sir::verify_escape_handles(sir.mod);
        ok &= require_(!verrs.empty(), "handle verify must fail when materialize_count is non-zero");

        bool has_materialize_msg = false;
        for (const auto& e : verrs) {
            if (e.msg.find("materialize_count must be 0") != std::string::npos) {
                has_materialize_msg = true;
                break;
            }
        }
        ok &= require_(has_materialize_msg, "handle verify must report materialize_count invariant violation");
        return ok;
    }

    static bool test_oir_gate_rejects_invalid_escape_handle() {
        // OIR lowering 진입 전 게이트는 escape-handle verify 실패 시 lowering을 중단해야 한다.
        const std::string src = R"(
            static let G: i32 = 7i32;
            fn sink(h: &&i32) -> i32 {
                return 0i32;
            }
            fn main() -> i32 {
                return sink(h: &&G);
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "OIR gate seed must parse/type-check cleanly");
        ok &= require_(ty.errors.empty(), "OIR gate seed must not emit tyck errors");
        ok &= require_(sir.cap.ok, "OIR gate seed must pass SIR capability check");
        ok &= require_(sir.handle_verify_errors.empty(), "OIR gate seed must pass SIR handle verify initially");
        ok &= require_(!sir.mod.escape_handles.empty(), "OIR gate seed must produce at least one escape handle");
        if (!ok) return false;

        sir.mod.escape_handles[0].materialize_count = 1;
        parus::oir::Builder ob(sir.mod, p.types);
        auto oir = ob.build();

        ok &= require_(!oir.gate_passed, "OIR gate must fail when escape handle verify fails");
        ok &= require_(!oir.gate_errors.empty(), "OIR gate must return at least one gate error");
        return ok;
    }

    static bool test_sir_mut_analysis_allows_mut_borrow_write_through() {
        // SIR mut-analysis는 &mut write-through를 불법 쓰기로 오검출하면 안 된다.
        const std::string src = R"(
            fn inc(x: &mut i32) -> void {
                x = x + 1;
                return;
            }
            fn main() -> i32 {
                set mut a = 1i32;
                inc(x: &mut a);
                return a;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);
        auto cap = run_cap(p, pres, ty);
        auto sir_cap = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "mut-analysis source must not emit parser/tyck/cap diagnostics");
        ok &= require_(ty.errors.empty(), "mut-analysis source must not emit tyck errors");
        ok &= require_(cap.ok, "mut-analysis source must pass capability check");
        ok &= require_(sir_cap.cap.ok, "mut-analysis source must pass SIR capability check");
        if (!ok) return false;

        parus::sir::BuildOptions bopt{};
        const auto mod = parus::sir::build_sir_module(
            p.ast, p.root, pres.sym, pres.name_resolve, ty, p.types, bopt
        );

        (void)parus::sir::analyze_mut(mod, p.types, p.bag);
        ok &= require_(!p.bag.has_code(parus::diag::Code::kWriteToImmutable),
            "SIR mut-analysis must not report WriteToImmutable for &mut write-through");
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

        parus::sir::BuildOptions bopt{};
        const auto mod = parus::sir::build_sir_module(
            p.ast, p.root, pres.sym, pres.name_resolve, ty, p.types, bopt
        );
        const auto verrs = parus::sir::verify_module(mod);

        ok &= require_(!mod.funcs.empty(), "SIR module must contain at least one function");
        ok &= require_(verrs.empty(), "SIR verifier must pass on declared_type test");
        if (!ok) return false;

        const auto& fn = mod.funcs.front();
        ok &= require_(fn.entry != parus::sir::k_invalid_block, "function entry block must exist");
        if (!ok) return false;

        const auto& entry = mod.blocks[fn.entry];
        const auto i64_ty = p.types.builtin(parus::ty::Builtin::kI64);

        bool found_x = false;
        for (uint32_t i = 0; i < entry.stmt_count; ++i) {
            const auto& st = mod.stmts[entry.stmt_begin + i];
            if (st.kind == parus::sir::StmtKind::kVarDecl && st.name == "x") {
                found_x = true;
                ok &= require_(st.declared_type == i64_ty, "SIR declared_type for 'set x = 1' must be i64");
                break;
            }
        }

        ok &= require_(found_x, "SIR entry block must contain var decl for 'x'");
        return ok;
    }

    static bool test_sir_control_flow_block_layout() {
        // while body / loop body / if branch stmt가 entry block에 섞이면 안 된다.
        const std::string src = R"(
            fn f1() -> i32 {
                set mut n = 0i32;
                while (n < 1i32) {
                    n = n + 1i32;
                }
                return n;
            }

            fn f2() -> i32 {
                set x = loop {
                    break 7i32;
                };
                return x;
            }

            fn f3() -> i32 {
                let cond: bool = true;
                if (cond) {
                    return 1i32;
                } else {
                    return 2i32;
                }
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "control-flow layout source must type-check cleanly");
        ok &= require_(ty.errors.empty(), "control-flow layout source must not emit tyck errors");
        if (!ok) return false;

        parus::sir::BuildOptions bopt{};
        const auto mod = parus::sir::build_sir_module(
            p.ast, p.root, pres.sym, pres.name_resolve, ty, p.types, bopt
        );
        const auto verrs = parus::sir::verify_module(mod);
        ok &= require_(verrs.empty(), "SIR verifier must pass on control-flow layout test");
        ok &= require_(mod.funcs.size() >= 3, "expected at least 3 lowered functions");
        if (!ok) return false;

        auto check_entry_stmt_kinds = [&](size_t fi, std::vector<parus::sir::StmtKind> expected) {
            if (fi >= mod.funcs.size()) return false;
            const auto& fn = mod.funcs[fi];
            if (fn.entry == parus::sir::k_invalid_block || (size_t)fn.entry >= mod.blocks.size()) return false;
            const auto& b = mod.blocks[fn.entry];
            if (b.stmt_count != expected.size()) return false;
            for (uint32_t i = 0; i < b.stmt_count; ++i) {
                const uint32_t sid = b.stmt_begin + i;
                if ((size_t)sid >= mod.stmts.size()) return false;
                if (mod.stmts[sid].kind != expected[i]) return false;
            }
            return true;
        };

        ok &= require_(
            check_entry_stmt_kinds(0, {parus::sir::StmtKind::kVarDecl, parus::sir::StmtKind::kWhileStmt, parus::sir::StmtKind::kReturn}),
            "f1 entry block stmt order must be [VarDecl, WhileStmt, Return]"
        );
        ok &= require_(
            check_entry_stmt_kinds(1, {parus::sir::StmtKind::kVarDecl, parus::sir::StmtKind::kReturn}),
            "f2 entry block stmt order must be [VarDecl, Return]"
        );
        ok &= require_(
            check_entry_stmt_kinds(2, {parus::sir::StmtKind::kVarDecl, parus::sir::StmtKind::kIfStmt}),
            "f3 entry block stmt order must be [VarDecl, IfStmt]"
        );
        return ok;
    }

    static bool test_file_cases_directory() {
#ifndef PARUS_TEST_CASE_DIR
        std::cerr << "  - PARUS_TEST_CASE_DIR is not defined\n";
        return false;
#else
        const std::filesystem::path case_dir{PARUS_TEST_CASE_DIR};
        bool ok = true;

        ok &= require_(std::filesystem::exists(case_dir), "case directory does not exist");
        ok &= require_(std::filesystem::is_directory(case_dir), "case directory path is not a directory");
        if (!ok) return false;

        std::vector<std::filesystem::path> files;
        for (const auto& entry : std::filesystem::directory_iterator(case_dir)) {
            if (!entry.is_regular_file()) continue;
            const auto& p = entry.path();
            if (p.extension() == ".pr") files.push_back(p);
        }

        std::sort(files.begin(), files.end());
        ok &= require_(files.size() >= 5, "at least 5 case files are required");
        if (!ok) return false;

        for (const auto& p : files) {
            std::cout << "  [CASE] " << p.filename().string() << "\n";
            ok &= run_file_case_(p);
        }
        return ok;
#endif
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
        {"diag_ambiguous_amp_prefix_chain", test_diag_ambiguous_amp_prefix_chain},
        {"diag_call_no_args_after_named_group", test_diag_call_no_args_after_named_group},
        {"diag_var_decl_name_expected", test_diag_var_decl_name_expected},
        {"diag_set_initializer_required", test_diag_set_initializer_required},
        {"diag_var_initializer_expected", test_diag_var_initializer_expected},
        {"diag_cast_target_type_expected", test_diag_cast_target_type_expected},
        {"diag_fn_name_expected", test_diag_fn_name_expected},
        {"diag_field_member_name_expected", test_diag_field_member_name_expected},
        {"diag_acts_for_not_supported", test_diag_acts_for_not_supported},
        {"diag_block_tail_expr_required", test_diag_block_tail_expr_required},
        {"cap_escape_on_slice_borrow_rejected", test_cap_escape_on_slice_borrow_rejected},
        {"borrow_read_in_arithmetic_ok", test_borrow_read_in_arithmetic_ok},
        {"mut_borrow_write_through_assignment_ok", test_mut_borrow_write_through_assignment_ok},
        {"cap_shared_conflict_with_mut", test_cap_shared_conflict_with_mut},
        {"cap_mut_conflict_with_shared", test_cap_mut_conflict_with_shared},
        {"cap_shared_write_conflict", test_cap_shared_write_conflict},
        {"escape_requires_static_or_boundary", test_escape_requires_static_or_boundary},
        {"static_allows_escape_storage", test_static_allows_escape_storage},
        {"sir_handle_verify_rejects_materialized_handle", test_sir_handle_verify_rejects_materialized_handle},
        {"oir_gate_rejects_invalid_escape_handle", test_oir_gate_rejects_invalid_escape_handle},
        {"sir_mut_analysis_allows_mut_borrow_write_through", test_sir_mut_analysis_allows_mut_borrow_write_through},
        {"sir_uses_symbol_declared_type_for_set", test_sir_uses_symbol_declared_type_for_set},
        {"sir_control_flow_block_layout", test_sir_control_flow_block_layout},
        {"file_cases_directory", test_file_cases_directory},
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
