"""
SharedFsWatcher — один системный QFileSystemWatcher на весь редактор.

Каждый клиент (редактор кода, дерево файлов) держит свой экземпляр-обёртку
с привычным API (addPath/removePaths/files/directories + сигналы), но реальный
QFileSystemWatcher (и его OS-дескрипторы) — один на процесс. События
перенаправляются только тем обёрткам, что подписаны на конкретный путь.

Windows теряет watch при перезаписи/удалении файла — ядро при событии
пере-добавляет путь, пока на него есть подписчики (случай git/build overwrite).
"""

import os
import functools
from typing import Dict, List, Set

from PyQt6.QtCore import QObject, QCoreApplication, QFileSystemWatcher, QTimer, pyqtSignal


class _WatcherCore(QObject):
    """Синглтон: реальный QFileSystemWatcher + реестр подписчиков по пути."""

    _instance: "_WatcherCore" = None

    def __init__(self):
        # Родитель — QApplication: ядро живёт дольше любых обёрток,
        # и его watcher не удалится раньше клиентов при закрытии приложения
        super().__init__(QCoreApplication.instance())
        self._watcher = QFileSystemWatcher(self)
        self._files: Dict[str, Set[QObject]] = {}
        self._dirs: Dict[str, Set[QObject]] = {}
        self._watcher.fileChanged.connect(self._on_file_changed)
        self._watcher.directoryChanged.connect(self._on_dir_changed)

    @classmethod
    def instance(cls) -> "_WatcherCore":
        if cls._instance is None:
            cls._instance = cls()
        return cls._instance

    # --- подписка/отписка ---

    def subscribe(self, path: str, client: QObject, is_dir: bool):
        table = self._dirs if is_dir else self._files
        subs = table.setdefault(path, set())
        if client in subs:
            return  # уже подписан — не добавляем повторно (Qt бросает на дубль)
        subs.add(client)
        watched = self._watcher.directories() if is_dir else self._watcher.files()
        if path not in watched:
            self._watcher.addPath(path)

    def unsubscribe(self, path: str, client: QObject, is_dir: bool):
        table = self._dirs if is_dir else self._files
        subs = table.get(path)
        if not subs:
            return
        subs.discard(client)
        if subs:
            return
        del table[path]
        watched = self._watcher.directories() if is_dir else self._watcher.files()
        if path in watched:
            self._watcher.removePaths([path])

    def unsubscribe_all(self, client: QObject, files: Set[str], dirs: Set[str]):
        """Отписка клиента (вызывается при его уничтожении)."""
        try:
            self._watcher.files()  # guard: ядро пережило обёртки?
        except RuntimeError:
            return  # завершение приложения — реальный watcher уже удалён
        for path in list(files):
            self.unsubscribe(path, client, False)
        for path in list(dirs):
            self.unsubscribe(path, client, True)

    # --- события ---

    def _on_file_changed(self, path: str):
        subs = self._files.get(path)
        if not subs:
            return
        for client in list(subs):
            client.fileChanged.emit(path)
        # Перезапись/удаление на Windows снимает watch не сразу (Qt убирает
        # путь из files() на следующей итерации) — проверяем после задержки
        QTimer.singleShot(50, functools.partial(self._ensure_watch, path, False))

    def _on_dir_changed(self, path: str):
        subs = self._dirs.get(path)
        if not subs:
            return
        for client in list(subs):
            client.directoryChanged.emit(path)
        QTimer.singleShot(50, functools.partial(self._ensure_watch, path, True))

    def _ensure_watch(self, path: str, is_dir: bool):
        """Восстанавливает watch, если ОС его сняла, а подписчики остались."""
        if not (self._dirs if is_dir else self._files).get(path):
            return  # подписчиков больше нет
        watched = self._watcher.directories() if is_dir else self._watcher.files()
        if path not in watched:
            self._watcher.addPath(path)


class SharedFsWatcher(QObject):
    """Обёртка с API QFileSystemWatcher; реальный watcher — общий синглтон."""

    fileChanged = pyqtSignal(str)
    directoryChanged = pyqtSignal(str)

    def __init__(self, parent=None):
        super().__init__(parent)
        self._files: Set[str] = set()
        self._dirs: Set[str] = set()
        core = _WatcherCore.instance()
        # Чистим реестр ядра, когда обёртка уничтожена вместе с родителем
        self.destroyed.connect(
            functools.partial(core.unsubscribe_all, self, self._files, self._dirs)
        )

    def addPath(self, path: str):
        if not path:
            return
        is_dir = os.path.isdir(path)
        target = self._dirs if is_dir else self._files
        if path in target:
            return
        target.add(path)
        _WatcherCore.instance().subscribe(path, self, is_dir)

    def removePaths(self, paths: List[str]):
        core = _WatcherCore.instance()
        for path in paths:
            if path in self._files:
                self._files.discard(path)
                core.unsubscribe(path, self, False)
            elif path in self._dirs:
                self._dirs.discard(path)
                core.unsubscribe(path, self, True)

    def files(self) -> List[str]:
        return list(self._files)

    def directories(self) -> List[str]:
        return list(self._dirs)
