#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_TYPE="Debug"
PREFIX="${PARUS_INSTALL_PREFIX:-$HOME/.local}"

usage() {
  cat <<'EOF'
Usage: ./install.sh [--build-type <Debug|Release>] [--prefix <path>]

This script performs:
  1) ./run.sh (build + tests)
  2) install parus/parusc/parus-lld/parusd/lei into PREFIX

Defaults:
  --build-type Debug
  --prefix $HOME/.local
EOF
}

while (($# > 0)); do
  case "$1" in
    --build-type)
      if (($# < 2)); then
        echo "install.sh: --build-type requires a value" >&2
        exit 1
      fi
      BUILD_TYPE="$2"
      shift 2
      ;;
    --prefix)
      if (($# < 2)); then
        echo "install.sh: --prefix requires a value" >&2
        exit 1
      fi
      PREFIX="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "install.sh: unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [ ! -x "${ROOT_DIR}/run.sh" ]; then
  echo "install.sh: run.sh not found or not executable" >&2
  exit 1
fi

echo "[install] build + test via ./run.sh ${BUILD_TYPE}"
"${ROOT_DIR}/run.sh" "${BUILD_TYPE}"

BUILD_DIR="${ROOT_DIR}/build"
PARUSC_BIN="${BUILD_DIR}/compiler/parusc/parusc"
PARUS_LLD_BIN="${BUILD_DIR}/compiler/parusc/parus-lld"
PARUSD_BIN="${BUILD_DIR}/compiler/parusc/parusd"
PARUS_BIN="${BUILD_DIR}/compiler/parusc/parus"
LEI_BIN=""

echo "[install] ensure lei tool is built"
if [ ! -x "${BUILD_DIR}/tools/Lei/lei" ]; then
  if ! cmake --build "${BUILD_DIR}" --target lei -j16; then
    echo "install.sh: failed to build lei target" >&2
    exit 1
  fi
fi

if [ -x "${BUILD_DIR}/tools/Lei/lei" ]; then
  LEI_BIN="${BUILD_DIR}/tools/Lei/lei"
fi

if [ ! -x "${PARUSC_BIN}" ]; then
  echo "install.sh: missing built parusc binary: ${PARUSC_BIN}" >&2
  exit 1
fi
if [ ! -x "${PARUS_LLD_BIN}" ]; then
  echo "install.sh: missing built parus-lld binary: ${PARUS_LLD_BIN}" >&2
  exit 1
fi
if [ ! -x "${PARUSD_BIN}" ]; then
  echo "install.sh: missing built parusd binary: ${PARUSD_BIN}" >&2
  exit 1
fi
if [ ! -x "${PARUS_BIN}" ]; then
  echo "install.sh: missing built parus binary: ${PARUS_BIN}" >&2
  exit 1
fi

LLVM_LANE="${PARUS_LLVM_VERSION:-20}"

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
  LLVM_CONFIG_BIN="$(resolve_candidate "${PARUS_LLVM_CONFIG_EXECUTABLE}")" || true
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

HOST_OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
HOST_ARCH="$(uname -m)"
case "${HOST_OS}:${HOST_ARCH}" in
  darwin:arm64) DEFAULT_TARGET="aarch64-apple-darwin" ;;
  darwin:x86_64) DEFAULT_TARGET="x86_64-apple-darwin" ;;
  linux:x86_64) DEFAULT_TARGET="x86_64-unknown-linux-gnu" ;;
  linux:aarch64|linux:arm64) DEFAULT_TARGET="aarch64-unknown-linux-gnu" ;;
  *)
    DEFAULT_TARGET="${HOST_ARCH}-unknown-${HOST_OS}"
    ;;
esac

TOOLCHAIN_ID="dev-${DEFAULT_TARGET}-llvm${LLVM_LANE}"
PARUS_HOME="${PREFIX}/share/parus"
TOOLCHAINS_DIR="${PARUS_HOME}/toolchains"
TOOLCHAIN_ROOT="${TOOLCHAINS_DIR}/${TOOLCHAIN_ID}"
BIN_DIR="${PREFIX}/bin"
SYSROOT_DIR="${TOOLCHAIN_ROOT}/sysroot"
TARGET_SYSROOT_DIR="${SYSROOT_DIR}/targets/${DEFAULT_TARGET}"

