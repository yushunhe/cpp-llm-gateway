#!/usr/bin/env python3
import argparse
import socketserver
import threading
from dataclasses import dataclass


def escape_field(text: str) -> str:
    return (
        text.replace("\\", "\\\\")
        .replace("\n", "\\n")
        .replace("\r", "\\r")
        .replace("\t", "\\t")
    )


def unescape_field(text: str) -> str:
    out = []
    i = 0
    while i < len(text):
        if text[i] != "\\":
            out.append(text[i])
            i += 1
            continue
        if i + 1 >= len(text):
            out.append("\\")
            break
        n = text[i + 1]
        if n == "n":
            out.append("\n")
        elif n == "r":
            out.append("\r")
        elif n == "t":
            out.append("\t")
        elif n == "\\":
            out.append("\\")
        else:
            out.append("\\")
            out.append(n)
        i += 2
    return "".join(out)


@dataclass
class InferenceRequest:
    prompt: str
    max_tokens: int
    temperature: float


def parse_line(line: str, max_tokens_limit: int) -> InferenceRequest:
    fields = line.rstrip("\n").split("\t")
    if len(fields) < 4 or fields[0] != "INFER":
        raise ValueError("bad request format")
    max_tokens = int(fields[1])
    temperature = float(fields[2])
    if max_tokens <= 0 or max_tokens > max_tokens_limit:
        raise ValueError("max_tokens out of range")
    if temperature < 0.0 or temperature > 2.0:
        raise ValueError("temperature out of range")
    prompt = unescape_field(fields[3])
    if not prompt:
        raise ValueError("empty prompt")
    return InferenceRequest(
        prompt=prompt, max_tokens=max_tokens, temperature=temperature
    )


class Backend:
    def infer(self, req: InferenceRequest) -> str:
        raise NotImplementedError


class MockBackend(Backend):
    def infer(self, req: InferenceRequest) -> str:
        return f"[mock] {req.prompt}"


class VllmBackend(Backend):
    def __init__(
        self,
        model: str,
        trust_remote_code: bool,
        gpu_memory_utilization: float,
        max_model_len: int,
    ):
        from vllm import SamplingParams

        self._model = model
        self._trust_remote_code = trust_remote_code
        self._gpu_memory_utilization = gpu_memory_utilization
        self._max_model_len = max_model_len
        self._SamplingParams = SamplingParams
        self._lock = threading.Lock()
        self._llm = self._build_llm()

    def _build_llm(self):
        from vllm import LLM

        return LLM(
            model=self._model,
            trust_remote_code=self._trust_remote_code,
            gpu_memory_utilization=self._gpu_memory_utilization,
            max_model_len=self._max_model_len,
        )

    @staticmethod
    def _is_engine_crash(exc: Exception) -> bool:
        msg = str(exc)
        return (
            "Engine core proc" in msg
            or "EngineCore" in msg
            or "CUDA error" in msg
            or "CUDA out of memory" in msg
        )

    def infer(self, req: InferenceRequest) -> str:
        params = self._SamplingParams(
            max_tokens=req.max_tokens,
            temperature=req.temperature,
        )
        with self._lock:
            try:
                outputs = self._llm.generate([req.prompt], params)
            except Exception as exc:
                if not self._is_engine_crash(exc):
                    raise
                self._llm = self._build_llm()
                outputs = self._llm.generate([req.prompt], params)
        if not outputs or not outputs[0].outputs:
            return ""
        return outputs[0].outputs[0].text


