# RAG over the C++ Gateway

LangChain does retrieval locally (sentence-transformers + FAISS); generation
goes through the kama-webserver gateway (:8080) -> model_daemon (:19090) ->
local LLM. vLLM is optional: the daemon's `--backend transformers` path needs
no vLLM at all.

## 0. Dependencies (once)

```bash
uv pip install -p .venv/bin/python langchain langchain-openai langchain-community \
    langchain-huggingface sentence-transformers faiss-cpu
```

## 1. Start the stack (no vLLM)

Terminal A — daemon with plain transformers backend (model already cached locally):

```bash
.venv/bin/python kama-webserver/model_daemon.py \
    --backend transformers --model Qwen/Qwen2.5-0.5B-Instruct --trust-remote-code
```

Terminal B — C++ gateway:

```bash
cmake -S kama-webserver -B kama-webserver/build -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build kama-webserver/build -j4
kama-webserver/bin/main
```

Protocol smoke test (bypasses RAG, checks the OpenAI-compatible HTTP path):

```bash
curl -s http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -H 'Authorization: Bearer sk-test' \
  -d '{"model":"local","messages":[{"role":"user","content":"你好"}],"max_tokens":64}'
```

## 2. Build the knowledge index

Drop your `.md`/`.txt` files into `kama-webserver/rag/docs/` (seeded with this
project's own docs), then:

```bash
cd kama-webserver && ../.venv/bin/python rag/build_index.py
```

Embedding model (`BAAI/bge-small-zh-v1.5`, ~100 MB, downloaded on first run)
runs on CPU — no GPU needed for indexing.

## 3. Ask

```bash
cd kama-webserver && ../.venv/bin/python rag/ask.py "如何在不加载模型的情况下测试整条链路？"
../.venv/bin/python kama-webserver/rag/ask.py   # interactive mode (run from repo root, imports build_index)
```

`ask.py` prints the retrieved chunks (with sources), then the generated answer.

## Tuning knobs

| What | Where | Notes |
|---|---|---|
| Chunk size/overlap | `build_index.py --chunk-size 500 --chunk-overlap 100` | biggest lever on answer quality |
| Retrieval depth | `ask.py --topk 4` | 8GB VRAM: context eats KV/seq budget fast on long prompts |
| Gateway | `ask.py --gateway http://127.0.0.1:8080/v1` | point at mock daemon to debug retrieval alone |
| Generation length | `ask.py --max-tokens 256` | capped by daemon `--max-tokens-limit` (1024) |
| Backend | `model_daemon.py --backend vllm\|transformers\|mock` | pluggable via `Backend` base class |

## Notes / known limits

- Prompt goes to the model raw (no chat template applied on the transformers
  path), so instruct-following is weaker than via vLLM chat; fine for demos,
  switch back to `--backend vllm` for quality.
- The C++ HTTP layer hardcodes `"created":0` and no `usage` field — LangChain
  tolerates this; strict clients may not.
- Long RAG prompts stress the in-file JSON parser (only `\\ \n \r \t` escapes
  are unescaped). If answers come back truncated/garbled, reproduce with curl
  to separate gateway vs. model issues.