echo "[install] prefix          : ${PREFIX}"
echo "[install] toolchain id    : ${TOOLCHAIN_ID}"
echo "[install] toolchain root  : ${TOOLCHAIN_ROOT}"

rm -rf "${TOOLCHAIN_ROOT}"
mkdir -p "${TOOLCHAIN_ROOT}/bin" "${TOOLCHAIN_ROOT}/libexec"
mkdir -p "${SYSROOT_DIR}" "${TARGET_SYSROOT_DIR}"
mkdir -p "${TARGET_SYSROOT_DIR}/parlib" "${TARGET_SYSROOT_DIR}/lib" "${TARGET_SYSROOT_DIR}/std/parlib" "${TARGET_SYSROOT_DIR}/std/obj" "${TARGET_SYSROOT_DIR}/native"
mkdir -p "${BIN_DIR}" "${TOOLCHAINS_DIR}"

cp -f "${PARUSC_BIN}" "${TOOLCHAIN_ROOT}/bin/parusc"
cp -f "${PARUS_LLD_BIN}" "${TOOLCHAIN_ROOT}/bin/parus-lld"
cp -f "${PARUSD_BIN}" "${TOOLCHAIN_ROOT}/bin/parusd"
cp -f "${PARUS_BIN}" "${TOOLCHAIN_ROOT}/bin/parus"
chmod +x "${TOOLCHAIN_ROOT}/bin/parusc" "${TOOLCHAIN_ROOT}/bin/parus-lld" "${TOOLCHAIN_ROOT}/bin/parusd" "${TOOLCHAIN_ROOT}/bin/parus"

if [ -z "${LEI_BIN}" ] || [ ! -x "${LEI_BIN}" ]; then
  echo "install.sh: missing built lei binary: ${BUILD_DIR}/tools/Lei/lei" >&2
  exit 1
fi
cp -f "${LEI_BIN}" "${TOOLCHAIN_ROOT}/bin/lei"
chmod +x "${TOOLCHAIN_ROOT}/bin/lei"

if [ -n "${LLVM_CONFIG_BIN}" ]; then
  LLVM_PREFIX="$("${LLVM_CONFIG_BIN}" --prefix)"
  for tool in ld64.lld ld.lld lld-link; do
    if [ -x "${LLVM_PREFIX}/bin/${tool}" ]; then
      cp -f "${LLVM_PREFIX}/bin/${tool}" "${TOOLCHAIN_ROOT}/bin/${tool}"
      chmod +x "${TOOLCHAIN_ROOT}/bin/${tool}"
    fi
  done
else
  for tool in ld64.lld ld.lld lld-link; do
    if command -v "${tool}" >/dev/null 2>&1; then
      cp -f "$(command -v "${tool}")" "${TOOLCHAIN_ROOT}/bin/${tool}"
      chmod +x "${TOOLCHAIN_ROOT}/bin/${tool}"
    fi
  done
fi

mkdir -p "${ROOT_DIR}/sysroot/std/src"
mkdir -p "${SYSROOT_DIR}/std/src"
cp -R "${ROOT_DIR}/sysroot/std/src/." "${SYSROOT_DIR}/std/src/"

to_u64_hash() {
  local hex="$1"
  # 15 hex digits -> 60-bit 범위로 bash 정수 오버플로우를 피한다.
  local short_hex="${hex:0:15}"
  printf "%llu" "$((16#${short_hex}))"
}

hash_file_u64() {
  local file="$1"
  local hex
  if command -v shasum >/dev/null 2>&1; then
    hex="$(shasum -a 256 "${file}" | awk '{print $1}')"
  elif command -v sha256sum >/dev/null 2>&1; then
    hex="$(sha256sum "${file}" | awk '{print $1}')"
  else
    hex="$(cksum "${file}" | awk '{print $1}')000000000000000"
  fi
  to_u64_hash "${hex}"
}

hash_text_u64() {
  local text="$1"
  local hex
  if command -v shasum >/dev/null 2>&1; then
    hex="$(printf "%s" "${text}" | shasum -a 256 | awk '{print $1}')"
  elif command -v sha256sum >/dev/null 2>&1; then
    hex="$(printf "%s" "${text}" | sha256sum | awk '{print $1}')"
  else
    hex="$(printf "%s" "${text}" | cksum | awk '{print $1}')000000000000000"
  fi
  to_u64_hash "${hex}"
}

