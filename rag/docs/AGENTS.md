# Agent Notes

## Repo Reality (use this, not README prose)
- This is a single C++/CMake project rooted at `kama-webserver/`.
- Build wiring is in `CMakeLists.txt`, `src/CMakeLists.txt`, `log/CMakeLists.txt`, and `memory/CMakeLists.txt`.
- `README.md` is mostly background/course material; prefer CMake + code as source of truth.

## Commands That Actually Work
- Configure on modern CMake with policy override:
  - `cmake -S . -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5`
- Build:
  - `cmake --build build -j4`
- Quick runtime smoke test (server blocks in event loop):
  - `timeout 2s ./bin/main`
- End-to-end local LLM stack:
  - `./scripts/run_local_model_stack.sh`
  - `MOCK_BACKEND=1 ./scripts/run_local_model_stack.sh` (no model load)
- Lightweight local browser UI (no Docker):
  - `./scripts/start_webui.sh` (serves `http://127.0.0.1:7860`)

## Build/Run Behavior You Might Miss
- Output paths are forced to repo-local directories (not inside `build/`):
  - executable: `bin/main`
  - shared libs: `lib/libsrc_lib.so`, `lib/liblog_lib.so`, `lib/libmemory_lib.so`
- Running `bin/main` creates `logs/` and writes async log files there.
- Server listens on port `8080` (`src/main.cc`).
- C++ server now forwards line-based inference requests to local daemon `127.0.0.1:19090`.
- Local daemon entrypoint: `model_daemon.py` (vLLM backend by default).
- `src/main.cc` now has two ingress modes: line protocol and `POST /v1/chat/completions` HTTP.
- Inference is offloaded to a worker queue in `src/main.cc`; do not move model calls back onto event-loop threads.
- HTTP body parsing now uses an in-file JSON parser (not regex extraction).
- HTTP `stream: true` returns SSE-style chunks and closes connection with `data: [DONE]`.
- `/v1/chat/completions` now flattens the full `messages` array into a role-tagged prompt; it no longer uses only the last message.

## Code Layout
- `src/`: networking/event-loop implementation and `main.cc` entrypoint.
- `log/`: async logging components (`log_lib`).
- `memory/`: memory pool components (`memory_lib`).
- `include/`: shared public headers for all modules.

## Verification Expectations
- No test/lint/typecheck tooling is configured in repo.
- Standard verification is: configure + build + short startup smoke run.
- For LLM path verification, use mock backend and send a TCP line request to `127.0.0.1:8080`.

## Working Tree Hygiene
- Repo currently has no `.gitignore`; local builds/runs create `build/`, `bin/`, `logs/`, and update `lib/*.so`.
- Treat those as generated artifacts and avoid committing unless a task explicitly requires built binaries.
