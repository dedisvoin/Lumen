"""
Сохранение/восстановление сессии: окно, тема, дерево панелей, вкладки.

Состояние пишется JSON-файлом в папку sessions рядом с редактором
(<editor>/sessions/session.json) при закрытии окна и восстанавливается
при старте (MainWindow._restore_session).
"""

import json
from pathlib import Path
from typing import Optional

from editor.resources import editor_path

SESSION_DIR = editor_path("sessions")
SESSION_FILE = SESSION_DIR / "session.json"


def save_session(window) -> None:
    """Снимок состояния окна в файл сессии (вызывается при закрытии)."""
    data = {
        "geometry": _geometry(window),
        "theme": window.current_theme,
        "root_sizes": [int(s) for s in window._root_splitter.sizes()],
        "tree_visible": window.file_tree.isVisible(),
        "folder": window.file_tree.root_path,
        "active": _active_tab(window),
        "tree": window.tile_manager.serialize(),
    }
    try:
        SESSION_DIR.mkdir(parents=True, exist_ok=True)
        SESSION_FILE.write_text(
            json.dumps(data, ensure_ascii=False), encoding="utf-8"
        )
    except OSError:
        pass  # сбой записи не должен блокировать закрытие окна


def load_session() -> Optional[dict]:
    """Сессия из файла или None, если её нет/она повреждена."""
    try:
        raw = SESSION_FILE.read_text(encoding="utf-8")
    except OSError:
        return None
    try:
        data = json.loads(raw)
    except (ValueError, TypeError):
        return None
    return data if isinstance(data, dict) else None


def _geometry(window) -> dict:
    return {
        "x": window.x(), "y": window.y(),
        "w": window.width(), "h": window.height(),
        "maximized": window.isMaximized(),
    }


def _active_tab(window) -> Optional[dict]:
    """Активная вкладка: номер панели (в порядке дерева) и номер вкладки."""
    tile = window.tile_manager
    editor = tile.active_editor
    pane = None
    if editor is not None:
        for p in tile._panes:
            if p._host_of(editor) is not None:
                pane = p
                break
    if pane is None:
        pane = tile.get_active_pane()
    if pane is None:
        return None
    return {"pane": tile._panes.index(pane), "tab": pane.tabs.currentIndex()}
