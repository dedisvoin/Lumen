"""
FileTreeWidget — боковая панель с деревом файлов.

Стилизуется темами как и остальной интерфейс: фон панели, цвет текста,
цвет линий дерева, выделение. Иконки папок/файлов берутся из editor/files_icons
(SVG) и перекрашиваются в цвета текущей темы.

Операции файловой системы (создание/переименование/удаление, drop путей,
поиск и перестроение узлов) вынесены в FileTreeOps (file_tree_ops.py).
"""

import os
from typing import Optional, List, Set

from PyQt6.QtCore import (
    Qt, QPoint, QPointF, QUrl, pyqtSignal,
)
from PyQt6.QtGui import QColor, QIcon, QPainter, QPen
from PyQt6.QtWidgets import (
    QWidget, QVBoxLayout, QTreeWidget, QTreeWidgetItem,
    QLabel, QMenu, QApplication, QSizePolicy,
    QAbstractItemView,
)

from editor.constants import (
    UI_FONT_SIZE,
    FALLBACK_BORDER, FALLBACK_FG,
    SIDEBAR_DARKEN,
    TAB_PADDING_PANE,
    DEFAULT_THEME,
)
from editor.font_settings import ui_font_family
from editor.scrollbar_qss import scrollbar_qss
from editor.scrollbar_fade import ScrollBarAutoHide
from editor.theme_manager import ThemeManager
from editor.theme_utils import ThemeColors, qss_color
from editor import themed_dialogs
from editor.fs_watcher import SharedFsWatcher
from editor.widgets.file_icons import file_icon_for_path, load_svg_icon
from editor.widgets.file_tree_ops import FileTreeOps, SKIP_DIRS


class FileTreeView(QTreeWidget):
    """QTreeWidget с темизируемыми линиями дерева и drag&drop файлов."""

    drop_paths = pyqtSignal(list)
    # Инлайн-редактор узла закрыт (Enter / Esc / потеря фокуса)
    edit_finished = pyqtSignal()

    def __init__(self, parent=None):
        super().__init__(parent)
        self._guide_color = QColor(FALLBACK_BORDER)
        self.setHeaderHidden(True)
        self.setColumnCount(1)
        self.setIndentation(18)
        self.setUniformRowHeights(True)
        self.setTextElideMode(Qt.TextElideMode.ElideMiddle)
        self.setExpandsOnDoubleClick(False)
        # Корневой элемент — вплотную к левому краю (без своей колонки
        # раскрытия), чтобы «|» уровней начинались с самого края, как в
        # текстовом виде дерева: super_folder / |  sub / |  |  file
        self.setRootIsDecorated(False)
        self.setDragEnabled(True)
        self.setDragDropMode(QAbstractItemView.DragDropMode.DragOnly)
        self.setAcceptDrops(True)
        # Инлайн-редактор запускаем только программно (Rename...):
        # двойной клик занят открытием файла, а не редактированием
        self.setEditTriggers(QAbstractItemView.EditTrigger.NoEditTriggers)

    # ---- тема ----

    def set_theme_colors(self, guide: QColor):
        self._guide_color = QColor(guide)
        self.viewport().update()

    def closeEditor(self, editor, hint):
        """Редактор узла закрыт — сообщаем панели (снять флаг режима)."""
        super().closeEditor(editor, hint)
        self.edit_finished.emit()

    # ---- drag & drop наружу (в панели редакторов) ----

    def mimeData(self, items):
        md = super().mimeData(items)
        urls = []
        for it in items:
            path = it.data(0, Qt.ItemDataRole.UserRole)
            if path:
                urls.append(QUrl.fromLocalFile(path))
        if urls:
            md.setUrls(urls)
        return md

    # ---- drag & drop внутрь (файлы/папки из проводника) ----

    def dragEnterEvent(self, event):
        if event.mimeData().hasUrls():
            event.acceptProposedAction()
        else:
            super().dragEnterEvent(event)

    def dragMoveEvent(self, event):
        if event.mimeData().hasUrls():
            event.acceptProposedAction()
        else:
            super().dragMoveEvent(event)

    def dropEvent(self, event):
        if event.mimeData().hasUrls():
            paths = [
                u.toLocalFile() for u in event.mimeData().urls()
                if u.isLocalFile()
            ]
            paths = [p for p in paths if p]
            if paths:
                self.drop_paths.emit(paths)
            event.acceptProposedAction()
        else:
            super().dropEvent(event)

    # ---- отрисовка линий дерева ----

    def drawBranches(self, painter, rect, index):
        """Far/Norton-стиль: только вертикальные «|» по уровням, без стрелок.

        Каждый элемент рисует «|» только на своей строке: по одному отрезку
        на каждый уровень предков. Смежные строки сшиваются в непрерывную
        линию, но каждая строка закрашивается ровно один раз — иначе
        полупрозрачные штрихи наслаиваются и «разгораются».
        """
        painter.save()
        painter.setRenderHint(QPainter.RenderHint.Antialiasing)

        indent = self.indentation()
        item = self.itemFromIndex(index)
        if item is None:
            painter.restore()
            return

        # Цепочка предков (без невидимого корня)
        ancestors = []
        cur = item.parent() if item else None
        while cur is not None and cur is not self.invisibleRootItem():
            ancestors.insert(0, cur)
            cur = cur.parent()

        top = rect.top()
        bottom = top + rect.height()
        base_x = rect.left()

        pen = QPen(self._guide_color, 1)
        pen.setCapStyle(Qt.PenCapStyle.FlatCap)
        painter.setPen(pen)

        for i in range(len(ancestors)):
            # Линия у правого края колонки уровня: имя идёт сразу за ней
            # (как в Far/Norton — 2 символа между «|» и именем)
            x = base_x + indent * (i + 1) - 2.0
            painter.drawLine(QPointF(x, top), QPointF(x, bottom))

        painter.restore()


