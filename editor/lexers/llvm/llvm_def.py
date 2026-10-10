"""
LLVM — описание языка: кастомный лексер (LLVMLexer).

Для LLVM IR нет LSP-сервера, поэтому lsp=None. Сам лексер — в
llvm_lexer.py рядом.
"""

from editor.lexers.language_def import LanguageDef
from editor.lexers.llvm.llvm_lexer import LLVMLexer


def llvm_def() -> LanguageDef:
    """Описание языка LLVM для агрегатора editor/lexers/registry.py."""
    return LanguageDef(
        id="llvm",
        extensions=[".ll"],
        custom_lexer=LLVMLexer,
    )