class TransformersBackend(Backend):
    """Plain HuggingFace transformers backend: no vLLM required.

    Runs comfortably on small models (0.5B-1.5B); falls back to CPU when
    no CUDA device is available.
    """

    def __init__(self, model: str, trust_remote_code: bool):
        import torch
        import transformers
        from transformers import AutoModelForCausalLM, AutoTokenizer

        self._torch = torch
        self._tok = AutoTokenizer.from_pretrained(
            model, trust_remote_code=trust_remote_code
        )
        # no accelerate/device_map: small models load whole onto cuda if present
        dtype_kwargs = (
            {"dtype": "auto"} if int(transformers.__version__[0]) >= 5 else {"torch_dtype": "auto"}
        )
        self._model = AutoModelForCausalLM.from_pretrained(
            model,
            trust_remote_code=trust_remote_code,
            **dtype_kwargs,
        )
        if torch.cuda.is_available():
            self._model = self._model.to("cuda")
        self._model.eval()
        self._lock = threading.Lock()

    def infer(self, req: InferenceRequest) -> str:
        inputs = self._tok(req.prompt, return_tensors="pt").to(self._model.device)
        kwargs = {
            "max_new_tokens": req.max_tokens,
            "do_sample": req.temperature > 0,
            "pad_token_id": self._tok.eos_token_id,
        }
        if req.temperature > 0:
            kwargs["temperature"] = req.temperature
            kwargs["top_p"] = 0.9
        with self._lock, self._torch.no_grad():
            out = self._model.generate(**inputs, **kwargs)
        text = self._tok.decode(
            out[0][inputs["input_ids"].shape[1] :], skip_special_tokens=True
        )
        return text.strip()


class LocalModelHandler(socketserver.StreamRequestHandler):
    backend: Backend = MockBackend()
    max_tokens_limit: int = 1024

    def handle(self) -> None:
        while True:
            raw = self.rfile.readline()
            if not raw:
                return
            if len(raw) > 4 * 1024 * 1024:
                self._write_err("request too large")
                continue
            try:
                req = parse_line(
                    raw.decode("utf-8", errors="replace"),
                    self.max_tokens_limit,
                )
                text = self.backend.infer(req)
                self.wfile.write(f"OK\t{escape_field(text)}\n".encode("utf-8"))
                self.wfile.flush()
            except Exception as exc:
                self._write_err(str(exc))

    def _write_err(self, msg: str) -> None:
        self.wfile.write(f"ERR\t{escape_field(msg)}\n".encode("utf-8"))
        self.wfile.flush()


class ThreadedTCPServer(socketserver.ThreadingMixIn, socketserver.TCPServer):
    daemon_threads = True
    allow_reuse_address = True


def main() -> None:
    parser = argparse.ArgumentParser(description="Local model daemon for kama-webserver")
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=19090)
    parser.add_argument("--model", default="Qwen/Qwen2.5-0.5B-Instruct")
    parser.add_argument("--trust-remote-code", action="store_true")
    parser.add_argument("--gpu-memory-utilization", type=float, default=0.90)
    parser.add_argument("--max-model-len", type=int, default=4096)
    parser.add_argument("--max-tokens-limit", type=int, default=1024)
    parser.add_argument(
        "--backend",
        choices=("vllm", "transformers", "mock"),
        default="vllm",
        help="inference backend (default: vllm)",
    )
    parser.add_argument(
        "--mock",
        action="store_true",
        help="shorthand for --backend mock (no model loaded)",
    )
    args = parser.parse_args()

    if args.mock:
        args.backend = "mock"

    backend: Backend
    if args.backend == "mock":
        backend = MockBackend()
        print(f"[model-daemon] mock backend listening on {args.host}:{args.port}")
    else:
        print(f"[model-daemon] loading model: {args.model} ({args.backend})")
        try:
            if args.backend == "vllm":
                backend = VllmBackend(
                    model=args.model,
                    trust_remote_code=args.trust_remote_code,
                    gpu_memory_utilization=args.gpu_memory_utilization,
                    max_model_len=args.max_model_len,
                )
            else:
                backend = TransformersBackend(
                    model=args.model,
                    trust_remote_code=args.trust_remote_code,
                )
        except Exception as exc:
            print(f"[model-daemon] failed to load model: {exc}")
            raise
        print(f"[model-daemon] ready on {args.host}:{args.port}")

    LocalModelHandler.backend = backend
    LocalModelHandler.max_tokens_limit = args.max_tokens_limit
    with ThreadedTCPServer((args.host, args.port), LocalModelHandler) as server:
        server.serve_forever()


if __name__ == "__main__":
    main()
