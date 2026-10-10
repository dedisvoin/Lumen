"""
Theme Manager — загрузка тем из JSON-файлов и применение к редактору/лексеру.
"""

import json
from pathlib import Path
from typing import Dict, Optional, List, Any

from PyQt6.QtCore import Qt, QPointF, QRectF
from PyQt6.QtGui import (
    QFont, QColor, QPixmap, QPainter, QPen,
)
from PyQt6.Qsci import (
    QsciScintilla, QsciLexer,
    QsciLexerCPP, QsciLexerPython,
    QsciLexerJava, QsciLexerJavaScript, QsciLexerXML, QsciLexerYAML,
    QsciLexerSQL, QsciLexerMarkdown, QsciLexerBash, QsciLexerLua,
    QsciLexerRuby,
)

from editor.constants import (
    CODE_FONT_SIZE, DEFAULT_THEME, FALLBACK_BG, FALLBACK_FG, FOLD_ICON_SIZE,
    DEFAULT_TAB_WIDTH, EDITOR_BG_DARKEN,
)
from editor.font_settings import code_font_family

# Красный по умолчанию для НЕпарной скобки (если в теме нет
# brace_mismatch_background)
_BRACE_MISMATCH_BG = "#ff4d4d"

# ----------------------------------------------------------------------
# Глобальный маппинг стилей для встроенных лексеров
# ----------------------------------------------------------------------
def _c_opt(klass, name: str, fallback: int) -> int:
    return getattr(klass, name, fallback)

STYLE_MAP: Dict[Any, Dict[str, int]] = {
    QsciLexerCPP: {
        "default": 0,
        "keyword": QsciLexerCPP.Keyword,
        "string": QsciLexerCPP.DoubleQuotedString,
        "string_char": QsciLexerCPP.SingleQuotedString,
        "number": QsciLexerCPP.Number,
        "comment": QsciLexerCPP.Comment,
        "commentline": QsciLexerCPP.CommentLine,
        "commentdoc": QsciLexerCPP.CommentDoc,
        "commentlinedoc": _c_opt(QsciLexerCPP, 'CommentLineDoc', 23),
        "preprocessor": QsciLexerCPP.PreProcessor,
        "operator": QsciLexerCPP.Operator,
    },
    QsciLexerPython: {
        "default": 0,
        "keyword": QsciLexerPython.Keyword,
        "string": QsciLexerPython.DoubleQuotedString,
        "string_single": QsciLexerPython.SingleQuotedString,
        "string_triple": QsciLexerPython.TripleSingleQuotedString,
        "string_tripledouble": QsciLexerPython.TripleDoubleQuotedString,
        "number": QsciLexerPython.Number,
        "comment": QsciLexerPython.Comment,
        "commentblock": QsciLexerPython.CommentBlock,
        "class_name": QsciLexerPython.ClassName,
        "function_name": QsciLexerPython.FunctionMethodName,
        "operator": QsciLexerPython.Operator,
    },
    QsciLexerJava: {
        "default": 0,
        "keyword": QsciLexerJava.Keyword,
        "string": QsciLexerJava.DoubleQuotedString,
        "number": QsciLexerJava.Number,
        "comment": QsciLexerJava.Comment,
    },
    QsciLexerJavaScript: {
        "default": 0,
        "keyword": QsciLexerJavaScript.Keyword,
        "string": QsciLexerJavaScript.DoubleQuotedString,
        "number": QsciLexerJavaScript.Number,
        "comment": QsciLexerJavaScript.Comment,
    },
    QsciLexerXML: {
        "default": 0,
        "string": QsciLexerXML.HTMLDoubleQuotedString,
        "comment": QsciLexerXML.HTMLComment,
    },
    QsciLexerYAML: {
        "default": QsciLexerYAML.Default,
    },
    QsciLexerSQL: {
        "default": 0,
        "keyword": QsciLexerSQL.Keyword,
        "string": QsciLexerSQL.DoubleQuotedString,
        "number": QsciLexerSQL.Number,
        "comment": QsciLexerSQL.Comment,
    },
    QsciLexerMarkdown: {
        "default": QsciLexerMarkdown.Default,
    },
    QsciLexerBash: {
        "default": 0,
        "keyword": QsciLexerBash.Keyword,
        "string": QsciLexerBash.DoubleQuotedString,
        "comment": QsciLexerBash.Comment,
    },
    QsciLexerLua: {
        "default": 0,
        "keyword": QsciLexerLua.Keyword,
        "string": QsciLexerLua.String,
        "number": QsciLexerLua.Number,
        "comment": QsciLexerLua.Comment,
    },
    QsciLexerRuby: {
        "default": 0,
        "keyword": QsciLexerRuby.Keyword,
        "string": QsciLexerRuby.DoubleQuotedString,
        "comment": QsciLexerRuby.Comment,
    },
}