TOOLCHAIN_HASH="$(hash_file_u64 "${TOOLCHAIN_ROOT}/bin/parusc")"
TARGET_HASH="$(hash_text_u64 "${DEFAULT_TARGET}|${LLVM_LANE}|c-v0|${TOOLCHAIN_HASH}")"

cat > "${SYSROOT_DIR}/manifest.json" <<EOF
{
  "format_version": 1,
  "toolchain_id": "${TOOLCHAIN_ID}",
  "toolchain_hash": ${TOOLCHAIN_HASH},
  "default_target_triple": "${DEFAULT_TARGET}",
  "abi_line": "c-v0",
  "install_prefix": "${PREFIX}"
}
EOF

cat > "${TARGET_SYSROOT_DIR}/manifest.json" <<EOF
{
  "format_version": 1,
  "target_triple": "${DEFAULT_TARGET}",
  "target_hash": ${TARGET_HASH},
  "toolchain_hash": ${TOOLCHAIN_HASH},
  "abi_line": "c-v0"
}
EOF

if [ "${HOST_OS}" = "darwin" ]; then
  SDK_REF="${PARUS_APPLE_SDK_ROOT:-${SDKROOT:-}}"
  if [ -z "${SDK_REF}" ] && command -v xcrun >/dev/null 2>&1; then
    SDK_REF="$(xcrun --sdk macosx --show-sdk-path 2>/dev/null || true)"
  fi
  if [ -n "${SDK_REF}" ]; then
    cat > "${TARGET_SYSROOT_DIR}/native/apple-sdk.ref" <<EOF
{
  "sdk_root": "${SDK_REF}"
}
EOF
  fi
fi

ln -sfn "${TOOLCHAIN_ROOT}" "${PARUS_HOME}/active-toolchain"

cat > "${BIN_DIR}/parusc" <<EOF
#!/usr/bin/env bash
set -euo pipefail
PARUS_PREFIX="${PREFIX}"
PARUS_HOME="\${PARUS_HOME:-\${PARUS_PREFIX}/share/parus}"
ACTIVE="\${PARUS_HOME}/active-toolchain"
if [ ! -e "\${ACTIVE}" ]; then
  echo "parusc launcher: active-toolchain not found: \${ACTIVE}" >&2
  exit 1
fi
TOOLCHAIN_ROOT="\$(cd "\${ACTIVE}" && pwd -P)"
export PARUS_TOOLCHAIN_ROOT="\${PARUS_TOOLCHAIN_ROOT:-\${TOOLCHAIN_ROOT}}"
export PARUS_SYSROOT="\${PARUS_SYSROOT:-\${TOOLCHAIN_ROOT}/sysroot}"
export PARUS_LLD="\${PARUS_LLD:-\${TOOLCHAIN_ROOT}/bin/parus-lld}"
export PARUSD="\${PARUSD:-\${TOOLCHAIN_ROOT}/bin/parusd}"
exec "\${TOOLCHAIN_ROOT}/bin/parusc" "\$@"
EOF

cat > "${BIN_DIR}/parus" <<EOF
#!/usr/bin/env bash
set -euo pipefail
PARUS_PREFIX="${PREFIX}"
PARUS_HOME="\${PARUS_HOME:-\${PARUS_PREFIX}/share/parus}"
ACTIVE="\${PARUS_HOME}/active-toolchain"
if [ ! -e "\${ACTIVE}" ]; then
  echo "parus launcher: active-toolchain not found: \${ACTIVE}" >&2
  exit 1
fi
TOOLCHAIN_ROOT="\$(cd "\${ACTIVE}" && pwd -P)"
export PARUS_TOOLCHAIN_ROOT="\${PARUS_TOOLCHAIN_ROOT:-\${TOOLCHAIN_ROOT}}"
export PARUS_SYSROOT="\${PARUS_SYSROOT:-\${TOOLCHAIN_ROOT}/sysroot}"
export PARUS_LLD="\${PARUS_LLD:-\${TOOLCHAIN_ROOT}/bin/parus-lld}"
export PARUSD="\${PARUSD:-\${TOOLCHAIN_ROOT}/bin/parusd}"
exec "\${TOOLCHAIN_ROOT}/bin/parus" "\$@"
EOF

