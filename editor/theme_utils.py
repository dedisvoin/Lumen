"""
Общие утилиты темизации: разбор цветов темы, направленное оттенение
фона, цвет точек-пробелов, QSS-цвета.

Раньше логика «достать bg/fg/border из темы» и «затемнить фон с
fallback'ом для #000000» дублировалась в main.py, code_editor.py,
file_tree.py, tab_theme.py и terminal.py — теперь она живёт здесь.

Направление оттенения зависит от типа темы (флаг "dark" в JSON или
светлота фона): на тёмной теме элементы светлеют, на светлой — темнеют.
"""

from dataclasses import dataclass
from typing import Optional

from PyQt6.QtCore import Qt, QRectF, QPointF
from PyQt6.QtGui import QColor, QPainter, QPen, QPainterPath
from PyQt6.QtSvg import QSvgRenderer

from editor.constants import (
    BLACK_FALLBACK, EDITOR_BG_DARKEN, FALLBACK_BG, FALLBACK_FG,
    WHITESPACE_DOT_MIX, CAPTION_BUTTON_W, CAPTION_ICON_BOX,
)


def resolve_bg(colors: dict) -> str:
    """Фон темы (с fallback'ом)."""
    return colors.get("background", FALLBACK_BG)


def resolve_fg(colors: dict) -> str:
    """Основной цвет текста (с fallback'ом)."""
    return colors.get("foreground", FALLBACK_FG)


def resolve_border(colors: dict, fg: str) -> str:
    """Цвет границ интерфейса: border, иначе margin_foreground, иначе fg."""
    return colors.get("border", colors.get("margin_foreground", fg))


def strengthen_border(bg: str, border: str, fg: str) -> str:
    """На светлом фоне светлая обводка теряется — подмешиваем цвет текста.

    Срабатывает только когда фон светлый, а обводка ближе к фону, чем
    к тексту темы: тёмные темы и явно тёмные обводки не трогаем.
    """
    bg_color = QColor(bg)
    if bg_color.lightness() <= 128:
        return border
    fg_color = QColor(fg)
    border_color = QColor(border)
    midpoint = (bg_color.lightness() + fg_color.lightness()) / 2
    if border_color.lightness() <= midpoint:
        return border
    return QColor(
        int(border_color.red() * 0.5 + fg_color.red() * 0.5),
        int(border_color.green() * 0.5 + fg_color.green() * 0.5),
        int(border_color.blue() * 0.5 + fg_color.blue() * 0.5),
    ).name()


def is_dark_theme(theme: dict) -> bool:
    """Тип темы: явный флаг "dark" в JSON, иначе по светлоте фона.

    Темы обязаны указывать dark/light явно (поле "dark"), но для тем без
    флага подстраховываемся светлотой фона (<= 128 — тёмная).
    """
    mode = theme.get("dark")
    if mode is not None:
        return bool(mode)
    return QColor(resolve_bg(theme.get("colors", {}))).lightness() <= 128


def _wcag_contrast(a: QColor, b: QColor) -> float:
    """Контраст WCAG: (L1+0.05)/(L2+0.05), где L — относительная яркость."""
    def lum(c):
        def lin(v):
            v /= 255.0
            return v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4
        return (0.2126 * lin(c.red()) + 0.7152 * lin(c.green())
                + 0.0722 * lin(c.blue()))

    la, lb = lum(a), lum(b)
    hi, lo = max(la, lb), min(la, lb)
    return (hi + 0.05) / (lo + 0.05)


# Минимальный контраст цвета палитры с фоном терминала на светлых темах:
# там white/bright_white в темах равны фону (белый на белом) — текст
# не читается, такие цвета затемняем.
_TERMINAL_MIN_CONTRAST = 2.2


def _ensure_terminal_contrast(color: QColor, bg: QColor) -> QColor:
    """Затемняет цвет палитры до минимального контраста со светлым фоном.

    Тёмные темы не трогаем: у них светлые цвета и так контрастны,
    а black намеренно равен фону (часть дизайна темы).
    """
    for _ in range(80):
        if _wcag_contrast(color, bg) >= _TERMINAL_MIN_CONTRAST:
            break
        color = color.darker(110)
    return color


