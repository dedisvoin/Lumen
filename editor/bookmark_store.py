"""
Хранилище закладок по отдельным файлам.

Закладки в сессии (tile_manager) живут только пока вкладка открыта и
сохраняются вместе со всем деревом панелей при закрытии окна. Чтобы закладка
переживала закрытие самой вкладки (и открытие файла заново), дублируем её
сюда — в JSON, с ключом по абсолютному пути файла. Пишется при закрытии
вкладки и читается при открытии файла.
"""

import json
import os
from pathlib import Path

from editor.resources import editor_path

BOOKMARKS_FILE = editor_path("sessions") / "bookmarks.json"

_cache = None


def _load_all() -> dict:
    global _cache
    if _cache is not None:
        return _cache
    try:
        raw = BOOKMARKS_FILE.read_text(encoding="utf-8")
        data = json.loads(raw)
    except (OSError, ValueError, TypeError):
        data = {}
    _cache = data if isinstance(data, dict) else {}
    return _cache


def _save_all() -> None:
    try:
        BOOKMARKS_FILE.parent.mkdir(parents=True, exist_ok=True)
        BOOKMARKS_FILE.write_text(
            json.dumps(_cache, ensure_ascii=False), encoding="utf-8"
        )
    except OSError:
        pass  # сбой записи не должен блокировать закрытие вкладки


def load_bookmarks(path: str) -> list:
    """Закладки файла (в формате get_bookmarks) или [] если их нет."""
    if not path:
        return []
    key = os.path.normcase(os.path.abspath(path))
    return _load_all().get(key, [])


def save_bookmarks(path: str, items: list) -> None:
    """Сохраняет/удаляет закладки файла. Пустой список стирает ключ."""
    if not path:
        return
    key = os.path.normcase(os.path.abspath(path))
    data = _load_all()
    if items:
        data[key] = items
    else:
        data.pop(key, None)
    _save_all()


def all_bookmarks() -> dict:
    """Все сохранённые закладки: {нормализованный_путь: [{"line", "note"}]}.

    Используется поиском по всем закладкам проекта (bookmark_finder.py) для
    учёта закладок в закрытых файлах — тех, что уже нет в открытых редакторах.
    """
    return _load_all()


def retarget_bookmarks(old_path: str, new_path: str) -> None:
    """Переносит закладки при переименовании/перемещении файла."""
    if not old_path or not new_path or old_path == new_path:
        return
    old = os.path.normcase(os.path.abspath(old_path))
    new = os.path.normcase(os.path.abspath(new_path))
    data = _load_all()
    if old in data:
        data[new] = data.pop(old)
        _save_all()
