"""
BaseCustomLexer — базовый класс для кастомных лексеров.
"""

from typing import Optional, Dict

from PyQt6.QtGui import QFont
from PyQt6.Qsci import QsciLexerCustom


class BaseCustomLexer(QsciLexerCustom):
    """Абстрактная основа для пользовательских лексеров с темизацией."""

    def __init__(self, parent=None, theme_data: Optional[dict] = None):
        super().__init__(parent)
        self.theme_data = theme_data or {}
        self._styles: Dict[int, str] = {0: "Default"}
        self._init_styles()
        default_font = QFont("Consolas", 11)
        default_font.setFixedPitch(True)
        self.setDefaultFont(default_font)

    def _init_styles(self):
        pass

    def language(self) -> str:
        return "Custom"

    def description(self, style: int) -> str:
        return self._styles.get(style, f"Style_{style}")

    def set_theme(self, theme_data: dict):
        """Применяет цвета из theme_data['styles'] к стилям."""
        pass

    def styleText(self, start: int, end: int):
        self.startStyling(start)
        self.setStyling(end - start, 0)