def resolve_terminal_palette(theme: dict, tc: "ThemeColors") -> list:
    """16-цветная палитра терминала из темы (ANSI-порядок: black..white,
    затем bright_black..bright_white).

    Приоритет у явной секции "terminal" в JSON темы. Без секции цвета
    берутся из стилей синтаксиса и фона темы, а яркие версии на тёмных
    темах осветляются (на светлых остаются базовыми — как в Solarized).
    """
    colors = theme.get("colors", {})
    styles = theme.get("styles", {})
    term = theme.get("terminal") or {}
    dark = tc.dark

    def style_color(name, fallback=None):
        st = styles.get(name)
        return st.get("color") if st and st.get("color") else fallback

    def pick(key, *fallbacks):
        for f in fallbacks:
            if f:
                return term.get(key) or f
        return fallbacks[-1]

    base = [
        pick("black", colors.get("background"), tc.bg),
        pick("red", colors.get("brace_mismatch_background"),
             style_color("keyword", "#f14c4c")),
        pick("green", style_color("string", "#98c379")),
        pick("yellow", style_color("number"),
             style_color("preprocessor", "#e5c07b")),
        pick("blue", style_color("function_name"),
             style_color("type"), style_color("class_name", "#61afef")),
        pick("magenta", style_color("keyword", "#c678dd")),
        pick("cyan", style_color("operator"),
             style_color("class_name"), style_color("type", "#56b6c2")),
        pick("white", colors.get("foreground"), tc.fg),
    ]
    base = [QColor(c) for c in base]

    bright = []
    for i, key in enumerate(
        ("bright_black", "bright_red", "bright_green", "bright_yellow",
         "bright_blue", "bright_magenta", "bright_cyan", "bright_white")
    ):
        explicit = term.get(key)
        if explicit:
            bright.append(QColor(explicit))
        elif i == 0 and "margin_foreground" in colors:
            bright.append(QColor(colors["margin_foreground"]))
        elif dark:
            bright.append(base[i].lighter(125))
        else:
            bright.append(base[i])
    palette = base + bright
    if not dark:
        bg = tc.editor_bg_color
        palette = [_ensure_terminal_contrast(c, bg) for c in palette]
    return palette


def shade(bg: str, percent: int, dark: bool = True) -> QColor:
    """Направленное оттенение фона на percent%.

    Тёмная тема → элементы светлеют (lighter), светлая → темнеют
    (darker). Для чистого #000000 lighter/darker ничего не меняют,
    поэтому подставляем fallback — иначе элементы сливаются с чёрным.
    """
    color = QColor(bg)
    if color == QColor("#000000"):
        return QColor(BLACK_FALLBACK)
    if dark:
        return color.lighter(percent)
    return color.darker(percent)


def qss_color(color: QColor) -> str:
    """QColor -> QSS-строка (rgba для полупрозрачных цветов)."""
    if color.alpha() >= 255:
        return color.name()
    return (f"rgba({color.red()},{color.green()},"
            f"{color.blue()},{color.alpha()})")


def apply_narrow_caption_buttons(title_bar) -> None:
    """Сужает кнопки окна (свернуть/развернуть/закрыть) и центрирует иконки.

    qframelesswindow жёстко ставит кнопкам setFixedSize(46, 32) и рисует
    иконки АБСОЛЮТНЫМИ координатами (напр. drawLine(18, 16, 28, 16)),
    то есть все они рассчитаны на бокс шириной CAPTION_ICON_BOX.

    Одного setFixedWidth мало: иконка свернуть нарисовалась бы на x=18..28 и
    при ширине 34 уехала вправо от центра. Поэтому paintEvent каждой кнопки
    заменяется на свою отрисовку — тот же рисунок, но сдвинутый на
    (ширина - CAPTION_ICON_BOX) / 2, то есть в центр новой кнопки.
    Метод вешается на ЭКЗЕМПЛЯР, поэтому сигналы и ссылки tb.minBtn и т.п.
    остаются нетронутыми.
    """
    for btn in (title_bar.minBtn, title_bar.maxBtn, title_bar.closeBtn):
        btn.setFixedWidth(CAPTION_BUTTON_W)
        kind = "min"
        if btn is title_bar.maxBtn:
            kind = "max"
        elif btn is title_bar.closeBtn:
            kind = "close"
        btn.paintEvent = _make_caption_paint_event(btn, kind)