class FileTreeWidget(QWidget):
    """Боковая панель «Дерево файлов» с кнопками и темизацией."""

    open_file_requested = pyqtSignal(str)
    folder_opened = pyqtSignal(str)
    # Файл/папка переименованы на диске: (старый путь, новый путь)
    file_renamed = pyqtSignal(str, str)

    def __init__(self, theme_manager: ThemeManager, parent=None):
        super().__init__(parent)
        self.theme_manager = theme_manager
        self._root_path: Optional[str] = None
        self._theme_name = DEFAULT_THEME
        # QSS контекстного меню (попапы не наследуют стиль главного окна)
        self._menu_qss: str = ""

        # Иконки из files_icons (пересоздаются при применении темы)
        self._icon_fg = QColor(FALLBACK_FG)
        self._folder_close_icon = self._icon("folder_close", self._icon_fg)
        self._folder_open_icon = self._icon("folder_open", self._icon_fg)

        self.setObjectName("fileTreePanel")
        # Без WA_StyledBackground QWidget не рисует фон/рамку из QSS
        self.setAttribute(Qt.WidgetAttribute.WA_StyledBackground, True)
        layout = QVBoxLayout(self)
        # Нижний отступ 1px — чтобы горизонтальный скроллбар дерева не
        # перекрывал нижнюю обводку панели (border-bottom #fileTreePanel)
        layout.setContentsMargins(0, 0, 0, 1)
        layout.setSpacing(0)

        # Путь к открытой папке
        self._path_label = QLabel("No folder")
        self._path_label.setObjectName("fileTreePath")
        self._path_label.setToolTip("")
        # Горизонтальная политика Ignored: ширина текста пути не должна
        # задавать минимальную ширину панели — сплиттер можно сузить как угодно
        self._path_label.setSizePolicy(
            QSizePolicy.Policy.Ignored, QSizePolicy.Policy.Fixed
        )
        layout.addWidget(self._path_label)

        # Пути раскрытых папок — восстанавливаются при перезаполнении
        self._expanded_paths: Set[str] = set()
        # Слежение за файловой системой: новые/удалённые файлы и папки
        # сразу появляются в дереве без ручного обновления
        self._fs_watcher = SharedFsWatcher(self)
        self._fs_watcher.directoryChanged.connect(self._on_fs_changed)

        # Операции файловой системы (создание/переименование/удаление)
        self._ops = FileTreeOps(self)

        # Дерево
        self.tree = FileTreeView()
        # Прячем бегунок при простое с затуханием — как в редакторе кода
        self._sb_ctl = ScrollBarAutoHide(self.tree, self)
        self.tree.itemActivated.connect(self._on_item_activated)
        self.tree.itemExpanded.connect(self._on_item_expanded)
        self.tree.itemCollapsed.connect(self._on_item_collapsed)
        self.tree.setContextMenuPolicy(
            Qt.ContextMenuPolicy.CustomContextMenu
        )
        self.tree.customContextMenuRequested.connect(self._on_context_menu)
        self.tree.drop_paths.connect(self._on_drop_paths)
        # Инлайн-переименование: editItem + коммит через itemChanged
        self.tree.itemChanged.connect(self._on_item_changed)
        self.tree.edit_finished.connect(self._on_edit_finished)
        layout.addWidget(self.tree, 1)

    # ---- доступ ----

    @property
    def root_path(self) -> Optional[str]:
        return self._root_path

    # ---- управление деревом ----

    def choose_folder(self):
        start = self._root_path or ""
        path = themed_dialogs.existing_directory(
            self, "Open Folder", start,
            self.theme_manager, self._theme_name,
        )
        if path:
            self.set_root_path(path)

    def set_root_path(self, path: str):
        path = os.path.abspath(path)
        if not os.path.isdir(path):
            return
        if path != self._root_path:
            self._expanded_paths.clear()
        self._root_path = path
        self._set_path_label(path)
        self.tree.clear()
        # Перестаём следить за старой папкой, следим за новой
        dirs = self._fs_watcher.directories()
        if dirs:
            self._fs_watcher.removePaths(dirs)
        self._fs_watcher.addPath(path)

        self._ops.build_root_item(path)
        self.folder_opened.emit(path)

    def refresh(self):
        if self._root_path:
            self.set_root_path(self._root_path)

    def collapse_all(self):
        self.tree.collapseAll()
        if self.tree.topLevelItemCount() > 0:
            self.tree.topLevelItem(0).setExpanded(True)

    # ---- наполнение ----

    def _populate(self, item: QTreeWidgetItem, dirpath: str):
        item.takeChildren()
        try:
            entries = sorted(
                os.scandir(dirpath),
                key=lambda e: (not e.is_dir(), e.name.lower()),
            )
        except OSError:
            return
        for entry in entries:
            name = entry.name
            if name.startswith(".") or name in SKIP_DIRS:
                continue
            child = QTreeWidgetItem([name])
            child.setData(0, Qt.ItemDataRole.UserRole, entry.path)
            if entry.is_dir():
                child.setIcon(0, self._folder_close_icon)
                # Заглушка: папка раскрываемая; реальное наполнение — при раскрытии
                child.addChild(QTreeWidgetItem(["…"]))
                # Следим за папкой: изменения в ней обновят дерево
                self._fs_watcher.addPath(entry.path)
            else:
                child.setIcon(0, self._file_icon(entry.path))
            item.addChild(child)
        # Папки, раскрытые до пересоздания узла, раскрываем снова
        self._restore_expanded(item)

    def _on_item_expanded(self, item: QTreeWidgetItem):
        path = item.data(0, Qt.ItemDataRole.UserRole)
        if not path:
            return
        if os.path.isdir(path):
            self._expanded_paths.add(path)
            item.setIcon(0, self._folder_open_icon)
            # Ещё не загружена: есть только dummy-заглушка
            if (item.childCount() == 1
                    and item.child(0).data(0, Qt.ItemDataRole.UserRole) is None):
                self._populate(item, path)

    def _on_item_collapsed(self, item: QTreeWidgetItem):
        path = item.data(0, Qt.ItemDataRole.UserRole)
        if path and os.path.isdir(path):
            self._expanded_paths.discard(path)
            item.setIcon(0, self._folder_close_icon)

    # ---- слежение за файловой системой ----

    def _on_fs_changed(self, path: str):
        """В папке появились/исчезли файлы — обновляем затронутый узел.

        Событие приходит на папку, где что-то изменилось. Если папка ещё
        загружена в дерево — перечитываем только её, состояние раскрытых
        папок при этом сохраняется.
        """
        if not self._root_path:
            return
        item = self._ops.find_item_by_path(path)
        if item is None:
            # Неизвестный путь (корень пересоздан) — перестраиваем всё дерево
            self.refresh()
            return
        if os.path.isdir(path):
            self._populate(item, path)
        else:
            # Папка/файл удалены или переименованы на диске — читаем родителя
            parent = item.parent()
            if parent is None:
                self.refresh()
                return
            parent_path = parent.data(0, Qt.ItemDataRole.UserRole)
            if parent_path and os.path.isdir(parent_path):
                self._populate(parent, parent_path)

    def _create_in_dir(self, item: QTreeWidgetItem, folder: str,
                       is_dir: bool):
        """Создаёт файл/папку в folder и перечитывает узел дерева."""
        created = (self._ops.create_dir(folder) if is_dir
                   else self._ops.create_file(folder))
        if created:
            self._populate(item, folder)

    def _restore_expanded(self, item: QTreeWidgetItem):
        """Раскрывает подпапки, раскрытые до пересоздания узла.

        У папки-заглушки setExpanded(True) запускает _on_item_expanded,
        который лениво загрузит её содержимое и восстановит вложенные
        раскрытые папки.
        """
        for i in range(item.childCount()):
            child = item.child(i)
            path = child.data(0, Qt.ItemDataRole.UserRole)
            if path and path in self._expanded_paths:
                child.setExpanded(True)

    # ---- события ----

    def _on_item_activated(self, item: QTreeWidgetItem, column: int):
        path = item.data(0, Qt.ItemDataRole.UserRole)
        if not path:
            return
        if os.path.isdir(path):
            item.setExpanded(not item.isExpanded())
        elif os.path.isfile(path):
            self.open_file_requested.emit(path)

    def _on_context_menu(self, pos: QPoint):
        item = self.tree.itemAt(pos)
        menu = QMenu(self)
        # QSS задаём напрямую: попапы не наследуют стиль главного окна
        if self._menu_qss:
            menu.setStyleSheet(self._menu_qss)
        # Скруглённые углы: прозрачный фон окна, чтобы углы не заливались
        menu.setAttribute(Qt.WidgetAttribute.WA_TranslucentBackground)
        # Windows: exec() может вернуть пункт под курсором даже когда меню
        # закрыто кликом мимо (по редактору) — и тогда «само» начнётся
        # переименование. Поэтому срабатываем только по явному выбору пункта:
        # сигнал triggered, а не возврат exec(). Меню удаляем при закрытии.
        menu.setAttribute(Qt.WidgetAttribute.WA_DeleteOnClose)

        if item is not None:
            path = item.data(0, Qt.ItemDataRole.UserRole)
            is_dir = bool(path) and os.path.isdir(path)
            is_root = item.parent() is None

            open_action = menu.addAction("Open")
            copy_action = menu.addAction("Copy Path")
            if is_dir:
                menu.addSeparator()
                new_file_action = menu.addAction("New File...")
                new_folder_action = menu.addAction("New Folder...")
            menu.addSeparator()
            rename_action = menu.addAction("Rename...")
            if not is_root:
                delete_action = menu.addAction("Delete")

            open_action.triggered.connect(
                lambda: self._on_item_activated(item, 0))
            copy_action.triggered.connect(
                lambda: QApplication.clipboard().setText(path))
            if is_dir:
                new_file_action.triggered.connect(
                    lambda: self._create_in_dir(item, path, is_dir=False))
                new_folder_action.triggered.connect(
                    lambda: self._create_in_dir(item, path, is_dir=True))
            rename_action.triggered.connect(
                lambda: self._ops.rename_item(item, path))
            if not is_root:
                delete_action.triggered.connect(
                    lambda: self._ops.delete_item(item, path))
        else:
            # Пустое место — создаём файлы/папки в корне открытой папки
            if not self._root_path:
                return
            new_file_action = menu.addAction("New File...")
            new_folder_action = menu.addAction("New Folder...")
            root_item = self.tree.topLevelItem(0)
            if root_item:
                new_file_action.triggered.connect(
                    lambda: self._create_in_dir(root_item, self._root_path,
                                                is_dir=False))
                new_folder_action.triggered.connect(
                    lambda: self._create_in_dir(root_item, self._root_path,
                                                is_dir=True))

        menu.exec(self.tree.viewport().mapToGlobal(pos))

    # ---- создание / переименование / удаление (делегаты в FileTreeOps) ----

    def _on_item_changed(self, item: QTreeWidgetItem, column: int):
        """Коммит инлайн-редактора — передаём в операции FS."""
        self._ops.on_item_changed(item, column)

    def _on_edit_finished(self):
        """Редактор узла закрыт — выходим из режима переименования."""
        self._ops.on_edit_finished()

    def _on_drop_paths(self, paths: List[str]):
        self._ops.drop_paths(paths)

    def _set_path_label(self, path: str):
        self._path_label.setToolTip(path)
        fm = self._path_label.fontMetrics()
        width = max(60, self._path_label.width() - 12)
        self._path_label.setText(
            fm.elidedText(path, Qt.TextElideMode.ElideMiddle, width)
        )

    def resizeEvent(self, event):
        super().resizeEvent(event)
        # if self._root_path:
        #     self._set_path_label(self._root_path)

    # ---- иконки ----

    def _icon(self, name: str, color: QColor) -> QIcon:
        """Кэшированная SVG-иконка из files_icons в заданном цвете."""
        return load_svg_icon(name, color)

    def _file_icon(self, path: str) -> QIcon:
        """Иконка файла по расширению (в цвете темы)."""
        return file_icon_for_path(path, self._icon_fg)

    def _folder_icon(self, item: QTreeWidgetItem) -> QIcon:
        """Иконка папки по её состоянию (открыта/закрыта)."""
        if item.isExpanded():
            return self._folder_open_icon
        return self._folder_close_icon

    # ---- тема ----

    def set_menu_qss(self, qss: str):
        """QSS контекстного меню — попапы не наследуют стиль главного окна."""
        self._menu_qss = qss

    def apply_theme(self, theme_name: str):
        self._theme_name = theme_name
        tc = ThemeColors.from_theme_manager(self.theme_manager, theme_name)
        bg = tc.bg_color
        fg = tc.fg
        border = tc.border
        muted = tc.muted
        selection = tc.selection
        panel = tc.shade(SIDEBAR_DARKEN)

        if selection:
            sel_bg = QColor(selection)
        else:
            sel_bg = QColor(fg)
            sel_bg.setAlpha(45)

        # Цвет линий дерева — как у линий фолдинга в редакторе
        guide = QColor(muted)
        guide.setAlpha(150)

        # Hover у элементов дерева — из цвета текста темы (белый оверлей
        # на светлом фоне был бы невидим)
        hover = QColor(fg)
        hover.setAlpha(15)

        # Иконки из files_icons в цвете темы
        new_fg = QColor(fg)
        if self._icon_fg is None or self._icon_fg != new_fg:
            self._icon_fg = new_fg
            self._folder_close_icon = self._icon("folder_close", self._icon_fg)
            self._folder_open_icon = self._icon("folder_open", self._icon_fg)
            self._refresh_icons()

        self.tree.set_theme_colors(guide)

        # Высота таба в таб-баре = вертикальный padding таба (TAB_PADDING_PANE)
        # + границы (1px сверху и снизу). Панель пути делаем такой же высоты.
        _tab_pad = TAB_PADDING_PANE.split()
        _label_v_pad = int(_tab_pad[0][:-2]) + int(_tab_pad[2][:-2]) + 2
        _label_top = _label_v_pad // 2
        _label_bottom = _label_v_pad - _label_top

        self.setStyleSheet(f"""
            #fileTreePanel {{
                background-color: {panel.name()};
                border-top: 0.5px solid {border};
                border-bottom: 0.5px solid {border};
            }}
            QLabel#fileTreePath {{
                color: {muted};
                font-family: "{ui_font_family()}";
                font-size: {UI_FONT_SIZE}pt;
                padding: {_label_top}px 8px {_label_bottom}px 8px;
            }}
            QTreeWidget {{
                background: transparent;
                border: none;
                color: {fg};
                outline: 0;
                font-family: "{ui_font_family()}";
                font-size: {UI_FONT_SIZE}pt;
            }}
            QTreeWidget::item {{
                padding: 2px 4px;
                border: none;
            }}
            QTreeWidget::item:hover {{
                background-color: {qss_color(hover)};
            }}
            QTreeWidget::item:selected {{
                background-color: {qss_color(sel_bg)};
                color: {fg};
            }}
        """ + scrollbar_qss(panel.name(), QColor(fg)))

        # Стиль полос выставляем прямо на них: QSS выше пришёл по
        # наследованию от панели, а при смене темы нужен и цвет бегунка
        # для затухания.
        self._sb_ctl.set_colors(panel.name(), QColor(fg))

    def _refresh_icons(self):
        """Обновляет иконки у уже построенных элементов дерева."""
        def walk(item: QTreeWidgetItem):
            for i in range(item.childCount()):
                child = item.child(i)
                path = child.data(0, Qt.ItemDataRole.UserRole)
                if path:
                    if os.path.isdir(path):
                        child.setIcon(0, self._folder_icon(child))
                    else:
                        child.setIcon(0, self._file_icon(path))
                walk(child)

        for i in range(self.tree.topLevelItemCount()):
            item = self.tree.topLevelItem(i)
            if item.data(0, Qt.ItemDataRole.UserRole):
                item.setIcon(0, self._folder_icon(item))
            walk(item)
