"""
LanguageDef — описание одного языка для ядра редактора.

Каждый язык живёт в своей папке editor/lexers/<lang>/ и возвращет один
LanguageDef. Агрегатор (editor/lexers/registry.py) собирает их в реестры
ext_map (расширение -> language_id) и LSP-конфигов. Ядру
(LexerManager, lsp.manager/bridge) не нужно знать про конкретные языки —
только строку language_id.
"""

from dataclasses import dataclass, field
from typing import Callable, Dict, List, Optional, Type

from editor.lexers.lsp_types import LspServerConfig


@dataclass
class LanguageDef:
    # language_id, используемый везде в ядре (cpp, python, lisp, lumenlang...)
    id: str
    # расширения файлов, которые относятся к этому языку
    extensions: List[str]
    # встроенный QsciLexer* (cpp/python/java/...) — либо custom_lexer
    builtin_lexer: Optional[Type] = None
    # кастомный QsciLexerCustom (lumen/lisp) — либо builtin_lexer
    custom_lexer: Optional[Type] = None
    # конфиг LSP-сервера или None, если для языка LSP не подключён
    lsp: Optional[LspServerConfig] = None
    # хуки в UI настроек: имя -> функция, возвращающая варианты (combo)
    settings_hooks: Dict[str, Callable] = field(default_factory=dict)
