"""
SettingsDialog — панель настроек редактора.

Полноценное окно (FramelessDialog с StandardTitleBar) с двумя колонками:
слева дерево категорий (General / Editor / Terminal / LSP / Themes / About),
справа — страница настроек выбранной категории. Страницы строятся из схемы
(settings_schema.py), контролы — кастомные (settings_widgets.py).

Кнопки внизу: Reset (сброс к дефолтам), Cancel, Apply (сохранить и применить,
окно остаётся), OK (сохранить, применить и закрыть). Применение настроек к
окну редактора делает settings_apply.apply_settings(window).
"""

from typing import List, Optional, Tuple

from PyQt6.QtCore import Qt, QPoint
from PyQt6.QtGui import QColor, QPainter, QPen
from PyQt6.QtWidgets import (
    QAbstractItemView,
    QFrame,
    QHBoxLayout,
    QLabel,
    QPushButton,
    QScrollArea,
    QSizePolicy,
    QSplitter,
    QStackedWidget,
    QTreeWidget,
    QTreeWidgetItem,
    QVBoxLayout,
    QWidget,
)

from qframelesswindow import FramelessDialog, StandardTitleBar

from editor.constants import UI_FONT_SIZE
from editor.font_settings import ui_font_family
from editor.settings_store import app_settings
from editor.settings_schema import (
    ACTION_RESET_ALL,
    ACTION_RESTART_LSP,
    categories,
    iter_leaf_pages,
)
from editor.settings_widgets import (
    ButtonControl,
    ComboControl,
    PathControl,
    PlusMinusSpinBox,
    SettingsControl,
    ShortcutControl,
    SpinControl,
    ToggleControl,
    build_control,
)
from editor.settings_apply import apply_settings, apply_lsp_overrides, restart_lsp
from editor.theme_utils import ThemeColors, qss_color
from editor.theme_preview import ThemePreview
from editor.scrollbar_qss import scrollbar_qss
from editor.themed_dialogs import style_titlebar
from editor import themed_dialogs

# Ширина левой колонки с деревом категорий
_TREE_WIDTH = 230
# Минимальная ширина подписи контрола
_LABEL_MIN_WIDTH = 190