def _lighten(color: QColor, amount: float = 0.33) -> QColor:
    """Смешивает цвет с белым на amount (0..1) — «фон, но светлее»."""
    return QColor(
        int(color.red() + (255 - color.red()) * amount),
        int(color.green() + (255 - color.green()) * amount),
        int(color.blue() + (255 - color.blue()) * amount),
    )


def _normalize_operator_style(theme: dict):
    """Операторы в тёмных темах — цвет фона, но светлее (без ярких красок).

    Светлые темы не трогаем: там автор темы сам задаёт цвет операторов
    под светлый фон (в Solarized это обычный цвет текста base00).
    Пересчитывается программно из фона темы при загрузке, поэтому работает
    и для будущих тем, и для всех лексеров (встроенных и кастомных).
    """
    colors = theme.get("colors", {})
    styles = theme.get("styles", {})
    bg = colors.get("background")
    operator = styles.get("operator")
    if not bg or not operator:
        return
    bg_color = QColor(bg)
    styles["operator"] = dict(operator)
    if bg_color.lightness() > 128:
        return
    styles["operator"]["color"] = _lighten(bg_color).name()


# ----------------------------------------------------------------------
# Иконки фолдинга
# ----------------------------------------------------------------------
# Маркеры фолдинга Scintilla (25–31) и типы их иконок
FOLD_MARKERS = (
    (25, "closed_connected"),  # SC_MARKNUM_FOLDEREND
    (26, "open_connected"),    # SC_MARKNUM_FOLDEROPENMID
    (27, "midtail"),           # SC_MARKNUM_MIDTAIL
    (28, "tail"),              # SC_MARKNUM_TAIL
    (29, "sub"),               # SC_MARKNUM_SUB
    (30, "closed"),            # SC_MARKNUM_FOLDER
    (31, "open"),              # SC_MARKNUM_FOLDEROPEN
)


def _fold_icon(kind: str, fill: QColor, outline: QColor,
               width: int = FOLD_ICON_SIZE, height: Optional[int] = None) -> QPixmap:
    """Рисует иконку фолдинга с антиалиасингом.

    Рендерим в scale-раз больше и плавно уменьшаем — так мелкие иконки
    получаются чёткими, без «лесенок» встроенных маркеров Scintilla.
    """
    h = width if height is None else height
    scale = 1
    bw, bh = width * scale, h * scale

    pm = QPixmap(bw, bh)
    pm.fill(fill)

    p = QPainter(pm)
    p.setRenderHint(QPainter.RenderHint.Antialiasing)
    #p.setRenderHint(QPainter.RenderHint.SmoothPixmapTransform)

    pen = QPen(outline, max(1, scale * 1))
    pen.setCapStyle(Qt.PenCapStyle.FlatCap)
    pen.setJoinStyle(Qt.PenJoinStyle.RoundJoin)
    p.setPen(pen)
    p.setBrush(QColor(fill))

    cx, cy = bw / 2.0, bh / 2.0

    def hline(x1, x2, y):
        p.drawLine(QPointF(x1, y), QPointF(x2, y))

    def vline(x, y1, y2):
        p.drawLine(QPointF(x, y1), QPointF(x, y2))

    if kind in ("closed", "open", "closed_connected", "open_connected"):
        # Кружок с плюсом/минусом, отцентрированный по высоте строки.
        # У свернутых блоков (closed) линию дерева не рисуем — только кружок;
        # у развёрнутых (open) линия продолжается к детям.
        r = bw * 0.4
        cy = bh / 2.0
        if kind == "open":
            vline(cx, bh / 2, bh)          # линия вниз — к детям
        elif kind != "closed":
            vline(cx, 0, bh)               # линия сквозь всю ячейку
        p.drawEllipse(QRectF(cx - r, cy - r, 2 * r, 2 * r))
        arm = r * 0.50
        if kind.startswith("closed"):
            vline(cx, cy - arm, cy + arm)   # вертикаль плюса
        hline(cx - arm, cx + arm, cy)       # горизонталь (+/−)

    elif kind == "midtail":
        # «├»: вертикаль на всю высоту + ручка вправо
        vline(cx, 0, bh)
        hline(cx, bw, cy)
    elif kind == "tail":
        # «└»: уголок
        vline(cx, 0, cy)
        hline(cx, bw, cy)
    elif kind == "sub":
        # «|»: вертикаль на всю высоту + ручка вправо
        vline(cx, 0, cy*2)
    p.end()

    return pm.scaled(width, h,
                     Qt.AspectRatioMode.IgnoreAspectRatio,
                     Qt.TransformationMode.SmoothTransformation)


