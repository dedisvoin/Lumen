"""
TileManager — управляет сплиттингом панелей редакторов.
"""

import os
from typing import Optional, List

from PyQt6.QtCore import Qt, QEvent, QObject
from PyQt6.QtGui import QColor, QPalette, QPainter, QPen
from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QSplitter, QRubberBand, QApplication,
)

from editor.lexer_manager import LexerManager
from editor.theme_manager import ThemeManager
from editor.theme_utils import ThemeColors
from editor.constants import DEFAULT_THEME, CODE_FONT_SIZE
from editor.widgets.editor_pane import EditorPane
from editor.widgets.code_editor import CodeEditor
from editor.widgets.terminal import TerminalWidget
from editor.widgets.diagnostics_widget import DiagnosticsWidget
from editor.widgets.symbols_widget import SymbolsWidget
from editor.lsp.manager import get_lsp_manager
from editor.scintilla_constants import (
    SCI_GETCURRENTPOS, SCI_INSERTTEXT, SCI_DELETERANGE, SCI_EMPTYUNDOBUFFER,
)


class _RubberBandThemer(QObject):
    """Красит QRubberBand (полоска при перетаскивании сплиттера) в акцент
    темы. Палитра/QSS на неё не действуют: Windows-стиль рисует её
    собственным синим цветом, поэтому перехватываем Paint события."""

    def __init__(self):
        super().__init__()
        self._color = QColor("#888888")
        self._installed = False

    def set_accent(self, color: QColor):
        self._color = QColor(color)
        app = QApplication.instance()
        if app is not None and not self._installed:
            app.installEventFilter(self)
            self._installed = True

    def eventFilter(self, obj, event):
        if event.type() == QEvent.Type.Paint and isinstance(obj, QRubberBand):
            self._paint_band(obj)
            return True
        return super().eventFilter(obj, event)

    def _paint_band(self, band: QRubberBand):
        painter = QPainter(band)
        rect = band.rect()
        c = self._color
        if band.shape() == QRubberBand.Shape.Line:
            painter.setPen(QPen(c, 5))
            if rect.width() > rect.height():
                painter.drawLine(rect.left(), rect.center().y(),
                                 rect.right(), rect.center().y())
            else:
                painter.drawLine(rect.center().x(), rect.top(),
                                 rect.center().x(), rect.bottom())
        else:
            painter.fillRect(rect, QColor(c.red(), c.green(), c.blue(), 120))
        painter.end()


_rubber_band_themer = _RubberBandThemer()


