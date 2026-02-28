#!/usr/bin/env bash
set -euo pipefail

BUILD_TYPE="${1:-Debug}"
ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${ROOT_DIR}/build"

GENERATOR="Unix Makefiles"
if command -v ninja >/dev/null 2>&1; then
  GENERATOR="Ninja"
fi

LLVM_LANE="${PARUS_LLVM_VERSION:-20}"
if [ "${LLVM_LANE}" != "20" ] && [ "${LLVM_LANE}" != "21" ]; then
  echo "Unsupported PARUS_LLVM_VERSION='${LLVM_LANE}'. Supported lanes: 20, 21."
  exit 1
fi

llvm_major_of() {
  local cfg="$1"
  local version
  version="$("${cfg}" --version 2>/dev/null || true)"
  echo "${version%%.*}"
}

resolve_candidate() {
  local cand="$1"
  if [ -x "${cand}" ]; then
    echo "${cand}"
    return 0
  fi
  if command -v "${cand}" >/dev/null 2>&1; then
    command -v "${cand}"
    return 0
  fi
  return 1
}

LLVM_CONFIG_BIN=""
if [ -n "${PARUS_LLVM_CONFIG_EXECUTABLE:-}" ]; then
  LLVM_CONFIG_BIN="$(resolve_candidate "${PARUS_LLVM_CONFIG_EXECUTABLE}")"
fi

if [ -z "${LLVM_CONFIG_BIN}" ]; then
  if [ "${LLVM_LANE}" = "20" ]; then
    CANDIDATES=(
      "/opt/homebrew/opt/llvm@20/bin/llvm-config"
      "/usr/local/opt/llvm@20/bin/llvm-config"
      "llvm-config"
    )
  else
    CANDIDATES=(
      "/opt/homebrew/opt/llvm/bin/llvm-config"
      "/opt/homebrew/opt/llvm@21/bin/llvm-config"
      "/usr/local/opt/llvm@21/bin/llvm-config"
      "llvm-config"
    )
  fi

  for cand in "${CANDIDATES[@]}"; do
    if resolved="$(resolve_candidate "${cand}" 2>/dev/null)"; then
      if [ "$(llvm_major_of "${resolved}")" = "${LLVM_LANE}" ]; then
        LLVM_CONFIG_BIN="${resolved}"
        break
      fi
    fi
  done
fi

if [ -z "${LLVM_CONFIG_BIN}" ]; then
  echo "Failed to find llvm-config for lane ${LLVM_LANE}."
  echo "Set PARUS_LLVM_CONFIG_EXECUTABLE explicitly (e.g. /opt/homebrew/opt/llvm@${LLVM_LANE}/bin/llvm-config)."
  exit 1
fi

if [ "$(llvm_major_of "${LLVM_CONFIG_BIN}")" != "${LLVM_LANE}" ]; then
  echo "llvm-config lane mismatch:"
  echo "  requested lane : ${LLVM_LANE}"
  echo "  llvm-config    : ${LLVM_CONFIG_BIN}"
  echo "  detected major : $(llvm_major_of "${LLVM_CONFIG_BIN}")"
  exit 1
fi

LLVM_PREFIX="$("${LLVM_CONFIG_BIN}" --prefix)"
LLVM_USE_TOOLCHAIN="ON"
LLVM_REQUIRE_TOOLCHAIN="ON"

C_COMPILER="${LLVM_PREFIX}/bin/clang"
CXX_COMPILER="${LLVM_PREFIX}/bin/clang++"
if [ ! -x "${C_COMPILER}" ] || [ ! -x "${CXX_COMPILER}" ]; then
  echo "Failed to find LLVM lane clang toolchain under:"
  echo "  ${LLVM_PREFIX}/bin/clang"
  echo "  ${LLVM_PREFIX}/bin/clang++"
  echo "Install LLVM lane ${LLVM_LANE} (brew llvm@${LLVM_LANE}) or set PARUS_LLVM_CONFIG_EXECUTABLE correctly."
  exit 1
fi

export CC="${C_COMPILER}"
export CXX="${CXX_COMPILER}"

CMAKE_EXTRA_ARGS=(
  "-DCMAKE_PREFIX_PATH=${LLVM_PREFIX}"
  "-DPARUS_LLVM_CONFIG_EXECUTABLE=${LLVM_CONFIG_BIN}"
)

