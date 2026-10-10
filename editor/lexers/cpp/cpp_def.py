"""
C++ — описание языка: встроенный лексер + LSP (clangd).

Вся пер-языковая логика (поиск clangd/clang, probing стандартов C++,
варианты комбобокса «C++ standard») вынесена сюда из старого
editor/lsp/config.py. Ядро ничего про C++ не знает — только language_id.
"""

import os
import shutil
import subprocess
import time
from typing import List, Optional, Tuple

from PyQt6.Qsci import QsciLexerCPP

from editor.lexers.language_def import LanguageDef
from editor.lexers.lsp_types import LspServerConfig
from editor.settings_store import app_settings
from editor.text_utils import CPP_KEYWORDS, CPP_TYPES


# --- поиск бинарей clangd/clang ---

def _find_clangd() -> Optional[str]:
    """clangd из PATH или из известных путей установки LLVM."""
    _CLANGD_FALLBACKS = (
        r"D:\clang\clang+llvm-21.1.1-x86_64-pc-windows-msvc\bin\clangd.exe",
        r"D:\mingw64\bin\clangd.exe",
        r"C:\Program Files\LLVM\bin\clangd.exe",
        r"C:\msys64\mingw64\bin\clangd.exe",
    )
    exe = shutil.which("clangd")
    if exe:
        return exe
    for p in _CLANGD_FALLBACKS:
        if os.path.isfile(p):
            return p
    return None


def _resolve_clang() -> Optional[str]:
    """clang.exe рядом с clangd (та же установка LLVM), иначе из PATH."""
    path = app_settings().get_str("lsp.clangd_path", "").strip()
    clangd = path if (path and os.path.isfile(path)) else _find_clangd()
    if clangd:
        for name in ("clang++.exe", "clang.exe"):
            cand = os.path.join(os.path.dirname(clangd), name)
            if os.path.isfile(cand):
                return cand
    return shutil.which("clang") or shutil.which("clang++")


# --- доступные стандарты C++ для clangd (--std) ---

_CPP_STD_LABELS = ("C++11", "C++14", "C++17", "C++20", "C++23", "C++26")

# Порядок от новейшего к старшему: probing идёт сверху вниз, а итоговая
# цепочка — все стандарты до максимально поддержанного включительно.
_CPP_STD_VALUES = ("c++11", "c++14", "c++17", "c++20", "c++23", "c++26")

# Кэш определения максимального стандарта: (timestamp, индекс в _CPP_STD_VALUES)
# None — clang не найден (кэшируем и «не найден», чтобы не гонять PATH каждое
# открытие панели настроек).
_std_probe_cache: List = [0.0, None]

_STD_PROBE_TTL = 60.0  # секунд жизни кэша


def _max_supported_std() -> Optional[int]:
    """Индекс в _CPP_STD_VALUES самого нового стандарта, который принимает
    установленный clang (`clang -std=c++XX -x c++ -fsyntax-only`). None —
    clang недоступен или ни один стандарт не прошёл."""
    now = time.monotonic()
    if now - _std_probe_cache[0] < _STD_PROBE_TTL:
        return _std_probe_cache[1]

    clang = _resolve_clang()
    max_idx = None
    if clang:
        for idx in range(len(_CPP_STD_VALUES) - 1, -1, -1):
            std = _CPP_STD_VALUES[idx]
            try:
                proc = subprocess.run(
                    [clang, "-x", "c++", f"-std={std}",
                     "-fsyntax-only", "-"],
                    input=b"", capture_output=True, timeout=15)
            except (OSError, subprocess.SubprocessError):
                break
            if proc.returncode == 0:
                max_idx = idx
                break
    _std_probe_cache[0] = now
    _std_probe_cache[1] = max_idx
    return max_idx


def cpp_standard_options() -> List[Tuple[str, str]]:
    """Варианты для combo «C++ standard»: [(label, value), ...].

    Значения из списка стандартов, которые поддерживает установленный
    clang (или статичный список, если clang не найден). Возвращается
    от новейшего к старшему, чтобы новые стандарты были видны сразу.
    """
    max_idx = _max_supported_std()
    if max_idx is None:
        max_idx = len(_CPP_STD_VALUES) - 1
    labels = _CPP_STD_LABELS[:max_idx + 1]
    values = _CPP_STD_VALUES[:max_idx + 1]
    return [("Auto", "")] + list(zip(reversed(labels), reversed(values)))


def cpp_def() -> LanguageDef:
    """Описание языка C++ для агрегатора editor/lexers/registry.py."""
    return LanguageDef(
        id="cpp",
        extensions=[".c", ".cpp", ".cxx", ".cc", ".h", ".hpp"],
        builtin_lexer=QsciLexerCPP,
        lsp=LspServerConfig(
            language_id="cpp",
            server_name="clangd",
            command=[_find_clangd() or "clangd",
                     "--background-index",
                     "--header-insertion=iwyu",
                     "--completion-style=detailed",
                     "--log=error",
                     "--compile-commands-dir={workspace}"],
            root_markers=("compile_commands.json",),
            semantic_tokens=True,
            inlay_hints=True,
            keywords=CPP_KEYWORDS,
            type_keywords=CPP_TYPES,
        ),
        settings_hooks={"cpp_standard": cpp_standard_options},
    )