class TileManager:
    """Менеджер панелей — сплиттинг, закрытие, применение темы."""

    def __init__(self, root_widget: QWidget,
                 lexer_manager: LexerManager,
                 theme_manager: ThemeManager):
        self.root = root_widget
        self.lexer_manager = lexer_manager
        self.theme_manager = theme_manager
        self.layout = QVBoxLayout(self.root)
        self.layout.setContentsMargins(0, 0, 0, 0)

        self._main_splitter = QSplitter(Qt.Orientation.Horizontal)
        self._main_splitter.setHandleWidth(0)
        self._main_splitter.setChildrenCollapsible(False)
        self._main_splitter.setOpaqueResize(True)
        self.layout.addWidget(self._main_splitter)
        self._panes: List[EditorPane] = []
        self.active_editor: Optional[CodeEditor] = None
        self.current_theme = DEFAULT_THEME

        initial_pane = EditorPane(self, lexer_manager, theme_manager)
        self._main_splitter.addWidget(initial_pane)
        self._panes.append(initial_pane)
        initial_pane.add_editor()

    def _is_pane_alive(self, pane: EditorPane) -> bool:
        """Проверяет, жив ли объект панели (защита от RuntimeError)."""
        if pane is None:
            return False
        try:
            tabs = pane.tabs
            if tabs is None:
                return False
            tabs.count()
            return True
        except (RuntimeError, AttributeError):
            return False

    def _clean_dead_panes(self):
        """Удаляет из _panes панели, которые уже были удалены."""
        alive = []
        for pane in self._panes:
            if self._is_pane_alive(pane):
                alive.append(pane)
        self._panes = alive

    def get_active_pane(self) -> Optional[EditorPane]:
        """Возвращает активную панель или None.

        Панель под фокусом — то, куда пользователь смотрит сейчас (включая
        вкладки терминала/диагностик). Раньше полагались только на
        active_editor, который после переноса таба/клика по не-редакторной
        вкладке мог указывать на панель без фокуса.
        """
        self._clean_dead_panes()
        focus = QApplication.focusWidget()
        if focus is not None:
            parent = focus
            while parent is not None and not isinstance(parent, EditorPane):
                parent = parent.parent()
            if isinstance(parent, EditorPane) and self._is_pane_alive(parent):
                return parent
        if self.active_editor:
            try:
                parent = self.active_editor.parent()
                while parent and not isinstance(parent, EditorPane):
                    parent = parent.parent()
                if isinstance(parent, EditorPane) and self._is_pane_alive(parent):
                    return parent
            except RuntimeError:
                self.active_editor = None
        for pane in self._panes:
            if self._is_pane_alive(pane):
                return pane
        return None

    def panes(self) -> List[EditorPane]:
        """Все живые панели (сбор диагностик, обход вкладок)."""
        self._clean_dead_panes()
        return list(self._panes)

    def _refresh_pane_borders(self):
        """Пересчитывает рамки панелей после split/close/restore.

        У смежных панелей на стыке не должно быть двух линий: рамка
        рисуется только с открытых сторон (см. EditorPane._pane_borders).
        """
        for pane in self._panes:
            try:
                if self._is_pane_alive(pane):
                    pane.refresh_borders()
            except RuntimeError:
                continue

    def split_pane(self, pane: EditorPane, direction: str,
                   file_path: Optional[str] = None,
                   data: Optional[dict] = None,
                   source=None):
        """Разделяет панель в указанном направлении."""
        self._clean_dead_panes()
        if not self._is_pane_alive(pane):
            return None

        parent = pane.parent()
        if parent is None:
            return None

        # Если родитель не сплиттер, создаём новый
        if not isinstance(parent, QSplitter):
            orientation = (
                Qt.Orientation.Horizontal
                if direction in ('left', 'right')
                else Qt.Orientation.Vertical
            )
            new_splitter = QSplitter(orientation)
            new_splitter.setHandleWidth(0)
            new_splitter.setChildrenCollapsible(False)
            new_splitter.setOpaqueResize(True)

            if parent is self._main_splitter:
                idx = self._main_splitter.indexOf(pane)
                if idx >= 0:
                    self._main_splitter.replaceWidget(idx, new_splitter)
            else:
                grandparent = parent.parent()
                if grandparent and isinstance(grandparent, QSplitter):
                    idx = grandparent.indexOf(parent)
                    grandparent.replaceWidget(idx, new_splitter)
                else:
                    self._main_splitter.addWidget(new_splitter)

            new_splitter.addWidget(pane)
            parent = new_splitter

        parent_orientation = parent.orientation()
        target_orientation = (
            Qt.Orientation.Horizontal
            if direction in ('left', 'right')
            else Qt.Orientation.Vertical
        )

        new_pane = EditorPane(self, self.lexer_manager, self.theme_manager)

        if parent_orientation == target_orientation:
            index = parent.indexOf(pane)
            if direction in ('left', 'top'):
                parent.insertWidget(index, new_pane)
            else:
                parent.insertWidget(index + 1, new_pane)
            self._equalize_splitter_sizes(parent)
        else:
            new_splitter = QSplitter(target_orientation)
            new_splitter.setHandleWidth(0)
            new_splitter.setChildrenCollapsible(False)
            new_splitter.setOpaqueResize(True)
            idx = parent.indexOf(pane)
            parent.replaceWidget(idx, new_splitter)

            if direction in ('left', 'top'):
                new_splitter.insertWidget(0, new_pane)
                new_splitter.addWidget(pane)
            else:
                new_splitter.addWidget(pane)
                new_splitter.addWidget(new_pane)

            self._equalize_splitter_sizes(new_splitter)
            parent = new_splitter

        self._panes.append(new_pane)

        if file_path:
            new_pane.add_editor(file_path=file_path)
        elif data:
            new_pane._move_or_copy_tab(data, source)
        else:
            new_pane.add_editor()

        self._remove_empty_splitters(self._main_splitter)
        self._update_splitter_sizes()
        self._refresh_pane_borders()
        return new_pane

    def _equalize_splitter_sizes(self, splitter: QSplitter):
        try:
            count = splitter.count()
            if count > 0:
                total = (
                    splitter.width()
                    if splitter.orientation() == Qt.Orientation.Horizontal
                    else splitter.height()
                )
                if total == 0:
                    total = 400
                size = total // count
                splitter.setSizes([size] * count)
        except RuntimeError:
            pass

    def _update_splitter_sizes(self):
        def update_recursive(w):
            try:
                if isinstance(w, QSplitter):
                    self._equalize_splitter_sizes(w)
                    for i in range(w.count()):
                        child = w.widget(i)
                        if child:
                            update_recursive(child)
            except RuntimeError:
                pass

        try:
            update_recursive(self._main_splitter)
        except RuntimeError:
            pass

    def _remove_empty_splitters(self, widget):
        """
        Рекурсивно удаляет все сплиттеры, в которых 0 или 1 виджет.
        Корневой сплиттер (self._main_splitter) НЕ УДАЛЯЕТСЯ НИКОГДА.
        """
        try:
            if not isinstance(widget, QSplitter):
                return

            # Сначала обрабатываем детей (снизу вверх)
            for i in range(widget.count() - 1, -1, -1):
                child = widget.widget(i)
                if child:
                    self._remove_empty_splitters(child)

            # Пропускаем корневой сплиттер — его не удаляем никогда
            if widget is self._main_splitter:
                return

            # Теперь смотрим на текущий сплиттер (только НЕ корневой)
            if widget.count() <= 1:
                child = widget.widget(0) if widget.count() == 1 else None
                parent = widget.parent()

                if isinstance(parent, QSplitter):
                    if child is not None:
                        idx = parent.indexOf(widget)
                        parent.replaceWidget(idx, child)
                        widget.deleteLater()
                    else:
                        # Пустой сплиттер — удаляем его
                        widget.deleteLater()
                else:
                    widget.deleteLater()
        except RuntimeError:
            pass

    def close_pane(self, pane: EditorPane) -> bool:
        """Закрывает панель (удаляет её и очищает сплиттеры).

        Возвращает False, если панель не закрыта (последняя в дереве
        или уже уничтожена). Диалоги сохранения — обязанность вызывающего
        (UI-слой), здесь только структура дерева и LSP-сессии.
        """
        self._clean_dead_panes()
        if not self._is_pane_alive(pane):
            return False

        if len(self._panes) <= 1:
            return False

        # Сбрасываем активный редактор, если он принадлежит закрываемой панели
        if self.active_editor is not None:
            try:
                p = self.active_editor
                while p is not None and p is not pane:
                    p = p.parent()
                if p is pane:
                    self.active_editor = None
            except RuntimeError:
                self.active_editor = None

        # Удаляем панель из списка
        if pane in self._panes:
            self._panes.remove(pane)

        # Отцепляем панель от дерева синхронно: deleteLater удалит её только
        # в следующем цикле событий, и без setParent(None) она ещё числится
        # соседом при пересчёте рамок (нижняя панель не получит линию табов)
        pane.setParent(None)
        pane.deleteLater()

        # Удаляем пустые сплиттеры
        self._remove_empty_splitters(self._main_splitter)
        self._update_splitter_sizes()

        self._clean_dead_panes()

        # Документы закрытой панели отвязаны через destroyed-цепочку bridge,
        # но сессии без открытых документов завершаем явно (как в restore)
        try:
            get_lsp_manager().shutdown_unused_sessions()
        except RuntimeError:
            pass

        # Переключаем активный редактор на оставшуюся панель
        if self.active_editor is None:
            for p in self._panes:
                try:
                    if self._is_pane_alive(p):
                        ed = p.current_editor()
                        if ed is not None:
                            self.active_editor = ed
                            break
                except RuntimeError:
                    continue
        self._refresh_pane_borders()
        return True

    def apply_theme_to_all(self, theme_name: str):
        """Единая точка применения темы к панелям."""
        self.current_theme = theme_name
        tc = ThemeColors.from_theme_manager(self.theme_manager, theme_name)
        accent = QColor(tc.selection) if tc.selection else QColor(tc.fg)
        _rubber_band_themer.set_accent(accent)

        try:
            self._theme_splitter_palette(self._main_splitter, accent)
        except RuntimeError:
            pass

        self._clean_dead_panes()
        for pane in self._panes:
            try:
                if self._is_pane_alive(pane):
                    for editor in pane.get_all_editors():
                        editor.apply_theme(theme_name)
                    pane.apply_pane_theme(theme_name)
            except RuntimeError:
                continue

    def _theme_splitter_palette(self, splitter: QSplitter, accent: QColor):
        try:
            pal = splitter.palette()
            pal.setColor(QPalette.ColorRole.WindowText, accent)
            pal.setColor(QPalette.ColorRole.Base, accent)
            splitter.setPalette(pal)
            for i in range(splitter.count()):
                child = splitter.widget(i)
                if child and isinstance(child, QSplitter):
                    self._theme_splitter_palette(child, accent)
        except RuntimeError:
            pass

    # --- сессия: сериализация/восстановление дерева панелей ---

    def serialize(self) -> dict:
        """Сериализует дерево сплиттеров и панелей в словарь сессии.

        Размеры НЕ выравниваем перед записью (раньше _update_splitter_sizes
        делил всё поровну, и пользовательские пропорции не попадали
        в сессию); чистим только мёртвые сплиттеры.
        """
        self._clean_dead_panes()
        try:
            self._remove_empty_splitters(self._main_splitter)
        except RuntimeError:
            pass

        def walk(w):
            try:
                if isinstance(w, QSplitter):
                    children = []
                    for i in range(w.count()):
                        child = w.widget(i)
                        if child:
                            child_data = walk(child)
                            if child_data is not None:
                                children.append(child_data)
                    if not children:
                        return None
                    return {
                        "type": "split",
                        "orientation": (
                            "h" if w.orientation() == Qt.Orientation.Horizontal
                            else "v"
                        ),
                        "sizes": [int(s) for s in w.sizes()],
                        "children": children,
                    }
                if isinstance(w, EditorPane):
                    if not self._is_pane_alive(w):
                        return None
                    tabs, active, n = [], 0, 0
                    cur = w.tabs.currentIndex()
                    for i in range(w.tabs.count()):
                        widget = w._unpack(w.tabs.widget(i))
                        if isinstance(widget, CodeEditor):
                            if i == cur:
                                active = n
                            line, idx = widget.getCursorPosition()
                            content = (
                                widget.text()
                                if (widget.isModified() or not widget.file_path)
                                else None
                            )
                            tabs.append({
                                "type": "editor",
                                "file_path": widget.file_path,
                                "content": content,
                                "cursor": [line, idx],
                                "scroll": widget.verticalScrollBar().value(),
                                "zoom": widget._font_size - CODE_FONT_SIZE,
                                "bookmarks": widget.get_bookmarks()
                                if hasattr(widget, "get_bookmarks") else [],
                            })
                        elif isinstance(widget, TerminalWidget):
                            if i == cur:
                                active = n
                            tabs.append({
                                "type": "terminal",
                                "cwd": widget._cwd,
                                "font": widget._font_size,
                            })
                        elif isinstance(widget, DiagnosticsWidget):
                            if i == cur:
                                active = n
                            tabs.append({"type": "diagnostics"})
                        elif isinstance(widget, SymbolsWidget):
                            if i == cur:
                                active = n
                            tabs.append({"type": "symbols"})
                        else:
                            continue
                        n += 1
                    if not tabs:
                        return None
                    return {"type": "pane", "active": active, "tabs": tabs}
                return None
            except RuntimeError:
                return None

        return walk(self._main_splitter)

    def restore(self, tree: Optional[dict]):
        """Восстанавливает дерево панелей из сессии."""
        old_panes = self._panes
        self._panes = []
        self.active_editor = None

        new_splitter = QSplitter(Qt.Orientation.Horizontal)
        new_splitter.setHandleWidth(0)
        new_splitter.setChildrenCollapsible(False)
        new_splitter.setOpaqueResize(True)

        self._build_widget(tree, new_splitter)

        if not self._panes:
            pane = EditorPane(self, self.lexer_manager, self.theme_manager)
            pane.add_editor()
            self._panes.append(pane)
            new_splitter.addWidget(pane)

        # Отвязываем старые панели от LSP
        manager = get_lsp_manager()
        for pane in old_panes:
            try:
                if self._is_pane_alive(pane):
                    for editor in pane.get_all_editors():
                        manager.unregister_editor(editor)
            except RuntimeError:
                continue
        manager.shutdown_unused_sessions()

        # Замена корневого сплиттера
        self.layout.removeWidget(self._main_splitter)
        self._main_splitter.deleteLater()
        self._main_splitter = new_splitter
        self.layout.addWidget(self._main_splitter)

        try:
            self._remove_empty_splitters(self._main_splitter)
        except RuntimeError:
            pass
        self._refresh_pane_borders()
        # Размеры не выравниваем: сохранённые sizes уже применены
        # в _build_widget, а финально их применит main._apply_session_sizes
        # после показа окна (до показа Qt не знает реальных размеров)

    def _build_widget(self, data: Optional[dict], parent_splitter: QSplitter):
        if not isinstance(data, dict):
            return

        wtype = data.get("type")
        if wtype == "split":
            children_data = data.get("children", [])
            if not children_data:
                return

            orientation = (
                Qt.Orientation.Horizontal
                if data.get("orientation") == "h"
                else Qt.Orientation.Vertical
            )
            sp = QSplitter(orientation)
            sp.setHandleWidth(0)
            sp.setChildrenCollapsible(False)
            sp.setOpaqueResize(True)

            for child in children_data:
                self._build_widget(child, sp)

            if sp.count() == 0:
                return

            sizes = data.get("sizes")
            if sizes and len(sizes) == sp.count():
                sizes = [int(s) for s in sizes]
                sp.setSizes(sizes)
                sp.setProperty("session_sizes", sizes)

            parent_splitter.addWidget(sp)

        elif wtype == "pane":
            pane = EditorPane(self, self.lexer_manager, self.theme_manager)
            parent_splitter.addWidget(pane)
            self._panes.append(pane)

            active = data.get("active", 0)
            j = 0
            for tab in data.get("tabs", []):
                tab_type = tab.get("type")
                if tab_type == "diagnostics":
                    pane.add_diagnostics_tab(activate=(j == active))
                    j += 1
                    continue
                if tab_type == "symbols":
                    pane.add_symbols_tab(activate=(j == active))
                    j += 1
                    continue
                if tab_type == "terminal":
                    terminal = pane._add_terminal_tab(tab)
                    font = tab.get("font")
                    if font:
                        terminal.set_font_size(int(font))
                    if j == active:
                        pane.tabs.setCurrentWidget(terminal)
                    j += 1
                    continue
                editor = _restore_tab(pane, tab, self.lexer_manager)
                if editor is None:
                    continue
                host = pane._host_of(editor)
                if host is not None and j == active:
                    pane.tabs.setCurrentWidget(host)
                j += 1

            if pane.tabs.count() == 0:
                pane.add_editor()


