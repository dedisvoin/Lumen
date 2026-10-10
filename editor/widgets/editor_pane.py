"""
EditorPane — панель с QTabWidget, содержащая CodeEditor-ы.
Поддерживает перетаскивание вкладок и сплиттинг.
"""

import os
import json
from typing import Optional, List, Tuple, TYPE_CHECKING

from PyQt6 import sip

from PyQt6.QtCore import Qt, QEvent, QPoint, QRect

from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QTabWidget, QTabBar, QSplitter,
)

from editor.widgets.code_editor import CodeEditor
from editor.widgets.breadcrumbs import BreadcrumbsBar
from editor.widgets.diagnostics_widget import DiagnosticsWidget
from editor.widgets.symbols_widget import SymbolsWidget
from editor.widgets.tab_bar import DraggableTabBar
from editor.widgets.drop_overlay import DropOverlay
from editor.widgets.find_bar import FindBar
from editor.widgets.qt_utils import sync_tab_data
from editor.widgets.terminal import (
    TerminalWidget, find_terminal, take_terminal_from,
)
from editor.widgets.tab_theme import apply_tab_bar_theme
from editor.theme_utils import ThemeColors
from editor.theme_manager import ThemeManager
from editor.lexer_manager import LexerManager
from editor.constants import DEFAULT_THEME
from editor import themed_dialogs
from editor.settings_apply import apply_editor_defaults

if TYPE_CHECKING:
    from editor.tile_manager import TileManager


class _EditorHost(QWidget):
    """Контейнер вкладки редактора: крошки, поиск (сверху) + редактор.

    Панели встроены в layout, поэтому при открытии занимают место над
    редактором (под таб-баром), а не рисуются поверх.
    """

    def __init__(self, editor: CodeEditor, parent=None):
        super().__init__(parent)
        self.editor = editor
        self.find_bar = FindBar(editor, self)
        self.breadcrumbs = BreadcrumbsBar(editor, self)
        editor._breadcrumbs_bar = self.breadcrumbs
        layout = QVBoxLayout(self)
        layout.setContentsMargins(0, 0, 0, 0)
        layout.setSpacing(0)
        layout.addWidget(self.find_bar)
        layout.addWidget(self.breadcrumbs)
        layout.addWidget(editor, 1)
        self.find_bar.hide()

    def toggle_find_bar(self):
        if self.find_bar.isVisible():
            self.find_bar.close_bar()
        else:
            self.find_bar.open()

    def find_next(self):
        if not self.find_bar.isVisible():
            self.find_bar.open()
        self.find_bar.find_next()

    def find_previous(self):
        if not self.find_bar.isVisible():
            self.find_bar.open()
        self.find_bar.find_previous()


