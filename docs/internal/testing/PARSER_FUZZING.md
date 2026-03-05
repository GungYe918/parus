# Parus Parser Fuzzing

Status: Implemented (optional build)

## Build Option

Enable fuzz targets:

```sh
cmake -S . -B build -DPARUS_BUILD_TESTS=ON -DPARUS_BUILD_FUZZERS=ON
cmake --build build --target parus_fuzz_parser_smoke parus_fuzz_parser_incremental -j8
```

Notes:

1. Requires clang/libFuzzer toolchain.
2. Default is OFF (`PARUS_BUILD_FUZZERS=OFF`).

## Targets

1. `parus_fuzz_parser_smoke`
   - Input -> `Lexer` + `Parser`
   - Goal: crash/hang/UB detection in parser core
2. `parus_fuzz_parser_incremental`
   - Input drives edit sequence over `IncrementalParserSession`
   - Goal: crash/hang/UB detection on reparse path

## Seed Corpus

1. `tests/fuzz/corpus/parser_seed/`
2. `tests/fuzz/corpus/incremental_seed/`

## PR Smoke Run

Use:

```sh
tools/tests/run_parser_pr_suite.sh
```

If fuzz binaries exist, each runs with:

- `-max_total_time=60`
- non-zero exits are reported as warnings (non-gating) to keep PR latency stable.

Override PR fuzz duration:

```sh
PARSER_PR_FUZZ_MAX_TIME=10 tools/tests/run_parser_pr_suite.sh
```

Fuzzer crash artifacts are written to:

- `/tmp/parus_fuzz_artifacts/` (override with `PARUS_FUZZ_ARTIFACT_DIR`)

## Nightly Run

Use:

```sh
tools/tests/run_parser_nightly_suite.sh
```

Override fuzz duration:

```sh
PARSER_FUZZ_MAX_TIME=1800 tools/tests/run_parser_nightly_suite.sh
```

Nightly also reports fuzzer crashes as warnings while preserving crash artifacts for triage.

## Crash Promotion Workflow

1. Keep crashing input as minimized `.pr` sample.
2. Add it to `tests/parser_crashes/`.
3. Ensure `parus_parser_crash_tests` reproduces before fix.
4. Land fix and keep regression sample permanently.