def _restore_tab(pane: EditorPane, tab: dict,
                 lexer_manager: LexerManager) -> Optional[CodeEditor]:
    """Восстанавливает одну вкладку-редактор из данных сессии."""
    path = tab.get("file_path")
    content = tab.get("content")
    if path and os.path.isfile(path) and content is None:
        editor = pane.add_editor(file_path=path)
    elif content is not None:
        lang_id = lexer_manager.get_language_id(path) if path else None
        editor = pane.add_editor(file_path=None, content=content,
                                 language_id=lang_id)
        if path:
            editor.file_path = path
            editor._watch_file(path)
            idx = pane.tabs.indexOf(pane._host_of(editor))
            pane.tabs.setTabText(idx, os.path.basename(path))
            pane._sync_tab_data(editor)
            _mark_modified(editor)
        elif content:
            _mark_modified(editor)
    else:
        return None

    cursor = tab.get("cursor")
    if cursor:
        editor.setCursorPosition(int(cursor[0]), int(cursor[1]))
    scroll = tab.get("scroll")
    if scroll:
        editor.verticalScrollBar().setValue(int(scroll))
    zoom = tab.get("zoom")
    if zoom:
        editor.zoomTo(int(zoom))
    bookmarks = tab.get("bookmarks")
    if bookmarks:
        editor.restore_bookmarks(bookmarks)
    return editor


def _mark_modified(editor: CodeEditor):
    """Помечает документ изменённым (пустая вставка+удаление + чистка undo)."""
    pos = editor.SendScintilla(SCI_GETCURRENTPOS)
    editor.SendScintilla(SCI_INSERTTEXT, pos, b" ")
    editor.SendScintilla(SCI_DELETERANGE, pos, 1)
    editor.SendScintilla(SCI_EMPTYUNDOBUFFER)