class EditorPane(QWidget):
    """Панель с вкладками редакторов."""

    def __init__(self, tile_manager: 'TileManager',
                 lexer_manager: LexerManager,
                 theme_manager: ThemeManager,
                 parent=None):
        super().__init__(parent)
        self.tile_manager = tile_manager
        self.lexer_manager = lexer_manager
        self.theme_manager = theme_manager
        self.setAcceptDrops(True)
        self.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground, False)

        self._drag_pos = QPoint()
        self._drag_active = False

        # Состояние тогла варнингов — живёт в панели, т.к. виджет вкладки
        # диагностик уничтожается при её закрытии
        self._diag_show_warnings = True

        self._setup_ui()

        self._overlay = DropOverlay(self)
        self._overlay.setGeometry(self.rect())

        self._install_event_filters(self)

    # --- init helpers ---

    def _install_event_filters(self, widget):
        widget.installEventFilter(self)
        for child in widget.children():
            if isinstance(child, QWidget):
                if child is not self._overlay:
                    self._install_event_filters(child)
                if isinstance(child, CodeEditor):
                    try:
                        viewport = child.viewport()
                        if viewport:
                            viewport.installEventFilter(self)
                            child.installEventFilter(self)
                    except Exception:
                        pass

    def _setup_ui(self):
        self._layout = QVBoxLayout(self)
        self._layout.setContentsMargins(0, 0, 0, 0)
        self.tabs = QTabWidget()
        self.tabs.setTabBar(DraggableTabBar())
        self.tabs.setTabsClosable(False)  # Кастомные кнопки в DraggableTabBar
        self.tabs.setMovable(True)
        # Подключаем сигнал напрямую от tabBar
        self.tabs.tabBar().tabCloseRequested.connect(self._close_tab)
        self._layout.addWidget(self.tabs)
        self.tabs.currentChanged.connect(self._on_current_changed)

    @property
    def current_theme(self) -> str:
        """Текущая тема (из tile_manager) — для свежесозданных вкладок."""
        return getattr(self.tile_manager, 'current_theme', DEFAULT_THEME)

    def _on_current_changed(self, index: int):
        if index >= 0:
            widget = self._unpack(self.tabs.widget(index))
            if isinstance(widget, CodeEditor):
                widget.setFocus()
            elif isinstance(widget, TerminalWidget):
                widget.focus_input()
            elif isinstance(widget, DiagnosticsWidget):
                # Активная панель определяется по фокусу (get_active_pane)
                widget.setFocus()
            elif isinstance(widget, SymbolsWidget):
                widget.setFocus()

    # --- поиск по файлу ---

    @staticmethod
    def _unpack(widget):
        """Достаёт редактор из контейнера вкладки (_EditorHost)."""
        return widget.editor if isinstance(widget, _EditorHost) else widget

    def _host_of(self, editor: CodeEditor) -> Optional[_EditorHost]:
        # Панель могла быть уничтожена (закрыта после переноса последней
        # вкладки), а textChanged редактора ещё зовёт сюда — не падаем
        if sip.isdeleted(self) or sip.isdeleted(self.tabs):
            return None
        for i in range(self.tabs.count()):
            widget = self.tabs.widget(i)
            if isinstance(widget, _EditorHost) and widget.editor is editor:
                return widget
        return None

    def current_editor(self) -> Optional[CodeEditor]:
        widget = self._unpack(self.tabs.currentWidget())
        return widget if isinstance(widget, CodeEditor) else None

    def _current_host(self) -> Optional[_EditorHost]:
        widget = self.tabs.currentWidget()
        return widget if isinstance(widget, _EditorHost) else None

    def toggle_find_bar(self):
        host = self._current_host()
        if host:
            host.toggle_find_bar()

    def find_next(self):
        host = self._current_host()
        if host:
            host.find_next()

    def find_previous(self):
        host = self._current_host()
        if host:
            host.find_previous()

    # --- diagnostics tab ---

    def _find_diagnostics_tab(self) -> Optional[int]:
        """Индекс вкладки диагностик в этой панели (или None)."""
        for i in range(self.tabs.count()):
            if isinstance(self._unpack(self.tabs.widget(i)),
                          DiagnosticsWidget):
                return i
        return None

    def has_diagnostics_tab(self) -> bool:
        """Есть ли вкладка диагностик в этой панели."""
        return self._find_diagnostics_tab() is not None

    def toggle_diagnostics_tab(self):
        """Ctrl+M: открыть список диагностик; повтор — закрыть где бы он ни был.

        Таб ищется по всем панелям дерева: если он активен — закрывается,
        если есть где-то ещё — активируется, иначе создаётся в этой панели.
        """
        found = []
        for pane in self.tile_manager.panes():
            idx = pane._find_diagnostics_tab()
            if idx is not None:
                found.append((pane, idx))
        if not found:
            self.add_diagnostics_tab()
            return
        # Таб открыт (текущая вкладка своей панели) — повторное нажатие закрывает
        for pane, i in found:
            if pane.tabs.currentIndex() == i:
                pane._close_tab(i)
                return
        # Таб существует, но не активен — показываем его в своей панели
        pane, i = found[0]
        pane.tabs.setCurrentWidget(pane.tabs.widget(i))

    # --- symbols tab ---

    def _find_symbols_tab(self) -> Optional[int]:
        """Индекс вкладки символов в этой панели (или None)."""
        for i in range(self.tabs.count()):
            if isinstance(self._unpack(self.tabs.widget(i)), SymbolsWidget):
                return i
        return None

    def has_symbols_tab(self) -> bool:
        """Есть ли вкладка символов в этой панели."""
        return self._find_symbols_tab() is not None

    def toggle_symbols_tab(self):
        """Ctrl+L: открыть список символов; повтор — закрыть где бы он ни был.

        Таб уникален на все панели дерева (как диагностики): если он активен —
        закрывается, если есть где-то ещё — активируется, иначе создаётся
        в этой панели.
        """
        found = []
        for pane in self.tile_manager.panes():
            idx = pane._find_symbols_tab()
            if idx is not None:
                found.append((pane, idx))
        if not found:
            self.add_symbols_tab()
            return
        # Таб открыт (текущая вкладка своей панели) — повторное нажатие закрывает
        for pane, i in found:
            if pane.tabs.currentIndex() == i:
                pane._close_tab(i)
                return
        # Таб существует, но не активен — показываем его в своей панели
        pane, i = found[0]
        pane.tabs.setCurrentWidget(pane.tabs.widget(i))

    # --- управление вкладками ---

    def _add_tab_widget(self, widget, title: str, tab_data: dict,
                        activate: bool = True):
        """Общая механика вкладки: addTab, tabData, тема, фокус.

        Единая точка для редактороподобных вкладок (терминал, диагностики,
        live-перенос) — раньше один и тот же набор шагов копировался
        в каждый метод добавления.
        """
        self.tabs.addTab(widget, title)
        idx = self.tabs.indexOf(widget)
        self.tabs.tabBar().setTabData(idx, tab_data)
        if activate:
            self.tabs.setCurrentWidget(widget)
        widget.apply_theme(self.current_theme)
        self.apply_pane_theme(self.current_theme)
        return widget

    def add_diagnostics_tab(self, activate: bool = True) -> DiagnosticsWidget:
        """Добавляет вкладку со списком диагностик всех открытых файлов."""
        widget = DiagnosticsWidget(self.tile_manager, self.theme_manager)
        widget.set_show_warnings(self._diag_show_warnings)
        widget.warn_visibility_changed.connect(self._set_diag_show_warnings)
        return self._add_tab_widget(widget, "Diagnostics",
                                    {"type": "diagnostics"},
                                    activate=activate)

    def add_symbols_tab(self, activate: bool = True) -> SymbolsWidget:
        """Добавляет вкладку со списком символов активного редактора."""
        widget = SymbolsWidget(self.tile_manager, self.theme_manager)
        return self._add_tab_widget(widget, "Symbols",
                                    {"type": "symbols"},
                                    activate=activate)

    def _set_diag_show_warnings(self, checked: bool):
        """Сохраняет состояние тогла варнингов между открытиями вкладки."""
        self._diag_show_warnings = checked

    # --- editor management ---

    def add_editor(self, file_path: Optional[str] = None,
                   content: str = "",
                   language_id: Optional[str] = None) -> CodeEditor:
        editor = CodeEditor(self.lexer_manager, self.theme_manager)
        editor.file_path = file_path
        if file_path:
            editor.load_file(file_path)
        else:
            editor.set_language(language_id)
            if content:
                editor.setText(content)
            else:
                editor.setText("")
        apply_editor_defaults(editor)
        editor.setModified(False)
        title = os.path.basename(file_path) if file_path else "Untitled"
        tab_data = {
            "file_path": file_path,
            "language_id": editor.language_id,
            "content": editor.text(),
        }
        editor = self._add_editor_tab(editor, tab_data, title)
        if file_path:
            from editor.bookmark_store import load_bookmarks
            saved = load_bookmarks(file_path)
            if saved:
                editor.restore_bookmarks(saved)
        return editor

    def _add_editor_tab(self, editor: CodeEditor, tab_data: dict,
                        title: str) -> CodeEditor:
        """Добавляет редактор вкладкой: контейнер, tabData, тема."""
        # tabData обновляется при старте драга (tab_bar.mouseMoveEvent) и
        # явными вызовами _sync_tab_data — без пер-кей-синхронизации
        editor.setAcceptDrops(True)
        editor.installEventFilter(self)
        try:
            viewport = editor.viewport()
            if viewport:
                viewport.installEventFilter(self)
        except Exception:
            pass

        host = _EditorHost(editor)
        self.tabs.addTab(host, title)
        tab_index = self.tabs.indexOf(host)
        self.tabs.tabBar().setTabData(tab_index, tab_data)
        self._sync_tab_data(editor)  # страховка: tabData гарантированно свежий
        self.tabs.setCurrentWidget(host)
        editor.apply_theme(self.current_theme)
        self.apply_pane_theme(self.current_theme)
        return editor

    def _sync_tab_data(self, editor: CodeEditor):
        """Обновляет tabData редактора (нужно для drag&drop и копирования)."""
        sync_tab_data(editor)

    # --- terminal tabs ---

    def add_terminal(self) -> TerminalWidget:
        """Добавляет вкладку терминала в эту панель."""
        return self._add_terminal_tab({"type": "terminal"})

    def _add_terminal_tab(self, data: dict) -> TerminalWidget:
        """Создаёт вкладку терминала из данных (drag&drop или сессия)."""
        terminal = TerminalWidget(self.theme_manager, cwd=data.get("cwd"))
        return self._add_terminal_tab_widget(terminal, data,
                                             terminal.title_text())

    def _add_terminal_tab_widget(self, terminal: TerminalWidget,
                                 data: dict, title: str) -> TerminalWidget:
        """Добавляет терминал вкладкой: tabData, тема, фокус."""
        tab_data = dict(data)
        tab_data["type"] = "terminal"
        tab_data["title"] = title
        return self._add_tab_widget(terminal, title, tab_data)

    def _accept_terminal_widget(self, terminal, data: dict) -> TerminalWidget:
        """Забирает существующий TerminalWidget из его текущего таб-виджета.

        Псевдоконсоль не пересоздаётся: виджет отсоединяется от старого
        хозяина и добавляется в эту панель (история, cwd, процесс живы).
        """
        title = terminal.title_text()
        take_terminal_from(terminal)
        return self._add_terminal_tab_widget(terminal, data, title)

    def _close_tab(self, index: int):
        host = self.tabs.widget(index)
        widget = self._unpack(host)
        tab_bar = self.tabs.tabBar()
        # При drag&drop-перемещении таба контент уже скопирован в новый таб,
        # поэтому диалог сохранения для исходного не показываем
        suppress_prompt = getattr(tab_bar, "_suppress_prompt", False)
        if (isinstance(widget, CodeEditor) and widget.isModified()
                and not suppress_prompt):
            reply = themed_dialogs.question(
                self, "Save Changes",
                f"Save changes to '{widget.file_path or 'Untitled'}'?",
                themed_dialogs.StandardButton.Yes
                | themed_dialogs.StandardButton.No
                | themed_dialogs.StandardButton.Cancel,
                themed_dialogs.StandardButton.Cancel,
                self.theme_manager, self.current_theme,
            )
            if reply == themed_dialogs.StandardButton.Cancel:
                return
            if reply == themed_dialogs.StandardButton.Yes:
                if not widget.save_file():
                    themed_dialogs.warning(
                        self, "Save Error", "Could not save file.",
                        self.theme_manager, self.current_theme)
                    return
        # Сохраняем закладки файла, чтобы они пережили закрытие вкладки
        # (при drag&drop-переносе таб не закрывается по-настоящему —
        # контент копируется в новый таб, поэтому пропускаем)
        if (not suppress_prompt
                and isinstance(widget, CodeEditor)
                and getattr(widget, "file_path", None)):
            from editor.bookmark_store import save_bookmarks
            save_bookmarks(widget.file_path, widget.get_bookmarks())
        # Терминал закрываем сразу, без подтверждения: процесс вместе
        # с деревом убивается taskkill /F /T
        kill = True
        self.tabs.removeTab(index)
        if isinstance(widget, TerminalWidget):
            widget.shutdown(kill=kill)
        host.deleteLater()
        # Панель опустела: закрываем пустую панель/окно или создаём Untitled
        if self.tabs.count() == 0:
            self.on_tab_removed()

    def on_tab_removed(self):
        """Разбирает опустевшую панель после удаления/переноса таба.

        Если есть другие панели — закрываем пустую. Иначе у «отдельного»
        окна (перетащенный таб, _close_when_pane_empty) закрываем всё окно,
        у главного окна создаём новый Untitled.
        """
        if self.tabs.count() > 0:
            return
        if self.tile_manager and len(self.tile_manager._panes) > 1:
            self.tile_manager.close_pane(self)
        elif getattr(self.window(), "_close_when_pane_empty", False):
            try:
                self.window().close()
            except RuntimeError:
                self.add_editor()
        else:
            self.add_editor()

    def retarget_tabs_with_path(self, old_path: str, new_path: str,
                                is_dir: bool = False):
        """Перенацеливает вкладки со старым путём на новый.

        Файл уже переименован на диске, поэтому вкладка не закрывается
        и не перезагружается: текст, курсор и несохранённые изменения
        остаются, меняется только путь и заголовок.
        """
        for idx in range(self.tabs.count()):
            widget = self._unpack(self.tabs.widget(idx))
            if not isinstance(widget, CodeEditor) or not widget.file_path:
                continue
            path = widget.file_path
            if path == old_path:
                target = new_path
            elif is_dir and path.startswith(old_path + os.sep):
                # Файл внутри переименованной папки
                target = os.path.join(
                    new_path, path[len(old_path) + 1:]
                )
            else:
                continue
            widget.file_path = target
            from editor.bookmark_store import retarget_bookmarks
            retarget_bookmarks(path, target)
            widget._watch_file(target)
            # Язык мог поменяться из-за нового расширения файла
            lang_id = self.lexer_manager.get_language_id(target)
            if lang_id != widget.language_id:
                widget.set_language(lang_id)
            title = os.path.basename(target)
            self.tabs.setTabText(idx, title)
            sync_tab_data(widget)

    def can_close_pane(self) -> bool:
        if self.tabs.count() == 0:
            return True
        if self.tile_manager and len(self.tile_manager._panes) > 1:
            return True
        return False

    # --- drag & drop ---

    def dragEnterEvent(self, event):
        mime = event.mimeData()
        if mime.hasFormat("application/x-editor-tab") or mime.hasUrls():
            self._drag_active = True
            self._drag_pos = event.position().toPoint()
            event.acceptProposedAction()

    def dragMoveEvent(self, event):
        if self._drag_active:
            self._drag_pos = event.position().toPoint()
            zone = self._get_drop_zone(self._drag_pos)
            self._overlay.set_indicator(zone)
            event.acceptProposedAction()

    def dragLeaveEvent(self, event):
        self._drag_active = False
        self._overlay.set_indicator(None)
        event.accept()

    def dropEvent(self, event):
        self._drag_active = False
        # self._drag_pos уже переведён в координаты панели через eventFilter,
        # а event.position() — в координатах дочернего виджета, поэтому
        # используем именно _drag_pos для определения зоны дропа
        zone = self._get_drop_zone(self._drag_pos)
        mime = event.mimeData()

        if mime.hasFormat("application/x-editor-tab"):
            data = json.loads(
                bytes(mime.data("application/x-editor-tab")).decode("utf-8")
            )
            if zone == "center":
                self._move_or_copy_tab(data, source=event.source())
            else:
                self.tile_manager.split_pane(self, zone, data=data,
                                             source=event.source())

        elif mime.hasUrls():
            for url in mime.urls():
                path = url.toLocalFile()
                if os.path.isfile(path):
                    if zone == "center":
                        self.add_editor(file_path=path)
                    else:
                        self.tile_manager.split_pane(self, zone, file_path=path)

        self._overlay.set_indicator(None)
        event.acceptProposedAction()

    def _get_drop_zone(self, pos: QPoint) -> Optional[str]:
        # Если курсор на tabBar — вставляем таб, а не создаём сплит
        tab_bar = self.tabs.tabBar()
        tab_bar_global = tab_bar.mapToGlobal(QPoint(0, 0))
        tab_bar_rect = QRect(tab_bar_global, tab_bar.size())
        if tab_bar_rect.contains(self.mapToGlobal(pos)):
            return "center"

        w, h = self.width(), self.height()
        x, y = pos.x(), pos.y()
        margin_x = w * 0.15
        margin_y = h * 0.15

        if x < margin_x:
            return "left"
        if x > w - margin_x:
            return "right"
        if y < margin_y:
            return "top"
        if y > h - margin_y:
            return "bottom"
        return "center"

    def _get_position_in_self(self, obj, event) -> QPoint:
        try:
            pos = event.position()
            if pos is not None:
                if obj != self:
                    global_pos = obj.mapToGlobal(pos.toPoint())
                    return self.mapFromGlobal(global_pos)
                return pos.toPoint()
        except Exception:
            pass
        return self._drag_pos

    def eventFilter(self, obj, event):
        event_type = event.type()

        if event_type == event.Type.DragEnter:
            try:
                mime_data = event.mimeData()
                if mime_data and (
                    mime_data.hasFormat("application/x-editor-tab")
                    or mime_data.hasUrls()
                ):
                    self._drag_active = True
                    self._drag_pos = self._get_position_in_self(obj, event)
                    event.acceptProposedAction()
                    return True
            except Exception:
                pass
            return False

        elif event_type == event.Type.DragMove:
            self._drag_active = True
            self._drag_pos = self._get_position_in_self(obj, event)
            zone = self._get_drop_zone(self._drag_pos)
            self._overlay.set_indicator(zone)
            event.acceptProposedAction()
            return True

        elif event_type == event.Type.DragLeave:
            self.dragLeaveEvent(event)
            return True

        elif event_type == event.Type.Drop:
            self._drag_pos = self._get_position_in_self(obj, event)
            self.dropEvent(event)
            return True

        return super().eventFilter(obj, event)

    # --- resize ---

    def resizeEvent(self, event):
        super().resizeEvent(event)
        self._overlay.setGeometry(self.rect())

    # --- move / copy tab ---

    def _take_live_host(self, data: dict, source) -> Optional[_EditorHost]:
        """Отцепляет живой _EditorHost от исходного таб-виджета.

        Аналог take_terminal_from для редакторов: панель поиска, курсор,
        выделение и undo-история сохраняются. Возвращает None, если вкладку
        забрать нельзя (source неизвестен или вкладка уже закрыта/заменена).
        """
        if not isinstance(source, QTabBar):
            return None
        tab_widget = source.parent()
        if not isinstance(tab_widget, QTabWidget):
            return None
        idx = data.get("drag_index", -1)
        if not (0 <= idx < tab_widget.count()):
            return None
        widget = tab_widget.widget(idx)
        if not isinstance(widget, _EditorHost):
            return None
        editor = widget.editor
        # Проверяем, что вкладка не успела поменяться с начала драга
        if (editor.file_path != data.get("file_path")
                or editor.language_id != data.get("language_id")):
            return None
        tab_widget.removeTab(idx)
        bar = tab_widget.tabBar()
        if bar is not None:
            bar._terminal_handed_off = True
        return widget

    def _add_host_tab(self, host: _EditorHost, data: dict) -> CodeEditor:
        """Добавляет уже существующий _EditorHost вкладкой (live-перенос)."""
        editor = host.editor
        # Перепривязываем к этой панели: tabData должен оставаться свежим,
        # а drag&drop на редакторе — работать (event filter предыдущей панели
        # исчез вместе с ней или уже установлен на этот же объект)
        editor.setAcceptDrops(True)
        editor.installEventFilter(self)
        try:
            viewport = editor.viewport()
            if viewport:
                viewport.installEventFilter(self)
        except Exception:
            pass
        title = data.get("title") or (
            os.path.basename(editor.file_path)
            if editor.file_path else "Untitled"
        )
        tab_data = dict(data)
        tab_data.pop("drag_index", None)
        self.tabs.addTab(host, title)
        tab_index = self.tabs.indexOf(host)
        self.tabs.tabBar().setTabData(tab_index, tab_data)
        self.tabs.setCurrentWidget(host)
        editor.apply_theme(self.current_theme)
        self.apply_pane_theme(self.current_theme)
        # Редактор перенесён в другое окно: кэшированная плашка закладки —
        # дочернее ToolTip-окно, привязанное к нативному окну-источнику.
        # После закрытия того окна его нативный хэндл мёртв и show() ничего
        # не рисует. Сбрасываем кэш, чтобы плашка пересоздалась в текущем
        # окне при следующем наведении.
        tip = getattr(editor, "_bookmark_tooltip", None)
        if tip is not None:
            try:
                tip.deleteLater()
            except Exception:
                pass
            editor._bookmark_tooltip = None
        return editor

    def _take_diagnostics_widget(self, data: dict, source) -> Optional[DiagnosticsWidget]:
        """Отцепляет живой DiagnosticsWidget от исходного таб-виджета.

        Аналог _take_live_host для эфемерной вкладки диагностик: виджет
        (и его таймер автообновления) переносится целиком.
        """
        if not isinstance(source, QTabBar):
            return None
        tab_widget = source.parent()
        if not isinstance(tab_widget, QTabWidget):
            return None
        idx = data.get("drag_index", -1)
        if not (0 <= idx < tab_widget.count()):
            return None
        widget = tab_widget.widget(idx)
        if not isinstance(widget, DiagnosticsWidget):
            return None
        # Вкладка могла закрыться/пересоздаться за время drag.exec()
        # (вложенный цикл событий выполняет deleteLater) — не отцепляем
        # чужую вкладку с тем же индексом
        if data.get("widget_id") is not None and id(widget) != data["widget_id"]:
            return None
        tab_widget.removeTab(idx)
        bar = tab_widget.tabBar()
        if bar is not None:
            bar._terminal_handed_off = True
        return widget

    def _add_diagnostics_widget(self, widget: DiagnosticsWidget) -> DiagnosticsWidget:
        """Добавляет живой DiagnosticsWidget вкладкой (live-перенос)."""
        return self._add_tab_widget(widget, "Diagnostics",
                                    {"type": "diagnostics"})

    def _take_symbols_widget(self, data: dict, source) -> Optional[SymbolsWidget]:
        """Отцепляет живой SymbolsWidget от исходного таб-виджета.

        Аналог _take_diagnostics_widget для вкладки символов: виджет (и его
        таймер автообновления) переносится целиком.
        """
        if not isinstance(source, QTabBar):
            return None
        tab_widget = source.parent()
        if not isinstance(tab_widget, QTabWidget):
            return None
        idx = data.get("drag_index", -1)
        if not (0 <= idx < tab_widget.count()):
            return None
        widget = tab_widget.widget(idx)
        if not isinstance(widget, SymbolsWidget):
            return None
        if data.get("widget_id") is not None and id(widget) != data["widget_id"]:
            return None
        tab_widget.removeTab(idx)
        bar = tab_widget.tabBar()
        if bar is not None:
            bar._terminal_handed_off = True
        return widget

    def _add_symbols_widget(self, widget: SymbolsWidget) -> SymbolsWidget:
        """Добавляет живой SymbolsWidget вкладкой (live-перенос)."""
        return self._add_tab_widget(widget, "Symbols",
                                    {"type": "symbols"})

    def _move_or_copy_tab(self, data: dict, source=None):
        if data.get("type") == "terminal":
            terminal = find_terminal(data.get("terminal_id"))
            if terminal is not None:
                # Переносим живой виджет вместе с сессией ConPTY
                return self._accept_terminal_widget(terminal, data)
            # Реестр не нашёл (например, виджет уже уничтожен) — fallback
            self._add_terminal_tab(data)
            return

        if data.get("type") == "diagnostics":
            # Эфемерная вкладка: переносим живой виджет целиком, иначе
            # пересоздание дало бы пустой редактор
            widget = self._take_diagnostics_widget(data, source)
            if widget is not None:
                return self._add_diagnostics_widget(widget)
            return

        if data.get("type") == "symbols":
            widget = self._take_symbols_widget(data, source)
            if widget is not None:
                return self._add_symbols_widget(widget)
            return

        # Переносим живой контейнер целиком (панель поиска, курсор и
        # undo-история сохраняются), как терминалы. Fallback — пересоздание.
        host = self._take_live_host(data, source)
        if host is not None:
            return self._add_host_tab(host, data)

        editor = CodeEditor(self.lexer_manager, self.theme_manager)
        # Используем контент из data (снимок делается при старте драга в
        # tab_bar), а не перечитываем файл: иначе теряются несохранённые
        # изменения при перемещении таба.
        editor.setText(data.get("content", ""))
        editor.file_path = data.get("file_path")
        lang = data.get("language_id")
        if editor.file_path and not lang:
            lang = editor.lexer_manager.get_language_id(editor.file_path)
        editor.language_id = lang
        editor.set_language(lang)
        editor.setModified(False)
        title = (os.path.basename(editor.file_path)
                 if editor.file_path else "Untitled")
        self._add_editor_tab(editor, data, title)

    # --- theme ---

    def _pane_borders(self) -> Tuple[bool, bool, bool, bool]:
        """Какие стороны панели рисуют рамку (left, top, right, bottom).

        Асимметричное правило для горизонтального стыка: линию стыка рисует
        правая панель своей левой стороной (она всегда включена), поэтому
        правая выключается при соседе справа. Верхняя и нижняя стороны
        рисуются всегда: top — линия под таб-баром, bottom — край панели.
        """
        borders = [True, True, True, True]
        w = self
        while True:
            parent = w.parent()
            if not isinstance(parent, QSplitter):
                break
            idx = parent.indexOf(w)
            count = parent.count()
            if parent.orientation() == Qt.Orientation.Horizontal:
                if idx < count - 1:
                    borders[2] = False      # есть сосед справа — стык рисует он
            w = parent
        return tuple(borders)

    def _has_neighbor_above(self) -> bool:
        """Есть ли панель сверху в вертикальном сплите (для линии табов)."""
        w = self
        while True:
            parent = w.parent()
            if not isinstance(parent, QSplitter):
                return False
            if (parent.orientation() == Qt.Orientation.Vertical
                    and parent.indexOf(w) > 0):
                return True
            w = parent
        return False

    def _set_tabs_qss(self, tc):
        """QSS области вкладок: рамка только с открытых сторон панели."""
        l, t, r, b = self._pane_borders()
        sides = [
            ("left", l), ("top", t), ("right", r), ("bottom", b),
        ]
        # 0px (а не none): Qt QSS не понимает none как ширину стороны и
        # сбрасывает всё правило рамки целиком
        pane_qss = "; ".join(
            (f"border-{side}: 1px solid {tc.border}"
             if show else f"border-{side}: 0px solid {tc.border}")
            for side, show in sides
        )
        self.tabs.setStyleSheet(f"""
            QTabWidget {{
                background: {tc.bg};
            }}
            QTabWidget::pane {{
                {pane_qss};
                background: transparent;
                margin-top: -1px;
            }}
        """)

    def refresh_borders(self):
        """Пересчитывает рамки после изменения дерева панелей (split/close)."""
        tc = ThemeColors.from_theme_manager(self.theme_manager,
                                            self.current_theme)
        # Верхняя линия табов зависит от позиции: у панели с соседом сверху
        # её даёт рамка нижнего края верхней панели (иначе дубль)
        apply_tab_bar_theme(self.tabs.tabBar(), tc,
                            tab_top_border=not self._has_neighbor_above())
        self._set_tabs_qss(tc)

    def apply_pane_theme(self, theme_name: str):
        """Применяет тему к таб-бару панели, оверлею и терминалам."""
        tc = ThemeColors.from_theme_manager(self.theme_manager, theme_name)
        apply_tab_bar_theme(self.tabs.tabBar(), tc,
                            tab_top_border=not self._has_neighbor_above())
        # Локальный QSS панели: рамка + прозрачный фон области вкладок.
        # Раньше жил в глобальном QSS главного окна — каждый setStyleSheet
        # окна переполировал ВСЁ дерево виджетов (лавина перерисовок).
        self._set_tabs_qss(tc)
        self._overlay.set_fg_color(tc.fg_color)
        for i in range(self.tabs.count()):
            widget = self.tabs.widget(i)
            if isinstance(widget, _EditorHost):
                widget.find_bar.apply_theme(theme_name)
            elif isinstance(widget, TerminalWidget):
                widget.apply_theme(theme_name)
            elif isinstance(widget, DiagnosticsWidget):
                widget.apply_theme(theme_name)
            elif isinstance(widget, SymbolsWidget):
                widget.apply_theme(theme_name)

    def activate_editor(self, editor: CodeEditor) -> bool:
        """Активирует вкладку с редактором; False — редактора нет в панели."""
        host = self._host_of(editor)
        if host is None:
            return False
        self.tabs.setCurrentWidget(host)
        return True

    # --- accessors ---

    def get_editor(self, index: int) -> Optional[CodeEditor]:
        return self._unpack(self.tabs.widget(index)) or None

    def get_all_editors(self) -> List[CodeEditor]:
        editors = []
        for i in range(self.tabs.count()):
            widget = self._unpack(self.tabs.widget(i))
            if isinstance(widget, CodeEditor):
                editors.append(widget)
        return editors
