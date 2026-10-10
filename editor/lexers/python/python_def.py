"""
Python — описание языка: встроенный лексер + LSP (basedpyright).

Пер-языковая логика (поиск basedpyright-langserver, ключевые слова)
вынесена сюда из старого editor/lsp/config.py.
"""

import os
import shutil
from typing import List, Optional, Tuple

from PyQt6.Qsci import QsciLexerPython

from editor.lexers.language_def import LanguageDef
from editor.lexers.lsp_types import LspServerConfig, _interpreter_path


_PY_KEYWORDS = ("False", "None", "True", "and", "as", "assert", "async",
                "await", "break", "class", "continue", "def", "del", "elif",
                "else", "except", "finally", "for", "from", "global", "if",
                "import", "in", "is", "lambda", "nonlocal", "not", "or",
                "pass", "raise", "return", "try", "while", "with", "yield",
                "match", "case")

_PY_TYPES = ("bool", "bytearray", "bytes", "complex", "dict", "float", "frozenset",
             "int", "list", "memoryview", "object", "range", "set", "slice",
             "str", "tuple", "type", "NoneType", "Any", "Optional", "Union",
             "Callable", "Iterable", "Iterator", "Sequence", "Mapping",
             "List", "Dict", "Set", "Tuple")


def _find_basedpyright() -> Optional[List[str]]:
    """Команда запуска basedpyright-langserver: бинарь из PATH либо из
    Scripts рядом с python.exe (pip кладёт туда скрипты, а папки может
    не быть в PATH — например, при установке для одного пользователя)."""
    exe = shutil.which("basedpyright-langserver")
    if exe:
        return [exe]
    py_dir = os.path.dirname(_interpreter_path())
    for p in (os.path.join(py_dir, "Scripts", "basedpyright-langserver.exe"),
              os.path.join(py_dir, "basedpyright-langserver.exe")):
        if os.path.isfile(p):
            return [p]
    return None


def python_def() -> LanguageDef:
    """Описание языка Python для агрегатора editor/lexers/registry.py."""
    return LanguageDef(
        id="python",
        extensions=[".py", ".pyw", ".pyi"],
        builtin_lexer=QsciLexerPython,
        lsp=LspServerConfig(
            language_id="python",
            server_name="basedpyright",
            command=(_find_basedpyright() or ["basedpyright-langserver"])
                    + ["--stdio"],
            root_markers=(".git", "pyproject.toml", "setup.py", "setup.cfg",
                          "requirements.txt"),
            trigger_chars={"."},
            semantic_tokens=True,
            inlay_hints=True,
            keywords=_PY_KEYWORDS,
            type_keywords=_PY_TYPES,
            line_comment="#",
            block_comments=False,
            triple_quotes=True,
            # Только открытые файлы: быстрый старт без анализа всего workspace.
            # Шумные unused-диагностики выключены (как раньше pycodestyle).
            # Langserver игнорирует initializationOptions и CLI-флаги pythonPath —
            # настройки он читает из workspace/didChangeConfiguration (server.py).
            # pythonPath = интерпретатор, под которым запущен редактор (3.13.7).
            initial_options={
                "python": {"pythonPath": _interpreter_path()},
                "basedpyright": {"analysis": {
                    "diagnosticMode": "openFilesOnly",
                    "reportUnusedImport": "none",
                    "reportUnusedVariable": "none",
                    "reportUnusedExpression": "none",
                    "inlayHints": {
                        "parameterNames": "all",
                        "callArgumentNames": "all",
                        "functionReturnTypes": True,
                        "variableTypes": True,
                    },
                }},
            },
        ),
    )
