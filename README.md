# cpp-llm-gateway

一个用 C++ 手写的**高性能 LLM 推理网关**：底层是基于 epoll 的 Reactor 并发网络框架，对外提供 **OpenAI 兼容的 HTTP 接口**（`/v1/chat/completions`，支持 SSE 流式）和轻量行协议两种入口，推理侧通过可插拔后端对接本地大模型（vLLM / HuggingFace Transformers / Mock），并已接入 LangChain 实现完整的本地 RAG 问答链路。

一句话概括：**客户端把请求发给 C++ 网关，网关把推理请求分发给本地模型守护进程，全程不出本机。**

```
客户端 / LangChain / 浏览器 UI
        │  OpenAI 兼容 HTTP (:8080) 或 行协议 TCP
        ▼
C++ 网关 (epoll Reactor, one-loop-per-thread)
        │  行协议 TCP (127.0.0.1:19090)
        ▼
model_daemon.py ──▶ vLLM │ Transformers │ Mock
```

## 核心特性

**网络框架（C++17 / CMake）**
- epoll (ET) + Reactor 事件引擎：`EventLoop` / `Channel` / `Poller` / `Acceptor` / `TcpServer` / `TcpConnection`
- master-slave Reactor：`EventLoopThreadPool`，one loop per thread
- 通用定时器-wheel：`Timer` + `TimerQueue`，连接空闲超时自动断开
- 异步日志系统：双缓冲落盘（`AsyncLogging` / `LogFile`），日志滚动
- 内存池：`memory_lib`，减少高频小块分配的开销
- 缓存组件：LFU 缓存策略实现

**LLM 网关（本项目扩展）**
- `POST /v1/chat/completions`：OpenAI 协议兼容，手写零依赖 JSON 解析与序列化
- `stream: true` SSE 流式返回（`data: [DONE]` 收尾），LangChain `ChatOpenAI` 仅改 `base_url` 即可直连
- 双协议入口：HTTP 与 `INFER\t` 行协议（供内部组件/调试使用）
- 推理在独立 worker 队列执行，不阻塞事件循环线程
- Python 守护进程提供三种可插拔推理后端：
  - `vllm`：生产级吞吐（8GB 显存可跑 0.5B~3B）
  - `transformers`：无 vLLM 依赖，CPU/GPU 自适应
  - `mock`：不加载模型，全链路调试用

**RAG 应用层（rag/）**
- `build_index.py`：文档 → 切块 → sentence-transformers 向量化 → FAISS 入库
- `ask.py`：问题 → 向量检索 Top-K → 组装 Prompt → 经网关生成（带来源展示）
- 检索在 Python 侧本地完成，生成走自研网关——网关即"私有 OpenAI"

## 快速开始

```bash
# 1. 构建（需要 cmake >= 3.5、支持 C++17 的 g++）
cmake -S . -B build -DCMAKE_POLICY_VERSION_MINIMUM=3.5
cmake --build build -j4

# 2. 一键启动全链路（网关 + 模型守护进程）
./scripts/run_local_model_stack.sh

# 无 GPU / 不加载模型，先验证链路：
MOCK_BACKEND=1 ./scripts/run_local_model_stack.sh
```

验证接口（与调 OpenAI 官方 API 姿势完全一致）：

```bash
curl -s http://127.0.0.1:8080/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"local","messages":[{"role":"user","content":"你好"}],"max_tokens":64}'
```

LangChain 接入：

```python
from langchain_openai import ChatOpenAI
llm = ChatOpenAI(base_url="http://127.0.0.1:8080/v1", api_key="sk-local", model="local")
```

RAG 建库与问答见 [rag/RAG.md](rag/RAG.md)。

## 目录结构

```
src/        网络框架实现 + main.cc（网关逻辑：HTTP/行协议解析、worker 队列、SSE）
include/    公共头文件
log/        异步日志组件（liblog_lib）
memory/     内存池组件（libmemory_lib）
model_daemon.py  推理守护进程（vLLM / Transformers / Mock 可插拔 Backend）
rag/        LangChain RAG：建库脚本、问答脚本、示例语料
scripts/    一键启动脚本
webui_proxy.py   轻量浏览器聊天 UI（http://127.0.0.1:7860）
```

## 设计要点

- **协议兼容优先**：对外表现为标准 OpenAI endpoint，任何生态工具（LangChain / LlamaIndex / openai SDK / curl）零改造接入
- **事件循环零阻塞**：LLM 推理耗时可达秒级，绝不放在 event-loop 线程执行，统一投递到推理 worker 队列，回写时经 `runInLoop` 回到连接所属线程
- **推理后端可插拔**：`Backend` 抽象基类隔离网关协议与模型实现，加新后端（Ollama、云端 API）只需实现 `infer()`
- **本地闭环**：语料、向量库、模型、网关全部在本机，适合对隐私敏感的私有知识问答场景

## 开发状态

- [x] OpenAI 兼容 chat 接口 + SSE 流式
- [x] Transformers / vLLM / Mock 三种推理后端
- [x] LangChain RAG 建库与问答脚本
- [ ] `/v1/embeddings` 端点（当前 embedding 由 Python 侧本地完成）
- [ ] `/v1/models` 探活端点
- [ ] 网关层 LFU 响应缓存接入
- [ ] RAG 全链路端到端测试

## 许可证

GPL-3.0，详见 [LICENSE](LICENSE)。
