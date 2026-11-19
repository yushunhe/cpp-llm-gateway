#!/usr/bin/env python3
"""Offline RAG index builder: docs dir -> chunk -> embedding -> FAISS.

Usage:
    .venv/bin/python rag/build_index.py [docs_dir]

Defaults: docs from rag/docs, index written to rag/kb.
Embeddings run locally (sentence-transformers); only generation
goes through the C++ gateway at query time.
"""
import argparse
import shutil
import sys
from pathlib import Path

from langchain_community.vectorstores import FAISS
from langchain_huggingface import HuggingFaceEmbeddings
from langchain_text_splitters import RecursiveCharacterTextSplitter

HERE = Path(__file__).resolve().parent
DEFAULT_DOCS = HERE / "docs"
DEFAULT_KB = HERE / "kb"
EMBED_MODEL = "BAAI/bge-small-zh-v1.5"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("docs_dir", nargs="?", default=str(DEFAULT_DOCS))
    parser.add_argument("--kb", default=str(DEFAULT_KB), help="output index dir")
    parser.add_argument("--chunk-size", type=int, default=500)
    parser.add_argument("--chunk-overlap", type=int, default=100)
    args = parser.parse_args()

    docs_dir = Path(args.docs_dir)
    files = sorted(
        p for p in docs_dir.rglob("*") if p.suffix.lower() in (".md", ".txt")
    )
    if not files:
        sys.exit(f"[build_index] no .md/.txt files found under {docs_dir}")

    from langchain_community.document_loaders import TextLoader

    documents = []
    for path in files:
        documents.extend(
            TextLoader(str(path), encoding="utf-8").load()
        )
    print(f"[build_index] loaded {len(documents)} documents from {docs_dir}")

    splitter = RecursiveCharacterTextSplitter(
        chunk_size=args.chunk_size,
        chunk_overlap=args.chunk_overlap,
        separators=["\n## ", "\n# ", "\n\n", "\n", "。", " ", ""],
    )
    chunks = splitter.split_documents(documents)
    print(f"[build_index] split into {len(chunks)} chunks")

    embeddings = HuggingFaceEmbeddings(
        model_name=EMBED_MODEL,
        encode_kwargs={"normalize_embeddings": True},
    )
    # full rebuild: a stale kb dir would otherwise mix old chunks in
    kb_dir = Path(args.kb)
    if kb_dir.exists():
        shutil.rmtree(kb_dir)
    vectorstore = FAISS.from_documents(chunks, embeddings)
    vectorstore.save_local(str(kb_dir))
    print(f"[build_index] index saved to {kb_dir} ({len(chunks)} vectors)")


if __name__ == "__main__":
    main()
