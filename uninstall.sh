#!/usr/bin/env bash
set -euo pipefail

PREFIX="${PARUS_INSTALL_PREFIX:-$HOME/.local}"
REMOVE_ALL=0
TOOLCHAIN_ID=""

usage() {
  cat <<'EOF'
Usage: ./uninstall.sh [--prefix <path>] [--toolchain-id <id>] [--all]

Defaults:
  --prefix $HOME/.local

Behavior:
  - 기본: active-toolchain이 가리키는 toolchain만 제거
  - --toolchain-id: 지정한 toolchain만 제거
  - --all: parus 설치 전체 제거 (toolchains + active + launchers)
EOF
}

while (($# > 0)); do
  case "$1" in
    --prefix)
      if (($# < 2)); then
        echo "uninstall.sh: --prefix requires a value" >&2
        exit 1
      fi
      PREFIX="$2"
      shift 2
      ;;
    --toolchain-id)
      if (($# < 2)); then
        echo "uninstall.sh: --toolchain-id requires a value" >&2
        exit 1
      fi
      TOOLCHAIN_ID="$2"
      shift 2
      ;;
    --all)
      REMOVE_ALL=1
      shift
      ;;
    -h|--help)
      usage
      exit 0
      ;;
    *)
      echo "uninstall.sh: unknown argument: $1" >&2
      usage
      exit 1
      ;;
  esac
done

PARUS_HOME="${PREFIX}/share/parus"
BIN_DIR="${PREFIX}/bin"
ACTIVE_LINK="${PARUS_HOME}/active-toolchain"
TOOLCHAINS_DIR="${PARUS_HOME}/toolchains"

if [ "${REMOVE_ALL}" -eq 1 ]; then
  rm -rf "${PARUS_HOME}"
  rm -f "${BIN_DIR}/parus" "${BIN_DIR}/parusc" "${BIN_DIR}/parus-lld" "${BIN_DIR}/parusd" "${BIN_DIR}/lei" "${BIN_DIR}/lei-build"
  echo "[uninstall] removed all parus artifacts under ${PREFIX}"
  exit 0
fi

TARGET_REMOVE=""
ACTIVE_RESOLVED=""
if [ -L "${ACTIVE_LINK}" ] || [ -e "${ACTIVE_LINK}" ]; then
  ACTIVE_RESOLVED="$(cd "${ACTIVE_LINK}" 2>/dev/null && pwd -P || true)"
fi

if [ -n "${TOOLCHAIN_ID}" ]; then
  TARGET_REMOVE="${TOOLCHAINS_DIR}/${TOOLCHAIN_ID}"
elif [ -L "${ACTIVE_LINK}" ] || [ -e "${ACTIVE_LINK}" ]; then
  TARGET_REMOVE="${ACTIVE_RESOLVED}"
fi

if [ -n "${TARGET_REMOVE}" ] && [ -d "${TARGET_REMOVE}" ]; then
  rm -rf "${TARGET_REMOVE}"
  echo "[uninstall] removed toolchain: ${TARGET_REMOVE}"
fi

if [ -n "${TARGET_REMOVE}" ] && [ -n "${ACTIVE_RESOLVED}" ] && [ "${ACTIVE_RESOLVED}" = "${TARGET_REMOVE}" ]; then
  rm -f "${ACTIVE_LINK}"
  echo "[uninstall] removed active-toolchain link"
fi

if [ -L "${ACTIVE_LINK}" ] && [ ! -e "${ACTIVE_LINK}" ]; then
  rm -f "${ACTIVE_LINK}"
fi

if [ ! -e "${ACTIVE_LINK}" ]; then
  rm -f "${BIN_DIR}/parus" "${BIN_DIR}/parusc" "${BIN_DIR}/parus-lld" "${BIN_DIR}/parusd" "${BIN_DIR}/lei" "${BIN_DIR}/lei-build"
  echo "[uninstall] removed launchers (no active toolchain)"
fi

echo "[uninstall] done"
