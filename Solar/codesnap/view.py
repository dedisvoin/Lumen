"""
CodeSnap preview view
"""
from PyQt6.QtCore import Qt
from PyQt6.QtGui import QPainter
from PyQt6.QtWidgets import QWidget
from Solar.codesnap.renderer import CodeSnapRenderer
from Solar.theme_utils import ThemeColors


class CodeSnapView(QWidget):
    def __init__(self, data=None, theme_manager=None, theme_name=None, parent=None):
        super().__init__(parent)
        self.data = data
        self.theme_manager = theme_manager
        self.theme_name = theme_name or ""
        self.renderer = CodeSnapRenderer()
        self.settings = {
            'show_line_numbers': True,
            'macos_style': True,
            'rounded': True,
            'shadow': True,
            'use_theme_bg': True,
            'padding': 16,
            'show_filename': True,
        }
        self.setMinimumSize(400, 200)
        self.setAutoFillBackground(False)

    def update_settings(self, s: dict):
        self.settings.update(s)
        self.update()

    def set_theme(self, theme_name):
        if theme_name is not None:
            self.theme_name = theme_name
        self.update()

    def apply_theme(self, theme_name):
        self.set_theme(theme_name)

    def paintEvent(self, event):
        painter = QPainter(self)
        tc = None
        if self.theme_manager and self.theme_name:
            try:
                tc = ThemeColors.from_theme_manager(self.theme_manager, self.theme_name)
            except Exception:
                try:
                    tc = self.theme_manager.get_theme_colors(self.theme_name)
                except Exception:
                    tc = None
        self.renderer.draw(painter, self.rect().adjusted(24, 24, -24, -24), self.data, self.settings, tc)
        painter.end()
