# Local LLM Bridge

This project now supports a local loopback model flow:

- Client -> C++ server (`0.0.0.0:8080`)
- C++ server -> local Python model daemon (`127.0.0.1:19090`)
- Python daemon -> vLLM backend

Detailed implementation doc (CN): `PROJECT_IMPLEMENTATION_CN.md`

## Start

Use one command to build and run both processes:

```bash
./scripts/run_local_model_stack.sh
```

By default the script auto-cleans stale listeners on `8080`/`19090`.
Disable this behavior with:

```bash
AUTO_CLEAN_PORTS=0 ./scripts/run_local_model_stack.sh
```

Use mock backend (no model load):

```bash
MOCK_BACKEND=1 ./scripts/run_local_model_stack.sh
```

Custom model:

```bash
MODEL_NAME=Qwen/Qwen2.5-7B-Instruct ./scripts/run_local_model_stack.sh
```

If you hit GPU OOM, reduce memory pressure:

```bash
GPU_MEMORY_UTILIZATION=0.75 MAX_MODEL_LEN=2048 ./scripts/run_local_model_stack.sh
```

For 8GB-class GPUs, prefer smaller models like `Qwen/Qwen2.5-0.5B-Instruct`.

Stability note:

- If vLLM reports `Engine core proc ... died unexpectedly`, it is usually runtime OOM or GPU-side failure.
- This bridge now has protection defaults:
  - request `max_tokens` is capped to 1024 in the C++ gateway
  - daemon enforces `--max-tokens-limit` (default 1024)
  - daemon retries once by recreating the vLLM engine when it detects engine-core crash

## Client protocol (line-based)

Send one line per request over TCP to `127.0.0.1:8080`.

### Simple mode

Any plain line is treated as prompt.

Example request:

```text
hello local llm
```

### Parameter mode

Format:

```text
/infer\t<max_tokens>\t<temperature>\t<prompt>
```

Example request:

```text
/infer	128	0.7	explain epoll in one paragraph
```

### Response format

- Success: `OK\t<text>`
- Failure: `ERR\t<message>`

Newlines/tabs/backslashes in payload are escaped as `\\n`, `\\t`, `\\\\`.

## HTTP (OpenAI-like) endpoint

The C++ server also accepts:

- `POST /v1/chat/completions`

Message handling note:

- The bridge now preserves full chat context by flattening all `messages` into a single prompt template (`System/User/Assistant/.../Assistant:`), instead of only using the last message.

Minimal request body example:

```json
{
  "model": "local",
  "messages": [
    {"role": "user", "content": "say hi"}
  ],
  "temperature": 0.2,
  "max_tokens": 64
}
```

It returns OpenAI-style JSON response. If body includes `"stream": true`,
it returns SSE chunks with final `data: [DONE]`.

Quick test:

```bash
curl -sS http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"local","messages":[{"role":"user","content":"say hi"}],"temperature":0.2,"max_tokens":64}'
```

Streaming quick test:

```bash
curl -N http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"local","messages":[{"role":"user","content":"stream this"}],"stream":true}'
```

## Local browser UI (no Docker)

If Docker permission is not ready, use the built-in lightweight web UI proxy:

```bash
./scripts/start_webui.sh
```

This script also auto-cleans stale listener on `7860` by default.

Then open:

- `http://127.0.0.1:7860`

This UI forwards requests to `http://127.0.0.1:8080/v1/chat/completions`.

Prerequisite: keep the model stack running in another terminal (`./scripts/run_local_model_stack.sh`).
