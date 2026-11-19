#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

AUTO_CLEAN_PORTS="${AUTO_CLEAN_PORTS:-1}"

if [[ ! -d "${ROOT_DIR}/../.venv" ]]; then
  echo "missing virtualenv at ${ROOT_DIR}/../.venv"
  exit 1
fi

source "${ROOT_DIR}/../.venv/bin/activate"

if fuser -n tcp 7860 >/dev/null 2>&1; then
  echo "port 7860 is already in use"
  if [[ "${AUTO_CLEAN_PORTS}" == "1" ]]; then
    echo "cleaning old process on port 7860"
    fuser -k -n tcp 7860 >/dev/null 2>&1 || true
    sleep 1
  else
    echo "set AUTO_CLEAN_PORTS=1 to auto-clean, or stop it manually"
    echo "example: fuser -k -n tcp 7860"
    exit 1
  fi
fi

python "${ROOT_DIR}/webui_proxy.py"
