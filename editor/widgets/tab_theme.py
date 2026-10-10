"""
Общий QSS таб-бара — единый стиль вкладок для панелей редакторов
и нижней панели терминалов. Функция tab_bar_qss() переиспользуется
и в глобальном QSS главного окна (main.py).
"""

from PyQt6.QtWidgets import QTabBar

from editor.constants import (
    UI_FONT_SIZE,
    SELECTED_TAB_DARKEN, TAB_BAR_DARKEN, UNSELECTED_TAB_DARKEN, HOVER_TAB_DARKEN,
    TAB_PADDING_PANE,
)
from editor.font_settings import ui_font_family
from editor.theme_utils import ThemeColors, shade


def tab_bar_qss(bg: str, fg: str, border: str, padding: str,
                dark: bool = True, selected_bg: str = None,
                tab_top_border: bool = True) -> str:
    """QSS вкладок QTabBar: фон бара, цвета табов, hover/выбранный таб.

    dark=True (тёмная тема) — оттенение светлит элементы, dark=False
    (светлая) — затемняет (направленное оттенение, см. theme_utils.shade).
    selected_bg — фон выбранной вкладки (обычно фон области кода, чтобы
    вкладка «прилипала» к содержимому); None — оттенённый bg.
    tab_top_border — рисовать ли верхнюю линию таба: у нижней панели
    вертикального сплита её даёт рамка верхней панели (иначе дубль).
    """
    bar_bg = shade(bg, TAB_BAR_DARKEN, dark)
    if selected_bg is None:
        selected_bg = shade(bg, SELECTED_TAB_DARKEN, dark).name()
    top_border_qss = (
        f"border-top: 0.5px solid {border};"
        if tab_top_border else
        "border-top: 0px solid transparent;"
    )
    return f"""
        QTabBar {{
                font-family: "{ui_font_family()}";
            font-size: {UI_FONT_SIZE}pt;
            background: {bar_bg.name()};
            border: none;
            padding: 0;
        }}
        QTabBar::tab {{
                font-family: "{ui_font_family()}";
            font-size: {UI_FONT_SIZE}pt;
            color: {fg};
            background: {shade(bg, UNSELECTED_TAB_DARKEN, dark).name()};
            padding: {padding};
            border: 0.5px solid {border};
            border-right: none;  /* без двойной линии между табами */
            {top_border_qss}
            margin: 0;
        }}
        QTabBar::tab:last, QTabBar::tab:only-one {{
            border-right: 0.5px solid {border};
        }}
        QTabBar::tab:selected {{
            background: {selected_bg};
            border-bottom: 0px solid transparent;
        }}
        QTabBar::tab:!selected {{
            border-bottom: 0.5px solid {border};
        }}
        QTabBar::tab:hover:!selected {{
            background: {shade(bg, HOVER_TAB_DARKEN, dark).name()};
        }}
    """


def apply_tab_bar_theme(tab_bar: QTabBar, tc: ThemeColors,
                        tab_top_border: bool = True):
    """Применяет тему к таб-бару: фон, цвета вкладок, X-кнопки закрытия."""
    tab_bar.set_theme_colors(tc.bg, tc.fg, tc.border)
    tab_bar.setStyleSheet(
        tab_bar_qss(tc.bg, tc.fg, tc.border, TAB_PADDING_PANE, tc.dark,
                    selected_bg=tc.editor_bg_color.name(),
                    tab_top_border=tab_top_border))
