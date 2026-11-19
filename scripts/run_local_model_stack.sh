#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

AUTO_CLEAN_PORTS="${AUTO_CLEAN_PORTS:-1}"

clean_port_if_needed() {
  local port="$1"
  if ! fuser -n tcp "${port}" >/dev/null 2>&1; then
    return
  fi

  echo "port ${port} is already in use"
  if [[ "${AUTO_CLEAN_PORTS}" == "1" ]]; then
    echo "cleaning old process on port ${port}"
    fuser -k -n tcp "${port}" >/dev/null 2>&1 || true
    sleep 1
  else
    echo "set AUTO_CLEAN_PORTS=1 to auto-clean, or stop it manually"
    echo "example: fuser -k -n tcp ${port}"
    exit 1
  fi
}

if [[ ! -d "${ROOT_DIR}/../.venv" ]]; then
  echo "missing virtualenv at ${ROOT_DIR}/../.venv"
  exit 1
fi

source "${ROOT_DIR}/../.venv/bin/activate"

clean_port_if_needed 8080
clean_port_if_needed 19090

if [[ ! -d "${ROOT_DIR}/build" ]]; then
  cmake -S "${ROOT_DIR}" -B "${ROOT_DIR}/build" -DCMAKE_POLICY_VERSION_MINIMUM=3.5
fi

cmake --build "${ROOT_DIR}/build" -j4

MODEL_NAME="${MODEL_NAME:-Qwen/Qwen2.5-0.5B-Instruct}"
GPU_MEMORY_UTILIZATION="${GPU_MEMORY_UTILIZATION:-0.90}"
MAX_MODEL_LEN="${MAX_MODEL_LEN:-4096}"
MAX_TOKENS_LIMIT="${MAX_TOKENS_LIMIT:-1024}"
DAEMON_ARGS=(
  --host 127.0.0.1
  --port 19090
  --model "${MODEL_NAME}"
  --gpu-memory-utilization "${GPU_MEMORY_UTILIZATION}"
  --max-model-len "${MAX_MODEL_LEN}"
  --max-tokens-limit "${MAX_TOKENS_LIMIT}"
)
if [[ "${MOCK_BACKEND:-0}" == "1" ]]; then
  DAEMON_ARGS=(--host 127.0.0.1 --port 19090 --mock)
fi

python "${ROOT_DIR}/model_daemon.py" "${DAEMON_ARGS[@]}" &
DAEMON_PID=$!

sleep 2
if ! kill -0 "${DAEMON_PID}" >/dev/null 2>&1; then
  echo "model daemon failed to start."
  echo "hint: this is often GPU OOM. try a smaller model, e.g.:"
  echo "  MODEL_NAME=Qwen/Qwen2.5-0.5B-Instruct ./scripts/run_local_model_stack.sh"
  echo "or lower memory pressure, e.g.:"
  echo "  GPU_MEMORY_UTILIZATION=0.75 MAX_MODEL_LEN=2048 MAX_TOKENS_LIMIT=512 ./scripts/run_local_model_stack.sh"
  exit 1
fi

cleanup() {
  kill "${DAEMON_PID}" >/dev/null 2>&1 || true
}
trap cleanup EXIT

echo "local model daemon pid=${DAEMON_PID}"
echo "starting C++ server on 0.0.0.0:8080"
"${ROOT_DIR}/bin/main"
