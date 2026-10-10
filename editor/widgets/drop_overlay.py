"""
DropOverlay — прозрачный оверлей для отображения зоны перетаскивания.
"""

from typing import Optional

from PyQt6.QtCore import Qt
from PyQt6.QtGui import QPainter, QColor, QBrush, QPen, QPaintEvent
from PyQt6.QtWidgets import QWidget


class DropOverlay(QWidget):
    """Прозрачный оверлей, рисующий индикатор зоны сброса."""

    def __init__(self, parent=None):
        super().__init__(parent)
        self.setAttribute(Qt.WidgetAttribute.WA_TransparentForMouseEvents, True)
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, False)
        self.setStyleSheet("background: transparent;")
        self.drop_indicator: Optional[str] = None
        self._fill = QColor(255, 255, 255, 80)
        self.hide()

    def set_fg_color(self, fg: QColor):
        """Цвет заливки зоны дропа — из темы, чтобы оверлей был виден
        и на тёмном, и на светлом фоне."""
        self._fill = QColor(fg)
        self._fill.setAlpha(80)
        self.update()

    def set_indicator(self, zone: Optional[str]):
        self.drop_indicator = zone
        if zone:
            self.show()
            self.raise_()
            self.update()
        else:
            self.hide()

    def paintEvent(self, event: QPaintEvent):
        if not self.drop_indicator:
            return

        painter = QPainter(self)
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        w, h = self.width(), self.height()

        painter.setBrush(QBrush(self._fill))
        painter.setPen(Qt.PenStyle.NoPen)

        if self.drop_indicator == "center":
            painter.drawRect(0, 0, w, h)
        elif self.drop_indicator == "left":
            painter.drawRect(0, 0, w // 2, h)
        elif self.drop_indicator == "right":
            painter.drawRect(w // 2, 0, w // 2, h)
        elif self.drop_indicator == "top":
            painter.drawRect(0, 0, w, h // 2)
        elif self.drop_indicator == "bottom":
            painter.drawRect(0, h // 2, w, h // 2)

        painter.end()
