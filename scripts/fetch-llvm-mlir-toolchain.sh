#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
RELEASE_VERSION="${PARUS_MLIR_RELEASE_VERSION:-22.1.4}"
MODE="binary"
OUT_ROOT=""
JOBS="${PARUS_MLIR_BUILD_JOBS:-}"

usage() {
  cat <<'EOF'
Usage: scripts/fetch-llvm-mlir-toolchain.sh [--release <22.1.4>] [--mode <binary|source>] [--root <path>] [--jobs <n>]

Fetch an official llvm-project GitHub release for the Parus MLIR lane.

Defaults:
  --release 22.1.4
  --mode binary
  --root .parus-toolchains/llvm-mlir/<release>/<host>

The final toolchain root is printed as the last line.
EOF
}

while (($# > 0)); do
  case "$1" in
    --release)
      if (($# < 2)); then
        echo "fetch-llvm-mlir-toolchain.sh: --release requires a value" >&2
        exit 1
      fi
      RELEASE_VERSION="$2"
      shift 2
      ;;
    --mode)
      if (($# < 2)); then
        echo "fetch-llvm-mlir-toolchain.sh: --mode requires a value" >&2
        exit 1
      fi
      MODE="$2"
      shift 2
      ;;
    --root)
      if (($# < 2)); then
        echo "fetch-llvm-mlir-toolchain.sh: --root requires a value" >&2
        exit 1
      fi
      OUT_ROOT="$2"
      shift 2
      ;;
    --jobs)
      if (($# < 2)); then
        echo "fetch-llvm-mlir-toolchain.sh: --jobs requires a value" >&2
        exit 1
      fi
      JOBS="$2"
      shift 2
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "fetch-llvm-mlir-toolchain.sh: unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

if [ "${MODE}" != "binary" ] && [ "${MODE}" != "source" ]; then
  echo "fetch-llvm-mlir-toolchain.sh: --mode must be binary or source" >&2
  exit 1
fi

if ! command -v curl >/dev/null 2>&1; then
  echo "fetch-llvm-mlir-toolchain.sh: curl is required" >&2
  exit 1
fi
if ! command -v jq >/dev/null 2>&1; then
  echo "fetch-llvm-mlir-toolchain.sh: jq is required" >&2
  exit 1
fi

HOST_OS="$(uname -s | tr '[:upper:]' '[:lower:]')"
HOST_ARCH="$(uname -m)"
case "${HOST_OS}:${HOST_ARCH}" in
  darwin:arm64) HOST_ID="macos-arm64"; BINARY_ASSET="LLVM-${RELEASE_VERSION}-macOS-ARM64.tar.xz" ;;
  darwin:x86_64) HOST_ID="macos-x64"; BINARY_ASSET="LLVM-${RELEASE_VERSION}-macOS-X64.tar.xz" ;;
  linux:x86_64) HOST_ID="linux-x64"; BINARY_ASSET="LLVM-${RELEASE_VERSION}-Linux-X64.tar.xz" ;;
  linux:aarch64|linux:arm64) HOST_ID="linux-arm64"; BINARY_ASSET="LLVM-${RELEASE_VERSION}-Linux-ARM64.tar.xz" ;;
  mingw*:x86_64|msys*:x86_64|cygwin*:x86_64) HOST_ID="windows-x64"; BINARY_ASSET="clang+llvm-${RELEASE_VERSION}-x86_64-pc-windows-msvc.tar.xz" ;;
  mingw*:aarch64|msys*:aarch64|cygwin*:aarch64|mingw*:arm64|msys*:arm64|cygwin*:arm64) HOST_ID="windows-arm64"; BINARY_ASSET="clang+llvm-${RELEASE_VERSION}-aarch64-pc-windows-msvc.tar.xz" ;;
  *)
    echo "fetch-llvm-mlir-toolchain.sh: unsupported host ${HOST_OS}:${HOST_ARCH}" >&2
    exit 1
    ;;
esac

if [ -z "${OUT_ROOT}" ]; then
  OUT_ROOT="${ROOT_DIR}/.parus-toolchains/llvm-mlir/${RELEASE_VERSION}/${HOST_ID}"
