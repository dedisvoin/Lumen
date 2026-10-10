"""
LexerManager — определяет язык файла по расширению и создаёт лексеры.

Данные (маппинг расширений и классы лексеров) берутся из агрегатора
editor/lexers/registry, который собирает их из пер-языковых описаний
editor/lexers/<lang>/. Здесь осталась только логика, связанная с редактором
(тема, фолдинг), плюс register_custom_lexer() — рантайм-хук для плагинов
(перекрывает встроенные языки из registry).
"""

from pathlib import Path
from typing import Optional, Tuple, Type

from PyQt6.Qsci import QsciLexerCustom, QsciScintilla

from editor.theme_manager import ThemeManager
from editor.lexers.registry import (
    language_for_ext, lexer_spec, register_lsp_server,
)
from editor.lexers.lsp_types import LspServerConfig


class LexerManager:
    """Управляет созданием лексеров и маппингом расширений."""

    def __init__(self, theme_manager: ThemeManager):
        self.theme_manager = theme_manager
        # Рантайм-переопределения (register_custom_lexer): language_id -> spec
        self._custom_lexers: dict = {}
        # Рантайм-расширения: ext(с точкой) -> language_id
        self._custom_exts: dict = {}

    def register_custom_lexer(self, ext: str, language_id: str,
                              lexer_class: Type, lsp_config=None):
        """Рантайм-регистрация языка (для тестов и плагинов).

        Перекрывает встроенный язык из registry. lsp_config (LspServerConfig)
        необязателен — если задан, регистрируется в LSP-агрегаторе тоже.
        """
        kind = "custom" if isinstance(lexer_class, type) and issubclass(
            lexer_class, QsciLexerCustom) else "builtin"
        self._custom_lexers[language_id] = (kind, lexer_class)
        self._custom_exts[ext.lower()] = language_id
        if lsp_config is not None:
            register_lsp_server(language_id, lsp_config)

    def get_language_id(self, file_path: str) -> Optional[str]:
        ext = Path(file_path).suffix.lower()
        return self._custom_exts.get(ext) or language_for_ext(ext)

    def get_lexer(self, language_id: str,
                  parent: QsciScintilla) -> Optional[Type]:
        spec = self._custom_lexers.get(language_id) or lexer_spec(language_id)
        if spec is None:
            return None
        kind, lexer_class = spec
        if kind == "builtin":
            # Включаем фолдинг для лексера — иначе fold levels не вычисляются
            # и маркеры сворачивания отображаются на неправильных строках.
            lexer = lexer_class(parent)
            lexer.setProperty("fold", "1")
            if hasattr(lexer, 'setFoldCompact'):
                lexer.setFoldCompact(False)
            return lexer
        # Кастомные лексеры — создаём сразу с текущей темой, чтобы они
        # были раскрашены ещё до первого apply_theme.
        theme_data = self.theme_manager.themes.get(
            self.theme_manager.current_theme, {})
        return lexer_class(parent, theme_data)
