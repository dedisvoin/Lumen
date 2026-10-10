"""
Протокол LSP: JSON-RPC-фрейминг, конвертации позиций Scintilla<->LSP
и инкрементальный diff для textDocument/didChange (вместо полной
передачи документа на каждую правку).
"""

import json
import os
import urllib.parse
from pathlib import Path
from typing import Dict, List, Optional, Tuple

from editor.scintilla_constants import (
    SCI_LINEFROMPOSITION,
    SCI_POSITIONFROMLINE,
)


def path_to_uri(path: str) -> str:
    return Path(path).as_uri()


def canonical_uri(uri: str) -> str:
    """Приводит uri из ответа сервера к виду Path.as_uri() (file:///C:/...).

    pyright нормализует Windows-uri в file:///c%3A/... (нижний регистр
    диска, ':' -> %3A), rust-analyzer шлёт file:///c:/... (нижний регистр
    диска), clangd отвечает в формате file:///C:/... . Приводим все формы
    к одному виду (письмо диска в верхнем регистре), чтобы сопоставить
    с ключами документов в LspManager.
    """
    if os.name == "nt" and uri.startswith("file:///"):
        rest = uri[len("file:///"):]
        if len(rest) >= 2 and rest[0].isalpha():
            if rest[1] == ":":
                uri = "file:///" + rest[0].upper() + rest[1:]
            elif rest[1] == "%" and rest[2:4] == "3A":
                uri = "file:///" + rest[0].upper() + ":" + rest[4:]
    return uri


def uri_to_path(uri: str) -> str:
    parsed = urllib.parse.urlparse(uri)
    p = urllib.parse.unquote(parsed.path)
    # file:///C:/... -> C:/...
    if os.name == "nt" and len(p) > 2 and p[0] == "/" and p[2] == ":":
        p = p[1:]
    return p


def _strip_eol(text: str) -> str:
    """Убираем EOL из editor.text(line) — Scintilla добавляет \\n к каждой строке."""
    if text.endswith("\r\n"):
        return text[:-2]
    if text.endswith("\n"):
        return text[:-1]
    return text


