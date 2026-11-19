# 项目实现说明（当前版本）

这份文档说明：这个仓库里「C++ WebServer + 本地 vLLM」是怎么拼起来的、各模块职责是什么、请求如何流转、如何启动和排障。

## 1. 目标与当前能力

目标：让现有 C++ 服务对外提供本地大模型能力。

当前已实现：

- 对外入口：C++ 服务监听 `8080`
- 对内推理：Python daemon 监听 `19090`，使用 vLLM
- 网页入口（无 Docker）：轻量 Web UI 监听 `7860`
- 协议支持：
  - TCP 行协议
  - HTTP `POST /v1/chat/completions`（OpenAI 风格，支持 `stream: true`）

---

## 2. 总体架构

```text
Browser(UI:7860) -> webui_proxy.py -> C++ Gateway(:8080) -> model_daemon.py(:19090) -> vLLM -> GPU
                                          ^
                                          | (也可直接 curl/OpenAI SDK 调用)
```

核心思想：

- C++ 负责网络接入、协议解析、并发调度
- Python 负责模型生命周期和推理
- 两者通过本地 TCP 文本协议通信，降低耦合

---

## 3. 目录与职责（重点文件）

- `src/main.cc`
  - C++ 网关主逻辑
  - 解析行协议与 HTTP 请求
  - 把推理任务投递到 worker 队列（避免阻塞 event loop）
  - 转发到 `127.0.0.1:19090`
  - 封装 OpenAI 风格响应与 SSE 响应

- `model_daemon.py`
  - 本地模型服务守护进程
  - 启动 vLLM，接收 `INFER\t...` 请求
  - 执行推理并回传 `OK\t...` / `ERR\t...`
  - 有引擎崩溃检测与一次重建重试

- `webui_proxy.py`
  - 轻量网页 + 代理
  - 提供 `http://127.0.0.1:7860`
  - 将网页请求转发到 `http://127.0.0.1:8080/v1/chat/completions`

- `scripts/run_local_model_stack.sh`
  - 一键启动 C++ + daemon
  - 自动清理端口占用（8080/19090）

- `scripts/start_webui.sh`
  - 一键启动本地网页 UI
  - 自动清理端口占用（7860）

---

## 4. 请求链路（两种）

### 4.1 HTTP 链路（推荐）

1. 客户端请求 `POST /v1/chat/completions` 到 `:8080`
2. C++ 解析 JSON（内置解析器）
3. C++ 将 `messages` 扁平化为角色提示词模板：
   - `System:` / `User:` / `Assistant:` ... 最后补 `Assistant:`
4. 任务进入 `InferenceExecutor` worker 队列
5. worker 调用本地 daemon（`19090`）
6. daemon 用 vLLM 推理，返回文本
7. C++ 生成 OpenAI 风格 JSON 返回
8. 若 `stream=true`，返回 `text/event-stream`，最后 `data: [DONE]`

### 4.2 行协议链路（调试方便）

- 简单模式：直接发一行 prompt
- 参数模式：`/infer\t<max_tokens>\t<temperature>\t<prompt>`
- 返回：`OK\t...` 或 `ERR\t...`

---

## 5. 稳定性与防护设计

### 5.1 防阻塞

- 推理不在 `onMessage` 线程里直接执行
- 通过 `InferenceExecutor` 线程池异步执行
- 结果通过 `queueInLoop` 回到连接所属 loop 发送

### 5.2 防爆显存/异常输入

- C++ 侧对 HTTP `max_tokens` 做上限钳制（当前 1024）
- daemon 侧 `--max-tokens-limit` 再做一次硬限制（默认 1024）
- 超大请求行长度有限制（4MB）

### 5.3 引擎崩溃恢复

- daemon 检测到典型 EngineCore 崩溃异常时：
  - 重建 vLLM 引擎
  - 同请求重试一次

### 5.4 端口占用处理

- 启动脚本默认 `AUTO_CLEAN_PORTS=1`
- 自动清理旧进程占用端口：
  - 服务栈：8080/19090
  - 网页 UI：7860

---

## 6. 运行方式（标准流程）

### 6.1 启服务栈

```bash
cd /home/corvus/Desktop/vllmproject/kama-webserver
./scripts/run_local_model_stack.sh
```

常用参数：

```bash
MODEL_NAME=Qwen/Qwen2.5-0.5B-Instruct \
GPU_MEMORY_UTILIZATION=0.75 \
MAX_MODEL_LEN=2048 \
MAX_TOKENS_LIMIT=512 \
./scripts/run_local_model_stack.sh
```

### 6.2 启网页（另一个终端）

```bash
cd /home/corvus/Desktop/vllmproject/kama-webserver
./scripts/start_webui.sh
```

打开：`http://127.0.0.1:7860`

---

## 7. 常见问题与定位

### 问题 A：`Address already in use`

含义：端口被旧进程占用。

处理：

- 默认脚本会自动清理
- 手工清理：`fuser -k -n tcp 8080 19090 7860`

### 问题 B：`Engine core proc ... died unexpectedly`

高概率是显存或 CUDA 运行时压力。

建议：

1. 用更小模型（如 `Qwen2.5-0.5B`）
2. 降 `GPU_MEMORY_UTILIZATION`
3. 降 `MAX_MODEL_LEN`
4. 降 `MAX_TOKENS_LIMIT`

### 问题 C：回答“跑偏/乱答”

当前已改为保留完整 `messages` 上下文，不再只取最后一条消息。
如果仍跑偏，优先调低 temperature，并使用更稳定模型配置。

---

## 8. 对外接口约定（当前）

### HTTP

- 路径：`POST /v1/chat/completions`
- 请求体字段：
  - `messages`（推荐）
  - `max_tokens`（可选）
  - `temperature`（可选）
  - `stream`（可选）

### 行协议

- 请求：`/infer\t<max_tokens>\t<temperature>\t<prompt>`
- 响应：`OK\t...` / `ERR\t...`

---

## 9. 当前边界（已知非目标）

- 不是完整 OpenAI 协议实现（仅覆盖当前需要的子集）
- SSE 为兼容式输出，非 token 级逐步解码
- 无鉴权、多租户、持久会话管理
- 无系统化监控（目前靠日志与脚本）

---

## 10. 下一步建议（按优先级）

1. 增加健康检查与 `doctor` 脚本（端口/进程/API 一键诊断）
2. 增加更严格的请求校验与错误码规范
3. 若面向多人使用，补鉴权与限流
4. 若追求体验，做真正 token 级流式输出
