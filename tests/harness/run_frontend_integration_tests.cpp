#include <parus/lex/Lexer.hpp>
#include <parus/parse/Parser.hpp>
#include <parus/diag/Render.hpp>
#include <parus/macro/Expander.hpp>
#include <parus/passes/Passes.hpp>
#include <parus/cap/CapabilityCheck.hpp>
#include <parus/tyck/TypeCheck.hpp>
#include <parus/type/TypeResolve.hpp>
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
        parus::type::TypeResolveResult type_resolve{};
        bool macro_type_ready = false;
        bool macro_type_ok = false;
    };

    static ParsedProgram parse_program(const std::string& src) {
        ParsedProgram p{};
        parus::Lexer lx(src, /*file_id=*/1, &p.bag);
        const auto tokens = lx.lex_all();

        parus::Parser parser(tokens, p.ast, p.types, &p.bag);
        p.root = parser.parse_program();
        return p;
    }

    static bool run_macro_and_type(ParsedProgram& p) {
        if (p.macro_type_ready) return p.macro_type_ok;
        p.macro_type_ready = true;

        const bool macro_ok = parus::macro::expand_program(p.ast, p.types, p.root, p.bag);
        if (p.bag.has_error() || !macro_ok) {
            p.macro_type_ok = false;
            return false;
        }

        p.type_resolve = parus::type::resolve_program_types(p.ast, p.types, p.root, p.bag);
        p.macro_type_ok = (!p.bag.has_error() && p.type_resolve.ok);
        return p.macro_type_ok;
    }

    static parus::passes::PassResults run_passes(ParsedProgram& p) {
        if (!run_macro_and_type(p)) {
            return parus::passes::PassResults{};
        }
        parus::passes::PassOptions opt{};
        return parus::passes::run_on_program(p.ast, p.root, p.bag, opt);
    }

    static parus::tyck::TyckResult run_tyck(
        ParsedProgram& p,
        const parus::passes::GenericPrepResult* gp = nullptr,
        const parus::sema::SymbolTable* seed = nullptr
    ) {
        if (!run_macro_and_type(p)) {
            return parus::tyck::TyckResult{};
        }
        parus::tyck::TypeChecker tc(p.ast, p.types, p.bag, &p.type_resolve, gp);
        tc.set_seed_symbol_table(seed);
        return tc.check_program(p.root);
    }

    static parus::cap::CapabilityResult run_cap(
        ParsedProgram& p,
        const parus::passes::PassResults& pres,
        const parus::tyck::TyckResult& ty
    ) {
        if (!run_macro_and_type(p)) {
            parus::cap::CapabilityResult out{};
            out.ok = false;
            return out;
        }
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
        if (!run_macro_and_type(p)) {
            out.cap.ok = false;
            return out;
        }
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

    static parus::oir::BuildResult run_oir(
        ParsedProgram& p,
        const parus::passes::PassResults& pres,
        const SirRun& sir
    ) {
        parus::oir::Builder builder(sir.mod, p.types, nullptr, &pres.sym);
        return builder.build();
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
        auto ty = run_tyck(prog, &pres.generic_prep);
        auto cap = run_cap(prog, pres, ty);
        auto sir = run_sir(prog, pres, ty);

        const std::string file_name = p.filename().string();
        const bool expect_error = (file_name.rfind("err_", 0) == 0);
        const bool expect_warning = (!expect_error && file_name.rfind("warn_", 0) == 0);

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
        if (expect_warning) {
            bool has_warning = false;
            for (const auto& d : prog.bag.diags()) {
                if (d.severity() == parus::diag::Severity::kWarning) {
                    has_warning = true;
                    break;
                }
            }
            ok &= require_(has_warning, "expected warning diagnostics for warn_ case, but none were emitted");
        }
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
            def main() -> u32 {
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

    static bool test_parser_aborted_guard_no_infinite_loop() {
        // lexer fatal(InvalidUtf8)로 parser가 aborted 상태여도 parse_program은 즉시 종료되어야 한다.
        parus::ast::AstArena ast;
        parus::ty::TypePool types;
        parus::diag::Bag bag;

        constexpr parus::Span bad_span{1, 0, 1};
        bag.add(parus::diag::Diagnostic(
            parus::diag::Severity::kError,
            parus::diag::Code::kInvalidUtf8,
            bad_span
        ));

        std::vector<parus::Token> toks{
            parus::Token{parus::syntax::TokenKind::kIdent, bad_span, "x"},
            parus::Token{parus::syntax::TokenKind::kEof, parus::Span{1, 1, 1}, ""}
        };

        parus::Parser parser(toks, ast, types, &bag);
        const auto root = parser.parse_program();

        bool ok = true;
        ok &= require_(root != parus::ast::k_invalid_stmt, "aborted parser must still return a program root");
        ok &= require_(ast.stmt(root).kind == parus::ast::StmtKind::kBlock, "program root must remain block stmt");
        ok &= require_(ast.stmt(root).stmt_count == 0, "aborted parser should not keep parsing non-EOF tokens");
        ok &= require_(bag.has_code(parus::diag::Code::kInvalidUtf8), "invalid utf8 diagnostic must be preserved");
        return ok;
    }

    static bool test_text_string_literal_typecheck_ok() {
        const std::string src = R"(
            def main() -> i32 {
                let s: text = "Hello, text";
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        auto ty = run_tyck(p);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "text literal source must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "text literal source must not emit tyck errors");
        return ok;
    }

    static bool test_raw_and_format_triple_string_lex_parse_ok() {
        const std::string src = R"(
            def main() -> i32 {
                let raw: text = R"""line1\nline2""";
                let fmt: text = F"""sum={1 + 2}""";
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        auto ty = run_tyck(p);

        bool saw_raw = false;
        bool saw_fmt = false;
        for (const auto& e : p.ast.exprs()) {
            if (e.kind != parus::ast::ExprKind::kStringLit) continue;
            if (e.text.size() >= 4 && e.text.substr(0, 4) == "R\"\"\"") saw_raw = true;
            if (e.text.size() >= 4 && e.text.substr(0, 4) == "F\"\"\"") saw_fmt = true;
        }

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "R/F triple string source must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "R/F triple string source must not emit tyck errors");
        ok &= require_(saw_raw, "parser must retain raw triple string literal token");
        ok &= require_(saw_fmt, "parser must retain format triple string literal token");
        return ok;
    }

    static bool test_fstring_parts_and_escape_split_ok() {
        const std::string src = R"(
            def main() -> i32 {
                let s: text = F"""A{{B}}C{1 + 2}D{3 * 4}E""";
                return 0i32;
            }
        )";

        auto p = parse_program(src);

        const parus::ast::Expr* fmt = nullptr;
        for (const auto& e : p.ast.exprs()) {
            if (e.kind == parus::ast::ExprKind::kStringLit && e.string_is_format) {
                fmt = &e;
                break;
            }
        }

        bool ok = true;
        ok &= require_(fmt != nullptr, "format triple string literal must exist");
        if (!ok) return false;

        ok &= require_(fmt->string_part_count == 5, "F-string must be split to literal/expr/literal/expr/literal");
        if (!ok) return false;

        const auto begin = fmt->string_part_begin;
        const auto& parts = p.ast.fstring_parts();
        ok &= require_(begin + fmt->string_part_count <= parts.size(), "F-string part slice must be in-range");
        if (!ok) return false;

        const auto& p0 = parts[begin + 0];
        const auto& p1 = parts[begin + 1];
        const auto& p2 = parts[begin + 2];
        const auto& p3 = parts[begin + 3];
        const auto& p4 = parts[begin + 4];

        ok &= require_(!p0.is_expr && p0.text == "A{B}C", "part[0] must be escaped literal text");
        ok &= require_(p1.is_expr && p1.expr != parus::ast::k_invalid_expr, "part[1] must be interpolation expr");
        ok &= require_(!p2.is_expr && p2.text == "D", "part[2] must be literal text");
        ok &= require_(p3.is_expr && p3.expr != parus::ast::k_invalid_expr, "part[3] must be interpolation expr");
        ok &= require_(!p4.is_expr && p4.text == "E", "part[4] must be literal text");

        ok &= require_(!p.bag.has_error(), "well-formed F-string source must not emit diagnostics");
        return ok;
    }

    static bool test_bare_dollar_string_removed_diag() {
        const std::string src = R"(
            def main() -> i32 {
                let s: text = $"A{{B}}C{1 + 2}D";
                return 0i32;
            }
        )";

        auto p = parse_program(src);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kBareDollarStringRemoved),
            "bare $\"...\" must emit dedicated parser diagnostic");
        return ok;
    }

    static bool test_fstring_brace_error_diagnostics() {
        const std::string src = R"(
            def main() -> i32 {
                let a: text = F"""x}y""";
                let b: text = F"""x{}y""";
                let c: text = F"""x{1 + 2""";
                let g: text = F"abc";
                return 0i32;
            }
        )";

        auto p = parse_program(src);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kUnexpectedToken),
            "invalid F-string braces must emit UnexpectedToken");
        ok &= require_(p.bag.has_code(parus::diag::Code::kExpectedToken),
            "unterminated F-string interpolation must emit ExpectedToken('}')");
        ok &= require_(p.bag.has_code(parus::diag::Code::kFStringShortFormUnsupported),
            "F\"...\" short form must emit dedicated parser diagnostic");
        return ok;
    }

    static bool test_macro_string_payload_expr_ok() {
        const std::string src = R"(
            macro id -> {
                with expr {
                    (x: expr) => expr { $0 };
                }
            }

            def main() -> i32 {
                let s: text = $id"sum=3";
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        auto ty = run_tyck(p);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "macro string payload source must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "macro string payload source must not emit tyck errors");
        return ok;
    }

    static bool test_macro_block_payload_expr_ok() {
        const std::string src = R"(
            macro id -> {
                with expr {
                    (x: expr) => expr { $0 };
                }
            }

            def main() -> i32 {
                let y: i32 = $id { 40i32 + 2i32 };
                return y;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        auto ty = run_tyck(p);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "macro block payload source must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "macro block payload source must not emit tyck errors");
        return ok;
    }

    static bool test_macro_block_payload_type_ok() {
        const std::string src = R"(
            macro pick_ty -> {
                with token {
                    ($x: tt) => type { i32 };
                }
            }

            def main() -> i32 {
                let v: $pick_ty { any_tokens_here } = 7i32;
                return v;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        auto ty = run_tyck(p);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "macro block payload in type context must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "macro block payload in type context must not emit tyck errors");
        return ok;
    }

    static bool test_pipe_forward_chain_canonicalize_and_sir_pipecall_ok() {
        const std::string src = R"(
            def add({a: i32, b: i32}) -> i32 { return a + b; }
            def mul({x: i32, y: i32}) -> i32 { return x * y; }
            def main() -> i32 {
                return 1i32 |> add(a: _, b: 2i32) |> mul(x: _, y: 10i32);
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p, &pres.generic_prep);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "pipe forward chain source must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "pipe forward chain source must not emit tyck errors");
        ok &= require_(sir.verify_errors.empty(), "pipe forward chain source must pass SIR verify");
        ok &= require_(sir.cap.ok, "pipe forward chain source must pass SIR capability");
        if (!ok) return false;

        bool has_pipe_binary = false;
        bool has_pipe_call_expr = false;
        for (const auto& e : p.ast.exprs()) {
            if (e.kind == parus::ast::ExprKind::kBinary &&
                e.op == parus::syntax::TokenKind::kPipeFwd) {
                has_pipe_binary = true;
            }
            if (e.kind == parus::ast::ExprKind::kCall && e.call_from_pipe) {
                has_pipe_call_expr = true;
            }
        }

        bool has_pipe_call_value = false;
        bool has_pipe_chain_value = false;
        for (const auto& v : sir.mod.values) {
            if (v.kind == parus::sir::ValueKind::kPipeCall) {
                has_pipe_call_value = true;
                bool has_pipe_input = false;
                bool has_ten_literal = false;
                const uint64_t begin = v.arg_begin;
                const uint64_t end = begin + v.arg_count;
                if (end <= sir.mod.args.size()) {
                    for (uint32_t i = 0; i < v.arg_count; ++i) {
                        const auto aid = v.arg_begin + i;
                        const auto avid = sir.mod.args[aid].value;
                        if (avid == parus::sir::k_invalid_value ||
                            avid >= sir.mod.values.size()) {
                            continue;
                        }
                        const auto& av = sir.mod.values[avid];
                        if (av.kind == parus::sir::ValueKind::kPipeCall) {
                            has_pipe_input = true;
                        }
                        if (av.kind == parus::sir::ValueKind::kIntLit &&
                            av.text.find("10") != std::string_view::npos) {
                            has_ten_literal = true;
                        }
                    }
                }
                if (has_pipe_input && has_ten_literal) {
                    has_pipe_chain_value = true;
                }
            }
        }

        ok &= require_(!has_pipe_binary, "valid pipe chains must be canonicalized before tyck");
        ok &= require_(has_pipe_call_expr, "canonicalized call expr must carry from_pipe metadata");
        ok &= require_(has_pipe_call_value, "SIR lowering must preserve pipe call as kPipeCall");
        ok &= require_(has_pipe_chain_value,
            "SIR pipe chain must feed previous pipe-call result into next pipe-call argument");
        return ok;
    }

    static bool test_pipe_forward_rhs_must_be_call_error() {
        const std::string src = R"(
            def main() -> i32 {
                set x = 1i32 |> 2i32;
                return x;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p, &pres.generic_prep);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kPipeFwdRhsMustBeCall),
            "pipe rhs non-call must emit kPipeFwdRhsMustBeCall");
        ok &= require_(!ty.errors.empty(), "pipe rhs non-call must fail typecheck");
        return ok;
    }

    static bool test_pipe_forward_hole_count_mismatch_error() {
        const std::string src = R"(
            def add({a: i32, b: i32}) -> i32 { return a + b; }
            def main() -> i32 {
                set x = 1i32 |> add(a: 2i32, b: 3i32);
                set y = 1i32 |> add(a: _, b: _);
                return x + y;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p, &pres.generic_prep);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kPipeHoleCountMismatch),
            "pipe hole count mismatch must emit kPipeHoleCountMismatch");
        ok &= require_(!ty.errors.empty(), "pipe hole count mismatch must fail typecheck");
        return ok;
    }

    static bool test_pipe_forward_positional_hole_rejected() {
        const std::string src = R"(
            def add(a: i32, b: i32) -> i32 { return a + b; }
            def main() -> i32 {
                set x = 1i32 |> add(_, 2i32);
                return x;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p, &pres.generic_prep);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kPipeHolePositionalNotAllowed),
            "positional hole in pipe call must emit kPipeHolePositionalNotAllowed");
        ok &= require_(!ty.errors.empty(), "positional hole in pipe call must fail typecheck");
        return ok;
    }

    static bool test_pipe_reverse_not_supported_yet() {
        const std::string src = R"(
            def add({a: i32, b: i32}) -> i32 { return a + b; }
            def main() -> i32 {
                set x = add(a: _, b: 2i32) <| 1i32;
                return x;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        (void)run_tyck(p, &pres.generic_prep);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kPipeRevNotSupportedYet),
            "pipe reverse must emit reserved-not-supported diagnostic");
        return ok;
    }

    static bool test_null_coalesce_assign_parsed_as_assign() {
        // '??='가 이항식이 아니라 대입식(Assign)으로 파싱되어야 한다.
        const std::string src = R"(
            def main() -> void {
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
            def main() -> i32 {
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

    static bool test_loop_expr_break_infer_context_propagates() {
        const std::string src = R"(
            proto Iterator {
                require type Item;
                require next(it: &mut Self, out: &mut Self::Item) -> bool;
            };

            proto IterSourceMove {
                require type Item;
                require type Iter;
                require into_iter(src: Self) -> Self::Iter;
            };

            struct CounterIter<T>: Iterator with [T == i32] {
                cur: T;
                end: T;
            };

            acts for CounterIter<T is Item> with [T == i32] {
                def next(mut self, out: &mut T) -> bool {
                    if (self.cur > self.end) {
                        return false;
                    }
                    *out = self.cur;
                    self.cur = self.cur + 1i32;
                    return true;
                }
            };

            struct Counter<T>: IterSourceMove with [T == i32] {
                start: T;
                end: T;
            };

            acts for Counter<T is Item, CounterIter<T> is Iter> with [T == i32] {
                def into_iter(self move) -> CounterIter<T> {
                    return CounterIter<T>{ cur: self.start, end: self.end };
                }
            };

            def take(x: i32?) -> i32 {
                if (x == null) { return 0i32; }
                return 42i32;
            }

            def ret_loop() -> i32? {
                return loop {
                    break 42;
                };
            }

            def main() -> i32 {
                let a: i32 = loop {
                    break 42;
                };
                let b: i32? = loop {
                    break 42;
                };
                let c: i32? = loop (x in Counter<i32>{ start: 1i32, end: 4i32 }) {
                    if (x == 4) {
                        break 42;
                    }
                };
                let d: i32 = loop {
                    break (loop {
                        break 42;
                    });
                };
                return take(loop {
                    break 42;
                });
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        auto ty = run_tyck(p);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "loop break infer-context program must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "loop break infer-context program must not emit tyck errors");
        return ok;
    }

    static bool test_loop_expr_break_nested_statement_loop_isolated() {
        const std::string ok_src = R"(
            def main() -> i32 {
                let y: i32 = loop {
                    while (true) {
                        break;
                    }
                    break 42i32;
                };
                return y;
            }
        )";
        const std::string bad_src = R"(
            def main() -> i32 {
                let y: i32 = loop {
                    while (true) {
                        break 1i32;
                    }
                    break 42i32;
                };
                return y;
            }
        )";

        auto p_ok = parse_program(ok_src);
        (void)run_passes(p_ok);
        auto ty_ok = run_tyck(p_ok);

        auto p_bad = parse_program(bad_src);
        (void)run_passes(p_bad);
        auto ty_bad = run_tyck(p_bad);

        bool ok = true;
        ok &= require_(!p_ok.bag.has_error(), "nested statement-loop break isolation positive case must not emit diagnostics");
        ok &= require_(ty_ok.errors.empty(), "nested statement-loop break isolation positive case must not emit tyck errors");
        ok &= require_(p_bad.bag.has_code(parus::diag::Code::kTypeBreakValueOnlyInLoopExpr),
            "inner statement-loop break value must still be rejected");
        ok &= require_(!ty_bad.errors.empty(),
            "inner statement-loop break value rejection must produce tyck error entry");
        return ok;
    }

    static bool test_loop_expr_break_infer_context_negative_regressions() {
        const std::string float_src = R"(
            def main() -> i32 {
                let y: f32? = loop {
                    break 42;
                };
                return 0i32;
            }
        )";
        const std::string mismatch_src = R"(
            proto Iterator {
                require type Item;
                require next(it: &mut Self, out: &mut Self::Item) -> bool;
            };

            proto IterSourceMove {
                require type Item;
                require type Iter;
                require into_iter(src: Self) -> Self::Iter;
            };

            struct CounterIter<T>: Iterator with [T == i32] {
                cur: T;
                end: T;
            };

            acts for CounterIter<T is Item> with [T == i32] {
                def next(mut self, out: &mut T) -> bool {
                    if (self.cur > self.end) {
                        return false;
                    }
                    *out = self.cur;
                    self.cur = self.cur + 1i32;
                    return true;
                }
            };

            struct Counter<T>: IterSourceMove with [T == i32] {
                start: T;
                end: T;
            };

            acts for Counter<T is Item, CounterIter<T> is Iter> with [T == i32] {
                def into_iter(self move) -> CounterIter<T> {
                    return CounterIter<T>{ cur: self.start, end: self.end };
                }
            };

            def main() -> i32 {
                let y: i32 = loop (x in Counter<i32>{ start: 1i32, end: 4i32 }) {
                    if (x == 4) {
                        break 42;
                    }
                };
                return y;
            }
        )";

        auto p_float = parse_program(float_src);
        (void)run_passes(p_float);
        auto ty_float = run_tyck(p_float);

        auto p_mismatch = parse_program(mismatch_src);
        (void)run_passes(p_mismatch);
        auto ty_mismatch = run_tyck(p_mismatch);

        bool ok = true;
        ok &= require_(p_float.bag.has_code(parus::diag::Code::kIntToFloatNotAllowed),
            "loop break infer-context must not allow int-to-float coercion");
        ok &= require_(!ty_float.errors.empty(),
            "loop break infer-context float rejection must produce tyck error entry");
        ok &= require_(p_mismatch.bag.has_code(parus::diag::Code::kTypeLetInitMismatch),
            "iter loop with natural end must still require optional destination");
        ok &= require_(!ty_mismatch.errors.empty(),
            "iter loop natural-end mismatch must produce tyck error entry");
        return ok;
    }

    static bool test_nullable_coalesce_rhs_context_propagates() {
        const std::string src = R"(
            proto Iterator {
                require type Item;
                require next(it: &mut Self, out: &mut Self::Item) -> bool;
            };

            proto IterSourceMove {
                require type Item;
                require type Iter;
                require into_iter(src: Self) -> Self::Iter;
            };

            struct CounterIter<T>: Iterator with [T == i32] {
                cur: T;
                end: T;
            };

            acts for CounterIter<T is Item> with [T == i32] {
                def next(mut self, out: &mut T) -> bool {
                    if (self.cur > self.end) {
                        return false;
                    }
                    *out = self.cur;
                    self.cur = self.cur + 1i32;
                    return true;
                }
            };

            struct Counter<T>: IterSourceMove with [T == i32] {
                start: T;
                end: T;
            };

            acts for Counter<T is Item, CounterIter<T> is Iter> with [T == i32] {
                def into_iter(self move) -> CounterIter<T> {
                    return CounterIter<T>{ cur: self.start, end: self.end };
                }
            };

            def main() -> i32 {
                set inferred = loop (x in Counter<i32>{ start: 1i32, end: 4i32 }) {
                    if (x == 4) {
                        break 42;
                    }
                };
                let inferred_value: i32 = inferred ?? 0;

                let a: i32? = loop (x in Counter<i32>{ start: 1i32, end: 4i32 }) {
                    if (x == 4) {
                        break 42;
                    }
                };
                let b: i32 = a ?? 0;

                let arr_opt: i32[3]? = null;
                let arr: i32[3] = arr_opt ?? [1, 2, 3];

                let c: i32? = 1i32;
                let d: i32 = c ?? loop {
                    break 42;
                };

                return inferred_value + b + arr[0i32] + d;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        auto ty = run_tyck(p);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "nullable coalesce rhs context propagation program must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "nullable coalesce rhs context propagation program must not emit tyck errors");
        return ok;
    }

    static bool test_nullable_coalesce_rhs_context_negative_regressions() {
        const std::string float_src = R"(
            def main() -> i32 {
                let y: i32? = 1i32;
                let t: i32 = y ?? 0.0f;
                return t;
            }
        )";
        const std::string lhs_src = R"(
            def main() -> i32 {
                let y: i32 = 1i32;
                let t: i32 = y ?? 0;
                return t;
            }
        )";

        auto p_float = parse_program(float_src);
        (void)run_passes(p_float);
        auto ty_float = run_tyck(p_float);

        auto p_lhs = parse_program(lhs_src);
        (void)run_passes(p_lhs);
        auto ty_lhs = run_tyck(p_lhs);

        bool ok = true;
        ok &= require_(p_float.bag.has_code(parus::diag::Code::kTypeNullCoalesceRhsMismatch),
            "nullable coalesce must still reject float rhs when lhs element is integer");
        ok &= require_(!ty_float.errors.empty(),
            "nullable coalesce float rhs rejection must produce tyck error entry");
        ok &= require_(p_lhs.bag.has_code(parus::diag::Code::kTypeNullCoalesceLhsMustBeOptional),
            "nullable coalesce must still reject non-optional lhs");
        ok &= require_(!ty_lhs.errors.empty(),
            "nullable coalesce non-optional lhs rejection must produce tyck error entry");
        return ok;
    }

    static bool test_while_break_value_rejected() {
        // while 같은 statement-loop에서는 break 값이 금지되어야 한다.
        const std::string src = R"(
            def main() -> i32 {
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
            def main(xs: i32[]) -> i32 {
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

    static bool test_diag_legacy_escape_token_rejected() {
        // legacy && escape 문법은 일반 구문 오류로 거부되어야 한다.
        const std::string src = R"(
            def main() -> i32 {
                set x = 1i32;
                set y = &&x;
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kUnexpectedToken) ||
                       p.bag.has_code(parus::diag::Code::kTypeNameExpected) ||
                       p.bag.has_code(parus::diag::Code::kExpectedToken),
            "legacy && must fail as a generic syntax error");
        ok &= require_(!p.bag.has_code(parus::diag::Code::kAmbiguousAmpPrefixChain),
            "legacy && failure must not rely on AmbiguousAmpPrefixChain");
        return ok;
    }

    static bool test_diag_call_arg_mix_not_allowed() {
        // 라벨 인자 이후 위치 인자를 두면 혼합 호출 진단이 나와야 한다.
        const std::string src = R"(
            def sub({ a: i32, b: i32, clamp: i32 = 0 }) -> i32 {
                return a - b + clamp;
            }
            def main() -> i32 {
                return sub(a: 1, b: 2, clamp: 1, 3);
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kCallArgMixNotAllowed),
            "mixed labeled/positional call must emit CallArgMixNotAllowed");
        return ok;
    }

    static bool test_diag_never_exposes_internal_infer_integer() {
        // 사용자 진단 문자열에는 내부 타입 토큰 "{integer}"가 노출되면 안 된다.
        const std::string src = R"(
            def add(a: i32, b: i32) -> i32 {
                return a + b;
            }
            def main() -> i32 {
                add(a: 1, b: 2);
                return 42;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kCallLabeledNotAllowedForPositionalFn),
            "positional-only function must reject labeled-only call form");

        bool leaked = false;
        for (const auto& d : p.bag.diags()) {
            const std::string msg = parus::diag::render_message(d, parus::diag::Language::kEn);
            if (msg.find("{integer}") != std::string::npos) {
                leaked = true;
                break;
            }
        }

        ok &= require_(!leaked, "user diagnostics must not expose internal '{integer}' token");
        return ok;
    }

    static bool test_diag_fn_named_group_mixed_with_positional_decl() {
        const std::string src = R"(
            def bad(a: i32, { b: i32 }) -> i32 {
                return a + b;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kFnNamedGroupMixedWithPositional),
            "mixed declaration form must emit FnNamedGroupMixedWithPositional");
        return ok;
    }

    static bool test_diag_named_group_only_rejects_positional_call() {
        const std::string src = R"(
            def only_ng({ a: i32, b: i32 = 0 }) -> i32 {
                return a + b;
            }
            def main() -> i32 {
                return only_ng(1, 2);
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kCallPositionalNotAllowedForNamedGroupFn),
            "named-group-only function must reject positional call form");
        return ok;
    }

    static bool test_diag_var_decl_name_expected() {
        // 변수 선언에서 이름이 빠지면 전용 진단이 나와야 한다.
        const std::string src = R"(
            def main() -> i32 {
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
            def main() -> i32 {
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
            def main() -> i32 {
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
            def main() -> i32 {
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
        // def 선언에는 함수 이름 식별자가 필요하다.
        const std::string src = R"(
            def (x: i32) -> i32 {
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
        // struct 멤버 선언은 이름 식별자가 필요하다.
        const std::string src = R"(
            struct P {
                i32;
            };
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kFieldMemberNameExpected),
            "missing struct member name must emit FieldMemberNameExpected");
        return ok;
    }

    static bool test_acts_for_parse_and_tyck_ok() {
        // acts for / operator(...) 기본 파싱과 tyck 경로가 동작해야 한다.
        const std::string src = R"(
            struct I32Box {
                v: i32;
            };

            acts for I32Box {
                def keep(self move) -> I32Box { return self; }
                operator(+)(self move, rhs: I32Box) -> I32Box { return self; }
            };
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        auto ty = run_tyck(p);

        bool ok = true;
        ok &= require_(!p.bag.has_code(parus::diag::Code::kActsForNotSupported),
            "acts for syntax must not emit legacy ActsForNotSupported");
        ok &= require_(ty.errors.empty(),
            "acts for / operator basic source should not produce tyck errors");
        return ok;
    }

    static bool test_diag_block_tail_expr_required() {
        // value-required block(if expr branch)에서 tail 식이 없으면 전용 진단이 나와야 한다.
        const std::string src = R"(
            def main() -> i32 {
                set x = if (true) {
                    set y = 1i32;
                } else {
                    2i32
                }
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
        // slice borrow 값에 ~를 적용하면 금지되어야 한다.
        const std::string src = R"(
            def main() -> i32 {
                let arr: i32[3] = [1, 2, 3];
                set s = &arr[0..:1];
                set h = ~s;
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);
        auto cap = run_cap(p, pres, ty);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!sir.cap.ok, "~ on slice borrow must fail SIR capability check");
        ok &= require_(p.bag.has_code(parus::diag::Code::kEscapeOperandMustNotBeBorrow),
            "~ on slice borrow must emit EscapeOperandMustNotBeBorrow");
        ok &= require_(cap.ok, "AST capability pass should stay as lightweight filter for this case");
        return ok;
    }

    static bool test_slice_const_oob_diagnostics() {
        const std::string src = R"(
            def main() -> i32 {
                let arr: i32[4] = [1, 2, 3, 4];
                set a = &arr[3..:1];
                set b = &arr[0..:10];
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        auto ty = run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kTypeSliceConstRangeInvalid),
            "invalid constant range must emit TypeSliceConstRangeInvalid");
        ok &= require_(p.bag.has_code(parus::diag::Code::kTypeSliceConstOutOfBounds),
            "constant upper bound OOB must emit TypeSliceConstOutOfBounds");
        ok &= require_(!ty.errors.empty(), "constant OOB slice source must fail typecheck");
        return ok;
    }

    static bool test_text_slicing_remains_unsupported() {
        const std::string src = R"(
            def main() -> i32 {
                let s: text = "abcd";
                set x = &s[1..2];
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        auto ty = run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kTypeIndexNonArray),
            "text slicing must remain rejected as non-array indexing");
        ok &= require_(!ty.errors.empty(), "text slicing source must fail typecheck");
        return ok;
    }

    static bool test_text_view_surface_and_borrow_deref_ok() {
        const std::string src = R"(
            def main() -> i32 {
                let s: text = "abc";
                let n: usize = s.len;
                let p: *const u8 = s.data;
                let arr: i32[4] = [1, 2, 3, 4];
                let k: usize = arr.len;
                let xs: i32[] = arr;
                let m: usize = xs.len;
                let q: *const i32 = xs.data;
                manual[abi] {
                    let t: text = text{ data: p, len: n };
                    let l: usize = t.len;
                }
                if (n == 3usize and k == 4usize and m == 4usize) {
                    return 42i32;
                }
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);
        auto cap = run_cap(p, pres, ty);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "text view surface source must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "text view surface source must not emit tyck errors");
        ok &= require_(cap.ok, "text view surface source must pass capability check");
        ok &= require_(sir.cap.ok, "text view surface source must pass SIR capability check");
        return ok;
    }

    static bool test_raw_ptr_deref_manual_gates() {
        const std::string read_src = R"(
            def main() -> i32 {
                let p: *const i32 = null;
                let x: i32 = *p;
                return x;
            }
        )";

        auto p_read = parse_program(read_src);
        (void)run_passes(p_read);
        auto ty_read = run_tyck(p_read);

        const std::string write_src = R"(
            def main() -> i32 {
                let p: *mut i32 = null;
                *p = 7i32;
                return 0i32;
            }
        )";

        auto p_write = parse_program(write_src);
        (void)run_passes(p_write);
        auto ty_write = run_tyck(p_write);

        bool ok = true;
        ok &= require_(p_read.bag.has_error(), "raw *const read without manual must emit diagnostics");
        ok &= require_(!ty_read.errors.empty(), "raw *const read without manual must fail typecheck");
        ok &= require_(p_write.bag.has_error(), "raw *const write without manual must emit diagnostics");
        ok &= require_(!ty_write.errors.empty(), "raw *const write without manual must fail typecheck");
        return ok;
    }

    static bool test_text_view_constructor_requires_manual_abi() {
        const std::string src = R"(
            def main() -> i32 {
                let p: *const u8 = null;
                let t: text = text{ data: p, len: 0usize };
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        auto ty = run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kManualAbiRequired),
            "text view constructor outside manual[abi] must emit ManualAbiRequired");
        ok &= require_(!ty.errors.empty(), "text view constructor outside manual[abi] must fail typecheck");
        return ok;
    }

    static bool test_legacy_ptr_type_syntax_rejected() {
        const std::string legacy_const_src = R"(
            def main() -> i32 {
                let p: ptr i32 = null;
                return 0i32;
            }
        )";
        auto legacy_const = parse_program(legacy_const_src);
        auto legacy_const_ty = run_tyck(legacy_const);

        const std::string legacy_mut_src = R"(
            def main() -> i32 {
                let p: ptr mut i32 = null;
                return 0i32;
            }
        )";
        auto legacy_mut = parse_program(legacy_mut_src);
        auto legacy_mut_ty = run_tyck(legacy_mut);

        const std::string bare_star_src = R"(
            def main() -> i32 {
                let p: *i32 = null;
                return 0i32;
            }
        )";
        auto bare = parse_program(bare_star_src);
        auto bare_ty = run_tyck(bare);

        bool ok = true;
        ok &= require_(legacy_const.bag.has_error(), "legacy ptr type syntax must fail parse/type");
        ok &= require_(legacy_mut.bag.has_error(), "legacy ptr mut type syntax must fail parse/type");
        ok &= require_(bare.bag.has_error(), "bare *T type syntax must fail parse/type");
        return ok;
    }

    static bool test_core_mem_and_hint_surface_ok() {
        const std::string attached_src = R"(
            $![Impl::Core];

            $![Impl::SizeOf]
            export def size_of<T>() -> usize;

            $![Impl::AlignOf]
            export def align_of<T>() -> usize;

            $![Impl::SpinLoop]
            export def spin_loop() -> void;
        )";

        const std::string ordinary_bodyless_src = R"(
            export def nope() -> void;
        )";

        const std::string impl_with_body_src = R"(
            $![Impl::SpinLoop]
            export def spin_loop() -> void {
            }
        )";

        const std::string impl_unknown_with_body_src = R"(
            $![Impl::Spawn]
            export def spawn() -> i32 {
                return 0i32;
            }
        )";

        auto attached = parse_program(attached_src);
        auto ordinary = parse_program(ordinary_bodyless_src);
        auto impl_with_body = parse_program(impl_with_body_src);
        auto impl_unknown_with_body = parse_program(impl_unknown_with_body_src);

        bool ok = true;
        ok &= require_(!attached.bag.has_error(), "attached Impl::* declarations must parse without syntax errors");
        ok &= require_(ordinary.bag.has_error(), "ordinary bodyless def must remain a syntax error");
        ok &= require_(!impl_with_body.bag.has_error(),
                       "recognized attached Impl::* declaration with body must remain syntactically valid");
        ok &= require_(!impl_unknown_with_body.bag.has_error(),
                       "unknown attached Impl::* declaration with body must remain syntactically valid");
        return ok;
    }

    static bool test_core_range_surface_ok() {
        const std::string src = R"(
            struct Range<T> {
                start: T;
                end: T;
            };

            struct RangeInclusive<T> {
                start: T;
                end: T;
            };

            def range_impl_<T>(start: T, end: T) with [T: Comparable] -> Range<T> {
                return Range<T>{ start: start, end: end };
            }

            def range_inclusive_impl_<T>(start: T, end: T) with [T: Comparable] -> RangeInclusive<T> {
                return RangeInclusive<T>{ start: start, end: end };
            }

            def range<T>(start: T, end: T) with [T: Comparable] -> Range<T> {
                return range_impl_<T>(start, end);
            }

            def range_inclusive<T>(start: T, end: T) with [T: Comparable] -> RangeInclusive<T> {
                return range_inclusive_impl_<T>(start, end);
            }

            def range_is_empty_<T>(start: T, end: T) with [T: Comparable] -> bool { return start >= end; }
            def range_contains_<T>(start: T, end: T, value: T) with [T: Comparable] -> bool {
                return value >= start and value < end;
            }
            def range_contains_range_<T>(start: T, end: T, other_start: T, other_end: T) with [T: Comparable] -> bool {
                return range_is_empty_<T>(other_start, other_end) or
                    (other_start >= start and other_end <= end);
            }
            def range_intersects_<T>(start: T, end: T, other_start: T, other_end: T) with [T: Comparable] -> bool {
                return not range_is_empty_<T>(start, end)
                    and not range_is_empty_<T>(other_start, other_end)
                    and start < other_end
                    and other_start < end;
            }
            def range_inclusive_is_empty_<T>(start: T, end: T) with [T: Comparable] -> bool { return start > end; }
            def range_inclusive_contains_<T>(start: T, end: T, value: T) with [T: Comparable] -> bool {
                return value >= start and value <= end;
            }
            def range_inclusive_contains_range_<T>(start: T, end: T, other_start: T, other_end: T) with [T: Comparable] -> bool {
                return range_inclusive_is_empty_<T>(other_start, other_end) or
                    (other_start >= start and other_end <= end);
            }
            def range_inclusive_intersects_<T>(start: T, end: T, other_start: T, other_end: T) with [T: Comparable] -> bool {
                return not range_inclusive_is_empty_<T>(start, end)
                    and not range_inclusive_is_empty_<T>(other_start, other_end)
                    and start <= other_end
                    and other_start <= end;
            }
            def range_inclusive_is_singleton_<T>(start: T, end: T) with [T: Comparable] -> bool {
                return not range_inclusive_is_empty_<T>(start, end) and start == end;
            }

            acts for Range<T> with [T: Comparable] {
                def is_empty(self) -> bool {
                    return range_is_empty_<T>(self.start, self.end);
                }

                def contains(self, value: T) -> bool {
                    return range_contains_<T>(self.start, self.end, value);
                }

                def contains_range(self, other: Range<T>) -> bool {
                    return range_contains_range_<T>(self.start, self.end, other.start, other.end);
                }

                def intersects(self, other: Range<T>) -> bool {
                    return range_intersects_<T>(self.start, self.end, other.start, other.end);
                }
            };

            acts for RangeInclusive<T> with [T: Comparable] {
                def is_empty(self) -> bool {
                    return range_inclusive_is_empty_<T>(self.start, self.end);
                }

                def contains(self, value: T) -> bool {
                    return range_inclusive_contains_<T>(self.start, self.end, value);
                }

                def contains_range(self, other: RangeInclusive<T>) -> bool {
                    return range_inclusive_contains_range_<T>(self.start, self.end, other.start, other.end);
                }

                def intersects(self, other: RangeInclusive<T>) -> bool {
                    return range_inclusive_intersects_<T>(self.start, self.end, other.start, other.end);
                }

                def is_singleton(self) -> bool {
                    return range_inclusive_is_singleton_<T>(self.start, self.end);
                }
            };

            def main() -> i32 {
                let a: Range<i32> = range(1i32, 4i32);
                let b: RangeInclusive<u32> = range_inclusive(1u32, 4u32);
                let c: Range<char> = range('a', 'z');
                let d: RangeInclusive<char> = range_inclusive('k', 'k');
                let e: Range<i32> = range(2i32, 3i32);
                let f: Range<i32> = range(4i32, 8i32);
                let x: bool = a.contains(2i32);
                let y: bool = a.contains(4i32);
                let z: bool = b.contains(4u32);
                let a_contains_range: bool = a.contains_range(e);
                let a_intersects_tail: bool = a.intersects(f);
                let c_has_mid: bool = c.contains('m');
                let c_has_hi: bool = c.contains('z');
                let d_singleton: bool = d.is_singleton();
                let d_intersects: bool = d.intersects(range_inclusive('k', 'm'));
                let ri_empty: bool = range_inclusive(5i32, 4i32).is_empty();
                if (x and not y and z and a_contains_range and not a_intersects_tail and
                    c_has_mid and not c_has_hi and d_singleton and d_intersects and ri_empty and
                    a.start == 1i32 and b.end == 4u32) {
                    return 42i32;
                }
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);
        auto cap = run_cap(p, pres, ty);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "core range surface source must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "core range surface source must not emit tyck errors");
        ok &= require_(cap.ok, "core range surface source must pass capability check");
        ok &= require_(sir.cap.ok, "core range surface source must pass SIR capability check");
        return ok;
    }

    static bool test_core_slice_surface_ok() {
        const std::string src = R"(
            def min_usize_(lhs: usize, rhs: usize) -> usize {
                if (lhs < rhs) { return lhs; }
                return rhs;
            }

            def len<T>(xs: T[]) -> usize {
                return xs.len;
            }

            def is_empty<T>(xs: T[]) -> bool {
                return xs.len == 0usize;
            }

            def as_ptr<T>(xs: T[]) -> *const T {
                return xs.data;
            }

            def subslice<T>(xs: T[], start: usize, end: usize) -> T[] {
                return xs[start..end];
            }

            def take_front<T>(xs: T[], n: usize) -> T[] {
                let clamped: usize = min_usize_(n, xs.len);
                return xs[0usize..clamped];
            }

            def drop_front<T>(xs: T[], n: usize) -> T[] {
                let clamped: usize = min_usize_(n, xs.len);
                return xs[clamped..xs.len];
            }

            def take_back<T>(xs: T[], n: usize) -> T[] {
                let clamped: usize = min_usize_(n, xs.len);
                return xs[xs.len - clamped..xs.len];
            }

            def drop_back<T>(xs: T[], n: usize) -> T[] {
                let clamped: usize = min_usize_(n, xs.len);
                return xs[0usize..xs.len - clamped];
            }

            def equal<T>(lhs: T[], rhs: T[]) with [T: Comparable] -> bool {
                if (lhs.len != rhs.len) {
                    return false;
                }
                set mut i = 0usize;
                while (i < lhs.len) {
                    if (not (lhs[i] == rhs[i])) {
                        return false;
                    }
                    i = i + 1usize;
                }
                return true;
            }

            def starts_with<T>(xs: T[], prefix: T[]) with [T: Comparable] -> bool {
                if (prefix.len > xs.len) {
                    return false;
                }
                set mut i = 0usize;
                while (i < prefix.len) {
                    if (not (xs[i] == prefix[i])) {
                        return false;
                    }
                    i = i + 1usize;
                }
                return true;
            }

            def ends_with<T>(xs: T[], suffix: T[]) with [T: Comparable] -> bool {
                if (suffix.len > xs.len) {
                    return false;
                }
                let offset: usize = xs.len - suffix.len;
                set mut i = 0usize;
                while (i < suffix.len) {
                    if (not (xs[offset + i] == suffix[i])) {
                        return false;
                    }
                    i = i + 1usize;
                }
                return true;
            }

            def main() -> i32 {
                let arr: i32[4] = [1i32, 2i32, 3i32, 4i32];
                let xs: i32[] = arr;
                let empty: i32[] = arr[0i32..0i32];
                let prefix: i32[] = arr[0i32..2i32];
                let middle: i32[] = arr[1i32..3i32];
                let suffix: i32[] = arr[2i32..4i32];
                let ptr: *const i32 = as_ptr(xs);
                let mid: i32[] = subslice(xs, 1usize, 3usize);
                let front: i32[] = take_front(xs, 2usize);
                let dropf: i32[] = drop_front(xs, 2usize);
                let back: i32[] = take_back(xs, 2usize);
                let dropb: i32[] = drop_back(xs, 2usize);
                let full: i32[] = take_front(xs, 99usize);
                let none: i32[] = drop_front(xs, 99usize);
                if (len(xs) == 4usize and is_empty(empty) and
                    equal(mid, middle) and equal(front, prefix) and equal(dropf, suffix) and
                    equal(back, suffix) and equal(dropb, prefix) and equal(full, xs) and is_empty(none) and
                    starts_with(xs, prefix) and ends_with(xs, suffix)) {
                    manual[abi] {
                        let q: *const i32 = ptr;
                    }
                    return 42i32;
                }
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);
        auto cap = run_cap(p, pres, ty);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "core slice surface source must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "core slice surface source must not emit tyck errors");
        ok &= require_(cap.ok, "core slice surface source must pass capability check");
        ok &= require_(sir.cap.ok, "core slice surface source must pass SIR capability check");
        return ok;
    }

    static bool test_core_slice_comparable_gate_rejected() {
        const std::string src = R"(
            def starts_with<T>(xs: T[], prefix: T[]) with [T: Comparable] -> bool {
                return true;
            }

            def main() -> bool {
                let xs: f32[2] = [1.0f32, 2.0f32];
                let prefix: f32[1] = [1.0f32];
                return starts_with(xs, prefix);
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        auto ty = run_tyck(p);

        bool ok = true;
        ok &= require_(!ty.errors.empty(), "slice helper on f32[] must fail Comparable gate");
        return ok;
    }

    static bool test_core_convert_surface_ok() {
        const std::string src = R"(
            proto Into<T> {
                require into(v: Self) -> T;
            };

            struct Wrapper : Into<i32> {
                value: i32;
            };

            acts for Wrapper {
                def into(self move) -> i32 {
                    return self.value;
                }
            };

            def into<T, U>(value: U) with [U: Into<T>] -> T {
                return value.into();
            }

            def main() -> i32 {
                let iv: i32 = into<i32, Wrapper>(Wrapper{ value: 5i32 });
                if (iv == 5i32) {
                    return 42i32;
                }
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        parus::passes::PassOptions opt{};
        opt.name_resolve.external_exports.push_back({
            .kind = parus::sema::SymbolKind::kFn,
            .path = "core::mem::replace",
            .link_name = "core::mem::replace",
            .declared_type = parus::ty::kInvalidType,
            .declared_type_repr = {},
            .declared_type_semantic = {},
            .decl_span = {},
            .decl_bundle_name = "core",
            .module_head = "mem",
            .decl_source_dir_norm = {},
            .is_export = true,
            .inst_payload = {},
        });
        opt.name_resolve.external_exports.push_back({
            .kind = parus::sema::SymbolKind::kFn,
            .path = "core::mem::swap",
            .link_name = "core::mem::swap",
            .declared_type = parus::ty::kInvalidType,
            .declared_type_repr = {},
            .declared_type_semantic = {},
            .decl_span = {},
            .decl_bundle_name = "core",
            .module_head = "mem",
            .decl_source_dir_norm = {},
            .is_export = true,
            .inst_payload = {},
        });
        opt.name_resolve.external_exports.push_back({
            .kind = parus::sema::SymbolKind::kFn,
            .path = "core::mem::take",
            .link_name = "core::mem::take",
            .declared_type = parus::ty::kInvalidType,
            .declared_type_repr = {},
            .declared_type_semantic = {},
            .decl_span = {},
            .decl_bundle_name = "core",
            .module_head = "mem",
            .decl_source_dir_norm = {},
            .is_export = true,
            .inst_payload = {},
        });
        auto pres = parus::passes::run_on_program(p.ast, p.root, p.bag, opt);
        auto ty = run_tyck(p, &pres.generic_prep, &pres.sym);
        auto cap = run_cap(p, pres, ty);
        auto sir = run_sir(p, pres, ty);
        auto oir = run_oir(p, pres, sir);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "core::convert sample must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "core::convert sample must typecheck");
        ok &= require_(cap.ok, "core::convert sample must pass capability check");
        ok &= require_(sir.verify_errors.empty(), "core::convert sample must produce valid SIR");
        ok &= require_(sir.cap.ok, "core::convert sample must pass SIR capability analysis");
        ok &= require_(sir.handle_verify_errors.empty(), "core::convert sample must pass handle verification");
        ok &= require_(oir.gate_passed, "core::convert sample must lower to valid OIR");
        return ok;
    }

    static bool test_core_convert_missing_member_rejected() {
        const std::string bad_sig_src = R"(
            proto Into<T> {
                require into(v: Self) -> T;
            };

            struct Bad : Into<i32> {
                value: i32;
            };

            acts for Bad {
                def into(self) -> i32 {
                    return self.value;
                }
            };
        )";

        const std::string bad_call_src = R"(
            proto Into<T> {
                require into(v: Self) -> T;
            };

            def into<T, U>(value: U) with [U: Into<T>] -> T {
                return value.into();
            }

            def main() -> i32 {
                return into<i32, bool>(true);
            }
        )";

        auto bad_sig_prog = parse_program(bad_sig_src);
        auto bad_sig_pres = run_passes(bad_sig_prog);
        auto bad_sig_ty = run_tyck(bad_sig_prog, &bad_sig_pres.generic_prep);

        auto bad_call_prog = parse_program(bad_call_src);
        auto bad_call_pres = run_passes(bad_call_prog);
        auto bad_call_ty = run_tyck(bad_call_prog, &bad_call_pres.generic_prep);

        bool ok = true;
        ok &= require_(bad_sig_prog.bag.has_code(parus::diag::Code::kProtoImplMissingMember),
                       "mismatched Into impl must report ProtoImplMissingMember");
        ok &= require_(!bad_sig_ty.errors.empty(), "mismatched Into impl must fail typecheck");
        ok &= require_(!bad_call_ty.errors.empty(), "calling constrained convert::into on bool must fail");
        return ok;
    }

    static bool test_applied_proto_constraints_across_with_sites_ok() {
        const std::string src = R"(
            proto Into<T> {
                require into(v: Self) -> T;
            };

            struct Wrapper : Into<i32> {
                value: i32;
            };

            acts for Wrapper {
                def into(self move) -> i32 {
                    return self.value;
                }
            };

            def convert<T>(value: T) with [T: Into<i32>] -> i32 {
                return value.into();
            }

            struct Token<T> with [T: Into<i32>] {
                raw: i32;
            };

            class Holder<T> with [T: Into<i32>] {
                value: T;
                init(v: T) {
                    self.value = v;
                }
            };

            acts for Token<T> with [T: Into<i32>] {
                def project(self) -> i32 {
                    return self.raw;
                }
            };

            def main() -> i32 {
                let v0: i32 = convert<Wrapper>(Wrapper{ value: 5i32 });
                let tk: Token<Wrapper> = Token<Wrapper>{ raw: 6i32 };
                let v1: i32 = tk.project();
                if (v0 == 5i32 and v1 == 6i32) {
                    return 42i32;
                }
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p, &pres.generic_prep);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "applied proto constraint sample must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "applied proto constraint sample must typecheck");
        return ok;
    }

    static bool test_generic_acts_owner_constraint_ok() {
        const std::string ok_src = R"(
            class Box<T> {
              value: T;
              init(v: T) {
                self.value = v;
              }
            };

            acts for Box<T> with [T: Comparable] {
              def same(self) -> bool {
                return self.value >= self.value;
              }
            };

            def main() -> bool {
              let b: Box<i32> = Box<i32>(1i32);
              return b.same();
            }
        )";

        const std::string err_src = R"(
            class Box<T> {
              value: T;
              init(v: T) {
                self.value = v;
              }
            };

            acts for Box<T> with [T: Comparable] {
              def same(self) -> bool {
                return self.value >= self.value;
              }
            };

            def main() -> bool {
              let b: Box<bool> = Box<bool>(true);
              return b.same();
            }
        )";

        auto ok_prog = parse_program(ok_src);
        auto ok_passes = run_passes(ok_prog);
        auto ok_ty = run_tyck(ok_prog, &ok_passes.generic_prep);

        auto err_prog = parse_program(err_src);
        auto err_passes = run_passes(err_prog);
        auto err_ty = run_tyck(err_prog, &err_passes.generic_prep);

        bool ok = true;
        ok &= require_(!ok_prog.bag.has_error(), "generic acts owner-constraint success sample must not emit diagnostics");
        ok &= require_(ok_ty.errors.empty(), "generic acts owner-constraint success sample must typecheck");
        ok &= require_(err_prog.bag.has_code(parus::diag::Code::kGenericDeclConstraintUnsatisfied),
                       "generic acts owner-constraint failure must report GenericDeclConstraintUnsatisfied");
        ok &= require_(!err_ty.errors.empty(), "generic acts owner-constraint failure must emit tyck errors");
        return ok;
    }

    static bool test_iter_proto_default_acts_satisfaction_ok() {
        const std::string ok_src = R"(
            proto Iterator {
                require type Item;
                require next(it: &mut Self, out: &mut Self::Item) -> bool;
            };

            struct CounterIter<T>: Iterator with [T == i32] {
                cur: T;
                end: T;
            };

            acts for CounterIter<T is Item> with [T == i32] {
                def next(mut self, out: &mut T) -> bool {
                    if (self.cur >= self.end) {
                        return false;
                    }
                    *out = self.cur;
                    self.cur = self.cur + 1i32;
                    return true;
                }
            };

            def main() -> i32 {
                set mut it = CounterIter<i32>{ cur: 1i32, end: 3i32 };
                set mut a = 0i32;
                set mut b = 0i32;
                let ok_a: bool = it.next(&mut a);
                let ok_b: bool = it.next(&mut b);
                if (ok_a and ok_b and a == 1i32 and b == 2i32) {
                    return 42i32;
                }
                return 0i32;
            }
        )";

        const std::string err_src = R"(
            proto Iterator {
                require type Item;
                require next(it: &mut Self, out: &mut Self::Item) -> bool;
            };

            struct CounterIter: Iterator {
                cur: i32;
                end: i32;
            };

            def main() -> i32 {
                return 0i32;
            }
        )";

        auto ok_prog = parse_program(ok_src);
        auto ok_pres = run_passes(ok_prog);
        auto ok_ty = run_tyck(ok_prog, &ok_pres.generic_prep);

        auto err_prog = parse_program(err_src);
        auto err_pres = run_passes(err_prog);
        auto err_ty = run_tyck(err_prog, &err_pres.generic_prep);

        bool ok = true;
        ok &= require_(!ok_prog.bag.has_error(), "default acts must satisfy Iterator requirement when signatures match");
        ok &= require_(ok_ty.errors.empty(), "Iterator default acts satisfaction success sample must typecheck");
        ok &= require_(err_prog.bag.has_code(parus::diag::Code::kProtoImplMissingMember),
                       "missing default acts method must report ProtoImplMissingMember");
        ok &= require_(!err_ty.errors.empty(), "missing default acts method must fail typecheck");
        return ok;
    }

    static bool test_core_iter_sequence_loop_bridge_ok() {
        const std::string src = R"(
            proto Iterator {
                require type Item;
                require next(it: &mut Self, out: &mut Self::Item) -> bool;
            };

            proto IterSourceMove {
                require type Item;
                require type Iter;
                require into_iter(src: Self) -> Self::Iter;
            };

            struct CounterIter<T>: Iterator with [T == i32] {
                cur: T;
                end: T;
            };

            acts for CounterIter<T is Item> with [T == i32] {
                def next(mut self, out: &mut T) -> bool {
                    if (self.cur >= self.end) {
                        return false;
                    }
                    *out = self.cur;
                    self.cur = self.cur + 1i32;
                    return true;
                }
            };

            struct Counter<T>: IterSourceMove with [T == i32] {
                start: T;
                end: T;
            };

            acts for Counter<T is Item, CounterIter<T> is Iter> with [T == i32] {
                def into_iter(self move) -> CounterIter<T> {
                    return CounterIter<T>{ cur: self.start, end: self.end };
                }
            };

            def main() -> i32 {
                set mut it = Counter<i32>{ start: 1i32, end: 4i32 }.into_iter();
                set mut a = 0i32;
                set mut b = 0i32;
                set mut c = 0i32;
                set mut d = 0i32;
                let ok_a: bool = it.next(&mut a);
                let ok_b: bool = it.next(&mut b);
                let ok_c: bool = it.next(&mut c);
                let ok_d: bool = it.next(&mut d);

                set mut sum = 0i32;
                loop (x in Counter<i32>{ start: 1i32, end: 4i32 }) {
                    set sum = sum + x;
                }

                set mut sum_for = 0i32;
                for (x in Counter<i32>{ start: 1i32, end: 4i32 }) {
                    set sum_for = sum_for + x;
                }

                if (ok_a and ok_b and ok_c and not ok_d and
                    a == 1i32 and b == 2i32 and c == 3i32 and
                    sum == 6i32 and sum_for == 6i32) {
                    return 42i32;
                }
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p, &pres.generic_prep);
        auto cap = run_cap(p, pres, ty);
        auto sir = run_sir(p, pres, ty);
        auto oir = run_oir(p, pres, sir);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "sequence loop bridge source must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "sequence loop bridge source must typecheck");
        ok &= require_(cap.ok, "sequence loop bridge source must pass capability check");
        ok &= require_(sir.verify_errors.empty(), "sequence loop bridge source must produce valid SIR");
        ok &= require_(sir.cap.ok, "sequence loop bridge source must pass SIR capability analysis");
        ok &= require_(sir.handle_verify_errors.empty(), "sequence loop bridge source must pass handle verification");
        ok &= require_(oir.gate_passed, "sequence loop bridge source must lower to valid OIR");
        return ok;
    }

    static bool test_acts_header_witness_syntax_enforced() {
        const std::string legacy_src = R"(
            proto Iterator {
                require type Item;
                require next(it: &mut Self, out: &mut Self::Item) -> bool;
            };

            struct CounterIter: Iterator {
                cur: i32;
                end: i32;
            };

            acts for CounterIter {
                type Item = i32;

                def next(mut self, out: &mut i32) -> bool {
                    return false;
                }
            };
        )";

        const std::string future_binder_src = R"(
            proto IterSourceMove {
                require type Item;
                require type Iter;
                require into_iter(src: Self) -> Self::Iter;
            };

            struct CounterIter<T> with [T == i32] {
                value: T;
            };

            struct Counter<T>: IterSourceMove with [T == i32] {
                value: T;
            };

            acts for Counter<CounterIter<T> is Iter, T is Item> with [T == i32] {
                def into_iter(self move) -> CounterIter<T> {
                    return CounterIter<T>{ value: self.value };
                }
            };
        )";

        auto legacy_prog = parse_program(legacy_src);
        auto legacy_pres = run_passes(legacy_prog);
        auto legacy_ty = run_tyck(legacy_prog, &legacy_pres.generic_prep);

        auto future_prog = parse_program(future_binder_src);
        (void)run_passes(future_prog);

        bool ok = true;
        ok &= require_(legacy_prog.bag.has_error(), "legacy acts body associated type syntax must be rejected");
        ok &= require_(!legacy_ty.errors.empty(), "legacy acts body associated type syntax must produce tyck error");
        ok &= require_(future_prog.bag.has_error(), "acts witness type must not reference future binders");
        return ok;
    }

    static bool test_class_private_visibility_enforced() {
        const std::string ok_src = R"(
            class SecretBox {
              private:
                value: i32;
                def secret(self) -> i32 {
                  return self.value;
                }

              public:
                init(v: i32) {
                  self.value = v;
                }

                def get(self) -> i32 {
                  return self.secret();
                }

                static def read_value(b: SecretBox) -> i32 {
                  return b.value;
                }
            }

            def main() -> i32 {
              let b: SecretBox = SecretBox(21i32);
              return b.get();
            }
        )";

        auto ok_prog = parse_program(ok_src);
        auto ok_pres = run_passes(ok_prog);
        auto ok_ty = run_tyck(ok_prog);
        auto ok_cap = run_cap(ok_prog, ok_pres, ok_ty);
        auto ok_sir = run_sir(ok_prog, ok_pres, ok_ty);

        const std::string bad_field_src = R"(
            class SecretBox {
              private:
                value: i32;

              public:
                init(v: i32) {
                  self.value = v;
                }
            }

            def main() -> i32 {
              let b: SecretBox = SecretBox(1i32);
              return b.value;
            }
        )";
        auto bad_field = parse_program(bad_field_src);
        (void)run_passes(bad_field);
        auto bad_field_ty = run_tyck(bad_field);

        const std::string bad_method_src = R"(
            class SecretBox {
              private:
                def secret(self) -> i32 {
                  return 7i32;
                }

              public:
                init() = default;
            }

            def main() -> i32 {
              let b: SecretBox = SecretBox();
              return b.secret();
            }
        )";
        auto bad_method = parse_program(bad_method_src);
        (void)run_passes(bad_method);
        auto bad_method_ty = run_tyck(bad_method);

        const std::string bad_acts_src = R"(
            class SecretBox {
              private:
                value: i32;

              public:
                init(v: i32) {
                  self.value = v;
                }
            }

            acts for SecretBox {
              def leak(self) -> i32 {
                return self.value;
              }
            }
        )";
        auto bad_acts = parse_program(bad_acts_src);
        (void)run_passes(bad_acts);
        auto bad_acts_ty = run_tyck(bad_acts);

        bool ok = true;
        ok &= require_(!ok_prog.bag.has_error(), "class private members must remain accessible inside declaring class");
        ok &= require_(ok_ty.errors.empty(), "class private members inside declaring class must not fail typecheck");
        ok &= require_(ok_cap.ok, "class private members inside declaring class must pass capability check");
        ok &= require_(ok_sir.cap.ok, "class private members inside declaring class must pass SIR capability check");

        ok &= require_(bad_field.bag.has_code(parus::diag::Code::kClassPrivateMemberAccessDenied),
            "external private field access must emit ClassPrivateMemberAccessDenied");
        ok &= require_(!bad_field_ty.errors.empty(), "external private field access must fail typecheck");

        ok &= require_(bad_method.bag.has_code(parus::diag::Code::kClassPrivateMemberAccessDenied),
            "external private method access must emit ClassPrivateMemberAccessDenied");
        ok &= require_(!bad_method_ty.errors.empty(), "external private method access must fail typecheck");

        ok &= require_(bad_acts.bag.has_code(parus::diag::Code::kClassPrivateMemberAccessDenied),
            "acts outside class must not access private class members");
        ok &= require_(!bad_acts_ty.errors.empty(), "acts outside class must fail typecheck on private member access");
        return ok;
    }

    static bool test_borrow_read_in_arithmetic_ok() {
        // &i32 파라미터를 산술식에서 읽기 값으로 사용할 수 있어야 한다.
        const std::string src = R"(
            def sum2(a: &i32, b: &i32) -> i32 {
                return a + b;
            }
            def main() -> i32 {
                let x: i32 = 10;
                let y: i32 = 20;
                set s = sum2(&x, &y);
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
            def inc(x: &mut i32) -> void {
                x = x + 1;
                return;
            }
            def main() -> i32 {
                set mut a = 1i32;
                inc(&mut a);
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
            def main() -> i32 {
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
            def main() -> i32 {
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
            def main() -> i32 {
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

    static bool test_escape_set_binding_commits_to_cell_ok() {
        // set-도입 local ~ binding도 이제 일반 cell commit 경로로 허용돼야 한다.
        const std::string src = R"(
            def sink(h: ~i32) -> i32 {
                return 0i32;
            }
            def main() -> i32 {
                let mut x: i32 = 1i32;
                set h = ~x;
                return sink(h);
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);
        auto cap = run_cap(p, pres, ty);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "set-introduced local ~ binding must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "set-introduced local ~ binding must typecheck");
        ok &= require_(cap.ok, "AST capability pass must allow local ~ cell commit");
        ok &= require_(sir.cap.ok, "SIR capability pass must allow local ~ cell commit");
        ok &= require_(sir.handle_verify_errors.empty(),
            "SIR handle verifier must accept local ~ cell commit");
        ok &= require_(sir.cap.cell_commit_count >= 1, "set-introduced local ~ binding must record a cell commit");
        return ok;
    }

    static bool test_static_allows_escape_storage() {
        // static place는 non-boundary 문맥에서도 ~를 허용해야 한다.
        const std::string src = R"(
            static G: i32 = 7i32;
            static mut HG: ~i32 = ~G;
            def main() -> i32 {
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);
        auto cap = run_cap(p, pres, ty);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "static + ~ source must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "static + ~ source must not emit tyck errors");
        ok &= require_(cap.ok, "static + ~ source must pass capability check");
        ok &= require_(sir.cap.ok, "static + ~ source must pass SIR capability check");
        return ok;
    }

    static bool test_consume_binding_static_optional_escape() {
        const std::string src = R"(
            proto Recoverable {};

            struct Logger {
                value: i32;
            };

            enum LogErr: Recoverable {
                case Missing,
            };

            static mut LOG: Logger? = null;

            def take_log?() -> ~Logger {
                let v: Logger = LOG else {
                    throw LogErr::Missing();
                };
                return ~v;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);
        auto cap = run_cap(p, pres, ty);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "consume-binding from static optional must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "consume-binding from static optional must typecheck");
        ok &= require_(cap.ok, "consume-binding from static optional must pass capability check");
        ok &= require_(sir.cap.ok, "consume-binding from static optional must pass SIR capability check");
        return ok;
    }

    static bool test_escape_first_class_places_accounting() {
        const std::string src = R"(
            proto Probe {
                provide def probe() -> i32 {
                    return 40i32;
                }
            };

            class Worker: Probe {
                value: i32;

                init(v: i32) {
                    self.value = v;
                }

                def run(self) -> i32 {
                    return self.value + 2i32;
                }
            };

            class Holder {
                value: ~Worker;

                init(v: ~Worker) {
                    self.value = v;
                }

                def run(self) -> i32 {
                    return self.value.run() + self.value->probe();
                }
            };

            def sink(h: ~Worker) -> i32 {
                return h.run() + h->probe();
            }

            def main() -> i32 {
                set a = Worker(5i32);
                let h: ~Worker = ~a;

                set b = Worker(7i32);
                let mut slot: (~Worker)? = ~b;
                let taken: ~Worker = slot else {
                    return 1i32;
                };

                set c = Worker(9i32);
                set holder = Holder(~c);
                return sink(h) + holder.run() + taken.run();
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p, &pres.generic_prep);
        auto cap = run_cap(p, pres, ty);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "first-class ~ place seed must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "first-class ~ place seed must not emit tyck errors");
        ok &= require_(cap.ok, "first-class ~ place seed must pass AST capability check");
        ok &= require_(sir.cap.ok, "first-class ~ place seed must pass SIR capability check");
        ok &= require_(sir.handle_verify_errors.empty(), "first-class ~ place seed must pass handle verification");
        if (!ok) return false;

        size_t cell_forms = 0;
        size_t abi_pack_forms = 0;
        for (const auto& h : sir.mod.escape_handles) {
            if (h.value_form == parus::sir::EscapeValueForm::kCell) {
                ++cell_forms;
                ok &= require_(h.cell_commit_count > 0, "cell-form escape handle must record cell commits");
                ok &= require_(h.abi_pack_count == 0, "cell-form escape handle must not record ABI packs");
            }
            if (h.value_form == parus::sir::EscapeValueForm::kAbiPack) {
                ++abi_pack_forms;
                ok &= require_(h.abi_pack_count > 0, "ABI-pack escape handle must record ABI packs");
                ok &= require_(h.cell_commit_count == 0, "ABI-pack escape handle must not record cell commits");
            }
        }
        ok &= require_(cell_forms >= 1, "first-class ~ place seed must produce at least one cell-form handle");
        ok &= require_(abi_pack_forms >= 1, "first-class ~ place seed must produce at least one ABI-pack handle");
        ok &= require_(sir.cap.cell_commit_count >= 1, "capability analysis must count cell commits");
        ok &= require_(sir.cap.abi_pack_count >= 1, "capability analysis must count ABI packs");
        return ok;
    }

    static bool test_escape_stored_handle_calls_stay_cell_only() {
        const std::string src = R"(
            class Worker {
                value: i32;

                init(v: i32) {
                    self.value = v;
                }

                def run(self) -> i32 {
                    return self.value + 1i32;
                }
            };

            struct Holder {
                value: ~Worker;
                spare: ~Worker;
            };

            def main() -> i32 {
                set a = Worker(1i32);
                set b = Worker(2i32);
                set left = ~a;
                set right = ~b;
                set holder = Holder{ value: left, spare: right };

                return left.run() + holder.value.run() + holder.spare.run();
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p, &pres.generic_prep);
        auto cap = run_cap(p, pres, ty);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "stored `~T` call seed must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "stored `~T` call seed must not emit tyck errors");
        ok &= require_(cap.ok, "stored `~T` call seed must pass AST capability check");
        ok &= require_(sir.cap.ok, "stored `~T` call seed must pass SIR capability check");
        ok &= require_(sir.handle_verify_errors.empty(), "stored `~T` call seed must pass handle verification");
        if (!ok) return false;

        ok &= require_(sir.cap.cell_commit_count >= 1, "stored `~T` call seed must still record cell commits");
        ok &= require_(sir.cap.abi_pack_count == 0, "stored `~T` calls must not introduce ABI packs");
        return ok;
    }

    static bool test_escape_sized_array_owner_cells() {
        const std::string src = R"(
            proto Probe {
                provide def probe() -> i32 {
                    return 40i32;
                }
            };

            class Worker: Probe {
                value: i32;

                init(v: i32) {
                    self.value = v;
                }

                def run(self) -> i32 {
                    return self.value + 1i32;
                }
            };

            class Pool {
                workers: (~Worker)[2];
                slots: ((~Worker)?)[2];

                init(a: ~Worker, b: ~Worker, c: ~Worker) {
                    self.workers = [a, b];
                    let empty_slots: ((~Worker)?)[2] = [null, null];
                    self.slots = empty_slots;
                    self.slots[0] ??= c;
                }

                def inspect(self) -> i32 {
                    return self.workers[0].run() + self.workers[1]->probe();
                }

                def take_slot(mut self) -> i32 {
                    let taken: ~Worker = self.slots[0] else {
                        return 1i32;
                    };
                    return taken.run();
                }
            };

            def make(seed: i32) -> ~Worker {
                set w = Worker(seed);
                return ~w;
            }

            def main() -> i32 {
                set local = [make(1i32), make(2i32)];

                set mut p = Pool(make(3i32), make(4i32), make(5i32));
                return local[0].run() + local[1]->probe() + p.inspect() + p.take_slot();
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p, &pres.generic_prep, &pres.sym);
        auto cap = run_cap(p, pres, ty);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "escape sized owner array seed must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "escape sized owner array seed must not emit tyck errors");
        ok &= require_(cap.ok, "escape sized owner array seed must pass AST capability check");
        ok &= require_(sir.cap.ok, "escape sized owner array seed must pass SIR capability check");
        ok &= require_(sir.handle_verify_errors.empty(), "escape sized owner array seed must pass handle verification");
        if (!ok) return false;

        return ok;
    }

    static bool test_escape_projected_owner_cells_and_named_aggregates() {
        const std::string src = R"(
            proto Probe {
                provide def probe() -> i32 {
                    return 40i32;
                }
            };

            class Worker: Probe {
                value: i32;

                init(v: i32) {
                    self.value = v;
                }

                def run(self) -> i32 {
                    return self.value + 1i32;
                }
            };

            class WorkerSlot {
                item: (~Worker)?;

                init(v: ~Worker) {
                    self.item = null;
                    self.item ??= v;
                }
            };

            class WorkerGroup {
                primary: ~Worker;
                slots: WorkerSlot[2];

                init(a: ~Worker, b: ~Worker, c: ~Worker) {
                    self.primary = a;
                    self.slots = [WorkerSlot(b), WorkerSlot(c)];
                }
            };

            class Pool {
                groups: WorkerGroup[2];

                init(a: ~Worker, b: ~Worker, c: ~Worker, d: ~Worker, e: ~Worker, f: ~Worker) {
                    self.groups = [WorkerGroup(a, b, c), WorkerGroup(d, e, f)];
                }

                def take(mut self) -> i32 {
                    let taken: ~Worker = self.groups[1].slots[1].item else {
                        return 1i32;
                    };
                    return self.groups[0].primary.run() + self.groups[0].primary->probe() + taken.run();
                }
            };

            def make(seed: i32) -> ~Worker {
                set w = Worker(seed);
                return ~w;
            }

            def main() -> i32 {
                set mut p = Pool(make(1i32), make(2i32), make(3i32), make(4i32), make(5i32), make(6i32));
                return p.take();
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p, &pres.generic_prep, &pres.sym);
        auto cap = run_cap(p, pres, ty);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "projected owner-cell seed must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "projected owner-cell seed must not emit tyck errors");
        ok &= require_(cap.ok, "projected owner-cell seed must pass AST capability check");
        ok &= require_(sir.cap.ok, "projected owner-cell seed must pass SIR capability check");
        ok &= require_(sir.handle_verify_errors.empty(), "projected owner-cell seed must pass handle verification");
        return ok;
    }

    static bool test_escape_sized_array_container_methods_surface() {
        const std::string src = R"(
            proto Probe {
                provide def probe() -> i32 {
                    return 20i32;
                }
            };

            class Worker: Probe {
                value: i32;

                init(v: i32) {
                    self.value = v;
                }

                def run(self) -> i32 {
                    return self.value + 1i32;
                }
            };

            class WorkerGroup {
                workers: (~Worker)[2];
                slots: ((~Worker)?)[2];

                init(a: ~Worker, b: ~Worker, c: ~Worker) {
                    self.workers = [a, b];
                    self.slots = [null, null];
                    let ignored: (~Worker)? = self.slots.put(1usize, c);
                }
            };

            class Pool {
                groups: WorkerGroup[1];
                plain: i32[2];

                init(a: ~Worker, b: ~Worker, c: ~Worker) {
                    self.groups = [WorkerGroup(a, b, c)];
                    self.plain = [10i32, 20i32];
                }

                def run(mut self) -> i32 {
                    self.plain.swap(0usize, 1usize);
                    self.groups[0].workers.swap(0usize, 1usize);
                    let old: ~Worker = self.groups[0].workers.replace(1usize, make(4i32));
                    set mut prev = self.groups[0].slots.put(1usize, old);
                    set mut taken_opt = self.groups[0].slots.take(1usize);
                    let taken: ~Worker = taken_opt else {
                        return 1i32;
                    };
                    let prev_worker: ~Worker = prev else {
                        return 2i32;
                    };
                    return self.plain[0usize] + self.groups[0].workers[0].run() +
                           self.groups[0].workers[1]->probe() + taken.run() + prev_worker.run();
                }
            };

            def make(seed: i32) -> ~Worker {
                set w = Worker(seed);
                return ~w;
            }

            def main() -> i32 {
                set mut p = Pool(make(1i32), make(2i32), make(3i32));
                return p.run();
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p, &pres.generic_prep, &pres.sym);
        auto cap = run_cap(p, pres, ty);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "sized array method surface seed must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "sized array method surface seed must not emit tyck errors");
        ok &= require_(cap.ok, "sized array method surface seed must pass AST capability check");
        ok &= require_(sir.cap.ok, "sized array method surface seed must pass SIR capability check");
        ok &= require_(sir.handle_verify_errors.empty(), "sized array method surface seed must pass handle verification");
        return ok;
    }

    static bool test_escape_named_aggregate_unsized_container_rejected() {
        const std::string src = R"(
            class Worker {
                value: i32;

                init(v: i32) {
                    self.value = v;
                }
            };

            class WorkerSlot {
                item: (~Worker)?;
            };

            class BadPool {
                slots: WorkerSlot[];
            };
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kTypeFieldMemberMustBePodBuiltin),
                       "unsized named aggregate owner container must be rejected by field policy");
        ok &= require_(!ty.errors.empty(), "unsized named aggregate owner container must fail tyck");
        (void)pres;
        return ok;
    }

    static bool test_sir_handle_verify_rejects_invalid_escape_pack_state() {
        // OIR 이전 단계에서는 call/return ABI pack 상태가 일관돼야 하며, 깨지면 verify가 실패해야 한다.
        const std::string src = R"(
            static G: i32 = 7i32;
            def sink(h: ~i32) -> i32 {
                return 0i32;
            }
            def main() -> i32 {
                return sink(~G);
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p);
        auto sir = run_sir(p, pres, ty);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "escape verify seed must parse/type-check cleanly");
        ok &= require_(ty.errors.empty(), "escape verify seed must not emit tyck errors");
        ok &= require_(sir.cap.ok, "escape verify seed must pass SIR capability check");
        ok &= require_(sir.handle_verify_errors.empty(), "escape verify seed must pass handle verify initially");
        ok &= require_(!sir.mod.escape_handles.empty(), "escape verify seed must produce at least one escape handle");
        if (!ok) return false;

        sir.mod.escape_handles[0].abi_pack_count = 0;
        const auto verrs = parus::sir::verify_escape_handles(sir.mod);
        ok &= require_(!verrs.empty(), "handle verify must fail when ABI pack accounting is inconsistent");

        bool has_pack_msg = false;
        for (const auto& e : verrs) {
            if (e.msg.find("abi_pack form requires") != std::string::npos) {
                has_pack_msg = true;
                break;
            }
        }
        ok &= require_(has_pack_msg, "handle verify must report ABI pack invariant violation");
        return ok;
    }

    static bool test_oir_gate_rejects_invalid_escape_handle() {
        // OIR lowering 진입 전 게이트는 invalid escape-handle pack state를 차단해야 한다.
        const std::string src = R"(
            static G: i32 = 7i32;
            def sink(h: ~i32) -> i32 {
                return 0i32;
            }
            def main() -> i32 {
                return sink(~G);
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

        sir.mod.escape_handles[0].abi_pack_count = 0;
        parus::oir::Builder ob(sir.mod, p.types, nullptr, &pres.sym);
        auto oir = ob.build();

        ok &= require_(!oir.gate_passed, "OIR gate must fail when escape handle verify fails");
        ok &= require_(!oir.gate_errors.empty(), "OIR gate must return at least one gate error");
        return ok;
    }

    static bool test_sir_mut_analysis_allows_mut_borrow_write_through() {
        // SIR mut-analysis는 &mut write-through를 불법 쓰기로 오검출하면 안 된다.
        const std::string src = R"(
            def inc(x: &mut i32) -> void {
                x = x + 1;
                return;
            }
            def main() -> i32 {
                set mut a = 1i32;
                inc(&mut a);
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
            def main() -> i64 {
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

        const auto& def = mod.funcs.front();
        ok &= require_(def.entry != parus::sir::k_invalid_block, "function entry block must exist");
        if (!ok) return false;

        const auto& entry = mod.blocks[def.entry];
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
            def f1() -> i32 {
                set mut n = 0i32;
                while (n < 1i32) {
                    n = n + 1i32;
                }
                return n;
            }

            def f2() -> i32 {
                set x = loop {
                    break 7i32;
                };
                return x;
            }

            def f3() -> i32 {
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

        auto find_func_by_name = [&](std::string_view name) -> const parus::sir::Func* {
            for (const auto& f : mod.funcs) {
                if (f.name == name) return &f;
            }
            return nullptr;
        };

        auto check_entry_stmt_kinds =
            [&](std::string_view fn_name, std::vector<parus::sir::StmtKind> expected) {
                const auto* def = find_func_by_name(fn_name);
                if (def == nullptr) return false;
                if (def->entry == parus::sir::k_invalid_block || (size_t)def->entry >= mod.blocks.size()) return false;
                const auto& b = mod.blocks[def->entry];
                if (b.stmt_count != expected.size()) return false;
                for (uint32_t i = 0; i < b.stmt_count; ++i) {
                    const uint32_t sid = b.stmt_begin + i;
                    if ((size_t)sid >= mod.stmts.size()) return false;
                    if (mod.stmts[sid].kind != expected[i]) return false;
                }
                return true;
            };

        ok &= require_(
            check_entry_stmt_kinds("f1", {parus::sir::StmtKind::kVarDecl, parus::sir::StmtKind::kWhileStmt, parus::sir::StmtKind::kReturn}),
            "f1 entry block stmt order must be [VarDecl, WhileStmt, Return]"
        );
        ok &= require_(
            check_entry_stmt_kinds("f2", {parus::sir::StmtKind::kVarDecl, parus::sir::StmtKind::kReturn}),
            "f2 entry block stmt order must be [VarDecl, Return]"
        );
        ok &= require_(
            check_entry_stmt_kinds("f3", {parus::sir::StmtKind::kVarDecl, parus::sir::StmtKind::kIfStmt}),
            "f3 entry block stmt order must be [VarDecl, IfStmt]"
        );
        return ok;
    }

    static bool test_c_abi_extern_export_ok() {
        const std::string src = R"(
            extern "C" def c_add(a: i32, b: i32) -> i32;
            extern "C" static mut errno: i32;

            export "C" def p_add(a: i32, b: i32) -> i32 {
                return a + b;
            }

            def main() -> i32 {
                return p_add(1i32, 2i32);
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        auto ty = run_tyck(p);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "valid C ABI declaration/source must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "valid C ABI declaration/source must not emit tyck errors");
        return ok;
    }

    static bool test_c_abi_reject_non_ffi_safe_type() {
        const std::string src = R"(
            extern "C" def bad_ref(x: &i32) -> i32;
            def main() -> i32 { return 0i32; }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kAbiCTypeNotFfiSafe),
            "C ABI function with borrow type must emit AbiCTypeNotFfiSafe");
        return ok;
    }

    static bool test_c_abi_reject_named_group() {
        const std::string src = R"(
            extern "C" def bad_ng({ a: i32, b: i32 }) -> i32;
            def main() -> i32 { return 0i32; }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kAbiCNamedGroupNotAllowed),
            "C ABI function with named-group must emit AbiCNamedGroupNotAllowed");
        return ok;
    }

    static bool test_c_abi_global_requires_static() {
        const std::string src = R"(
            extern "C" mut errno: i32;
            def main() -> i32 { return 0i32; }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kAbiCGlobalMustBeStatic),
            "C ABI global without static must emit AbiCGlobalMustBeStatic");
        return ok;
    }

    static bool test_c_abi_layout_c_field_ok() {
        const std::string src = R"(
            struct layout(c) align(16) Vec2 {
                x: f32;
                y: f32;
            };

            extern "C" def consume(v: Vec2) -> i32;

            def main() -> i32 { return 0i32; }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        auto ty = run_tyck(p);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "layout(c) struct in C ABI signature must pass diagnostics");
        ok &= require_(ty.errors.empty(), "layout(c) struct in C ABI signature must pass tyck");
        return ok;
    }

    static bool test_c_abi_reject_non_layout_field() {
        const std::string src = R"(
            struct Vec2 {
                x: f32;
                y: f32;
            };

            extern "C" def consume(v: Vec2) -> i32;
            def main() -> i32 { return 0i32; }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kAbiCTypeNotFfiSafe),
            "non-layout(c) struct in C ABI signature must emit AbiCTypeNotFfiSafe");
        return ok;
    }

    static bool test_field_export_disallowed() {
        const std::string src = R"(
            export struct layout(c) Vec2 {
                x: f32;
                y: f32;
            }
            def main() -> i32 { return 0i32; }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kUnexpectedToken),
            "export struct must be rejected by parser");
        return ok;
    }

    static bool test_var_mut_prefix_forbidden_on_set() {
        const std::string src = R"(
            def main() -> i32 {
                mut set a = 6i32;
                return a;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kVarMutMustFollowKw),
            "prefix 'mut set' must emit VarMutMustFollowKw");
        return ok;
    }

    static bool test_var_mut_prefix_forbidden_on_static() {
        const std::string src = R"(
            mut static G: i32 = 1i32;
            def main() -> i32 { return G; }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(p.bag.has_code(parus::diag::Code::kVarMutMustFollowKw),
            "prefix 'mut static' must emit VarMutMustFollowKw");
        return ok;
    }

    static size_t count_diag_code_(const parus::diag::Bag& bag, parus::diag::Code code) {
        size_t n = 0;
        for (const auto& d : bag.diags()) {
            if (d.code() == code) ++n;
        }
        return n;
    }

    static bool test_try_catch_ast_shape_parsed() {
        const std::string src = R"(
            def run?() -> i32 {
                try {
                    return 1i32;
                } catch (e) {
                    return 2i32;
                } catch (e2: i32) {
                    return 3i32;
                }
            }
        )";

        auto p = parse_program(src);

        bool found = false;
        bool shape_ok = false;
        for (const auto& s : p.ast.stmts()) {
            if (s.kind != parus::ast::StmtKind::kTryCatch) continue;
            found = true;

            const auto& clauses = p.ast.try_catch_clauses();
            const uint64_t begin = s.catch_clause_begin;
            const uint64_t end = begin + s.catch_clause_count;
            if (begin <= clauses.size() && end <= clauses.size() && s.catch_clause_count == 2) {
                const auto& c0 = clauses[begin + 0];
                const auto& c1 = clauses[begin + 1];
                shape_ok =
                    !c0.has_typed_bind &&
                    c1.has_typed_bind &&
                    c0.bind_name == "e" &&
                    c1.bind_name == "e2";
            }
            break;
        }

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "try-catch source must parse without diagnostics");
        ok &= require_(found, "AST must include TryCatch stmt");
        ok &= require_(shape_ok, "TryCatch AST must preserve catch clause slice/typed binder info");
        return ok;
    }

    static bool test_try_expr_is_unary_try() {
        const std::string src = R"(
            def load?() -> i32 {
                return 1i32;
            }

            def boot() -> i32 {
                set v = try load();
                return 0i32;
            }
        )";

        auto p = parse_program(src);

        bool found_try_unary = false;
        for (const auto& e : p.ast.exprs()) {
            if (e.kind == parus::ast::ExprKind::kUnary &&
                e.op == parus::syntax::TokenKind::kKwTry) {
                found_try_unary = true;
                break;
            }
        }

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "try-expr source must parse without diagnostics");
        ok &= require_(found_try_unary, "try expr must be represented as unary(kKwTry)");
        return ok;
    }

    static bool test_try_expr_operand_must_be_throwing_call_single_core() {
        const std::string src = R"(
            def plain() -> i32 {
                return 1i32;
            }

            def run() -> i32 {
                set v = try plain();
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(count_diag_code_(p.bag, parus::diag::Code::kTryExprOperandMustBeThrowingCall) == 1,
            "try expr over non-throwing call must emit TryExprOperandMustBeThrowingCall exactly once");
        return ok;
    }

    static bool test_generic_proto_target_arity_reports_once() {
        const std::string src = R"(
            proto Holder<T> {
                require get(v: T) -> T;
            };

            class Bad: Holder<i32, i32> {
                init() = default;
                def get(self, v: i32) -> i32 { return v; }
            };

            def main() -> i32 { return 0i32; }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(count_diag_code_(p.bag, parus::diag::Code::kGenericTypePathArityMismatch) >= 1,
            "generic proto arity mismatch must be reported at least once");
        ok &= require_(!p.bag.has_code(parus::diag::Code::kProtoImplTargetNotSupported),
            "generic proto arity mismatch must not cascade to ProtoImplTargetNotSupported");
        ok &= require_(!p.bag.has_code(parus::diag::Code::kProtoImplMissingMember),
            "generic proto arity mismatch must not cascade to ProtoImplMissingMember");
        return ok;
    }

    static bool test_generic_proto_target_not_found_reports_once() {
        const std::string src = R"(
            class MissingImpl: MissingProto<i32> {
                init() = default;
            };

            def main() -> i32 { return 0i32; }
        )";

        auto p = parse_program(src);
        (void)run_passes(p);
        (void)run_tyck(p);

        bool ok = true;
        ok &= require_(count_diag_code_(p.bag, parus::diag::Code::kGenericTypePathTemplateNotFound) == 1,
            "generic proto target-not-found must be reported exactly once");
        ok &= require_(!p.bag.has_code(parus::diag::Code::kProtoImplTargetNotSupported),
            "generic proto target-not-found must not cascade to ProtoImplTargetNotSupported");
        return ok;
    }

    static bool test_generic_type_eq_constraints_work() {
        const std::string src = R"(
            def only_i32<T>(x: T) with [T == i32] -> i32 {
                return 1i32;
            }

            def same<T, U>(x: T, y: U) with [T == U] -> bool {
                return true;
            }

            def main() -> i32 {
                let a: i32 = only_i32(1i32);
                let b: bool = same(1i32, 2i32);
                return a;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p, &pres.generic_prep);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "generic type equality success case must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "generic type equality success case must not emit tyck errors");
        return ok;
    }

    static bool test_generic_type_eq_constraint_mismatch_reports() {
        const std::string src = R"(
            def only_i32<T>(x: T) with [T == i32] -> i32 {
                return 1i32;
            }

            def main() -> i32 {
                return only_i32(1u32);
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        (void)run_tyck(p, &pres.generic_prep);

        bool ok = true;
        ok &= require_(count_diag_code_(p.bag, parus::diag::Code::kGenericConstraintTypeMismatch) == 1,
            "generic type equality mismatch must emit GenericConstraintTypeMismatch exactly once");
        return ok;
    }

    static bool test_generic_struct_constraint_checked_in_signature_types() {
        const std::string src = R"(
            struct OnlyI32<T> with [T == i32] {
                value: T;
            };

            def f(x: OnlyI32<u32>) -> i32 {
                return 0i32;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        (void)run_tyck(p, &pres.generic_prep);

        bool ok = true;
        ok &= require_(count_diag_code_(p.bag, parus::diag::Code::kGenericConstraintTypeMismatch) == 1,
            "generic struct equality mismatch in signature types must emit GenericConstraintTypeMismatch exactly once");
        return ok;
    }

    static bool test_mono_stats_dedup_repeated_generic_request() {
        const std::string src = R"(
            def id<T>(x: T) -> T {
                return x;
            }

            def main() -> i32 {
                let a: i32 = id(1i32);
                let b: i32 = id(2i32);
                return a + b;
            }
        )";

        auto p = parse_program(src);
        auto pres = run_passes(p);
        auto ty = run_tyck(p, &pres.generic_prep);

        bool ok = true;
        ok &= require_(!p.bag.has_error(), "mono-stats dedup case must not emit diagnostics");
        ok &= require_(ty.errors.empty(), "mono-stats dedup case must not emit tyck errors");
        ok &= require_(ty.mono_stats.unique_request_count == 1,
            "repeated identical generic request must count as one unique mono request");
        ok &= require_(ty.mono_stats.mono_cache_miss_count >= 1,
            "first generic instantiation must register a mono cache miss");
        ok &= require_(ty.mono_stats.mono_cache_hit_count >= 1,
            "repeated generic instantiation must register a mono cache hit");
        return ok;
    }

    static bool test_file_cases_directory() {
#ifndef PARUS_FRONTEND_CASE_DIR
        std::cerr << "  - PARUS_FRONTEND_CASE_DIR is not defined\n";
        return false;
#else
        std::vector<std::filesystem::path> case_dirs{
            std::filesystem::path{PARUS_FRONTEND_CASE_DIR},
        };
#ifdef PARUS_PARSER_CASE_DIR
        case_dirs.push_back(std::filesystem::path{PARUS_PARSER_CASE_DIR});
#endif

        bool ok = true;
        std::vector<std::filesystem::path> files;

        for (const auto& case_dir : case_dirs) {
            ok &= require_(std::filesystem::exists(case_dir), "case directory does not exist");
            ok &= require_(std::filesystem::is_directory(case_dir), "case directory path is not a directory");
            if (!ok) return false;

            for (const auto& entry : std::filesystem::directory_iterator(case_dir)) {
                if (!entry.is_regular_file()) continue;
                const auto& p = entry.path();
                if (p.extension() == ".pr") files.push_back(p);
            }
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
        bool (*def)();
    };

    const Case cases[] = {
        {"suffix_literals_work", test_suffix_literals_work},
        {"parser_aborted_guard_no_infinite_loop", test_parser_aborted_guard_no_infinite_loop},
        {"text_string_literal_typecheck_ok", test_text_string_literal_typecheck_ok},
        {"raw_and_format_triple_string_lex_parse_ok", test_raw_and_format_triple_string_lex_parse_ok},
        {"fstring_parts_and_escape_split_ok", test_fstring_parts_and_escape_split_ok},
        {"bare_dollar_string_removed_diag", test_bare_dollar_string_removed_diag},
        {"fstring_brace_error_diagnostics", test_fstring_brace_error_diagnostics},
        {"macro_string_payload_expr_ok", test_macro_string_payload_expr_ok},
        {"macro_block_payload_expr_ok", test_macro_block_payload_expr_ok},
        {"macro_block_payload_type_ok", test_macro_block_payload_type_ok},
        {"pipe_forward_chain_canonicalize_and_sir_pipecall_ok", test_pipe_forward_chain_canonicalize_and_sir_pipecall_ok},
        {"pipe_forward_rhs_must_be_call_error", test_pipe_forward_rhs_must_be_call_error},
        {"pipe_forward_hole_count_mismatch_error", test_pipe_forward_hole_count_mismatch_error},
        {"pipe_forward_positional_hole_rejected", test_pipe_forward_positional_hole_rejected},
        {"pipe_reverse_not_supported_yet", test_pipe_reverse_not_supported_yet},
        {"null_coalesce_assign_parsed_as_assign", test_null_coalesce_assign_parsed_as_assign},
        {"loop_expr_break_value_allowed", test_loop_expr_break_value_allowed},
        {"loop_expr_break_infer_context_propagates", test_loop_expr_break_infer_context_propagates},
        {"loop_expr_break_nested_statement_loop_isolated", test_loop_expr_break_nested_statement_loop_isolated},
        {"loop_expr_break_infer_context_negative_regressions", test_loop_expr_break_infer_context_negative_regressions},
        {"nullable_coalesce_rhs_context_propagates", test_nullable_coalesce_rhs_context_propagates},
        {"nullable_coalesce_rhs_context_negative_regressions", test_nullable_coalesce_rhs_context_negative_regressions},
        {"while_break_value_rejected", test_while_break_value_rejected},
        {"loop_header_var_name_resolved", test_loop_header_var_name_resolved},
        {"diag_legacy_escape_token_rejected", test_diag_legacy_escape_token_rejected},
        {"diag_call_arg_mix_not_allowed", test_diag_call_arg_mix_not_allowed},
        {"diag_never_exposes_internal_infer_integer", test_diag_never_exposes_internal_infer_integer},
        {"diag_fn_named_group_mixed_with_positional_decl", test_diag_fn_named_group_mixed_with_positional_decl},
        {"diag_named_group_only_rejects_positional_call", test_diag_named_group_only_rejects_positional_call},
        {"diag_var_decl_name_expected", test_diag_var_decl_name_expected},
        {"diag_set_initializer_required", test_diag_set_initializer_required},
        {"diag_var_initializer_expected", test_diag_var_initializer_expected},
        {"diag_cast_target_type_expected", test_diag_cast_target_type_expected},
        {"diag_fn_name_expected", test_diag_fn_name_expected},
        {"diag_field_member_name_expected", test_diag_field_member_name_expected},
        {"acts_for_parse_and_tyck_ok", test_acts_for_parse_and_tyck_ok},
        {"diag_block_tail_expr_required", test_diag_block_tail_expr_required},
        {"cap_escape_on_slice_borrow_rejected", test_cap_escape_on_slice_borrow_rejected},
        {"slice_const_oob_diagnostics", test_slice_const_oob_diagnostics},
        {"text_slicing_remains_unsupported", test_text_slicing_remains_unsupported},
        {"text_view_surface_and_borrow_deref_ok", test_text_view_surface_and_borrow_deref_ok},
        {"raw_ptr_deref_manual_gates", test_raw_ptr_deref_manual_gates},
        {"text_view_constructor_requires_manual_abi", test_text_view_constructor_requires_manual_abi},
        {"legacy_ptr_type_syntax_rejected", test_legacy_ptr_type_syntax_rejected},
        {"core_mem_and_hint_surface_ok", test_core_mem_and_hint_surface_ok},
        {"core_range_surface_ok", test_core_range_surface_ok},
        {"core_slice_surface_ok", test_core_slice_surface_ok},
        {"core_slice_comparable_gate_rejected", test_core_slice_comparable_gate_rejected},
        {"core_convert_surface_ok", test_core_convert_surface_ok},
        {"core_convert_missing_member_rejected", test_core_convert_missing_member_rejected},
        {"applied_proto_constraints_across_with_sites_ok", test_applied_proto_constraints_across_with_sites_ok},
        {"generic_acts_owner_constraint_ok", test_generic_acts_owner_constraint_ok},
        {"iter_proto_default_acts_satisfaction_ok", test_iter_proto_default_acts_satisfaction_ok},
        {"core_iter_sequence_loop_bridge_ok", test_core_iter_sequence_loop_bridge_ok},
        {"acts_header_witness_syntax_enforced", test_acts_header_witness_syntax_enforced},
        {"class_private_visibility_enforced", test_class_private_visibility_enforced},
        {"borrow_read_in_arithmetic_ok", test_borrow_read_in_arithmetic_ok},
        {"mut_borrow_write_through_assignment_ok", test_mut_borrow_write_through_assignment_ok},
        {"cap_shared_conflict_with_mut", test_cap_shared_conflict_with_mut},
        {"cap_mut_conflict_with_shared", test_cap_mut_conflict_with_shared},
        {"cap_shared_write_conflict", test_cap_shared_write_conflict},
        {"escape_set_binding_commits_to_cell_ok", test_escape_set_binding_commits_to_cell_ok},
        {"static_allows_escape_storage", test_static_allows_escape_storage},
        {"consume_binding_static_optional_escape", test_consume_binding_static_optional_escape},
        {"escape_first_class_places_accounting", test_escape_first_class_places_accounting},
        {"escape_stored_handle_calls_stay_cell_only", test_escape_stored_handle_calls_stay_cell_only},
        {"escape_sized_array_owner_cells", test_escape_sized_array_owner_cells},
        {"escape_projected_owner_cells_and_named_aggregates", test_escape_projected_owner_cells_and_named_aggregates},
        {"escape_sized_array_container_methods_surface", test_escape_sized_array_container_methods_surface},
        {"escape_named_aggregate_unsized_container_rejected", test_escape_named_aggregate_unsized_container_rejected},
        {"sir_handle_verify_rejects_invalid_escape_pack_state", test_sir_handle_verify_rejects_invalid_escape_pack_state},
        {"oir_gate_rejects_invalid_escape_handle", test_oir_gate_rejects_invalid_escape_handle},
        {"sir_mut_analysis_allows_mut_borrow_write_through", test_sir_mut_analysis_allows_mut_borrow_write_through},
        {"sir_uses_symbol_declared_type_for_set", test_sir_uses_symbol_declared_type_for_set},
        {"sir_control_flow_block_layout", test_sir_control_flow_block_layout},
        {"c_abi_extern_export_ok", test_c_abi_extern_export_ok},
        {"c_abi_reject_non_ffi_safe_type", test_c_abi_reject_non_ffi_safe_type},
        {"c_abi_reject_named_group", test_c_abi_reject_named_group},
        {"c_abi_global_requires_static", test_c_abi_global_requires_static},
        {"c_abi_layout_c_field_ok", test_c_abi_layout_c_field_ok},
        {"c_abi_reject_non_layout_field", test_c_abi_reject_non_layout_field},
        {"field_export_disallowed", test_field_export_disallowed},
        {"var_mut_prefix_forbidden_on_set", test_var_mut_prefix_forbidden_on_set},
        {"var_mut_prefix_forbidden_on_static", test_var_mut_prefix_forbidden_on_static},
        {"try_catch_ast_shape_parsed", test_try_catch_ast_shape_parsed},
        {"try_expr_is_unary_try", test_try_expr_is_unary_try},
        {"try_expr_operand_must_be_throwing_call_single_core", test_try_expr_operand_must_be_throwing_call_single_core},
        {"generic_proto_target_arity_reports_once", test_generic_proto_target_arity_reports_once},
        {"generic_proto_target_not_found_reports_once", test_generic_proto_target_not_found_reports_once},
        {"generic_type_eq_constraints_work", test_generic_type_eq_constraints_work},
        {"generic_type_eq_constraint_mismatch_reports", test_generic_type_eq_constraint_mismatch_reports},
        {"generic_struct_constraint_checked_in_signature_types", test_generic_struct_constraint_checked_in_signature_types},
        {"mono_stats_dedup_repeated_generic_request", test_mono_stats_dedup_repeated_generic_request},
        {"file_cases_directory", test_file_cases_directory},
    };

    int failed = 0;
    for (const auto& tc : cases) {
        std::cout << "[TEST] " << tc.name << "\n";
        const bool ok = tc.def();
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
