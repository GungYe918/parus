# Parus Parser Test Strategy

Status: Implemented (v1 split)
Audience: frontend/parser contributors

## Goals

1. Validate parser correctness independently from semantic passes.
2. Verify recovery continuity (suffix declarations survive malformed regions).
3. Keep frontend integration regression coverage unchanged in a separate suite.
4. Add deterministic incremental parse stress checks.

## Test Suite Layout

1. `parus_parser_tests` (parser-only)
   - File corpus: `tests/parser_cases/*.pr`
   - Parses only (`Lexer` + `Parser::parse_program`)
   - No pass/tyck/cap/sir invocation
2. `parus_frontend_integration_tests`
   - Full frontend pipeline regression
   - Corpus: `tests/frontend_cases/*.pr` and `tests/parser_cases/*.pr`
3. `parus_parser_stress_tests`
   - Deterministic random edit sequences
   - Compares incremental parse with full rebuild
4. `parus_parser_crash_tests`
   - No-crash/no-hang regression corpus
   - Corpus: `tests/parser_crashes/*.pr`

## Case Naming Policy

Parser case files use one of:

1. `ok_*.pr`
2. `warn_*.pr`
3. `err_*.pr`

Legacy `case*.pr` files are migrated by:

- `tools/tests/migrate_case_prefix.py`
- Report: `tests/parser_cases/MIGRATION_REPORT.md`

## In-file Expectation Directives

Parser-only runner supports directive overrides:

1. `//@expect-error <DiagCodeName>`
2. `//@expect-warning <DiagCodeName>`
3. `//@expect-no-parser-error`

Priority:

1. If any directive is present, directive-based assertions are used.
2. Otherwise, file prefix fallback (`ok_/warn_/err_`) is used.

## Parser vs Integration Corpus Policy

1. Parser-only syntax/recovery corpus: `tests/parser_cases/`
2. Post-parse semantic regressions: `tests/frontend_cases/`
3. Crash minimization corpus: `tests/parser_crashes/`

## PR / Nightly Entry Points

1. PR profile (~3 min budget): `tools/tests/run_parser_pr_suite.sh`
2. Nightly profile: `tools/tests/run_parser_nightly_suite.sh`

## Failure Artifacts

Incremental mismatch artifacts are written to:

- `/tmp/parus_parser_stress_failures/`

Each artifact includes:

1. mismatch reason
2. incremental/full diagnostic multisets
3. top-level shape
4. AST fingerprint
5. source snapshot
