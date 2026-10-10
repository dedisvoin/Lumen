"""
Lisp — описание языка: кастомный лексер (LispLexer) + LSP (cl-lsp).

Пер-языковая логика (Roswell-обёртка cl-lsp, ключевые слова) вынесена
сюда из старого editor/lsp/config.py. Сам лексер — в lisp_lang.py рядом.
"""

from typing import Optional, Tuple

from editor.lexers.language_def import LanguageDef
from editor.lexers.lisp.lisp_lang import LispLexer
from editor.lexers.lsp_types import LspServerConfig


# Fallback-ключевые слова для Lisp (лексер LispLexer раскрашивает сам;
# keywords не используются, т.к. semantic_tokens=False)
_LISP_KEYWORDS = (
    "defun", "defmacro", "defgeneric", "defmethod", "defclass", "defstruct",
    "defpackage", "in-package", "defvar", "defparameter", "defconstant",
    "lambda", "let", "let*", "flet", "labels", "macrolet", "if", "cond",
    "case", "when", "unless", "and", "or", "not", "quote", "quasiquote",
    "function", "setq", "setf", "progn", "loop", "do", "do*", "dolist",
    "dotimes", "return", "return-from", "block", "catch", "throw",
    "unwind-protect", "multiple-value-bind", "eval-when", "the", "declare",
    "handler-case", "handler-bind", "ignore-errors", "assert", "check-type",
    "go", "tagbody", "incf", "decf", "push", "pop", "pushnew", "car", "cdr",
    "cons", "list", "append", "reverse", "mapcar", "apply", "funcall",
    "format", "print", "prin1", "princ", "read", "read-line", "load",
    "compile", "compile-file", "eval", "error", "warn", "signal",
    "make-instance", "slot-value", "values", "nth-value",
)


def _find_cl_lsp() -> Optional[list]:
    """Команда запуска cl-lsp (Common Lisp Language Server).

    На POSIX достаточно имени в PATH. На Windows `cl-lsp` — Roswell-скрипт
    без .exe/.bat, Python его не запустит: оборачиваем вызов через
    `ros -Q -- <скрипт>` (ros.exe в PATH). Первый запуск долгий (SBCL +
    quickload), запросы в Lumen стоят в очереди до готовности.
    """
    import os
    import shutil
    from pathlib import Path

    exe = shutil.which("cl-lsp")
    if exe:
        return [exe]
    home = Path.home()
    for script in (home / ".roswell" / "bin" / "cl-lsp",
                   home / ".roswell" / "lisp" / "quicklisp" / "bin" / "cl-lsp"):
        if script.is_file():
            return ["ros", "-Q", "--", str(script)]
    return None


def lisp_def() -> LanguageDef:
    """Описание языка Lisp для агрегатора editor/lexers/registry.py."""
    return LanguageDef(
        id="lisp",
        extensions=[".lisp", ".lsp", ".cl", ".el", ".scm", ".ss", ".rkt"],
        custom_lexer=LispLexer,
        lsp=LspServerConfig(
            language_id="lisp",
            server_name="cl-lsp",
            command=_find_cl_lsp() or ["cl-lsp"],
            root_markers=(".asd",),
            trigger_chars={":"},
            semantic_tokens=False,
            inlay_hints=False,
            hover=True,
            keywords=_LISP_KEYWORDS,
            line_comment=";",
            block_comments=False,
        ),
    )
