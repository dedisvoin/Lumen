"""
Lisp — определение языка: лексер (LispLexer) + LSP (cl-lsp).
"""

from typing import Optional

from editor.lexers.language_def import LanguageDef
from editor.lexers.lisp.lisp_lang import LispLexer
from editor.lexers.lsp_types import LspServerConfig


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
    """Поиск cl-lsp. Приоритет — локальная папка editor/lsp/bin/lisp."""
    import shutil
    from pathlib import Path

    here = Path(__file__).resolve().parent  # editor/lexers/lisp
    local_bin = here.parent.parent / "lsp" / "bin" / "lisp"  # editor/lsp/bin/lisp

    # 1) Приоритет - локальная обёртка
    wrapper_cmd = local_bin / "cl-lsp.cmd"
    if wrapper_cmd.is_file():
        return [str(wrapper_cmd)]

    wrapper_bat = local_bin / "cl-lsp.bat"
    if wrapper_bat.is_file():
        return [str(wrapper_bat)]

    # 2) Готовый .exe
    for name in ("cl-lsp.exe",):
        cand = local_bin / name
        if cand.is_file():
            return [str(cand)]

    # 3) cl-lsp скрипт - через настоящий roswell
    cl_lsp_script = local_bin / "cl-lsp"
    cl_lsp_ros = local_bin / "cl-lsp.ros"
    ros_real = Path(r"C:\Users\Ivan\scoop\apps\roswell\current\ros.exe")
    ros = ros_real if ros_real.is_file() else (shutil.which("ros") or shutil.which("ros.exe"))

    if cl_lsp_script.is_file() and ros:
        return [str(ros), "-Q", "--", str(cl_lsp_script)]
    if cl_lsp_ros.is_file() and ros:
        return [str(ros), "-Q", "--", str(cl_lsp_ros)]

    # 4) PATH
    exe = shutil.which("cl-lsp")
    if exe:
        return [exe]

    # 5) Roswell из home
    home = Path.home()
    for script in (home / ".roswell" / "bin" / "cl-lsp",
                   home / ".roswell" / "lisp" / "quicklisp" / "bin" / "cl-lsp"):
        if script.is_file():
            if ros:
                return [ros, "-Q", "--", str(script)]
            return ["ros", "-Q", "--", str(script)]

    return ["cl-lsp"]


def lisp_def() -> LanguageDef:
    return LanguageDef(
        id="lisp",
        extensions=[".lisp", ".lsp", ".cl", ".el", ".scm", ".ss", ".rkt"],
        custom_lexer=LispLexer,
        lsp=LspServerConfig(
            language_id="lisp",
            server_name="cl-lsp",
            command=_find_cl_lsp() or ["cl-lsp"],
            root_markers=(".asd",),
            trigger_chars={":", "(", "-", "/"},
            semantic_tokens=False,
            inlay_hints=False,
            hover=True,
            keywords=_LISP_KEYWORDS,
            line_comment=";",
            block_comments=False,
        ),
    )
