"""Локальная документация для ASM-лексера.

Документация хранится в asm_docs.json рядом с этим файлом. Загрузчик строит
регистронезависимый индекс для инструкций, регистров, типов, директив и
специальных ключевых слов. Используется для hover и автодополнения, когда LSP
недоступен.
"""

from __future__ import annotations

import json
from functools import lru_cache
from pathlib import Path
from typing import Optional


_DOC_PATH = Path(__file__).with_name("asm_docs.json")


def _normalize(token: str) -> str:
    return (token or "").strip().lower()


@lru_cache(maxsize=1)
def load_asm_docs() -> dict:
    try:
        with _DOC_PATH.open("r", encoding="utf-8") as f:
            data = json.load(f)
    except Exception:
        return {"categories": {}}

    categories = data.get("categories") or {}
    index: dict[str, dict] = {}
    for cat_name, items in categories.items():
        if not isinstance(items, dict):
            continue
        for key, doc in items.items():
            if not isinstance(doc, dict):
                continue
            norm = _normalize(key)
            if not norm:
                continue
            item = dict(doc)
            item.setdefault("name", doc.get("name") or key)
            item.setdefault("kind", cat_name)
            index[norm] = item
            if norm.startswith('.'):
                index.setdefault(norm.lstrip('.'), item)
            else:
                index.setdefault('.' + norm, item)
    data["_index"] = index
    return data


def get_asm_doc(token: str) -> Optional[dict]:
    token = _normalize(token)
    if not token:
        return None
    data = load_asm_docs()
    index = data.get("_index") or {}
    doc = index.get(token)
    if doc is not None:
        return doc
    if token.startswith('.'):
        return index.get(token.lstrip('.'))
    return index.get('.' + token)


_KIND_RU = {
    "instruction": "инструкция",
    "register": "регистр",
    "type": "тип",
    "directive": "директива",
    "special": "специальное ключевое слово",
    "symbol": "символ",
}


def format_asm_doc(doc: dict) -> str:
    if not isinstance(doc, dict):
        return ""
    name = doc.get("name") or doc.get("signature") or "ASM"
    kind = doc.get("kind") or "symbol"
    kind_display = _KIND_RU.get(str(kind).lower(), kind)
    signature = doc.get("signature") or name
    summary = (doc.get("summary") or "").strip()
    description = (doc.get("description") or "").strip()
    examples = doc.get("examples") or []
    notes = doc.get("notes") or []

    lines = [f"```asm\n{signature}\n```", f"*{kind_display}*"]
    if summary:
        lines += ["", summary]
    if description and description != summary:
        lines += ["", description]
    if examples:
        lines += ["", "**Примеры**"]
        for ex in examples:
            ex = str(ex).rstrip()
            if ex:
                lines += ["```asm", ex, "```"]
    if notes:
        lines += ["", "**Примечания**"]
        for note in notes:
            note = str(note).strip()
            if note:
                lines += [f"- {note}"]
    return "\n".join(lines).strip()


def get_asm_doc_text(token: str) -> str:
    doc = get_asm_doc(token)
    return format_asm_doc(doc) if doc else ""
