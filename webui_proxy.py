#!/usr/bin/env python3
import json
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib import request as urllib_request
from urllib.error import URLError


BACKEND_URL = "http://127.0.0.1:8080/v1/chat/completions"


HTML = """<!doctype html>
<html lang="zh-CN">
<head>
  <meta charset="utf-8" />
  <meta name="viewport" content="width=device-width, initial-scale=1" />
  <title>Local LLM WebUI</title>
  <style>
    :root { --bg:#0f172a; --panel:#111827; --muted:#94a3b8; --text:#e5e7eb; --accent:#10b981; }
    body { margin:0; font-family: ui-sans-serif, system-ui, sans-serif; background: radial-gradient(circle at 20% 20%, #1f2937 0, #0f172a 45%, #020617 100%); color: var(--text); }
    .wrap { max-width: 900px; margin: 32px auto; padding: 16px; }
    .card { background: rgba(17,24,39,.85); border: 1px solid #334155; border-radius: 14px; padding: 16px; backdrop-filter: blur(4px); }
    h1 { margin: 0 0 10px; font-size: 22px; }
    p { margin: 0 0 12px; color: var(--muted); }
    textarea { width: 100%; min-height: 140px; resize: vertical; border-radius: 10px; border: 1px solid #475569; background:#0b1220; color: var(--text); padding: 10px; }
    .row { display:flex; gap:10px; align-items:center; margin-top:10px; flex-wrap:wrap; }
    input, button { border-radius: 8px; border: 1px solid #475569; background:#0b1220; color: var(--text); padding:8px 10px; }
    button { background: linear-gradient(90deg,#059669,#10b981); border:0; color:#052e2b; font-weight:700; cursor:pointer; }
    pre { white-space: pre-wrap; line-height:1.45; background:#020617; border:1px solid #334155; border-radius: 10px; padding:12px; min-height:100px; }
    .err { color: #fca5a5; }
  </style>
</head>
<body>
  <div class="wrap">
    <div class="card">
      <h1>Local LLM WebUI</h1>
      <p>此页面通过本地代理调用 <code>/v1/chat/completions</code>，无需 Docker。</p>
      <textarea id="prompt" placeholder="输入问题，例如：用三句话解释 epoll"></textarea>
      <div class="row">
        <label>max_tokens <input id="maxTokens" type="number" value="256" min="1" max="4096" /></label>
        <label>temperature <input id="temperature" type="number" value="0.7" min="0" max="2" step="0.1" /></label>
        <button id="sendBtn">发送</button>
      </div>
      <p id="status"></p>
      <pre id="output"></pre>
    </div>
  </div>
  <script>
    const sendBtn = document.getElementById('sendBtn');
    const prompt = document.getElementById('prompt');
    const statusEl = document.getElementById('status');
    const output = document.getElementById('output');
    const maxTokens = document.getElementById('maxTokens');
    const temperature = document.getElementById('temperature');

    sendBtn.addEventListener('click', async () => {
      const text = prompt.value.trim();
      if (!text) return;
      statusEl.textContent = '请求中...';
      statusEl.className = '';
      output.textContent = '';
      try {
        const resp = await fetch('/chat', {
          method: 'POST',
          headers: {'Content-Type': 'application/json'},
          body: JSON.stringify({
            prompt: text,
            max_tokens: Number(maxTokens.value || 256),
            temperature: Number(temperature.value || 0.7)
          })
        });
        const data = await resp.json();
        if (!resp.ok) throw new Error(data.error || 'request failed');
        output.textContent = data.text || '';
        statusEl.textContent = '完成';
      } catch (err) {
        statusEl.textContent = '失败';
        statusEl.className = 'err';
        output.textContent = String(err);
      }
    });
  </script>
</body>
</html>
"""


class Handler(BaseHTTPRequestHandler):
    def _send_json(self, code: int, payload: dict) -> None:
        body = json.dumps(payload, ensure_ascii=False).encode("utf-8")
        self.send_response(code)
        self.send_header("Content-Type", "application/json; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_GET(self) -> None:
        if self.path != "/":
            self.send_error(HTTPStatus.NOT_FOUND, "not found")
            return
        body = HTML.encode("utf-8")
        self.send_response(HTTPStatus.OK)
        self.send_header("Content-Type", "text/html; charset=utf-8")
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def do_POST(self) -> None:
        if self.path != "/chat":
            self.send_error(HTTPStatus.NOT_FOUND, "not found")
            return
        try:
            length = int(self.headers.get("Content-Length", "0"))
        except ValueError:
            self._send_json(400, {"error": "invalid content-length"})
            return
        raw = self.rfile.read(length)
        try:
            payload = json.loads(raw.decode("utf-8"))
        except Exception:
            self._send_json(400, {"error": "invalid json"})
            return

        prompt = str(payload.get("prompt", "")).strip()
        if not prompt:
            self._send_json(400, {"error": "prompt is required"})
            return
        max_tokens = int(payload.get("max_tokens", 256))
        temperature = float(payload.get("temperature", 0.7))

        req_data = {
            "model": "local",
            "messages": [{"role": "user", "content": prompt}],
            "max_tokens": max_tokens,
            "temperature": temperature,
        }
        req = urllib_request.Request(
            BACKEND_URL,
            data=json.dumps(req_data).encode("utf-8"),
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with urllib_request.urlopen(req, timeout=120) as resp:
                backend = json.loads(resp.read().decode("utf-8"))
        except URLError as exc:
            self._send_json(502, {"error": f"backend unavailable: {exc}"})
            return
        except Exception as exc:
            self._send_json(502, {"error": f"backend error: {exc}"})
            return

        try:
            text = backend["choices"][0]["message"]["content"]
        except Exception:
            self._send_json(
                500, {"error": "unexpected backend response", "raw": backend}
            )
            return
        self._send_json(200, {"text": text})


def main() -> None:
    server = ThreadingHTTPServer(("127.0.0.1", 7860), Handler)
    print("[webui] http://127.0.0.1:7860")
    print("[webui] backend -> http://127.0.0.1:8080/v1/chat/completions")
    server.serve_forever()


if __name__ == "__main__":
    main()
