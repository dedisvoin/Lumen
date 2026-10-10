"""
DetachedWindow — отдельное frameless-окно редактора.

Когда таб перетаскивают за пределы окна, под курсором открывается новое
окно с собственным TileManager: в него можно перетаскивать табы из других
окон обычным drag&drop, сплитить панели и т.д. У окна нет меню-бара —
в тайтл-баре только кнопки закрыть и максимизировать. Шорткаты поиска
по файлу/папке и работы с тасками дублируют главное окно.
"""

import ctypes
import os

from PyQt6.QtGui import QAction, QColor, QPalette
from PyQt6.QtWidgets import QApplication, QVBoxLayout, QWidget

from qframelesswindow import FramelessWindow, StandardTitleBar

from editor.constants import TITLE_FONT_SIZE
from editor.file_finder import FileFindPopup
from editor.bookmark_finder import BookmarkFindPopup, collect_bookmarks
from editor.font_settings import ui_font_family
from editor.spotlight import SpotlightController
from editor.tasks import TasksController
from editor.theme_utils import ThemeColors, apply_narrow_caption_buttons
from editor.tile_manager import TileManager
from editor.widgets.terminal import TerminalWidget

# Реестр открытых окон-«деточек»: главное окно обновляет им тему и
# закрывает их при своём закрытии.
_detached_windows = []


def all_detached_windows():
    """Все живые окна-деточки (главное окно синхронизирует с ними тему)."""
    alive = []
    for w in _detached_windows:
        try:
            w.tile_manager.panes()
            alive.append(w)
        except RuntimeError:
            continue
    return alive