fi
case "${OUT_ROOT}" in
  /*) ;;
  *) OUT_ROOT="${ROOT_DIR}/${OUT_ROOT}" ;;
esac

TOOLCHAIN_ROOT="${OUT_ROOT}/toolchain"
DOWNLOAD_DIR="${OUT_ROOT}/downloads"
SOURCE_DIR="${OUT_ROOT}/src"
BUILD_DIR="${OUT_ROOT}/build-source"
TAG="llvmorg-${RELEASE_VERSION}"
API_URL="https://api.github.com/repos/llvm/llvm-project/releases/tags/${TAG}"

mkdir -p "${DOWNLOAD_DIR}"

echo "[mlir-toolchain] release : ${TAG}"
echo "[mlir-toolchain] mode    : ${MODE}"
echo "[mlir-toolchain] host    : ${HOST_ID}"
echo "[mlir-toolchain] out     : ${OUT_ROOT}"

RELEASE_JSON="${DOWNLOAD_DIR}/${TAG}.json"
curl -fsSL "${API_URL}" -o "${RELEASE_JSON}"

asset_field() {
  local name="$1"
  local field="$2"
  jq -r --arg name "${name}" --arg field "${field}" \
    '.assets[] | select(.name == $name) | .[$field] // empty' "${RELEASE_JSON}"
}

download_asset() {
  local asset_name="$1"
  local url digest expected archive actual
  url="$(asset_field "${asset_name}" "browser_download_url")"
  digest="$(asset_field "${asset_name}" "digest")"
  if [ -z "${url}" ]; then
    echo "fetch-llvm-mlir-toolchain.sh: asset '${asset_name}' not found in ${TAG}" >&2
    exit 1
  fi
  archive="${DOWNLOAD_DIR}/${asset_name}"
  if [[ "${digest}" == sha256:* ]]; then
    expected="${digest#sha256:}"
    if [ -f "${archive}" ]; then
      if command -v shasum >/dev/null 2>&1; then
        actual="$(shasum -a 256 "${archive}" | awk '{print $1}')"
      else
        actual="$(sha256sum "${archive}" | awk '{print $1}')"
      fi
      if [ "${actual}" = "${expected}" ]; then
        echo "[mlir-toolchain] cached  : ${asset_name}" >&2
      else
        echo "[mlir-toolchain] refetch : ${asset_name} (cached sha256 mismatch)" >&2
        curl -fL "${url}" -o "${archive}"
      fi
    else
      echo "[mlir-toolchain] fetch   : ${asset_name}" >&2
      curl -fL "${url}" -o "${archive}"
    fi
    if command -v shasum >/dev/null 2>&1; then
      actual="$(shasum -a 256 "${archive}" | awk '{print $1}')"
    else
      actual="$(sha256sum "${archive}" | awk '{print $1}')"
    fi
    if [ "${actual}" != "${expected}" ]; then
      echo "fetch-llvm-mlir-toolchain.sh: sha256 mismatch for ${asset_name}" >&2
      echo "  expected: ${expected}" >&2
      echo "  actual  : ${actual}" >&2
      exit 1
    fi
    echo "[mlir-toolchain] sha256  : ok" >&2
  else
    echo "[mlir-toolchain] fetch   : ${asset_name}" >&2
    curl -fL "${url}" -o "${archive}"
    echo "[mlir-toolchain] warning : release asset has no sha256 digest" >&2
  fi

  local attestation_name="${asset_name}.jsonl"
  local attestation_url
  attestation_url="$(asset_field "${attestation_name}" "browser_download_url")"
  if [ -n "${attestation_url}" ]; then
    local attestation_path="${DOWNLOAD_DIR}/${attestation_name}"
    curl -fsSL "${attestation_url}" -o "${attestation_path}"
    if command -v gh >/dev/null 2>&1; then
      gh attestation verify --repo llvm/llvm-project "${archive}" --bundle "${attestation_path}" >&2 || {
        echo "fetch-llvm-mlir-toolchain.sh: gh attestation verification failed" >&2
        exit 1
      }
      echo "[mlir-toolchain] attest  : ok" >&2
    else
      echo "[mlir-toolchain] warning : gh not found; skipped attestation verification" >&2
    fi
  fi

  printf '%s\n' "${archive}"
}

if [ "${MODE}" = "binary" ]; then
  ARCHIVE="$(download_asset "${BINARY_ASSET}")"
  rm -rf "${TOOLCHAIN_ROOT}"
  mkdir -p "${TOOLCHAIN_ROOT}"
  tar -xf "${ARCHIVE}" -C "${TOOLCHAIN_ROOT}" --strip-components=1
else
  SOURCE_ASSET="llvm-project-${RELEASE_VERSION}.src.tar.xz"
  ARCHIVE="$(download_asset "${SOURCE_ASSET}")"
  rm -rf "${SOURCE_DIR}" "${BUILD_DIR}" "${TOOLCHAIN_ROOT}"
  mkdir -p "${SOURCE_DIR}" "${BUILD_DIR}" "${TOOLCHAIN_ROOT}"
  tar -xf "${ARCHIVE}" -C "${SOURCE_DIR}" --strip-components=1
  if [ -z "${JOBS}" ]; then
    JOBS="$(getconf _NPROCESSORS_ONLN 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)"
  fi
  cmake -S "${SOURCE_DIR}/llvm" -B "${BUILD_DIR}" \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX="${TOOLCHAIN_ROOT}" \
    -DLLVM_ENABLE_PROJECTS="mlir;lld" \
    -DLLVM_TARGETS_TO_BUILD="Native" \
    -DBUILD_SHARED_LIBS=OFF \
    -DLLVM_BUILD_LLVM_DYLIB=OFF \
    -DLLVM_LINK_LLVM_DYLIB=OFF \
    -DLLVM_INCLUDE_TESTS=OFF \
    -DLLVM_INCLUDE_EXAMPLES=OFF \
    -DLLVM_INCLUDE_BENCHMARKS=OFF \
    -DMLIR_INCLUDE_TESTS=OFF
  cmake --build "${BUILD_DIR}" --target install -j"${JOBS}"
fi

if [ ! -f "${TOOLCHAIN_ROOT}/lib/cmake/mlir/MLIRConfig.cmake" ]; then
  echo "fetch-llvm-mlir-toolchain.sh: MLIRConfig.cmake not found in ${TOOLCHAIN_ROOT}" >&2
  exit 1
fi
if [ ! -f "${TOOLCHAIN_ROOT}/lib/cmake/llvm/LLVMConfig.cmake" ]; then
  echo "fetch-llvm-mlir-toolchain.sh: LLVMConfig.cmake not found in ${TOOLCHAIN_ROOT}" >&2
  exit 1
fi
if [ ! -x "${TOOLCHAIN_ROOT}/bin/llvm-config" ]; then
  echo "fetch-llvm-mlir-toolchain.sh: llvm-config not found in ${TOOLCHAIN_ROOT}" >&2
  exit 1
fi

echo "[mlir-toolchain] root    : ${TOOLCHAIN_ROOT}"
printf '%s\n' "${TOOLCHAIN_ROOT}"
