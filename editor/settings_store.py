"""
SettingsStore — настройки редактора в JSON-файле editor/settings.json.

Хранилище плоское: ключи вида "editor.code_font_size", значения — базовые
типы (bool/int/str). Дефолты живут в DEFAULTS и в схеме (settings_schema.py);
store подставляет дефолт, если ключа в файле нет. Файл перезагружается
однократно (кэш), запись — по вызову save() (не на каждое изменение поля).

Тот же паттерн, что у TaskStore (editor/tasks.json): JSON рядом с редактором,
чтобы файл был виден пользователю и переживал переустановку.
"""

import json
import threading
from pathlib import Path
from typing import Any, Optional

from editor.resources import editor_path

from editor.constants import (
    CODE_FONT_SIZE,
    DEFAULT_TAB_WIDTH,
    DEFAULT_THEME,
    UI_FONT_SIZE,
)
from editor.shortcuts import SHORTCUT_DEFAULTS

SETTINGS_FILE = editor_path("settings.json")

# Ключ -> значение по умолчанию. "" у themes.current означает «тему из настроек
# не применять»: сессия/меню остаются источником истины, пока пользователь
# явно не выбрал тему в панели настроек.
DEFAULTS: dict = {
    "general.ui_font_family": "",
    "general.ui_font_size": UI_FONT_SIZE,
    "editor.code_font_family": "",
    "editor.code_font_size": CODE_FONT_SIZE,
    "editor.tab_width": DEFAULT_TAB_WIDTH,
    "editor.eol_mode": "lf",
    "editor.show_line_numbers": True,
    "editor.show_whitespace": True,
    "editor.highlight_caret_line": False,
    "editor.auto_indent": True,
    "editor.word_wrap": False,
    "editor.show_fold_margin": True,
    "editor.fold_on_open": "off",
    "editor.underline_preprocessor": True,
    "editor.auto_import.python": True,
    "editor.auto_import.cpp": True,
    "editor.trim_trailing_whitespace": False,
    "editor.scrollbar_auto_hide": True,
    "editor.scrollbar_hide_delay": 800,
    "editor.scrollbar_fade": True,
    "autosave.enabled": False,
    "autosave.interval": 30,
    "autosave.on_focus_lost": False,
    "terminal.font_size": 11,
    "view.spotlight": False,
    "view.spotlight_dark_alpha": 230,
    "view.spotlight_light_alpha": 190,
    "view.spotlight_skip_terminal": False,
    "view.spotlight_skip_diagnostics": False,
    "lsp.enabled": True,
    "lsp.clangd_path": "",
    "lsp.clangd_args": "",
    "lsp.clangd.standard": "",
    "lsp.clangd.error_lens": True,
    "lsp.clangd.hover": True,
    "lsp.clangd.inlay_hints": True,
    "lsp.basedpyright_path": "",
    "lsp.basedpyright_args": "",
    "lsp.python.type_checking": "default",
    "lsp.python.error_lens": True,
    "lsp.python.hover": True,
    "lsp.python.inlay_hints": True,
    "lsp.python_path": "",
    "lsp.cl_lsp_path": "",
    "lsp.cl_lsp_args": "",
    "lsp.sbcl_path": "",
    "lsp.lisp.error_lens": True,
    "lsp.lisp.hover": True,
    "lsp.rust_analyzer_path": "",
    "lsp.rust_analyzer_args": "",
    "lsp.rust.check_on_save": True,
    "lsp.rust.error_lens": True,
    "lsp.rust.hover": True,
    "lsp.rust.inlay_hints": True,
    "lsp.lumen_path": "",
    "lsp.lumen_args": "",
    "lsp.lumen.check_on_save": True,
    "lsp.lumen.deep_analysis": True,
    "lsp.lumen.error_lens": True,
    "lsp.lumen.hover": True,
    "lsp.lumen.inlay_hints": True,
    "themes.current": "",
}

# Шорткаты (shortcuts.<id>) — из реестра editor/shortcuts.py
DEFAULTS.update({f"shortcuts.{k}": v for k, v in SHORTCUT_DEFAULTS.items()})


class SettingsStore:
    """Типизированное хранилище настроек в JSON-файле."""

    def __init__(self, path: str):
        self._path = Path(path)
        self._data: dict = {}
        self._loaded = False
        self._lock = threading.Lock()

    def _ensure_loaded(self):
        if self._loaded:
            return
        try:
            raw = self._path.read_text(encoding="utf-8")
            data = json.loads(raw)
            self._data = data if isinstance(data, dict) else {}
        except (OSError, ValueError):
            self._data = {}
        self._loaded = True

    def get(self, key: str, default: Any = None) -> Any:
        """Значение ключа (из файла) или дефолт из DEFAULTS/аргумента."""
        with self._lock:
            self._ensure_loaded()
            if key in self._data:
                return self._data[key]
        return DEFAULTS.get(key, default)

    def get_bool(self, key: str, default: bool = False) -> bool:
        return bool(self.get(key, default))

    def get_int(self, key: str, default: int = 0) -> int:
        try:
            return int(self.get(key, default))
        except (TypeError, ValueError):
            return default

    def get_str(self, key: str, default: str = "") -> str:
        value = self.get(key, default)
        return value if isinstance(value, str) else str(value)

    def set(self, key: str, value: Any):
        with self._lock:
            self._ensure_loaded()
            self._data[key] = value

    def set_many(self, items: dict):
        with self._lock:
            self._ensure_loaded()
            self._data.update(items)

    def save(self):
        """Записывает файл (сбой записи не должен ронять приложение)."""
        with self._lock:
            self._ensure_loaded()
            try:
                self._path.parent.mkdir(parents=True, exist_ok=True)
                self._path.write_text(
                    json.dumps(self._data, ensure_ascii=False, indent=2),
                    encoding="utf-8",
                )
            except OSError:
                pass

    def reset(self):
        """Сбрасывает настройки к дефолтам (не пишет файл до save())."""
        with self._lock:
            self._data = dict(DEFAULTS)

    def clear_file(self):
        """Удаляет файл настроек на диске (полный сброс)."""
        try:
            self._path.unlink()
        except OSError:
            pass
        with self._lock:
            self._data = dict(DEFAULTS)
            self._loaded = True

    def as_dict(self) -> dict:
        with self._lock:
            self._ensure_loaded()
            return dict(self._data)


# ---------------------------------------------------------------------------
# Глобальный доступ: один экземпляр на приложение
# ---------------------------------------------------------------------------
_app_settings: Optional[SettingsStore] = None


def app_settings() -> SettingsStore:
    global _app_settings
    if _app_settings is None:
        _app_settings = SettingsStore(SETTINGS_FILE)
    return _app_settings


# Реэкспорт дефолтной темы для схемы (чтобы не импортировать constants в UI)
def default_theme() -> str:
    return DEFAULT_THEME
