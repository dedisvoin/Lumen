"""
SignaturePopup — всплывающая подпись сигнатуры при вводе '('.

Показывает label активной сигнатуры (активный параметр подсвечен),
документацию ниже и счётчик перегрузок. Позиционируется над строкой
каретки, цвета — из темы редактора.
"""

import html
from typing import List, Optional

from PyQt6.QtCore import QPoint, QRectF, Qt
from PyQt6.QtGui import (
    QColor, QFont, QPainter, QPainterPath, QPen,
)
from PyQt6.QtWidgets import (
    QApplication, QFrame, QLabel, QVBoxLayout,
)

from editor.constants import UI_FONT_SIZE
from editor.scintilla_constants import (
    SCI_GETCURRENTPOS, SCI_POINTXFROMPOSITION, SCI_POINTYFROMPOSITION,
    SCI_TEXTHEIGHT,
)
from editor.theme_utils import ThemeColors
from editor.widgets.error_lens import ErrorLensOverlay

_MAX_WIDTH = 720          # максимальная ширина, px
_PAD_X, _PAD_Y = 12, 8    # внутренние отступы
_OFFSET_Y = 6             # отступ попапа от строки каретки, px


class SignaturePopup(QFrame):
    """Плашка сигнатуры функции: label с подсветкой активного параметра."""

    def __init__(self, editor):
        super().__init__(editor)
        self.editor = editor
        self.setObjectName("SignaturePopup")
        self.setWindowFlags(
            Qt.WindowType.ToolTip
            | Qt.WindowType.FramelessWindowHint
            | Qt.WindowType.WindowDoesNotAcceptFocus
        )
        self.setAttribute(Qt.WidgetAttribute.WA_ShowWithoutActivating, True)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, True)
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self.setMaximumWidth(_MAX_WIDTH)

        self._tc: Optional[ThemeColors] = None
        self._signatures: List[dict] = []
        self._active = 0
        self._param = 0
        self._label = QLabel()
        self._label.setTextFormat(Qt.TextFormat.RichText)
        self._label.setWordWrap(False)
        self._label.setContentsMargins(_PAD_X, _PAD_Y, _PAD_X, 0)
        self._doc = QLabel()
        self._doc.setWordWrap(True)
        self._doc.setContentsMargins(_PAD_X, 4, _PAD_X, _PAD_Y)
        lay = QVBoxLayout(self)
        lay.setContentsMargins(0, 0, 0, 0)
        lay.setSpacing(0)
        lay.addWidget(self._label)
        lay.addWidget(self._doc)
        self._doc.hide()

    # --- тема ---

    def paintEvent(self, event):
        """Фон-плашка и рамка (QSS на прозрачном окне не рисуется)."""
        p = QPainter(self)
        p.setRenderHint(QPainter.RenderHint.Antialiasing)
        if self._tc is None:
            bg, border = QColor("#1e1e2e"), QColor("#45475a")
        else:
            bg = self._tc.shade(105)
            border = QColor(self._tc.border)
        path = QPainterPath()
        path.addRoundedRect(
            QRectF(self.rect()).adjusted(0.5, 0.5, -0.5, -0.5), 6, 6
        )
        p.fillPath(path, bg)
        p.setPen(QPen(border, 1))
        p.drawPath(path)

    def apply_theme(self, theme_name: str):
        """Цвета попапа из темы редактора."""
        self._tc = ThemeColors.from_theme_manager(
            self.editor.theme_manager, theme_name
        )
        f = QFont(self.font())
        f.setPointSize(UI_FONT_SIZE)
        self._label.setFont(f)
        self._doc.setFont(f)
        self._paint()

    # --- содержимое ---

    def set_signatures(self, signatures: List[dict], active: int, param: int):
        """Обновляет содержимое: активная сигнатура + подсветка параметра."""
        self._active = max(0, min(active, len(signatures) - 1))
        self._param = param
        self._signatures = signatures
        self._paint()

    def _paint(self):
        """Перерисовывает label/doc по текущей сигнатуре."""
        tc = self._tc
        if tc is None:
            return
        sigs = getattr(self, "_signatures", None) or []
        sig = sigs[self._active] if sigs else {}
        label = sig.get("label") or ""
        html_label = self._render_label(label, sig.get("params") or [],
                                        self._param, tc)
        if len(sigs) > 1:
            html_label += (f"<span style='color:{tc.muted}'>&nbsp;&nbsp;"
                           f"{self._active + 1}/{len(sigs)}</span>")
        self._label.setText(html_label)
        doc = sig.get("doc") or ""
        if doc:
            self._doc.setText(doc)
            self._doc.show()
            self._label.setContentsMargins(_PAD_X, _PAD_Y, _PAD_X, 0)
        else:
            self._doc.hide()
            # Одна строка: нижний отступ как верхний, чтобы подпись
            # не прилипала к нижней кромке плашки
            self._label.setContentsMargins(_PAD_X, _PAD_Y, _PAD_X, _PAD_Y)
        self.adjustSize()

    def _render_label(self, label: str, params, param_idx: int, tc) -> str:
        """label сигнатуры в HTML: приглушённый текст, активный параметр — fg."""
        if not params:
            return (f"<span style='color:{tc.muted}'>"
                    + html.escape(label) + "</span>")
        hl = tc.selection or tc.muted
        parts = []
        last = 0
        for i, (_name, s, e) in sorted(
                enumerate(params), key=lambda p: p[1][1]):
            if s < 0 or e < s or s >= len(label):
                continue
            e = min(e, len(label))
            if s > last:
                parts.append(html.escape(label[last:s]))
            seg = html.escape(label[s:e])
            if i == param_idx:
                parts.append(f"<b><span style='color:{tc.fg};"
                             f"background-color:{hl}'>{seg}</span></b>")
            else:
                parts.append(seg)
            last = max(last, e)
        if last < len(label):
            parts.append(html.escape(label[last:]))
        return (f"<span style='color:{tc.muted}'>"
                + "".join(parts) + "</span>")

    # --- показ ---

    def show_at_caret(self, editor):
        """Показывает попап над строкой каретки, не выходя за экран."""
        self.adjustSize()
        try:
            pos = editor.SendScintilla(SCI_GETCURRENTPOS)
            x = editor.SendScintilla(SCI_POINTXFROMPOSITION, 0, pos)
            y = editor.SendScintilla(SCI_POINTYFROMPOSITION, 0, pos)
            margin = ErrorLensOverlay.margin_x(editor)
            line_h = editor.SendScintilla(SCI_TEXTHEIGHT, 0)
        except RuntimeError:
            return
        pt = editor.mapToGlobal(QPoint(int(x) + margin, int(y)))
        screen = QApplication.primaryScreen().availableGeometry()
        # Над строкой; если не влезает — под строкой
        gy = pt.y() - self.height() - _OFFSET_Y
        if gy < screen.top():
            gy = pt.y() + line_h + _OFFSET_Y
        gx = min(max(pt.x(), screen.left()), screen.right() - self.width())
        self.move(gx, gy)
        self.show()
        self.raise_()