# ----------------------------------------------------------------------
# ThemeManager
# ----------------------------------------------------------------------
class ThemeManager:
    """Загружает темы из *.json и применяет их к редакторам."""

    def __init__(self, themes_dir: str):
        self.themes_dir = Path(themes_dir)
        self.themes: Dict[str, dict] = {}
        self.current_theme = DEFAULT_THEME
        self.load_themes()

    def load_themes(self):
        """Загружает все *.json из self.themes_dir в self.themes."""
        self.themes.clear()
        if not self.themes_dir.exists():
            return
        for json_file in self.themes_dir.glob("*.json"):
            try:
                with open(json_file, "r", encoding="utf-8") as f:
                    data = json.load(f)
                    name = data.get("name", json_file.stem)
                    _normalize_operator_style(data)
                    self.themes[name] = data
            except Exception:
                pass

    def get_theme_names(self) -> List[str]:
        return list(self.themes.keys())

    def apply_fold_icons(self, editor: QsciScintilla, fill: QColor,
                         outline: QColor, line_height: int = 12):
        """Заменяет встроенные маркеры фолдинга на отрисованные иконки.

        fill    — цвет заливки (фон fold-маржина, чтобы кружки сливались с ним),
        outline — цвет линий и символов.
        """
        for marker_id, kind in FOLD_MARKERS:
            # Все иконки — на всю высоту строки, чтобы хвосты и уголки
            # стыковались в непрерывные линии дерева
            pm = _fold_icon(kind, fill, outline,
                            width=FOLD_ICON_SIZE,
                            height=max(line_height, FOLD_ICON_SIZE))
            editor.markerDefine(pm, marker_id)

    def apply_theme(self, editor: QsciScintilla, theme_name: str,
                    lexer: Optional[QsciLexer] = None):
        """Применяет тему к редактору и лексеру."""
        theme = self.themes.get(theme_name)
        self.current_theme = theme_name
        if not theme:
            return
        colors = theme.get("colors", {})
        styles = theme.get("styles", {})

        # Общие настройки редактора
        if "foreground" in colors:
            editor.setColor(QColor(colors["foreground"]))
        if "background" in colors:
            editor.setPaper(QColor(colors["background"]))
        if "caret" in colors:
            editor.setCaretForegroundColor(QColor(colors["caret"]))
        if "caret_line" in colors:
            editor.setCaretLineBackgroundColor(QColor(colors["caret_line"]))
        if "selection" in colors:
            editor.setSelectionBackgroundColor(QColor(colors["selection"]))
        if "margin_foreground" in colors:
            editor.setMarginsForegroundColor(QColor(colors["margin_foreground"]))
        if "margin_background" in colors:
            editor.setMarginsBackgroundColor(QColor(colors["margin_background"]))
        if "folding_foreground" in colors and "folding_background" in colors:
            bg_editor = QColor(colors.get("background", FALLBACK_BG))
            # Как в TwistLang: оба аргумента setFoldMarginColors ОДИНАКОВЫЕ (цвет фона).
            #   fore → SCI_SETFOLDMARGINHICOLOUR — рамки/линии сворачивания
            #   back → SCI_SETFOLDMARGINCOLOUR  — вся заливка fold margin
            # Когда оба цвета совпадают — рамки невидимы, видна только сплошная заливка.
            editor.setFoldMarginColors(bg_editor, bg_editor)

            # Цвета маркеров (+/–) НЕ задаём здесь: вместо встроенных растровых
            # символов Scintilla подставляются отрисованные pixmap-иконки —
            # это делает CodeEditor.apply_fold_icons() с актуальным фоном редактора.
            # Хвосты/уголки (midtail/tail/sub) должны тянуться на всю высоту
            # строки, поэтому иконки пересобираются при зуме.

        # Подсветка парных скобок (включена setBraceMatching в CodeEditor)
        # Пара подсвечивается фоном чуть светлее selection — чтобы отличать
        # её от реального выделения текста.
        if "brace_match_background" in colors:
            match_bg = QColor(colors["brace_match_background"])
        elif "selection" in colors:
            match_bg = _lighten(QColor(colors["selection"]), 0.18)
        else:
            match_bg = QColor(colors.get("background", FALLBACK_BG))
        editor.setMatchedBraceBackgroundColor(match_bg)
        editor.setMatchedBraceForegroundColor(QColor(colors.get(
            "brace_match_foreground", colors.get("foreground", FALLBACK_FG))))

        editor.setUnmatchedBraceBackgroundColor(QColor(colors.get(
            "brace_mismatch_background", _BRACE_MISMATCH_BG)))
        editor.setUnmatchedBraceForegroundColor(QColor(colors.get(
            "brace_mismatch_foreground", colors.get("foreground", FALLBACK_FG))))

        # Применяем стили к лексеру, если он есть
        if lexer is not None:
            # Если это кастомный лексер с методом set_theme
            if hasattr(lexer, "set_theme"):
                lexer.set_theme(theme)
                return

            # Для встроенных лексеров используем STYLE_MAP
            lexer_class = type(lexer)
            style_map = STYLE_MAP.get(lexer_class)
            if style_map is None:
                return

            # Задаём шрифт и цвета по умолчанию — семейство из настроек.
            # Размер — из редактора (_font_size, зум), а не из темы: смена
            # палитры не должна пересоздавать шрифты встроенных лексеров.
            default_style = styles.get("default", {})
            cur_size = editor.font().pointSize()
            size = (cur_size if cur_size and cur_size > 0
                    else default_style.get("size", CODE_FONT_SIZE))
            default_font = QFont(code_font_family(), size)
            default_font.setFixedPitch(True)
            default_font.setBold(default_style.get("bold", False))
            default_font.setItalic(default_style.get("italic", False))
            lexer.setDefaultFont(default_font)
            if "color" in default_style:
                lexer.setDefaultColor(QColor(default_style["color"]))
            if "background" in default_style:
                lexer.setDefaultPaper(QColor(default_style["background"]))

            # 1) Шрифт code_font_family() + фон из default-стиля для ВСЕХ стилей лексера
            default_bg = default_style.get("background")
            default_bg_color = QColor(default_bg) if default_bg else None
            for sid in range(128):
                if not lexer.description(sid):
                    continue
                # Принудительно code_font_family() + цвета по умолчанию для каждого стиля
                lexer.setFont(default_font, sid)
                if "color" in default_style:
                    lexer.setColor(QColor(default_style["color"]), sid)
                if default_bg_color:
                    lexer.setPaper(default_bg_color, sid)

            # 2) Цвета/шрифты из STYLE_MAP + фоны ТОЛЬКО если явно указаны
            for style_key, style_id in style_map.items():
                style_data = styles.get(style_key)
                if not style_data:
                    continue
                if "color" in style_data:
                    lexer.setColor(QColor(style_data["color"]), style_id)
                if "background" in style_data:
                    lexer.setPaper(QColor(style_data["background"]), style_id)

                font = QFont(code_font_family(), size)
                font.setFixedPitch(True)
                font.setBold(style_data.get("bold", False))
                font.setItalic(style_data.get("italic", False))
                font.setUnderline(style_data.get("underline", False))
                lexer.setFont(font, style_id)
