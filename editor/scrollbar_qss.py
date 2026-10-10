"""
Общий QSS тонких скроллбаров — один стиль для редактора кода,
дерева файлов и любых других панелей.
"""

from PyQt6.QtGui import QColor

from editor.constants import (SCROLLBAR_WIDTH, SCROLLBAR_HANDLE_INSET,
                               SCROLLBAR_HANDLE_START, SCROLLBAR_HANDLE_END)


def scrollbar_qss(bg: str, fg: QColor, width: int = SCROLLBAR_WIDTH,
                  alpha: float = 1.0) -> str:
    """QSS скроллбара: фон bg, бегунок — цвет fg с прозрачностью.

    Выглядит одинаково в вертикали и горизонтали: без стрелок, бегунок
    уже самой полосы и отодвинут от её краёв на SCROLLBAR_HANDLE_INSET,
    поэтому трек остаётся невидимым и читается как «плавающий» бегунок
    (как в VS Code). Толщина бегунка выводится из полосы и отступов:
    при полосе 20px и отступе 7px бегунок — 6px.
    По длине трека бегунок тоже не прилипает к краям: отступ
    SCROLLBAR_HANDLE_START в начале (сверху/слева) и
    SCROLLBAR_HANDLE_END в конце (снизу/справа).

    alpha (0..1) — множитель непрозрачности бегунка для плавного
    затухания: 1.0 — полностью виден, 0.0 — невидим, полоса при этом
    не освобождает место. Это ключевое отличие от ScrollBarAlwaysOff,
    который сужает вьюпорт и вызывает reflow текста (строка прыгает,
    позиция каретки уезжает).

    Трек всегда красится в bg — цвет фона редактора, а не
    «прозрачный»: за transparent проступает палитра виджета, а
    ::groove Qt рисует сам своим серым. Оба случая давали за полосой
    цвет, отличный от фона редактора.
    """
    r, g, b = fg.red(), fg.green(), fg.blue()
    inset = SCROLLBAR_HANDLE_INSET
    start = SCROLLBAR_HANDLE_START
    end = SCROLLBAR_HANDLE_END
    handle_w = width - 2 * inset
    if handle_w < 2:
        inset = 0
        start = 0
        end = 0
        handle_w = width
    radius = max(1, handle_w // 2)
    k = 0.0 if alpha < 0 else (1.0 if alpha > 1 else alpha)

    def rgba(base: int) -> str:
        return f"rgba({r},{g},{b},{int(round(base * k))})"

    h, h_hover, h_press = rgba(50), rgba(100), rgba(160)
    return f"""
        QScrollBar:vertical {{
            width: {width}px;
            background: {bg};
            margin: 0;
            border: none;
        }}
        QScrollBar::groove:vertical {{
            background: {bg};
            border: none;
        }}
        QScrollBar::handle:vertical {{
            background: {h};
            border: none;
            min-height: 30px;
            min-width: {handle_w}px;
            max-width: {handle_w}px;
            margin: {start}px {inset}px {end}px {inset}px;
            border-radius: {radius}px;
        }}
        QScrollBar::handle:vertical:hover {{
            background: {h_hover};
        }}
        QScrollBar::handle:vertical:pressed {{
            background: {h_press};
        }}
        QScrollBar::add-line:vertical,
        QScrollBar::sub-line:vertical {{
            height: 0;
            background: none;
        }}
        QScrollBar::add-page:vertical,
        QScrollBar::sub-page:vertical {{
            background: {bg};
        }}
        QScrollBar:horizontal {{
            height: {width}px;
            background: {bg};
            margin: 0;
            border: none;
        }}
        QScrollBar::groove:horizontal {{
            background: {bg};
            border: none;
        }}
        QScrollBar::handle:horizontal {{
            background: {h};
            border: none;
            min-width: 30px;
            min-height: {handle_w}px;
            max-height: {handle_w}px;
            margin: {inset}px {end}px {inset}px {start}px;
            border-radius: {radius}px;
        }}
        QScrollBar::handle:horizontal:hover {{
            background: {h_hover};
        }}
        QScrollBar::handle:horizontal:pressed {{
            background: {h_press};
        }}
        QScrollBar::add-line:horizontal,
        QScrollBar::sub-line:horizontal {{
            width: 0;
            background: none;
        }}
        QScrollBar::add-page:horizontal,
        QScrollBar::sub-page:horizontal {{
            background: {bg};
        }}
    """
