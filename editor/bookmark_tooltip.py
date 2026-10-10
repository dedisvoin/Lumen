"""
Кастомная плашка-подсказка для закладки (вместо встроенного QToolTip).

Показывается при наведении на иконку закладки с пояснением. Стиль совпадает
с виджетом ввода закладки (BookmarkInputPopup): тот же фон/бордер/радиус и
наследуемый шрифт. Плашка прозрачна для мыши (WA_TransparentForMouseEvents),
поэтому не перехватывает события редактора — курсор можно продолжать водить
и плашка корректно прячется/обновляется при уходе с закладки.

Тень: у ToolTip-окон нет нативной тени (в отличие от Popup, как у виджета
поиска по файлу), поэтому рисуем QGraphicsDropShadowEffect на вложенной
«карточке», а вокруг неё оставляем прозрачный запас (_shadow_margin), чтобы
тень не обрезалась границами frameless-окна.
"""

from PyQt6.QtCore import Qt
from PyQt6.QtGui import QColor
from PyQt6.QtWidgets import (QFrame, QLabel, QVBoxLayout,
                             QGraphicsDropShadowEffect)

from editor.theme_utils import ThemeColors


class BookmarkTooltip(QFrame):
    """Плашка с пояснением закладки в стиле BookmarkInputPopup."""

    def __init__(self, theme_manager, theme_name, parent=None):
        super().__init__(
            parent,
            Qt.WindowType.ToolTip | Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.NoDropShadowWindowHint,
        )
        # Не перехватываем события мыши — редактор продолжает получать
        # mouseMove и сам прячет/обновляет плашку при уходе курсора.
        # WindowType.ToolTip гарантирует, что плашка не крадёт фокус у редактора.
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        # Запас вокруг карточки, чтобы тень не обрезалась границами окна.
        # Тень расходится примерно на blurRadius/2 в каждую сторону плюс
        # смещение, поэтому запас обязан быть больше этой величины.
        self._shadow_margin = 26
        self.setMaximumWidth(360 + self._shadow_margin * 2)

        tc = ThemeColors.from_theme_manager(theme_manager, theme_name)
        self._theme_name = theme_name
        self._bg = tc.editor_bg_color.name()
        self._fg = tc.fg
        self._border = tc.border

        outer = QVBoxLayout(self)
        outer.setContentsMargins(self._shadow_margin, self._shadow_margin,
                                 self._shadow_margin, self._shadow_margin)
        outer.setSpacing(0)

        self._card = QFrame()
        self._card.setStyleSheet(f"""
            QFrame {{
                background-color: {self._bg};
                border: 0.5px solid {self._border};
                border-radius: 6px;
            }}
            QLabel {{
                background-color: transparent;
                color: {self._fg};
                border: none;
                padding: 6px 8px;
            }}
        """)
        card_layout = QVBoxLayout(self._card)
        card_layout.setContentsMargins(6, 6, 6, 6)
        card_layout.setSpacing(0)
        self._label = QLabel()
        self._label.setWordWrap(True)
        self._label.setTextInteractionFlags(
            Qt.TextInteractionFlag.NoTextInteraction)
        card_layout.addWidget(self._label)

        shadow = QGraphicsDropShadowEffect(self._card)
        shadow.setBlurRadius(32)
        shadow.setOffset(0, 4)
        shadow.setColor(QColor(0, 0, 0, 80))
        self._card.setGraphicsEffect(shadow)

        outer.addWidget(self._card)

    def set_note(self, text: str):
        self._label.setText(text)
        # Смена текста у word-wrap QLabel пересчитывает высоту только после
        # фактической перекладки layout-а. Без принудительной активации окно
        # (и сам label) сохраняют старый размер, и текст обрезается — особенно
        # при быстром переходе с одной закладки на другую.
        self._card.layout().activate()
        self._label.adjustSize()
        self.adjustSize()