HAS_LLD_LINKER=0
if [ -x "${LLVM_PREFIX}/bin/ld64.lld" ] || [ -x "${LLVM_PREFIX}/bin/ld.lld" ]; then
  HAS_LLD_LINKER=1
elif command -v ld64.lld >/dev/null 2>&1 || command -v ld.lld >/dev/null 2>&1; then
  HAS_LLD_LINKER=1
fi

if [ "${HAS_LLD_LINKER}" -eq 1 ]; then
  CMAKE_EXTRA_ARGS+=(
    "-DCMAKE_EXE_LINKER_FLAGS=-fuse-ld=lld"
    "-DCMAKE_SHARED_LINKER_FLAGS=-fuse-ld=lld"
    "-DCMAKE_MODULE_LINKER_FLAGS=-fuse-ld=lld"
  )
fi

CCACHE_BIN=""
if [ -x "/opt/homebrew/bin/ccache" ]; then
  CCACHE_BIN="/opt/homebrew/bin/ccache"
elif command -v ccache >/dev/null 2>&1; then
  CCACHE_BIN="$(command -v ccache)"
fi

if [ -n "${CCACHE_BIN}" ]; then
  export CCACHE_DIR="${ROOT_DIR}/.ccache"
  mkdir -p "${CCACHE_DIR}"
  "${CCACHE_BIN}" -M 10G >/dev/null || true
  CMAKE_EXTRA_ARGS+=(
    "-DCMAKE_C_COMPILER_LAUNCHER=${CCACHE_BIN}"
    "-DCMAKE_CXX_COMPILER_LAUNCHER=${CCACHE_BIN}"
  )
fi

if [ -d "${BUILD_DIR}" ]; then
  rm -rf "${BUILD_DIR}"
fi

cmake -S "${ROOT_DIR}" -B "${BUILD_DIR}" \
  -G "${GENERATOR}" \
  -DCMAKE_C_COMPILER="${C_COMPILER}" \
  -DCMAKE_CXX_COMPILER="${CXX_COMPILER}" \
  -DCMAKE_AR="${LLVM_PREFIX}/bin/llvm-ar" \
  -DCMAKE_RANLIB="${LLVM_PREFIX}/bin/llvm-ranlib" \
  -DCMAKE_TRY_COMPILE_TARGET_TYPE=STATIC_LIBRARY \
  -DCMAKE_BUILD_TYPE="${BUILD_TYPE}" \
  -DPARUS_BUILD_TESTS=ON \
  -DPARUS_ENABLE_BACKEND=ON \
  -DPARUS_ENABLE_AOT_BACKEND=ON \
  -DPARUS_AOT_ENABLE_LLVM=ON \
  -DPARUS_LLVM_USE_TOOLCHAIN="${LLVM_USE_TOOLCHAIN}" \
  -DPARUS_LLVM_VERSION="${LLVM_LANE}" \
  -DPARUS_LLVM_REQUIRE_TOOLCHAIN="${LLVM_REQUIRE_TOOLCHAIN}" \
  -DCMAKE_EXPORT_COMPILE_COMMANDS=ON \
  "${CMAKE_EXTRA_ARGS[@]}"

cmake --build "${BUILD_DIR}" --target parus_stage1 -j16
cmake --build "${BUILD_DIR}" --target parus_stage2 -j16
cmake --build "${BUILD_DIR}" --target parus_parser_tests parus_oir_tests parus_cli_tests parus_config_tests parus_oir_llvm_tests parus_ffi_tests parus_parlib_tests -j16

echo ""
echo "vscode-parus npm validation"
if ! command -v npm >/dev/null 2>&1; then
  echo "npm is required for vscode-parus validation but was not found in PATH."
  exit 1
fi
(
  cd "${ROOT_DIR}/tools/vscode-parus"
  npm run check
  npm run compile
)

echo ""
echo "Built successfully (stage1 + stage2 + tests)."
echo "LLVM lane            : ${LLVM_LANE}"
echo "llvm-config          : ${LLVM_CONFIG_BIN}"
echo "C compiler           : ${C_COMPILER}"
echo "CXX compiler         : ${CXX_COMPILER}"
echo "LLVM toolchain mode  : ${LLVM_USE_TOOLCHAIN} (required=${LLVM_REQUIRE_TOOLCHAIN})"
if [ -n "${CCACHE_BIN}" ]; then
  echo "ccache launcher      : ${CCACHE_BIN}"
fi
echo "Run: ${BUILD_DIR}/compiler/parusc/parusc"
