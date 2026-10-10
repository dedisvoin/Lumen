"""
FileTreeOps — операции файловой системы для дерева файлов.

Создание, переименование и удаление файлов/папок, обработка drop путей,
поиск узлов дерева по пути и перестроение корневого узла. Не содержит
логики отрисовки и тем: работает с виджетом FileTreeWidget через self.w
(меняет дерево и его состояние, эмитит его сигналы).
"""

import os
import shutil
from typing import Callable, List, Optional

from PyQt6.QtCore import Qt
from PyQt6.QtWidgets import QTreeWidgetItem

from editor import themed_dialogs


# Каталоги, которые не показываем в дереве
SKIP_DIRS = {"__pycache__", ".git", ".idea", ".vs", ".vscode", "node_modules"}


def _touch(path: str):
    """Создаёт пустой файл (аналог touch)."""
    with open(path, "w", encoding="utf-8"):
        pass


class FileTreeOps:
    """FS-операции FileTreeWidget: создание, переименование, удаление, drop."""

    def __init__(self, widget):
        self.w = widget
        # Инлайн-переименование: (item, старый путь) пока редактор открыт
        self._rename_pending: Optional[tuple] = None

    # ---- создание ----

    def create_entry(self, title: str, label: str, folder: str,
                     create: Callable[[str], None]) -> bool:
        """Общий флоу «создать файл/папку»: имя → проверка → создание.

        Возвращает True, если создано; сообщения об ошибках показывает сам.
        """
        tm, theme = self.w.theme_manager, self.w._theme_name
        name, ok = themed_dialogs.input_text(self.w, title, label, tm, theme)
        if not ok:
            return False
        name = name.strip()
        if not name:
            return False
        path = os.path.join(folder, name)
        if os.path.exists(path):
            themed_dialogs.warning(self.w, title,
                                   f"'{name}' already exists.", tm, theme)
            return False
        try:
            create(path)
        except OSError as e:
            themed_dialogs.warning(self.w, title, str(e), tm, theme)
            return False
        return True

    def create_file(self, folder: str) -> bool:
        """Диалог имени → создать пустой файл в folder. True, если создан."""
        return self.create_entry("New File", "File name:", folder, _touch)

    def create_dir(self, folder: str) -> bool:
        """Диалог имени → создать папку в folder. True, если создана."""
        return self.create_entry("New Folder", "Folder name:",
                                 folder, os.mkdir)

    # ---- переименование ----

    def rename_item(self, item: QTreeWidgetItem, path: str):
        """Начать инлайн-переименование узла прямо в дереве."""
        if not path:
            return
        self._rename_pending = (item, path)
        # QTreeWidgetItem не редактируемый по умолчанию — без этого флага
        # editItem проваливается с предупреждением «edit: editing failed»
        item.setFlags(item.flags() | Qt.ItemFlag.ItemIsEditable)
        self.w.tree.editItem(item, 0)

    def on_item_changed(self, item: QTreeWidgetItem, column: int):
        """Коммит инлайн-редактора: применяем переименование."""
        if column != 0 or not self._rename_pending:
            return
        pending_item, old_path = self._rename_pending
        if pending_item is not item:
            return
        old_name = os.path.basename(old_path)
        new_name = item.text(0).strip()
        # Событие старта редактирования шлёт то же имя — флаг ещё нужен
        if not new_name or new_name == old_name:
            if new_name != old_name:
                # Пустое имя — откатываем текст узла
                item.setText(0, old_name)
            return
        # Реально переименовываем: флаг снимаем ДО обновления дерева,
        # чтобы setText/setData/_populate не попали в обработчик повторно
        self._rename_pending = None
        self.apply_rename(item, old_path, new_name)

    def on_edit_finished(self):
        """Редактор закрыт (Esc / клик в сторону) — выходим из режима."""
        self._rename_pending = None

    def apply_rename(self, item: QTreeWidgetItem, old_path: str,
                     new_name: str):
        """Переименовать файл/папку на диске и обновить узел дерева."""
        tm, theme = self.w.theme_manager, self.w._theme_name
        new_path = os.path.join(os.path.dirname(old_path), new_name)
        if os.path.exists(new_path):
            themed_dialogs.warning(
                self.w, "Rename", f"'{new_name}' already exists.", tm, theme)
            item.setText(0, os.path.basename(old_path))
            return
        try:
            os.rename(old_path, new_path)
        except OSError as e:
            themed_dialogs.warning(self.w, "Rename", str(e), tm, theme)
            item.setText(0, os.path.basename(old_path))
            return
        # Вкладки со старым путём перенацеливает главное окно
        self.w.file_renamed.emit(old_path, new_path)
        item.setText(0, new_name)
        item.setData(0, Qt.ItemDataRole.UserRole, new_path)
        if os.path.isdir(new_path):
            # Раскрытая папка переименована — обновляем запомненный путь
            if old_path in self.w._expanded_paths:
                self.w._expanded_paths.discard(old_path)
                self.w._expanded_paths.add(new_path)
            if item.parent() is None:
                # Переименован корень — перестраиваем всё дерево
                self.w._root_path = new_path
                self.w._set_path_label(new_path)
                self.w.tree.clear()
                self.build_root_item(new_path)
            else:
                item.setIcon(0, self.w._folder_open_icon)
                # Пути детей устарели — перечитываем поддерево
                self.w._populate(item, new_path)
        else:
            item.setIcon(0, self.w._file_icon(new_path))

    # ---- удаление ----

    def delete_item(self, item: QTreeWidgetItem, path: str):
        if not path:
            return
        tm, theme = self.w.theme_manager, self.w._theme_name
        name = os.path.basename(path) or path
        is_dir = os.path.isdir(path)
        if is_dir:
            message = f"Delete folder '{name}' and all its contents?"
        else:
            message = f"Delete '{name}'?"
        answer = themed_dialogs.question(
            self.w, "Delete", message,
            themed_dialogs.StandardButton.Yes
            | themed_dialogs.StandardButton.No,
            themed_dialogs.StandardButton.No,
            tm, theme,
        )
        if answer != themed_dialogs.StandardButton.Yes:
            return
        try:
            if is_dir:
                shutil.rmtree(path)
            else:
                os.remove(path)
        except OSError as e:
            themed_dialogs.warning(self.w, "Delete", str(e), tm, theme)
            return
        parent = item.parent()
        if parent is not None:
            parent.removeChild(item)
        else:
            self.w.tree.takeTopLevelItem(
                self.w.tree.indexOfTopLevelItem(item)
            )
            self.w._root_path = None
            self.w._path_label.setText("No folder")
            self.w._path_label.setToolTip("")

    # ---- drop путей из проводника ----

    def drop_paths(self, paths: List[str]):
        for p in paths:
            if os.path.isdir(p):
                self.w.set_root_path(p)
                return
        for p in paths:
            if os.path.isfile(p):
                self.w.open_file_requested.emit(p)

    # ---- поиск/перестроение узлов дерева ----

    def find_item_by_path(self, path: str) -> Optional[QTreeWidgetItem]:
        """Ищет узел дерева по его пути на диске (рекурсивно)."""
        def search(item: QTreeWidgetItem) -> Optional[QTreeWidgetItem]:
            if item.data(0, Qt.ItemDataRole.UserRole) == path:
                return item
            for i in range(item.childCount()):
                found = search(item.child(i))
                if found is not None:
                    return found
            return None

        for i in range(self.w.tree.topLevelItemCount()):
            found = search(self.w.tree.topLevelItem(i))
            if found is not None:
                return found
        return None

    def build_root_item(self, path: str) -> QTreeWidgetItem:
        """Создаёт корневой узел дерева для path и наполняет его."""
        root_item = QTreeWidgetItem([os.path.basename(path) or path])
        root_item.setData(0, Qt.ItemDataRole.UserRole, path)
        root_item.setIcon(0, self.w._folder_open_icon)
        self.w.tree.addTopLevelItem(root_item)
        self.w._populate(root_item, path)
        root_item.setExpanded(True)
        self.w._restore_expanded(root_item)
        return root_item
