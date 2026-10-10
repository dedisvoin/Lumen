"""
Lumen — описание языка: кастомный лексер (LumenLangLexer) + LSP-сервер.

LSP-сервер (lumen-lsp.exe) собирается из lumen-lsp.cpp (в корне рядом с
lumenc.cpp) и публикует диагностики (textDocument/publishDiagnostics),
семантические токены (textDocument/semanticTokens/full) и подсказки.
Подсветка .lm идёт через SemanticLexer: арки LSP-токенов + лёгкий
fallback-лексер (общие списки слов для C++), поэтому lumenlang передаёт
пустые keywords/type_keywords как разрешено конструктором.
"""
import os
import shutil

from editor.lexers.language_def import LanguageDef
from editor.lexers.lumen.lumen_lang import LumenLangLexer
from editor.lexers.lsp_types import LspServerConfig
from editor.resources import editor_path


def _find_lumen_lsp() -> str:
    """Абсолютный путь к lumen-lsp.exe (C++-сервер Lumen).

    Ищем рядом с редактором: сначала в корне проекта Lumen, затем в
    папке editor/, затем в PATH. Пустая строка -> сервер недоступен
    (LSP для lumen отключается ядром).
    """
    candidates = [
        editor_path("..", "lumen-lsp.exe"),
        editor_path("lumen-lsp.exe"),
        editor_path("..", "lsp", "lumen-lsp.exe"),
    ]
    for c in candidates:
        if os.path.isfile(str(c)):
            return str(c)
    return shutil.which("lumen-lsp.exe") or shutil.which("lumen-lsp") or ""


def lumen_def() -> LanguageDef:
    """Описание языка Lumen для агрегатора editor/lexers/registry.py."""
    return LanguageDef(
        id="lumenlang",
        extensions=[".lm"],
        custom_lexer=LumenLangLexer,
        lsp=LspServerConfig(
            language_id="lumenlang",
            server_name="lumen-lsp",
            command=[_find_lumen_lsp() or "lsp_server", "--stdio"],
            error_lens=True,
            hover=True,
            inlay_hints=True,
            semantic_tokens=True,
            line_comment="//",
            block_comments=True,
            initial_options={
                "lumen": {
                    "checkOnSave": {"enable": True},
                    "deepAnalysis": {"enable": True},
                    "diagnostics": {"enable": True},
                    "inlayHints": {"enable": True},
                },
            },
        ),
    )