def pos_to_lsp(editor, pos: int) -> dict:
    """Байтовая позиция Scintilla -> {line, character} (UTF-16, как в LSP)."""
    line = editor.SendScintilla(SCI_LINEFROMPOSITION, pos)
    line_start = editor.SendScintilla(SCI_POSITIONFROMLINE, line)
    byte_col = pos - line_start
    line_bytes = _strip_eol(editor.text(line)).encode("utf-8")
    prefix = line_bytes[:byte_col].decode("utf-8", "replace")
    return {"line": line,
            "character": len(prefix.encode("utf-16-le")) // 2}


def lsp_to_pos(editor, lsp_pos: dict) -> int:
    """{line, character} (UTF-16) -> байтовая позиция Scintilla."""
    line = lsp_pos.get("line", 0)
    character = lsp_pos.get("character", 0)
    line_start = editor.SendScintilla(SCI_POSITIONFROMLINE, line)
    line_bytes = _strip_eol(editor.text(line)).encode("utf-16-le")
    prefix = line_bytes[: character * 2].decode("utf-16-le", "replace")
    return line_start + len(prefix.encode("utf-8"))


def encode(msg: dict) -> bytes:
    body = json.dumps(msg).encode("utf-8")
    return b"Content-Length: %d\r\n\r\n" % len(body) + body


def normalize_completion(result) -> List[dict]:
    if not result:
        return []
    if isinstance(result, dict):
        return result.get("items") or []
    return result or []


def normalize_locations(result) -> List[Tuple[str, dict]]:
    """(uri, start) для результата definition (Location|Location[]|LocationLink[])."""
    out: List[Tuple[str, dict]] = []
    if isinstance(result, dict):
        if "uri" in result and "range" in result:
            out.append((result["uri"], result["range"].get("start", {})))
        elif "targetUri" in result:
            start = result.get("targetSelectionRange", {}).get("start")
            out.append((result["targetUri"], start))
    elif isinstance(result, list):
        for item in result:
            out.extend(normalize_locations(item))
    return out


def normalize_workspace_edit(result) -> List[Tuple[str, List[Tuple[dict, dict, str]]]]:
    """WorkspaceEdit -> [(uri, [(start, end, new_text)])].

    Обрабатывает обе формы LSP: changes {uri: [TextEdit]} и
    documentChanges: [TextDocumentEdit {textDocument: {uri}, edits}].
    Позиции остаются в LSP-координатах (UTF-16) — конвертация происходит
    при применении (в редактор или в текст файла на диске).
    """
    out: List[Tuple[str, List[Tuple[dict, dict, str]]]] = []
    if not isinstance(result, dict):
        return out

    def _items(edits) -> List[Tuple[dict, dict, str]]:
        items = []
        for e in edits or []:
            if not isinstance(e, dict):
                continue
            rng = e.get("range") or {}
            items.append((rng.get("start", {}), rng.get("end", {}),
                          e.get("newText", "")))
        return items

    for uri, edits in (result.get("changes") or {}).items():
        items = _items(edits)
        if items:
            out.append((uri, items))
    for doc in result.get("documentChanges") or []:
        if not isinstance(doc, dict):
            continue
        uri = (doc.get("textDocument") or {}).get("uri")
        if not uri:
            continue
        items = _items(doc.get("edits"))
        if items:
            out.append((uri, items))
    return out


def lsp_range_to_offsets(text: str, start: dict, end: dict) -> Tuple[int, int]:
    """LSP-диапазон (UTF-16) -> (start, end) в code points строки text.

    Правки к файлу на диске работают с Python str, где индексы — code
    points; LSP считает позиции в UTF-16 units (суррогатная пара = 2).
    """
    lines = text.split("\n")

    def _cp(line_idx: int, char: int) -> int:
        if line_idx >= len(lines):
            return len(text)
        prefix = lines[line_idx].encode("utf-16-le")[:max(char, 0) * 2]
        return len(prefix.decode("utf-16-le", "replace"))

    start_off = (sum(len(l) + 1 for l in lines[:start.get("line", 0)])
                 + _cp(start.get("line", 0), start.get("character", 0)))
    end_off = (sum(len(l) + 1 for l in lines[:end.get("line", 0)])
               + _cp(end.get("line", 0), end.get("character", 0)))
    return start_off, end_off


def apply_text_edits(text: str, edits) -> str:
    """Применяет [(start, end, new_text)] LSP-правки к тексту.

    Правки не пересекаются (гарантия LSP) — применяем от конца к началу,
    чтобы смещения предыдущих замен не съезжали.
    """
    offsets = [(lsp_range_to_offsets(text, s, e), n) for s, e, n in edits]
    for (s, e), n in sorted(offsets, reverse=True):
        text = text[:s] + n + text[e:]
    return text


def hover_text(result) -> Optional[str]:
    if not result:
        return None
    contents = result.get("contents")
    if isinstance(contents, str):
        return contents
    if isinstance(contents, dict):
        return contents.get("value", "")
    if isinstance(contents, list):
        parts = []
        for c in contents:
            if isinstance(c, str):
                parts.append(c)
            elif isinstance(c, dict):
                parts.append(c.get("value") or "")
        return "\n\n".join(p for p in parts if p)
    return None


def normalize_signature_help(result) -> Optional[dict]:
    """SignatureHelp -> {active, param, signatures} для попапа сигнатуры.

    signatures: [{label, doc, params: [(name, start, end)]}], где start/end —
    индексы параметра внутри label (для подсветки активного). У параметра
    label может быть строкой — тогда ищем первое вхождение в label сигнатуры.
    """
    if not isinstance(result, dict):
        return None
    sigs = []
    for s in result.get("signatures") or []:
        if not isinstance(s, dict):
            continue
        label = s.get("label") or ""
        params = []
        for p in s.get("parameters") or []:
            if not isinstance(p, dict):
                continue
            pl = p.get("label")
            if isinstance(pl, list) and len(pl) == 2:
                start, end = int(pl[0]), int(pl[1])
            elif isinstance(pl, str) and pl:
                start = label.find(pl)
                end = start + len(pl) if start >= 0 else -1
            else:
                start = end = -1
            params.append((p.get("name") or "", start, end))
        sigs.append({
            "label": str(label),
            "doc": _plain_doc(s.get("documentation")),
            "params": params,
        })
    if not sigs:
        return None
    return {
        "active": int(result.get("activeSignature") or 0),
        "param": int(result.get("activeParameter") or 0),
        "signatures": sigs,
    }


def _plain_doc(doc) -> str:
    """documentation из LSP: строка или MarkupContent {kind, value} -> текст."""
    if isinstance(doc, dict):
        return doc.get("value") or ""
    return str(doc or "")


def normalize_symbols(result, lines=None) -> List[Tuple[str, int, int, int]]:
    """documentSymbol ответ -> плоский список (name, kind, line, character).

    Некоторые серверы отдают SymbolInformation[]: location.range.start
    указывает на НАЧАЛО определения (class Bar:, def foo), а не на имя —
    позицию имени находим первым вхождением в строке (без регулярок). Стандартные
    DocumentSymbol[] (selectionRange) точны сами по себе; children
    разворачиваем рекурсивно.
    """
    out: List[Tuple[str, int, int, int]] = []
    if not isinstance(result, list):
        return out
    for item in result:
        if not isinstance(item, dict):
            continue
        if "location" in item:  # SymbolInformation
            start = (item.get("location") or {}).get("range") or {}
            start = start.get("start") or {}
            name = item.get("name", "")
            line = start.get("line", 0)
            char = start.get("character", 0)
            if lines and 0 <= line < len(lines):
                found = lines[line].find(name)
                if found >= 0:
                    char = found
            out.append((name, item.get("kind", 0), line, char))
        elif "selectionRange" in item:  # DocumentSymbol (вложенный)
            start = (item.get("selectionRange") or {}).get("start") or {}
            out.append((item.get("name", ""), item.get("kind", 0),
                        start.get("line", 0), start.get("character", 0)))
            out.extend(normalize_symbols(item.get("children"), lines))
    return out


def normalize_symbol_tree(result, lines=None) -> List[dict]:
    """documentSymbol ответ -> дерево для breadcrumbs.

    Два формата LSP: DocumentSymbol[] (selectionRange + children) и
    SymbolInformation[] (location + containerName — basedpyright и др.).
    Первый уже дерево, второй — плоский список: собираем иерархию по
    containerName. Узел: {name, kind, start, end, children}.
    """
    docs: List[dict] = []
    flat: List[dict] = []
    if isinstance(result, list):
        for item in result:
            if not isinstance(item, dict):
                continue
            if "selectionRange" in item:
                docs.append(_normalize_doc_symbol(item, lines))
                continue
            rng = (item.get("location") or {}).get("range") or {}
            start = rng.get("start")
            end = rng.get("end")
            if not start or not end:
                continue  # без диапазона символ для крошек бесполезен
            flat.append({
                "name": item.get("name", ""),
                "kind": item.get("kind", 0),
                "detail": item.get("detail") or "",
                "start": (start.get("line", 0), start.get("character", 0)),
                "end": (end.get("line", 0), end.get("character", 0)),
                "file": item.get("file") or "",
                "container": item.get("containerName"),
            })
    if docs or not flat:
        return docs
    return _build_tree_from_flat(flat)


def _normalize_doc_symbol(item: dict, lines) -> dict:
    """DocumentSymbol -> узел дерева (range/selectionRange, дети рекурсивно)."""
    rng = item.get("range") or {}
    sel = item.get("selectionRange") or {}
    start = rng.get("start") or sel.get("start") or {}
    end = rng.get("end") or sel.get("end") or {}
    return {
        "name": item.get("name", ""),
        "kind": item.get("kind", 0),
        "detail": item.get("detail") or "",
        "start": (start.get("line", 0), start.get("character", 0)),
        "end": (end.get("line", 0), end.get("character", 0)),
        "file": item.get("file") or "",
        "children": normalize_symbol_tree(item.get("children"), lines),
    }


def _build_tree_from_flat(flat: List[dict]) -> List[dict]:
    """SymbolInformation[] -> дерево: дети цепляются к родителю.

    Сначала по containerName (basedpyright и др.), затем по вложенности
    range (clangd отдаёт плоский список без containerName: родитель —
    наименьший символ, чей диапазон строго содержит диапазон узла).
    """
    roots: List[dict] = []
    by_name: Dict[str, dict] = {}
    for node in flat:
        node["children"] = []
        if node.get("name") and node["name"] not in by_name:
            by_name[node["name"]] = node
    for node in flat:
        parent = by_name.get(node.get("container")) or _range_parent(node, flat)
        if parent is None:
            roots.append(node)
        else:
            parent["children"].append(node)
    return roots


def _range_parent(node: dict, all_nodes: List[dict]) -> Optional[dict]:
    """Наименьший символ, чей range строго содержит range узла (или None)."""
    s, e = node["start"], node["end"]
    best = None
    best_area: Optional[Tuple[int, int]] = None
    for other in all_nodes:
        os_, oe = other["start"], other["end"]
        if (os_, oe) == (s, e) or not (os_ <= s and oe >= e):
            continue
        area = (oe[0] - os_[0], oe[1] - os_[1])
        if best_area is None or area < best_area:
            best, best_area = other, area
    return best


def symbol_path_at(tree, editor, pos) -> List[Tuple[str, int, int, int]]:
    """Дерево documentSymbol -> путь (name, kind, start, end) под позицией.

    Спускаемся по символам, чей диапазон содержит pos (range включает тело
    символа): сегменты хлебных крошек — от корня к самому вложенному.
    """
    out: List[Tuple[str, int, int, int]] = []
    for node in tree:
        start = node.get("start")
        end = node.get("end")
        if not start or not end:
            continue
        s = lsp_to_pos(editor, {"line": start[0], "character": start[1]})
        e = lsp_to_pos(editor, {"line": end[0], "character": end[1]})
        if not (s <= pos < e):
            continue
        name = node.get("name", "")
        if name:
            out.append((name, node.get("kind", 0), s, e))
        out.extend(symbol_path_at(node.get("children") or [], editor, pos))
        break
    return out


# LSP SymbolKind -> имя токена из _SEM_TOKEN_MAP (semantic_lexer.py)
_SYMBOL_KIND_NAME = {
    2: "namespace", 5: "class", 6: "method", 7: "property",
    8: "variable", 10: "enum", 12: "function", 13: "variable",
    14: "enumMember",
}


def symbols_to_tokens(symbols) -> List[int]:
    """(name, kind, line, character) -> относительные токены
    [d_line, d_char, length, имя_токена, 0] — формат data semanticTokens.

    Так bridge раскрашивает определения через существующий _SEM_TOKEN_MAP
    (class/function/method/...), без отдельного кода для documentSymbol.
    """
    data: List[int] = []
    prev_line = prev_char = 0
    for name, kind, line, char in sorted(symbols, key=lambda s: (s[2], s[3])):
        if not name:
            continue
        length = len(name.encode("utf-16-le")) // 2
        d_line = line - prev_line
        d_char = char if d_line else char - prev_char
        data.extend((d_line, d_char, length,
                     _SYMBOL_KIND_NAME.get(kind, "variable"), 0))
        prev_line, prev_char = line, char + length
    return data


def symbol_names(symbols) -> Dict[str, str]:
    """(name, kind, ...) -> {имя: имя_токена}: имена из documentSymbol для
    подсветки вхождений (использование класса/функции в коде)."""
    names: Dict[str, str] = {}
    for name, kind, _, _ in symbols:
        if name:
            names[name] = _SYMBOL_KIND_NAME.get(kind, "variable")
    return names


# ---------------------------------------------------------------------------
# Инкрементальные didChange: diff по байтам (префикс/суффикс), LSP-диапазоны
# ---------------------------------------------------------------------------

_MAX_CHANGE_REGION = 8192  # больше изменённого региона — полная синхронизация


def incremental_changes(old_text: str, new_text: str) -> Optional[List[dict]]:
    """contentChanges для didChange: только изменённый диапазон.

    Общий префикс/суффикс вырезается на уровне code points, затем смещения
    переводятся в байты (всегда на границе UTF-8 символа); если изменённый
    регион больше _MAX_CHANGE_REGION — возвращаем None (серверу уйдёт полный
    текст, как фолбэк). Диапазон в LSP-координатах {line, character} UTF-16.
    """
    # Ищем общий префикс/суффикс на уровне code points, а не байт: побайтовое
    # сравнение ломается на многобайтовых UTF-8 символах, т.к. ведущие байты
    # разных символов могут совпадать (µ=0xC2 0xB5 и ¢=0xC2 0xA2) и префикс
    # останавливается на байте-продолжении -> невыровненный срез -> DecodeError.
    n_c, m_c = len(old_text), len(new_text)
    p = 0
    while p < n_c and p < m_c and old_text[p] == new_text[p]:
        p += 1
    s = 0
    while s < n_c - p and s < m_c - p and old_text[n_c - 1 - s] == new_text[m_c - 1 - s]:
        s += 1

    old_b = old_text.encode("utf-8")
    new_b = new_text.encode("utf-8")
    n, m = len(old_b), len(new_b)
    # code-point смещения -> байтовые (всегда на границе символа UTF-8)
    p_b = len(old_text[:p].encode("utf-8"))
    s_b = len(old_text[n_c - s:].encode("utf-8")) if s else 0
    o_len, nn_len = n - p_b - s_b, m - p_b - s_b
    if o_len == 0 and nn_len == 0:
        return []
    if o_len > _MAX_CHANGE_REGION or nn_len > _MAX_CHANGE_REGION:
        return None
    return [{
        "range": {
            "start": _lsp_position_at(old_b, p_b),
            "end": _lsp_position_at(old_b, n - s_b),
        },
        "rangeLength": _utf16_len(old_b, p_b, n - s_b),
        "text": new_b[p_b:m - s_b].decode("utf-8"),
    }]


def _lsp_position_at(text_b: bytes, byte_off: int) -> dict:
    """{line, character} по байтовому смещению в документе (UTF-16 колонка)."""
    nl = text_b.rfind(b"\n", 0, byte_off)
    line = text_b.count(b"\n", 0, byte_off)
    seg = text_b[nl + 1:byte_off].decode("utf-8", "replace")
    return {"line": line, "character": len(seg.encode("utf-16-le")) // 2}


def _utf16_len(text_b: bytes, start: int, end: int) -> int:
    """Длина байтового отрезка в UTF-16 code units (для rangeLength)."""
    seg = text_b[start:end].decode("utf-8", "replace")
    return len(seg.encode("utf-16-le")) // 2
