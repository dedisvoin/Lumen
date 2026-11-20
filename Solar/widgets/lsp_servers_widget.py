"""
LspServersWidget — вкладка со списком LSP-серверов и их логами.

Показывает все сконфигурированные/запущенные LSP-серверы, их состояние
и даёт возможность посмотреть логи stderr.
"""

from typing import Optional

from PyQt6.QtCore import Qt, QTimer, QRectF, QSize, pyqtSignal
from PyQt6.QtGui import QColor, QFont, QFontMetrics, QPainter
from PyQt6.QtWidgets import (
    QApplication, QHBoxLayout, QLabel, QListWidget, QListWidgetItem,
    QPushButton, QSplitter, QTextEdit, QToolButton, QVBoxLayout, QWidget,
)

from Solar.font_settings import code_font_family
from Solar.lsp.manager import get_lsp_manager
from Solar.scrollbar_qss import scrollbar_qss
from Solar.theme_utils import ThemeColors
from Solar.widgets.animated_hover import HoverFade


class _HoverButton(QToolButton):
    _HOVER_INSET = 0
    _HOVER_RADIUS = 8

    def __init__(self, parent=None):
        super().__init__(parent)
        self._hover_color = QColor(255, 255, 255, 20)
        self._fade = HoverFade(self)
        self.setAttribute(Qt.WidgetAttribute.WA_Hover)
        self.setStyleSheet("QToolButton { border: none; background: transparent; }")

    def set_hover_color(self, color: QColor):
        self._hover_color = QColor(color)
        self.update()

    def enterEvent(self, event):
        self._fade.fade_in()
        super().enterEvent(event)

    def leaveEvent(self, event):
        self._fade.fade_out()
        super().leaveEvent(event)

    def paintEvent(self, event):
        c = QColor(self._hover_color)
        c.setAlpha(round(c.alpha() * self._fade.progress()))
        if c.alpha() > 0:
            p = QPainter(self)
            p.setRenderHint(QPainter.RenderHint.Antialiasing)
            rect = QRectF(self.rect()).adjusted(
                self._HOVER_INSET, self._HOVER_INSET,
                -self._HOVER_INSET, -self._HOVER_INSET,
            )
            p.setPen(Qt.PenStyle.NoPen)
            p.setBrush(c)
            p.drawRoundedRect(rect, self._HOVER_RADIUS, self._HOVER_RADIUS)
            p.end()
        super().paintEvent(event)


class LspServersWidget(QWidget):
    def __init__(self, tile_manager=None, theme_manager=None):
        super().__init__()
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self._tm = tile_manager
        self._theme_manager = theme_manager
        self._theme_name = ""
        self._fg = "#cdd6f4"
        self._muted = "#6c7086"
        self._accent = "#89b4fa"
        self._ok = "#a6e3a1"
        self._warn = "#f9e2af"
        self._err = "#f38ba8"

        self._title = QLabel("LSP Servers")
        self._title.setFont(QFont(code_font_family(), 10))
        self._refresh = _HoverButton(self)
        self._refresh.setText("Refresh")
        self._refresh.setToolTip("Refresh list")
        self._refresh.clicked.connect(self.refresh)

        header = QHBoxLayout()
        header.addWidget(self._title)
        header.addStretch(1)
        header.addWidget(self._refresh)

        self._list = QListWidget()
        self._list.setFont(QFont(code_font_family(), 9))
        self._list.currentItemChanged.connect(self._on_select)

        self._log = QTextEdit()
        self._log.setReadOnly(True)
        self._log.setFont(QFont(code_font_family(), 9))
        self._log.setLineWrapMode(QTextEdit.LineWrapMode.NoWrap)

        self._split = QSplitter(Qt.Orientation.Vertical)
        self._split.addWidget(self._list)
        self._split.addWidget(self._log)
        self._split.setSizes([200, 400])

        layout = QVBoxLayout(self)
        layout.setContentsMargins(8, 6, 8, 8)
        layout.setSpacing(6)
        layout.addLayout(header)
        layout.addWidget(self._split, 1)

        self._timer = QTimer(self)
        self._timer.setInterval(1000)
        self._timer.timeout.connect(self.refresh)
        self._timer.start()

        if self._theme_manager is not None:
            try:
                tn = getattr(self._theme_manager, 'current_theme_name', None)
                if callable(tn):
                    self.apply_theme(tn())
                else:
                    tn2 = getattr(self._theme_manager, 'theme_name', '')
                    if callable(tn2):
                        self.apply_theme(tn2())
                    elif isinstance(tn2, str):
                        self.apply_theme(tn2)
            except Exception:
                pass

        self.refresh()

    def apply_theme(self, theme_name: str):
        self._theme_name = theme_name
        tc = ThemeColors.from_theme_manager(self._theme_manager, theme_name)
        self._fg = tc.fg
        self._muted = tc.comment if tc.comment else tc.fg
        self._accent = tc.accent if tc.accent else tc.fg
        hover = QColor(self._fg)
        hover.setAlpha(20)
        self._refresh.set_hover_color(hover)
        self.setStyleSheet(f"QWidget {{ background: {tc.bg}; color: {self._fg}; }}")
        self._list.setStyleSheet(
            f"QListWidget {{ background: {tc.bg}; color: {self._fg}; border: none; }}"
            + scrollbar_qss(tc)
        )
        self._log.setStyleSheet(
            f"QTextEdit {{ background: {tc.bg}; color: {self._fg}; border: none; }}"
            + scrollbar_qss(tc)
        )
        self._title.setStyleSheet(f"color: {self._fg};")
        self.refresh()

    def showEvent(self, event):
        super().showEvent(event)
        if not self._timer.isActive():
            self._timer.start()
        self.refresh()

    def hideEvent(self, event):
        super().hideEvent(event)
        self._timer.stop()

    def refresh(self):
        self._list.clear()
        try:
            manager = get_lsp_manager()
            statuses = manager.lsp_statuses()
        except Exception:
            statuses = []
        if not statuses:
            item = QListWidgetItem("No LSP servers running/configured")
            item.setForeground(QColor(self._muted))
            self._list.addItem(item)
            self._log.clear()
            return
        for st in statuses:
            server = st.get("server") or "?"
            lang = st.get("language_id") or "?"
            state = st.get("state") or "?"
            err = st.get("error") or ""
            text = f"{server}  •  {lang}  •  {state}"
            if err and state == "error":
                text += "  (error)"
            item = QListWidgetItem(text)
            if state == "running":
                item.setForeground(QColor(self._ok))
            elif state == "starting":
                item.setForeground(QColor(self._warn))
            elif state == "error":
                item.setForeground(QColor(self._err))
            else:
                item.setForeground(QColor(self._muted))
            item.setData(Qt.ItemDataRole.UserRole, st)
            self._list.addItem(item)
        if self._list.count() > 0 and self._list.currentRow() < 0:
            self._list.setCurrentRow(0)
        self._on_select(self._list.currentItem(), None)

    def _on_select(self, item: Optional[QListWidgetItem], _prev):
        if item is None:
            self._log.clear()
            return
        st = item.data(Qt.ItemDataRole.UserRole)
        if not isinstance(st, dict):
            self._log.clear()
            return
        try:
            manager = get_lsp_manager()
            session = None
            for key, s in sorted(manager._sessions.items()):
                if key[0] == (st.get("language_id") or ""):
                    session = s
                    break
            text = ""
            if session is not None:
                text = session.stderr_text() or ""
            if not text:
                text = st.get("error") or "No logs available"
            self._log.setPlainText(text)
        except Exception as e:
            self._log.setPlainText(str(e))
