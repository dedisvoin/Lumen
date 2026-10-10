"""
Builtin-языки без LSP: только встроенные QsciLexer* и расширения.

Они не имеют собственной LSP-конфигурации, поэтому собраны здесь одним
списком, а не раскиданы по папкам (для них нет пер-языковой логики).
LSP-языки (cpp/python/lisp) и кастомные лексеры (lumen) лежат в своих
папках editor/lexers/<lang>/.
"""

from PyQt6.Qsci import (
    QsciLexerJava, QsciLexerJavaScript, QsciLexerXML, QsciLexerYAML,
    QsciLexerSQL, QsciLexerMarkdown, QsciLexerBash, QsciLexerLua,
    QsciLexerRuby,
)

from editor.lexers.language_def import LanguageDef


def builtin_defs() -> list:
    """LanguageDef для языков без LSP (только подсветка)."""
    return [
        LanguageDef("java", [".java"], builtin_lexer=QsciLexerJava),
        LanguageDef("javascript",
                    [".js", ".jsx", ".mjs"], builtin_lexer=QsciLexerJavaScript),
        LanguageDef("xml",
                    [".xml", ".html", ".htm", ".svg"], builtin_lexer=QsciLexerXML),
        LanguageDef("yaml", [".yaml", ".yml"], builtin_lexer=QsciLexerYAML),
        LanguageDef("sql", [".sql"], builtin_lexer=QsciLexerSQL),
        LanguageDef("markdown",
                    [".md", ".markdown"], builtin_lexer=QsciLexerMarkdown),
        LanguageDef("bash",
                    [".sh", ".bash", ".zsh"], builtin_lexer=QsciLexerBash),
        LanguageDef("lua", [".lua"], builtin_lexer=QsciLexerLua),
        LanguageDef("ruby", [".rb"], builtin_lexer=QsciLexerRuby),
        # C# без выделенного лексера (совместимо со старым поведением)
        LanguageDef("csharp", [".cs"], builtin_lexer=None),
        # JSON без лексера (обычная подсветка без раскраски)
        LanguageDef("json", [".json"], builtin_lexer=None),
    ]
