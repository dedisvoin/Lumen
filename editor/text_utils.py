"""
Общие константы и утилиты (единый источник правды).

Словарь C++ токенов, числовой regex и rgb-упаковка раньше копировались
в config/semantic_lexer/hover_popup/bridge и потихоньку расходились —
теперь живут в одном месте.
"""

# C++ ключевые слова и базовые типы: fallback семантического лексера
# и словарь подсветки сигнатур (C/Python-строки).
CPP_KEYWORDS = ("alignas", "alignof", "and", "and_eq", "asm", "bitand",
                "bitor", "break", "case", "catch", "class", "co_await",
                "co_return", "co_yield", "compl", "concept", "const",
                "consteval", "constexpr", "constinit", "const_cast",
                "continue", "decltype", "default", "delete", "do",
                "dynamic_cast", "else", "enum", "explicit", "export",
                "extern", "false", "for", "friend", "goto", "if", "inline",
                "mutable", "namespace", "new", "noexcept", "not", "not_eq",
                "nullptr", "operator", "or", "or_eq", "private", "protected",
                "public", "register", "reinterpret_cast", "requires",
                "return", "sizeof", "static", "static_assert", "static_cast",
                "struct", "switch", "template", "this", "thread_local",
                "throw", "true", "try", "typedef", "typeid", "typename",
                "union", "using", "virtual", "volatile", "while", "xor",
                "xor_eq")

CPP_TYPES = ("auto", "bool", "char", "char16_t", "char32_t", "char8_t",
             "double", "float", "int", "long", "short", "signed",
             "unsigned", "void", "wchar_t")

# Числовой литерал C-подобных языков: 0x/0b/float, суффиксы f/u/l и
# разделители-апострофы (как в SemanticLexer).
NUMBER_PATTERN = (r"(?:0[xX][0-9a-fA-F']+|0[bB][01']+"
                  r"|[0-9][0-9']*(?:\.[0-9']*)?(?:[eE][+-]?[0-9']+)?[fFuUlL]*"
                  r"|\.[0-9][0-9']*(?:[eE][+-]?[0-9']+)?[fFuUlL]*)")


def rgb_int(color) -> int:
    """QColor -> целое Scintilla (порядок BBGGRR)."""
    return (color.blue() << 16) | (color.green() << 8) | color.red()
