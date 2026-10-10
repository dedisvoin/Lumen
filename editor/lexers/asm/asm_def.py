"""
Asm — описание языка: встроенный лексер (через подкласс AsmLexer).

LSP для ассемблера нет, поэтому lsp=None. Расширения покрывают MASM/NASM
и GNU-ассемблер (.s/.S).
"""

from editor.lexers.asm.asm_lexer import AsmLexer
from editor.lexers.language_def import LanguageDef


def asm_def() -> LanguageDef:
    """Описание языка Asm для агрегатора editor/lexers/registry.py."""
    return LanguageDef(
        id="asm",
        # Расширения только в нижнем регистре: language_for_ext() и
        # get_language_id() приводят suffix к lower() перед поиском, поэтому
        # вариант ".S" никогда не находился — это был мёртвый ключ EXT_MAP.
        extensions=[".asm", ".nasm", ".s", ".inc"],
        custom_lexer=AsmLexer,
    )