cat > "${BIN_DIR}/parus-lld" <<EOF
#!/usr/bin/env bash
set -euo pipefail
PARUS_PREFIX="${PREFIX}"
PARUS_HOME="\${PARUS_HOME:-\${PARUS_PREFIX}/share/parus}"
ACTIVE="\${PARUS_HOME}/active-toolchain"
if [ ! -e "\${ACTIVE}" ]; then
  echo "parus-lld launcher: active-toolchain not found: \${ACTIVE}" >&2
  exit 1
fi
TOOLCHAIN_ROOT="\$(cd "\${ACTIVE}" && pwd -P)"
export PARUS_TOOLCHAIN_ROOT="\${PARUS_TOOLCHAIN_ROOT:-\${TOOLCHAIN_ROOT}}"
export PARUS_SYSROOT="\${PARUS_SYSROOT:-\${TOOLCHAIN_ROOT}/sysroot}"
exec "\${TOOLCHAIN_ROOT}/bin/parus-lld" "\$@"
EOF

cat > "${BIN_DIR}/parusd" <<EOF
#!/usr/bin/env bash
set -euo pipefail
PARUS_PREFIX="${PREFIX}"
PARUS_HOME="\${PARUS_HOME:-\${PARUS_PREFIX}/share/parus}"
ACTIVE="\${PARUS_HOME}/active-toolchain"
if [ ! -e "\${ACTIVE}" ]; then
  echo "parusd launcher: active-toolchain not found: \${ACTIVE}" >&2
  exit 1
fi
TOOLCHAIN_ROOT="\$(cd "\${ACTIVE}" && pwd -P)"
export PARUS_TOOLCHAIN_ROOT="\${PARUS_TOOLCHAIN_ROOT:-\${TOOLCHAIN_ROOT}}"
export PARUS_SYSROOT="\${PARUS_SYSROOT:-\${TOOLCHAIN_ROOT}/sysroot}"
exec "\${TOOLCHAIN_ROOT}/bin/parusd" "\$@"
EOF

cat > "${BIN_DIR}/lei" <<EOF
#!/usr/bin/env bash
set -euo pipefail
PARUS_PREFIX="${PREFIX}"
PARUS_HOME="\${PARUS_HOME:-\${PARUS_PREFIX}/share/parus}"
ACTIVE="\${PARUS_HOME}/active-toolchain"
if [ ! -e "\${ACTIVE}" ]; then
  echo "lei launcher: active-toolchain not found: \${ACTIVE}" >&2
  exit 1
fi
TOOLCHAIN_ROOT="\$(cd "\${ACTIVE}" && pwd -P)"
exec "\${TOOLCHAIN_ROOT}/bin/lei" "\$@"
EOF
chmod +x "${BIN_DIR}/lei"

chmod +x "${BIN_DIR}/parus" "${BIN_DIR}/parusc" "${BIN_DIR}/parus-lld" "${BIN_DIR}/parusd"

ZSHRC="${HOME}/.zshrc"
mkdir -p "$(dirname "${ZSHRC}")"
PATH_LINE="export PATH=\"${PREFIX}/bin:\$PATH\""
if [ -f "${ZSHRC}" ]; then
  if ! grep -F "${PATH_LINE}" "${ZSHRC}" >/dev/null 2>&1; then
    {
      echo ""
      echo "# parus local toolchain"
      echo "${PATH_LINE}"
    } >> "${ZSHRC}"
  fi
else
  {
    echo "# parus local toolchain"
    echo "${PATH_LINE}"
  } > "${ZSHRC}"
fi

echo "[install] done"
echo "  parus launcher  : ${BIN_DIR}/parus"
echo "  parusc launcher : ${BIN_DIR}/parusc"
echo "  parus-lld       : ${BIN_DIR}/parus-lld"
echo "  parusd          : ${BIN_DIR}/parusd"
echo "  lei             : ${BIN_DIR}/lei"
echo "  sysroot         : ${SYSROOT_DIR}"
echo "  toolchain hash  : ${TOOLCHAIN_HASH}"
echo "  target hash     : ${TARGET_HASH}"
echo ""
echo "Open a new shell or run:"
echo "  export PATH=\"${PREFIX}/bin:\$PATH\""
echo "  parus --version"
