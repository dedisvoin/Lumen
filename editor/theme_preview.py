"""
ThemePreview — настоящее превью темы: read-only редактор кода на QScintilla.

Текст менять нельзя (setReadOnly + NoFocus + без drop), но подсветка,
номера строк, fold-маржин, caret-line, выделение и индикаторы ошибок
рисуются реальным редактором на палитре выбранной темы. Обновляется
через set_theme(name).

Структура как у попапа автодополнения (CompletionPopup): внешний QFrame
с рамкой и скруглением, нарисованными честно через QSS по всему окну,
а внутри — редактор с отступом _MARGIN. Содержимое никогда не заходит
в зону скругления, поэтому никакая маска не нужна и углы плавные.
"""

from typing import Optional

from PyQt6.QtCore import Qt
from PyQt6.QtGui import QColor, QFont, QFontMetrics
from PyQt6.QtWidgets import QFrame, QVBoxLayout
from PyQt6.Qsci import QsciScintilla

from editor.constants import (
    CODE_FONT_SIZE, DEFAULT_TAB_WIDTH, FOLD_ICON_SIZE,
)
from editor.font_settings import code_font_family
from editor.lexer_manager import LexerManager
from editor.scintilla_constants import (
    SCI_SETHSCROLLBAR, SCI_SETVSCROLLBAR, SCI_TEXTHEIGHT,
    SCI_SETXOFFSET, SCI_GOTOPOS,
    SCI_INDICSETSTYLE, SCI_INDICSETFORE, SCI_INDICSETUNDER,
    SCI_SETINDICATORCURRENT, SCI_INDICATORFILLRANGE,
    SCI_INDICATORCLEARRANGE,
    INDIC_SQUIGGLE,
)
from editor.theme_manager import ThemeManager
from editor.theme_utils import ThemeColors
from editor.text_utils import rgb_int

# Размер шрифта превью: настоящий CODE_FONT_SIZE (20) слишком крупный
_PREVIEW_FONT_SIZE = 10

_PREVIEW_SOURCE = """# Theme preview
import os
import re

class Config:
    \"\"\"Editor colors, syntax, errors.\"\"\"
    def __init__(self, path):
        self.path = path
        self._cache = {}

    @staticmethod
    def _read(path: str) -> str:
        with open(path, "r") as f:
            return f.read().strip()

    def load(self, path):
        ok = True
        data = self._read(path)  # cached
        print(f"loaded {len(data)} bytes")
        return data if ok else []
"""

# Номера индикаторов для волн ошибок/предупреждений (standalone, не конфликтуют)
_ERR_IND = 5
_WARN_IND = 6


class _PreviewScintilla(QsciScintilla):
    """QsciScintilla без зума: превью всегда должно показывать шрифт
    темы как есть, поэтому Ctrl+колесо, пич-жест тачпада и программный
    zoom (zoomIn/zoomOut/zoomTo) отключены."""

    def wheelEvent(self, event):
        if event.modifiers() & Qt.KeyboardModifier.ControlModifier:
            event.ignore()
            return
        super().wheelEvent(event)

    def zoomIn(self, *_):
        pass

    def zoomOut(self, *_):
        pass

    def zoomTo(self, *_):
        pass


