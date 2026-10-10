"""
LspServerConfig — описание LSP-сервера для одного языка (JSON-RPC по stdio).

Вынесено из editor/lsp/config.py в editor/lexers, т.к. это описание
языка, а не ядро LSP. Ядро (editor/lsp/{manager,bridge,server,protocol})
импортирует тип отсюда через фасад editor/lsp/config.py.
"""

import dataclasses
import importlib.util
import os
import shutil
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Dict, List, Optional, Set, Tuple

from editor.resources import editor_path


# Nuitka определяет __compiled__ в каждом скомпилированном модуле
_COMPILED = "__compiled__" in globals()


def _interpreter_path() -> str:
    """Реальный python.exe для LSP-серверов.

    В скомпилированном exe sys.executable — сам exe, поэтому путь к
    интерпретатору запекается при сборке (build.py -> editor/interpreter.txt).
    """
    if _COMPILED:
        marker = editor_path("interpreter.txt")
        try:
            p = marker.read_text(encoding="utf-8").strip()
        except OSError:
            p = ""
        if p and os.path.isfile(p):
            return p
    return sys.executable


@dataclass
class LspServerConfig:
    """Описание LSP-сервера для одного языка (JSON-RPC по stdio)."""

    language_id: str                    # LSP languageId (textDocument/languageId)
    command: List[str]                  # argv; первый элемент — бинарь (путь или имя из PATH)
    server_name: str = ""              # имя для статус-бара (basedpyright, clangd...)
    root_markers: Tuple[str, ...] = ()  # файлы, определяющие корень воркспейса
    trigger_chars: Set[str] = field(default_factory=lambda: {".", ":"})
    semantic_tokens: bool = False       # textDocument/semanticTokens/full
    inlay_hints: bool = False           # textDocument/inlayHint
    pull_diagnostics: bool = True       # LSP 3.17 pull (textDocument/diagnostic);
                                        # False — push (textDocument/publishDiagnostics)
    error_lens: bool = True             # инлайн-диагностики (ErrorLens)
    hover: bool = True                  # hover-тултипы
    keywords: Tuple[str, ...] = ()      # fallback-ключевые слова семант. лексера
    type_keywords: Tuple[str, ...] = () # fallback-типы семант. лексера
    line_comment: str = "//"            # комментарий строки для fallback-лексера
    block_comments: bool = True         # /* */ для fallback-лексера
    triple_quotes: bool = False         # """ """ / ''' ''' (Python-строки)
    initial_options: dict = field(default_factory=dict)  # initializationOptions
    capabilities: dict = field(default_factory=dict)     # дозапись в initialize

    def resolve_command(self, workspace: str) -> Optional[List[str]]:
        """Резолвит бинарь (имя из PATH / абсолютный путь) и подставляет
        {workspace} в аргументы. None — сервер недоступен."""
        if not self.command:
            return None
        exe = self.command[0]
        if not (os.path.isabs(exe) and os.path.isfile(exe)):
            found = shutil.which(exe)
            if not found:
                return None
            exe = found
        args = [str(a).replace("{workspace}", workspace)
                for a in self.command[1:]]
        return [exe, *args]
