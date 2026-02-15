#!/usr/bin/env bash
set -euo pipefail

BUILD_TYPE="${1:-Debug}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

GENERATOR="Unix Makefiles"
if command -v ninja >/dev/null 2>&1; then
  GENERATOR="Ninja"
fi

# Build compiler toolchain: prefer system clang++ for ABI stability.
if [ -x "/usr/bin/clang++" ]; then
  CXX_COMPILER="/usr/bin/clang++"
elif command -v clang++ >/dev/null 2>&1; then
  CXX_COMPILER="clang++"
else
  CXX_COMPILER="c++"
fi

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -G "${GENERATOR}" \
  -DCMAKE_CXX_COMPILER="${CXX_COMPILER}" \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DPARUS_BUILD_TESTS=ON \
  -DPARUS_ENABLE_BACKEND=ON \
  -DPARUS_ENABLE_AOT_BACKEND=ON \
  -DPARUS_AOT_ENABLE_LLVM=ON \
  -DPARUS_LLVM_VERSION=20 \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON

# Stage1: frontend/backend libraries
cmake --build "${BUILD_DIR}" --target parus_stage1 -j16
# Stage2: parusc tool (driver + internal compiler)
cmake --build "${BUILD_DIR}" --target parus_stage2 -j16

echo ""
echo "Built successfully (stage1 + stage2)."
echo "Run: ${BUILD_DIR}/compiler/parusc/parusc"