def _make_caption_paint_event(btn, kind: str):
    """Собирает paintEvent, центрирующий иконку кнопки окна."""

    def paintEvent(e):
        p = QPainter(btn)
        color, bg = btn._getColors()

        # фон кнопки (обычная заливка rect виджета)
        p.setBrush(bg)
        p.setPen(Qt.PenStyle.NoPen)
        p.drawRect(btn.rect())

        # переносим жёсткую геометрию библиотеки в центр узкой кнопки
        p.translate((btn.width() - CAPTION_ICON_BOX) / 2, 0)

        p.setBrush(Qt.BrushStyle.NoBrush)
        pen = QPen(color, 1)
        pen.setCosmetic(True)
        p.setPen(pen)

        if kind == "min":
            p.drawLine(18, 16, 28, 16)
        elif kind == "max":
            p.setRenderHints(
                QPainter.RenderHint.Antialiasing
                | QPainter.RenderHint.SmoothPixmapTransform
            )
            r = btn.devicePixelRatioF()
            p.scale(1 / r, 1 / r)
            if not btn._isMax:
                p.drawRect(int(18 * r), int(11 * r), int(10 * r), int(10 * r))
            else:
                p.drawRect(int(18 * r), int(13 * r), int(8 * r), int(8 * r))
                x0 = int(18 * r) + int(2 * r)
                y0 = 13 * r
                dw = int(2 * r)
                path = QPainterPath(QPointF(x0, y0))
                path.lineTo(x0, y0 - dw)
                path.lineTo(x0 + 8 * r, y0 - dw)
                path.lineTo(x0 + 8 * r, y0 - dw + 8 * r)
                path.lineTo(x0 + 8 * r - dw, y0 - dw + 8 * r)
                p.drawPath(path)
        else:
            # close: библиотека рисует svg на весь rect; сохраняем её
            # пропорции (QSvgRenderer сам вписывает svg по preserveAspectRatio),
            # отдавая бокс той же ширины, что и раньше, но сдвинутый в центр
            p.setRenderHints(
                QPainter.RenderHint.Antialiasing
                | QPainter.RenderHint.SmoothPixmapTransform
            )
            color_name = color.name()
            path_nodes = btn._svgDom.elementsByTagName("path")
            for i in range(path_nodes.length()):
                el = path_nodes.at(i).toElement()
                el.setAttribute("stroke", color_name)
            renderer = QSvgRenderer(btn._svgDom.toByteArray())
            renderer.render(p, QRectF(0, 0, CAPTION_ICON_BOX, btn.height()))
        p.end()

    return paintEvent


