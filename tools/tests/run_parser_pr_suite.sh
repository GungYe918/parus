#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
BUILD_DIR="${1:-${ROOT_DIR}/build}"
FUZZ_TIME="${PARSER_PR_FUZZ_MAX_TIME:-60}"

ctest --test-dir "${BUILD_DIR}" --output-on-failure -R "^(parus_parser_tests|parus_frontend_integration_tests|parus_parser_stress_tests|parus_parser_incremental_merge_tests|parus_parser_crash_tests|parus_parser_ast_golden_tests)$"

FUZZ_SMOKE_BIN="${BUILD_DIR}/tests/parus_fuzz_parser_smoke"
FUZZ_INC_BIN="${BUILD_DIR}/tests/parus_fuzz_parser_incremental"
FUZZ_ARTIFACT_DIR="${PARUS_FUZZ_ARTIFACT_DIR:-/tmp/parus_fuzz_artifacts}"

PARSER_CORPUS="${ROOT_DIR}/tests/fuzz/corpus/parser_seed"
INCR_CORPUS="${ROOT_DIR}/tests/fuzz/corpus/incremental_seed"
mkdir -p "${FUZZ_ARTIFACT_DIR}"

if [[ -x "${FUZZ_SMOKE_BIN}" ]]; then
  if ! "${FUZZ_SMOKE_BIN}" "${PARSER_CORPUS}" -artifact_prefix="${FUZZ_ARTIFACT_DIR}/" -max_total_time="${FUZZ_TIME}" -print_final_stats=1; then
    echo "[warn] parus_fuzz_parser_smoke reported a crash (non-gating in PR suite)"
  fi
else
  echo "[skip] parus_fuzz_parser_smoke not built"
fi

if [[ -x "${FUZZ_INC_BIN}" ]]; then
  if ! "${FUZZ_INC_BIN}" "${INCR_CORPUS}" -artifact_prefix="${FUZZ_ARTIFACT_DIR}/" -max_total_time="${FUZZ_TIME}" -print_final_stats=1; then
    echo "[warn] parus_fuzz_parser_incremental reported a crash (non-gating in PR suite)"
  fi
else
  echo "[skip] parus_fuzz_parser_incremental not built"
fi

echo "parser PR suite completed"