class DetachedWindow(FramelessWindow):
    """Окно для «оторванного» таба: свой TileManager, только кнопки
    закрыть/максимизировать в тайтл-баре, без меню."""

    def __init__(self, theme_manager, lexer_manager, theme_name: str,
                 title: str = "Lumen"):
        super().__init__()
        self.theme_manager = theme_manager
        self.lexer_manager = lexer_manager
        self.current_theme = theme_name
        # Пустая единственная панель закрывает всё окно (а не создаёт
        # Untitled, как главное окно). См. EditorPane.on_tab_removed.
        self._close_when_pane_empty = True

        self.setObjectName("detachedWindow")
        # Название — то же, что у главного окна (передаёт _create_detach_window)
        self.setWindowTitle(title)
        self.setMinimumSize(320, 220)

        # Тайтл-бар: только закрыть и максимизировать (без меню и кнопки
        # свернуть)
        standard_tb = StandardTitleBar(self)
        standard_tb.titleLabel.setText(title)
        standard_tb.minBtn.hide()
        self.setTitleBar(standard_tb)
        apply_narrow_caption_buttons(standard_tb)
        # Как в главном окне: убираем отступ и иконку по умолчанию, иначе
        # заголовок уезжает вправо (спейсер 10px + iconLabel 20px)
        if standard_tb.hBoxLayout.count() > 0:
            item = standard_tb.hBoxLayout.itemAt(0)
            if item and item.spacerItem():
                standard_tb.hBoxLayout.removeItem(item)
        if hasattr(standard_tb, "iconLabel"):
            standard_tb.iconLabel.hide()

        self._enable_dwm_rounded_corners()

        central = QWidget()
        self.tile_manager = TileManager(central, self.lexer_manager,
                                        self.theme_manager)

        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, self.titleBar.height(), 0, 0)
        layout.setSpacing(0)
        layout.addWidget(central)

        # Таски: у окна свой TasksController (работает с его панелями
        # и терминалами), а хранилище тасков общее для всего приложения
        self._tasks = TasksController(self)
        self._file_finder_popup = None
        self._register_shortcuts()

        # Зенитный спотлайт (см. spotlight.py): настройка view.spotlight
        # читается из хранилища — деточка создаётся уже после применения
        self.spotlight = SpotlightController(
            self.tile_manager, self.theme_manager, parent=self)
        try:
            from editor.settings_store import app_settings
            _store = app_settings()
            self.spotlight.set_enabled(
                _store.get_bool("view.spotlight", False))
            self.spotlight.set_alphas(
                _store.get_int("view.spotlight_dark_alpha", 230),
                _store.get_int("view.spotlight_light_alpha", 190))
            self.spotlight.set_skip(
                _store.get_bool("view.spotlight_skip_terminal", False),
                _store.get_bool("view.spotlight_skip_diagnostics", False))
        except Exception:
            pass

        self.apply_theme(theme_name)

        _detached_windows.append(self)
        self.destroyed.connect(self._on_destroyed)

    # ---- шорткаты: поиск по файлу/папке и таски ----

    def _register_shortcuts(self):
        """Дублирует шорткаты главного окна (см. menu_builder.build)."""
        from editor.shortcuts import bind_shortcut
        for shortcut_id, slot in (
            ("file.save", self._save_file),
            ("bookmarks.find", self._open_bookmark_finder),
            ("view.find", self._find_in_file),
            ("view.find_next", self._find_next_in_file),
            ("view.find_previous", self._find_prev_in_file),
            ("view.find_file", self._open_file_finder),
            ("run.task_create", self._tasks.create),
            ("run.task_run", self._tasks.run_list),
            ("run.task_last", self._tasks.run_last),
        ):
            action = QAction(self)
            bind_shortcut(action, shortcut_id)
            action.triggered.connect(slot)
            self.addAction(action)

    # ---- поиск по файлу (Ctrl+F / F3 / Shift+F3) ----

    def _find_in_file(self):
        pane = self.tile_manager.get_active_pane()
        if pane:
            pane.toggle_find_bar()

    def _find_next_in_file(self):
        pane = self.tile_manager.get_active_pane()
        if pane:
            pane.find_next()

    def _find_prev_in_file(self):
        pane = self.tile_manager.get_active_pane()
        if pane:
            pane.find_previous()

    # ---- поиск файла по всей папке (Ctrl+Shift+F) ----

    def _workspace_root(self) -> str:
        """Корень поиска: папка, открытая в главном окне, иначе папка
        активного файла."""
        for tl in QApplication.topLevelWidgets():
            ft = getattr(tl, "file_tree", None)
            if ft is not None:
                root = ft.root_path
                if root:
                    return root
        editor = self._get_current_editor()
        if editor and editor.file_path:
            d = os.path.dirname(editor.file_path)
            if d:
                return d
        return ""

    def _open_file_finder(self):
        """Ctrl+Shift+F: поиск файлов в открытой папке."""
        root = self._workspace_root()
        if not root:
            return
        popup = FileFindPopup(
            root, self.theme_manager, self.current_theme,
            self._open_finder_file, self,
        )
        popup.adjustSize()
        geo = self.geometry()
        popup.move(geo.center().x() - popup.width() // 2, geo.top() + 100)
        popup.show()
        self._file_finder_popup = popup  # держим ссылку, пока попап открыт

    def _open_finder_file(self, path: str):
        """Открывает выбранный в поиске файл и передаёт ему фокус."""
        pane = self.tile_manager.get_active_pane()
        if pane:
            pane.add_editor(file_path=path)
        editor = self._get_current_editor()
        if editor is not None:
            editor.setFocus()

    def _open_bookmark_finder(self):
        """Ctrl+Shift+B: поиск по всем закладкам проекта."""
        entries = collect_bookmarks()
        popup = BookmarkFindPopup(
            entries, self.theme_manager, self.current_theme,
            self._open_bookmark, self,
        )
        popup.adjustSize()
        geo = self.geometry()
        popup.move(geo.center().x() - popup.width() // 2, geo.top() + 100)
        popup.show()
        self._bookmark_finder_popup = popup  # держим ссылку, пока попап открыт

    def _open_bookmark(self, path: str, line: int):
        """Переходит к закладке: открывает файл (если закрыт) и прыгает
        на строку с показом плашки-пояснения."""
        pane = self.tile_manager.get_active_pane()
        if pane:
            pane.add_editor(file_path=path)
        editor = self._get_current_editor()
        if editor is None:
            return
        editor.setCursorPosition(line, 0)
        try:
            editor.ensureLineVisible(line)
        except Exception:
            pass
        editor.setFocus()
        note = ""
        try:
            note = editor._bookmark_note_at_line(line) or ""
        except Exception:
            note = ""
        if note and hasattr(editor, "_show_bookmark_tooltip"):
            try:
                editor._show_bookmark_tooltip(note, line)
            except Exception:
                pass

    def _get_current_editor(self):
        """Активный редактор окна (как MainWindow._get_current_editor)."""
        ae = self.tile_manager.active_editor
        if ae is not None:
            try:
                ae.objectName()
                return ae
            except RuntimeError:
                self.tile_manager.active_editor = None
        pane = self.tile_manager.get_active_pane()
        if pane:
            current = pane.current_editor()
            if current is not None:
                return current
        return None

    def _update_tab_title(self, editor):
        """Обновляет заголовок вкладки и tabData после сохранения (как в
        главном окне)."""
        tab_widget = editor.find_tab_widget()
        if tab_widget is not None:
            for i in range(tab_widget.count()):
                w = tab_widget.widget(i)
                if getattr(w, "editor", None) is editor:
                    tab_widget.setTabText(
                        i, os.path.basename(editor.file_path))
                    break
        from editor.widgets.qt_utils import sync_tab_data
        sync_tab_data(editor, include_content=False)

    def _save_file(self):
        """Ctrl+S: сохраняет активный редактор окна."""
        editor = self._get_current_editor()
        if editor is None:
            return
        if editor.file_path:
            if editor.save_file():
                self._update_tab_title(editor)
        else:
            self._save_as()

    def _save_as(self):
        from editor import themed_dialogs
        editor = self._get_current_editor()
        if editor is None:
            return
        path = themed_dialogs.save_file_name(
            self, "Save As", "", self.theme_manager, self.current_theme)
        if path:
            if editor.save_file(path):
                new_lang = editor.lexer_manager.get_language_id(path)
                if new_lang != editor.language_id:
                    cur_line, cur_idx = editor.getCursorPosition()
                    editor.set_language(new_lang)
                    editor.setCursorPosition(cur_line, cur_idx)
                self._update_tab_title(editor)

    # ---- тема ----

    def apply_theme(self, theme_name: str):
        self.current_theme = theme_name
        tc = ThemeColors.from_theme_manager(self.theme_manager, theme_name)
        self.tile_manager.apply_theme_to_all(theme_name)
        self.spotlight.set_theme(theme_name)
        self._style_titlebar(tc)
        pal = self.palette()
        pal.setColor(QPalette.ColorRole.Window, QColor(tc.bg))
        self.setPalette(pal)

    def _style_titlebar(self, tc):
        tb = self.titleBar
        tb.setStyleSheet(f"StandardTitleBar {{ background-color: {tc.title_bg or tc.bg}; }}")
        tb.titleLabel.setStyleSheet(f"""
            QLabel {{
                color: {tc.fg};
                font-family: '{ui_font_family()}';
                font-size: {TITLE_FONT_SIZE}pt;
                background: transparent;
                border: none;
                padding-left: 7px;
            }}
        """)
        for btn in (tb.maxBtn, tb.closeBtn):
            btn.setNormalColor(QColor(tc.fg))
            btn.setHoverColor(QColor(tc.fg))
            btn.setPressedColor(QColor(tc.fg))
        hover_bg = QColor(tc.fg)
        hover_bg.setAlpha(30)
        tb.maxBtn.setHoverBackgroundColor(hover_bg)
        tb.closeBtn.setHoverBackgroundColor(QColor(232, 17, 35))

    # ---- жизнь окна ----

    def _has_tabs(self) -> bool:
        """Есть ли хоть одна вкладка в окне (для автозакрытия пустого)."""
        try:
            for pane in self.tile_manager.panes():
                if pane.tabs.count() > 0:
                    return True
        except RuntimeError:
            return False
        return False

    def _shutdown_terminals(self):
        for pane in self.tile_manager._panes:
            try:
                for i in range(pane.tabs.count()):
                    widget = pane.tabs.widget(i)
                    if isinstance(widget, TerminalWidget):
                        widget.shutdown()
            except RuntimeError:
                pass

    def closeEvent(self, event):
        # Попап поиска файлов: закрываем до выхода, чтобы фоновое
        # сканирование остановилось (см. MainWindow.closeEvent)
        popup = self._file_finder_popup
        if popup is not None:
            try:
                popup.close()
            except RuntimeError:
                pass
        try:
            self._tasks._cancel_task()
        except (RuntimeError, AttributeError):
            pass
        self._shutdown_terminals()
        # Выписку из реестра делаем здесь: destroyed может не наступить,
        # пока Python держит ссылку на закрытое окно
        try:
            if self in _detached_windows:
                _detached_windows.remove(self)
        except RuntimeError:
            pass
        super().closeEvent(event)

    def _on_destroyed(self):
        if self in _detached_windows:
            _detached_windows.remove(self)

    # ---- DWM ----

    def _enable_dwm_rounded_corners(self):
        """DWM: скруглённые углы окна (Windows 11+)."""
        try:
            hwnd = int(self.winId())
            c_value = ctypes.c_int(2)
            ctypes.windll.dwmapi.DwmSetWindowAttribute(
                hwnd, 33, ctypes.byref(c_value), ctypes.sizeof(c_value)
            )
        except Exception:
            pass
