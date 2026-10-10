"""
dialog_theme — QSS стандартных диалогов в стиле TaskDialog.

QMessageBox, QInputDialog и QFileDialog (в режиме без нативного окна)
получают те же цвета, что и диалог создания таска: фон области кода,
приглушённые подписи, рамки и кнопки. QSS навешивается на сам диалог
в themed_dialogs.install_titlebar (не на QApplication — смена app-QSS
заставляет Qt переполировать все виджеты приложения на каждой смене
темы); все правила ограничены селекторами классов диалогов.
"""

from PyQt6.QtGui import QColor

from editor.constants import UI_FONT_SIZE
from editor.font_settings import ui_font_family
from editor.theme_utils import ThemeColors, qss_color


def dialog_theme_qss(tc: ThemeColors) -> str:
    bg = tc.editor_bg_color.name()
    fg = tc.fg
    border = tc.border
    muted = tc.muted
    acc = QColor(tc.selection or tc.fg)
    acc_name = acc.name()
    # Выделение строк/элементов — как выделение в дереве файлов
    sel = QColor(acc)
    sel.setAlpha(45)
    hover = QColor(fg)
    hover.setAlpha(30)

    return f"""
        QMessageBox, QInputDialog, QFileDialog {{
            background-color: {bg};
            font-family: "{ui_font_family()}";
            font-size: {UI_FONT_SIZE}pt;
        }}
        QMessageBox QLabel, QInputDialog QLabel, QFileDialog QLabel {{
            color: {muted};
            font-family: "{ui_font_family()}";
            font-size: {UI_FONT_SIZE}pt;
        }}
        QMessageBox QPushButton, QInputDialog QPushButton,
        QFileDialog QPushButton {{
            color: {fg};
            background-color: {bg};
            border: 0.5px solid {border};
            border-radius: 4px;
            padding: 4px 14px;
            font-family: "{ui_font_family()}";
            font-size: {UI_FONT_SIZE}pt;
        }}
        QMessageBox QPushButton:hover, QInputDialog QPushButton:hover,
        QFileDialog QPushButton:hover,
        QMessageBox QPushButton:default, QInputDialog QPushButton:default,
        QFileDialog QPushButton:default {{
            border-color: {acc_name};
        }}
        QMessageBox QCheckBox {{
            color: {fg};
            spacing: 6px;
        }}
        QInputDialog QLineEdit, QFileDialog QLineEdit {{
            background-color: {bg};
            color: {fg};
            border: 0.5px solid {border};
            border-radius: 4px;
            padding: 4px 6px;
            selection-background-color: {qss_color(sel)};
        }}
        QInputDialog QLineEdit:focus, QFileDialog QLineEdit:focus {{
            border-color: {acc_name};
        }}
        QInputDialog QComboBox, QFileDialog QComboBox {{
            background-color: {bg};
            color: {fg};
            border: 0.5px solid {border};
            border-radius: 4px;
            padding: 3px 6px;
        }}
        QInputDialog QComboBox QAbstractItemView,
        QFileDialog QComboBox QAbstractItemView {{
            background-color: {bg};
            color: {fg};
            border: 0.5px solid {border};
            selection-background-color: {qss_color(sel)};
        }}
        QFileDialog QListView, QFileDialog QTreeView {{
            background-color: {bg};
            color: {fg};
            border: 0.5px solid {border};
            border-radius: 4px;
            outline: 0;
        }}
        QFileDialog QListView::item:selected,
        QFileDialog QTreeView::item:selected {{
            background-color: {qss_color(sel)};
        }}
        QFileDialog QHeaderView::section {{
            background-color: {bg};
            color: {muted};
            border: none;
            border-bottom: 0.5px solid {border};
            padding: 3px 6px;
        }}
        QFileDialog QToolButton {{
            background: transparent;
            border: none;
            color: {fg};
            padding: 3px;
            border-radius: 4px;
        }}
        QFileDialog QToolButton:hover {{
            background-color: {qss_color(hover)};
        }}
        QFileDialog QSplitter::handle {{
            background-color: {border};
        }}
    """