class ThemePreview(QFrame):
    """Превью темы как попап автодополнения: внешний QFrame с рамкой и
    скруглением через QSS, внутри — настоящий read-only редактор."""

    _MARGIN = 4      # отступ содержимого от рамки (зона скругления)
    _RADIUS = 8      # радиус скругления рамки

    def __init__(self, theme_manager: ThemeManager, parent=None):
        super().__init__(parent)
        self._theme_manager = theme_manager
        self._lexer_manager = LexerManager(theme_manager)
        self._current_theme: Optional[str] = None

        # Рамку и скругление рисует QSS по всему окну (как у попапа)
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        self.setFrameShape(QFrame.Shape.NoFrame)

        ed = _PreviewScintilla(self)
        ed.setUtf8(True)
        ed.setEolMode(QsciScintilla.EolMode.EolUnix)
        ed.setTabWidth(DEFAULT_TAB_WIDTH)
        ed.setIndentationsUseTabs(False)
        ed.setIndentationGuides(True)
        ed.setWhitespaceVisibility(
            QsciScintilla.WhitespaceVisibility.WsVisible)
        ed.setWhitespaceSize(2)
        ed.setAutoIndent(False)

        # Read-only: текст менять нельзя, фокус не забираем
        ed.setReadOnly(True)
        ed.setCaretWidth(0)
        ed.setCaretLineVisible(True)
        ed.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        ed.setAcceptDrops(False)
        ed.setFrameShape(QsciScintilla.Shape.NoFrame)

        # Маржины как в настоящем редакторе: 2 — номера, 3 — fold
        ed.setMarginType(2, QsciScintilla.MarginType.NumberMargin)
        ed.setMarginLineNumbers(2, True)
        ed.setFolding(QsciScintilla.FoldStyle.CircledTreeFoldStyle, 3)
        ed.setMarginWidth(3, FOLD_ICON_SIZE)

        font = QFont(code_font_family(), _PREVIEW_FONT_SIZE)
        font.setFixedPitch(True)
        ed.setFont(font)
        self._margin_font = QFont(font)
        self._margin_font.setFixedPitch(True)
        ed.setMarginsFont(self._margin_font)
        self.editor = ed
        self.update_margin_width()

        # Скролл не нужен: весь образец виден сразу
        ed.SendScintilla(SCI_SETVSCROLLBAR, 0)
        ed.SendScintilla(SCI_SETHSCROLLBAR, 0)
        ed.setVerticalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        ed.setHorizontalScrollBarPolicy(Qt.ScrollBarPolicy.ScrollBarAlwaysOff)

        ed.setText(_PREVIEW_SOURCE)
        self._setup_indicator(ed, _ERR_IND)
        self._setup_indicator(ed, _WARN_IND)
        # Код сразу промотан влево до конца
        ed.SendScintilla(SCI_SETXOFFSET, 0)
        ed.SendScintilla(SCI_GOTOPOS, 0)

        rows = len(_PREVIEW_SOURCE.splitlines())
        row_h = QFontMetrics(font).height() + 4
        ed.setFixedHeight(rows * row_h + 6)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(self._MARGIN, self._MARGIN,
                                  self._MARGIN, self._MARGIN)
        layout.setSpacing(0)
        layout.addWidget(ed)

    # ------------------------------------------------------------------
    # Тема
    # ------------------------------------------------------------------
    def set_theme(self, theme_name: str):
        """Применяет тему как к настоящему редактору (лексер + маржины +
        маркеры фолдинга) и рисует демо-индикаторы ошибок."""
        manager = self._theme_manager
        if not hasattr(manager, "apply_theme"):
            return
        theme = manager.themes.get(theme_name)
        if not theme:
            return
        self._current_theme = theme_name

        ed = self.editor
        lexer = self._lexer_manager.get_lexer("python", ed)
        ed.setLexer(lexer)
        manager.apply_theme(ed, theme_name, lexer)

        tc = ThemeColors.from_theme_manager(manager, theme_name)
        line_height = ed.SendScintilla(SCI_TEXTHEIGHT, 0)
        manager.apply_fold_icons(
            ed, tc.editor_bg_color, tc.fg_color, line_height)

        # Демо: caret-line и выделение
        colors = theme.get("colors", {})
        caret_line = colors.get("caret_line")
        if caret_line:
            caret_color = QColor(caret_line)
        else:
            caret_color = (tc.editor_bg_color.lighter(105) if tc.dark
                           else tc.editor_bg_color.darker(105))
        ed.setCaretLineBackgroundColor(caret_color)
        ed.setCaretLineVisible(True)
        ed.setCursorPosition(16, 0)
        ed.setSelection(15, 8, 15, 12)  # слово "load"
        ed.setSelectionBackgroundColor(
            QColor(colors.get("selection", "#264f78")))

        # Демо-индикаторы: ошибка под "data", предупреждение под "ok"
        self._draw_indicators(ed, colors)

        # Код промотан влево до конца (горизонтальный сдвиг нулевой)
        ed.SendScintilla(SCI_SETXOFFSET, 0)
        ed.SendScintilla(SCI_GOTOPOS, 0)

        # Рамка и фон как у попапа автодополнения: честный QSS по всему
        # окну, без маски. Фон рамки — тот же цвет, что и бумага редактора,
        # поэтому квадратные углы редактора (внутри отступа _MARGIN) не
        # видны, а рамка со скруглением — плавная.
        bg_hex = colors.get("background", "#1e1e2e")
        border = QColor(tc.border)
        self.setStyleSheet(
            f"ThemePreview {{"
            f"  background-color: {bg_hex};"
            f"  border: 0.5px solid {border.name()};"
            f"  border-radius: {self._RADIUS}px;"
            f"}}")

    # ------------------------------------------------------------------
    # Внутреннее
    # ------------------------------------------------------------------
    def update_margin_width(self):
        fm = QFontMetrics(self._margin_font)
        self.editor.setMarginWidth(2, fm.horizontalAdvance("9999") + 6)

    @staticmethod
    def _setup_indicator(editor, number: int):
        editor.SendScintilla(SCI_INDICSETSTYLE, number, INDIC_SQUIGGLE)
        editor.SendScintilla(SCI_INDICSETUNDER, number, True)

    def _pos(self, line: int, col: int) -> int:
        return self.editor.positionFromLineIndex(line, col)

    def _draw_indicators(self, editor, colors: dict):
        error = colors.get("error", "#f14c4c")
        warning = colors.get("warning", "#d19a66")

        editor.SendScintilla(SCI_SETINDICATORCURRENT, _ERR_IND)
        editor.SendScintilla(SCI_INDICATORCLEARRANGE, 0, editor.length())
        editor.SendScintilla(SCI_SETINDICATORCURRENT, _WARN_IND)
        editor.SendScintilla(SCI_INDICATORCLEARRANGE, 0, editor.length())
        editor.SendScintilla(SCI_INDICSETFORE, _ERR_IND,
                             rgb_int(QColor(error)))
        editor.SendScintilla(SCI_INDICSETFORE, _WARN_IND,
                             rgb_int(QColor(warning)))

        # "data" на строке 17
        editor.SendScintilla(SCI_SETINDICATORCURRENT, _ERR_IND)
        editor.SendScintilla(SCI_INDICATORFILLRANGE, self._pos(17, 8), 4)
        # "ok" на строке 16
        editor.SendScintilla(SCI_SETINDICATORCURRENT, _WARN_IND)
        editor.SendScintilla(SCI_INDICATORFILLRANGE, self._pos(16, 8), 2)