class SettingsSeparator(QWidget):
    """Горизонтальный разделитель строк настроек.

    Рисуется в физических пикселях (ширина пера 1/dpr), поэтому при любом
    дробном масштабировании Windows линия имеет одинаковую толщину 1px —
    в отличие от QFrame высотой 1 логический px, который округляется
    по-разному в зависимости от позиции.
    """

    def __init__(self, color=None, parent=None):
        super().__init__(parent)
        self._color = QColor(color) if color else QColor(0, 0, 0, 70)
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, False)
        self.setFixedHeight(1)

    def set_line_color(self, color: QColor):
        self._color = QColor(color)
        self.update()

    def paintEvent(self, event):
        painter = QPainter(self)
        dpr = self.devicePixelRatioF()
        if dpr <= 0.0:
            dpr = 1.0
        painter.setRenderHint(QPainter.RenderHint.Antialiasing, False)
        painter.scale(dpr, dpr)
        y_dev = max(0, int(self.height() * dpr) // 2)
        painter.setPen(QPen(self._color, 1))
        painter.drawLine(QPoint(0, y_dev),
                         QPoint(int(self.width() * dpr), y_dev))
        painter.end()


class SettingsDialog(FramelessDialog):
    """Окно настроек: дерево категорий слева, страницы справа."""

    def __init__(self, theme_manager, theme_name: str, window=None):
        super().__init__(window)
        self._theme_manager = theme_manager
        self._theme_name = theme_name
        self._window = window
        self._controls: List[Tuple[str, SettingsControl]] = []
        self._theme_was_set = bool(app_settings().get_str("themes.current"))
        self._current_theme = getattr(window, "current_theme", None) or theme_name

        self.setWindowTitle("Settings")
        self.setModal(True)
        self.resize(1040, 690)

        # Тайтл-бар как у главного окна / TaskDialog
        tb = StandardTitleBar(self)
        tb.titleLabel.setText("Settings")
        tb.maxBtn.hide()
        if tb.hBoxLayout.count() > 0:
            item = tb.hBoxLayout.itemAt(0)
            if item and item.spacerItem():
                tb.hBoxLayout.removeItem(item)
        if hasattr(tb, "iconLabel"):
            tb.iconLabel.hide()
        self.setTitleBar(tb)

        root = QVBoxLayout(self)
        root.setSpacing(0)
        root.setContentsMargins(16, self.titleBar.height() + 10, 16, 14)

        # --- две колонки: дерево + страницы ---
        self._splitter = QSplitter(Qt.Orientation.Horizontal, self)
        self._splitter.setChildrenCollapsible(False)
        # Без видимой ручки: границу рисует сама рамка дерева (border: 1px)
        self._splitter.setHandleWidth(0)
        root.addWidget(self._splitter, 1)

        self._tree = QTreeWidget(self._splitter)
        self._tree.setHeaderHidden(True)
        # Отступ листьев показывает древовидную структуру; полосу ветки на
        # ховере маскирует правило QTreeWidget::branch в QSS (см. тему).
        self._tree.setIndentation(14)
        self._tree.setUniformRowHeights(True)
        # Без стрелок-квадратиков веток (Windows style): категории всегда
        # развёрнуты, дети видны сразу — дерево выглядит как плоский список.
        self._tree.setRootIsDecorated(False)
        self._tree.setSelectionMode(
            QAbstractItemView.SelectionMode.SingleSelection)
        self._tree.setFocusPolicy(Qt.FocusPolicy.NoFocus)
        self._tree.setSizePolicy(QSizePolicy.Policy.Fixed,
                                 QSizePolicy.Policy.Expanding)
        self._tree.setFixedWidth(_TREE_WIDTH)
        self._splitter.addWidget(self._tree)

        # Вертикальная линия, отделяющая дерево категорий от страниц
        self._separator = QFrame(self._splitter)
        self._separator.setObjectName("treeSeparator")
        self._separator.setFrameShape(QFrame.Shape.NoFrame)
        self._separator.setFixedWidth(1)
        self._splitter.addWidget(self._separator)

        self._stack = QStackedWidget(self._splitter)
        self._splitter.addWidget(self._stack)
        self._splitter.setStretchFactor(0, 0)
        self._splitter.setStretchFactor(1, 0)
        self._splitter.setStretchFactor(2, 1)
        self._splitter.setSizes([_TREE_WIDTH, 1, 669])

        # --- нижняя панель кнопок ---
        bottom = QHBoxLayout()
        bottom.setSpacing(8)
        bottom.setContentsMargins(0, 10, 0, 0)

        self._reset_btn = QPushButton("Reset", self)
        self._reset_btn.setCursor(Qt.CursorShape.PointingHandCursor)
        self._reset_btn.setToolTip("Reset all settings to defaults")
        self._reset_btn.clicked.connect(self._reset_all)
        bottom.addWidget(self._reset_btn)
        bottom.addStretch(1)

        self._cancel_btn = QPushButton("Cancel", self)
        self._cancel_btn.clicked.connect(self.reject)
        bottom.addWidget(self._cancel_btn)

        self._apply_btn = QPushButton("Apply", self)
        self._apply_btn.clicked.connect(self._save_and_apply)
        bottom.addWidget(self._apply_btn)

        self._ok_btn = QPushButton("OK", self)
        self._ok_btn.setDefault(True)
        self._ok_btn.clicked.connect(self._on_ok)
        bottom.addWidget(self._ok_btn)
        root.addLayout(bottom)

        self._build_tree()
        self._build_pages()

        self._tree.currentItemChanged.connect(self._on_category_changed)
        self._tree.setCurrentItem(self._tree.topLevelItem(0))
        self._tree.expandAll()

        self._apply_dialog_theme()
        self._tree.setFocus()

    # ------------------------------------------------------------------
    # Построение UI
    # ------------------------------------------------------------------
    def _build_tree(self):
        """Дерево категорий: ветви с детьми разворачиваются."""
        cat_tree = categories()
        for cat in cat_tree:
            top = QTreeWidgetItem([cat["title"]])
            top.setData(0, Qt.ItemDataRole.UserRole, cat["key"])
            if cat["children"]:
                for child in cat["children"]:
                    item = QTreeWidgetItem([child["title"]])
                    item.setData(0, Qt.ItemDataRole.UserRole, child["key"])
                    top.addChild(item)
            self._tree.addTopLevelItem(top)

    def _build_pages(self):
        """Страницы (по одной на листовую категорию) + список контролов."""
        for page_key, title, items in iter_leaf_pages():
            self._build_page(page_key, items)

    def _build_page(self, page_key: str, items: list):
        page = QWidget()
        page_layout = QVBoxLayout(page)
        page_layout.setContentsMargins(8, 4, 8, 8)
        page_layout.setSpacing(2)

        total = len(items)
        for index, item in enumerate(items):
            ctype = item.get("type")
            if ctype == "info":
                control = build_control(item)
                page_layout.addWidget(control)
                continue
            control = build_control(item)
            self._configure_control(page_key, item, control)

            if ctype == "button":
                # Кнопки-действия — без дублирующей подписи слева
                btn_row = QHBoxLayout()
                btn_row.setContentsMargins(0, 6, 0, 6)
                btn_row.addStretch(1)
                btn_row.addWidget(control)
                page_layout.addLayout(btn_row)
            else:
                row = QHBoxLayout()
                row.setContentsMargins(0, 6, 0, 6)
                row.setSpacing(16)
                label = QLabel(item.get("label", ""))
                label.setObjectName("settingsName")
                label.setMinimumWidth(_LABEL_MIN_WIDTH)
                label.setSizePolicy(QSizePolicy.Policy.Fixed,
                                    QSizePolicy.Policy.Preferred)
                row.addWidget(label, 0, Qt.AlignmentFlag.AlignVCenter)
                row.addWidget(control, 1, Qt.AlignmentFlag.AlignRight
                              | Qt.AlignmentFlag.AlignVCenter)
                page_layout.addLayout(row)

                help_text = item.get("help")
                if help_text:
                    help_label = QLabel(help_text)
                    help_label.setObjectName("settingsHelp")
                    help_label.setWordWrap(True)
                    help_label.setSizePolicy(QSizePolicy.Policy.Preferred,
                                             QSizePolicy.Policy.Preferred)
                    page_layout.addWidget(help_label)

                # Превью выбранной темы — в том же блоке, что и выбор темы
                if page_key == "themes" and item.get("key") == "themes.current":
                    self._theme_preview = ThemePreview(self._theme_manager)
                    preview_row = QHBoxLayout()
                    preview_row.setContentsMargins(12, 0, 0, 0)
                    preview_row.addWidget(self._theme_preview)
                    page_layout.addLayout(preview_row)
                    for key, control in self._controls:
                        if key == "themes.current" and isinstance(
                                control, ComboControl):
                            control.combo.currentTextChanged.connect(
                                lambda _name: self._refresh_theme_preview())
                            break
                    self._refresh_theme_preview()

            # Горизонтальный разделитель между строками настроек (после
            # превью темы не рисуем — превью не строка настроек)
            if page_key == "themes" and item.get("key") == "themes.current":
                pass
            elif index < total - 1:
                sep = SettingsSeparator()
                page_layout.addWidget(sep)
            else:
                page_layout.addSpacing(4)

        page_layout.addStretch(1)

        scroll = QScrollArea()
        scroll.setWidgetResizable(True)
        scroll.setFrameShape(QScrollArea.Shape.NoFrame)
        scroll.setHorizontalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAlwaysOff)
        scroll.setVerticalScrollBarPolicy(
            Qt.ScrollBarPolicy.ScrollBarAsNeeded)
        scroll.setWidget(page)
        self._stack.addWidget(scroll)

    def _configure_control(self, page_key: str, item: dict,
                           control: SettingsControl):
        """Загружает значение, настраивает динамику и действие кнопки."""
        store = app_settings()
        key = item.get("key", "")
        ctype = item.get("type")

        if ctype == "combo" and key == "themes.current":
            options = self._theme_manager.get_theme_names()
            current = store.get_str(key) or self._current_theme
            control.set_options(options, current)
        elif ctype == "combo" and key == "lsp.python_path":
            from editor.python_env import (
                list_installed_pythons, python_version, selected_python,
            )
            entries = list_installed_pythons()
            current = store.get_str(key, "") or selected_python()["path"]
            values = [e["path"] for e in entries]
            if current and current not in values:
                entries.insert(0, {
                    "path": current,
                    "version": python_version(current) or "",
                })
            options = [
                f"Python {e['version']}  ({e['path']})" for e in entries
            ]
            values = [e["path"] for e in entries]
            control.set_data(options, values, current)
            # длинные пути: ширина ограничена, текст обрезается многоточием
            control.set_max_width(430)
        elif ctype == "combo" and key == "general.ui_font_family":
            from editor.font_settings import ui_font_families, ui_font_family
            families = ui_font_families()
            current = store.get_str(key, "") or ui_font_family()
            if current not in families:
                families.insert(0, current)
            control.set_data(families, families, current)
            control.set_max_width(430)
        elif ctype == "combo" and key == "editor.code_font_family":
            from editor.font_settings import mono_font_families, code_font_family
            families = mono_font_families()
            current = store.get_str(key, "") or code_font_family()
            if current not in families:
                families.insert(0, current)
            control.set_data(families, families, current)
            control.set_max_width(430)
        elif ctype == "combo" and key == "lsp.clangd.standard":
            from editor.lsp.config import cpp_standard_options
            options = [label for label, _ in cpp_standard_options()]
            values = [value for _, value in cpp_standard_options()]
            control.set_data(options, values, store.get_str(key, ""))
            control.set_max_width(160)
        elif ctype == "path":
            if isinstance(control, PathControl):
                control.set_theme(self._theme_manager, self._theme_name)
            control.set_value(store.get(key, ""))
        elif ctype == "shortcut":
            if isinstance(control, ShortcutControl):
                control.set_default(item.get("default", ""))
            control.set_value(store.get(key))
        elif ctype == "button":
            action = item.get("action")
            if action == ACTION_RESET_ALL:
                control.set_click_handler(self._reset_all)
            elif action == ACTION_RESTART_LSP:
                control.set_click_handler(self._restart_lsp)
        else:
            control.set_value(store.get(key))

        self._controls.append((key, control))

    # ------------------------------------------------------------------
    # Логика
    # ------------------------------------------------------------------
    def _collect(self) -> dict:
        """Значения всех контролов; тема сохраняется только если выбрана
        пользователем явно (иначе остаётся "" и не трогает сессию)."""
        out = {}
        for key, control in self._controls:
            if key == "themes.current":
                text = control.get_value()
                if not self._theme_was_set and text == self._current_theme:
                    continue
                out[key] = text
                continue
            value = control.get_value()
            if value is not None:
                out[key] = value
        return out

    def _save_and_apply(self):
        """Пишет значения в store, сохраняет файл и применяет к окну."""
        store = app_settings()
        store.set_many(self._collect())
        store.save()
        if self._window is not None:
            apply_settings(self._window)
            self._theme_name = self._window.current_theme
            self._current_theme = self._window.current_theme
            self._apply_dialog_theme()

    def _on_ok(self):
        self._save_and_apply()
        self.accept()

    def _reset_all(self):
        """Сбрасывает настройки к дефолтам и перезагружает контролы."""
        reply = themed_dialogs.question(
            self, "Reset Settings",
            "Reset all settings to defaults?",
            themed_dialogs.StandardButton.Yes
            | themed_dialogs.StandardButton.Cancel,
            themed_dialogs.StandardButton.Cancel,
            self._theme_manager, self._theme_name,
        )
        if reply != themed_dialogs.StandardButton.Yes:
            return
        store = app_settings()
        store.reset()
        store.save()
        self._theme_was_set = False
        for key, control in self._controls:
            control.set_value(store.get(key))
        # Темы-комбо возвращаем к текущей теме окна
        for key, control in self._controls:
            if key == "themes.current" and isinstance(control, ComboControl):
                control.set_options(self._theme_manager.get_theme_names(),
                                    self._current_theme)
                break
        if self._window is not None:
            apply_settings(self._window)

    def _restart_lsp(self):
        """Перезапускает LSP-сессии под текущими значениями полей."""
        if self._window is None:
            return
        store = app_settings()
        store.set_many(self._collect())
        store.save()
        apply_lsp_overrides()
        restart_lsp(self._window)

    def _refresh_theme_preview(self):
        """Перерисовывает превью темой, выбранной в комбо."""
        preview = getattr(self, "_theme_preview", None)
        if preview is None:
            return
        name = ""
        for key, control in self._controls:
            if key == "themes.current":
                name = control.get_value()
                break
        if not name:
            name = self._current_theme
        if name in self._theme_manager.themes:
            preview.set_theme(name)

    def _on_category_changed(self, current, _previous):
        if current is None:
            return
        key = current.data(0, Qt.ItemDataRole.UserRole)
        index = 0
        for page_key, _title, _items in iter_leaf_pages():
            if page_key == key:
                self._stack.setCurrentIndex(index)
                return
            index += 1

    # ------------------------------------------------------------------
    # Тема
    # ------------------------------------------------------------------
    def _apply_dialog_theme(self):
        tc = ThemeColors.from_theme_manager(
            self._theme_manager, self._theme_name)
        bg = tc.editor_bg_color
        fg = QColor(tc.fg)
        border = QColor(tc.border)
        muted = QColor(tc.muted)
        accent = QColor(tc.selection or tc.fg)
        sel = QColor(accent)
        sel.setAlpha(45)
        hover = QColor(fg)
        hover.setAlpha(30)
        press = QColor(fg)
        press.setAlpha(50)

        style_titlebar(self.titleBar, tc)

        # Тумблеры — кастомная отрисовка, красим напрямую
        track_off = bg.darker(125) if tc.dark else bg.darker(108)
        track_on = tc.accent_color
        knob = QColor(bg)
        for _key, control in self._controls:
            if isinstance(control, ToggleControl):
                control.set_colors(track_off, track_on, knob)
            elif isinstance(control, SpinControl):
                control.spin.set_plus_minus_colors(fg, border)
            elif isinstance(control, ComboControl):
                control.combo.set_arrow_color(fg)

        # Разделители строк — кастомная отрисовка в физических пикселях
        sep_color = QColor(border)
        sep_color.setAlpha(130)
        for i in range(self._stack.count()):
            page = self._stack.widget(i).widget()
            for sep in page.findChildren(SettingsSeparator):
                sep.set_line_color(sep_color)

        self.setStyleSheet(f"""
            SettingsDialog {{
                background-color: {bg.name()};
                font-family: "{ui_font_family()}";
                font-size: {UI_FONT_SIZE}pt;
            }}
            QLabel {{
                color: {muted.name()};
                background: transparent;
            }}
            QLabel#settingsName {{
                color: {fg.name()};
                font-size: {UI_FONT_SIZE + 1}pt;
                font-weight: 600;
                padding-left: 12px;
            }}
            QLabel#settingsHelp {{
                color: rgba({muted.red()},{muted.green()},{muted.blue()},170);
                font-size: 9pt;
                padding-left: 12px;
            }}
            QLabel#shortcutDefault {{
                color: rgba({muted.red()},{muted.green()},{muted.blue()},170);
                font-size: 9pt;
            }}
            QTreeWidget {{
                background-color: {bg.name()};
                color: {fg.name()};
                border: none;
                padding: 4px;
                outline: 0;
            }}
            QTreeWidget::branch {{
                background-color: {bg.name()};
            }}
            QTreeWidget::item {{
                height: 26px;
                border-radius: 5px;
                padding: 0 6px;
            }}
            QTreeWidget::item:hover {{
                background-color: {qss_color(hover)};
            }}
            QTreeWidget::item:selected {{
                background-color: {qss_color(sel)};
                color: {fg.name()};
            }}
            QFrame#treeSeparator {{
                background-color: {border.name()};
                border: none;
            }}
            QLineEdit, QPlainTextEdit, QSpinBox {{
                background-color: {bg.name()};
                color: {fg.name()};
                border: 0.5px solid {border.name()};
                border-radius: 4px;
                padding: 4px 6px;
                selection-background-color: {qss_color(sel)};
            }}
            QSpinBox {{
                padding-right: 40px;
            }}
            QLineEdit, QPlainTextEdit {{
                placeholder-text-color: rgba({muted.red()},{muted.green()},{muted.blue()},200);
            }}
            QLineEdit:focus, QPlainTextEdit:focus, QSpinBox:focus {{
                border-color: {accent.name()};
            }}
            QPlainTextEdit {{
                font-family: "{ui_font_family()}";
                font-size: 9pt;
            }}
            QComboBox {{
                background-color: {bg.name()};
                color: {fg.name()};
                border: 0.5px solid {border.name()};
                border-radius: 4px;
                padding: 4px 8px;
            }}
            QComboBox::drop-down {{
                subcontrol-origin: border;
                subcontrol-position: top right;
                width: 20px;
                border-top: 0.5px solid {border.name()};
                border-bottom: 0.5px solid {border.name()};
                border-left: 0.5px solid {border.name()};
                border-top-right-radius: 3px;
                border-bottom-right-radius: 3px;
                background-color: {qss_color(hover)};
            }}
            QComboBox::down-arrow {{
                width: 0;
                height: 0;
            }}
            QComboBox:focus {{
                border-color: {accent.name()};
            }}
            QComboBox QAbstractItemView {{
                background-color: {bg.name()};
                color: {fg.name()};
                border: 0.5px solid {border.name()};
                selection-background-color: {qss_color(sel)};
                outline: 0;
            }}
            QComboBox QAbstractItemView::item {{
                padding: 1px 8px;
                border: none;
                background-color: transparent;
                color: {fg.name()};
            }}
            QComboBox QAbstractItemView::item:hover {{
                background-color: {qss_color(hover)};
            }}
            QComboBox QAbstractItemView::item:selected {{
                background-color: {qss_color(sel)};
                color: {fg.name()};
            }}
            QRadioButton {{
                color: {fg.name()};
                spacing: 6px;
                background: transparent;
            }}
            QRadioButton::indicator {{
                width: 14px;
                height: 14px;
                border-radius: 7px;
                border: 0.5px solid {border.name()};
                background-color: {bg.name()};
            }}
            QRadioButton::indicator:hover {{
                border-color: {accent.name()};
            }}
            QRadioButton::indicator:checked {{
                background: qradialgradient(
                    cx: 0.5, cy: 0.5, radius: 0.5, fx: 0.5, fy: 0.5,
                    stop: 0 {fg.name()},
                    stop: 0.45 {fg.name()},
                    stop: 0.5 {bg.name()},
                    stop: 1 {bg.name()}
                );
            }}
            QPushButton, QToolButton {{
                color: {fg.name()};
                background-color: {bg.name()};
                border: 0.5px solid {border.name()};
                border-radius: 4px;
                padding: 4px 14px;
            }}
            QPushButton:hover, QToolButton:hover {{
                border-color: {accent.name()};
                background-color: {qss_color(hover)};
            }}
            QPushButton:pressed, QToolButton:pressed {{
                background-color: {qss_color(press)};
            }}
            QPushButton:default {{
                border-color: {accent.name()};
            }}
            QScrollArea {{
                background: transparent;
                border: none;
            }}
            QScrollArea > QWidget > QWidget {{
                background-color: transparent;
            }}
        """ + scrollbar_qss(bg.name(), fg))


def open_settings(window) -> None:
    """Модальный показ панели настроек (из MainWindow)."""
    dialog = SettingsDialog(window.theme_manager, window.current_theme,
                            window)
    dialog.exec()
    dialog.deleteLater()
