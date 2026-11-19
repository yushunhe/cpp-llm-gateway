#!/usr/bin/env python3
"""RAG query entrypoint: retrieve from FAISS, then generate via the C++ gateway.

Pipeline: question -> embedding -> FAISS top-k -> prompt -> POST
http://127.0.0.1:8080/v1/chat/completions (kama-webserver) ->
model_daemon -> local LLM.

Usage:
    .venv/bin/python rag/ask.py "你的问题"
    .venv/bin/python rag/ask.py            # interactive mode
"""
import argparse
from pathlib import Path

from langchain_community.vectorstores import FAISS
from langchain_huggingface import HuggingFaceEmbeddings
from langchain_openai import ChatOpenAI

from build_index import EMBED_MODEL

HERE = Path(__file__).resolve().parent

PROMPT_TEMPLATE = """你是本项目的技术问答助手。请只根据下面提供的资料回答问题；\
资料里没有的内容，直接说不知道，不要编造。回答末尾列出你依据的资料来源。

【资料】
{context}

【问题】{question}
"""


def format_context(docs) -> str:
    parts = []
    for i, doc in enumerate(docs, 1):
        source = doc.metadata.get("source", "unknown")
        parts.append(f"[资料{i} | 来源: {source}]\n{doc.page_content}")
    return "\n\n".join(parts)


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("question", nargs="?", help="leave empty for interactive mode")
    parser.add_argument("--kb", default=str(HERE / "kb"))
    parser.add_argument("--topk", type=int, default=4)
    parser.add_argument(
        "--gateway",
        default="http://127.0.0.1:8080/v1",
        help="OpenAI-compatible base URL of the C++ server",
    )
    parser.add_argument("--model", default="local-vllm-bridge")
    parser.add_argument("--max-tokens", type=int, default=256)
    args = parser.parse_args()

    embeddings = HuggingFaceEmbeddings(
        model_name=EMBED_MODEL,
        encode_kwargs={"normalize_embeddings": True},
    )
    vectorstore = FAISS.load_local(
        args.kb, embeddings, allow_dangerous_deserialization=True
    )
    # ChatOpenAI appends /chat/completions to base_url -> hits our C++ :8080
    llm = ChatOpenAI(
        base_url=args.gateway,
        api_key="sk-local",
        model=args.model,
        max_tokens=args.max_tokens,
        temperature=0.2,
        timeout=120,
    )

    def ask(question: str) -> None:
        docs = vectorstore.similarity_search(question, k=args.topk)
        prompt = PROMPT_TEMPLATE.format(
            context=format_context(docs), question=question
        )
        print("\n--- 检索命中 ---")
        for i, doc in enumerate(docs, 1):
            print(f"[{i}] {doc.metadata.get('source')} :: {doc.page_content[:60]}...")
        print("\n--- 生成中(经 C++ 网关 :8080) ---")
        answer = llm.invoke(prompt)
        print(f"\n{answer.content}")

    if args.question:
        ask(args.question)
        return
    while True:
        try:
            q = input("\nRAG> ").strip()
        except EOFError:
            break
        if q in ("", "exit", "quit"):
            break
        ask(q)


if __name__ == "__main__":
    main()
