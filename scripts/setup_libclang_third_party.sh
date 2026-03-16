#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
OUT_DIR="${ROOT_DIR}/third_party/libclang"

detect_llvm_prefix() {
  if [[ -n "${LLVM_PREFIX:-}" && -d "${LLVM_PREFIX}" ]]; then
    echo "${LLVM_PREFIX}"
    return 0
  fi

  local candidates=(
    "/opt/homebrew/opt/llvm"
    "/usr/local/opt/llvm"
    "/usr/lib/llvm-21"
    "/usr/lib/llvm-20"
  )
  for p in "${candidates[@]}"; do
    if [[ -d "${p}/include/clang-c" ]]; then
      echo "${p}"
      return 0
    fi
  done
  return 1
}

LLVM_ROOT="$(detect_llvm_prefix || true)"
if [[ -z "${LLVM_ROOT}" ]]; then
  echo "error: could not find LLVM install with clang-c headers."
  echo "hint: set LLVM_PREFIX=/path/to/llvm"
  exit 1
fi

mkdir -p "${OUT_DIR}/include"
rsync -a --delete "${LLVM_ROOT}/include/clang-c/" "${OUT_DIR}/include/clang-c/"

if [[ -f "${LLVM_ROOT}/LICENSE.TXT" ]]; then
  cp "${LLVM_ROOT}/LICENSE.TXT" "${OUT_DIR}/LICENSE.TXT"
fi

echo "libclang third_party headers synced."
echo "  llvm root: ${LLVM_ROOT}"
echo "  output   : ${OUT_DIR}"
echo
echo "cmake hint:"
echo "  -DPARUS_ENABLE_CIMPORT=ON -DPARUS_LIBCLANG_ROOT=${OUT_DIR}"