@dataclass
class ThemeColors:
    """Разобранные цвета темы: фон, текст, границы, приглушённый, выделение.

    dark=True — тёмная тема (оттенение светлит элементы),
    dark=False — светлая (оттенение затемняет элементы).
    """

    bg: str
    fg: str
    border: str
    muted: str
    selection: Optional[str]
    accent: Optional[str] = None
    editor_bg: Optional[str] = None
    title_bar_bg: Optional[str] = None
    inlay_hint: Optional[str] = None
    error: Optional[str] = None
    warning: Optional[str] = None
    dark: bool = True

    @classmethod
    def from_theme_manager(cls, theme_manager, theme_name: str) -> "ThemeColors":
        theme = theme_manager.themes.get(theme_name, {})
        colors = theme.get("colors", {})
        fg = resolve_fg(colors)
        bg = resolve_bg(colors)
        return cls(
            bg=bg,
            fg=fg,
            border=strengthen_border(bg, resolve_border(colors, fg), fg),
            muted=colors.get("margin_foreground",
                             colors.get("folding_foreground", fg)),
            selection=colors.get("selection"),
            accent=colors.get("accent"),
            editor_bg=colors.get("editor_background"),
            title_bar_bg=colors.get("title_bar_background"),
            inlay_hint=colors.get("inlay_hint"),
            error=colors.get("error"),
            warning=colors.get("warning"),
            dark=is_dark_theme(theme),
        )

    @property
    def is_light(self) -> bool:
        """Тема светлая (элементы интерфейса затемняются)."""
        return not self.dark

    @property
    def title_bg(self) -> Optional[str]:
        """Фон тайтл-бара: explicit title_bar_background из темы (Zed-темы
        хотят чуть другой фон окна, чем у кода), иначе None."""
        return self.title_bar_bg

    @property
    def editor_bg_color(self) -> QColor:
        """Фон области кода: явный editor_background из темы (светлые темы
        хотят точный фон, как в Solarized), иначе оттенённый фон темы."""
        if self.editor_bg:
            return QColor(self.editor_bg)
        return shade(self.bg, EDITOR_BG_DARKEN, self.dark)

    @property
    def inlay_hint_color(self) -> Optional[QColor]:
        """Цвет inlay-подсказок из темы (None — использовать дефолт)."""
        if self.inlay_hint:
            return QColor(self.inlay_hint)
        return None

    @property
    def error_color(self) -> Optional[QColor]:
        """Цвет ошибок (ErrorLens/индикаторы) из темы, иначе None."""
        if self.error:
            return QColor(self.error)
        return None

    @property
    def warning_color(self) -> Optional[QColor]:
        """Цвет предупреждений (ErrorLens/индикаторы) из темы, иначе None."""
        if self.warning:
            return QColor(self.warning)
        return None

    @property
    def bg_color(self) -> QColor:
        return QColor(self.bg)

    @property
    def title_bg(self) -> Optional[str]:
        """Фон тайтл-бара из темы (title_bar_background), иначе None."""
        return self.title_bar_bg

    @property
    def fg_color(self) -> QColor:
        return QColor(self.fg)

    @property
    def accent_color(self) -> QColor:
        """Акцентный (зелёный) цвет темы; fallback для старых тем.

        На светлых темах акцент приглушается (насыщенность режется вдвое):
        иначе насыщенный цвет вроде #859900 (Solarized Light) вырвиглазно
        смотрится на светлом фоне интерфейса.
        """
        if self.accent:
            color = QColor(self.accent)
        else:
            color = QColor("#23d18b" if self.dark else "#188038")
        if not self.dark:
            h, s, v, _a = color.getHsv()
            if v > 0:
                color.setHsv(h, max(s // 2, 0), v)
        return color

    def shade(self, percent: int) -> QColor:
        """Фон темы с направленным оттенением по типу темы."""
        return shade(self.bg, percent, self.dark)

    def whitespace_color(self) -> QColor:
        """Цвет точек-пробелов: фон редактора, смешанный с текстом темы.

        На тёмной теме текст светлый — точки светлеют; на светлой текст
        тёмный — точки темнеют. В отличие от lighter/darker, смесь всегда
        заметно отличается от фона, поэтому точки видны на любой теме.
        """
        bg_color = self.editor_bg_color
        fg_color = QColor(self.fg)
        return QColor(
            int(bg_color.red() + (fg_color.red() - bg_color.red()) * WHITESPACE_DOT_MIX),
            int(bg_color.green() + (fg_color.green() - bg_color.green()) * WHITESPACE_DOT_MIX),
            int(bg_color.blue() + (fg_color.blue() - bg_color.blue()) * WHITESPACE_DOT_MIX),
        )
